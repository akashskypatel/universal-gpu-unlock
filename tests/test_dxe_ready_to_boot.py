"""Source contract for late DXE entry and one-shot GPU preparation.

This is an offline order check, not an EFI event or GPU execution test.
"""
from pathlib import Path
import re
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/dxe_main.c").read_text(encoding="utf-8")


def function_body(name):
    match = re.search(
        r"\b(?:static\s+)?(?:EFI_STATUS|VOID)\s+(?:EFIAPI\s+)?" +
        re.escape(name) + r"\s*\([^)]*\)\s*\{", SOURCE
    )
    if not match:
        raise AssertionError(f"required function missing: {name}")
    depth, start, offset = 1, match.end(), match.end()
    while depth and offset < len(SOURCE):
        depth += (SOURCE[offset] == "{") - (SOURCE[offset] == "}")
        offset += 1
    if depth:
        raise AssertionError(f"invalid function bounds: {name}")
    return SOURCE[start:offset-1]


class ReadyToBootOrderTests(unittest.TestCase):
    def test_entrypoint_registers_event_only(self):
        entry = function_body("AdaUnlockDriverEntryPoint")
        self.assertIn("CreateEventEx(", entry)
        self.assertIn("&ReadyToBootGroup", entry)
        self.assertIn("GuDxeOnReadyToBoot", entry)
        self.assertNotIn("GuDxeRun(", entry)
        self.assertNotIn("GuInit(", entry)
        self.assertNotIn("GuMsiRelease(", entry)
        self.assertNotIn("GuBootOpen(", entry)

    def test_callback_one_shot_and_closes_event_before_work(self):
        notify = function_body("GuDxeOnReadyToBoot")
        self.assertIn("if(DxeExecuted", notify)
        self.assertLess(notify.index("DxeExecuted=TRUE"), notify.index("GuDxeRun("))
        self.assertLess(notify.index("CloseEvent("), notify.index("GuDxeRun("))

    def test_original_active_guards_retained_in_actual_work(self):
        body = function_body("GuDxeRun")
        self.assertIn("GU_ACTIVE_SEQUENCE_PREPARE_ONLY", body)
        self.assertIn("ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1", body)
        self.assertLess(body.index("GuDiscoverSingleGpu("), body.index("GuMsiRelease("))


if __name__ == "__main__":
    unittest.main()
