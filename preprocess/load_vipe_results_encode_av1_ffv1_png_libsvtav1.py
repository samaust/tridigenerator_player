#!/usr/bin/env python3
"""Encode a ViPE result sequence as synchronized color, mask, and depth video.

The Matroska output contains AV1 color, lossless FFV1 instance IDs, and
lossless PNG-compressed uint16 depth.  A JSON sidecar contains the information
needed to turn the quantized depth back into metres, together with per-frame
camera-to-world poses and camera intrinsics.

This script intentionally imports OpenCV and OpenEXR lazily so ``--help`` can
still explain the required dependencies when it is run outside ViPE's venv.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
from typing import Iterator
import zipfile

import numpy as np


class VipeEncodingError(RuntimeError):
    """Raised when ViPE artifacts cannot be encoded safely."""


def require_executable(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise VipeEncodingError(f"Required executable is not on PATH: {name}")
    return path


def load_image_modules():
    try:
        import cv2  # type: ignore
        import Imath  # type: ignore
        import OpenEXR  # type: ignore
    except ImportError as exc:
        raise VipeEncodingError(
            "OpenCV and OpenEXR are required. Run this script with ViPE's "
            f"virtual environment (missing module: {exc.name})."
        ) from exc
    return cv2, Imath, OpenEXR


def run_json(command: list[str]) -> dict:
    try:
        result = subprocess.run(
            command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
    except subprocess.CalledProcessError as exc:
        raise VipeEncodingError(
            f"Command failed ({' '.join(command)}):\n{exc.stderr.strip()}"
        ) from exc
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise VipeEncodingError(f"Command returned invalid JSON: {' '.join(command)}") from exc


def numeric_archive_entries(path: Path, suffix: str) -> list[tuple[int, str]]:
    with zipfile.ZipFile(path) as archive:
        names = [name for name in archive.namelist() if not name.endswith("/")]
    entries: list[tuple[int, str]] = []
    for name in names:
        member = Path(name)
        if member.suffix.lower() != suffix:
            raise VipeEncodingError(f"Unexpected member in {path}: {name}")
        try:
            frame_index = int(member.stem)
        except ValueError as exc:
            raise VipeEncodingError(f"Non-numeric frame name in {path}: {name}") from exc
        entries.append((frame_index, name))
    entries.sort()
    if len({index for index, _ in entries}) != len(entries):
        raise VipeEncodingError(f"Duplicate frame indices in {path}")
    return entries


def discover_sequence(root: Path, requested: str | None) -> str:
    rgb_dir = root / "rgb"
    if not rgb_dir.is_dir():
        raise VipeEncodingError(f"Missing ViPE rgb directory: {rgb_dir}")
    names = sorted(path.stem for path in rgb_dir.glob("*.mp4"))
    if requested:
        if requested not in names:
            raise VipeEncodingError(
                f"Sequence {requested!r} has no matching file in {rgb_dir}"
            )
        return requested
    if not names:
        raise VipeEncodingError(f"No MP4 sequences found in {rgb_dir}")
    if len(names) != 1:
        raise VipeEncodingError(
            "More than one sequence was found; select one with --sequence: "
            + ", ".join(names)
        )
    return names[0]


def probe_source(ffprobe: str, path: Path) -> dict:
    probe = run_json(
        [
            ffprobe,
            "-v", "error",
            "-select_streams", "v:0",
            "-count_frames",
            "-show_entries",
            "stream=width,height,avg_frame_rate,r_frame_rate,nb_read_frames,nb_frames",
            "-of", "json",
            str(path),
        ]
    )
    streams = probe.get("streams", [])
    if len(streams) != 1:
        raise VipeEncodingError(f"Expected one video stream in {path}, found {len(streams)}")
    stream = streams[0]
    rate_text = stream.get("avg_frame_rate") or stream.get("r_frame_rate")
    try:
        rate_num, rate_den = (int(value) for value in rate_text.split("/"))
    except (AttributeError, TypeError, ValueError) as exc:
        raise VipeEncodingError(f"Could not determine frame rate for {path}") from exc
    if rate_num <= 0 or rate_den <= 0:
        raise VipeEncodingError(f"Invalid frame rate for {path}: {rate_text}")
    count_text = stream.get("nb_read_frames") or stream.get("nb_frames")
    try:
        frame_count = int(count_text)
    except (TypeError, ValueError) as exc:
        raise VipeEncodingError(f"Could not determine frame count for {path}") from exc
    return {
        "width": int(stream["width"]),
        "height": int(stream["height"]),
        "frame_count": frame_count,
        "rate_num": rate_num,
        "rate_den": rate_den,
        "rate": f"{rate_num}/{rate_den}",
    }


def read_exr_member(archive, name: str, OpenEXR, Imath) -> np.ndarray:
    with archive.open(name) as source:
        try:
            exr = OpenEXR.InputFile(source)
            header = exr.header()
            data_window = header["dataWindow"]
            width = data_window.max.x - data_window.min.x + 1
            height = data_window.max.y - data_window.min.y + 1
            half = Imath.PixelType(Imath.PixelType.HALF)
            data = np.frombuffer(exr.channel("Z", half), dtype=np.float16)
            exr.close()
        except Exception as exc:
            raise VipeEncodingError(f"Failed to read EXR member {name}: {exc}") from exc
    if data.size != width * height:
        raise VipeEncodingError(
            f"EXR {name} has {data.size} values, expected {width * height}"
        )
    return data.reshape(height, width).astype(np.float32)


def decode_mask(data: bytes, name: str, cv2) -> np.ndarray:
    mask = cv2.imdecode(np.frombuffer(data, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if mask is None:
        raise VipeEncodingError(f"Failed to decode mask {name}")
    if mask.ndim != 2 or mask.dtype != np.uint8:
        raise VipeEncodingError(
            f"Mask {name} must be single-channel uint8, got shape={mask.shape}, dtype={mask.dtype}"
        )
    return mask


def parse_camera_models(path: Path, expected_indices: list[int]) -> list[str]:
    models: dict[int, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw_line.strip():
            continue
        try:
            index_text, model = raw_line.split(":", 1)
            index = int(index_text.strip())
        except ValueError as exc:
            raise VipeEncodingError(f"Malformed camera model at {path}:{line_number}") from exc
        models[index] = model.strip()
    if sorted(models) != expected_indices:
        raise VipeEncodingError(f"Camera-model indices do not match video frames in {path}")
    unsupported = sorted({model for model in models.values() if model != "PINHOLE"})
    if unsupported:
        raise VipeEncodingError(
            "This encoder currently supports PINHOLE sequences only; found: "
            + ", ".join(unsupported)
        )
    return [models[index] for index in expected_indices]


def parse_mask_labels(path: Path) -> dict[str, str]:
    labels: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw_line.strip():
            continue
        try:
            index_text, label = raw_line.split(":", 1)
            index = int(index_text.strip())
        except ValueError as exc:
            raise VipeEncodingError(f"Malformed mask label at {path}:{line_number}") from exc
        if not 0 <= index <= 255:
            raise VipeEncodingError(f"Mask ID outside uint8 range at {path}:{line_number}")
        labels[str(index)] = label.strip()
    return labels


def validate_npz(path: Path, frame_indices: list[int], shape: tuple[int, ...], label: str):
    try:
        with np.load(path, allow_pickle=False) as values:
            data = np.asarray(values["data"])
            indices = np.asarray(values["inds"])
    except (OSError, KeyError, ValueError) as exc:
        raise VipeEncodingError(f"Could not load {label} artifact {path}: {exc}") from exc
    if data.shape != shape:
        raise VipeEncodingError(f"{label} data in {path} has shape {data.shape}, expected {shape}")
    if indices.ndim != 1 or indices.tolist() != frame_indices:
        raise VipeEncodingError(f"{label} indices in {path} do not match video frames")
    if not np.isfinite(data).all():
        raise VipeEncodingError(f"{label} data in {path} contains non-finite values")
    return data.astype(np.float32, copy=False)


def inspect_artifacts(root: Path, sequence: str, ffprobe: str) -> dict:
    paths = {
        "rgb": root / "rgb" / f"{sequence}.mp4",
        "mask": root / "mask" / f"{sequence}.zip",
        "mask_labels": root / "mask" / f"{sequence}.txt",
        "depth": root / "depth" / f"{sequence}.zip",
        "pose": root / "pose" / f"{sequence}.npz",
        "intrinsics": root / "intrinsics" / f"{sequence}.npz",
        "camera": root / "intrinsics" / f"{sequence}_camera.txt",
    }
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise VipeEncodingError("Missing required artifacts:\n  " + "\n  ".join(missing))

    video = probe_source(ffprobe, paths["rgb"])
    frame_indices = list(range(video["frame_count"]))
    mask_entries = numeric_archive_entries(paths["mask"], ".png")
    depth_entries = numeric_archive_entries(paths["depth"], ".exr")
    if [index for index, _ in mask_entries] != frame_indices:
        raise VipeEncodingError("Mask archive frame indices do not match RGB frames")
    if [index for index, _ in depth_entries] != frame_indices:
        raise VipeEncodingError("Depth archive frame indices do not match RGB frames")

    poses = validate_npz(paths["pose"], frame_indices, (video["frame_count"], 4, 4), "pose")
    intrinsics = validate_npz(
        paths["intrinsics"], frame_indices, (video["frame_count"], 4), "intrinsics"
    )
    camera_models = parse_camera_models(paths["camera"], frame_indices)
    labels = parse_mask_labels(paths["mask_labels"])
    return {
        "paths": paths,
        "video": video,
        "indices": frame_indices,
        "mask_entries": mask_entries,
        "depth_entries": depth_entries,
        "poses": poses,
        "intrinsics": intrinsics,
        "camera_models": camera_models,
        "labels": labels,
    }


def validate_images(info: dict, cv2, Imath, OpenEXR) -> dict:
    height = info["video"]["height"]
    width = info["video"]["width"]
    expected_shape = (height, width)
    maximum = 0.0
    valid_pixels = 0
    invalid_pixels = 0

    with zipfile.ZipFile(info["paths"]["mask"]) as masks:
        for _, name in info["mask_entries"]:
            mask = decode_mask(masks.read(name), name, cv2)
            if mask.shape != expected_shape:
                raise VipeEncodingError(
                    f"Mask {name} has shape {mask.shape}, expected {expected_shape}"
                )

    with zipfile.ZipFile(info["paths"]["depth"]) as depths:
        for _, name in info["depth_entries"]:
            depth = read_exr_member(depths, name, OpenEXR, Imath)
            if depth.shape != expected_shape:
                raise VipeEncodingError(
                    f"Depth {name} has shape {depth.shape}, expected {expected_shape}"
                )
            valid = np.isfinite(depth) & (depth > 0)
            count = int(np.count_nonzero(valid))
            valid_pixels += count
            invalid_pixels += depth.size - count
            if count:
                maximum = max(maximum, float(np.max(depth[valid])))
    if not math.isfinite(maximum) or maximum <= 0:
        raise VipeEncodingError("Depth archive contains no finite positive depth values")
    return {
        "max_depth_metres": maximum,
        "units_per_metre": 65535.0 / maximum,
        "valid_pixels": valid_pixels,
        "invalid_pixels": invalid_pixels,
    }


def mask_frames(info: dict, cv2) -> Iterator[bytes]:
    with zipfile.ZipFile(info["paths"]["mask"]) as archive:
        for _, name in info["mask_entries"]:
            yield decode_mask(archive.read(name), name, cv2).tobytes(order="C")


def quantize_depth(depth: np.ndarray, units_per_metre: float) -> np.ndarray:
    valid = np.isfinite(depth) & (depth > 0)
    encoded = np.zeros(depth.shape, dtype=np.uint16)
    scaled = np.rint(np.clip(depth[valid] * units_per_metre, 1, 65535))
    encoded[valid] = scaled.astype(np.uint16)
    return encoded.astype(">u2", copy=False)


def depth_frames(info: dict, depth_info: dict, Imath, OpenEXR) -> Iterator[bytes]:
    with zipfile.ZipFile(info["paths"]["depth"]) as archive:
        for _, name in info["depth_entries"]:
            depth = read_exr_member(archive, name, OpenEXR, Imath)
            yield quantize_depth(depth, depth_info["units_per_metre"]).tobytes(order="C")


def write_frames(fd: int, frames: Iterator[bytes], errors: queue.Queue) -> None:
    try:
        with os.fdopen(fd, "wb", buffering=0) as output:
            for frame in frames:
                output.write(frame)
    except BrokenPipeError as exc:
        errors.put(exc)
    except BaseException as exc:
        errors.put(exc)
        try:
            os.close(fd)
        except OSError:
            pass


def encode_video(
    info: dict,
    depth_info: dict,
    output_path: Path,
    ffmpeg: str,
    cv2,
    Imath,
    OpenEXR,
    preset: int,
    crf: int,
) -> None:
    mask_read, mask_write = os.pipe()
    depth_read, depth_write = os.pipe()
    video = info["video"]
    temporary_output = output_path.with_name(f".{output_path.name}.partial")
    temporary_output.unlink(missing_ok=True)
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-i", str(info["paths"]["rgb"]),
        "-f", "rawvideo", "-pix_fmt", "gray", "-video_size", f"{video['width']}x{video['height']}",
        "-framerate", video["rate"], "-i", f"pipe:{mask_read}",
        "-f", "rawvideo", "-pix_fmt", "gray16be", "-video_size", f"{video['width']}x{video['height']}",
        "-framerate", video["rate"], "-i", f"pipe:{depth_read}",
        "-map", "0:v:0", "-c:v:0", "libsvtav1", "-preset:v:0", str(preset),
        "-crf:v:0", str(crf), "-pix_fmt:v:0", "yuv420p", "-metadata:s:v:0", "title=COLOR",
        "-map", "1:v:0", "-c:v:1", "ffv1", "-pix_fmt:v:1", "gray",
        "-metadata:s:v:1", "title=MASK",
        "-map", "2:v:0", "-c:v:2", "png", "-pix_fmt:v:2", "gray16be",
        "-metadata:s:v:2", "title=DEPTH",
        "-an", "-sn",
        "-frames:v:0", str(video["frame_count"]),
        "-frames:v:1", str(video["frame_count"]),
        "-frames:v:2", str(video["frame_count"]),
        "-f", "matroska",
        str(temporary_output),
    ]
    errors: queue.Queue = queue.Queue()
    with tempfile.NamedTemporaryFile(mode="w+b", suffix=".ffmpeg.log") as log:
        try:
            process = subprocess.Popen(
                command,
                stdout=subprocess.DEVNULL,
                stderr=log,
                pass_fds=(mask_read, depth_read),
            )
        finally:
            os.close(mask_read)
            os.close(depth_read)
        producers = [
            threading.Thread(
                target=write_frames,
                args=(mask_write, mask_frames(info, cv2), errors),
                name="mask-writer",
            ),
            threading.Thread(
                target=write_frames,
                args=(depth_write, depth_frames(info, depth_info, Imath, OpenEXR), errors),
                name="depth-writer",
            ),
        ]
        for producer in producers:
            producer.start()
        for producer in producers:
            producer.join()
        return_code = process.wait()
        log.seek(0)
        ffmpeg_log = log.read().decode("utf-8", errors="replace").strip()
    producer_errors = []
    while not errors.empty():
        producer_errors.append(errors.get())
    if return_code != 0 or producer_errors:
        temporary_output.unlink(missing_ok=True)
        details = [f"FFmpeg exited with status {return_code}"]
        details.extend(f"Producer failed: {error}" for error in producer_errors)
        if ffmpeg_log:
            details.append(ffmpeg_log)
        raise VipeEncodingError("\n".join(details))
    os.replace(temporary_output, output_path)


def verify_output(ffprobe: str, output_path: Path, info: dict) -> None:
    probe = run_json(
        [
            ffprobe, "-v", "error", "-count_frames",
            "-show_entries", "stream=index,codec_name,codec_type,pix_fmt,width,height,nb_read_frames",
            "-of", "json", str(output_path),
        ]
    )
    streams = [stream for stream in probe.get("streams", []) if stream.get("codec_type") == "video"]
    expected = [("av1", "yuv420p"), ("ffv1", "gray"), ("png", "gray16be")]
    if len(streams) != 3:
        raise VipeEncodingError(f"Encoded output has {len(streams)} video streams, expected 3")
    for stream, (codec, pixel_format) in zip(streams, expected):
        actual = (stream.get("codec_name"), stream.get("pix_fmt"))
        if actual != (codec, pixel_format):
            raise VipeEncodingError(
                f"Stream {stream.get('index')} is {actual}, expected {(codec, pixel_format)}"
            )
        if (int(stream["width"]), int(stream["height"])) != (
            info["video"]["width"], info["video"]["height"]
        ):
            raise VipeEncodingError(f"Stream {stream.get('index')} has incorrect dimensions")
        if int(stream.get("nb_read_frames", -1)) != info["video"]["frame_count"]:
            raise VipeEncodingError(f"Stream {stream.get('index')} has incorrect frame count")


def make_manifest(sequence: str, output_path: Path, info: dict, depth_info: dict) -> dict:
    video = info["video"]
    return {
        "schema_version": 1,
        "file": output_path.name,
        "sequence": sequence,
        "frame_count": video["frame_count"],
        "width": video["width"],
        "height": video["height"],
        "fps": video["rate_num"] / video["rate_den"],
        "frame_rate": {"numerator": video["rate_num"], "denominator": video["rate_den"]},
        "streams": {
            "color": {"index": 0, "codec": "av1", "pixel_format": "yuv420p"},
            "mask": {"index": 1, "codec": "ffv1", "pixel_format": "gray"},
            "depth": {"index": 2, "codec": "png", "pixel_format": "gray16be"},
        },
        "depth": {
            "encoding": "uint16_linear",
            "units": "metres",
            "units_per_metre": depth_info["units_per_metre"],
            "depth_scale_factor": depth_info["units_per_metre"],
            "invalid_value": 0,
            "max_depth_metres": depth_info["max_depth_metres"],
            "valid_source_pixels": depth_info["valid_pixels"],
            "invalid_source_pixels": depth_info["invalid_pixels"],
        },
        "pose": {
            "type": "camera_to_world",
            "matrix_layout": "row_major",
            "coordinate_convention": "opencv_x_right_y_down_z_forward",
            "frame_indices": info["indices"],
            "matrices": info["poses"].reshape(video["frame_count"], 16).tolist(),
        },
        "intrinsics": {
            "layout": ["fx", "fy", "cx", "cy"],
            "units": "pixels",
            "frame_indices": info["indices"],
            "camera_models": info["camera_models"],
            "values": info["intrinsics"].tolist(),
        },
        "mask_labels": info["labels"],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Encode ViPE results as synchronized AV1 color, FFV1 mask, and PNG uint16 depth"
    )
    parser.add_argument("--input", required=True, type=Path, help="ViPE results directory")
    parser.add_argument("--output", type=Path, default=Path("vipe_encoded"), help="Output directory")
    parser.add_argument("--sequence", help="Sequence basename (required if input contains multiple sequences)")
    parser.add_argument("--overwrite", action="store_true", help="Replace existing MKV and JSON outputs")
    parser.add_argument("--svt-preset", type=int, default=5, help="libsvtav1 preset (default: 5)")
    parser.add_argument("--svt-crf", type=int, default=35, help="libsvtav1 CRF (default: 35)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        root = args.input.expanduser().resolve()
        if not root.is_dir():
            raise VipeEncodingError(f"Input is not a directory: {root}")
        if not 0 <= args.svt_preset <= 13:
            raise VipeEncodingError("--svt-preset must be between 0 and 13")
        if not 0 <= args.svt_crf <= 63:
            raise VipeEncodingError("--svt-crf must be between 0 and 63")
        ffmpeg = require_executable("ffmpeg")
        ffprobe = require_executable("ffprobe")
        cv2, Imath, OpenEXR = load_image_modules()
        sequence = discover_sequence(root, args.sequence)
        output_dir = args.output.expanduser().resolve()
        output_path = output_dir / f"{sequence}.mkv"
        manifest_path = output_dir / f"{sequence}.json"
        existing = [path for path in (output_path, manifest_path) if path.exists()]
        if existing and not args.overwrite:
            raise VipeEncodingError(
                "Output already exists (use --overwrite): " + ", ".join(map(str, existing))
            )
        output_dir.mkdir(parents=True, exist_ok=True)

        print(f"Inspecting ViPE sequence {sequence!r}...")
        info = inspect_artifacts(root, sequence, ffprobe)
        depth_info = validate_images(info, cv2, Imath, OpenEXR)
        print(
            f"Encoding {info['video']['frame_count']} frames at {info['video']['rate']} fps; "
            f"depth range ends at {depth_info['max_depth_metres']:.6g} m"
        )
        encode_video(
            info, depth_info, output_path, ffmpeg, cv2, Imath, OpenEXR,
            args.svt_preset, args.svt_crf,
        )
        verify_output(ffprobe, output_path, info)
        manifest = make_manifest(sequence, output_path, info, depth_info)
        temporary_manifest = manifest_path.with_name(f".{manifest_path.name}.partial")
        temporary_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary_manifest, manifest_path)
        print(f"Wrote {output_path}")
        print(f"Wrote {manifest_path}")
        return 0
    except VipeEncodingError as exc:
        print(f"error: {exc}", file=__import__("sys").stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
