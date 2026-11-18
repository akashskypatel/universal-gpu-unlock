"""Contract tests for the persisted boot record (A7, partial).

The record is the only state that survives a failed boot. These tests pin the
properties that make it safe to write at all:

  1. it is inert - no actuation decision may branch on it;
  2. it is written in two phases, intent before the first mutation and outcome
     after, so a power loss is visible on the next boot;
  3. a corrupt or short record is refused, never repaired or guessed at;
  4. a write failure is logged and never changes the transaction's result;
  5. both entrypoints (application and DXE driver) do the same thing.

Nothing here claims that re-applying the transaction is safe, and nothing claims
hardware evidence.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = ROOT / "tests"
APP = (SRC / "app.c").read_text(encoding="utf-8")
DXE = (SRC / "dxe_main.c").read_text(encoding="utf-8")
STATE = (SRC / "boot_state.c").read_text(encoding="utf-8")
HEADER = (SRC / "boot_state.h").read_text(encoding="utf-8")
BOOT_IO = (SRC / "boot_io.c").read_text(encoding="utf-8")
ENTRYPOINTS = (("app.c", APP), ("dxe_main.c", DXE), ("boot_io.c", BOOT_IO))


class RecordIsInert(unittest.TestCase):
    def test_nothing_branches_on_the_record(self):
        for name, text in ENTRYPOINTS:
            for line in text.splitlines():
                if "boot_state" not in line and "BOOT_STATE" not in line:
                    continue
                stripped = line.strip()
                # No control transfer may depend on the record: not an early
                # return, not a goto, not a break. It may only be logged.
                for forbidden in ("goto ", "return ", "break;", "continue;"):
                    self.assertNotIn(forbidden, stripped,
                                     "%s transfers control on the record: %s" % (name, stripped))
                # The one permitted condition is the warning about an
                # interrupted previous boot, and its body is a log call.
                if "if(" in stripped or "if (" in stripped:
                    self.assertIn("Phase", stripped,
                                  "%s branches on the record: %s" % (name, stripped))
                    self.assertIn("INTENT", stripped,
                                  "%s branches on the record: %s" % (name, stripped))
        self.assertIn("Nothing in the actuation path branches on", HEADER)

    def test_module_has_no_decision_surface(self):
        # No function in the module returns a policy answer.
        self.assertNotIn("EFI_BOOT_STATE_", STATE)
        for forbidden in ("GuPreflight", "GuActuationAdmitted", "GU_ACTUATION_ADMITTED"):
            self.assertNotIn(forbidden, STATE)


class TwoPhaseWrites(unittest.TestCase):
    def test_intent_precedes_the_sequence_and_outcome_follows(self):
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            intent = text.index("GU_BOOT_STATE_INTENT")
            outcome = text.index("GU_BOOT_STATE_OUTCOME")
            sequence = min(text.index("GuExecutePreparedSequence"),
                           text.index("GuExecuteUnlock") if "GuExecuteUnlock" in text else 10**9)
            self.assertLess(intent, sequence, name)
            self.assertLess(sequence, outcome, name)

    def test_record_is_read_inside_boot_open_and_before_discovery(self):
        # One read in GuBootOpen covers the application, the DXE driver and the
        # simulator, which never runs UefiMain.
        boot_open = BOOT_IO.split("EFI_STATUS GuBootOpen", 1)[1].split("EFI_STATUS GuBootLoadAssets", 1)[0]
        self.assertIn("GuBootStateRead(Table,&previous)", boot_open)
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            self.assertLess(text.index("GuBootStateRead"), text.index("GuDiscoverSingleGpu"), name)

    def test_write_failures_are_cast_to_void(self):
        # A record write is never allowed to alter the transaction result.
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            self.assertEqual(text.count("(VOID)GuBootStateRecord"), 2, name)

    def test_interrupted_boot_is_reported(self):
        self.assertIn("BOOT_STATE WARNING=previous boot stopped during a transaction", BOOT_IO)
        # A corrupt record is reported and ignored, never repaired during boot.
        self.assertIn("BOOT_STATE PRESENT=0 READ_STATUS=%#llx IGNORED=1", BOOT_IO)


class Validation(unittest.TestCase):
    def test_short_or_corrupt_records_are_refused(self):
        self.assertIn("if (size != sizeof(buffer)) return EFI_COMPROMISED_DATA;", STATE)
        self.assertIn("if (!Valid(&buffer)) return EFI_COMPROMISED_DATA;", STATE)
        self.assertIn("return s == EFI_NOT_FOUND ? EFI_SUCCESS : s", STATE)

    def test_host_unit_passes(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("A host C compiler is required")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_boot_state"
            build = subprocess.run(
                [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-DGU_BOOT_STATE_HOST_TEST",
                 "-fsanitize=undefined", "-fno-sanitize-recover=all",
                 "-I", str(SRC), "-I", str(TESTS),
                 str(TESTS / "test_boot_state.c"), str(SRC / "boot_state.c"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            done = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn("checks OK", done.stdout)


class BuildWiring(unittest.TestCase):
    def test_both_builders_and_the_simulator_compile_the_module(self):
        for name in ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1"):
            text = (SRC / name).read_text(encoding="utf-8")
            self.assertIn("'boot_state.c'", text, name)
        self.assertIn('"boot_state.c"', (ROOT / "tools" / "run_simulation.py").read_text(
            encoding="utf-8"))

    def test_attributes_survive_a_reboot(self):
        self.assertIn("EFI_VARIABLE_NON_VOLATILE", STATE)


if __name__ == "__main__":
    unittest.main()
