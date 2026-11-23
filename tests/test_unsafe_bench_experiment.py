"""Contracts for the explicitly opted-in, unverified DXE bench variant."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


class UnsafeBenchVariantContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = (SRC / "port.h").read_text(encoding="utf-8")
        cls.checks = (SRC / "checks.c").read_text(encoding="utf-8")
        cls.release = (SRC / "msi_release.c").read_text(encoding="utf-8")
        cls.boot = (SRC / "boot_io.c").read_text(encoding="utf-8")
        cls.dxe = (SRC / "dxe_main.c").read_text(encoding="utf-8")
        cls.builder = (SRC / "Build-Dxe-Driver.ps1").read_text(encoding="utf-8")
        cls.app_builder = (SRC / "Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
        cls.app = (SRC / "app.c").read_text(encoding="utf-8")
        cls.execution = (SRC / "unlock_execution.c").read_text(encoding="utf-8")
        cls.port_source = (SRC / "port.c").read_text(encoding="utf-8")
        cls.sign_deploy = (SRC / "Build-Sign-Deploy-UniversalGPU.ps1").read_text(encoding="utf-8")
        cls.selection_builder = (SRC / "Gpu-Selection-Build.ps1").read_text(encoding="utf-8")
        cls.catalog = json.loads((ROOT / "docs/image_variants.json").read_text(encoding="utf-8"))

    def test_plural_selection_argument_accepts_a_single_bdf(self):
        self.assertIn("[ValidateRange(1,128)][int]$MaximumTargets", self.selection_builder)
        self.assertIn("$requested.Count -lt 1", self.selection_builder)
        self.assertIn("requires 1-$MaximumTargets comma-separated PCI locations", self.selection_builder)

    def test_override_requires_explicit_ordered_unverified_actuation_and_file_only(self):
        block = self.port.split("#if defined(GU_UNVERIFIED_BENCH_CONDITIONS)", 1)[1].split("#endif", 1)[0]
        self.assertIn("GU_ACTIVE_UNVERIFIED_ACTUATION", block)
        self.assertIn("GU_ACTIVE_SINGLE_TARGET", block)
        self.assertIn("GU_DXE_FILE_ONLY_OUTPUT", self.port)

    def test_only_aspm_admission_is_waived_in_the_named_experiment(self):
        accepted = self.checks.split("BOOLEAN GuLinkAccepted(", 1)[1].split("const char *GuLinkReason", 1)[0]
        self.assertIn("#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) || defined(GU_STOP_RELEASE_ONLY_BENCH)", accepted)
        self.assertIn("Verdict == GU_LINK_OK || Verdict == GU_LINK_ASPM_ENABLED", accepted)
        self.assertIn("return Verdict == GU_LINK_OK;", accepted)
        self.assertIn("if ((Link.errors & 0x06U) != 0) return GU_LINK_ERROR_LOGGED;", self.checks)
        self.assertIn("if (!Link.trained) return GU_LINK_NOT_TRAINED;", self.checks)

    def test_console_ownership_waiver_is_compile_time_scoped_and_logged(self):
        for fn, marker in (("ReadOnlyAmdConsoleOwnership", "INDEPENDENT_AMD_CONSOLE"),
                           ("ReadOnlyAmdConsoleGraph", "LIVE_AMD_TEXTOUT_GRAPH")):
            body = self.release.split(f"static EFI_STATUS {fn}", 1)[1]
            self.assertIn("#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) && !defined(GU_STOP_RELEASE_ONLY_BENCH)", body)
            self.assertIn(f"GATE={marker} RESULT=BYPASSED", body)
        stop = self.release.split("static void StopContractLog", 1)[1].split("EFI_STATUS GuMsiRelease", 1)[0]
        self.assertIn("Console=FALSE", stop)

    def test_file_only_mode_suppresses_image_screen_messages_but_keeps_file_log(self):
        for fn in ("VOID GuBootSay", "static VOID TraceSay"):
            body = self.boot.split(fn, 1)[1].split("\n}", 1)[0]
            self.assertIn("GU_DXE_FILE_ONLY_OUTPUT", body)
        self.assertIn("Log->Write(Log,&bytes,buffer)", self.boot)
        self.assertIn("Log->Flush(Log)", self.boot)
        self.assertIn("SCREEN_OUTPUT=SUPPRESSED", self.dxe)

    def test_builder_uses_runtime_inventory_and_marks_image(self):
        self.assertIn("[switch]$StopReleaseOnlyBench", self.builder)
        self.assertIn("-StopReleaseOnlyBench requires -AcceptUnverifiedActuation", self.builder)
        self.assertIn("No explicit BDFs means the default live-inventory select-all policy", self.builder)
        self.assertIn("at least one BDF", self.builder)
        self.assertIn("$selection.Count -lt 1", self.builder)
        self.assertIn("GPU-SEQUENCE-STOP-RELEASE-BENCH-DXE-' + $selection.Label", self.builder)
        self.assertIn("/DGU_UNVERIFIED_BENCH_CONDITIONS /DGU_DXE_FILE_ONLY_OUTPUT /DGU_STOP_RELEASE_ONLY_BENCH", self.builder)
        self.assertIn("GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi", self.builder)
        self.assertIn("Where-Object { $_ -ne", self.builder)
        self.assertIn(r"vendor\unlock_610.c", self.builder)
        for field in ("ASPM_ADMISSION_BYPASSED", "INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED", "FILE_ONLY_OUTPUT",
                      "STOP_RELEASE_ONLY", "FIRMWARE_TRANSACTION", "UNLOCK", "REATTACH_ATTEMPTED", "REATTACH_VERIFIED"):
            self.assertIn(field + "=", self.builder)

    def test_mandatory_warning_and_three_second_gate_precede_mutation(self):
        self.assertIn("EFI_STATUS GuBootBenchCountdown(BOOLEAN console_output,BOOLEAN console_required)", self.boot)
        countdown = self.boot.split("EFI_STATUS GuBootBenchCountdown(BOOLEAN console_output,BOOLEAN console_required)", 1)[1].split("EFI_STATUS GuBootBenchReportCompletion", 1)[0]
        self.assertIn("size_t count=GuSelectedGpuCount();", countdown)
        file_only = countdown.split("if(!console_output)", 1)[1].split("/* A sole-GOP target", 1)[0]
        self.assertIn("CONSOLE_IO=0", file_only)
        self.assertNotIn("Table->ConOut", file_only)
        self.assertIn("if(console_required)return display;", countdown)
        self.assertIn("FILE_LOG=AUTHORITATIVE GPU_ACTION=%s", countdown)
        completion = self.boot.split("EFI_STATUS GuBootBenchReportCompletion(BOOLEAN console_output)", 1)[1].split("#endif", 1)[0]
        self.assertIn("BENCH_COMPLETION=FILE_ONLY CONSOLE_IO=0", completion)
        self.assertIn("BENCH_COMPLETION=CONSOLE_DISPLAYED", completion)
        self.assertIn("BENCH_COMPLETION=CONSOLE_NOT_SHOWN", completion)
        self.assertLess(countdown.index("if(!console_output)"), countdown.index("Table->ConOut->OutputString"))
        for phrase in ("Live NVIDIA Stop/release and reattach test starts in 3 seconds",
                       "selected for this test", "PCI=", "go black", "disconnect, or flicker",
                       "reattachment is attempted, not guaranteed", "independent display"):
            self.assertIn(phrase, self.boot)
        self.assertIn("Table->BootServices->Stall(3000000U)", self.boot)
        self.assertIn("GPU_ACTION=REFUSED", self.boot)
        sequence = self.execution.split("EFI_STATUS GuExecuteStopReleaseSequence", 1)[1].split("#endif", 1)[0]
        self.assertGreater(sequence.index("GuBootBenchCountdown("), sequence.index("GuMsiPreflight("))
        self.assertLess(sequence.index("GuBootBenchCountdown("), sequence.index("GuMsiRelease("))
        self.assertIn("GuMsiStopReleaseFileOnly()", sequence)
        self.assertIn("GuMsiStopReleaseOnlyGopTarget()", sequence)
        self.assertIn("GuMsiStopReleaseConsoleRestored()", self.app)
        self.assertIn("GuMsiStopReleaseConsoleRestored()", self.dxe)
        self.assertIn("SYSTEM_TABLE_CONOUT", self.boot)
        preflight = self.release.split("EFI_STATUS GuMsiPreflight", 1)[1].split("static BOOLEAN DecodeMask", 1)[0]
        topology = preflight.index("s=Gops(TRUE)")
        consoleless = preflight.index("Plan.consoleless_target_gop=TRUE")
        console_init = preflight.index("if(!Plan.consoleless_target_gop) {")
        self.assertLess(topology, consoleless)
        self.assertLess(consoleless, console_init)
        self.assertLess(console_init, preflight.index("Plan.console=t->ConsoleOutHandle"))
        self.assertIn("STOP_RELEASE_CONSOLE=NOT_REQUIRED TARGET_IS_ONLY_GOP=1", self.release)
        self.assertIn("if(Plan.consoleless_target_gop)return EFI_SUCCESS;", self.release)
        self.assertIn("if(Plan.consoleless_target_gop) {", self.release)

    def test_stop_only_console_gate_accepts_non_target_nvidia_survivor_but_requires_it(self):
        check = self.release.split("static EFI_STATUS ReadOnlyAmdConsoleOwnership", 1)[1].split("static EFI_STATUS ReadOnlyAmdConsoleGraph", 1)[0]
        variable = self.release.split("static EFI_STATUS AmdConsoleVariable", 1)[1].split("static EFI_STATUS ReadOnlyAmdConsoleOwnership", 1)[0]
        self.assertIn("#ifdef GU_STOP_RELEASE_ONLY_BENCH\n      int owner=GuDevicePathDisplayOwner", variable)
        self.assertIn("#else\n      int owner=GuDevicePathAmdDisplayOwner", variable)
        self.assertIn("#ifdef GU_STOP_RELEASE_ONLY_BENCH\n    if(GuDevicePathDisplayOwner", check)
        self.assertIn("#else\n    if(GuDevicePathAmdDisplayOwner", check)
        self.assertIn("NO_INDEPENDENT_SURVIVOR_GOP", check)
        self.assertIn("matching_gops!=1", variable)
        graph = self.release.split("static EFI_STATUS ReadOnlyAmdConsoleGraph", 1)[1].split("static EFI_STATUS ReadConfigMode", 1)[0]
        self.assertIn("if(!survivor||e->count!=1)return EFI_ACCESS_DENIED;", graph)

    def test_stop_release_preparation_skips_assets_diagnostics_and_profile(self):
        body = self.execution.split("EFI_STATUS GuPrepareBoundGpu", 1)[1].split("EFI_STATUS GuExecuteStopReleaseSequence", 1)[0]
        self.assertIn("STOP_RELEASE_PREPARED=1 FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0", body)
        self.assertIn("#ifdef GU_STOP_RELEASE_ONLY_BENCH", body)
        self.assertIn("stop_release_ready=TRUE", body)
        self.assertIn("#else", body)
        stop_path = body.split("#ifdef GU_STOP_RELEASE_ONLY_BENCH", 1)[1].split("#else", 1)[0]
        self.assertNotIn("GuBootLoadAssets()", stop_path)
        self.assertNotIn("GuDiagnosticScan(", stop_path)
        self.assertNotIn("GuAd104Prepare()", stop_path)

    def test_release_profile_has_no_vendor_unlock_call_and_reconnects(self):
        release = self.execution.split("EFI_STATUS GuExecuteStopReleaseSequence", 1)[1].split("#endif", 1)[0]
        for symbol in ("GuMsiPreflight(", "GuMsiRelease(", "GuMsiReattachAfterStop(",
                       "GuFinishSelectedGpuStep(", "GuVerifySelectedSequence("):
            self.assertIn(symbol, release)
        self.assertNotIn("gpu_unlock_610", release)
        self.assertIn("#ifndef GU_STOP_RELEASE_ONLY_BENCH", self.execution)
        self.assertIn("ConnectController(Plan.pci,NULL,NULL,TRUE)", self.release)
        self.assertIn("Expected(0,FALSE)", self.release)
        self.assertIn("Gops(FALSE)", self.release)
        self.assertIn("FIRMWARE_TRANSACTION=0", self.release)
        self.assertIn("GuMsiVerifyFinalGraph()", self.port_source)

    def test_stop_only_app_does_not_load_windows_image_or_handoff(self):
        branch_start = self.app.rfind("#ifdef GU_STOP_RELEASE_ONLY_BENCH", 0, self.app.index("STOP_RELEASE_ONLY_IMAGE=1"))
        branch_end = self.app.index('#else\n  stage="stage-windows-image"', branch_start)
        stop_branch = self.app[branch_start:branch_end]
        self.assertNotIn("GuBootLoadWindows()", stop_branch)
        self.assertNotIn("GuBootWindows()", stop_branch)
        self.assertIn("OS_HANDOFF=NONE", stop_branch)
        self.assertIn("STOP_RELEASE_ONLY=REFUSED REASON=PRIOR_FAILED_GPU_STATE", self.app)

    def test_bootable_application_variant_excludes_unlock_and_stages_as_application(self):
        self.assertIn("[switch]$StopReleaseOnlyBench", self.app_builder)
        self.assertIn("No explicit BDFs means the default live-inventory select-all policy", self.app_builder)
        self.assertIn("at least one BDF", self.app_builder)
        self.assertIn("$selection.Count -lt 1", self.app_builder)
        self.assertIn("GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME.EFI", self.app_builder)
        self.assertIn("GPU-SEQUENCE-STOP-RELEASE-BENCH-' + $selection.Label", self.app_builder)
        self.assertIn("/subsystem:efi_application", self.app_builder)
        self.assertIn(r"Where-Object { $_ -ne 'vendor\unlock_610.c' }", self.app_builder)
        self.assertIn("/DGU_STOP_RELEASE_ONLY_BENCH", self.app_builder)
        branch_start = self.app.rfind("#ifdef GU_STOP_RELEASE_ONLY_BENCH", 0, self.app.index("STOP_RELEASE_ONLY_IMAGE=1"))
        branch_end = self.app.index('#else\n  stage="stage-windows-image"', branch_start)
        stop_branch = self.app[branch_start:branch_end]
        self.assertNotIn("GuBootLoadWindows()", stop_branch)
        self.assertNotIn("GuBootBenchCountdown(", stop_branch)
        self.assertIn("GuBootBenchReportCompletion(!GuMsiStopReleaseFileOnly()||", stop_branch)
        self.assertIn("GuMsiStopReleaseConsoleRestored()", stop_branch)
        self.assertIn("GuExecuteStopReleaseSequence(", stop_branch)
        self.assertNotIn("GuBootWindows()", stop_branch)
        self.assertIn("OS_HANDOFF=NONE", stop_branch)
        self.assertIn("GuRuntimeSelectionSelectAll()", self.app)
        self.assertIn("GuSelectedGpuRecordAt", self.boot)
        self.assertNotIn("RTX 4090 then RTX 3080", self.boot)
        self.assertNotIn("GuExecutePreparedSequence(", stop_branch)
        self.assertNotIn("GuBootChoose(", stop_branch)
        variant = next(v for v in self.catalog["variants"]
                       if v["id"] == "app-stop-release-sequence-bench")
        self.assertEqual(variant["family"], "prepare")
        self.assertNotIn("DXE", variant["binary"])
        self.assertTrue(variant["requires_local_boot_profile"])
        self.assertNotIn("requires_selection", variant)
        self.assertTrue(variant["switches"]["RuntimeSelection"])
        self.assertEqual(variant["attestations"][2],
                         "TARGET_ORDER=RUNTIME_SUPPORTED_NVIDIA_ENUMERATION")
        for token in ("FIRMWARE_TRANSACTION=0", "UNLOCK=0", "STOP_RELEASE_ONLY=1",
                      "ASPM_ADMISSION_BYPASSED=1", "REATTACH_ATTEMPTED=1"):
            self.assertIn(token, variant["attestations"])

    def test_catalogue_keeps_override_as_a_separate_actuating_variant(self):
        matches = [v for v in self.catalog["variants"] if v["id"] == "dxe-stop-release-sequence-bench"]
        self.assertEqual(len(matches), 1)
        v = matches[0]
        self.assertIn("image_variants.json", self.sign_deploy)
        self.assertIn("function Select-Variants", self.sign_deploy)
        self.assertIn("Remove('RuntimeSelection')", self.sign_deploy)
        self.assertIn("(Get-SelectionCount) -ge 1", self.sign_deploy)
        self.assertEqual(v["safety"], "actuates")
        self.assertEqual(v["binary"], "GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi")
        self.assertTrue(v["switches"]["StopReleaseOnlyBench"])
        self.assertTrue(v["switches"]["RuntimeSelection"])
        self.assertNotIn("requires_selection", v)
        self.assertEqual(v["attestations"][2],
                         "TARGET_ORDER=RUNTIME_SUPPORTED_NVIDIA_ENUMERATION")
        for token in ("ASPM_ADMISSION_BYPASSED=1", "INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED=CONDITIONAL",
                      "FILE_ONLY_OUTPUT=1", "STOP_RELEASE_ONLY=1", "FIRMWARE_TRANSACTION=0",
                      "UNLOCK=0", "REATTACH_ATTEMPTED=1", "REATTACH_VERIFIED=0",
                      "RECOVERY_VERIFIED=0"):
            self.assertIn(token, v["attestations"])
        original = [v for v in self.catalog["variants"] if v["id"] == "dxe-actuate-sequence"]
        self.assertEqual(len(original), 1)
        self.assertNotIn("StopReleaseOnlyBench", original[0]["switches"])


if __name__ == "__main__":
    unittest.main()
