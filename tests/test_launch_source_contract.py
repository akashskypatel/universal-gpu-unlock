"""A resident image is a launch relaxation, not an actuation one.

`src/launch_source.c` decides where the image may start from; the C test proves the
decision. These checks prove the wiring kept every other guarantee:

  1. the default build still refuses a non-USB launch, and the refusal names the
     reason instead of failing silently;
  2. a resident build must identify its ESP at runtime - the C preprocessor
     refuses `GU_ESP_RESIDENT` without `GU_RUNTIME_ESP`, and both builders refuse
     the flag combination before compiling;
  3. the install step still requires removable media, so a resident image cannot
     stage a bundle onto the volume it is running from;
  4. the resident launch is gated on the staged bundle record and a writable log
     directory, and says so in one line that also states the actuation gates are
     unchanged.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOOT = (ROOT / "src" / "boot_io.c").read_text(encoding="utf-8")
MODULE = (ROOT / "src" / "launch_source.c").read_text(encoding="utf-8")
RUNNER = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")
BUILDERS = ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1")


class TheDefaultBuildIsUnchanged(unittest.TestCase):
    def test_a_non_usb_launch_still_loses(self):
        self.assertIn("GU_LAUNCH_OBSERVATION launch={0};", BOOT)
        self.assertIn("if(GuLaunchSourceDecide(&launch)!=GU_LAUNCH_ALLOW_RESIDENT) {", BOOT)
        self.assertIn("return EFI_ACCESS_DENIED;", BOOT)

    def test_the_build_flag_is_the_only_thing_that_can_admit_a_resident_launch(self):
        # Without -EspResident the observation says so, and the module refuses.
        self.assertRegex(BOOT, r"#else\s+launch\.EspResidentBuild=GU_LS_FALSE;\s+#endif")
        self.assertIn("if(!observation->EspResidentBuild) return GU_LAUNCH_REASON_BUILD_REFUSES;", MODULE)

    def test_the_refusal_names_the_reason(self):
        self.assertIn('GuLog("LAUNCH_SOURCE DECISION=%s REASON=%s REMOVABLE=%u PARTITION=%u "', BOOT)


class AResidentImageIdentifiesItsOwnEsp(unittest.TestCase):
    def test_the_preprocessor_refuses_a_pinned_resident_build(self):
        self.assertIn("#error GU_ESP_RESIDENT requires GU_RUNTIME_ESP", BOOT)

    def test_both_builders_refuse_the_flag_combination_before_compiling(self):
        for builder in BUILDERS:
            text = (ROOT / "src" / builder).read_text(encoding="utf-8-sig")
            self.assertIn("[switch]$EspResident", text, builder)
            self.assertIn("Assert-GuExecutionBuildOptions ", text, builder)
            self.assertLess(text.index("Assert-GuExecutionBuildOptions "),
                            text.index("Resolve-GuVcVars "), builder)
            self.assertLess(text.index("Assert-GuExecutionBuildOptions "),
                            text.index("New-GuGpuSelectionSpec "), builder)
            self.assertIn("-EspResident:$EspResident", text, builder)
            self.assertIn("-RuntimeEsp:$RuntimeEsp", text, builder)
            self.assertIn("-SyntheticCompileProfile:$SyntheticCompileProfile", text, builder)
            self.assertIn("/DGU_ESP_RESIDENT", text, builder)
        options = (ROOT / "src" / "Gpu-Execution-Sources.ps1").read_text(encoding="utf-8")
        self.assertIn("if ($EspResident -and -not $RuntimeEsp) {", options)
        self.assertIn("'-EspResident requires -RuntimeEsp", options)
        self.assertIn("if ($EspResident -and $SyntheticCompileProfile) {", options)
        self.assertIn("'-EspResident cannot be combined with -SyntheticCompileProfile", options)

    def test_the_resident_written_line_says_the_gates_are_unchanged(self):
        self.assertIn("LAUNCH_SOURCE=ESP_RESIDENT_WINDOWS_ESP", BOOT)
        self.assertIn("ACTUATION_GATES_UNCHANGED=1", BOOT)


class InstallingStillNeedsTheStick(unittest.TestCase):
    def test_the_install_step_still_refuses_a_resident_launch(self):
        stage = BOOT[BOOT.index("EFI_STATUS GuAssetInstallStage(VOID) {"):]
        self.assertIn("if(!BootFromUsb) {", stage)
        self.assertIn("REASON=IMAGE_NOT_ON_REMOVABLE_MEDIA", stage)

    def test_booting_from_the_stick_is_what_sets_boot_from_usb(self):
        self.assertEqual(BOOT.count("BootFromUsb=TRUE;"), 1)
        self.assertIn("if(usb) {\n    BootFromUsb=TRUE;", BOOT)


class TheResidentGatesAreTheDocumentedOnes(unittest.TestCase):
    def test_the_bundle_record_and_the_log_are_both_required(self):
        self.assertIn("launch.BundleRecord=StagedBundleRecordUsable()", BOOT)
        self.assertIn("launch.LogWritable=GU_LS_TRUE;", BOOT)
        self.assertIn("if(!observation->BundleRecord) return GU_LAUNCH_REASON_BUNDLE_RECORD;", MODULE)
        self.assertIn("if(!observation->LogWritable) return GU_LAUNCH_REASON_LOG_NOT_WRITABLE;", MODULE)

    def test_the_loader_check_is_the_one_that_already_existed(self):
        # No second definition of "does this volume carry Windows".
        # One definition and one forward declaration, not a copy of the loader
        # probe inside the resident path.
        self.assertEqual(
            BOOT.count("static BOOLEAN EspCarriesWindowsManager(EFI_BOOT_SERVICES *b,EFI_HANDLE handle) {"), 1)
        self.assertEqual(
            BOOT.count("static BOOLEAN EspCarriesWindowsManager(EFI_BOOT_SERVICES *b,EFI_HANDLE handle);"), 1)

    def test_every_refusal_has_a_name(self):
        reasons = set(re.findall(r"GU_LAUNCH_REASON_([A-Z_]+)\b", MODULE))
        named = set(re.findall(r'case GU_LAUNCH_REASON_([A-Z_]+): return "', MODULE))
        self.assertEqual(reasons, named,
                         f"unnamed: {sorted(reasons - named)}, stale: {sorted(named - reasons)}")
        self.assertIn("NONE", reasons)

    def test_the_module_is_compiled_into_every_image(self):
        self.assertIn('"launch_source.c"', RUNNER)
        self.assertIn('"launch_source.c"',
                      (ROOT / "tools" / "waiver_build_diff.py").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
