"""The variant catalogue is what the sign/deploy script will actually build.

`docs/image_variants.json` claims to list every EFI image this repository can
produce. `src/Build-Sign-Deploy-UniversalGPU.ps1` builds exactly that list and
refuses to rename an image whose name it did not predict, so the catalogue is
load-bearing: a wrong entry is a failed run at the bench, not a broken test.

These tests keep it honest against the builders:

  1. every family names a builder that exists, with the PE subsystem that
     builder's own `link /subsystem:` line produces;
  2. every switch a variant passes is a parameter of that builder;
  3. the expected output name is recomputed here from the builders' naming
     rules, so a renamed artifact cannot slip through;
  4. every attestation a variant requires names a field the builder's status
     line actually prints;
  5. the safety classes agree with the switches: only the waived variants claim
     `actuates`, only synthetic builds are `compile-only`, and a variant that
     needs a reviewed ESP GUID is exactly one that passes `WindowsEspGuid`;
  6. the script's own gates are in the text (no staging from `-All`, no DXE as
     `BOOTX64.EFI`, no actuating stage without an explicit switch), and it stays
     PowerShell 5.1-compatible;
  7. the rendered matrix in the user document is exactly the catalogue.
"""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
import sys

sys.path.insert(0, str(ROOT / "tools"))
import render_image_variants as renderer  # noqa: E402

MANIFEST = json.loads((ROOT / "docs" / "image_variants.json").read_text(encoding="utf-8"))
DOC = (ROOT / "docs" / "BUILD_SIGN_DEPLOY.md").read_text(encoding="utf-8")
SCRIPT_PATH = ROOT / "src" / "Build-Sign-Deploy-UniversalGPU.ps1"
SCRIPT = SCRIPT_PATH.read_text(encoding="utf-8")
FAMILY = {family["id"]: family for family in MANIFEST["families"]}

# The variant matrix models a selection as one target unless it asks for a
# sequence; that is the only reading of "any" that names a file.
SINGLE = "0000:01:00.0"
SEQUENCE = "0000:01:00.0,0000:04:00.0"


def builder_text(family_id: str) -> str:
    return (ROOT / FAMILY[family_id]["builder"]).read_text(encoding="utf-8")


def builder_parameters(family_id: str) -> set[str]:
    text = builder_text(family_id)
    block = text[text.index("param("):]
    block = block[:block.index("\n)")]
    return set(re.findall(r"\$(\w+)", block))


def selection_count(variant: dict) -> int:
    needs = variant.get("requires_selection")
    if needs == "sequence":
        return 2
    if needs in ("single", "any"):
        return 1
    return 0


def label(count: int) -> str:
    """The label src/Gpu-Selection-Build.ps1 computes for this selection."""
    parts = (SINGLE if count == 1 else SEQUENCE).split(",")
    canonical = [",".join(parts)] if False else parts
    rendered = ["-".join(part.replace(".", ":").split(":")) for part in canonical]
    if len(rendered) <= 2:
        return "--".join(rendered)
    raise AssertionError("the matrix never uses more than two targets")


def expected_binary(variant: dict) -> str:
    switches = variant["switches"]
    count = selection_count(variant)
    guid = "WindowsEspGuid" in switches
    family = variant["family"]
    if family == "diagnostic":
        if switches.get("MsiB850RomVariables"):
            stem = "MSI-B850-ROM-VARIABLES"
        elif switches.get("MsiB850FuseVersion"):
            stem = "MSI-B850-FUSE-VERSION"
        elif switches.get("MsiB850Sec2Ucode1"):
            stem = "MSI-B850-SEC2-UCODE1"
        else:
            stem = None
        if stem:
            return f"{stem}{'-WINDOWS-HANDOFF' if guid else ''}-DIAGNOSTIC.EFI"
        if count:
            return f"GPU-SELECTION-<label>-DIAGNOSTIC.EFI"
        if guid:
            return "GPU-TOPOLOGY-WINDOWS-HANDOFF-DIAGNOSTIC.EFI"
        return "GPU-TOPOLOGY-DIAGNOSTIC.EFI"
    if family in ("prepare", "prepare-dxe"):
        dxe = "-DXE" if family == "prepare-dxe" else ""
        ext = ".efi" if family == "prepare-dxe" else ".EFI"
        if switches.get("RuntimeSelection") and switches.get("AcceptUnverifiedActuation"):
            return f"GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION{dxe}{ext}"
        if switches.get("RuntimeSelection"):
            return f"GPU-RUNTIME-SELECTION-PREP{dxe}{ext}"
        if switches.get("AcceptUnverifiedActuation") and count > 1:
            return f"GPU-SEQUENCE-UNVERIFIED-ACTUATION{dxe}-<label>{ext}"
        if switches.get("AcceptUnverifiedActuation"):
            return f"GPU-ONE-TARGET-UNVERIFIED-ACTUATION{dxe}-<label>{ext}"
        if switches.get("SyntheticCompileProfile") and count > 1:
            return f"GPU-SEQUENCE-COMPILE-ONLY{dxe}-<label>{ext}"
        if switches.get("SyntheticCompileProfile"):
            return f"GPU-ONE-TARGET-COMPILE-ONLY{dxe}-<label>{ext}"
        if count:
            return f"GPU-ACTIVE-PREP{dxe}-<label>{ext}"
        return "AD102-EXPERIMENTAL.EFI" if family == "prepare" else "AdaUnlockDxe.efi"
    if family == "stop-observation":
        if switches.get("BothNvidia"):
            target = "DUAL-NVIDIA"
        elif switches.get("Target3080"):
            target = "RTX3080"
        else:
            target = "RTX4090"
        suffix = "WINDOWS-HANDOFF" if guid else "ONCE"
        return f"MSI-{target}-STOP-{suffix}-EXPERIMENT.EFI"
    raise AssertionError(f"unmodelled family: {family}")


def powershell_source() -> str:
    """The script with here-strings, quoted strings and comments removed."""
    text = re.sub(r"@'\n.*?\n'@", "", SCRIPT, flags=re.S)
    text = re.sub(r"@\"\n.*?\n\"@", "", text, flags=re.S)
    text = re.sub(r"'[^'\n]*'", "''", text)
    text = re.sub(r'"[^"\n]*"', '""', text)
    return re.sub(r"#[^\n]*", "", text)


def script_parameters() -> list[str]:
    block = SCRIPT[SCRIPT.index("param("):]
    block = block[:block.index("\n)\n")]
    return re.findall(r"\$(\w+)", block)


def validated_parameters() -> dict[str, str]:
    block = SCRIPT[SCRIPT.index("param("):]
    block = block[:block.index("\n)\n")]
    found = {}
    for match in re.finditer(r"\[(Validate\w+)\([^\n]*?\)\]\s*\n\s*\[[^\]]+\]\$(\w+)", block):
        found[match.group(2)] = match.group(1)
    return found


class TheCatalogueMatchesTheBuilders(unittest.TestCase):
    def test_every_family_names_a_builder_that_exists(self):
        for family in MANIFEST["families"]:
            self.assertTrue((ROOT / family["builder"]).is_file(), family["id"])

    def test_every_family_declares_the_subsystem_its_builder_links(self):
        for family in MANIFEST["families"]:
            text = builder_text(family["id"])
            want = "efi_application" if family["subsystem"] == 10 else "efi_boot_service_driver"
            other = "efi_boot_service_driver" if family["subsystem"] == 10 else "efi_application"
            self.assertIn(f"/subsystem:{want}", text, family["id"])
            self.assertNotIn(f"/subsystem:{other}", text, family["id"])

    def test_every_switch_is_a_parameter_of_that_builder(self):
        for variant in MANIFEST["variants"]:
            parameters = builder_parameters(variant["family"])
            for switch, value in variant["switches"].items():
                self.assertIn(switch, parameters,
                              f"{variant['id']}: {switch} is not a parameter of "
                              f"{FAMILY[variant['family']]['builder']}")
                if switch == "WindowsEspGuid":
                    self.assertEqual(value, "<esp>", variant["id"])
                elif isinstance(value, bool):
                    self.assertTrue(value, f"{variant['id']}: a false switch is not a switch")

    def test_the_status_token_is_the_one_the_builder_prints(self):
        for family in MANIFEST["families"]:
            self.assertIn(f'"{family["status_token"]} PATH=', builder_text(family["id"]),
                          family["id"])

    def test_every_expected_output_name_follows_the_builders_naming_rule(self):
        for variant in MANIFEST["variants"]:
            self.assertEqual(variant["binary"], expected_binary(variant),
                             f"{variant['id']}: the catalogue and the builder naming disagree")

    def test_labels_are_what_the_selection_helper_produces(self):
        helper = (ROOT / "src" / "Gpu-Selection-Build.ps1").read_text(encoding="utf-8")
        match = re.search(r"if \(\$canonical\.Count -le 2\) \{\s*\$label = (.+)", helper)
        self.assertIsNotNone(match, "the selection helper no longer computes a two-target label")
        self.assertIn("--", match.group(1))
        self.assertIn("-replace '[:.]','-'", match.group(1))
        self.assertEqual(label(1), "0000-01-00-0")
        self.assertEqual(label(2), "0000-01-00-0--0000-04-00-0")

    def test_every_attestation_names_a_field_the_builder_prints(self):
        for variant in MANIFEST["variants"]:
            text = builder_text(variant["family"])
            for token in variant["attestations"]:
                key = token.split("=")[0]
                self.assertIn(f"{key}=", text,
                              f"{variant['id']}: {key}= is not printed by "
                              f"{FAMILY[variant['family']]['builder']}")


class TheSafetyClassesAreConsistent(unittest.TestCase):
    def test_read_only_images_never_pass_a_waiver_or_a_synthetic_profile(self):
        for variant in MANIFEST["variants"]:
            if variant["safety"] != "read-only":
                continue
            self.assertEqual(variant["family"], "diagnostic", variant["id"])
            for switch in ("AcceptUnverifiedActuation", "SyntheticCompileProfile", "AcceptUnboundedStopRisk"):
                self.assertNotIn(switch, variant["switches"], variant["id"])

    def test_only_waived_variants_claim_actuation(self):
        for variant in MANIFEST["variants"]:
            waived = bool(variant["switches"].get("AcceptUnverifiedActuation"))
            self.assertEqual(waived, variant["safety"] == "actuates", variant["id"])

    def test_only_synthetic_profiles_are_compile_only(self):
        for variant in MANIFEST["variants"]:
            synthetic = bool(variant["switches"].get("SyntheticCompileProfile"))
            self.assertEqual(synthetic, variant["safety"] == "compile-only", variant["id"])

    def test_only_the_stop_family_is_stop_only(self):
        families = {v["family"] for v in MANIFEST["variants"] if v["safety"] == "stop-only"}
        self.assertEqual(families, {"stop-observation"})
        for variant in MANIFEST["variants"]:
            if variant["family"] == "stop-observation":
                self.assertTrue(variant["switches"]["AcceptUnboundedStopRisk"], variant["id"])

    def test_a_reviewed_esp_guid_is_exactly_a_windows_esp_guid_switch(self):
        for variant in MANIFEST["variants"]:
            self.assertEqual(bool(variant.get("requires_esp_guid")),
                             "WindowsEspGuid" in variant["switches"], variant["id"])

    def test_the_dual_stop_never_appears_without_pinned_handoff(self):
        for variant in MANIFEST["variants"]:
            if variant["switches"].get("BothNvidia"):
                self.assertIn("WindowsEspGuid", variant["switches"], variant["id"])
                self.assertTrue(variant["switches"].get("AcceptWindowsHandoffRisk"), variant["id"])

    def test_every_safety_class_says_whether_it_writes_the_gpu(self):
        for name, safety in MANIFEST["safety_classes"].items():
            self.assertIn("writes_gpu", safety, name)
            self.assertIn("may_be_staged", safety, name)
            self.assertTrue(safety["why"], name)
        self.assertFalse(MANIFEST["safety_classes"]["read-only"]["writes_gpu"])
        self.assertTrue(MANIFEST["safety_classes"]["actuates"]["writes_gpu"])

    def test_the_diagnostic_family_never_needs_a_selection_to_build(self):
        for variant in MANIFEST["variants"]:
            if variant["family"] != "diagnostic":
                continue
            if variant.get("requires_selection"):
                self.assertNotIn("MsiB850RomVariables", variant["switches"], variant["id"])
                self.assertNotIn("MsiB850FuseVersion", variant["switches"], variant["id"])
                self.assertNotIn("MsiB850Sec2Ucode1", variant["switches"], variant["id"])


class TheScriptHonoursTheCatalogue(unittest.TestCase):
    def test_the_script_reads_the_catalogue(self):
        self.assertIn("docs\\image_variants.json", SCRIPT)
        self.assertIn("ConvertFrom-Json", SCRIPT)

    def test_the_staging_rule_mirrors_the_safety_classes(self):
        for name, safety in MANIFEST["safety_classes"].items():
            expected = (not safety["writes_gpu"]) and name != "compile-only"
            self.assertEqual(safety["may_be_staged"] is True, expected,
                             f"{name}: may_be_staged must be true exactly when the image "
                             "is safe to boot and has something to observe")

    def test_the_script_never_stages_from_a_matrix_run(self):
        self.assertIn("-All must not stage", SCRIPT)
        self.assertIn("-SignOnly and -Deploy are mutually exclusive", SCRIPT)

    def test_the_script_refuses_a_dxe_as_a_boot_application(self):
        self.assertIn("A DXE boot-service driver cannot be staged as BOOTX64.EFI", SCRIPT)

    def test_the_script_requires_an_explicit_switch_before_staging_an_actuating_image(self):
        self.assertIn("-AcceptActuatingDeploy", SCRIPT)
        self.assertIn("Refusing to stage", SCRIPT)
        self.assertIn("writes_gpu", SCRIPT)

    def test_the_script_checks_the_certificate_is_in_the_bios_db(self):
        self.assertIn("Get-SecureBootUEFI -Name db", SCRIPT)
        self.assertIn("$X509SignatureTypeGuidBytes", SCRIPT)
        self.assertIn("is not present in the BIOS db", SCRIPT)
        self.assertIn("DB_ENROLLED=", SCRIPT)

    def test_the_private_key_never_leaves_wsl(self):
        self.assertIn("secureboot-signing/uefi-db.key", SCRIPT)
        self.assertIn("$HOME/", SCRIPT)
        # No Windows-side path for the key, and no copy of it into the repo.
        self.assertNotIn("Copy-Item -LiteralPath $key", SCRIPT)
        self.assertNotIn("uefi-db.key')", SCRIPT)

    def test_paths_cross_into_wsl_only_as_base64(self):
        self.assertIn("base64 -d | bash", SCRIPT)
        self.assertIn("To-B64", SCRIPT)
        self.assertIn("wslpath -u", SCRIPT)
        self.assertIn("still contains carriage returns", SCRIPT)

    def test_the_script_stays_powershell_51_compatible(self):
        self.assertIn("#Requires -Version 5.1", SCRIPT)
        # The embedded bash payload may use && and ||; PowerShell may not.
        outside = powershell_source()
        for banned in ("??", "?.", "-Parallel", "&&", "||"):
            self.assertNotIn(banned, outside, f"{banned} is not PowerShell 5.1")

    def test_the_script_is_brace_balanced(self):
        outside = powershell_source()
        self.assertEqual(outside.count("{"), outside.count("}"))
        self.assertEqual(outside.count("("), outside.count(")"))

    def test_no_local_variable_shadows_a_parameter_of_the_script(self):
        # PowerShell is case-insensitive, so `foreach ($family in ...)` assigns to
        # the [ValidateSet]-constrained $Family parameter and the script dies on
        # its first line of real work. Only these three plain strings may be
        # reassigned; anything else is a shadowing bug.
        allowed = {"RepoRoot", "OutputDirectory", "PublicCertificate"}
        body = powershell_source()
        assigned = set(m.group(1) for m in re.finditer(r"(?<![\w:])\$(\w+)\s*=[^=]", body))
        assigned |= set(m.group(1) for m in re.finditer(r"foreach\s*\(\s*\$(\w+)\s+in", body))
        parameters = script_parameters()
        for name in sorted(assigned):
            match = [p for p in parameters if p.lower() == name.lower()]
            self.assertFalse(
                match and match[0] not in allowed,
                f"${name} shadows the parameter ${match[0] if match else name}; "
                "PowerShell would validate the assigned value against the parameter")

    def test_no_validated_parameter_is_written_to(self):
        body = powershell_source()
        for name, attribute in validated_parameters().items():
            pattern = r"(?<![\w:])\$" + re.escape(name) + r"\s*=[^=]"
            self.assertIsNone(re.search(pattern, body, flags=re.IGNORECASE),
                              f"${name} carries [{attribute}] and must not be reassigned")

    def test_the_script_does_not_reuse_a_switch_name(self):
        body = powershell_source()
        for name in ("List", "All", "Only", "Deploy", "SignOnly", "AcceptActuatingDeploy"):
            pattern = r"(?<![\w:])\$" + name + r"\s*=[^=]"
            self.assertIsNone(re.search(pattern, body, flags=re.IGNORECASE), name)

    def test_the_signing_call_cannot_leak_output_into_the_exit_code(self):
        # A live `& wsl.exe` line next to `return $LASTEXITCODE` hands the caller
        # an array of sbverify output plus the code, so a good signature reads as
        # a failure. The output must be captured and the code returned alone.
        self.assertIn("$wslOutput = & wsl.exe", SCRIPT)
        self.assertIn("return [int]$exitCode", SCRIPT)
        self.assertNotIn("return $LASTEXITCODE", SCRIPT)
        bare = [line for line in SCRIPT.splitlines() if line.strip().startswith("& wsl.exe")]
        self.assertEqual(bare, [], "the wsl.exe call must be captured, not left on the pipeline")
        self.assertIn("-isnot [int]", SCRIPT)

    def test_a_selection_that_cannot_build_a_variant_skips_it(self):
        # -All with two BDFs must not fail the single-target variants: the builder
        # would produce the sequence image under a name this catalogue gives to
        # something else.
        self.assertIn("function Test-VariantSelection", SCRIPT)
        self.assertIn("NEEDS_EXACTLY_ONE_BDF", SCRIPT)
        self.assertIn("NEEDS_TWO_OR_MORE_BDFS", SCRIPT)
        self.assertIn("VARIANT_SKIPPED", SCRIPT)
        self.assertIn("SKIPPED_HINT", SCRIPT)

    def test_the_catalogue_has_variants_that_need_each_selection_shape(self):
        shapes = {v.get("requires_selection") for v in MANIFEST["variants"]}
        self.assertEqual(shapes, {None, "any", "single", "sequence"})

    def test_no_format_operator_hides_after_a_concatenation(self):
        # `"a" + 'b' -f $x` formats the second string only and leaves {0} literal.
        lines = SCRIPT.splitlines()
        for index, line in enumerate(lines):
            if not line.rstrip().endswith(("' +", '" +')):
                continue
            # Walk the continuation: every line that begins with a quote is part
            # of this concatenation, and none of them may carry a -f operator.
            following = index + 1
            while following < len(lines) and lines[following].lstrip().startswith(("'", '"')):
                self.assertNotIn("-f", lines[following],
                                 f"line {following + 1}: a -f operator hides in a "
                                 "concatenation and would format only the last string")
                following += 1

    def test_a_long_matrix_run_shows_a_progress_count(self):
        self.assertIn("VARIANT_BEGIN [{0}/{1}]", SCRIPT)
        self.assertIn("$variantTotal = $variants.Count", SCRIPT)

    def test_sign_only_refuses_a_stage_drive(self):
        self.assertIn("-SignOnly and -StageDrive are mutually exclusive", SCRIPT)

    def test_the_script_states_what_signing_does_not_prove(self):
        self.assertIn("HARDWARE_VERIFIED=0", SCRIPT)
        self.assertIn("HardwareVerified = $false", SCRIPT)
        self.assertIn("Windows61742BooterProven = $false", SCRIPT)

    def test_no_variant_claims_a_hardware_result(self):
        text = json.dumps(MANIFEST).lower()
        for claim in ('"hardware_verified": true', '"verified": true', '"safe": true'):
            self.assertNotIn(claim, text)

    def test_a_failing_build_still_shows_what_the_compiler_said(self):
        # `@(& $builder @args)` buffers the output and throws it away when the
        # builder throws, so a real compile error reached the operator as a bare
        # "Compilation failed" with no clue in it. The output is streamed now,
        # and the failure names the last line the builder printed.
        self.assertNotIn("@(& $buildScript", SCRIPT)
        self.assertIn("$buildError = $_", SCRIPT)
        self.assertIn("Builder failed for {0}: {1}. Last builder line: {2}", SCRIPT)

    def test_no_array_subexpression_wraps_a_generic_list(self):
        # Windows PowerShell 5.1 throws ArgumentException "Argument types do not
        # match" from @() around a List[object] held in a variable, and reports it
        # against the enclosing hashtable literal - which looked like the matrix
        # JSON failing at the end of a run.
        lists = re.findall(r"\$(\w+)\s*=\s*New-Object 'System\.Collections\.Generic\.List\[", SCRIPT)
        self.assertTrue(lists, "the script is expected to use generic lists")
        for name in lists:
            self.assertIsNone(
                re.search(r"@\(\s*\$" + re.escape(name) + r"\s*\)", SCRIPT),
                f"@(${name}) throws on Windows PowerShell 5.1; use ${name}.ToArray()")
        self.assertIn("Variants = $results.ToArray()", SCRIPT)

    def test_signing_stderr_cannot_terminate_the_run(self):
        # sbsign writes "Signing Unsigned original image" on stderr. With
        # $ErrorActionPreference='Stop' and 2>&1, Windows PowerShell 5.1 turns
        # that banner into a terminating NativeCommandError, so every variant of
        # a good run failed with the banner as its message.
        block = SCRIPT[SCRIPT.index("function Invoke-WslSign"):SCRIPT.index("function Stage-SignedEfi")]
        self.assertIn("$ErrorActionPreference = 'Continue'", block)
        self.assertIn("$restorePreference = $ErrorActionPreference", block)

    def test_a_variant_that_needs_the_private_boot_profile_is_skipped(self):
        # src/boot_profile.h includes the private local/boot_profile.h for a
        # waived build: without it the compiler stops, which is a setup step the
        # script should name before spending a compile on it.
        self.assertIn("NEEDS_LOCAL_BOOT_PROFILE", SCRIPT)
        self.assertIn("requires_local_boot_profile", SCRIPT)
        self.assertIn("local\\boot_profile.h", SCRIPT)
        self.assertIn("SETUP_HINT", SCRIPT)

    def test_a_private_boot_profile_is_required_exactly_where_the_source_demands_it(self):
        for variant in MANIFEST["variants"]:
            switches = variant["switches"]
            waived = bool(switches.get("AcceptUnverifiedActuation"))
            runtime_esp = bool(switches.get("RuntimeEsp"))
            self.assertEqual(
                bool(variant.get("requires_local_boot_profile")),
                waived and not runtime_esp,
                f"{variant['id']}: only a waived build without -RuntimeEsp compiles a "
                "pinned ESP, so only it may demand local/boot_profile.h")

    def test_no_preparation_variant_uses_runtime_selection_while_the_source_refuses_it(self):
        # Re-add prep-runtime-selection only if the source grows a preparation
        # profile for runtime target selection; until then it has no legal build.
        port = (ROOT / "src" / "port.h").read_text(encoding="utf-8")
        self.assertIn("Runtime target selection requires the ordered selected-target execution mode", port)
        discovery = (ROOT / "src" / "gpu_discovery.c").read_text(encoding="utf-8")
        self.assertIn("One-target active execution requires explicit transaction mode", discovery)
        blocked = {entry["id"] for entry in MANIFEST["not_buildable"]}
        live = {variant["id"] for variant in MANIFEST["variants"]}
        for name in ("prep-runtime-selection", "prep-runtime-selection-resident",
                     "dxe-prep-runtime-selection"):
            self.assertIn(name, blocked)
            self.assertNotIn(name, live)

    def test_an_explicit_only_request_never_skips_silently(self):
        self.assertIn("Cannot build {0}: {1}. -Only asked for this image specifically", SCRIPT)

    def test_a_signing_preflight_runs_before_the_first_build(self):
        # A wrong -WslDistribution/-WslUser used to surface as a failed signature
        # on the first image, after every build had already run. The preflight
        # proves the WSL side once, at the top.
        self.assertIn("function Test-WslSigningReady", SCRIPT)
        self.assertIn("WSL_SIGNING_READY", SCRIPT)
        call = SCRIPT.index("[void](Test-WslSigningReady")
        loop = SCRIPT.index("foreach ($variant in $variants)")
        self.assertLess(call, loop, "the preflight must run before the matrix")

    def test_the_preflight_names_the_wsl_target_it_used(self):
        # "Did my -WslDistribution / -WslUser matter?" has to be answerable from
        # the log; the line reports what wsl.exe actually resolved.
        block = SCRIPT[SCRIPT.index("function Test-WslSigningReady"):SCRIPT.index("function Stage-SignedEfi")]
        for token in ("WSL_DISTRO_NAME", "id -un", "home=$HOME", "key=$key", "tools="):
            self.assertIn(token, block)

    def test_the_preflight_reports_every_gap_before_building(self):
        block = SCRIPT[SCRIPT.index("function Test-WslSigningReady"):SCRIPT.index("function Stage-SignedEfi")]
        # No `set -e` in this payload: one missing tool must not hide a missing key.
        self.assertIn("set -o pipefail", block)
        self.assertNotIn("set -euo pipefail", block)
        for token in ("WSL_SIGNING_MISSING tool=", "WSL_SIGNING_MISSING key=",
                      "WSL_SIGNING_MISMATCH cer=", "WSL_SIGNING_MISMATCH key=",
                      "WSL_SIGNING_BROKEN"):
            self.assertIn(token, block)

    def test_stage_drive_alone_stages_the_selected_image(self):
        # -StageDrive is the intent, -Only is which image. Requiring a second copy
        # of the id was redundant, and the run that forgot it looked successful.
        self.assertIn("if ($StageDrive) {", SCRIPT)
        self.assertIn("$stageVariantId = $deployVariant.id", SCRIPT)
        self.assertIn("-StageDrive stages exactly one image", SCRIPT)
        self.assertNotIn("nothing is copied without -Deploy", SCRIPT)

    def test_a_deploy_that_disagrees_with_the_selection_is_refused(self):
        # -Deploy survives only as an assertion about what gets staged.
        self.assertIn("-Deploy {0} is not the selected variant {1}", SCRIPT)

    def test_deploy_without_a_stage_drive_is_refused(self):
        self.assertIn("-Deploy names the image to stage, so it needs -StageDrive <letter>", SCRIPT)

    def test_a_deploy_run_says_what_it_staged(self):
        self.assertIn('Say (("STAGED {0} PATH={1} DRIVE={2}:")', SCRIPT)
        self.assertIn("$stagedPath = $deployment.Path", SCRIPT)


class TheDocumentIsRendered(unittest.TestCase):
    def test_the_matrix_block_is_exactly_the_rendered_catalogue(self):
        start = DOC.index(renderer.BEGIN)
        end = DOC.index(renderer.END) + len(renderer.END)
        self.assertEqual(DOC[start:end], renderer.render(MANIFEST))

    def test_every_variant_is_visible_to_the_operator(self):
        for variant in MANIFEST["variants"]:
            self.assertIn(f"`{variant['id']}`", DOC)
            stem = variant["binary"].split("<label>")[0]
            self.assertIn(stem, DOC, variant["id"])

    def test_the_document_covers_the_gates_and_the_caveat(self):
        flat = " ".join(DOC.split())
        for phrase in ("-AcceptActuatingDeploy", "-SignOnly", "never stages",
                       "HARDWARE_VERIFIED=0", "does not prove", "DB_ENROLLED"):
            self.assertTrue(phrase in flat, f"the document never says {phrase!r}")

    def test_the_document_maps_to_the_donor_session(self):
        for variant in ("diag-topology-log", "prep-selected", "actuate-one-target"):
            self.assertIn(variant, DOC)
        self.assertIn("DONOR_HARDWARE_SESSION.md", DOC)


if __name__ == "__main__":
    unittest.main()
