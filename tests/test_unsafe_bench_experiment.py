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
        self.assertIn("/DGU_UNVERIFIED_BENCH_CONDITIONS /DGU_STOP_RELEASE_ONLY_BENCH", self.builder)
        self.assertIn("[switch]$FileOnlyBootLog", self.builder)
        self.assertIn("if ($FileOnlyBootLog)", self.builder)
        self.assertIn("-StopReleaseOnlyBench requires -FileOnlyBootLog", self.builder)
        self.assertIn("-StopReleaseOnlyBench requires -FileOnlyBootLog", self.app_builder)
        self.assertIn("/DGU_DXE_FILE_ONLY_OUTPUT", self.builder)
        self.assertIn("GPU-SEQUENCE-STOP-RELEASE-BENCH-RUNTIME-DXE.efi", self.builder)
        self.assertIn("Where-Object { $_ -ne", self.builder)
        self.assertIn(r"vendor\unlock_610.c", self.builder)
        for field in ("ASPM_ADMISSION_BYPASSED", "INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED", "FILE_ONLY_OUTPUT",
                      "STOP_RELEASE_ONLY", "FIRMWARE_TRANSACTION", "UNLOCK", "REATTACH_ATTEMPTED", "REATTACH_VERIFIED"):
            self.assertIn(field + "=", self.builder)

    def test_mandatory_logged_warning_precedes_stop(self):
        countdown = self.boot.split("EFI_STATUS GuBootBenchCountdown(VOID)", 1)[1].split("EFI_STATUS GuBootBenchReportCompletion", 1)[0]
        self.assertIn("BenchBuildTargetText(targets,GU_STOP_BENCH_TARGET_TEXT_CHARS)", countdown)
        self.assertIn("BenchWriteConsole(prefix,targets,suffix)", countdown)
        self.assertIn("FILE_LOG=AUTHORITATIVE", countdown)
        self.assertIn("if(!GuBootLogOpen())return EFI_DEVICE_ERROR;", countdown)
        self.assertIn("Stall(3000000U)", countdown)
        self.assertNotIn("console_required", countdown)
        self.assertLess(countdown.index("if(!GuBootLogOpen())return EFI_DEVICE_ERROR;"),
                        countdown.index("Stall(3000000U)"))
        display = self.boot.split("static EFI_STATUS BenchWriteConsole", 1)[1].split("EFI_STATUS GuBootBenchCountdown", 1)[0]
        self.assertIn("HandleProtocol(handles[i],&text_guid,(VOID **)&live)", display)
        self.assertIn("live!=expected[i]", display)
        self.assertIn("ConsoleOutHandle", display)
        self.assertIn("StandardErrorHandle", display)
        self.assertIn("Do not rearrange display cables", self.boot)
        self.assertIn("detected NVIDIA GPUs", self.boot)
        self.assertIn("GuSelectedGpuRecordAt", self.boot)
        completion = self.boot.split("EFI_STATUS GuBootBenchReportCompletion(VOID)", 1)[1].split("#endif", 1)[0]
        self.assertIn("GuBootLogOpen()", completion)
        self.assertIn("BenchWriteConsole(prefix,targets,suffix)", completion)
        self.assertIn("FILE_LOG=AUTHORITATIVE", completion)
        sequence = self.execution.split("EFI_STATUS GuExecuteStopReleaseSequence", 1)[1].split("#endif", 1)[0]
        self.assertLess(sequence.index("GuMsiPreflight("),
                        sequence.index("GuBootBenchCountdown()"))
        self.assertLess(sequence.index("GuBootBenchCountdown()"),
                        sequence.index("GuMsiRelease()"))
        self.assertIn("GuMsiReattachAfterStop()", sequence)
        self.assertIn("GuBootBenchReportCompletion()", self.app)
        self.assertIn("GuBootBenchReportCompletion()", self.dxe)
        preflight = self.release.split("EFI_STATUS GuMsiPreflight", 1)[1].split("static BOOLEAN DecodeMask", 1)[0]
        self.assertLess(preflight.index("s=Gops(TRUE)"),
                        preflight.index("Plan.consoleless_target_gop=TRUE"))
        consumers = self.release.split("static EFI_STATUS Consumers(VOID)", 1)[1].split("EFI_STATUS GuMsiPreflight", 1)[0]
        self.assertIn("console_child++", consumers)
        self.assertIn("Consumers refused target-console child", consumers)

    def test_run0039_refusal_identifies_exact_child_without_waiver(self):
        classifier = self.release.split("static EFI_STATUS ClassifyIdleOutput", 1)[1].split("static EFI_STATUS Consumers", 1)[0]
        for reason in ("NOT_EXACT_TARGET_CHILD", "PROTECTED_HANDLE",
                       "SHARED_OR_CONSUMED_CHILD", "TARGET_PATH_NOT_PROVEN"):
            self.assertIn("REASON=" + reason, classifier)
        self.assertIn("other->handle==h||other->agent==h||other->controller==h", classifier)
        self.assertIn("GuIsPreparedPeerHandle(h)", classifier)
        self.assertIn("return EFI_ACCESS_DENIED;", classifier)
        self.assertLess(classifier.index("REASON=SHARED_OR_CONSUMED_CHILD"),
                        classifier.index("e->idle_output=TRUE;"))

    def test_verified_edid_child_is_a_stage_three_only_dependent(self):
        # RUN0040: standard EDID Discovered on the selected output child.
        # Only the exact 128-bit UEFI identity may be classified.
        self.assertIn("static EFI_GUID EdidDiscoveredGuid={0x1c0c34f6,0xd380,0x41fa,", self.release)
        self.assertIn("{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}", self.release)
        removed = self.release.split("static BOOLEAN Removed(const EDGE *e,UINTN stage)", 1)[1].split(
            "static BOOLEAN ProtectedHandle", 1)[0]
        self.assertIn("if(stage>=3&&e->idle_output&&e->handle==e->controller&&", removed)
        self.assertIn("e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&e->count==1&&", removed)
        self.assertIn("Guid(&e->protocol,&EdidDiscoveredGuid)", removed)
        classifier = self.release.split("static EFI_STATUS ClassifyIdleOutput", 1)[1].split(
            "static EFI_STATUS Consumers", 1)[0]
        self.assertIn("other->handle==h&&other->controller==h", classifier)
        self.assertIn("Guid(&other->protocol,&EdidDiscoveredGuid)", classifier)
        self.assertIn("edid_index==Original.count", classifier)
        self.assertIn("GuIsPreparedPeerHandle(peer->handle)", classifier)
        self.assertIn("GuIsPreparedPeerHandle(peer->controller)", classifier)
        self.assertIn("ValidateDriverAgent(dependent->agent,3U)", classifier)
        self.assertIn("dependent->idle_output=TRUE;", classifier)
        self.assertLess(classifier.index("ValidateDriverAgent(dependent->agent,3U)"),
                        classifier.index("dependent->idle_output=TRUE;"))
        self.assertIn("STOP_RELEASE_IDLE_EDID=ACCEPTED", classifier)
        self.assertIn("REASON=EDID_DRIVER_NOT_VERIFIED", classifier)
        self.assertIn("REASON=SHARED_OR_CONSUMED_CHILD", classifier)

    def test_stop_only_file_write_or_flush_failure_remains_sticky(self):
        logging = self.boot.split("VOID GuLog(const char *format,...)", 1)[1].split("VOID GuLogText", 1)[0]
        for token in ("#ifdef GU_STOP_RELEASE_ONLY_BENCH", "Log->Write?", "Log->Flush?",
                      "BootFileStatus=EFI_DEVICE_ERROR",
                      "if(BootFileStatus!=EFI_SUCCESS)return;"):
            self.assertIn(token, logging)
        ready = self.boot.split("BOOLEAN GuBootLogOpen(VOID)", 1)[1].split("EFI_HANDLE GuBootGetTargetEspVolume", 1)[0]
        self.assertIn("Log!=NULL&&BootFileStatus==EFI_SUCCESS", ready)

    def test_multi_gpu_warning_uses_independent_gop_before_each_stop(self):
        warning = self.release.split("EFI_STATUS GuMsiStopReleaseWarnSurvivor(VOID)", 1)[1].split(
            "EFI_STATUS GuMsiReattachAfterStop", 1)[0]
        sequence = self.execution.split("EFI_STATUS GuExecuteStopReleaseSequence", 1)[1].split(
            "#endif", 1)[0]
        countdown = self.boot.split("EFI_STATUS GuBootBenchCountdown(VOID)", 1)[1].split(
            "EFI_STATUS GuBootBenchReportCompletion", 1)[0]
        self.assertIn("GuSelectedGpuRecordAt(i,&selected)", warning)
        self.assertIn("selected.handle==Plan.pci", warning)
        self.assertIn("(selected.pci_id&0xffffU)!=0x10deU", warning)
        self.assertIn("GuDevicePathHasPrefix(", warning)
        self.assertIn("Plan.survivors[k].path_bytes", warning)
        self.assertIn("Interface(chosen->handle,&GopGuid,(VOID **)&g)", warning)
        self.assertIn("g!=chosen->interface", warning)
        self.assertIn("EfiBltVideoFill", warning)
        self.assertIn("EfiBltBufferToVideo", warning)
        self.assertIn("PHYSICAL_PIXELS_VERIFIED=0", warning)
        self.assertIn("GuVerifyActivePeers(Plan.table)", warning)
        self.assertIn("count>1U?", countdown)
        self.assertIn("GuMsiStopReleaseWarnSurvivor()", countdown)
        self.assertIn("count>1U&&display!=EFI_SUCCESS)return display", countdown)
        self.assertLess(sequence.index("GuMsiPreflight("),
                        sequence.index("GuBootBenchCountdown()"))
        self.assertLess(sequence.index("GuBootBenchCountdown()"),
                        sequence.index("GuMsiRelease()"))
        self.assertLess(sequence.index("GuMsiRelease()"),
                        sequence.index("GuMsiReattachAfterStop()"))
        self.assertLess(sequence.index("GuMsiReattachAfterStop()"),
                        sequence.index("GuSelectNextGpu(table)"))
        self.assertNotIn("warning_done", sequence)

    def test_warning_font_contains_every_character_drawn(self):
        warning = self.release.split("typedef struct {char symbol;UINT8 row[7];} GU_BENCH_GLYPH;", 1)[1].split(
            "EFI_STATUS GuMsiReattachAfterStop", 1)[0]
        glyph_table = warning.split("static const GU_BENCH_GLYPH BenchGlyphs[]={", 1)[1].split("};", 1)[0]
        import re
        glyphs = set(re.findall(r"\{'(.)',\{", glyph_table))
        phrases = ["WARNING GPU DRIVER STOP TEST", "TARGET 0000:00:00.0",
                   "DISPLAY MAY GO DARK", "RECOVERY MAY NEED POWER OFF",
                   "REATTACHMENT FAILED", "SYSTEM WILL REBOOT", "IN 30 SECONDS",
                   "USB BOOT MAY REPEAT THE TEST", "ENTER BIOS SELECT OTHER DRIVE",
                   "BREAK BOOT LOOP IN BIOS"]
        for char in "".join(phrases):
            if char != " ":
                self.assertIn(char, glyphs)
        self.assertIn("Gu.discovery_identity.segment", warning)
        self.assertIn("Gu.discovery_identity.bus", warning)
        self.assertIn("Gu.discovery_identity.device", warning)
        self.assertIn("Gu.discovery_identity.function", warning)

    def test_failed_reattach_warns_on_peer_gop_before_thirty_second_reboot(self):
        notice = self.release.split(
            "EFI_STATUS GuMsiStopReleaseShowReattachFailureNotice(VOID)", 1)[1].split(
            "EFI_STATUS GuMsiReattachAfterStop", 1)[0]
        failed = self.app.rsplit("failed:", 1)[1]
        execution = self.execution.split("EFI_STATUS GuExecuteStopReleaseSequence", 1)[1].split(
            "#endif", 1)[0]
        self.assertIn("StopReleaseReattachFailed=TRUE;", execution)
        self.assertIn("STOP_RELEASE_REATTACH=FAILED", execution)
        self.assertIn("if(GuStopReleaseReattachFailed())", failed)
        self.assertIn("GuMsiStopReleaseShowReattachFailureNotice()", failed)
        self.assertIn("ENTER_BIOS_SELECT_OTHER_DRIVE=1", failed)
        self.assertIn("USB_FIRST_BOOT_MAY_LOOP=1", failed)
        self.assertIn("Stall(30000000U)", failed)
        self.assertIn("ResetSystem(EfiResetWarm,EFI_ABORTED,0,NULL)", failed)
        self.assertLess(failed.index("GuMsiStopReleaseShowReattachFailureNotice()"),
                        failed.index("Stall(30000000U)"))
        self.assertLess(failed.index("GuBootClose()", failed.index("GuMsiStopReleaseShowReattachFailureNotice()")),
                        failed.index("Stall(30000000U)"))
        self.assertLess(failed.index("Stall(30000000U)"),
                        failed.index("ResetSystem(EfiResetWarm,EFI_ABORTED,0,NULL)"))
        self.assertIn("if(notice!=EFI_SUCCESS", failed)
        self.assertIn("GuBootHalt();return EFI_ABORTED;", failed)
        self.assertIn("AUTO_WARM_RESET=0", failed)
        self.assertIn("FIRMWARE_TRANSACTION=0", failed)
        self.assertNotIn("GuBootSay(", failed.split("if(GuStopReleaseReattachFailed())", 1)[1].split(
            "AUTO_WARM_RESET=0", 1)[0])
        for code in ("warning_peer_gop", "GuDevicePathUnchanged(",
                     "g!=survivor->interface", "EfiBltVideoFill",
                     "EfiBltBufferToVideo", "PHYSICAL_PIXELS_VERIFIED=0"):
            if code == "EfiBltBufferToVideo":
                self.assertIn(code, self.release)
            else:
                self.assertIn(code, notice)
        for line in ("REATTACHMENT FAILED", "SYSTEM WILL REBOOT", "IN 30 SECONDS",
                     "USB BOOT MAY REPEAT THE TEST", "ENTER BIOS SELECT OTHER DRIVE",
                     "BREAK BOOT LOOP IN BIOS"):
            self.assertIn(line, notice)

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

    def test_console_refusal_names_variable_and_reports_selected_owner_without_waiver(self):
        check = self.release.split("static EFI_STATUS ReadOnlyAmdConsoleOwnership", 1)[1].split(
            "static EFI_STATUS ReadOnlyAmdConsoleGraph", 1)[0]
        variable = self.release.split("static EFI_STATUS AmdConsoleVariable", 1)[1].split(
            "static EFI_STATUS ReadOnlyAmdConsoleOwnership", 1)[0]
        for name in ("ConOut", "ErrOut"):
            self.assertIn(f'AmdConsoleVariable(L"{name}","{name}",owners,display_count)', check)
        self.assertIn("const char *variable_label", variable)
        self.assertIn("TARGET_PCI_MATCH=%d", variable)
        self.assertIn("variable_label,owner,(unsigned)matching_gops,target_pci_match", variable)
        self.assertIn("Interface(Plan.pci,&PathGuid,(VOID **)&target_path)", variable)
        self.assertIn("target_pci_match=GuDevicePathHasPrefix(", variable)
        self.assertLess(variable.index("target_pci_match=GuDevicePathHasPrefix("),
                        variable.index("data[at+1]=subtype;"))
        self.assertIn("if(owner!=1||matching_gops!=1)", variable)
        self.assertIn("return EFI_ACCESS_DENIED;", variable)
        self.assertNotIn("name,owner,(unsigned)matching_gops", variable)

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
        self.assertIn("static BOOLEAN ReattachedGraphExpected(VOID)", self.release)
        self.assertIn("Gops(FALSE)", self.release)
        self.assertIn("FIRMWARE_TRANSACTION=0", self.release)
        reattach = self.release.split("EFI_STATUS GuMsiReattachAfterStop(VOID)", 1)[1].split("#endif", 1)[0]
        self.assertIn("TARGET_GOP_RECOGNIZED=%u TARGET_CHILD_PATHS_MATCHED=%zu", reattach)
        self.assertIn("DRIVERS_VERIFIED=1 SURVIVOR_GOPS_MATCHED=%zu", reattach)
        self.assertNotIn("Expected(0,FALSE)", reattach)
        self.assertIn("DISPLAY_OUTPUT_TESTED=0", reattach)
        self.assertIn("ReconcileTargetChildren()", reattach)
        self.assertIn("ReattachedGraphExpected()", reattach)
        self.assertIn("ReattachedDriversVerified()", reattach)
        self.assertIn("ReattachedTargetPciRecognized()", reattach)
        self.assertLess(reattach.index("ReconcileTargetChildren()"),
                        reattach.index("ReattachedGraphExpected()"))
        self.assertLess(reattach.index("ReattachedTargetPciRecognized()"),
                        reattach.index("Plan.reattach_verified=TRUE"))
        final = self.release.split("BOOLEAN GuMsiVerifyFinalGraph(VOID)", 1)[1].split("#else", 1)[0]
        for check in ("ReconcileTargetChildren()", "ReattachedGraphExpected()",
                      "ReattachedDriversVerified()", "ReattachedTargetPciRecognized()"):
            self.assertIn(check, final)
        self.assertLess(reattach.index("s=Gops(FALSE)"),
                        reattach.index("Plan.reattach_verified=TRUE"))
        self.assertLess(reattach.index("s=ReadOnlyAmdConsoleOwnership()"),
                        reattach.index("Plan.reattach_verified=TRUE"))
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
        self.assertIn("[switch]$FileOnlyBootLog", self.app_builder)
        branch_start = self.app.rfind("#ifdef GU_STOP_RELEASE_ONLY_BENCH", 0, self.app.index("STOP_RELEASE_ONLY_IMAGE=1"))
        branch_end = self.app.index('#else\n  stage="stage-windows-image"', branch_start)
        stop_branch = self.app[branch_start:branch_end]
        self.assertNotIn("GuBootLoadWindows()", stop_branch)
        self.assertNotIn("GuBootBenchCountdown(", stop_branch)
        self.assertIn("GuBootBenchReportCompletion()", stop_branch)
        self.assertIn("GuBootBenchReportCompletion()", stop_branch)
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
        # The USB signing catalogue must build the same runtime-ESP profile
        # that the Windows CI compiles, without requiring a private ESP GUID.
        self.assertNotIn("requires_local_boot_profile", variant)
        self.assertNotIn("requires_selection", variant)
        self.assertTrue(variant["switches"]["RuntimeEsp"])
        self.assertIn("RUNTIME_ESP=1", variant["attestations"])
        self.assertTrue(variant["switches"]["RuntimeSelection"])
        self.assertTrue(variant["switches"]["FileOnlyBootLog"])
        self.assertIn("TARGET_ORDER=RUNTIME_SUPPORTED_NVIDIA_ENUMERATION",
                      variant["attestations"])
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
        self.assertTrue(v["switches"]["FileOnlyBootLog"])
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
