import importlib.util
from pathlib import Path
import unittest

import numpy as np

MODULE_PATH = Path(__file__).with_name("load_vipe_results_encode_av1_ffv1_png_libsvtav1.py")
SPEC = importlib.util.spec_from_file_location("vipe_preprocess", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ColorReferenceTests(unittest.TestCase):
    def test_srgb_transfer_function(self):
        actual = MODULE.srgb_to_linear(np.array([0.0, 0.04045, 1.0]))
        np.testing.assert_allclose(actual, [0.0, 0.0031308, 1.0], rtol=1e-5)

    def test_frame_rejection_and_masks(self):
        rgb = np.array([[[0.5, 0.5, 0.5], [0.8, 0.2, 0.2]],
                        [[0.5, 0.5, 0.5], [0.0, 0.0, 0.0]]])
        mask = np.array([[1, 2], [1, 3]], dtype=np.uint8)
        depth = np.array([[1.0, 1.0], [np.nan, 1.0]], dtype=np.float32)
        rgb_sum, log_sum, count, per_mask = MODULE.accumulate_color_frame(rgb, mask, depth)
        self.assertEqual(count, 2)
        self.assertEqual(set(per_mask), {1, 2})
        self.assertTrue(np.all(np.isfinite(rgb_sum)))
        self.assertTrue(np.isfinite(log_sum))

    def test_reference_chromaticity_has_unit_luminance(self):
        reference = MODULE._finish_color_reference(
            np.array([4.0, 2.0, 1.0]), np.log(0.2) * 2, 2)
        luminance = np.dot(reference["chromaticity"], MODULE.LUMINANCE_WEIGHTS)
        self.assertAlmostEqual(luminance, 1.0)
        self.assertAlmostEqual(reference["log_average_luminance"], 0.2)

    def test_empty_global_reference_fails(self):
        with self.assertRaises(MODULE.VipeEncodingError):
            MODULE._finish_color_reference(np.zeros(3), 0.0, 0)


if __name__ == "__main__":
    unittest.main()
