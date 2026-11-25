"""Source contracts for the legacy 16-record inventory boundary.

The 16-device structure is an intentional bound for bounded callers, not a GPU
count policy. Boot discovery must stay runtime-sized, so the legacy type and
its fixed arrays may not appear in the boot entrypoint. No hardware is used.
SPDX-License-Identifier: GPL-2.0-only
"""
from __future__ import annotations

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
DISCOVERY = (SRC / "gpu_discovery.c").read_text(encoding="utf-8")
INVENTORY_H = (SRC / "gpu_inventory.h").read_text(encoding="utf-8")
DISCOVERY_H = (SRC / "gpu_discovery.h").read_text(encoding="utf-8")
INVENTORY_C = (SRC / "gpu_inventory.c").read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(r"\bEFI_STATUS\s+" + re.escape(name) + r"\s*\(", source)
    if match is None:
        raise AssertionError("missing function: " + name)
    start = source.index("{", match.end() - 1)
    depth, at = 1, start + 1
    while at < len(source) and depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    if depth:
        raise AssertionError("unterminated function: " + name)
    return source[start:at]


class LegacyInventoryBoundaryTests(unittest.TestCase):
    def test_legacy_type_is_documented_as_bounded_only(self):
        header = INVENTORY_H
        self.assertIn("GU_INVENTORY_MAX_NVIDIA 16U", header)
        self.assertIn("Legacy fixed-capacity view for bounded callers only", header)
        flat = " ".join(re.sub(r"\*", " ", header).split())
        self.assertIn("boot discovery allocates runtime-sized record storage", flat)
        self.assertIn("must not use GU_GPU_INVENTORY or GuCollectGpuInventory", flat)
        self.assertIn("Dynamic boot discovery uses GuCollectGpuRecords with "
                      "caller-sized storage", flat)

    def test_no_source_module_uses_the_legacy_collector(self):
        for path in sorted(SRC.glob("*.c")):
            if path.name == "gpu_inventory.c":
                continue  # definition site of the bounded collector
            text = path.read_text(encoding="utf-8")
            with self.subTest(path=path.name):
                self.assertNotIn("GuCollectGpuInventory(", text,
                                 "boot sources must use GuCollectGpuRecords")

    def test_boot_discovery_is_runtime_sized(self):
        body = function_body(DISCOVERY, "GuDiscoverSingleGpu")
        self.assertIn("AllocateDiscoveryStorage", body)
        self.assertIn("prepared.inventory.capacity", body)
        for legacy in ("GU_GPU_PREPARED_SEQUENCE", "GuPrepareGpuSequence(",
                       "GuRefreshPreparedSequence(", "GU_GPU_INVENTORY"):
            with self.subTest(symbol=legacy):
                self.assertNotIn(legacy, body)

    def test_bounded_adapters_remain_explicitly_bounded(self):
        self.assertIn("Bounded compatibility API", DISCOVERY_H)
        self.assertIn("GU_GPU_PREPARED_SEQUENCE", DISCOVERY_H)
        body = function_body(DISCOVERY, "GuPrepareGpuSequence")
        self.assertIn("requested_count>GU_INVENTORY_MAX_NVIDIA", body)
        refreshed = function_body(DISCOVERY, "GuRefreshPreparedSequence")
        self.assertIn("GU_INVENTORY_MAX_NVIDIA", refreshed)

    def test_selection_interface_is_shared_by_dynamic_callers(self):
        self.assertIn("GuFindGpuRecordSequence", DISCOVERY)
        self.assertNotIn("GuFindGpuSequence(", DISCOVERY)
        self.assertIn("GU_INVENTORY_OVERLAP", INVENTORY_C)
        self.assertIn("GU_INVENTORY_OVERLAP = 6", INVENTORY_H)


if __name__ == "__main__":
    unittest.main()
