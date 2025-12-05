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


def encode_multiple_frames_with_alpha_to_av1(
        arr,
        width,
        height,
        fps,
        output_dir,
        manifest_path):
    """
    Group by PointSourceId to get a sequence of RGBAZ frames 
    and encodes the frames as an 8-bit AV1 video file using the yuva444p format
    """
    print("--- Starting AV1 Encoding ---")

    # Create directories
    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

    # Prepare some variables
    point_source_ids = np.unique(arr['PointSourceId'])
    expected_n = width * height
    file_name = "RGBAZ.webm"
    file_path = os.path.join(output_dir, file_name)
    MAX_8BIT = 255

    # Define FFmpeg Command
    ffmpeg_command = [
        "ffmpeg",
        # --- Input Flags (MUST come before -i) ---
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgba",  # Input is 4-channel 8-bit RGBA
        "-s",
        f"{2*width}x{height}",
        "-framerate",
        str(fps),
        "-i",
        "pipe:0",  # Read raw data from standard input

        # --- Video Filter Complex (Splits the RGBA stream) ---
        "-filter_complex",
        (
            "[0:v]split[color][alpha];"  # Split the input stream into two named streams
            "[color]format=yuv420p[c];"  # Stream 1: Keep color, convert to standard YUV
            "[alpha]alphaextract,format=gray[a]" # Stream 2: Extract Alpha, convert to Grayscale
        ),

        # --- Stream 0 (Color Track) ---
        "-map",
        "[c]",
        "-c:v:0",
        "libvpx-vp9",
        "-pix_fmt",
        "yuv420p",
        "-b:v:0",
        "0",
        "-crf",
        "25",

        # --- Stream 1 (Alpha Track) ---
        "-map",
        "[a]",
        "-c:v:1",
        "libvpx-vp9", # Use the same codec for the alpha track
        "-pix_fmt",
        "gray",     # Grayscale format for the alpha channel
        "-b:v:1",
        "0",
        "-crf",
        "25",
        
        # --- Output File ---
        "-c:a",
        "copy",
        file_path,
    ]

    # Prepare manifest
    manifest = {
        "file": file_name,
        "width": int(2*width),
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

            # Depth
            # Split 24 bit variable into 3x8-bit channels (R, G, B) 
            Z = np.clip(np.abs(subset['Z'].astype(np.float32) * 1000), 0, 16_777_215).astype(np.uint32)
            
            # 8 MSBs -> Red Channel
            Z_R = ((Z >> 16) & MAX_8BIT).astype(np.uint8)
            
            # Middle 8 bits -> Green Channel
            Z_G = ((Z >> 8) & MAX_8BIT).astype(np.uint8)
            
            # 8 LSBs -> Blue Channel
            Z_B = (Z & MAX_8BIT).astype(np.uint8)
            
            # Colors
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
            A = np.clip(np.where(classification == 7, 0.0, 1.0).astype(np.float32) * 255, 0, 255).astype(np.uint8)

            # Numpy array (H, W, 4)
            ZZZA_stacked_array = np.stack([Z_R, Z_G, Z_B, A], axis=0)
            ZZZA_transposed_array = np.transpose(ZZZA_stacked_array)
            ZZZA_final_image_array = ZZZA_transposed_array.reshape((height, width, 4))

            # Numpy array (H, W, 4)
            RGBA_stacked_array = np.stack([R, G, B, A], axis=0)
            RGBA_transposed_array = np.transpose(RGBA_stacked_array)
            RGBA_final_image_array = RGBA_transposed_array.reshape((height, width, 4))

            # Numpy array (H, 2*W, 4)
            combined_array = np.concatenate((ZZZA_final_image_array, RGBA_final_image_array), axis=1)

            # Convert the NumPy array (H, 2*W, 4) to raw bytes (RGBA format)
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
            print(f"Encoding successful! Video saved as {filename}")
        else:
            print("Encoding failed.")
            print("FFmpeg Error Output:")
            print(stderr.decode("utf-8"))

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
    encode_multiple_frames_with_alpha_to_av1(
        arr, args.width, args.height, args.fps, args.output, args.manifest)


if __name__ == "__main__":
    main()
