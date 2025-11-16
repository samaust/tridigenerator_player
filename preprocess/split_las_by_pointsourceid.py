#!/usr/bin/env python3
"""
Split a LAS file (PDRF2) by PointSourceId and export GPU-ready binary frames.

Each frame_{id}.bin file layout (little endian):
-------------------------------------------------
uint32 width
uint32 height
uint32 vertex_count
float32[vertex_count*3] positions  (X, Y, Z)
uint8[vertex_count*4]  colors_cls  (R, G, B, Classification)

Notes:
- Classification is stored directly as uint8.
- RGB values are scaled to 0-255 if needed.
- Width * Height must equal vertex_count (you provide via args).

Classification = 3 : Visible point
Classification = 7 : Invisible point
"""
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
    arr = pipe.arrays[0]
    return arr


def export_frames(arr, width, height, output_dir, manifest_path):
    """Group by PointSourceId and write binary per frame."""
    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

    point_source_ids = np.unique(arr['PointSourceId'])

    manifest = {"frames": [], "width": int(width), "height": int(height)}

    for pid in point_source_ids:
        subset = arr[arr['PointSourceId'] == pid]
        n = subset.size
        expected_n = width * height
        if n != expected_n:
            raise ValueError(
                f"Frame {pid}: expected {expected_n} points "
                f"but got {n}. Check width/height or LAS content."
            )

        # Extract columns
        X = subset['X'].astype(np.float32)
        Y = subset['Y'].astype(np.float32)
        Z = subset['Z'].astype(np.float32)

        R = subset['Red'].astype(np.float32)
        G = subset['Green'].astype(np.float32)
        B = subset['Blue'].astype(np.float32)

        # Normalize color if values exceed 255 (common LAS scaling)
        max_rgb = max(R.max(), G.max(), B.max())
        if max_rgb > 255.0:
            R = np.clip(R / max_rgb * 255.0, 0, 255)
            G = np.clip(G / max_rgb * 255.0, 0, 255)
            B = np.clip(B / max_rgb * 255.0, 0, 255)

        R = R.astype(np.uint8)
        G = G.astype(np.uint8)
        B = B.astype(np.uint8)
        Classification = subset['Classification'].astype(np.uint8)

        # Write binary
        frame_name = f"frame_{int(pid):05d}.bin"
        frame_path = os.path.join(output_dir, frame_name)
        with open(frame_path, "wb") as f:
            # header
            f.write(struct.pack("<III", width, height, n))
            # positions (XYZ)
            np.stack([X, Y, Z], axis=1).astype('<f4').tofile(f)
            # colors + class (RGBA)
            rgba = np.stack([R, G, B, Classification], axis=1).astype('<u1')
            rgba.tofile(f)

        # TODO : add support for subfolder
        manifest["frames"].append({"id": int(pid), "file": f"{frame_name}", "points": int(n)})

    # Write manifest
    with open(manifest_path, "w", encoding="utf-8") as mf:
        json.dump(manifest, mf, indent=2)
    print(f"Wrote {len(point_source_ids)} frames -> {output_dir}")
    print(f"Manifest written to {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description="Split LAS by PointSourceId")
    parser.add_argument("--input", required=True, help="Path to input LAS file")
    parser.add_argument("--width", required=True, type=int, help="Grid width (vertex count in x)")
    parser.add_argument("--height", required=True, type=int, help="Grid height (vertex count in y)")
    parser.add_argument("--output", default="../server/frames", help="Output directory for binary frames")
    parser.add_argument("--manifest", default="../server/manifest/frames.json", help="Manifest output path")
    args = parser.parse_args()

    arr = run_pipeline(args.input)
    export_frames(arr, args.width, args.height, args.output, args.manifest)


if __name__ == "__main__":
    main()
