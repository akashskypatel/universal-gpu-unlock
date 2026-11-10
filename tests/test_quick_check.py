"""CPU-only tests of the multi-GPU throughput survey's arithmetic and selection."""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "quick_check.py"
spec = importlib.util.spec_from_file_location("gpu_quick_check", SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class BenchmarkReportContract(unittest.TestCase):
    def test_tflops_from_measured_latency(self):
        # 4096^3 GEMM with one millisecond = 137.438953472 TFLOPS.
        self.assertAlmostEqual(benchmark.tflops(4096, 1.0), 137.438953472)

    def test_nonfinite_or_zero_latency_refused(self):
        for invalid in (0, -1, float("nan"), float("inf")):
            with self.subTest(value=invalid), self.assertRaises(ValueError):
                benchmark.tflops(4096, invalid)

    def test_invalid_matrix_size_refused(self):
        with self.assertRaises(ValueError):
            benchmark.tflops(0, 1.0)

    def test_explicit_second_gpu(self):
        self.assertEqual(benchmark.selected_devices("1", 2), [1])

    def test_all_gpu_indices(self):
        self.assertEqual(benchmark.selected_devices("all", 2), [0, 1])

    def test_invalid_device_does_not_fall_back_to_gpu_zero(self):
        for bad in ("2", "-1", "auto", "", "gpu0"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                benchmark.selected_devices(bad, 2)

    def test_zero_device_count_refused(self):
        with self.assertRaises(ValueError):
            benchmark.selected_devices("all", 0)

    def test_no_automatic_claim_of_unlock(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("UNLOCK_VERIFIED=false", source)
        self.assertIn("MEASURED_NOT_VERIFIED", source)
        self.assertNotIn("FULLY UNLOCKED", source)
        self.assertNotIn("100% full unrestricted", source)


if __name__ == "__main__":
    unittest.main()
