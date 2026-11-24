"""Contract tests for the bounded firmware watchdog (A8).

The image used to disable the firmware timer on entry and never re-armed it, so
a hang anywhere in the sequence left the machine frozen with a half-written
card. A8 arms a bound instead and cancels it before the handover.

The mechanism is deliberately opt-in: the default build keeps
``GU_WATCHDOG_SECONDS`` at 0, which disables the timer exactly as before. No
timing budget has been measured on hardware, and picking one without a
measurement would trade a frozen screen for a reboot loop.

These tests pin the mechanism, not a threshold:

  1. the timer is programmed in exactly two places, both in `boot_io.c`;
  2. the arm is inside `GuBootOpen`, after the preparation-only early return, so
     a prepare-only build never arms it;
  3. the bound is converted to the firmware's 100 ns unit and an implausible
     value is refused rather than armed;
  4. the report helper only reads; it never programs the timer;
  5. the handover path cancels the timer after the outcome is recorded and
     before the Windows image is started, and says in the log whether a bound was
     in force.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOOT_IO = (ROOT / "src" / "boot_io.c").read_text(encoding="utf-8")
BOOT_IO_H = (ROOT / "src" / "boot_io.h").read_text(encoding="utf-8")
APP = (ROOT / "src" / "app.c").read_text(encoding="utf-8")

ARM = "SetWatchdogTimer(Bound * 10000000U, 0, 0, NULL)"
CANCEL = "SetWatchdogTimer(0, 0, 0, NULL)"


def function_body(text: str, signature: str) -> str:
    """The body of the definition that starts at `signature`, to its last brace."""
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end + 3]


class WatchdogContract(unittest.TestCase):
    def test_the_timer_is_programmed_in_exactly_two_places(self):
        self.assertEqual(BOOT_IO.count("SetWatchdogTimer("), 2,
                         "exactly one arm and one cancel")
        self.assertEqual(BOOT_IO.count(ARM), 1)
        self.assertEqual(BOOT_IO.count(CANCEL), 1)
        for name, text in (("app.c", APP), ("profile.c",
                                            (ROOT / "src" / "profile.c").read_text(encoding="utf-8"))):
            self.assertNotIn("SetWatchdogTimer", text,
                             f"{name} must not program the firmware timer")

    def test_the_default_build_arms_nothing(self):
        self.assertIn("#ifndef GU_WATCHDOG_SECONDS\n#define GU_WATCHDOG_SECONDS 0\n#endif",
                      BOOT_IO)
        # A default build programs a zero timer and returns before the armed
        # flag is ever set, so it behaves exactly as it used to.
        arm = BOOT_IO.index(ARM)
        gate = BOOT_IO.index("if (s == EFI_SUCCESS && Bound) {", arm)
        self.assertLess(arm, gate)
        self.assertLess(gate, BOOT_IO.index("WatchdogArmed = TRUE", gate),
                        "the armed flag is only reachable with a real bound")

    def test_the_arm_belongs_to_open_and_follows_the_prepare_only_return(self):
        open_body = function_body(BOOT_IO, "EFI_STATUS GuBootOpen(EFI_HANDLE image,")
        prepare_only = open_body.index("GU_ACTIVE_SEQUENCE_PREPARE_ONLY")
        call = open_body.index("WatchdogProgram((UINTN)GU_WATCHDOG_SECONDS)")
        self.assertLess(prepare_only, call,
                        "a prepare-only build must never arm the timer")
        # Opening the image is the one site; the programming itself lives with
        # the rest of the timer code, out of the boot-file harnesses' way.
        self.assertIn(ARM, function_body(BOOT_IO, "static EFI_STATUS WatchdogProgram(UINTN Bound) {"))

    def test_an_implausible_bound_is_refused_not_armed(self):
        self.assertIn("#define GU_WATCHDOG_MAX_SECONDS 3600U", BOOT_IO)
        guarded = BOOT_IO.index("if (Bound > (UINTN)GU_WATCHDOG_MAX_SECONDS) {")
        self.assertLess(guarded, BOOT_IO.index(ARM),
                        "the sanity check must precede the programming")
        refusal = BOOT_IO[guarded:BOOT_IO.index(ARM)]
        self.assertIn("WATCHDOG=REFUSED", refusal)
        self.assertIn("REASON=IMPLAUSIBLE_THRESHOLD", refusal)
        self.assertIn("EFI_INVALID_PARAMETER", refusal)

    def test_the_bound_is_converted_to_firmware_units(self):
        # EFI_TIME units are 100 ns: seconds * 10,000,000, and no other scale.
        self.assertIn("* 10000000U", ARM)

    def test_the_report_helper_only_reads(self):
        body = function_body(BOOT_IO, "BOOLEAN GuBootWatchdogArmed(UINTN *seconds) {")
        self.assertNotIn("SetWatchdogTimer", body)
        self.assertIn("WatchdogArmed ? WatchdogSeconds : 0", body,
                      "a build with no bound must report zero, not a stale value")

    def test_the_cancel_reports_and_clears_the_armed_flag(self):
        body = function_body(BOOT_IO, "EFI_STATUS GuBootWatchdogCancel(VOID) {")
        self.assertIn(CANCEL, body)
        self.assertIn("WATCHDOG=CANCELLED SECONDS=", body)
        self.assertIn("WATCHDOG=CANCEL_FAILED", body)
        self.assertIn("WatchdogArmed = FALSE", body)
        self.assertLess(body.index("if (!WatchdogArmed) return EFI_SUCCESS;"),
                        body.index(CANCEL),
                        "an unarmed build must not touch the timer")

    def test_the_handover_cancels_after_the_outcome_and_before_windows(self):
        outcome = APP.index("GU_BOOT_STATE_OUTCOME")
        cancel = APP.index("GuBootWatchdogCancel()", outcome)
        windows = APP.index('stage="start-Windows"', outcome)
        self.assertLess(outcome, cancel,
                        "the timer must outlive the transaction it bounds")
        self.assertLess(cancel, windows,
                        "nothing may stay armed across the handover")

    def test_stop_release_application_cancels_timer_and_returns_to_firmware(self):
        marker = APP.index("STOP_RELEASE_ONLY_IMAGE=1")
        start = APP.rfind("#ifdef GU_STOP_RELEASE_ONLY_BENCH", 0, marker)
        end = APP.index('#else\n  stage="stage-windows-image"', marker)
        from test_unsafe_bench_experiment import without_windows_handoff
        branch = without_windows_handoff(APP[start:end])
        execution = branch.index("GuExecuteStopReleaseSequence(")
        cancel = branch.index("GuBootWatchdogCancel()")
        close = branch.index("GuBootClose()", cancel)
        self.assertLess(execution, cancel)
        self.assertLess(cancel, close)
        self.assertNotIn("GuBootWindows()", branch)
        self.assertIn("OS_HANDOFF=NONE", branch)

    def test_the_run_says_whether_a_bound_was_in_force(self):
        self.assertIn('GuLog("WATCHDOG BOUND=%u SECONDS ACTIVE=1\\n"', APP)
        self.assertIn("REASON=NO_BUILD_THRESHOLD", APP)
        # Reported before the first mutation, so the log order alone says
        # whether the transaction ran bounded.
        self.assertLess(APP.index("WATCHDOG BOUND=%u"), APP.index("GuExecutePreparedSequence"))

    def test_the_public_surface_is_declared(self):
        self.assertIn("BOOLEAN GuBootWatchdogArmed(UINTN *seconds);", BOOT_IO_H)
        self.assertIn("EFI_STATUS GuBootWatchdogCancel(VOID);", BOOT_IO_H)


if __name__ == "__main__":
    unittest.main()
