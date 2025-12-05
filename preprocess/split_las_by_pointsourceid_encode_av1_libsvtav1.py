import argparse
import json
import os
import subprocess

import numpy as np
import pdal


def run_pipeline(input_path):
    """Run PDAL pipeline to read LAS and return a numpy structured array."""
    pipeline_json = {
        "pipeline": [
            {"type": "readers.las", "filename": input_path}
        ]
    }
    pipe = pdal.Pipeline(json.dumps(pipeline_json))
    pipe.execute()
    return pipe.arrays[0]


def encode_color_and_depth_streams(
        arr,
        width,
        height,
        fps,
        output_dir,
        manifest_path):
    """
    Groups points by PointSourceId to create frames and encodes them into a
    single MKV file with three video streams:
    - Stream 0: 8-bit color (libsvtav1, yuv420p)
    - Stream 1: 8-bit alpha (ffv1, gray)
    - Stream 2: 16-bit depth (ffv1, gray16le)
    """
    print("--- Starting Triple Stream Encoding (Color + Alpha + Depth) ---")

    # Create directories
    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

    # Prepare some variables
    point_source_ids = np.unique(arr['PointSourceId'])
    expected_n = width * height
    file_name = "ColorAlphaDepth.mkv"
    file_path = os.path.join(output_dir, file_name)

    # Define FFmpeg Command
    ffmpeg_command = [
        "ffmpeg",
        "-y", # Overwrite output file if it exists
        # --- Input Flags (MUST come before -i) ---
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgba", # Input is 4-channel 8-bit RGBA
        "-s",
        f"{width}x{2*height}", # Combined frame (color on top, depth on bottom)
        "-framerate",
        str(fps),
        "-i",
        "pipe:0",  # Read raw data from standard input

        # --- Video Filter Complex (Splits the input frame) ---
        "-filter_complex",
        (
            f"[0:v]crop={width}:{height}:0:0[rgba_in];" # Crop top half (RGBA)
            "[rgba_in]split[rgba_for_color][rgba_for_alpha];" # Split the stream for separate processing
            "[rgba_for_alpha]alphaextract[alpha_stream];"     # Extract alpha channel
            "[rgba_for_color]format=yuv420p[color_stream];"   # Format RGB part for libsvtav1
            f"[0:v]crop={width}:{height}:0:{height},format=gray16le[depth_stream]" # Crop bottom half (Depth)
        ),

        # --- Stream 0: Color (libsvtav1) ---
        "-map", "[color_stream]",
        "-c:v:0", "libsvtav1",
        "-preset", "8", # Faster preset
        "-crf", "23", # Good quality/size balance

        # --- Stream 1: Alpha (ffv1) ---
        "-map", "[alpha_stream]",
        "-c:v:1", "ffv1", # Lossless codec for alpha

        # --- Stream 2: Depth (ffv1) ---
        "-map", "[depth_stream]",
        "-c:v:2", "ffv1", # Lossless codec for depth

        # --- Output File ---
        "-c:a",
        "copy",
        file_path,
    ]

    # The input to ffmpeg is a single frame of size (width, 2*height).
    # The top half (height) contains the RGBA color data.
    # The bottom half (height) contains the 16-bit depth data, which we
    # pack into an RGBA-like structure (2 bytes per pixel) to match the pix_fmt.
    #
    # [ R G B A ] -> Color pixel
    # [ D D D D ] -> Depth pixel (16 bits) stored across 4 bytes.
    # We will use view(np.uint8) to send bytes.
    # A depth pixel (uint16) will be sent as two uint8 bytes.
    # To make it fit the rgba pix_fmt, we need 4 bytes per pixel.
    # So we create a (height, width, 2) uint16 array and view it as (height, width, 4) uint8.

    # Prepare manifest
    manifest = {
        "file": file_name,
        "width": int(width),
        "height": int(height),
        "fps": int(fps),
    }

    # Start and Pipe Data
    try:
        process = subprocess.Popen(
            ffmpeg_command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,
        )

        for i, pid in enumerate(point_source_ids):
            subset = arr[arr['PointSourceId'] == pid]
            n = subset.size

            if n != expected_n:
                raise ValueError(
                    f"Frame {pid}: expected {expected_n} points but got {n}."
                )

            # --- Prepare Depth Data (16-bit) ---
            depth_m = np.abs(subset['Z'].astype(np.float32))
            depth_16bit = np.clip(depth_m * 1000.0, 0, 65535).astype(np.uint16)

            # --- Prepare Color Data (8-bit) ---
            R = subset['Red'].astype(np.float32)
            G = subset['Green'].astype(np.float32)
            B = subset['Blue'].astype(np.float32)

            # Normalize if LAS stores 0–65535 or other scaling
            max_rgb = max(R.max(), G.max(), B.max())
            if max_rgb > 1.0:
                R = np.clip(np.clip(R / max_rgb, 0.0, 1.0) * 255, 0, 255).astype(np.uint8)
                G = np.clip(np.clip(G / max_rgb, 0.0, 1.0) * 255, 0, 255).astype(np.uint8)
                B = np.clip(np.clip(B / max_rgb, 0.0, 1.0) * 255, 0, 255).astype(np.uint8)

            # Compute alpha based on classification
            classification = subset['Classification']
            A = np.clip(np.where(classification == 7, 0.0, 1.0) * 255, 0, 255).astype(np.uint8)

            # --- Combine into a single frame for piping ---

            # Color (H, W, 4)
            color_array = np.stack([R, G, B, A], axis=-1).reshape((height, width, 4))

            # Depth (H, W, 1) -> (H, W, 2) -> (H, W, 4) as uint8
            # We create a (H, W, 2) array of uint16 and view it as (H, W, 4) uint8
            depth_array_16bit_2channel = np.zeros((height, width, 2), dtype=np.uint16)
            depth_array_16bit_2channel[:, :, 0] = depth_16bit.reshape((height, width))
            depth_array_8bit_4channel = depth_array_16bit_2channel.view(np.uint8)

            # Combined array (2*H, W, 4)
            combined_array = np.concatenate((color_array, depth_array_8bit_4channel), axis=0)

            raw_frame_bytes = combined_array.tobytes()

            # Write the raw frame data to FFmpeg's stdin pipe
            process.stdin.write(raw_frame_bytes)

            if (i + 1) % (fps * 2) == 0:
                print(f"  Piped {i + 1} frames...")

        # Finalize and Close
        process.stdin.close()
        print("Piping complete. Waiting for FFmpeg to finish encoding...")

        stdout, stderr = process.communicate()

        if process.returncode == 0:
            print(f"Encoding successful! Video saved as {file_path}")
        else:
            print("Encoding failed.")
            print("FFmpeg Error Output:")
            print(stderr.decode("utf-8") if stderr else "No stderr output.")

    except FileNotFoundError:
        print(
            "Error: FFmpeg executable not found. Please ensure it is installed and available in your system PATH."
        )
    except Exception as e:
        print(f"An unexpected error occurred: {e}")

    # Write manifest JSON
    with open(manifest_path, "w", encoding="utf-8") as mf:
        json.dump(manifest, mf, indent=2)

    print(f"Wrote {len(point_source_ids)} frames into {output_dir}")
    print(f"Manifest written to {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description="Split LAS by PointSourceId into binary frames")
    parser.add_argument("--input", required=True, help="Path to input LAS file")
    parser.add_argument("--width", required=True, type=int, help="Grid width")
    parser.add_argument("--height", required=True, type=int, help="Grid height")
    parser.add_argument("--fps", required=True, type=int, help="Frames per second")
    parser.add_argument("--output", default="../server/frames", help="Output directory for binary frames")
    parser.add_argument("--manifest", default="../server/manifest/frames.json", help="Manifest output path")

    args = parser.parse_args()

    arr = run_pipeline(args.input)
    encode_color_and_depth_streams(
        arr, args.width, args.height, args.fps, args.output, args.manifest)


if __name__ == "__main__":
    main()
