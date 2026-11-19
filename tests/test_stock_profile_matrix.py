"""The four stock profiles must stay buildable, and stay different (A4 finding).

The finding this file pins: before the A4 pass, three of the four stock profiles
compiled and then failed to link. The entry-latch API and the four-phase selector
machine were unconditional call sites with `#ifdef`-guarded definitions, so the
"one image, four profiles" matrix was really one image and nobody noticed - the
matrix was never built.

Two things have to stay true:

  1. every profile links, from the same sources, with no extra configuration
     (`tools/run_simulation.py matrix` builds each one and the linker is the
     judge);
  2. each profile has its own selector contract, and says so - the unlocking
     policies write SS1, entry-latched-preserve must not, and only the phased
     policy may claim to detect a repeated SS1 write.

The matrix test is a real compile-and-link of five binaries (roughly fifteen
seconds). Set `GU_SKIP_BUILD_TESTS=1` to skip it in a hurry; CI and the release
check do not set it.
"""
from pathlib import Path
import os
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
VENDOR = (SRC / "vendor" / "unlock_610.c").read_text(encoding="utf-8")
MSI = (SRC / "msi_release.c").read_text(encoding="utf-8")
RUNNER = ROOT / "tools" / "run_simulation.py"


def selector_step() -> str:
    start = VENDOR.index("static int unlock_transaction_selector(")
    end = VENDOR.index("\n}\n", start)
    return VENDOR[start:end]


class ProfileMatrix(unittest.TestCase):
    @unittest.skipIf(os.environ.get("GU_SKIP_BUILD_TESTS") == "1",
                     "GU_SKIP_BUILD_TESTS=1")
    def test_every_profile_compiles_and_links(self):
        done = subprocess.run([sys.executable, str(RUNNER), "matrix"],
                              capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(done.returncode, 0,
                         done.stdout[-3000:] + done.stderr[-3000:])
        self.assertIn("SIM_MATRIX=PASS", done.stdout)
        for profile in ("default", "original", "observed", "preserve"):
            self.assertIn(f"SIM_MATRIX_PROFILE={profile} BUILD=PASS", done.stdout)


class SelectorContracts(unittest.TestCase):
    def test_the_step_is_one_function_with_one_branch_per_policy(self):
        step = selector_step()
        self.assertEqual(VENDOR.count("static int unlock_transaction_selector("), 1)
        self.assertIn("#ifdef GU_ENTRY_LATCHED_SS1_FULL", step)
        self.assertIn("GU_ENTRY_LATCHED_SS1_PRESERVE", step)
        # One SS0 write for every policy, before the branch.
        self.assertEqual(step.count('write_selector(work, "SS0"'), 1)
        self.assertLess(step.index('write_selector(work, "SS0"'),
                        step.index("#ifdef GU_ENTRY_LATCHED_SS1_FULL"))

    def test_every_policy_names_its_contract(self):
        step = selector_step()
        for marker in ("SELECTOR_PATH=PHASED", "SELECTOR_PATH=PRESERVE",
                       "SELECTOR_PATH=UNTRACKED"):
            self.assertIn(marker, step)
        # Each marker sits in the branch that earns it.
        phased = step.index("SELECTOR_PATH=PHASED")
        preserve = step.index("SELECTOR_PATH=PRESERVE")
        untracked = step.index("SELECTOR_PATH=UNTRACKED")
        self.assertLess(phased, preserve)
        self.assertLess(preserve, untracked)

    def test_preserve_does_not_write_the_selector_it_preserves(self):
        step = selector_step()
        preserve = step[step.index("#if defined(GU_ENTRY_LATCHED_SS1_PRESERVE)"):
                        step.index("#else", step.index("GU_ENTRY_LATCHED_SS1_PRESERVE"))]
        self.assertNotIn('write_selector(work, "SS1"', preserve)
        self.assertIn("untouched", preserve)

    def test_only_the_phased_policy_consumes_a_phase(self):
        step = selector_step()
        phased_start = step.index("#ifdef GU_ENTRY_LATCHED_SS1_FULL")
        phased_end = step.index("#else", phased_start)
        phased = step[phased_start:phased_end]
        self.assertEqual(phased.count("GuSelectorPhaseFor"), 3)
        self.assertNotIn("GuSelectorPhaseFor", step[phased_end:])

    def test_the_non_latched_branch_writes_both_selectors_and_admits_the_gap(self):
        step = selector_step()
        tail = step[step.index("SELECTOR_PATH=PRESERVE"):]
        self.assertIn('write_selector(work, "SS1"', tail)
        self.assertIn("no_entry_latch", tail)
        self.assertIn("A second write is not detected here", step)

    def test_the_checks_that_follow_the_write_are_policy_aware(self):
        # Both the pre-FLR and post-FLR comparisons, and the pre-close silicon
        # check, expect SS1 to match the profile's contract rather than a
        # constant - otherwise a preserve build faults after its reset.
        self.assertIn('ss1_contract = "preserved"', VENDOR)
        self.assertIn("contract=%s", VENDOR)
        self.assertIn("ss1_contract=\"preserved\"", MSI)
        self.assertIn("ss1_ok", MSI)
        for probe in ("pre-FLR selectors", "post-FLR SEC2 reset released"):
            index = VENDOR.index(probe)
            self.assertIn("expected", VENDOR[index:index + 400])

    def test_preserve_does_not_claim_to_detect_a_repeat(self):
        # The spend guard is full-policy only; a claim elsewhere would be false.
        port = (SRC / "port.c").read_text(encoding="utf-8")
        # The guard and the spend are adjacent statements under the same #ifdef.
        self.assertIn(
            "#ifdef GU_ENTRY_LATCHED_SS1_FULL\n"
            "  if(O==0x823820 && (V!=0x8U || owner->selector_phase!=GU_SELECTOR_PHASE_SS1_WRITE || owner->ss1_write_consumed)) {",
            port)
        # And the preserving policy refuses that offset outright instead.
        self.assertIn(
            "#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE\n"
            "  if(O==0x823820){owner->fault=TRUE;return;}", port)
        self.assertEqual(port.count("ss1_write_consumed=TRUE"), 1)


if __name__ == "__main__":
    unittest.main()
