"""The donor-hardware session is a set of gates, not a checklist in prose.

The donor card and the recovery toolkit are on the bench, so the operator needs
two things before the first boot: what a run may and may not do at each stage,
and a mechanical way to tell afterwards whether the run did it. This suite keeps
the second honest against the first:

  1. every gate token is a format string the file it names actually prints, so a
     gate cannot be satisfied by text no build of this tree writes;
  2. no token is so generic that an unrelated line could satisfy it;
  3. the stages, the ordering and the "needs two logs" gate are well formed;
  4. the verifier passes the fixtures that model a good run, fails the ones that
     model a truncated, mixed or simulated one, and refuses to score a synthetic
     log as hardware evidence at all;
  5. nothing here may claim hardware verification: no verdict may read
     `HARDWARE_VERIFIED=1`, and the rendered document is byte-identical to the
     manifest so the plan and the check cannot drift.
"""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import render_hardware_session as renderer  # noqa: E402
import verify_run_log  # noqa: E402

MANIFEST = json.loads((ROOT / "docs" / "hardware_session.json").read_text(encoding="utf-8"))
DOC = (ROOT / "docs" / "DONOR_HARDWARE_SESSION.md").read_text(encoding="utf-8")
FIXTURES = ROOT / "tests" / "fixtures" / "hardware_session"
VERIFIER = ROOT / "tools" / "verify_run_log.py"

STAGES = ("readonly", "prepare", "actuate")


def groups():
    """Every token-bearing group in the manifest, with the gate it belongs to."""
    for gate in MANIFEST["gates"]:
        for group in gate.get("requires", []):
            yield gate["id"], group
        for group in gate.get("requires_any", []):
            yield gate["id"], group
        for group in gate.get("forbids", []):
            yield gate["id"], group


def run_verifier(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(VERIFIER), *args],
                          capture_output=True, text=True, cwd=ROOT)


class TheGatesAreGrounded(unittest.TestCase):
    def test_every_token_is_a_format_string_its_emitter_prints(self):
        for gate_id, group in groups():
            text = (ROOT / group["emitter"]).read_text(encoding="utf-8")
            self.assertIn(group["token"], text,
                          f"{gate_id}: {group['token']!r} is not in {group['emitter']}")

    def test_every_advisory_token_is_printed_by_its_emitter(self):
        for item in MANIFEST["advisories"]:
            text = (ROOT / item["emitter"]).read_text(encoding="utf-8")
            for token in [item["token"]] + list(item.get("alternatives", [])):
                self.assertIn(token, text, f"{item['id']}: {token!r} is not in {item['emitter']}")

    def test_no_token_is_mostly_wildcards(self):
        for gate_id, group in groups():
            weight = verify_run_log.literal_weight(group["token"])
            self.assertGreaterEqual(
                weight, 12,
                f"{gate_id}: {group['token']!r} has only {weight} literal characters, "
                "so an unrelated line could satisfy it")

    def test_every_group_says_why(self):
        for gate_id, group in groups():
            self.assertTrue(group.get("why"), f"{gate_id}: a token has no reason recorded")

    def test_every_gate_says_what_it_does_not_prove(self):
        for gate in MANIFEST["gates"]:
            self.assertTrue(gate.get("proves"), f"{gate['id']} does not say what it proves")
            self.assertTrue(gate.get("does_not_prove"),
                            f"{gate['id']} does not say what it leaves open")

    def test_a_two_log_gate_says_so(self):
        for gate in MANIFEST["gates"]:
            if gate.get("min_logs", 1) > 1:
                self.assertGreaterEqual(gate["min_logs"], 2, gate["id"])


class TheStagesAreWellFormed(unittest.TestCase):
    def test_stages_are_the_expected_three_in_order(self):
        self.assertEqual(tuple(stage["id"] for stage in MANIFEST["stages"]), STAGES)
        self.assertEqual(tuple(verify_run_log.STAGE_ORDER), STAGES)

    def test_every_gate_belongs_to_a_stage_and_every_stage_has_gates(self):
        for gate in MANIFEST["gates"]:
            self.assertIn(gate["stage"], STAGES, gate["id"])
        for stage in STAGES:
            self.assertTrue([g for g in MANIFEST["gates"] if g["stage"] == stage], stage)

    def test_gate_ids_are_unique(self):
        ids = [gate["id"] for gate in MANIFEST["gates"]]
        self.assertEqual(len(ids), len(set(ids)))

    def test_gate_ids_are_prefixed_with_their_stage(self):
        prefix = {"readonly": "R", "prepare": "P", "actuate": "A"}
        for gate in MANIFEST["gates"]:
            self.assertTrue(gate["id"].startswith(prefix[gate["stage"]]), gate["id"])

    def test_the_read_stages_cannot_contain_a_write(self):
        for gate_id, group in groups():
            if gate_id.startswith(("R", "P")) and "forbids" and group["token"].startswith(
                    "SELECTED_GPU_STEP=ACTIVE"):
                self.assertTrue(gate_id.startswith("P"), gate_id)

    def test_every_manual_gate_records_something(self):
        for item in MANIFEST["manual_gates"]:
            self.assertTrue(item.get("record"), item["id"])
            self.assertTrue(item.get("why"), item["id"])


class ThePatternsMatchRealLines(unittest.TestCase):
    def test_format_conversions_become_the_shape_they_print(self):
        cases = {
            "ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u":
                "ACTIVE_GPU[0] BDF=0000:01:00.0",
            "STOP: stage=%s status=%#llx mutated=%u":
                "STOP: stage=transaction status=0x0 mutated=1",
            "PRESENT=%u MODE=100%% DONE":
                "PRESENT=3 MODE=100% DONE",
        }
        for token, line in cases.items():
            self.assertIsNotNone(verify_run_log.token_to_pattern(token).search(line), token)

    def test_a_pattern_does_not_match_a_different_line(self):
        pattern = verify_run_log.token_to_pattern("BOOT_STATE PRESENT=1 PHASE=%u")
        self.assertIsNone(pattern.search("BOOT_STATE PRESENT=0 PHASE=1"))
        # ... and the same token still matches a real one.
        self.assertIsNotNone(pattern.search("BOOT_STATE PRESENT=1 PHASE=2 BOOT_COUNT=3"))


class TheVerifierScoresRuns(unittest.TestCase):
    def test_a_good_read_only_run_passes_its_own_stage(self):
        result = run_verifier(str(FIXTURES / "readonly.txt"), "--self-test", "--stage", "readonly")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("RUN_LOG_VERDICT=PASS_SELF_TEST", result.stdout)
        self.assertIn("GATES_FAILED=0", result.stdout)

    def test_a_good_preparation_run_passes_its_own_stage(self):
        result = run_verifier(str(FIXTURES / "readonly.txt"), str(FIXTURES / "prepare.txt"),
                              "--self-test", "--stage", "prepare")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("GATE P1_PREPARATION_REFUSED_BEFORE_MUTATION STAGE=prepare RESULT=PASS",
                      result.stdout)

    def test_a_good_actuation_run_passes_its_own_stage(self):
        result = run_verifier(str(FIXTURES / "actuate_boot1.txt"),
                              str(FIXTURES / "actuate_boot2.txt"),
                              "--self-test", "--stage", "actuate")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("GATE A5_NEXT_BOOT_READ_THE_RECORD STAGE=actuate RESULT=PASS", result.stdout)

    def test_earlier_stages_are_never_reported_as_passes(self):
        result = run_verifier(str(FIXTURES / "actuate_boot1.txt"),
                              str(FIXTURES / "actuate_boot2.txt"),
                              "--self-test", "--stage", "actuate")
        self.assertNotIn("STAGE=readonly RESULT=PASS", result.stdout)
        self.assertIn("STAGE=readonly RESULT=PRIOR_STAGE", result.stdout)

    def test_a_truncated_log_fails_and_names_the_gate(self):
        result = run_verifier(str(FIXTURES / "truncated.txt"), "--self-test", "--stage", "readonly")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FIRST_FAILED=R2_ENUMERATION_COMPLETED", result.stdout)

    def test_an_actuation_run_without_a_following_boot_fails(self):
        result = run_verifier(str(FIXTURES / "actuate_boot1.txt"), "--self-test",
                              "--stage", "actuate")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("GATE A5_NEXT_BOOT_READ_THE_RECORD STAGE=actuate RESULT=FAIL", result.stdout)
        self.assertIn("TOO_FEW_LOGS", result.stdout)

    def test_a_simulated_log_cannot_pass_as_an_actuation_run(self):
        text = (FIXTURES / "actuate_boot1.txt").read_text(encoding="utf-8")
        simulated = text.replace(
            "ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=1 ORDERED_SEQUENTIAL=1 "
            "MUTATION_AUTHORIZED=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0 "
            "SIGNED_BOOTER_UNVERIFIED=1 WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION",
            "ACTIVE_EXECUTION=SIMULATED SELECTED=1 ORDERED_SEQUENTIAL=1 "
            "MUTATION_AUTHORIZED=1 ENVIRONMENT=HOST_SIMULATOR CAN_DETACH=0 CAN_UNLOCK=0 "
            "CAN_FLASH=0 RECOVERY_VERIFIED=0 SIGNED_BOOTER_UNVERIFIED=1")
        self.assertNotEqual(text, simulated)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "boot1.txt"
            path.write_text(simulated, encoding="utf-8")
            result = run_verifier(str(path), str(FIXTURES / "actuate_boot2.txt"),
                                  "--self-test", "--stage", "actuate")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("GATE A1_WAIVER_RECORDED_ONCE STAGE=actuate RESULT=FAIL", result.stdout)

    def test_the_waiver_may_appear_only_once_in_a_log(self):
        text = (FIXTURES / "actuate_boot1.txt").read_text(encoding="utf-8")
        waiver = [line for line in text.splitlines() if "WAIVED_UNVERIFIED" in line][0]
        doubled = text + waiver + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "boot1.txt"
            path.write_text(doubled, encoding="utf-8")
            result = run_verifier(str(path), str(FIXTURES / "actuate_boot2.txt"),
                                  "--self-test", "--stage", "actuate")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("TOO_OFTEN", result.stdout)

    def test_a_stop_in_a_preparation_session_fails_the_stage(self):
        text = (FIXTURES / "prepare.txt").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "prepare.txt"
            path.write_text(text + "STOP_CONTRACT device=2684 stage=1 OWNER=1\n", encoding="utf-8")
            result = run_verifier(str(path), "--self-test", "--stage", "prepare")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("FORBIDDEN", result.stdout)


class SyntheticLogsAreRefused(unittest.TestCase):
    def test_a_fixture_is_refused_without_the_self_test_flag(self):
        result = run_verifier(str(FIXTURES / "readonly.txt"), "--stage", "readonly")
        self.assertEqual(result.returncode, 3, result.stdout)
        self.assertIn("RUN_LOG_VERDICT=REFUSED REASON=SYNTHETIC_FIXTURE", result.stdout)

    def test_the_fixtures_all_carry_the_marker(self):
        for path in sorted(FIXTURES.glob("*.txt")):
            self.assertIn(verify_run_log.SYNTHETIC_MARKER, path.read_text(encoding="utf-8"), path.name)

    def test_a_self_test_verdict_can_never_read_pass(self):
        result = run_verifier(str(FIXTURES / "readonly.txt"), "--self-test", "--stage", "readonly")
        self.assertEqual(result.returncode, 0)
        self.assertIn("RUN_LOG_VERDICT=PASS_SELF_TEST", result.stdout)
        self.assertNotIn("RUN_LOG_VERDICT=PASS ", result.stdout)


class NothingClaimsHardwareVerification(unittest.TestCase):
    def test_no_manifest_field_claims_verification(self):
        text = json.dumps(MANIFEST)
        self.assertNotIn('"verified": true', text)
        self.assertNotIn('"hardware_verified": true', text)

    def test_no_verifier_output_can_print_hardware_verified_one(self):
        for stage, logs in (("readonly", ["readonly.txt"]),
                            ("prepare", ["readonly.txt", "prepare.txt"]),
                            ("actuate", ["actuate_boot1.txt", "actuate_boot2.txt"]),
                            ("actuate", ["truncated.txt"])):
            result = run_verifier(*[str(FIXTURES / name) for name in logs],
                                  "--self-test", "--stage", stage)
            self.assertIn("HARDWARE_VERIFIED=0", result.stdout, stage)
            self.assertNotIn("HARDWARE_VERIFIED=1", result.stdout + result.stderr, stage)

    def test_every_unprovable_fact_is_printed_on_every_run(self):
        result = run_verifier(str(FIXTURES / "readonly.txt"), "--self-test")
        for item in MANIFEST["never_established_by_a_log"]:
            self.assertIn(f"NOT_PROVEN {item['id']}", result.stdout)
            self.assertTrue(item["why"], item["id"])

    def test_every_manual_gate_is_printed_on_every_run(self):
        result = run_verifier(str(FIXTURES / "readonly.txt"), "--self-test")
        for item in MANIFEST["manual_gates"]:
            self.assertIn(f"MANUAL {item['id']}", result.stdout)

    def test_the_document_never_claims_a_verified_card(self):
        self.assertNotIn("HARDWARE_VERIFIED=1", DOC)
        self.assertIn("claims nothing", DOC)


class TheDocumentIsRendered(unittest.TestCase):
    def test_the_document_is_the_rendered_manifest(self):
        self.assertEqual(DOC, renderer.render(MANIFEST))

    def test_every_gate_and_manual_gate_is_in_the_document(self):
        for gate in MANIFEST["gates"]:
            self.assertIn(gate["id"], DOC)
        for item in MANIFEST["manual_gates"]:
            self.assertIn(item["id"], DOC)

    def test_the_verifier_can_list_the_plan(self):
        result = run_verifier("--list")
        self.assertEqual(result.returncode, 0, result.stdout)
        for gate in MANIFEST["gates"]:
            self.assertIn(gate["id"], result.stdout)

    def test_a_missing_log_is_a_usage_error_not_a_pass(self):
        result = run_verifier(str(FIXTURES / "does_not_exist.txt"), "--self-test")
        self.assertEqual(result.returncode, 2)
        self.assertIn("VERIFY_ERROR=MISSING_LOG", result.stderr)


if __name__ == "__main__":
    unittest.main()
