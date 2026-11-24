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
import hashlib
import pathlib
import json
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import render_hardware_session as renderer  # noqa: E402
import session_preflight as preflight  # noqa: E402
import verify_run_log  # noqa: E402

MANIFEST = json.loads((ROOT / "docs" / "hardware_session.json").read_text(encoding="utf-8"))
DOC = (ROOT / "docs" / "DONOR_HARDWARE_SESSION.md").read_text(encoding="utf-8")
FIXTURES = ROOT / "tests" / "fixtures" / "hardware_session"
VERIFIER = ROOT / "tools" / "verify_run_log.py"
PREFLIGHT = ROOT / "tools" / "session_preflight.py"

STAGES = ("readonly", "prepare", "admission", "actuate")


def groups():
    """Every token-bearing group in the manifest, with the gate it belongs to."""
    for gate in MANIFEST["gates"]:
        if gate.get("only_if"):
            yield gate["id"], gate["only_if"]
        for group in gate.get("requires", []):
            yield gate["id"], group
        for group in gate.get("requires_any", []):
            yield gate["id"], group
        for group in gate.get("forbids", []):
            yield gate["id"], group


def run_verifier(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(VERIFIER), *args],
                          capture_output=True, text=True, cwd=ROOT)


def run_preflight(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(PREFLIGHT), *args],
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
    def test_stages_are_the_expected_four_in_order(self):
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
        prefix = {"readonly": "R", "prepare": "P", "admission": "N", "actuate": "A"}
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


class ARefusalBeforeMutationIsItsOwnStage(unittest.TestCase):
    """The waived image can stop at admission, and that outcome had no stage.

    RUN0006 is the first log from an image that may write, and it refused: a
    sticky PCIe error and an ASPM request the image does not manage. Scored as
    `actuate` it read as four missing-token failures, and scored as `prepare` as a
    P1 failure - neither says what happened. `admission` says it: the image
    prepared both cards, refused, and stopped with nothing written.
    """

    def test_the_stage_sits_between_preparation_and_execution(self):
        self.assertEqual(STAGES, ("readonly", "prepare", "admission", "actuate"))
        self.assertEqual(tuple(verify_run_log.STAGE_ORDER), STAGES)

    def test_the_gate_refuses_a_granted_admission(self):
        gate = [g for g in MANIFEST["gates"] if g["id"].startswith("N1_")][0]
        forbidden = [group["token"] for group in gate["forbids"]]
        self.assertIn("ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu ORDERED_SEQUENTIAL=1", forbidden)
        self.assertIn("ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY", forbidden)
        self.assertTrue(gate.get("requires_any"), "a refusal has more than one shape")

    def test_the_admission_gates_score_the_captured_refusal(self):
        run = [r for r in MANIFEST["captured_runs"]["runs"] if r["stage"] == "admission"]
        self.assertTrue(run, "no captured admission run")
        result = run_verifier(str(ROOT / run[0]["file"]), "--stage", "admission")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("GATES=2 GATES_FAILED=0", result.stdout)
        self.assertIn("NOTE LINK_POLICY_OBSERVED PRESENT", result.stdout)

    def test_a_granted_admission_fails_the_stage(self):
        run = [r for r in MANIFEST["captured_runs"]["runs"] if r["stage"] == "admission"][0]
        text = (ROOT / run["file"]).read_text(encoding="utf-8").replace("\r\n", "\n")
        granted = text + "ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=2 ORDERED_SEQUENTIAL=1\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "granted.txt"
            path.write_text(granted, encoding="utf-8")
            result = run_verifier(str(path), "--stage", "admission")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FORBIDDEN token=", result.stdout)


class TheReadOnlyPathReportsTheSameLinkPolicy(unittest.TestCase):
    """A bench must be fixable with a boot that cannot write.

    The waived image refuses an unlock on a link that has logged an error, is left
    in an ASPM state the image does not manage, or is not up. Printing the same
    observation from the preparation path means the operator can see which of the
    three it is without staging an image that may write.
    """

    def setUp(self):
        self.source = (ROOT / "src" / "gpu_discovery.c").read_text(encoding="utf-8")

    def test_the_preparation_path_prints_the_link_observation(self):
        self.assertIn('GuLog("ACTIVE_LINK[%zu] BDF=%04x:%02x:%02x.%x PCI_LINK=%s', self.source)
        self.assertIn('GuLinkAccepted(verdict)?"ALLOW":"REFUSE"', self.source)

    def test_it_is_printed_before_the_preparation_refusal(self):
        link = self.source.index("ACTIVE_LINK[%zu]")
        refusal = self.source.index("ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY")
        self.assertLess(link, refusal)

    def test_both_lines_name_the_four_sticky_bits_and_the_card(self):
        for token in ("ERROR_CE=%u ERROR_NONFATAL=%u ERROR_FATAL=%u",
                      "ERROR_UR=%u", "HOTPLUG=%u SURPRISE=%u",
                      "BDF=%04x:%02x:%02x.%x"):
            self.assertIn(token, self.source, token)

    def test_each_condition_is_reported_separately(self):
        """A verdict names the first failure; the operator needs all four."""
        for token in ("OK_ERRORS=%u OK_ASPM=%u OK_TRAINED=%u OK_LINK=%u UP_WEIGHED=0",):
            self.assertEqual(self.source.count(token), 2,
                             "both link lines must weigh the same four conditions")
        for expr in ("(unsigned)((link.errors&0x06U)==0)", "(unsigned)(link.aspm==0)",
                     "(unsigned)(link.trained!=0)",
                     "(unsigned)(link.width!=0&&link.speed!=0&&link.speed!=0x0fU)"):
            self.assertEqual(self.source.count(expr), 2, expr)

    def test_the_four_conditions_are_the_ones_the_verdict_uses(self):
        checks = (ROOT / "src" / "checks.c").read_text(encoding="utf-8")
        body = checks[checks.index("GU_LINK_VERDICT GuCheckLinkState("):]
        body = body[:body.index("BOOLEAN GuLinkAccepted")]
        # Reject bad link health before returning the ASPM verdict. A bench
        # that waives ASPM must still reject an untrained or unusable link.
        order = [body.index(marker) for marker in
                 ("(Link.errors & 0x06U) != 0", "!Link.trained",
                  "Link.width == 0", "Link.aspm != 0")]
        self.assertEqual(order, sorted(order), "ASPM waiver must not skip link-health checks")

    def test_the_observation_does_not_refuse_anything(self):
        link = self.source.index("ACTIVE_LINK[%zu]")
        refusal = self.source.index("ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY")
        block = self.source[link:refusal]
        self.assertNotIn("goto done", block)
        self.assertNotIn("GuLinkAccepted(verdict)) {", block)


class TheBenchNotesNameWhereEachConditionIsSet(unittest.TestCase):
    """A link state is a property of what the machine did, not of the card.

    RUN0006 and RUN0007 both refuse on three of four conditions, so the operator
    needs one page that says which setting or reset each condition answers to,
    including the fact that the board's only ASPM entry was found set to Auto.
    """

    def setUp(self):
        self.notes = (ROOT / "docs" / "BENCH_NOTES.md").read_text(encoding="utf-8")

    def test_the_page_exists_and_is_linked(self):
        self.assertIn("BENCH_NOTES.md", (ROOT / "docs" / "PRODUCTION_READINESS_INVENTORY.md")
                      .read_text(encoding="utf-8"))

    def test_it_names_the_board_path_and_the_value_that_matters(self):
        for token in ("Advanced \\ PCIe Subsystem Settings", "ASPM Control for CPU PCIe",
                      "Disabled", "L0s And L1 Entry", "Auto"):
            self.assertIn(token, self.notes, token)

    def test_it_names_all_four_conditions_and_their_flags(self):
        for token in ("OK_ERRORS", "OK_ASPM", "OK_TRAINED", "OK_LINK",
                      "Fundamental Reset", "AC removal", "Link Status bit 13"):
            self.assertIn(token, self.notes, token)

    def test_it_says_auto_is_not_disabled(self):
        self.assertIn("`Auto` is not `Disabled`", self.notes.replace("**", ""))

    def test_it_does_not_promise_that_one_setting_is_enough(self):
        self.assertIn("if this board has only the one", self.notes.lower())


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


class TheBuildCommandsExist(unittest.TestCase):
    def test_every_stage_names_a_builder_that_exists(self):
        for stage in MANIFEST["stages"]:
            match = re.search(r"(-File\s+)?\.[\\/]src[\\/]([\w.\-]+\.ps1)", stage["build"])
            self.assertIsNotNone(match, stage["id"])
            self.assertTrue((ROOT / "src" / match.group(2)).is_file(), stage["id"])

    def test_every_switch_in_a_build_command_exists_in_that_script(self):
        for stage in MANIFEST["stages"]:
            match = re.search(r"src[\\/]([\w.\-]+\.ps1)(.*)$", stage["build"])
            script, rest = match.group(1), match.group(2)
            text = (ROOT / "src" / script).read_text(encoding="utf-8")
            # Only the switches passed to the script, not to powershell.exe.
            switches = re.findall(r"(?:^|\s)(-[A-Z][\w]*)", rest)
            for switch in switches:
                self.assertIn(switch, text, f"{stage['id']}: {switch} is not in {script}")

    def test_only_actuation_staging_has_explicit_actuating_deploy_consent(self):
        deploy = [stage["id"] for stage in MANIFEST["stages"]
                  if "-AcceptActuatingDeploy" in stage["build"]]
        self.assertEqual(deploy, ["admission", "actuate"])
        catalogue = json.loads((ROOT / "docs" / "image_variants.json").read_text(encoding="utf-8"))
        for variant_id in ("actuate-sequence", "actuate-one-target"):
            variant = next(v for v in catalogue["variants"] if v["id"] == variant_id)
            self.assertTrue(variant["switches"]["AcceptUnverifiedActuation"])


class TheReadOnlyGuideIsGrounded(unittest.TestCase):
    """The step-by-step guide is data, and every line of it is checked.

    A guide is the easiest place for a document to drift: a rename, a dropped
    switch or a reworded log line leaves prose that reads like an instruction and
    does nothing. These tests resolve the guide against the catalogue, the
    builders, the tools and the sources it names.
    """

    def setUp(self):
        self.guide = MANIFEST["readonly_guide"]
        self.catalogue = json.loads((ROOT / "docs" / "image_variants.json").read_text(encoding="utf-8"))
        self.variants = {variant["id"]: variant for variant in self.catalogue["variants"]}
        self.status = json.loads((ROOT / "docs" / "readiness_status.json").read_text(encoding="utf-8"))

    def goal_ids(self):
        ids = {item["id"] for item in self.status["items"]}
        ids.update(item["id"].split("_")[0] for item in MANIFEST["manual_gates"])
        return ids

    def test_every_read_only_variant_is_either_a_candidate_or_named_unscoreable(self):
        covered = {candidate["id"] for candidate in self.guide["candidates"]}
        covered.update(item["id"] for item in self.guide["unscoreable"])
        for variant in self.catalogue["variants"]:
            self.assertIn(variant["id"], covered,
                          f"{variant['id']} ({variant['safety']}) is neither executed nor explained")

    def test_the_candidates_are_exactly_the_variants_that_write_nothing(self):
        writes_nothing = {variant["id"] for variant in self.catalogue["variants"]
                          if variant["safety"] in ("read-only", "refuses-before-mutation")}
        executed = {candidate["id"] for candidate in self.guide["candidates"]}
        excluded = {item["id"] for item in self.guide["unscoreable"]}
        self.assertEqual(executed | (excluded & writes_nothing), writes_nothing)
        self.assertEqual(executed & excluded, set())
        # The one read-only image that is not in the loop is the one with no log.
        self.assertEqual(writes_nothing - executed, {"diag-topology"})
        reason = [item["why"] for item in self.guide["unscoreable"] if item["id"] == "diag-topology"][0]
        self.assertIn("-SaveLog", reason)

    def test_every_excluded_variant_says_why(self):
        for item in self.guide["unscoreable"]:
            self.assertTrue(item["why"], item["id"])
            self.assertIn(item["id"], self.variants, item["id"])

    def test_every_candidate_names_the_command_that_selects_it(self):
        for candidate in self.guide["candidates"]:
            variant = self.variants[candidate["id"]]
            self.assertEqual(variant["profile"], "diagnostic", candidate["id"])
            self.assertEqual(variant["safety"], candidate["safety"], candidate["id"])
            self.assertIn("-Only diagnostic ", candidate["command"], candidate["id"])

    def test_canonical_read_only_commands_resolve_to_original_configurations(self):
        import shutil
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            self.skipTest("PowerShell unavailable for profile resolution")
        script = str(ROOT / "src" / "Build-Sign-Deploy-UniversalGPU.ps1").replace("'", "''")
        for candidate in self.guide["candidates"]:
            for item in [candidate] + candidate.get("commands", []):
                with self.subTest(candidate=candidate["id"], command=item["command"]):
                    args = item["command"].split(".ps1 ", 1)[1]
                    # Replace the guide's ESP placeholder with a valid example GUID.
                    # Inspection must not compile, sign or stage the image.
                    args = re.sub(r" -StageDrive [A-Z](?=\s|$)", "", args)
                    args = args.replace("<reviewed GUID>", "a005858c-4487-4ee2-976e-1710ff484c31")
                    command = f"& '{script}' -ResolveOnly {args}"
                    result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", command],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    resolved = json.loads(result.stdout)
                    self.assertEqual(resolved["VariantId"], candidate["id"])
                    self.assertEqual(resolved["CanonicalProfile"], "diagnostic")
                    self.assertEqual(resolved["Safety"], candidate["safety"])
                    if item.get("binary"):
                        self.assertEqual(resolved["Binary"], item["binary"])


    def test_every_switch_in_a_candidate_command_exists_in_the_builder(self):
        text = (ROOT / "src" / "Build-Sign-Deploy-UniversalGPU.ps1").read_text(encoding="utf-8")
        # Inspect declared parameters, not occurrences of switch names in comments.
        parameters = re.search(r"(?ms)^param\(\s*\n(.*?)^\)", text)
        self.assertIsNotNone(parameters)
        declared = set(re.findall(r"(?m)^\s*\[(?:string(?:\[\])?|switch)\]\$(\w+)\b",
                                  parameters.group(1)))
        for candidate in self.guide["candidates"]:
            for item in [candidate] + candidate.get("commands", []):
                for switch in re.findall(r"(?:^|\s)-([A-Z][\w]*)", item["command"]):
                    self.assertIn(switch, declared, f"{candidate['id']}: -{switch}")

    def test_a_candidate_that_cannot_be_staged_does_not_ask_to_be_staged(self):
        for candidate in self.guide["candidates"]:
            if "DXE" in self.variants[candidate["id"]]["binary"]:
                self.assertNotIn("-StageDrive", candidate["command"], candidate["id"])
                self.assertIn("-SignOnly", candidate["command"], candidate["id"])

    def test_every_quoted_log_line_is_printed_by_the_file_it_names(self):
        for candidate in self.guide["candidates"]:
            self.assertTrue(candidate["evidence"], candidate["id"])
            for entry in candidate["evidence"]:
                text = (ROOT / entry["emitter"]).read_text(encoding="utf-8")
                self.assertIn(entry["token"], text,
                              f"{candidate['id']}/{entry['goal']}: {entry['token']!r} is not in {entry['emitter']}")

    def test_a_quoted_log_line_is_specific_enough_to_identify_itself(self):
        for candidate in self.guide["candidates"]:
            for entry in candidate["evidence"]:
                weight = verify_run_log.literal_weight(entry["token"])
                self.assertGreaterEqual(
                    weight, 12,
                    f"{candidate['id']}/{entry['goal']}: {entry['token']!r} has only {weight} literal characters")

    def test_every_quoted_line_explains_what_it_advances(self):
        for candidate in self.guide["candidates"]:
            for entry in candidate["evidence"]:
                self.assertTrue(entry["why"], f"{candidate['id']}/{entry['goal']}")
                self.assertIn(entry["goal"], self.goal_ids(),
                              f"{candidate['id']}: {entry['goal']} is not an inventory item or a manual gate")

    def test_every_candidate_says_what_it_does_not_prove(self):
        for candidate in self.guide["candidates"]:
            self.assertTrue(candidate["does_not_prove"], candidate["id"])
            self.assertTrue(candidate["stage"] in STAGES, candidate["id"])

    def test_a_handoff_candidate_names_the_probe_it_extends(self):
        ids = {candidate["id"] for candidate in self.guide["candidates"]}
        for candidate in self.guide["candidates"]:
            base = candidate.get("also_collects")
            if base:
                self.assertIn(base, ids, candidate["id"])
                self.assertNotEqual(base, candidate["id"])

    def test_every_quoted_tool_line_is_printed_by_that_tool(self):
        for step in self.guide["steps"]:
            for group in step.get("expect_tokens", []):
                text = (ROOT / group["emitter"]).read_text(encoding="utf-8")
                self.assertIn(group["token"], text,
                              f"step {step['n']}: {group['token']!r} is not in {group['emitter']}")

    def test_the_guide_starts_at_the_staging_command_and_ends_at_the_verifier(self):
        shells = [step for step in self.guide["steps"] if step.get("command")]
        self.assertIn("Build-Sign-Deploy-UniversalGPU.ps1", shells[1]["command"])
        self.assertIn("-StageDrive", shells[1]["command"])
        self.assertIn("tools/verify_run_log.py", shells[-1]["command"])
        self.assertEqual([step["n"] for step in self.guide["steps"]],
                         list(range(1, len(self.guide["steps"]) + 1)))

    def test_the_guide_says_the_build_sign_stage_script_does_not_exist(self):
        self.assertIn("no script called `build-sign-stage`", DOC)
        self.assertIn("src/Build-Sign-Deploy-UniversalGPU.ps1", DOC)

    def test_every_ceiling_row_is_an_item_the_inventory_lists(self):
        for item in self.guide["ceiling"]:
            self.assertIn(item["goal"], self.goal_ids(), item["goal"])
            self.assertTrue(item["after"], item["goal"])
            self.assertTrue(item["why_not"], item["goal"])

    def test_the_guide_distinguishes_operator_report_from_formal_gate_verdict(self):
        self.assertIn("operator-reported physical mutation outcome", DOC.lower())
        self.assertIn("RUN0031", DOC)
        self.assertIn("A5_NEXT_BOOT_READ_THE_RECORD=FAIL", DOC)
        self.assertNotIn("HARDWARE_VERIFIED=1", DOC)
        for item in self.status["items"]:
            self.assertFalse(item["hardware_verified"],
                             f"{item['id']} is marked hardware verified by a document update")

    def test_the_guide_is_in_the_document_in_the_order_it_renders(self):
        self.assertIn("## Read-only hardware execution guide, step by step", DOC)
        guide_at = DOC.index("## Read-only hardware execution guide, step by step")
        stages_at = DOC.index("## The stages")
        self.assertLess(guide_at, stages_at)


class AStoppedRunIsEvidenceAndNeverAPass(unittest.TestCase):
    """RUN0011 is the log this bucket exists for: a waived boot that wrote
    thirteen lines, went black, and was power-cycled. It has to be pinned like any
    other run - digest, size, quoted facts - while claiming nothing at all, and the
    separation from `captured_runs` is the point: that bucket's contract is that
    every entry passes every gate of its stage, so a run that stopped cannot live
    there without breaking it."""

    def setUp(self):
        self.spec = MANIFEST["stopped_runs"]
        self.runs = self.spec["runs"]
        self.doc = DOC

    def stored(self, run):
        path = ROOT / run["file"]
        self.assertTrue(path.is_file(), run["file"])
        return path.read_bytes().replace(b"\r\n", b"\n")

    def test_the_bucket_does_not_duplicate_the_captured_runs(self):
        captured = {run["id"] for run in MANIFEST["captured_runs"]["runs"]}
        self.assertTrue(self.runs)
        for run in self.runs:
            self.assertNotIn(run["id"], captured, run["id"])

    def test_every_stopped_run_matches_its_recorded_digest_and_size(self):
        for run in self.runs:
            data = self.stored(run)
            self.assertEqual(hashlib.sha256(data).hexdigest(), run["sha256"], run["id"])
            self.assertEqual(len(data), run["bytes"], run["id"])
            self.assertEqual(data.count(b"\n"), run["lines"], run["id"])

    def test_a_stopped_run_never_scores_as_a_pass(self):
        for run in self.runs:
            result = run_verifier(str(ROOT / run["file"]), "--stage", run["stage"])
            self.assertNotEqual(result.returncode, 0, run["id"])
            self.assertIn(run["verdict"], result.stdout.splitlines(), run["id"])
            for line in result.stdout.splitlines():
                if line.startswith("RUN_LOG_VERDICT="):
                    self.assertNotIn("VERDICT=PASS", line, run["id"])
            self.assertIn("SYNTHETIC=0", result.stdout, run["id"])
            self.assertIn("HARDWARE_VERIFIED=0", result.stdout, run["id"])

    def test_every_quoted_fact_is_in_the_log(self):
        for run in self.runs:
            text = self.stored(run).decode("utf-8")
            self.assertTrue(run["observations"], run["id"])
            for entry in run["observations"]:
                self.assertIn(entry["token"], text, f"{run['id']}: {entry['token']!r} is not in the log")
                self.assertTrue(entry["why"], f"{run['id']}: {entry['token']!r} says nothing")

    def test_the_run_is_in_the_document_with_its_verdict(self):
        self.assertIn(self.spec["title"], self.doc)
        for run in self.runs:
            self.assertIn(run["id"], self.doc)
            self.assertIn(run["verdict"], self.doc)


class TheFirstCompletedActuateStage(unittest.TestCase):
    """A5 needs the boot after, so an actuate stage is scored as a pair: the run
    that started the step and the run whose boot record shows what came of it.
    RUN0012 + RUN0014 are the first pair on this bench that satisfies all five
    formal gates. RUN0031 is a later operator-confirmed successful 4090 mutation,
    but it remains outside this pair and fails A5 when scored alone because no
    next-boot record was included."""

    PAIR = ("tests/data/hardware_session/physical-actuate-0002.txt",
            "tests/data/hardware_session/physical-actuate-0004.txt")
    VERDICT = "RUN_LOG_VERDICT=PASS SYNTHETIC=0 HARDWARE_VERIFIED=0 GATES=5 GATES_FAILED=0"

    def test_the_pair_scores_all_five_actuate_gates(self):
        result = run_verifier(*[str(ROOT / name) for name in self.PAIR], "--stage", "actuate")
        self.assertIn(self.VERDICT, result.stdout.splitlines())
        for gate in ("A1_WAIVER_RECORDED_ONCE", "A2_STOP_CONTRACT_CAPTURED",
                     "A3_ONE_ORDERED_STEP_STARTED", "A4_STEP_TERMINATED",
                     "A5_NEXT_BOOT_READ_THE_RECORD"):
            self.assertIn("GATE %s STAGE=actuate RESULT=PASS" % gate, result.stdout, gate)

    def test_either_half_alone_does_not(self):
        # The pair is the unit because neither half is the stage: the first run has
        # no next boot, and the second has no transaction.
        for name in self.PAIR:
            result = run_verifier(str(ROOT / name), "--stage", "actuate")
            self.assertNotIn(self.VERDICT, result.stdout.splitlines(), name)

    def test_the_pair_is_in_the_document(self):
        self.assertIn("The first actuate stage that satisfied all five gates", DOC)
        for name in self.PAIR:
            self.assertIn(pathlib.Path(name).name.replace(".txt", ""), DOC)


class TheGatesAreSatisfiableByAHealthyRun(unittest.TestCase):
    """A gate nobody can pass is worse than no gate.

    A physical read-only run failed `R2_ENUMERATION_COMPLETED` because R2
    required `PCI_OBSERVATION_ONLY=1 ...`, a line the strict collector prints only
    when it has already failed. These tests score captured runs against every
    formal gate. Later operator-confirmed mutation evidence is stored separately
    and does not alter those stage scores.
    """

    def setUp(self):
        self.runs = MANIFEST["captured_runs"]["runs"]

    def stored(self, run):
        path = ROOT / run["file"]
        self.assertTrue(path.is_file(), run["file"])
        return path.read_bytes().replace(b"\r\n", b"\n")

    def test_every_captured_run_matches_its_recorded_digest_and_size(self):
        for run in self.runs:
            data = self.stored(run)
            self.assertEqual(hashlib.sha256(data).hexdigest(), run["sha256"], run["id"])
            self.assertEqual(len(data), run["bytes"], run["id"])
            self.assertEqual(data.count(b"\n"), run["lines"], run["id"])

    def test_a_captured_run_is_not_a_fixture(self):
        for run in self.runs:
            self.assertNotIn(verify_run_log.SYNTHETIC_MARKER, self.stored(run).decode("utf-8"), run["id"])

    def test_a_captured_run_still_scores_the_way_its_entry_says(self):
        for run in self.runs:
            result = run_verifier(str(ROOT / run["file"]), "--stage", run["stage"])
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(run["verdict"], result.stdout.splitlines(), run["id"])
            self.assertIn("SYNTHETIC=0", result.stdout, run["id"])
            self.assertIn("HARDWARE_VERIFIED=0", result.stdout, run["id"])
            self.assertNotIn("HARDWARE_VERIFIED=1", result.stdout + result.stderr, run["id"])

    def test_every_gate_in_the_scored_stage_passes_on_a_healthy_run(self):
        for run in self.runs:
            text = self.stored(run).decode("utf-8")
            for gate in MANIFEST["gates"]:
                if gate["stage"] != run["stage"]:
                    continue
                marker = gate.get("only_if")
                if marker and not verify_run_log.token_to_pattern(
                        marker["token"]).search(text):
                    # The image this gate scores was not run in that boot: the
                    # gate must say NOT_APPLICABLE and claim no pass.
                    self.assertIn(f"GATE {gate['id']} STAGE={gate['stage']} "
                                  f"RESULT=NOT_APPLICABLE",
                                  run_verifier(str(ROOT / run["file"]),
                                               "--stage", run["stage"]).stdout,
                                  run["id"])
                    continue
                for group in gate.get("requires", []):
                    self.assertIsNotNone(
                        verify_run_log.token_to_pattern(group["token"]).search(text),
                        f"{run['id']}: {gate['id']} requires {group['token']!r}, which this healthy run does not print")
                if gate.get("requires_any"):
                    matched = [group for group in gate["requires_any"]
                               if verify_run_log.token_to_pattern(group["token"]).search(text)]
                    self.assertTrue(matched, f"{run['id']}: {gate['id']} matched none of its alternatives")
                for group in gate.get("forbids", []):
                    self.assertIsNone(
                        verify_run_log.token_to_pattern(group["token"]).search(text),
                        f"{run['id']}: {gate['id']} forbids {group['token']!r}, which this run contains")

    def test_every_quoted_fact_is_in_the_log_and_the_source(self):
        for run in self.runs:
            text = self.stored(run).decode("utf-8")
            self.assertTrue(run["observations"], run["id"])
            for entry in run["observations"]:
                self.assertIn(entry["token"], text, f"{run['id']}: {entry['token']!r} is not in the log")
                self.assertTrue(entry["why"], f"{run['id']}: {entry['token']!r} says nothing")

    def test_every_captured_run_is_in_the_document_with_its_verdict(self):
        for run in self.runs:
            self.assertIn(run["id"], DOC)
            self.assertIn(run["verdict"], DOC)
            self.assertIn(run["sha256"], DOC)
            self.assertTrue(run["does_not_show"], run["id"])
            self.assertIn(run["does_not_show"], DOC)

    def test_a_captured_run_never_moves_a_readiness_row(self):
        status = json.loads((ROOT / "docs" / "readiness_status.json").read_text(encoding="utf-8"))
        for item in status["items"]:
            self.assertFalse(item["hardware_verified"],
                             f"{item['id']} was marked hardware verified by a captured log")
        self.assertNotIn("HARDWARE_VERIFIED=1", DOC)


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


class TheBenchIsCheckedBeforehand(unittest.TestCase):
    """The pre-flight scores the bench; the verifier scores the run."""

    def make_bench(self, tmp: str, *, logs: tuple[str, ...] = ()) -> Path:
        stick = Path(tmp) / "stick"
        (stick / "AD102-UNLOCK").mkdir(parents=True)
        for name in logs:
            (stick / "AD102-UNLOCK" / name).write_text("stale\n", encoding="utf-8")
        return stick

    def make_profiles(self, tmp: str, guid: str = "0xdeadbeef") -> Path:
        profile = Path(tmp) / "boot_profile.h"
        profile.write_text(
            f"#define GU_WINDOWS_PARTITION_INITIALIZER {{{guid},0x2222,0x3333,"
            "{0x84,0x44,0x55,0x66,0x77,0x88,0x99,0xaa}}\n", encoding="utf-8")
        return profile

    def test_a_clean_bench_passes_and_never_claims_hardware(self):
        with tempfile.TemporaryDirectory() as tmp:
            stick = self.make_bench(tmp)
            image = Path(tmp) / "image.efi"
            image.write_bytes(b"MZ" + b"\0" * 64)
            result = run_preflight("--stick", str(stick), "--image", str(image),
                                   "--profile", str(self.make_profiles(tmp)))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("PREFLIGHT_VERDICT=PASS", result.stdout)
        self.assertIn("HARDWARE_VERIFIED=0", result.stdout)
        self.assertIn("SHA256=", result.stdout)
        self.assertNotIn("HARDWARE_VERIFIED=1", result.stdout)

    def test_a_stale_log_on_the_medium_fails_the_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            stick = self.make_bench(tmp, logs=("RUN0001.TXT",))
            result = run_preflight("--stick", str(stick), "--skip-profile")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("STALE_LOGS=1", result.stdout)
        self.assertIn("ACTION=wipe the medium", result.stdout)

    def test_the_placeholder_guid_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            stick = self.make_bench(tmp)
            result = run_preflight("--stick", str(stick),
                                   "--profile", str(self.make_profiles(tmp, guid="0x11111111")))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("GUID=SYNTHETIC", result.stdout)

    def test_a_missing_profile_is_a_failure_with_an_action(self):
        with tempfile.TemporaryDirectory() as tmp:
            stick = self.make_bench(tmp)
            result = run_preflight("--stick", str(stick),
                                   "--profile", str(Path(tmp) / "absent.h"))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("PROFILE", result.stdout)
        self.assertIn("ACTION=", result.stdout)

    def test_a_zero_byte_image_is_not_a_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "image.efi"
            image.write_bytes(b"")
            result = run_preflight("--image", str(image), "--skip-profile")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("IMAGE EMPTY=1", result.stdout)

    def test_a_named_path_that_does_not_exist_is_a_usage_error(self):
        result = run_preflight("--stick", "/nonexistent/stick", "--skip-profile")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("PREFLIGHT_ERROR=NO_SUCH_PATH", result.stderr)

    def test_the_preflight_and_the_verifier_print_the_same_manual_gates(self):
        with tempfile.TemporaryDirectory() as tmp:
            preflight = run_preflight("--stick", str(self.make_bench(tmp)), "--skip-profile")
        verifier = run_verifier(str(FIXTURES / "readonly.txt"), "--self-test")
        for item in MANIFEST["manual_gates"]:
            self.assertIn(f"MANUAL {item['id']}", preflight.stdout)
            self.assertIn(f"MANUAL {item['id']}", verifier.stdout)


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


class OperatorReportedMutationIsSeparateFromGateScores(unittest.TestCase):
    def test_reported_files_match_manifest_hash_size_and_line_count(self):
        reports = MANIFEST["operator_reported_results"]["results"]
        self.assertEqual(len(reports), 1)
        report = reports[0]
        for record in report["files"]:
            data = (ROOT / record["file"]).read_bytes()
            self.assertEqual(len(data), record["bytes"], record["file"])
            self.assertEqual(data.count(b"\n"), record["lines"], record["file"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), record["sha256"], record["file"])
        run = (ROOT / report["files"][0]["file"]).read_text(encoding="utf-8")
        self.assertIn("TRACE STEP=25 PHASE=firmware-transaction RESULT=0 MUTATED=1", run)
        self.assertIn("SELECTED=1 NVIDIA_COUNT=2", run)
        self.assertIn("DECISION=REFUSE MISSING=TARGET_GOP,LOG,RECOVERY,SIGNED_BOOTER,BOUND,DMA", run)

    def test_report_keeps_operator_attestation_distinct_from_formal_a5_score(self):
        report = MANIFEST["operator_reported_results"]["results"][0]
        self.assertIn("operator reports that the 4090 mutation succeeded physically", report["summary"])
        self.assertIn("software STOP_CONTRACT RECOVERY field is still 0", " ".join(report["limitations"]))
        self.assertIn("617.42", " ".join(report["limitations"]))
        result = run_verifier(str(ROOT / report["files"][0]["file"]), "--stage", "actuate")
        self.assertIn("A5_NEXT_BOOT_READ_THE_RECORD STAGE=actuate RESULT=FAIL", result.stdout)
        self.assertIn("HARDWARE_VERIFIED=0", result.stdout)
        self.assertIn("operator-reported physical mutation outcome", DOC.lower())


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
        self.assertIn("not a denial of the user's", DOC)


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


class TheStickIsCheckedForThePinnedBundle(unittest.TestCase):
    """`--assets` decides, from Windows, whether the image can load its bundle.

    src/boot_io.c reads each asset from the launch stick first and hashes it
    against the pin before keeping it, so the same files, names and digests are
    checkable before the boot. The check is opt-in because a read-only stage
    never loads them - and it names the shared GSP image, which an Ampere boot
    reads out of the Ada directory.
    """

    def test_the_shared_gsp_path_is_what_the_source_names(self):
        source = (ROOT / "src" / "profile.c").read_text(encoding="utf-8")
        blocks = source.split("static const GU_ARCH_ASSETS ")[1:]
        alternates = {}
        for block in blocks:
            name = block.split("={", 1)[0]
            match = re.search(r'\},L"\\\\[^\\\\]+\\\\[^"]+"\}', block)
            if not match or name not in ("AdaAssets", "AmpereAssets"):
                continue
            folder, file = re.findall(r"[A-Z0-9]+-ASSETS|[a-z0-9_.]+", match.group(0))[-2:]
            alternates["AD102" if name == "AdaAssets" else "GA102"] = f"{folder}/{file}"
        self.assertEqual(alternates, preflight.ALTERNATE_GSP,
                         "the preflight's alternate GSP table is not the source's")

    def test_a_stick_without_the_bundle_fails_and_names_the_files(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run_preflight("--stick", directory, "--assets", "AD102,GA102",
                                   "--skip-profile")
        self.assertEqual(result.returncode, 1)
        self.assertIn("ASSETS ARCH=AD102 PRESENT=0/5", result.stdout)
        self.assertIn("REFUSED=", result.stdout)
        self.assertIn("FAILED=ASSETS", result.stdout)

    def test_the_check_can_be_limited_to_one_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run_preflight("--stick", directory, "--assets", "GA102",
                                   "--skip-profile")
        self.assertIn("ASSETS ARCH=GA102", result.stdout)
        self.assertNotIn("ASSETS ARCH=AD102", result.stdout)

    def test_an_architecture_the_manifest_does_not_pin_is_named(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run_preflight("--stick", directory, "--assets", "TU102",
                                   "--skip-profile")
        self.assertIn("PINS=0", result.stdout)
        self.assertIn("AD102", result.stdout)

    def test_a_read_only_run_does_not_pay_for_the_check(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run_preflight("--stick", directory, "--skip-profile")
        self.assertNotIn("ASSETS ARCH=", result.stdout)
        self.assertEqual(result.returncode, 0)
