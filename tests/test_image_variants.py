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

    def test_the_script_states_what_signing_does_not_prove(self):
        self.assertIn("HARDWARE_VERIFIED=0", SCRIPT)
        self.assertIn("HardwareVerified = $false", SCRIPT)
        self.assertIn("Windows61742BooterProven = $false", SCRIPT)

    def test_no_variant_claims_a_hardware_result(self):
        text = json.dumps(MANIFEST).lower()
        for claim in ('"hardware_verified": true', '"verified": true', '"safe": true'):
            self.assertNotIn(claim, text)


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
