import argparse
import json
import os
import struct

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


def export_frames(arr, width, height, output_dir, manifest_path):
    """Group by PointSourceId and write binary per frame."""
    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

    point_source_ids = np.unique(arr['PointSourceId'])
    expected_n = width * height

    manifest = {
        "frames": [],
        "width": int(width),
        "height": int(height)
    }

    for pid in point_source_ids:
        subset = arr[arr['PointSourceId'] == pid]
        n = subset.size

        if n != expected_n:
            raise ValueError(
                f"Frame {pid}: expected {expected_n} points but got {n}."
            )

        #
        # Positions (normalized to float32)
        #
        X = subset['X'].astype(np.float32)
        Y = subset['Y'].astype(np.float32)
        Z = subset['Z'].astype(np.float32)

        #
        # Colors (normalize to 0–1 float32)
        #
        R = subset['Red'].astype(np.float32)
        G = subset['Green'].astype(np.float32)
        B = subset['Blue'].astype(np.float32)

        # Normalize if LAS stores 0–65535 or other scaling
        max_rgb = max(R.max(), G.max(), B.max())
        if max_rgb > 1.0:
            R = np.clip(R / max_rgb, 0.0, 1.0)
            G = np.clip(G / max_rgb, 0.0, 1.0)
            B = np.clip(B / max_rgb, 0.0, 1.0)

        #
        # Compute alpha based on classification
        #
        classification = subset['Classification']
        A = np.where(classification == 7, 0.0, 1.0).astype(np.float32)

        #
        # Stack RGBA as float32
        #
        rgba = np.stack([R, G, B, A], axis=1).astype('<f4')

        #
        # Write binary file
        #
        frame_name = f"frame_{int(pid):05d}.bin"
        frame_path = os.path.join(output_dir, frame_name)

        with open(frame_path, "wb") as f:
            # Header
            f.write(struct.pack("<III", width, height, n))

            # XYZ float32 (n × 3)
            np.stack([X, Y, Z], axis=1).astype('<f4').tofile(f)

            # RGBA float32 (n × 4)
            rgba.tofile(f)

        manifest["frames"].append({
            "id": int(pid),
            "file": frame_name,
            "points": int(n)
        })

    #
    # Write manifest JSON
    #
    with open(manifest_path, "w", encoding="utf-8") as mf:
        json.dump(manifest, mf, indent=2)

    print(f"Wrote {len(point_source_ids)} frames into {output_dir}")
    print(f"Manifest written to {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description="Split LAS by PointSourceId into binary frames")
    parser.add_argument("--input", required=True, help="Path to input LAS file")
    parser.add_argument("--width", required=True, type=int, help="Grid width")
    parser.add_argument("--height", required=True, type=int, help="Grid height")
    parser.add_argument("--output", default="../server/frames", help="Output directory for binary frames")
    parser.add_argument("--manifest", default="../server/manifest/frames.json", help="Manifest output path")

    args = parser.parse_args()

    arr = run_pipeline(args.input)
    export_frames(arr, args.width, args.height, args.output, args.manifest)


if __name__ == "__main__":
    main()
