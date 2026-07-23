import importlib.util
from pathlib import Path
from unittest import mock
import unittest

import numpy as np

MODULE_PATH = Path(__file__).with_name(
    "load_vipe_results_encode_av1_ffv1_png_libsvtav1.py"
)
SPEC = importlib.util.spec_from_file_location("vipe_audio_preprocess", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AudioPreservationTests(unittest.TestCase):
    def test_probe_source_discovers_first_audio_stream(self):
        probe = {
            "streams": [
                {
                    "index": 0,
                    "codec_name": "h264",
                    "codec_type": "video",
                    "width": 1280,
                    "height": 720,
                    "avg_frame_rate": "30/1",
                    "nb_read_frames": "30",
                },
                {
                    "index": 1,
                    "codec_name": "aac",
                    "codec_type": "audio",
                    "sample_rate": "44100",
                    "channels": 2,
                    "duration": "1.0",
                },
            ]
        }
        with mock.patch.object(MODULE, "run_json", return_value=probe):
            result = MODULE.probe_source("ffprobe", Path("source.mp4"))
        self.assertEqual(
            result["audio"],
            {
                "codec": "aac",
                "sample_rate": 44100,
                "channels": 2,
                "duration": 1.0,
            },
        )

    def test_probe_source_allows_missing_audio(self):
        probe = {
            "streams": [{
                "codec_type": "video",
                "width": 16,
                "height": 16,
                "avg_frame_rate": "24/1",
                "nb_read_frames": "1",
            }]
        }
        with mock.patch.object(MODULE, "run_json", return_value=probe):
            result = MODULE.probe_source("ffprobe", Path("silent.mp4"))
        self.assertIsNone(result["audio"])

    def test_manifest_audio_is_optional_and_keeps_video_indices(self):
        info = {
            "video": {
                "frame_count": 1,
                "width": 16,
                "height": 16,
                "rate_num": 30,
                "rate_den": 1,
                "audio": {
                    "codec": "aac",
                    "sample_rate": 44100,
                    "channels": 2,
                    "duration": 1.0,
                },
            },
            "indices": [0],
            "poses": np.eye(4, dtype=np.float32)[None, ...],
            "intrinsics": np.array([[1, 1, 0, 0]], dtype=np.float32),
            "camera_models": ["PINHOLE"],
            "labels": {},
        }
        depth = {
            "units_per_metre": 1.0,
            "max_depth_metres": 1.0,
            "valid_pixels": 1,
            "invalid_pixels": 0,
        }
        manifest = MODULE.make_manifest(
            "sample", Path("sample.mkv"), info, depth, {}
        )
        self.assertEqual(manifest["streams"]["color"]["index"], 0)
        self.assertEqual(manifest["streams"]["mask"]["index"], 1)
        self.assertEqual(manifest["streams"]["depth"]["index"], 2)
        self.assertEqual(
            manifest["streams"]["audio"],
            {"index": 3, "codec": "aac", "sample_rate": 44100, "channels": 2},
        )

        info["video"]["audio"] = None
        silent = MODULE.make_manifest(
            "silent", Path("silent.mkv"), info, depth, {}
        )
        self.assertNotIn("audio", silent["streams"])


if __name__ == "__main__":
    unittest.main()
