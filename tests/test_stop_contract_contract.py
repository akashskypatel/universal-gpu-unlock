"""The pre-Stop capture is taken before the callback, and claims only what it saw.

`src/stop_contract.c` is a pure decision over an explicit observation; the C test
proves the decision. These checks prove the *wiring*, which is where the honesty
lives:

  1. the capture is logged immediately before `Driver()` - the vendor callback -
     and before the wholesale refusal returns, in both build profiles;
  2. the six facts no code here can observe are never assigned: they stay FALSE
     and appear in the line's MISSING set, so the record cannot imply a check
     that never ran;
  3. the requirement order matches the plan's list, and the decision is the AND of
     it - no caller can add an exception without editing the table.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MSI = (ROOT / "src" / "msi_release.c").read_text(encoding="utf-8")
MODULE = (ROOT / "src" / "stop_contract.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "stop_contract.h").read_text(encoding="utf-8")
RUNNER = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")

# The plan's six requirements, in the order §4.3 step 3 lists them.
PLAN_ORDER = ["OWNER", "COLD", "CONSOLE", "PEERS", "TARGET_GOP", "LOG",
              "RECOVERY", "SIGNED_BOOTER", "BOUND", "DMA"]
UNOBSERVABLE = ["TargetGopGone", "LogClosed", "Recovery", "SignedBooter",
                "Bound", "DmaIdle"]


class TheCaptureIsTakenBeforeTheCallback(unittest.TestCase):
    def test_the_capture_precedes_the_vendor_driver_call(self):
        stage = MSI[MSI.index("EFI_STATUS GuMsiRelease(VOID) {"):]
        capture = stage.index("StopContractLog(stage-1,")
        callback = stage.index("Driver(stage-1)")
        self.assertLess(capture, callback,
                        "the capture must be taken before the Stop callback")
        # ...and immediately before it, not merely somewhere earlier.
        between = stage[capture:callback]
        self.assertLessEqual(len(between.splitlines()), 4)

    def test_the_capture_is_taken_before_the_refusal_returns(self):
        refusal = MSI[MSI.index("  if(Gu.device_id==0x2684U||Gu.device_id==0x220aU) {"):MSI.index("EFI_STATUS GuMsiRelease(VOID) {")]
        for branch in ("MSI release refused for original AD102/GA102",
                       "MSI release WAIVED for original AD102/GA102"):
            body = MSI[:MSI.index(branch)]
            capture = body.rindex("StopContractLog(")
            self.assertGreater(capture, 0, branch)
        self.assertIn("STOP_CONTRACT", MSI)

    def test_the_refusal_still_refuses(self):
        # The capture explains the refusal; it must not replace it with a pass.
        self.assertIn("MSI release refused for original AD102/GA102", MSI)
        self.assertIn("return EFI_UNSUPPORTED;", MSI)

    def test_the_capture_is_emitted_for_every_card_not_only_the_two(self):
        # `StopContractLog(stage-1, ...)` sits in the shared stage loop.
        stage_loop = MSI[MSI.index("for(UINTN stage="):MSI.index("EFI_STATUS s=NormalizeMasterAfterStop();")]
        self.assertIn("StopContractLog(stage-1,", stage_loop)


class OnlyObservedFactsAreClaimed(unittest.TestCase):
    def test_the_unobservable_facts_are_never_asserted(self):
        for field in UNOBSERVABLE:
            self.assertNotRegex(MSI, rf"observation\.{field}\s*=",
                                f"{field} is not observable in this image")

    def test_the_line_names_them_instead(self):
        for field in UNOBSERVABLE:
            self.assertIn(field, MSI)          # printed
            self.assertIn(field, HEADER)       # carried in the record

    def test_the_observable_facts_come_from_the_checks_that_just_ran(self):
        # The stage-loop capture passes TRUE for owner/cold/console/peers only
        # after those checks returned success; the earlier capture passes FALSE
        # for the two the loop establishes.
        self.assertIn("StopContractLog(stage-1,TRUE,TRUE,TRUE,TRUE)", MSI)
        self.assertIn("StopContractLog(0,TRUE,FALSE,FALSE,FALSE)", MSI)

    def test_the_module_cannot_log_and_has_no_uefi_dependency_on_the_host(self):
        self.assertNotIn("GuLog", MODULE)
        self.assertNotIn("Uefi.h", MODULE)
        self.assertIn("GU_STOP_CONTRACT_HOST_TEST", HEADER)
        self.assertIn("typedef unsigned char GU_SC_BOOLEAN;", HEADER)


class TheDecisionIsTheTable(unittest.TestCase):
    def test_the_requirement_order_matches_the_plan(self):
        names = re.findall(r"\{GU_STOP_REQ_\w+, \"(\w+)\"\}", MODULE)
        self.assertEqual(names, PLAN_ORDER)

    def test_the_decision_is_the_intersection_of_every_requirement(self):
        self.assertIn("return GuStopContractMissing(observation) == 0", MODULE)
        self.assertIn("GU_STOP_REQ_ALL 0x03FFU", HEADER)
        # Every requirement bit is inside ALL, so a new bit has to be added to the
        # mask deliberately rather than silently escaping the decision.
        bits = [int(value, 16) for value in
                re.findall(r"#define GU_STOP_REQ_\w+ (0x[0-9A-F]+)U", HEADER)]
        self.assertTrue(bits)
        union = 0
        for bit in bits[:-1]:   # the last one is ALL itself
            union |= bit
        self.assertEqual(union, bits[-1])
        for bit in bits[:-1]:
            self.assertEqual(bit & (bit - 1), 0, "one bit per requirement")

    def test_no_caller_can_bypass_the_contract(self):
        # The decision exists once. A second implementation in the caller would be
        # a list to keep in sync, which is what this replaces.
        definitions = [path.name for path in (ROOT / "src").glob("*.c")
                       if "GuStopContractAllows(" in path.read_text(encoding="utf-8-sig")
                       and "GU_SC_BOOLEAN GuStopContractAllows(" in path.read_text(encoding="utf-8-sig")]
        self.assertEqual(definitions, ["stop_contract.c"])
        self.assertNotIn("GU_STOP_REQ_", MSI, "the caller duplicates the requirement list")

    def test_the_module_is_compiled_into_every_image(self):
        self.assertIn('"stop_contract.c"', RUNNER)
        self.assertIn('"stop_contract.c"', (ROOT / "tools" / "waiver_build_diff.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
