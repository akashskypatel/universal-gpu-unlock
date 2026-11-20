"""The donor go/no-go gate: it may only say GO when every precondition holds.

`tools/verify_run_log.py` scores one stage.  `tools/donor_gate.py` decides
whether the session may advance, so its failure modes are the ones that matter:
a later stage without its predecessors, a card that changed between stages, a
manual gate that was assumed instead of acknowledged, and a synthetic fixture
mistaken for a hardware run.  Each is exercised here, and the passing case must
still report `HARDWARE_VERIFIED=0`.
"""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "donor_gate.py"
FIXTURES = ROOT / "tests" / "fixtures" / "hardware_session"
MARKER = "SYNTHETIC_FIXTURE_NOT_A_HARDWARE_RUN"

ACKS = ",".join([
    "M1_RECOVERY_TOOLKIT_READY",
    "M2_DONOR_CARD_RECORDED",
    "M3_LOG_MEDIUM_EMPTY",
    "M4_ONE_CARD_ONLY",
    "M5_HALT_STOPS_THE_SESSION",
    "M6_STOCK_BOOT_AFTER_EVERY_RUN",
])


def run(*args: str, ack: str = ACKS) -> subprocess.CompletedProcess:
    command = [sys.executable, str(TOOL), "--self-test", *args]
    if ack is not None:
        command.extend(["--ack", ack])
    return subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)


def fixture(name: str) -> str:
    return str(FIXTURES / name)


class TheGateSaysGoOnlyWhenEverythingHolds(unittest.TestCase):
    def test_read_only_and_prepare_with_every_manual_gate_is_a_go(self):
        result = run("--readonly", fixture("readonly.txt"),
                     "--prepare", fixture("prepare.txt"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONOR_GATE VERDICT=GO", result.stdout)
        self.assertIn("DONOR_GATE identity BDF=01:00.0 DEVICE=2684", result.stdout)
        self.assertIn("DONOR_GATE manual ACKED=6 REQUIRED=6 RESULT=PASS", result.stdout)
        self.assertIn("NEXT=run the actuate stage", result.stdout)

    def test_a_scored_actuation_stage_advances_the_verdict_text(self):
        result = run("--readonly", fixture("readonly.txt"),
                     "--prepare", fixture("prepare.txt"),
                     "--actuate", fixture("actuate_boot1.txt"),
                     fixture("actuate_boot2.txt"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONOR_GATE actuate RESULT=PASS GATES=5", result.stdout)
        self.assertIn("NEXT=continue the session only as M6 permits", result.stdout)

    def test_the_go_verdict_never_claims_hardware_verification(self):
        result = run("--readonly", fixture("readonly.txt"),
                     "--prepare", fixture("prepare.txt"))
        self.assertNotIn("HARDWARE_VERIFIED=1", result.stdout)
        self.assertNotIn("VERIFIED=1 ", result.stdout)
        self.assertIn("HARDWARE_VERIFIED=0", result.stdout)

    def test_json_keeps_the_same_verdict_and_flag(self):
        result = run("--readonly", fixture("readonly.txt"),
                     "--prepare", fixture("prepare.txt"), "--json")
        payload = json.loads(result.stdout)
        self.assertEqual(payload["verdict"], "GO")
        self.assertIs(payload["hardware_verified"], False)
        self.assertEqual(payload["facts"]["bdf"], "01:00.0")
        self.assertEqual(payload["facts"]["blocked"], [])


class EveryPreconditionBlocksTheSession(unittest.TestCase):
    def test_an_unacknowledged_manual_gate_blocks(self):
        result = run("--readonly", fixture("readonly.txt"),
                     "--prepare", fixture("prepare.txt"),
                     ack="M1_RECOVERY_TOOLKIT_READY,M2_DONOR_CARD_RECORDED")
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED MANUAL_UNACKNOWLEDGED", result.stdout)
        self.assertIn("M6_STOCK_BOOT_AFTER_EVERY_RUN", result.stdout)
        self.assertIn("VERDICT=NO_GO", result.stdout)

    def test_a_misspelled_acknowledgement_is_refused_rather_than_ignored(self):
        result = run("--readonly", fixture("readonly.txt"),
                     "--prepare", fixture("prepare.txt"),
                     ack=ACKS + ",M7_I_AM_SURE")
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED UNKNOWN_ACK ids=M7_I_AM_SURE", result.stdout)

    def test_a_failing_read_only_gate_blocks(self):
        result = run("--readonly", fixture("truncated.txt"),
                     "--prepare", fixture("prepare.txt"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("DONOR_GATE readonly RESULT=FAIL", result.stdout)
        self.assertIn("R2_ENUMERATION_COMPLETED", result.stdout)
        self.assertIn("BLOCKED=GATE_READONLY", result.stdout)

    def test_a_card_that_moved_between_stages_blocks(self):
        text = (FIXTURES / "prepare.txt").read_text(encoding="utf-8")
        moved = text.replace("BDF=0000:01:00.0", "BDF=0000:02:00.0")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "prepare-moved.txt"
            path.write_text(moved, encoding="utf-8")
            result = run("--readonly", fixture("readonly.txt"), "--prepare", str(path))
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED IDENTITY_CHANGED", result.stdout)
        self.assertIn("RESULT=FAIL", result.stdout)

    def test_a_stage_that_names_no_pci_id_cannot_prove_identity(self):
        text = (FIXTURES / "prepare.txt").read_text(encoding="utf-8")
        stripped = text.replace("ID=10de:2684", "ID=unknown")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "prepare-anonymous.txt"
            path.write_text(stripped, encoding="utf-8")
            result = run("--readonly", fixture("readonly.txt"), "--prepare", str(path))
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED IDENTITY_UNKNOWN", result.stdout)

    def test_one_stage_naming_an_id_does_not_cover_for_another(self):
        """Every stage has to identify the card it ran on, not just one of them.

        The rule this tool prints says a stage that names no PCI id is refused
        rather than assumed, and a prepare log from a different card in the same
        slot would otherwise ride on the read-only log's identity.
        """
        text = (FIXTURES / "prepare.txt").read_text(encoding="utf-8")
        stripped = text.replace("ID=10de:2684", "ID=unknown")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "prepare-anonymous.txt"
            path.write_text(stripped, encoding="utf-8")
            result = run("--readonly", fixture("readonly.txt"), "--prepare", str(path))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("BLOCKED IDENTITY_UNKNOWN STAGE=prepare", result.stdout)
        self.assertIn("reason=this_stage_named_no_pci_id", result.stdout)

    def test_an_actuation_stage_identifies_its_card_too(self):
        """Every log in a stage counts: strip the identity from both boots."""
        with tempfile.TemporaryDirectory() as directory:
            anonymous = []
            for name in ("actuate_boot1.txt", "actuate_boot2.txt"):
                text = (FIXTURES / name).read_text(encoding="utf-8")
                stripped = "\n".join(row for row in text.splitlines()
                                     if not row.startswith("ACTIVE_GPU[")) + "\n"
                self.assertNotIn("ID=10de:2684", stripped)
                path = Path(directory) / ("anonymous-" + name)
                path.write_text(stripped, encoding="utf-8")
                anonymous.append(str(path))
            result = run("--readonly", fixture("readonly.txt"),
                         "--prepare", fixture("prepare.txt"),
                         "--actuate", *anonymous)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("BLOCKED IDENTITY_UNKNOWN STAGE=actuate", result.stdout)

    def test_two_cards_in_one_stage_are_not_a_single_donor(self):
        text = (FIXTURES / "prepare.txt").read_text(encoding="utf-8")
        extra = text + "ACTIVE_GPU[1] BDF=0000:02:00.0 ID=10de:2684 SUB=10de:a140\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "prepare-two-cards.txt"
            path.write_text(extra, encoding="utf-8")
            result = run("--readonly", fixture("readonly.txt"), "--prepare", str(path))
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED IDENTITY_NOT_SINGLE", result.stdout)

    def test_actuation_without_its_predecessors_blocks(self):
        result = run("--actuate", fixture("actuate_boot1.txt"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("BLOCKED PRIOR_STAGE_MISSING stage=readonly needed_by=actuate",
                      result.stdout)
        self.assertIn("BLOCKED PRIOR_STAGE_MISSING stage=prepare needed_by=actuate",
                      result.stdout)

    def test_a_synthetic_fixture_is_refused_without_the_self_test_switch(self):
        result = subprocess.run(
            [sys.executable, str(TOOL), "--ack", ACKS,
             "--readonly", fixture("readonly.txt"), "--prepare", fixture("prepare.txt")],
            capture_output=True, text=True, timeout=60, check=False,
        )
        self.assertEqual(result.returncode, 3)
        self.assertIn("BLOCKED SYNTHETIC_FIXTURE", result.stdout)
        self.assertIn("HARDWARE_VERIFIED=0", result.stdout)

    def test_a_missing_log_file_is_a_usage_error(self):
        result = run("--readonly", str(FIXTURES / "absent.txt"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("MISSING_LOG", result.stderr)

    def test_no_stage_at_all_is_a_usage_error(self):
        result = run()
        self.assertEqual(result.returncode, 2)


class RealLogsNeedNoSwitch(unittest.TestCase):
    def test_the_marker_is_the_only_difference(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for name in ("readonly.txt", "prepare.txt"):
                text = (FIXTURES / name).read_text(encoding="utf-8")
                text = "\n".join(line for line in text.splitlines()
                                 if MARKER not in line)
                path = Path(directory) / name
                path.write_text(text + "\n", encoding="utf-8")
                paths.append(str(path))
            result = subprocess.run(
                [sys.executable, str(TOOL), "--ack", ACKS,
                 "--readonly", paths[0], "--prepare", paths[1]],
                capture_output=True, text=True, timeout=60, check=False,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONOR_GATE VERDICT=GO NEXT=", result.stdout)
        self.assertIn("SYNTHETIC=0 HARDWARE_VERIFIED=0", result.stdout)


if __name__ == "__main__":
    unittest.main()
