import argparse
import json
import os
import subprocess
import threading

import numpy as np
import pdal


def run_pipeline(input_path):
    pipeline_json = {"pipeline": [{"type": "readers.las", "filename": input_path}]}
    pipe = pdal.Pipeline(json.dumps(pipeline_json))
    pipe.execute()
    return pipe.arrays[0]

def rgb_frame(arr, point_source_ids, width, height):
    expected_n = width * height
    for i, pid in enumerate(point_source_ids):
        subset = arr[arr["PointSourceId"] == pid]
        n = subset.size

        if n != expected_n:
            raise ValueError(
                f"Frame {pid}: expected {expected_n} points, got {n}"
            )

        # ---------------------------------------------------
        # Color
        # ---------------------------------------------------
        R = subset["Red"].astype(np.float32)
        G = subset["Green"].astype(np.float32)
        B = subset["Blue"].astype(np.float32)

        max_rgb = max(R.max(), G.max(), B.max())
        if max_rgb > 1.0:
            scale = 255.0 / max_rgb
            R = (R * scale).clip(0, 255).astype(np.uint8)
            G = (G * scale).clip(0, 255).astype(np.uint8)
            B = (B * scale).clip(0, 255).astype(np.uint8)
        else:
            R = (R * 255).astype(np.uint8)
            G = (G * 255).astype(np.uint8)
            B = (B * 255).astype(np.uint8)

        rgb = np.stack([R, G, B], axis=-1)
        rgb_bytes = rgb.reshape(height, width, 3).tobytes()

        if len(rgb_bytes) != width * height * 3:
            raise RuntimeError("Incorrect RGB byte size.")

        # ---------------------------------------------------
        # Write frame to FFmpeg
        # ---------------------------------------------------
        yield rgb_bytes

def alpha_frame(arr, point_source_ids, width, height):
    expected_n = width * height
    for i, pid in enumerate(point_source_ids):
        subset = arr[arr["PointSourceId"] == pid]
        n = subset.size

        if n != expected_n:
            raise ValueError(
                f"Frame {pid}: expected {expected_n} points, got {n}"
            )

        # ---------------------------------------------------
        # Alpha mask
        # ---------------------------------------------------
        classification = subset["Classification"]
        A = np.where(classification == 7, 0, 255).astype(np.uint8)

        a = np.stack([A], axis=-1)
        a_bytes = a.reshape(height, width, 1).tobytes()

        if len(a_bytes) != width * height * 1:
            raise RuntimeError("Incorrect A byte size.")

        # ---------------------------------------------------
        # Write both frames to FFmpeg
        # ---------------------------------------------------
        yield a_bytes

def depth_frame(arr, point_source_ids, width, height, depth_scale_factor):
    expected_n = width * height
    for i, pid in enumerate(point_source_ids):
        subset = arr[arr["PointSourceId"] == pid]
        n = subset.size

        if n != expected_n:
            raise ValueError(
                f"Frame {pid}: expected {expected_n} points, got {n}"
            )

        # ---------------------------------------------------
        # Depth frame (true 16-bit)
        # ---------------------------------------------------
        depth_m = np.abs(subset["Z"].astype(np.float32))
        depth_16 = np.clip(depth_m * depth_scale_factor, 0, 65535).astype(np.uint16)
        depth_frame = depth_16.reshape(height, width)

        depth_bytes = depth_frame.tobytes()
        if len(depth_bytes) != width * height * 2:
            raise RuntimeError("Incorrect depth byte size.")

        if i ==0:
            print("\n")
            print(i)
            print("\n")
            for b in depth_16[:16]:
                print(f"{b}", end=" ")

        # ---------------------------------------------------
        # Write both frames to FFmpeg
        # ---------------------------------------------------
        yield depth_bytes

def write_to_pipe(pipe_name, frames_bytes):
    print(f"Starting write to {pipe_name}...")
    try:
        # Open the pipe in write-binary mode
        with open(pipe_name, 'wb') as f:
            for frame_bytes in frames_bytes:
                f.write(frame_bytes)
    except BrokenPipeError:
        print(f"Pipe {pipe_name} was closed by the reader (ffmpeg) prematurely.")
    except Exception as e:
        print(f"Error writing to pipe {pipe_name}: {e}")
    print(f"Finished writing to {pipe_name}.")

def encode_color_alpha_depth_streams(
    arr,
    width,
    height,
    fps,
    output_dir,
    manifest_path
):

    print("--- Starting Triple Stream Encoding (Color + Alpha + Depth) ---")

    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

    file_name = "ColorAlphaDepth.mkv"
    file_path = os.path.join(output_dir, file_name)

    point_source_ids = np.unique(arr["PointSourceId"])

    # Find max depth to calculate scale factor for full 16-bit range
    max_depth = np.abs(arr["Z"]).max()
    depth_scale_factor = 65535.0 / max_depth if max_depth > 0 else 1.0
    print(f"Max depth found: {max_depth}. Using scale factor: {depth_scale_factor}")

    # Create the named pipes (FIFOs)
    pipe1_path = './pipe1'
    pipe2_path = './pipe2'
    pipe3_path = './pipe3'
    try:
        os.mkfifo(pipe1_path)
        os.mkfifo(pipe2_path)
        os.mkfifo(pipe3_path)
    except FileExistsError:
        print("Named pipes already exist.")

    # ---------------------------------------------------------------
    # FFmpeg command - 3 INPUT STREAMS
    #   0 Color rgb24 frames
    #   1 Alpha gray frames
    #   2 Depth gray16be frames
    # ---------------------------------------------------------------
    ffmpeg_command = [
        "ffmpeg",
        "-y",

        # ---------- RGB INPUT ----------
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "-s", f"{width}x{height}",
        "-framerate", str(fps),
        "-i", pipe1_path,

        # ---------- Alpha INPUT ----------
        "-f", "rawvideo",
        "-pix_fmt", "gray", # 8-bit grayscale
        "-s", f"{width}x{height}",
        "-framerate", str(fps),
        "-i", pipe2_path,

        # ---------- DEPTH INPUT ----------
        "-f", "rawvideo",
        "-pix_fmt", "gray16le",
        "-s", f"{width}x{height}",
        "-framerate", str(fps),
        "-i", pipe3_path,

        "-filter_complex",
        (
            "[0:v]format=yuv420p[color]"
        ),

        # ---------- Stream 0: Color - AV1 ----------
        "-map", "[color]",
        "-c:v:0", "libsvtav1",
        "-preset", "5",
        "-crf", "35",

        # ---------- Stream 1: Alpha - FFV1 (GRAY) ----------
        "-map", "1:v",
        "-c:v:1", "ffv1",
        "-pix_fmt:v:1", "gray",

        # ---------- Stream 2: Depth - PNG (GRAY16BE) ----------
        "-map", "2:v",
        "-c:v:2", "png",
        "-pix_fmt:v:2", "gray16be",

        file_path,
    ]

    # ---------------------------------------------------------------
    # Start process
    # ---------------------------------------------------------------

    print(f"Starting ffmpeg command: {' '.join(ffmpeg_command)}")
    process = subprocess.Popen(
        ffmpeg_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=False
    )

    rgb_frame_generator = rgb_frame(arr, point_source_ids, width, height)
    alpha_frame_generator = alpha_frame(arr, point_source_ids, width, height)
    depth_frame_generator = depth_frame(arr, point_source_ids, width, height, depth_scale_factor)

    # Start separate threads to write data to the pipes
    t1 = threading.Thread(target=write_to_pipe, args=(pipe1_path, rgb_frame_generator))
    t2 = threading.Thread(target=write_to_pipe, args=(pipe2_path, alpha_frame_generator))
    t3 = threading.Thread(target=write_to_pipe, args=(pipe3_path, depth_frame_generator))
    
    t1.start()
    t2.start()
    t3.start()

    # Wait for the writing threads to complete
    t1.join()
    t2.join()
    t3.join()
    
    # Wait for the ffmpeg process to finish
    stdout, stderr = process.communicate()
    print("FFmpeg finished execution.")

    if process.returncode != 0:
        print("FFmpeg Error:")
        print(stderr.decode())
    else:
        print(f"Successfully created {file_path}")

    # 5. Clean up the named pipes
    os.remove(pipe1_path)
    os.remove(pipe2_path)
    os.remove(pipe3_path)
    print("Cleaned up pipes.")

    # ---------------------------------------------------------------
    # Manifest
    # ---------------------------------------------------------------
    manifest = {
        "file": file_name,
        "width": int(width),
        "height": int(height),
        "fps": int(fps),
        "depth_scale_factor": depth_scale_factor
    }

    with open(manifest_path, "w", encoding="utf-8") as mf:
        json.dump(manifest, mf, indent=2)

    print(f"Wrote {len(point_source_ids)} frames.")
    print(f"Manifest written to {manifest_path}")

def main():
    parser = argparse.ArgumentParser(
        description="Encode LAS as Color/Alpha/Depth MKV (two FFmpeg inputs)"
    )
    parser.add_argument("--input", required=True)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--height", required=True, type=int)
    parser.add_argument("--fps", required=True, type=int)
    parser.add_argument("--output", default="../server/frames")
    parser.add_argument("--manifest", default="../server/manifest/frames.json")
    args = parser.parse_args()

    arr = run_pipeline(args.input)
    encode_color_alpha_depth_streams(
        arr, args.width, args.height, args.fps, args.output, args.manifest
    )

if __name__ == "__main__":
    main()
