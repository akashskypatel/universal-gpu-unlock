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
import tempfile
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
    if family == "maintenance":
        return "BOOT-STATE-MAINTENANCE.EFI"
    if family == "diagnostic":
        if switches.get("MsiB850RomVariables"):
            stem = "MSI-B850-ROM-VARIABLES"
        elif switches.get("MsiB850FuseVersion"):
            stem = "MSI-B850-FUSE-VERSION"
        elif switches.get("MsiB850Sec2Ucode1"):
            stem = "MSI-B850-SEC2-UCODE1"
        elif switches.get("VramBlockProbe"):
            stem = "VRAM-BLOCK-PROBE"
        else:
            stem = None
        if stem:
            return f"{stem}{'-WINDOWS-HANDOFF' if guid else ''}-DIAGNOSTIC.EFI"
        if switches.get("CycleGopText"):
            return "GPU-TOPOLOGY-GOP-TEXT-CYCLE-DIAGNOSTIC.EFI"
        if count:
            return f"GPU-SELECTION-<label>-DIAGNOSTIC.EFI"
        if guid:
            return "GPU-TOPOLOGY-WINDOWS-HANDOFF-DIAGNOSTIC.EFI"
        return "GPU-TOPOLOGY-DIAGNOSTIC.EFI"
    if family in ("prepare", "prepare-dxe"):
        dxe = "-DXE" if family == "prepare-dxe" else ""
        ext = ".efi" if family == "prepare-dxe" else ".EFI"
        if switches.get("FirmwareReattach"):
            return "GPU-RUNTIME-FIRMWARE-REATTACH.EFI"
        if switches.get("StopReleaseWindowsHandoff"):
            return "GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-WINDOWS-HANDOFF.EFI"
        if switches.get("StopReleaseOnlyBench"):
            return f"GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME{dxe}{ext}"
        operation_suffix = {"Menu": "", "Unlock": "-UNLOCK",
                            "UnlockWithResume": "-UNLOCK-WITH-RESUME",
                            "InstallDriver": "-INSTALL-DRIVER",
                            "RemoveDriver": "-REMOVE-DRIVER"}[switches.get("ApplicationOperation", "Menu")]
        if switches.get("RuntimeSelection") and switches.get("AcceptUnverifiedActuation"):
            return f"GPU-RUNTIME-SELECTION-UNVERIFIED-ACTUATION{dxe}{operation_suffix}{ext}"
        if switches.get("RuntimeSelection"):
            return f"GPU-RUNTIME-SELECTION-PREP{dxe}{ext}"
        if switches.get("AcceptUnverifiedActuation") and count > 1:
            return f"GPU-SEQUENCE-UNVERIFIED-ACTUATION{dxe}-<label>{operation_suffix}{ext}"
        if switches.get("AcceptUnverifiedActuation"):
            return f"GPU-ONE-TARGET-UNVERIFIED-ACTUATION{dxe}-<label>{operation_suffix}{ext}"
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

    def test_four_fixed_operation_profiles_are_distinct(self):
        expected = {"app-unlock": "Unlock", "app-unlock-with-resume": "UnlockWithResume",
                    "app-install-driver": "InstallDriver", "app-remove-driver": "RemoveDriver"}
        found = {v["id"]: v for v in MANIFEST["variants"]
                 if v["switches"].get("ApplicationOperation") and not v["switches"].get("FirmwareReattach")}
        self.assertEqual({key: v["switches"]["ApplicationOperation"] for key, v in found.items()}, expected)
        self.assertEqual(len({v["binary"] for v in found.values()}), 4)
        for v in found.values():
            self.assertEqual(v["family"], "prepare")
            self.assertEqual(v["safety"], "actuates")
            self.assertTrue(v["switches"]["AcceptUnverifiedActuation"])
            self.assertTrue(v["switches"]["RuntimeSelection"])
            self.assertTrue(v["switches"]["RuntimeEsp"])
            self.assertIn("FIXED_OPERATION=1", v["attestations"])

    def test_combined_profile_is_separate_from_fixed_detached_operations(self):
        variants = [v for v in MANIFEST["variants"] if v["switches"].get("FirmwareReattach")]
        self.assertEqual(len(variants), 1)
        v = variants[0]
        self.assertEqual(v["id"], "app-firmware-reattach")
        self.assertEqual(v["family"], "prepare")
        self.assertEqual(v["safety"], "actuates")
        for switch in ("RuntimeSelection", "RuntimeEsp", "EnableSingleTargetExecution", "AcceptUnverifiedActuation"):
            self.assertTrue(v["switches"][switch])
        self.assertEqual(v["switches"]["ApplicationOperation"], "Unlock")
        self.assertNotIn("StopReleaseOnlyBench", v["switches"])
        self.assertNotIn("EspResident", v["switches"])
        self.assertIn("ASPM_ADMISSION_BYPASSED=1", v["attestations"])

    def test_every_attestation_names_a_field_the_builder_prints(self):
        for variant in MANIFEST["variants"]:
            text = builder_text(variant["family"])
            for token in variant["attestations"]:
                key = token.split("=")[0]
                self.assertIn(f"{key}=", text,
                              f"{variant['id']}: {key}= is not printed by "
                              f"{FAMILY[variant['family']]['builder']}")


class TheAttestationsAreWhatTheBuilderWouldPrint(unittest.TestCase):
    """The declared value, not just the field name.

    A catalogue entry says what an image is; the builder prints what it built, and
    the script compares the two literally. Nothing compared the *values* until
    `actuate-sequence` declared `SINGLE_TARGET_ACTIVE=1` and the builder printed
    `0` for its two compiled targets - visible only in a real `-Only` build with
    two BDFs, because CI never builds an image that may write.
    """

    def model(self, variant):
        switches = variant.get("switches", {})
        synthetic = 1 if switches.get("SyntheticCompileProfile") else 0
        accept = 1 if switches.get("AcceptUnverifiedActuation") else 0
        runtime = 1 if switches.get("RuntimeSelection") else 0
        runtime_esp = 1 if switches.get("RuntimeEsp") else 0
        unsafe_bench = 1 if switches.get("StopReleaseOnlyBench") else 0
        combined = 1 if switches.get("FirmwareReattach") else 0
        operation = switches.get("ApplicationOperation", "Menu")
        firmware_transaction = int(accept and not unsafe_bench and operation not in ("InstallDriver", "RemoveDriver"))
        count = {"single": 1, "sequence": 2, "any": 1}.get(variant.get("requires_selection") or "", 0)
        return count, {
            "SELECTED_COUNT": "RUNTIME" if combined else count,
            "RUNTIME_SELECTION": runtime,
            "RUNTIME_ESP": runtime_esp,
            "PREPARATION_ONLY": int(count > 0 and not synthetic and not accept),
            "SINGLE_TARGET_ACTIVE": int((synthetic or accept) and (runtime or count == 1)),
            "ORDERED_SEQUENCE_COMPILE_ONLY": int(synthetic and count > 1),
            "ORDERED_SEQUENCE": int((accept and count > 1) or unsafe_bench or combined),
            "ASPM_ADMISSION_BYPASSED": int(unsafe_bench or combined),
            "INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED": 1 if combined else "CONDITIONAL" if unsafe_bench else 0,
            "FILE_ONLY_OUTPUT": unsafe_bench,
            "STOP_RELEASE_ONLY": unsafe_bench,
            "APP_OPERATION": operation,
            "FIXED_OPERATION": int(operation != "Menu"),
            "FIRMWARE_TRANSACTION": firmware_transaction,
            "UNLOCK": firmware_transaction,
            "REATTACH_ATTEMPTED": int(unsafe_bench or combined),
            "FIRMWARE_REATTACH": combined,
            "REATTACH_VERIFIED": 0,
            "FIRMWARE_ASSET_FILES_READ": 0 if unsafe_bench else None,
            "PROM_READ": 0 if unsafe_bench else None,
            "OS_HANDOFF": ("WINDOWS_AFTER_SUCCESS" if switches.get("StopReleaseWindowsHandoff") else "NONE") if unsafe_bench else None,
            "TARGET_ORDER": "RUNTIME_OPERATOR_SELECTION" if combined else "RUNTIME_SUPPORTED_NVIDIA_ENUMERATION" if unsafe_bench else "",
            "MULTI_GPU_MUTATION_ALLOWED": accept,
            "SYNTHETIC_PROFILE": synthetic,
            "UNVERIFIED_ACTUATION": accept,
            "ACTUATION_VERIFIED": 0,
            "CAN_DETACH": 0,
            "CAN_UNLOCK": 0,
            "CAN_FLASH": 0,
            "RECOVERY_VERIFIED": 0,
            "NOT_FOR_DEPLOYMENT": int(synthetic or accept),
        }

    MODELLED_FAMILIES = ("prepare", "prepare-dxe")

    def test_every_declared_value_is_the_one_the_builder_prints(self):
        # The arithmetic below is the prepare family's STATUS line. The other
        # families print their own tokens (WINDOWS_HANDOFF, NO_KEY_INPUT, ...);
        # their field names are covered above, their values are their builders'
        # own business.
        checked = 0
        for variant in MANIFEST["variants"]:
            if variant["family"] not in self.MODELLED_FAMILIES:
                continue
            count, model = self.model(variant)
            text = builder_text(variant["family"])
            for token in variant["attestations"]:
                key, _, value = token.partition("=")
                with self.subTest(variant=variant["id"], token=token):
                    if key == "WAIVER":
                        self.assertIn(token, text)
                        continue
                    self.assertIn(key, model,
                                  f"{variant['id']}: {key} is not modelled, so nothing "
                                  "checks its value")
                    if value == "<selection-count>":
                        self.assertGreaterEqual(count, 1,
                                                f"{variant['id']}: a count token needs a selection")
                        checked += 1
                        continue
                    self.assertEqual(str(model[key]), value,
                                     f"{variant['id']}: the catalogue says {token} and "
                                     f"{FAMILY[variant['family']]['builder']} prints "
                                     f"{key}={model[key]}")
                    checked += 1
        self.assertGreater(checked, 20)

    def test_a_count_token_is_the_only_placeholder(self):
        for variant in MANIFEST["variants"]:
            for token in variant["attestations"]:
                for placeholder in re.findall(r"<[^>]+>", token):
                    self.assertEqual(placeholder, "<selection-count>", variant["id"])

    def test_the_script_resolves_the_count_and_refuses_other_brackets(self):
        self.assertIn(".Replace('<selection-count>', [string](Get-SelectionCount))", SCRIPT)
        self.assertIn("Unknown attestation placeholder for", SCRIPT)

    def test_the_sequence_images_do_not_claim_a_single_target(self):
        for vid in ("actuate-sequence", "dxe-actuate-sequence"):
            variant = [v for v in MANIFEST["variants"] if v["id"] == vid][0]
            self.assertIn("SINGLE_TARGET_ACTIVE=0", variant["attestations"], vid)
            self.assertIn("ORDERED_SEQUENCE=1", variant["attestations"], vid)
            self.assertIn("SELECTED_COUNT=<selection-count>", variant["attestations"], vid)

    def test_the_one_target_images_pin_the_other_way(self):
        for vid in ("actuate-one-target", "actuate-one-target-resident", "dxe-actuate-one-target"):
            variant = [v for v in MANIFEST["variants"] if v["id"] == vid][0]
            self.assertIn("SINGLE_TARGET_ACTIVE=1", variant["attestations"], vid)
            self.assertIn("ORDERED_SEQUENCE=0", variant["attestations"], vid)
            self.assertIn("SELECTED_COUNT=1", variant["attestations"], vid)


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
            expected = (not safety["writes_gpu"]) and name not in ("compile-only", "maintenance")
            self.assertEqual(safety["may_be_staged"] is True, expected,
                             f"{name}: may_be_staged must be true exactly when the image "
                             "is safe to boot and has something to observe")

    def test_maintenance_is_explicit_and_isolated(self):
        variants = [v for v in MANIFEST["variants"] if v["family"] == "maintenance"]
        self.assertEqual(len(variants), 1)
        variant = variants[0]
        self.assertEqual(variant["id"], "boot-state-maintenance")
        self.assertEqual(variant["safety"], "maintenance")
        self.assertEqual(variant["switches"], {})
        safety = MANIFEST["safety_classes"]["maintenance"]
        self.assertFalse(safety["writes_gpu"])
        self.assertEqual(safety["may_be_staged"], "with -AcceptActuatingDeploy")
        builder = builder_text("maintenance")
        for token in ("MAINTENANCE_ONLY=1", "GPU_ACTION=0", "NVRAM_DELETE=EXPLICIT_REVIEW_ONLY",
                      "NO_BOOT_ENTRY_CHANGE=1", "NO_RESET=1", "HARDWARE_TESTED=0"):
            self.assertIn(token, variant["attestations"])
            self.assertIn(token, builder)
        gate = SCRIPT.index("if ($safety.may_be_staged -ne $true -and -not $AcceptActuatingDeploy)")
        maintenance = SCRIPT.index("if ($deployVariant.safety -eq 'maintenance')")
        self.assertLess(gate, maintenance)
        self.assertIn("Maintenance staging requires -Only maintenance -Operation ReviewBootState.", SCRIPT)
        stage = SCRIPT[SCRIPT.index("function Stage-SignedEfi"):SCRIPT.index("# --- catalogue")]
        self.assertLess(stage.index("if ($MaintenanceOnly -and $otherEfiCount -ne 0)"),
                        stage.index("$replacementStarted = $false"))
        self.assertLess(stage.index("Maintenance media is no longer isolated"),
                        stage.index("Copy-Item -LiteralPath $SignedFile -Destination $temporary"))
        self.assertIn("-MaintenanceOnly:($variant.safety -eq 'maintenance')", SCRIPT)

    def test_the_script_never_stages_from_a_matrix_run(self):
        self.assertIn("-All must not stage", SCRIPT)
        self.assertIn("-SignOnly and -StageDrive are mutually exclusive", SCRIPT)

    def test_the_script_refuses_a_dxe_as_a_boot_application(self):
        self.assertIn("A DXE boot-service driver cannot be staged as BOOTX64.EFI", SCRIPT)

    def test_actuating_one_target_can_stage_its_signed_dxe_companion_in_driver_path(self):
        self.assertIn("[switch]$StageDxeCompanion", SCRIPT)
        self.assertIn("-StageDxeCompanion requires firmware-transaction -Operation Menu", SCRIPT)
        self.assertIn("(Get-SelectionCount) -ne 1", SCRIPT)
        self.assertIn("Test-PairedTransactionVariant $variants[0] 'prepare'", SCRIPT)
        self.assertIn("Test-PairedTransactionVariant $_ 'prepare-dxe'", SCRIPT)
        self.assertIn("if ($companion.Count -ne 1)", SCRIPT)
        self.assertIn("if ($result.id -eq $dxeStageVariantId) { $result.destination = $dxeDeployment.Path }", SCRIPT)
        self.assertIn("if ($result.id -eq $stageVariantId) { $result.destination = $bootDeployment.Path }", SCRIPT)
        self.assertIn("$companion[0]) + @($variants)", SCRIPT)
        self.assertIn("Stage-SignedDxeCompanion", SCRIPT)
        self.assertIn("Join-Path $driversDir 'AdaUnlockDxe.efi'", SCRIPT)
        self.assertIn("EFI\\Drivers\\AdaUnlockDxe.efi", SCRIPT)
        self.assertIn("STAGED_DXE_COMPANION PATH=", SCRIPT)
        self.assertIn("$dxeStageVariantId -and $dxeStageVariantId -eq $variantId", SCRIPT)

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

    def test_disk_stage_is_explicit_and_non_destructive(self):
        self.assertIn("[switch]$StageDisk", SCRIPT)
        self.assertIn("[switch]$StageDrive", SCRIPT)
        self.assertIn("[Parameter(ValueFromRemainingArguments=$true)]", SCRIPT)
        self.assertIn("New-StagePartition $selectedStageDisk $PartitionSizeMB", SCRIPT)
        self.assertIn("$PSBoundParameters.ContainsKey('StageDisk')", SCRIPT)
        provision = SCRIPT.split('function New-StagePartition(', 1)[1].split('function Assert-UsbVolume(', 1)[0]
        for forbidden in ('Clear-Disk', 'Initialize-Disk', 'Resize-Partition', 'Remove-Partition'):
            self.assertNotIn(forbidden, provision)
        self.assertLess(provision.index('Read-Host'), provision.index('New-Partition -DiskId'))
        self.assertLess(provision.index('New-Partition -DiskId'), provision.index('Format-Volume -Partition $new[0]'))
        self.assertIn('(Get-StageLayoutKey $old) -cne $beforeKey', provision)
        self.assertIn('STAGE_PARTITION_GUID=$script:StagePartitionGuid', SCRIPT)
        self.assertIn("$arguments['StagePartitionGuid'] = $script:StagePartitionGuid", SCRIPT)

    def test_staging_requires_explicit_profile(self):
        self.assertIn("if (-not $Only) { throw 'Staging requires an explicit -Only <profile> request.' }", SCRIPT)
        self.assertNotIn("[string]$Deploy,", SCRIPT)

    def test_a_deploy_run_says_what_it_staged(self):
        self.assertIn('Say (("STAGED {0} PATH={1} DRIVE={2}:")', SCRIPT)
        self.assertIn("$stagedPath = $deployment.Path", SCRIPT)


    def test_firmware_asset_stage_is_restricted_to_firmware_transactions(self):
        self.assertIn("if ($deployVariant.profile -eq 'firmware-transaction')", SCRIPT)
        self.assertEqual(SCRIPT.count("-FirmwareAssetRepo $assetRepoForStage"), 2)
        stage = SCRIPT[SCRIPT.index("function Stage-SignedEfi"):SCRIPT.index("# --- catalogue")]
        self.assertLess(stage.index("Stage-FirmwareAssets -AssetRepo"), stage.index("$replacementStarted = $true"))

    def test_firmware_asset_paths_match_the_reference_verifier(self):
        import verify_reference_assets
        stage = SCRIPT.split("function Get-FirmwareAssetPlan {", 1)[1].split("function Stage-FirmwareAssets", 1)[0]
        paths = stage.split("$assetPaths = @(", 1)[1].split(")", 1)[0]
        self.assertEqual(set(re.findall(r"'([^']+\.bin)'", paths)), set(verify_reference_assets.PINS))
        provenance = json.loads((ROOT / "docs" / "asset_provenance.json").read_text(encoding="utf-8"))
        pins = {record["asset"].rsplit("/", 1)[-1]: (record["bytes"], record["sha256"])
                for record in provenance["records"]}
        for path, pin in verify_reference_assets.PINS.items():
            self.assertEqual(pins[Path(path).name], pin)



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


class CanonicalProfileResolution(unittest.TestCase):
    """Exercise the actual wrapper without compiling, signing or touching USB."""
    @classmethod
    def setUpClass(cls):
        import shutil
        cls.shell = shutil.which("pwsh") or shutil.which("powershell")
        if not cls.shell:
            raise unittest.SkipTest("PowerShell unavailable; profile resolution was not executed")

    def resolve(self, args, valid=True, script_path=SCRIPT_PATH):
        import subprocess
        # The command is composed only from fixed test arguments. -Command is
        # needed for explicit false switch values in Windows PowerShell 5.1.
        command = ("& '" + str(script_path).replace("'", "''") + "' -ResolveOnly -RepoRoot '"
                   + str(ROOT).replace("'", "''") + "' " + " ".join(args))
        result = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", command],
                                text=True, capture_output=True, timeout=30)
        if not valid:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertNotIn('"VariantId"', result.stdout)
            return None
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout.strip())


    def test_stage_target_binding_and_interactive_selection(self):
        import subprocess
        # Use the real parameter declaration and argument gates plus the
        # real selector. Stub storage and stop before any staging code.
        header = SCRIPT[SCRIPT.index("[CmdletBinding("):SCRIPT.index("$KeyInWsl =")]
        selector = SCRIPT[SCRIPT.index("function Resolve-StageTarget("):SCRIPT.index("# Disk provisioning is separate")]
        harness = header + r'''
function Say([string]$Text) { [Console]::Out.WriteLine($Text) }
function Get-Disk {
    param($ErrorAction)
    [pscustomobject]@{Number=0;FriendlyName='Test SATA';BusType='SATA';PartitionStyle='GPT';IsBoot=$false;IsSystem=$false;LogicalSectorSize=512;LargestFreeExtent=256MB}
    [pscustomobject]@{Number=3;FriendlyName='Test USB';BusType='USB';PartitionStyle='MBR';IsBoot=$false;IsSystem=$false;LogicalSectorSize=512;LargestFreeExtent=0}
}
function Get-Partition {
    param($ErrorAction)
    [pscustomobject]@{DiskNumber=0;DriveLetter='F'}
    [pscustomobject]@{DiskNumber=3;DriveLetter='G'}
    [pscustomobject]@{DiskNumber=3;DriveLetter='H'}
}
function Read-Host { param($Prompt) [Console]::Out.WriteLine("PROMPT=$Prompt"); return $env:GU_TEST_STAGE_INPUT }
''' + selector + r'''
if ($script:DiskStageRequested) {
    $chosen = Resolve-StageTarget $true $StageTargetArgument
    [Console]::Out.WriteLine("DISK=$chosen")
} elseif ($StageDrive) {
    $chosen = Resolve-StageTarget $false $StageTargetArgument
    [Console]::Out.WriteLine("DRIVE=$chosen")
} else { [Console]::Out.WriteLine('NO_STAGE') }
'''
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "selector.ps1"
            script.write_text(harness, encoding="utf-8")
            import os
            cases = [
                ("-StageDisk 0 -AcceptActuatingDeploy", "", "DISK=0", False),
                ("-StageDisk -AcceptActuatingDeploy", "0", "DISK=0", True),
                ("-AcceptActuatingDeploy -StageDisk", "3", "DISK=3", True),
                ("-StageDrive g -AcceptActuatingDeploy", "", "DRIVE=G", False),
                ("-StageDrive -AcceptActuatingDeploy", "h", "DRIVE=H", True),
                ("-AcceptActuatingDeploy -StageDrive", "G", "DRIVE=G", True),
                ("", "", "NO_STAGE", False),
                ("-StageDisk", "", None, True),
                ("-StageDisk", "99", None, True),
                ("-StageDisk", "-1", None, True),
                ("-StageDrive", "Z", None, True),
                ("-StageDrive", "C", None, True),
                ("-StageDrive GG", "", None, False),
                ("-StageDisk abc", "", None, False),
                ("-StageDisk 0 3", "", None, False),
                ("-StageDisk -StageDrive", "0", None, False),
                ("-StageDisk -ResolveOnly", "0", None, False),
                ("-StageDrive -ResolveOnly", "G", None, False),
                ("-StageDrive -List", "G", None, False),
                ("-StageDisk -Typo", "0", None, False),
                ("0", "", None, False),
            ]
            for options, answer, expected, prompt in cases:
                with self.subTest(options=options, answer=answer):
                    command = "& '" + str(script).replace("'", "''") + "' -Only diagnostic " + options
                    result = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", command],
                                            capture_output=True, text=True, timeout=30,
                                            env={**os.environ, "GU_TEST_STAGE_INPUT": answer})
                    if expected is None:
                        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn(expected, result.stdout)
                    self.assertEqual("PROMPT=" in result.stdout, prompt, result.stdout + result.stderr)
                    if prompt:
                        for name in ("Number", "FriendlyName", "BusType", "PartitionStyle", "IsBoot", "IsSystem", "LogicalSectorSize", "UnallocatedMiB"):
                            self.assertIn(name, result.stdout)
                        if "-StageDrive" in options:
                            self.assertIn("DriveLetter", result.stdout)
                            self.assertIn("G,H", result.stdout)

    def test_disk_provisioning_with_mock_storage_services(self):
        import subprocess
        functions = SCRIPT[SCRIPT.index("function Get-StageDiskIdentity("):SCRIPT.index("function Get-ExistingEfi(")]
        host = r'''
param([int]$Mode)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:ProvisionedStage = $null
$script:creates = 0
$script:formats = 0
$script:disk = [pscustomobject]@{ Number=0; UniqueId='disk-fixture'; SerialNumber='serial'; Size=10GB; LargestFreeExtent=2GB; LogicalSectorSize=512; BusType='NVMe'; PartitionStyle='GPT'; IsBoot=$false; IsSystem=$false; IsOffline=$false; IsReadOnly=$false; FriendlyName='fixture' }
$script:parts = @([pscustomobject]@{ Guid='11111111-1111-1111-1111-111111111111'; Offset=1MB; Size=1GB; PartitionNumber=1; DiskNumber=0; GptType='ebd0a0a2-b9e5-4433-87c0-68b6b72699c7'; IsSystem=$false; IsBoot=$false; DriveLetter='F' })
$script:newGuid = '22222222-2222-2222-2222-222222222222'
if ($Mode -ge 10) { $script:parts[0].GptType='c12a7328-f81f-11d2-ba4b-00a0c93ec93b'; $script:disk.LargestFreeExtent=0 }
if ($Mode -eq 2) { $script:disk.LargestFreeExtent=256MB }
if ($Mode -eq 3) { $script:disk.IsSystem=$true }
if ($Mode -eq 6) { $script:parts[0].GptType='c12a7328-f81f-11d2-ba4b-00a0c93ec93b' }
if ($Mode -eq 8) { $script:disk.PartitionStyle='RAW' }
function Get-Disk { param($Number,$ErrorAction) $script:disk | Select-Object * }
function Get-Partition { param($DiskNumber,$DriveLetter,$ErrorAction) if ($DriveLetter) { $script:parts | Where-Object DriveLetter -eq $DriveLetter } else { $script:parts | ForEach-Object { $_ | Select-Object * } } }
function Get-Volume { param($DriveLetter,$ErrorAction) [pscustomobject]@{FileSystem='FAT32';FileSystemLabel='GPUUNLOCK';Size=1GB;UniqueId='volume-fixture'} }
function Assert-NormalDirectory { param($Root) }
function Say([string]$Text) { [Console]::Out.WriteLine($Text) }
function Read-Host { param($Prompt) if ($Mode -eq 1 -or $Mode -eq 11) { return 'NO' }; if ($Mode -eq 4) { $script:disk.UniqueId='replacement' }; if ($Mode -eq 9 -or $Mode -eq 12) { $script:parts[0].Size=2GB }; if ($Mode -ge 10) { return 'REUSE 0 11111111-1111-1111-1111-111111111111' }; return 'CREATE 0 1024' }
function New-Partition {
    param($DiskId,$Size,$GptType,[switch]$AssignDriveLetter,$ErrorAction)
    $script:creates++
    if ($DiskId -ne 'disk-fixture' -or $Size -ne 1GB -or -not $AssignDriveLetter) { throw 'Unexpected create arguments' }
    $p = [pscustomobject]@{Guid=$script:newGuid;Offset=2GB;Size=$Size;PartitionNumber=2;DiskNumber=0;GptType=$GptType;IsSystem=$false;IsBoot=$false;DriveLetter='G'}
    if ($Mode -eq 5) { $script:parts[0].Size=2GB }
    $script:parts += $p
    return $p
}
function Format-Volume {
    param($Partition,$FileSystem,$AllocationUnitSize,$NewFileSystemLabel,$Confirm,$ErrorAction)
    $script:formats++
    if ($Partition.Guid -ne $script:newGuid -or $FileSystem -ne 'FAT32' -or $AllocationUnitSize -ne 512) { throw 'Unexpected format target' }
}
''' + functions + r'''
# Receipt and content matching is tested separately against files.
function Get-StageReuseEvidence { param($Disk,$Partition,$Root,$ReceiptDirectory) if ($Mode -lt 10 -or $Mode -eq 13) { throw 'No matching receipt' }; [pscustomobject]@{Hash='fixture-hash';Receipt='receipt-fixture'} }
$failed=$false
if ((Get-StagePartitionSizeMB 84585936 512 0) -ne 162 -or
    (Get-StagePartitionSizeMB 84585936 4096 0) -ne 272 -or
    (Get-StagePartitionSizeMB 0 512 0) -ne 64 -or
    (Get-StagePartitionSizeMB 84585936 512 200) -ne 200) { throw 'Unexpected automatic partition size' }
try { [void](Get-StagePartitionSizeMB 84585936 512 100); throw 'Undersized override accepted' } catch { if ($_.Exception.Message -notlike 'PartitionSizeMB must be at least*') { throw } }
try {
    $script:ProvisionedStage = New-StagePartition 0 1024 ($Mode -ne 7) 0 'receipt-directory'
    if ($Mode -eq 10) {
        if (-not $script:ProvisionedStage.Reused -or $script:ProvisionedStage.Guid -ne [guid]'11111111-1111-1111-1111-111111111111' -or (Assert-UsbVolume 'F') -ne 'F:\') { throw 'Invalid reuse identity' }
    }
    if ($Mode -eq 0) {
        if ($script:ProvisionedStage.Guid -ne [guid]$script:newGuid -or (Assert-UsbVolume 'G') -ne 'G:\') { throw 'Wrong stage identity' }
        $script:parts[1].Guid='33333333-3333-3333-3333-333333333333'
        try { [void](Assert-UsbVolume 'G'); throw 'Mismatched partition accepted' } catch { if ($_.Exception.Message -ne 'Provisioned stage partition identity changed.') { throw } }
    }
} catch { $failed=$true; [Console]::Out.WriteLine($_.Exception.Message) }
if ($Mode -eq 0) {
    if ($failed -or $script:creates -ne 1 -or $script:formats -ne 1) { throw 'Expected exact successful create/format' }
} elseif ($Mode -eq 10) {
    if ($failed -or $script:creates -ne 0 -or $script:formats -ne 0) { throw 'Verified reuse must not provision or format' }
} elseif ($Mode -eq 5) {
    if (-not $failed -or $script:creates -ne 1 -or $script:formats -ne 0) { throw 'Changed layout must prevent format' }
} else {
    if (-not $failed -or $script:creates -ne 0 -or $script:formats -ne 0) { throw 'Expected refusal before writes' }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "disk.ps1"
            script.write_text(host, encoding="utf-8")
            for mode in range(14):
                with self.subTest(mode=mode):
                    result = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-File", str(script), str(mode)],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.resolve(["-Only", "diagnostic", "-StageDisk", "0"], valid=False)
        self.resolve(["-Only", "diagnostic", "-PartitionSizeMB", "1024"], valid=False)

    def test_stage_reuse_requires_matching_local_deployment_evidence(self):
        import hashlib
        import struct
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            media = base / "media"
            boot = media / "EFI" / "BOOT" / "BOOTX64.EFI"
            boot.parent.mkdir(parents=True)
            output = base / "signed"
            output.mkdir()
            image = bytearray(512)
            struct.pack_into("<H", image, 0, 0x5A4D)
            struct.pack_into("<I", image, 0x3C, 0x80)
            struct.pack_into("<IH", image, 0x80, 0x4550, 0x8664)
            struct.pack_into("<H", image, 0x98, 0x20B)
            struct.pack_into("<H", image, 0x98 + 68, 10)
            boot.write_bytes(image)
            artifact = output / "UniversalGPU-fixture-signed.efi"
            artifact.write_bytes(image)
            receipt = output / (artifact.name + ".json")
            pin = "11111111-1111-1111-1111-111111111111"
            record = {
                "ProvisionedPartitionGuid": pin,
                "ProvisionedDiskUniqueId": "disk-fixture",
                "SignedSha256": hashlib.sha256(image).hexdigest(),
                "VerifiedBySbverify": True,
                "Destination": r"G:\EFI\BOOT\BOOTX64.EFI",
                "Subsystem": 10,
                "AttestationsVerified": ["STAGE_PARTITION_GUID=" + pin],
            }
            helpers = SCRIPT[SCRIPT.index("function Get-Sha256"):SCRIPT.index("# A switch plus one optional")]
            helpers += SCRIPT[SCRIPT.index("function Get-StageReuseEvidence("):SCRIPT.index("function Get-StageReuseTarget(")]
            helpers += SCRIPT[SCRIPT.index("function Get-ExistingEfi("):SCRIPT.index("function Get-DbCertificateDer")]
            harness = base / "reuse.ps1"
            harness.write_text(
                "param([string]$Media,[string]$Receipts)\nSet-StrictMode -Version Latest\n$ErrorActionPreference='Stop'\n"
                + helpers + "\n"
                "$disk=[pscustomobject]@{UniqueId='disk-fixture';BusType='NVMe'}\n"
                "$part=[pscustomobject]@{Guid='11111111-1111-1111-1111-111111111111'}\n"
                "$evidence=Get-StageReuseEvidence $disk $part $Media $Receipts\n"
                "if (-not $evidence.Hash) { throw 'Missing evidence' }\n",
                encoding="utf-8")
            def check(valid):
                result = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-File", str(harness), str(media), str(output)],
                                        text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode == 0, valid, result.stdout + result.stderr)
            check(False)
            receipt.write_text(json.dumps(record), encoding="utf-8")
            check(True)
            for key, value in (("ProvisionedPartitionGuid", "22222222-2222-2222-2222-222222222222"),
                               ("ProvisionedDiskUniqueId", "other-disk"), ("SignedSha256", "0" * 64),
                               ("VerifiedBySbverify", False), ("AttestationsVerified", []),
                               ("Destination", None), ("Subsystem", 11)):
                with self.subTest(field=key):
                    receipt.write_text(json.dumps(dict(record, **{key: value})), encoding="utf-8")
                    check(False)
            receipt.write_text(json.dumps(record), encoding="utf-8")
            artifact.write_bytes(b"changed")
            check(False)
            artifact.write_bytes(image)
            foreign = boot.parent / "foreign.efi"
            foreign.write_bytes(image)
            check(False)
            foreign.unlink()
            changed = bytearray(image)
            changed[-1] = 1
            boot.write_bytes(changed)
            check(False)
            boot.write_bytes(image)
            check(True)

    def test_firmware_asset_staging_on_temporary_media(self):
        import subprocess
        import verify_reference_assets
        with tempfile.TemporaryDirectory() as directory:
            media = Path(directory) / "media"
            media.mkdir()
            (media / "keep.txt").write_text("preserve", encoding="utf-8")
            harness = Path(directory) / "stage.ps1"
            helpers = SCRIPT[SCRIPT.index("function Get-Sha256"):SCRIPT.index("function Assert-EfiImage")]
            helpers += SCRIPT[SCRIPT.index("function Assert-FirmwareAssetFile"):SCRIPT.index("function Stage-SignedEfi")]
            harness.write_text(
                "param([string]$Repo, [string]$Media)\n"
                "$ErrorActionPreference = 'Stop'\nSet-StrictMode -Version Latest\n"
                "function Assert-UsbVolume([string]$Drive) { return $Media }\n"
                "function Get-Volume { param($DriveLetter, $ErrorAction) return [pscustomobject]@{ UniqueId = 'fixture' } }\n"
                "function Say([string]$Text) { [Console]::Out.WriteLine($Text) }\n"
                + helpers + "\n"
                "$plan = @(Get-FirmwareAssetPlan -AssetRepo $Repo)\n"
                "if (($plan | ForEach-Object { [int64]$_.Pin.bytes } | Measure-Object -Sum).Sum -ne 84585936) { throw 'Wrong bundle size' }\n"
                "$result = @(Stage-FirmwareAssets -AssetRepo $Repo -Root $Media -Drive X -ExpectedVolumeId fixture)\n"
                "if ($result.Count -ne 9) { throw 'Unexpected asset count' }\n",
                encoding="utf-8")
            command = [self.shell, "-NoProfile", "-NonInteractive", "-File", str(harness), str(ROOT), str(media)]
            result = subprocess.run(command, text=True, capture_output=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name, (size, digest) in verify_reference_assets.PINS.items():
                self.assertEqual(verify_reference_assets.check_file(media / name, size, digest), "MATCHED")
            self.assertFalse((media / "GA102-ASSETS" / "gsp_ga10x.bin").exists())
            result = subprocess.run(command, text=True, capture_output=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # A corrupt Ampere GSP would shadow the good shared copy. Do not replace it.
            bad = media / "GA102-ASSETS" / "gsp_ga10x.bin"
            bad.write_bytes(b"wrong")
            missing = media / "AD102-ASSETS" / "booter_load_ad102.bin"
            missing.unlink()
            result = subprocess.run(command, text=True, capture_output=True, timeout=90)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Firmware asset does not match the reference", result.stdout + result.stderr)
            self.assertFalse(missing.exists())
            self.assertEqual(bad.read_bytes(), b"wrong")
            self.assertEqual((media / "keep.txt").read_text(encoding="utf-8"), "preserve")
            self.assertFalse(list(media.rglob(".new-asset-*.tmp")))

    def test_canonical_configs_preserve_builder_contracts(self):
        one = ["-SelectionBdf", "'0000:01:00.0'"]
        two = ["-SelectionBdfs", "'0000:01:00.0,0000:04:00.0'"]
        esp = ["-WindowsEspGuid", "'a005858c-4487-4ee2-976e-1710ff484c31'"]
        cases = [
            ("diag-topology", ["diagnostic", "-SaveLog:$false"], []),
            ("diag-topology-log", ["diagnostic"], []),
            ("diag-topology-gop-text-cycle", ["diagnostic", "-Probe", "GopTextCycle"], []),
            ("diag-topology-handoff", ["diagnostic"], esp),
            ("diag-selection", ["diagnostic"], one),
            ("prep-selected", ["diagnostic", "-Probe", "Preflight"], one),
            ("prep-selected", ["diagnostic", "-Probe", "Preflight"], two + ["-CapturedPromReference"]),
            ("dxe-prep-selected", ["diagnostic", "-Probe", "Preflight", "-ImageKind", "Dxe"], two),
            ("compile-one-target", ["compile-check"], one),
            ("compile-sequence", ["compile-check"], two),
            ("dxe-compile-one-target", ["compile-check", "-ImageKind", "Dxe"], one),
            ("dxe-compile-sequence", ["compile-check", "-ImageKind", "Dxe"], two),
            ("app-stop-release-sequence-bench", ["driver-transition", "-Operation", "StopReattach"], []),
            ("app-stop-release-sequence-bench", ["driver-transition", "-Operation", "StopReattach"], two),
            ("app-stop-release-sequence-handoff", ["driver-transition", "-Operation", "StopReattach", "-Handoff", "WindowsAfterSuccess"], []),
            ("dxe-stop-release-sequence-bench", ["driver-transition", "-Operation", "StopReattach", "-ImageKind", "Dxe"], []),
            ("dxe-stop-release-sequence-bench", ["driver-transition", "-Operation", "StopReattach", "-ImageKind", "Dxe"], two),
            ("actuate-one-target", ["firmware-transaction", "-Operation", "Menu"], one),
            ("actuate-sequence", ["firmware-transaction", "-Operation", "Menu"], two),
            ("actuate-one-target-resident", ["firmware-transaction", "-Operation", "Menu", "-EspPolicy", "Resident"], one),
            ("actuate-runtime-selection", ["firmware-transaction", "-Operation", "Menu"], []),
            ("actuate-runtime-selection-resident", ["firmware-transaction", "-Operation", "Menu", "-EspPolicy", "Resident"], []),
            ("dxe-actuate-one-target", ["firmware-transaction", "-Operation", "DxeTransaction", "-ImageKind", "Dxe"], one),
            ("dxe-actuate-sequence", ["firmware-transaction", "-Operation", "DxeTransaction", "-ImageKind", "Dxe"], two),
            ("dxe-actuate-runtime-selection", ["firmware-transaction", "-Operation", "DxeTransaction", "-ImageKind", "Dxe"], []),
            ("app-firmware-reattach", ["firmware-transaction", "-Operation", "UnlockReattach"], []),
            ("app-firmware-reattach", ["firmware-transaction", "-Operation", "UnlockReattach"], esp),
            ("app-unlock", ["firmware-transaction", "-Operation", "Unlock"], []),
            ("app-unlock-with-resume", ["firmware-transaction", "-Operation", "UnlockWithResume"], []),
            ("app-install-driver", ["maintenance", "-Operation", "InstallDriver"], []),
            ("app-remove-driver", ["maintenance", "-Operation", "RemoveDriver"], []),
            ("boot-state-maintenance", ["maintenance", "-Operation", "ReviewBootState"], []),
            ("stop-dual-handoff", ["driver-transition", "-Operation", "ObserveStop", "-ObservationTarget", "BothNvidia"], esp),
        ]
        for suffix, probe in (("msi-rom", "MsiRom"), ("msi-fuse", "MsiFuse"),
                              ("msi-sec2", "MsiSec2"), ("vram-blocks", "VramBlocks")):
            cases.append(("diag-" + suffix, ["diagnostic", "-Probe", probe], []))
            cases.append(("diag-" + suffix + "-handoff", ["diagnostic", "-Probe", probe], esp))
        for card in ("4090", "3080"):
            opts = ["driver-transition", "-Operation", "ObserveStop", "-ObservationTarget", "RTX" + card]
            cases.append(("stop-" + card + "-once", opts, []))
            cases.append(("stop-" + card + "-handoff", opts, esp))
        self.assertEqual({alias for alias, _, _ in cases},
                         {v["id"] for v in MANIFEST["variants"]})
        variants = {v["id"]: v for v in MANIFEST["variants"]}
        import copy
        import shutil
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        fixture = Path(temporary.name)
        (fixture / "src").mkdir()
        (fixture / "docs").mkdir()
        renamed_script = fixture / "src" / SCRIPT_PATH.name
        shutil.copy2(SCRIPT_PATH, renamed_script)
        renamed = copy.deepcopy(MANIFEST)
        for variant in renamed["variants"]:
            variant["id"] = "rewritten-" + variant["id"]
        (fixture / "docs" / "image_variants.json").write_text(json.dumps(renamed), encoding="utf-8")
        for configuration, canonical, extra in cases:
            with self.subTest(configuration=configuration, canonical=canonical, extra=extra):
                new = self.resolve(["-Only"] + canonical + extra)
                expected = variants[configuration]
                self.assertEqual(new["CanonicalProfile"], canonical[0])
                self.assertEqual(new["RequestedCanonicalProfile"], canonical[0])
                self.assertEqual(new["VariantId"], configuration)
                self.assertEqual(new["Safety"], expected["safety"])
                self.assertEqual(new["Builder"], FAMILY[expected["family"]]["builder"])
                self.assertEqual(new["Subsystem"], FAMILY[expected["family"]]["subsystem"])
                self.assertEqual(new["ExpectedAttestations"], expected["attestations"])
                switches = {k: v for k, v in expected["switches"].items() if v is not False}
                guid = None
                if "-WindowsEspGuid" in extra:
                    guid = extra[extra.index("-WindowsEspGuid") + 1].strip("'")
                    switches["WindowsEspGuid"] = guid
                selection = []
                for key in ("SelectionBdf", "SelectionBdfs"):
                    if "-" + key in extra:
                        value = extra[extra.index("-" + key) + 1].strip("'")
                        switches[key] = value
                        selection = value.split(",")
                compiled_stop = bool(selection and switches.get("StopReleaseOnlyBench"))
                if compiled_stop:
                    switches.pop("RuntimeSelection")
                if "-CapturedPromReference" in extra:
                    switches["CapturedPromReference"] = True
                self.assertEqual(new["Switches"], switches)
                self.assertEqual(new["Selection"], selection)
                binary = expected_binary(expected)
                if compiled_stop:
                    suffix = "-DXE-" if expected["family"] == "prepare-dxe" else "-"
                    extension = ".efi" if expected["family"] == "prepare-dxe" else ".EFI"
                    binary = "GPU-SEQUENCE-STOP-RELEASE-BENCH" + suffix + label(len(selection)) + extension
                elif "<label>" in binary:
                    binary = binary.replace("<label>", label(len(selection)))
                self.assertEqual(new["Binary"], binary)
                attestations = [a.replace("<selection-count>", str(len(selection))) for a in expected["attestations"]]
                if compiled_stop:
                    attestations = [a.replace("RUNTIME_SELECTION=1", "RUNTIME_SELECTION=0").replace(
                        "TARGET_ORDER=RUNTIME_SUPPORTED_NVIDIA_ENUMERATION", "TARGET_ORDER=" + ",".join(selection))
                        for a in attestations]
                if switches.get("FirmwareReattach"):
                    attestations.append("WINDOWS_ESP_GUID=" + (guid or "NONE"))
                    attestations.append("STAGE_PARTITION_GUID=NONE")
                self.assertEqual(new["ResolvedAttestations"], attestations)
                self.assertNotIn("RequestedLegacyAlias", new)
                rewritten = self.resolve(["-Only"] + canonical + extra, script_path=renamed_script)
                self.assertEqual(rewritten.pop("VariantId"), "rewritten-" + configuration)
                self.assertEqual(rewritten, {key: value for key, value in new.items() if key != "VariantId"})

    def test_stop_selection_and_combined_esp_follow_builder_configuration(self):
        two = ["-SelectionBdfs", "'0000:04:00.0,0000:01:00.0'"]
        for kind, expected, runtime in (
            ("Application", "GPU-SEQUENCE-STOP-RELEASE-BENCH-0000-04-00-0--0000-01-00-0.EFI",
             "GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME.EFI"),
            ("Dxe", "GPU-SEQUENCE-STOP-RELEASE-BENCH-DXE-0000-04-00-0--0000-01-00-0.efi",
             "GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi"),
        ):
            args = ["-Only", "driver-transition", "-Operation", "StopReattach", "-ImageKind", kind]
            result = self.resolve(args + two)
            self.assertEqual(result["Binary"], expected)
            self.assertEqual(result["Selection"], ["0000:04:00.0", "0000:01:00.0"])
            self.assertNotIn("RuntimeSelection", result["Switches"])
            self.assertEqual(result["Switches"]["SelectionBdfs"], "0000:04:00.0,0000:01:00.0")
            self.assertIn("RUNTIME_SELECTION=0", result["ResolvedAttestations"])
            self.assertIn("TARGET_ORDER=0000:04:00.0,0000:01:00.0", result["ResolvedAttestations"])
            self.assertNotIn("RUNTIME_SELECTION=1", result["ResolvedAttestations"])
            result = self.resolve(args)
            self.assertEqual(result["Binary"], runtime)
            self.assertEqual(result["Selection"], [])
            self.assertTrue(result["Switches"]["RuntimeSelection"])
            self.assertIn("RUNTIME_SELECTION=1", result["ResolvedAttestations"])
            self.assertIn("TARGET_ORDER=RUNTIME_SUPPORTED_NVIDIA_ENUMERATION", result["ResolvedAttestations"])
        handoff = self.resolve(["-Only", "driver-transition", "-Operation", "StopReattach",
                                "-Handoff", "WindowsAfterSuccess"])
        self.assertEqual(handoff["Binary"], "GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-WINDOWS-HANDOFF.EFI")
        self.assertTrue(handoff["Switches"]["StopReleaseWindowsHandoff"])
        self.assertIn("OS_HANDOFF=WINDOWS_AFTER_SUCCESS", handoff["ResolvedAttestations"])
        self.resolve(["-Only", "app-stop-release-sequence-handoff"] + two, valid=False)
        self.resolve(["-Only", "driver-transition", "-Operation", "StopReattach",
                      "-Handoff", "WindowsAfterSuccess"] + two, valid=False)
        guid = "a005858c-4487-4ee2-976e-1710ff484c31"
        combined = ["-Only", "firmware-transaction", "-Operation", "UnlockReattach"]
        result = self.resolve(combined + ["-WindowsEspGuid", guid])
        self.assertEqual(result["Switches"]["WindowsEspGuid"], guid)
        self.assertIn("WINDOWS_ESP_GUID=" + guid, result["ResolvedAttestations"])
        result = self.resolve(combined)
        self.assertNotIn("WindowsEspGuid", result["Switches"])
        self.assertIn("WINDOWS_ESP_GUID=NONE", result["ResolvedAttestations"])
        for operation in ("Unlock", "UnlockWithResume"):
            args = ["-Only", "firmware-transaction", "-Operation", operation]
            result = self.resolve(args)
            self.assertNotIn("WindowsEspGuid", result["Switches"])
            self.assertFalse(any(a.startswith("WINDOWS_ESP_GUID=") for a in result["ResolvedAttestations"]))
            self.resolve(args + ["-WindowsEspGuid", guid], valid=False)

    def test_paired_transaction_predicate_accepts_only_original_pair(self):
        import subprocess
        script = str(SCRIPT_PATH).replace("'", "''")
        manifest = str(ROOT / "docs" / "image_variants.json").replace("'", "''")
        # Load only the two pure functions from the actual wrapper. No build,
        # signing, USB discovery or staging code is executed.
        command = r"""
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile('SCRIPT_PATH', [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Wrapper parse failed' }
foreach ($name in @('Test-VariantSwitch', 'Test-PairedTransactionVariant')) {
    $functions = @($ast.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true))
    if ($functions.Count -ne 1) { throw "Missing unique function $name" }
    . ([scriptblock]::Create($functions[0].Extent.Text))
}
$catalogue = Get-Content -LiteralPath 'MANIFEST_PATH' -Raw | ConvertFrom-Json
$results = foreach ($variant in $catalogue.variants) {
    $id = $variant.id
    $app = Test-PairedTransactionVariant $variant 'prepare'
    $dxe = Test-PairedTransactionVariant $variant 'prepare-dxe'
    $variant.id = 'renamed-' + $id
    [ordered]@{ id = $id; app = $app; dxe = $dxe;
                renamedApp = (Test-PairedTransactionVariant $variant 'prepare');
                renamedDxe = (Test-PairedTransactionVariant $variant 'prepare-dxe') }
}
ConvertTo-Json -InputObject @($results) -Compress
""".replace("SCRIPT_PATH", script).replace("MANIFEST_PATH", manifest)
        result = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", command],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = json.loads(result.stdout)
        self.assertEqual({r["id"] for r in rows}, {v["id"] for v in MANIFEST["variants"]})
        self.assertEqual(len(rows), 41)
        self.assertEqual([r["id"] for r in rows if r["app"]], ["actuate-one-target"])
        self.assertEqual([r["id"] for r in rows if r["dxe"]], ["dxe-actuate-one-target"])
        for row in rows:
            self.assertEqual(row["app"], row["renamedApp"], row["id"])
            self.assertEqual(row["dxe"], row["renamedDxe"], row["id"])

    def test_missing_and_ambiguous_build_contracts_refuse_resolution(self):
        import copy
        import shutil
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "docs").mkdir()
            script = root / "src" / SCRIPT_PATH.name
            shutil.copy2(SCRIPT_PATH, script)
            manifest = root / "docs" / "image_variants.json"
            original = next(v for v in MANIFEST["variants"] if v["id"] == "diag-topology-log")
            for defect in ("missing", "duplicate", "safety", "selection", "esp", "local", "switch"):
                with self.subTest(defect=defect):
                    data = copy.deepcopy(MANIFEST)
                    target = next(v for v in data["variants"] if v["id"] == original["id"])
                    if defect == "missing":
                        data["variants"].remove(target)
                    elif defect == "duplicate":
                        duplicate = copy.deepcopy(target)
                        duplicate["id"] = "duplicate-build"
                        data["variants"].append(duplicate)
                    elif defect == "safety":
                        target["safety"] = "actuates"
                    elif defect == "selection":
                        target["requires_selection"] = "single"
                    elif defect == "esp":
                        target["requires_esp_guid"] = True
                    elif defect == "local":
                        target["requires_local_boot_profile"] = True
                    else:
                        target["switches"]["CycleGopText"] = True
                    manifest.write_text(json.dumps(data), encoding="utf-8")
                    self.resolve(["-Only", "diagnostic"], valid=False, script_path=script)

    def test_configuration_ids_are_not_public_selectors(self):
        for variant in MANIFEST["variants"]:
            with self.subTest(configuration=variant["id"]):
                self.resolve(["-Only", variant["id"]], valid=False)
        self.resolve(["-Only", "diagnostic,compile-check"], valid=False)
        self.resolve(["-Only", "diagnostic", "-ListLegacy"], valid=False)
        self.resolve(["-Only", "diagnostic", "-Deploy", "diag-topology-log"], valid=False)

    def test_generated_profile_summary_matches_manifest(self):
        generated = renderer.render(MANIFEST)
        self.assertIn("5 canonical profiles. 41 build configurations", generated)
        expected = {"diagnostic": 15, "driver-transition": 8, "firmware-transaction": 11,
                    "maintenance": 3, "compile-check": 4}
        for profile, count in expected.items():
            self.assertEqual(sum(v["profile"] == profile for v in MANIFEST["variants"]), count)
            self.assertIn(f"| `{profile}` | {count} |", generated)

    def test_public_listing_has_five_profiles_and_configuration_listing_has_all_configs(self):
        import subprocess
        command = "& '" + str(SCRIPT_PATH).replace("'", "''") + "' "
        expected = {"diagnostic", "driver-transition", "firmware-transaction", "maintenance", "compile-check"}
        self.assertEqual({p["id"] for p in MANIFEST["profiles"]}, expected)
        self.assertEqual(len(MANIFEST["profiles"]), 5)
        for option in ("-List", "-ListConfigurations"):
            result = subprocess.run([self.shell, "-NoProfile", "-NonInteractive", "-Command", command + option],
                                    text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            profiles = re.findall(r"^PROFILE (\S+)", result.stdout, re.M)
            variants = re.findall(r"^VARIANT (\S+) PROFILE=(\S+)", result.stdout, re.M)
            self.assertEqual(len(profiles), 5)
            self.assertEqual(set(profiles), expected)
            if option == "-List":
                self.assertEqual(variants, [])
            else:
                self.assertEqual(len(variants), len(MANIFEST["variants"]))
                self.assertEqual(dict(variants), {v["id"]: v["profile"] for v in MANIFEST["variants"]})

    def test_receipt_keeps_verified_values_separate_from_templates(self):
        self.assertIn("$verifiedAttestations += $attestation", SCRIPT)
        self.assertIn('"WINDOWS_ESP_GUID=$expectedEsp"', SCRIPT)
        self.assertIn('foreach ($attestation in @(Get-ResolvedAttestations $variant))', SCRIPT)
        self.assertIn('ResolvedAttestations = @(Get-ResolvedAttestations $resolvedVariant)', SCRIPT)
        self.assertIn("ExpectedAttestations = @($variant.attestations)", SCRIPT)
        self.assertIn("AttestationsVerified = $verifiedAttestations", SCRIPT)
        self.assertIn("CanonicalProfile = $variant.profile", SCRIPT)
        self.assertIn("HardwareVerified = $false", SCRIPT)

    def test_documented_preflight_reference_resolves(self):
        result = self.resolve(["-Only", "diagnostic", "-Probe", "Preflight",
                               "-SelectionBdfs", "'0000:01:00.0,0000:04:00.0'",
                               "-CapturedPromReference", "-SignOnly"])
        self.assertEqual(result["VariantId"], "prep-selected")
        self.assertEqual(result["Builder"], "src/Build-Experimental-MSVC.ps1")
        self.assertTrue(result["Switches"]["CapturedPromReference"])

    def test_resolution_validates_selection_and_predicts_output(self):
        result = self.resolve(["-Only", "compile-check", "-SelectionBdfs",
                               "'0000:01:00.0,0000:0a:00.0'"])
        self.assertEqual(result["Selection"], ["0000:01:00.0", "0000:0A:00.0"])
        self.assertEqual(result["Binary"], "GPU-SEQUENCE-COMPILE-ONLY-0000-01-00-0--0000-0A-00-0.EFI")
        result = self.resolve(["-Only", "diagnostic"])
        self.assertEqual(result["Selection"], [])
        self.assertEqual(result["Binary"], "GPU-TOPOLOGY-DIAGNOSTIC.EFI")
        result = self.resolve(["-Only", "driver-transition", "-Operation", "StopReattach",
                               "-SelectionBdf", "'0000:04:00.0'"])
        self.assertEqual(result["Selection"], ["0000:04:00.0"])
        self.assertEqual(result["Binary"], "GPU-SEQUENCE-STOP-RELEASE-BENCH-0000-04-00-0.EFI")
        self.assertIn("RUNTIME_SELECTION=0", result["ResolvedAttestations"])
        self.assertIn("TARGET_ORDER=0000:04:00.0", result["ResolvedAttestations"])
        self.assertNotIn("RUNTIME_SELECTION=1", result["ResolvedAttestations"])
        result = self.resolve(["-Only", "compile-check", "-SelectionBdfs",
                               "'0000:01:00.0,0000:04:00.0'"])
        self.assertIn("SELECTED_COUNT=2", result["ResolvedAttestations"])
        self.assertTrue(all("<" not in value for value in result["ResolvedAttestations"]))
        result = self.resolve(["-Only", "firmware-transaction", "-Operation", "UnlockReattach"])
        self.assertIn("WINDOWS_ESP_GUID=NONE", result["ResolvedAttestations"])
        result = self.resolve(["-Only", "firmware-transaction", "-Operation", "UnlockReattach",
                               "-WindowsEspGuid", "'A005858C-4487-4EE2-976E-1710FF484C31'"])
        self.assertIn("WINDOWS_ESP_GUID=a005858c-4487-4ee2-976e-1710ff484c31",
                      result["ResolvedAttestations"])
        for args in (
            ["-Only", "compile-check", "-SelectionBdf", "'bad'"],
            ["-Only", "compile-check", "-SelectionBdf", "'0000:01:20.0'"],
            ["-Only", "compile-check", "-SelectionBdfs", "'0000:01:00.0,0000:01:00.0'"],
            ["-Only", "compile-check", "-SelectionBdfs", "'0000:01:00.0,'"],
            ["-Only", "compile-one-target", "-SelectionBdfs", "'0000:01:00.0,0000:04:00.0'"],
            ["-Only", "diagnostic", "-WindowsEspGuid", "'bad'"],
            ["-Only", "diagnostic", "-WindowsEspGuid", "'00000000-0000-0000-0000-000000000000'"],
        ):
            with self.subTest(args=args):
                self.resolve(args, valid=False)

    def test_invalid_combinations_refuse_resolution(self):
        one = ["-SelectionBdf", "'0000:01:00.0'"]
        esp = ["-WindowsEspGuid", "'a005858c-4487-4ee2-976e-1710ff484c31'"]
        cases = [
            ["-Only", "diagnostic", "-Probe", "GopTextCycle"] + esp,
            ["-Only", "diagnostic", "-Probe", "MsiRom"] + one,
            ["-Only", "diagnostic", "-SaveLog:$false"] + one,
            ["-Only", "diagnostic", "-Operation", "Unlock"],
            ["-Only", "diagnostic", "-Probe", "Preflight"],
            ["-Only", "diagnostic", "-Probe", "Preflight"] + one + esp,
            ["-Only", "diagnostic", "-Probe", "Preflight", "-SaveLog:$false"] + one,
            ["-Only", "diagnostic", "-Probe", "Preflight", "-ImageKind", "Dxe", "-CapturedPromReference"] + one,
            ["-Only", "compile-check"],
            ["-Only", "compile-check"] + one + esp,
            ["-Only", "driver-transition"],
            ["-Only", "driver-transition", "-Operation", "StopReattach", "-Handoff", "WindowsAfterSuccess"] + one,
            ["-Only", "driver-transition", "-Operation", "ObserveStop", "-ObservationTarget", "BothNvidia"],
            ["-Only", "firmware-transaction"],
            ["-Only", "firmware-transaction", "-Operation", "UnlockReattach", "-EspPolicy", "Pinned"],
            ["-Only", "firmware-transaction", "-Operation", "UnlockWithResume"] + esp,
            ["-Only", "maintenance", "-Operation", "ReviewBootState"] + one,
            ["-Only", "maintenance", "-Operation", "ReviewBootState", "-StageDrive", "G"],
            ["-Only", "diag-topology", "-Probe", "Topology"],
        ]
        for args in cases:
            with self.subTest(args=args):
                self.resolve(args, valid=False)


if __name__ == "__main__":
    unittest.main()


class TheReviewedProfileSkipSaysWhatToCreate(unittest.TestCase):
    """`NEEDS_LOCAL_BOOT_PROFILE` is the one skip that is a setup step.

    A waived image compiles `src/boot_profile.h`, which includes
    `src/local/boot_profile.h`. That directory is gitignored and per machine, so
    a fresh checkout cannot build an actuating image - and an operator who only
    sees the skip reason has nowhere to go. The failure names the file, says what
    goes in it, and says why `-WindowsEspGuid` is not a substitute.
    """

    def setUp(self):
        self.script = (ROOT / "src" / "Build-Sign-Deploy-UniversalGPU.ps1").read_text(encoding="utf-8")

    def test_the_failure_names_the_file_to_create(self):
        self.assertIn("REMEDY=create src\\local\\boot_profile.h from src\\boot_profile.example.h", self.script)

    def test_the_failure_says_the_directory_is_per_machine(self):
        self.assertIn("gitignored", self.script)
        self.assertIn("-WindowsEspGuid does not", self.script)

    def test_the_failure_carries_the_check_that_confirms_it(self):
        self.assertIn("VERIFY=python3 tools\\session_preflight.py --profile src\\local\\boot_profile.h", self.script)

    def test_the_remedy_is_only_attached_to_that_skip(self):
        block = self.script[self.script.index("if ($skipReason -eq 'NEEDS_LOCAL_BOOT_PROFILE') {"):]
        block = block[:block.index("throw $message")]
        self.assertEqual(block.count("REMEDY="), 1)
        for other in ("NEEDS_SELECTION", "NEEDS_ESP_GUID", "NEEDS_EXACTLY_ONE_BDF"):
            self.assertNotIn(other, block)

    def test_the_document_walks_through_it(self):
        doc = (ROOT / "docs" / "BUILD_SIGN_DEPLOY.md").read_text(encoding="utf-8")
        self.assertIn("### When a variant asks for the reviewed ESP profile", doc)
        for token in ("NEEDS_LOCAL_BOOT_PROFILE", "src\\local\\boot_profile.h",
                      "boot_profile.example.h", "MSFT_Partition",
                      "GUID=REVIEWED", "0x11111111"):
            self.assertIn(token, doc, token)

    def test_the_document_says_a_guid_is_reread_and_not_assumed(self):
        doc = (ROOT / "docs" / "BUILD_SIGN_DEPLOY.md").read_text(encoding="utf-8")
        section = doc[doc.index("### When a variant asks for the reviewed ESP profile"):]
        section = section[:section.index("## 3. Commands")]
        self.assertIn("re-partition changes it", section)
        self.assertIn("M1_RECOVERY_TOOLKIT_READY", section)

    def test_explicit_guid_is_exclusive_to_combined_runtime_application(self):
        builder = (ROOT / "src" / "Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
        self.assertIn("if (-not $FirmwareReattach -or -not $RuntimeEsp) { throw '-WindowsEspGuid", builder)
        self.assertLess(builder.index("if (-not $FirmwareReattach -or -not $RuntimeEsp)"),
                        builder.index("Push-Location"))
        self.assertIn("if ((Test-VariantSwitch $Variant 'FirmwareReattach') -and $WindowsEspGuid)", SCRIPT)
        self.assertEqual([v["id"] for v in MANIFEST["variants"] if v["switches"].get("FirmwareReattach")],
                         ["app-firmware-reattach"])
        dxe = (ROOT / "src" / "Build-Dxe-Driver.ps1").read_text(encoding="utf-8")
        self.assertNotIn("WindowsEspGuid", dxe)
        profile = (ROOT / "src" / "boot_profile.h").read_text(encoding="utf-8")
        self.assertIn('#include "local/boot_profile.h"', profile)
        for variant in MANIFEST["variants"]:
            if variant.get("requires_local_boot_profile"):
                self.assertNotIn("WindowsEspGuid", variant["switches"], variant["id"])
                self.assertNotEqual(variant["id"], "app-firmware-reattach")

    def test_the_synthetic_guid_is_still_refused_by_the_preflight(self):
        import session_preflight
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "boot_profile.h"
            path.write_text("#define GU_WINDOWS_PARTITION_INITIALIZER {0x11111111,0x2222,0x3333,"
                            "{0x84,0x44,0x55,0x66,0x77,0x88,0x99,0xaa}}\n", encoding="utf-8")
            ok, note = session_preflight.check_profile(path)
        self.assertFalse(ok)
        self.assertIn("GUID=SYNTHETIC", note)


class TheCapturedReferenceIsAnExplicitBuildOption(unittest.TestCase):
    """The two captured references ride a switch, not a variant.

    A variant that differed only by a define would produce the same file name as
    the variant it differs from, and two catalogue entries writing one path is a
    run that signs an image it cannot name. So the reference is a switch on the
    build, restricted to the family whose sources link the table.
    """

    BUILDER = "Build-Experimental-MSVC.ps1"

    def test_the_switch_exists_on_both_scripts(self):
        self.assertIn("[switch]$CapturedPromReference", SCRIPT)
        builder = (ROOT / "src" / self.BUILDER).read_text(encoding="utf-8")
        self.assertIn("[switch]$CapturedPromReference", builder)
        self.assertIn("/DGU_PROM_REFERENCE_CAPTURED", builder)

    def test_only_the_prepare_family_can_carry_it(self):
        guard = SCRIPT[SCRIPT.index("if ($CapturedPromReference) {"):]
        guard = guard[:guard.index("$arguments['CapturedPromReference'] = $true")]
        self.assertIn("$Variant.family -ne 'prepare'", guard)
        self.assertIn("throw", guard)
        # And it lands in the arguments the receipt records under Switches.
        self.assertIn("Switches = $buildArguments", SCRIPT)

    def test_a_matrix_run_refuses_it(self):
        # -All would pass the define to families whose sources never read a PROM.
        guard = SCRIPT[SCRIPT.index("if ($CapturedPromReference -and -not $Only) {"):]
        self.assertIn("throw", guard[:guard.index("\n    }")])

    def test_no_variant_carries_it(self):
        for variant in MANIFEST["variants"]:
            self.assertNotIn("CapturedPromReference", variant["switches"], variant["id"])

    def test_the_run_says_it_is_carrying_one(self):
        self.assertIn("PROM_REFERENCE=CAPTURED", SCRIPT)
        line = SCRIPT[SCRIPT.index("PROM_REFERENCE=CAPTURED"):]
        line = line[:line.index("\n")]
        self.assertIn("AUTHENTICATION=0", line,
                      "the line must not let a reference read as authentication")

    def test_the_documented_example_uses_a_real_variant(self):
        example = r".\src\Build-Sign-Deploy-UniversalGPU.ps1 -Only diagnostic -Probe Preflight -SelectionBdfs '0000:01:00.0,0000:04:00.0' -CapturedPromReference -SignOnly"
        self.assertIn(example, DOC.splitlines())
        variant = next(v for v in MANIFEST["variants"] if v["id"] == "prep-selected")
        self.assertEqual(variant["profile"], "diagnostic")
        self.assertEqual(variant["family"], "prepare")
