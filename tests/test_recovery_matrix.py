"""The recovery matrix is a contract, not prose (S5 / A8 exit criteria).

A8's exit criteria ask for a "documented, tested recovery matrix (mid-transaction
power loss, Stop hang, FLR refusal, partial multi-GPU) with a software-visible
state that survives the failure". The software-visible state exists (A7's record,
plus the refusal and retention logs). The matrix did not.

`docs/recovery_matrix.json` is the source of truth and `docs/RECOVERY_MATRIX.md`
is rendered from it, so prose cannot drift from the manifest. These tests keep the
manifest honest:

  1. every symptom is a log line this source actually prints — the token must
     appear in the file the manifest names;
  2. every record field it cites exists in `GU_BOOT_STATE`;
  3. every `sim_scenario` names a row the simulator actually runs;
  4. no class claims to be verified: nothing here has been observed on hardware,
     and the one field that could say otherwise (`why_unverified`) must be there;
  5. the rendered document covers exactly the manifest's classes, with each
     class's steps and proof present.
"""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / "docs" / "recovery_matrix.json").read_text(encoding="utf-8"))
DOC = (ROOT / "docs" / "RECOVERY_MATRIX.md").read_text(encoding="utf-8")
BOOT_STATE = (ROOT / "src" / "boot_state.h").read_text(encoding="utf-8")
RUNNER = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")

# Classes A8 names explicitly, so the matrix cannot quietly drop one.
REQUIRED_CLASSES = {"power-loss-mid-transaction", "stop-hang",
                    "function-reset-refused", "partial-multi-target"}


def record_fields() -> set[str]:
    body = BOOT_STATE[BOOT_STATE.index("typedef struct {"):
                      BOOT_STATE.index("} GU_BOOT_STATE;")]
    fields = set()
    for line in body.splitlines():
        match = re.match(r"\s+(?:UINT\d+|UINT8)\s+(\w+)", line)
        if match and match.group(1) != "Magic":
            fields.add(match.group(1))
    return fields


def scenario_variants() -> set[str]:
    block = RUNNER[RUNNER.index("SCENARIOS = ("):RUNNER.index("\n)\n", RUNNER.index("SCENARIOS = ("))]
    return set(re.findall(r'\("(?:prepare|transaction)", "([\w-]+)"', block))


class TheMatrixIsGrounded(unittest.TestCase):
    def test_every_symptom_is_a_line_this_source_prints(self):
        for entry in MANIFEST["classes"]:
            for symptom in entry["symptoms"]:
                text = (ROOT / symptom["file"]).read_text(encoding="utf-8")
                self.assertIn(symptom["token"], text,
                              f"{entry['id']}: {symptom['token']!r} is not in "
                              f"{symptom['file']}")

    def test_every_record_field_exists(self):
        fields = record_fields()
        for entry in MANIFEST["classes"]:
            self.assertTrue(entry["record"], f"{entry['id']} cites no record state")
            for field in entry["record"]:
                self.assertIn(field, fields,
                              f"{entry['id']}: {field} is not a GU_BOOT_STATE field")

    def test_every_named_scenario_is_one_the_simulator_runs(self):
        variants = scenario_variants()
        for entry in MANIFEST["classes"]:
            scenario = entry["sim_scenario"]
            if scenario is None:
                continue
            variant = scenario.split(" ", 1)[1]
            self.assertIn(variant, variants,
                          f"{entry['id']}: {scenario!r} is not a simulator row")

    def test_the_classes_a8_names_are_all_present(self):
        ids = {entry["id"] for entry in MANIFEST["classes"]}
        self.assertTrue(REQUIRED_CLASSES <= ids,
                        f"missing: {sorted(REQUIRED_CLASSES - ids)}")

    def test_no_class_claims_recovery_is_verified(self):
        for entry in MANIFEST["classes"]:
            self.assertIs(entry["verified"], False,
                          f"{entry['id']} claims a verified recovery")
            self.assertTrue(entry["why_unverified"].strip(),
                            f"{entry['id']} does not say what is missing")
            self.assertTrue(entry["proof"], f"{entry['id']} has no proof criteria")
            self.assertTrue(entry["steps"], f"{entry['id']} has no steps")

    def test_the_manifest_is_self_consistent(self):
        ids = [entry["id"] for entry in MANIFEST["classes"]]
        self.assertEqual(len(ids), len(set(ids)), "duplicate class id")
        for entry in MANIFEST["classes"]:
            for key in ("title", "record_state", "card_state", "machine_state",
                        "steps", "proof", "symptoms"):
                self.assertIn(key, entry)
                self.assertTrue(entry[key], f"{entry['id']} has an empty {key}")


class TheDocumentMatchesTheManifest(unittest.TestCase):
    def test_the_document_is_exactly_the_rendered_manifest(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        import render_recovery_matrix as renderer
        self.assertEqual(DOC, renderer.render(MANIFEST),
                         "docs/RECOVERY_MATRIX.md is stale: run "
                         "python3 tools/render_recovery_matrix.py --write")

    def test_every_class_has_a_section_and_no_others(self):
        headings = set(re.findall(r"^### `([\w-]+)`", DOC, re.M))
        ids = {entry["id"] for entry in MANIFEST["classes"]}
        self.assertEqual(headings, ids,
                         "the rendered document and the manifest disagree")

    def test_each_section_carries_its_steps_and_proof(self):
        for entry in MANIFEST["classes"]:
            start = DOC.index(f"### `{entry['id']}`")
            end = DOC.find("\n### `", start + 1)
            section = DOC[start:] if end < 0 else DOC[start:end]
            for step in entry["steps"]:
                self.assertIn(step, section,
                              f"{entry['id']}: a manifest step is missing from the document")
            for proof in entry["proof"]:
                self.assertIn(proof, section,
                              f"{entry['id']}: a proof criterion is missing from the document")
            self.assertIn(entry["why_unverified"], section)
            for field in entry["record"]:
                self.assertIn(field, section,
                              f"{entry['id']}: record field {field} is not in the document")

    def test_the_document_says_what_it_is_not(self):
        flat = " ".join(DOC.split())
        self.assertIn("not a claim that any of this has been executed", flat)
        self.assertIn("CAN_UNLOCK=false", DOC)
        self.assertIn("RECOVERY_VERIFIED=false", DOC)


if __name__ == "__main__":
    unittest.main()
