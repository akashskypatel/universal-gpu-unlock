"""CPU-only tests of benchmark device selection and result attribution.

CUDA is not required and is never initialised: tools/benchmark_4090.py imports
torch lazily inside its measurement path, so its attribution logic is testable
here. A benchmark result must never be attributable to an unnamed device.
SPDX-License-Identifier: GPL-2.0-only
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "benchmark_4090.py"
spec = importlib.util.spec_from_file_location("benchmark_legacy_4090", SCRIPT)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class LegacyBenchmarkSelectionTests(unittest.TestCase):
    def test_explicit_ordinals_resolve(self):
        for spec_text, expected in (("0", 0), ("1", 1), ("7", 7)):
            with self.subTest(spec=spec_text):
                self.assertEqual(benchmark.selected_device(spec_text, 8), expected)

    def test_out_of_range_ordinal_is_refused_not_clamped(self):
        for spec_text, count in (("2", 2), ("8", 8), ("1", 1)):
            with self.subTest(spec=spec_text, count=count), \
                    self.assertRaises(ValueError):
                benchmark.selected_device(spec_text, count)

    def test_invalid_ordinal_is_refused(self):
        for spec_text in ("auto", "", "gpu0", "cuda:0", "-1", "1.5", None):
            with self.subTest(spec=spec_text), self.assertRaises(ValueError):
                benchmark.selected_device(spec_text, 2)

    def test_no_devices_is_refused(self):
        with self.assertRaises(ValueError):
            benchmark.selected_device("0", 0)

    def test_identity_names_the_measured_device(self):
        props = SimpleNamespace(name="NVIDIA GeForce RTX 4090", pci_bus_id="00000000:01:00.0")
        line = benchmark.device_identity(1, props)
        self.assertIn("CUDA_INDEX=1", line)
        self.assertIn("NAME=NVIDIA GeForce RTX 4090", line)
        self.assertIn("PCI_BUS_ID=00000000:01:00.0", line)
        self.assertIn("ATTRIBUTION=MEASURED_NOT_VERIFIED", line)

    def test_missing_identity_fields_stay_explicit(self):
        line = benchmark.device_identity(0, SimpleNamespace())
        self.assertIn("NAME=UNKNOWN", line)
        self.assertIn("PCI_BUS_ID=UNKNOWN", line)


class LegacyBenchmarkSourceContractTests(unittest.TestCase):
    def test_script_is_explicitly_legacy(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("LEGACY_SINGLE_DEVICE_SCRIPT=1", source)
        self.assertIn("LEGACY_SINGLE_DEVICE=1", source)
        self.assertIn("LEGACY_DEFAULT_DEVICE_USED=1", source)
        self.assertIn('ATTRIBUTION = "MEASURED_NOT_VERIFIED"', source)
        self.assertIn("ATTRIBUTION={ATTRIBUTION}", source)
        self.assertIn("THROUGHPUT_IS_NOT_UNLOCK_EVIDENCE=1", source)

    def test_no_hard_coded_device_zero_execution_path(self):
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn('torch.device("cuda:0")', source)
        self.assertNotIn("torch.cuda.get_device_properties(0)", source)
        self.assertIn('torch.device(f"cuda:{device_index}")', source)
        self.assertIn("torch.cuda.get_device_properties(device_index)", source)

    def test_import_does_not_require_torch(self):
        # The module is already imported at collection time without torch.
        self.assertFalse(hasattr(benchmark, "torch"))


if __name__ == "__main__":
    unittest.main()
