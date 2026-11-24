"""Contracts for the explicitly opted-in, unverified DXE bench variant."""
import json
import re
import subprocess
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def without_windows_handoff(branch):
    """Select the existing return-to-firmware compile-time branches."""
    return re.sub(r"#ifdef GU_STOP_RELEASE_WINDOWS_HANDOFF\n(.*?)(?:#else\n(.*?))?#endif\n",
                  lambda match: match.group(2) or "", branch, flags=re.S)


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
        self.assertIn("#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) || defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_FIRMWARE_REATTACH)", accepted)
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
        self.assertIn("Assert-GuExecutionBuildOptions ", self.builder)
        self.assertIn("Assert-GuExecutionBuildOptions ", self.app_builder)
        self.assertIn("-StopReleaseOnlyBench requires -AcceptUnverifiedActuation",
                      (Path(__file__).resolve().parents[1] / "src" / "Gpu-Execution-Sources.ps1").read_text(encoding="utf-8"))
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
        for token in ("Log->Write?", "Log->Flush?",
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
        self.assertIn("GuWarningGpuRecordAt(i,&selected)", warning)
        self.assertIn("#define GuWarningGpuRecordAt GuSelectedGpuRecordAt", self.release)
        self.assertIn("#define GuWarningGpuRecordAt GuPreparedGpuRecordAt", self.release)
        self.assertIn("selected.handle==Plan.pci", warning)
        self.assertIn("(selected.pci_id&0xffffU)!=0x10deU", warning)
        self.assertIn("GuDevicePathHasPrefix(", warning)
        self.assertIn("Plan.survivors[k].path_bytes", warning)
        self.assertIn("Interface(chosen->handle,&GopGuid,(VOID **)&g)", warning)
        self.assertIn("g!=chosen->interface", warning)
        self.assertIn("EfiBltVideoFill", warning)
        # The warning uses the shared glyph renderer for its text. Verify
        # the call and the actual BLT operation, not its former inline location.
        glyph_line = self.release.split("static EFI_STATUS BenchGopLine(", 1)[1].split(
            "static char BenchHex", 1)[0]
        self.assertIn("BenchGopLine(g,", warning)
        self.assertIn("g->Blt(g,pixels,EfiBltBufferToVideo", glyph_line)
        self.assertIn("if(s!=EFI_SUCCESS)return s;", glyph_line)
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
                        sequence.index("GuPrepareNextSelectedGpu(&owner,table,stage)"))
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
                   "BREAK BOOT LOOP IN BIOS", "GPU REATTACHMENT VERIFIED",
                   "STOP RELEASE COMPLETE", "FIRMWARE DRIVERS RESTORED"]
        for char in "".join(phrases):
            if char != " ":
                self.assertIn(char, glyphs)
        self.assertIn("Gu.discovery_identity.segment", warning)
        self.assertIn("Gu.discovery_identity.bus", warning)
        self.assertIn("Gu.discovery_identity.device", warning)
        self.assertIn("Gu.discovery_identity.function", warning)

    def test_verified_reattach_replaces_only_the_original_peer_warning(self):
        notice = self.release.split("static EFI_STATUS BenchShowReattachSuccess(VOID)", 1)[1].split(
            "EFI_STATUS GuMsiStopReleaseWarnSurvivor", 1)[0]
        reattach = self.release.split("EFI_STATUS GuMsiReattachAfterStop(VOID)", 1)[1].split(
            "#endif", 1)[0]
        for gate in ("!ReleaseOwnerMatches()", "!Plan.reattach_verified",
                     "!Plan.reconnecting", "Plan.stage", "Plan.claimed", "Gu.owned",
                     "Gu.fault", "!Plan.warning_peer_gop", "!GuBootLogOpen()",
                     "Plan.survivors[i].handle==Plan.warning_peer_gop",
                     "!survivor->seen", "g!=survivor->interface",
                     "GuDevicePathUnchanged(", "Equal(g->Mode,", "Equal(g->Mode->Info,"):
            self.assertIn(gate, notice)
        self.assertIn("EfiBltVideoFill,0,0,12,12,580,198,0", notice)
        self.assertIn("background={24,96,16,0}", notice)
        self.assertIn('BenchGopLine(g,24,26,scale,background,"GPU REATTACHMENT VERIFIED")', notice)
        self.assertIn('BenchGopLine(g,24,67,scale,background,target)', notice)
        glyph_line = self.release.split("static EFI_STATUS BenchGopLine(", 1)[1].split(
            "static char BenchHex", 1)[0]
        self.assertIn("EFI_GRAPHICS_OUTPUT_BLT_PIXEL background", glyph_line)
        self.assertIn("white:background", glyph_line)
        self.assertIn("Gu.discovery_identity.segment", notice)
        self.assertLess(notice.index("GuDevicePathUnchanged("), notice.index("EfiBltVideoFill"))
        self.assertLess(notice.index("EfiBltVideoFill"), notice.index("BenchGopLine("))
        for check in ("ReattachedGraphExpected()", "ReattachedDriversVerified()",
                      "BenchTargetConsoleRestored()", "GuVerifyActivePeers(Plan.table)",
                      "Plan.reattach_verified=TRUE"):
            self.assertLess(reattach.index(check), reattach.index("BenchShowReattachSuccess()"))
        self.assertIn("if(Plan.warning_peer_gop)", reattach)
        self.assertIn("STOP_RELEASE_SUCCESS_GOP=%s", reattach)
        self.assertIn("PHYSICAL_PIXELS_VERIFIED=0 CONSOLE_IO=0", reattach)
        self.assertNotIn("return display", reattach)
        self.assertNotIn("->ConOut", notice)
        self.assertNotIn("SetMode(", notice)

    def test_combined_firmware_and_reattach_contract(self):
        core = self.execution.split("static EFI_STATUS GuExecuteUnlockCore", 1)[1].split("EFI_STATUS GuExecuteUnlock(", 1)[0]
        ordered = ["GuMsiPreflight(", "GuMsiStopReleaseWarnSurvivor()", "Stall(3000000U)",
                   "GuMsiRelease()", "gpu_unlock_610_unlock(", "gpu_unlock_610_release_shared(",
                   "GuCompatCloseDevice(", "GuMsiFinish()", "GuMsiReattachAfterStop()",
                   "GuMsiReadyForWindows()"]
        self.assertEqual(sorted(core.index(x) for x in ordered), [core.index(x) for x in ordered])
        prepare = self.release.split("static EFI_STATUS PrepareReconnect(VOID)", 1)[1].split("#else", 1)[0]
        for gate in ("Plan.claimed||Gu.owned", "!Plan.hardware_verified", "!Gu.quiesced",
                     "Gu.flrs!=1", "!GuSafeToReturn()", "!GuBootLogOpen()"):
            self.assertIn(gate, prepare)
        self.assertNotIn("CloseProtocol(", prepare)
        proof = self.release.split("BOOLEAN GuMsiCompletedPlatform(", 1)[1].split("#elif", 1)[0]
        self.assertIn("plan->hardware_verified && plan->reattach_verified", proof)
        self.assertIn("plan->stage==0", proof)
        self.assertIn("GuSafeToReturnFor(platform)", proof)
        self.assertIn("if(policy!=GU_S3_DISABLED)return EFI_UNSUPPORTED;", self.execution)
        self.assertIn("GU_UNVERIFIED_BENCH_CONDITIONS", self.port.split("#ifdef GU_FIRMWARE_REATTACH", 1)[1])
        self.assertIn("FUNCTIONAL_PERSISTENCE=PENDING_WINDOWS", core)

    def test_combined_containment_does_not_use_the_stopped_console(self):
        failed = self.app.rsplit("failed:", 1)[1]
        combined = failed.split("#ifdef GU_FIRMWARE_REATTACH", 1)[1].split("#else", 1)[0]
        self.assertIn("if(GuExecutionNeedsContainment())GuBootSilence();", combined)
        self.assertIn("else GuBootUnsilence();", combined)
        self.assertIn("GuBootWatchdogCancel()", failed)
        self.assertIn("FIRMWARE_REATTACH_CONTAINMENT=HALT NEXT_GPU=BLOCKED AUTO_WARM_RESET=0", failed)
        halt = self.boot.split("VOID GuBootHalt(VOID)", 1)[1].split("/* ----", 1)[0]
        say = self.boot.split("VOID GuBootSay(", 1)[1].split("\n}", 1)[0]
        self.assertIn("GuBootSay(", halt)
        self.assertIn("!Silent&&Table&&Table->ConOut", say)

    def test_combined_reconnect_and_completion_require_independent_proofs(self):
        prepare = self.release.split("static EFI_STATUS PrepareReconnect(VOID)", 1)[1].split("#ifdef GU_FIRMWARE_REATTACH\n#define GU_REATTACH_TRANSACTION_LOG", 1)[0]
        completion = self.release.split("BOOLEAN GuMsiCompletedPlatform(", 1)[1].split("/* Check the live graph", 1)[0]
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef void VOID;
#define FALSE 0
#define TRUE 1
#define EFI_SUCCESS 0
#define EFI_NOT_READY 7
typedef struct {
  void *table,*pci,*self;
  int ready,mutated,stage,claimed,survivor_baseline_valid,master_normalized,hardware_verified,reattach_verified;
} PLAN;
typedef struct { PLAN plan; } STATE;
typedef struct {
  STATE *release_state;
  void *handle,*image;
  int owned,fault,quiesced,flrs,retained,allocations,dma_count,master_attributes_pending;
} GU_PLATFORM;
static STATE state;
#define Plan state.plan
static GU_PLATFORM Gu;
static int owner_ok, log_ok;
static int ReleaseOwnerMatches(void) {return owner_ok;}
static int ReleaseStateMatches(const GU_PLATFORM *p) {return owner_ok&&p==&Gu&&p->release_state==&state;}
static int GuBootLogOpen(void) {return log_ok;}
static int GuSafeToReturnFor(const GU_PLATFORM *p) {
  return !p->retained&&!p->allocations&&!p->dma_count&&!p->master_attributes_pending&&p->quiesced&&!p->fault;
}
static int GuSafeToReturn(void) {return GuSafeToReturnFor(&Gu);}
''' + "static EFI_STATUS PrepareReconnect(VOID)" + prepare + "\nBOOLEAN GuMsiCompletedPlatform(" + completion + r'''
static void baseline(void) {
  memset(&state,0,sizeof(state));memset(&Gu,0,sizeof(Gu));
  owner_ok=log_ok=1;
  Gu.release_state=&state;Gu.handle=&Gu;Gu.image=&state;
  Plan.table=&state;Plan.pci=Gu.handle;Plan.self=Gu.image;
  Plan.ready=Plan.mutated=Plan.survivor_baseline_valid=Plan.master_normalized=Plan.hardware_verified=1;
  Plan.stage=3;Gu.quiesced=Gu.flrs=1;
}
int main(void) {
  baseline();assert(PrepareReconnect()==EFI_SUCCESS);
  assert(!GuMsiCompletedPlatform(&Gu,&state));
  #define REFUSE_RECONNECT(x) do {baseline();x;assert(PrepareReconnect()!=EFI_SUCCESS);} while(0)
  REFUSE_RECONNECT(owner_ok=0);REFUSE_RECONNECT(log_ok=0);
  REFUSE_RECONNECT(Plan.claimed=1);REFUSE_RECONNECT(Gu.owned=1);
  REFUSE_RECONNECT(Plan.hardware_verified=0);REFUSE_RECONNECT(Gu.quiesced=0);
  REFUSE_RECONNECT(Gu.flrs=0);REFUSE_RECONNECT(Gu.flrs=2);
  REFUSE_RECONNECT(Gu.dma_count=1);REFUSE_RECONNECT(Gu.retained=1);
  REFUSE_RECONNECT(Gu.fault=1);REFUSE_RECONNECT(Gu.allocations=1);
  REFUSE_RECONNECT(Gu.master_attributes_pending=1);
  baseline();Plan.stage=0;Plan.reattach_verified=1;
  assert(GuMsiCompletedPlatform(&Gu,&state));
  Plan.reattach_verified=0;assert(!GuMsiCompletedPlatform(&Gu,&state));
  Plan.reattach_verified=1;Plan.hardware_verified=0;assert(!GuMsiCompletedPlatform(&Gu,&state));
  Plan.hardware_verified=1;Gu.dma_count=1;assert(!GuMsiCompletedPlatform(&Gu,&state));
  Gu.dma_count=0;assert(!GuMsiCompletedPlatform(&Gu,&Gu));
  Gu.fault=1;assert(!GuMsiCompletedPlatform(&Gu,&state));
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "combined.c"
            binary = Path(tmp) / "combined"
            source.write_text(harness, encoding="utf-8")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-DGU_FIRMWARE_REATTACH", str(source), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)

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

    def test_each_selected_target_console_experiment_requires_exact_pci_and_peer(self):
        start = self.release.index("static EFI_STATUS BenchConfiguredConsolePaths(")
        end = self.release.index("static EFI_STATUS AmdConsoleVariable(", start)
        paths = self.release[start:end]
        preflight = self.release.split("EFI_STATUS GuMsiPreflight(", 1)[1].split(
            "static BOOLEAN DecodeMask", 1)[0]
        self.assertIn("for(size_t i=0;i<GuSelectedGpuCount();i++)", paths)
        self.assertIn("GuSelectedGpuRecordAt(i,&candidate)", paths)
        self.assertIn("(EFI_HANDLE)candidate.handle==Plan.pci&&candidate.pci==Gu.pci", paths)
        self.assertIn("if(++matches>1U)return EFI_COMPROMISED_DATA;", paths)
        self.assertIn("if(matches!=1U)return EFI_NOT_READY;", paths)
        self.assertIn("STEP=%zu", paths)
        self.assertNotIn("GuSelectedGpuRecordAt(0U,&first)", paths)
        self.assertIn("!Plan.survivor_count", paths)
        self.assertIn('BenchConfiguredConsolePaths(L"ConOut",TRUE)', paths)
        self.assertIn('BenchConfiguredConsolePaths(L"ErrOut",FALSE)', paths)
        self.assertIn("GuDevicePathHasPrefix(", paths)
        self.assertIn("if(!(target==1&&peers==0U) && !(target==0&&peers==1U))", paths)
        self.assertIn("if(target==1)targets++;", paths)
        self.assertIn("return !require_target||targets?EFI_SUCCESS:EFI_ACCESS_DENIED;", paths)
        self.assertIn("if(target<0||peers>1U)return EFI_COMPROMISED_DATA", paths)
        self.assertNotIn("SetVariable(", paths)
        self.assertNotIn("DisconnectController(", paths)
        self.assertIn("EFI_STATUS console_owner=BenchSelectedTargetConsole();", preflight)
        self.assertIn("if(console_owner==EFI_SUCCESS)", preflight)
        self.assertIn("Plan.target_console_experiment=TRUE;", preflight)
        self.assertIn("Plan.consoleless_target_gop=TRUE;", preflight)
        self.assertIn("StopFileOnlyOutput=TRUE;", preflight)
        self.assertLess(preflight.index("s=Gops(TRUE)"),
                        preflight.index("BenchSelectedTargetConsole()"))
        self.assertLess(preflight.index("BenchSelectedTargetConsole()"),
                        preflight.index("ReadOnlyAmdConsoleOwnership()"))
        self.assertIn("TARGET_CONSOLE_EXPERIMENT=%u", self.release)
        self.assertIn("STOP_RELEASE_CONSOLE=REFUSED", self.release)

    def test_stopped_target_uses_peer_direct_text_or_gop_but_never_global_conout(self):
        acquire = self.release.split(
            "EFI_STATUS GuMsiStopReleaseTryPeerConsole(VOID)", 1)[1].split(
            "/* Used only after a failed reattachment.", 1)[0]
        sequence = self.execution.split(
            "EFI_STATUS GuExecuteStopReleaseSequence", 1)[1].split("#endif", 1)[0]
        reattach = self.release.split(
            "EFI_STATUS GuMsiReattachAfterStop(VOID)", 1)[1].split("#endif", 1)[0]
        final = self.release.split("BOOLEAN GuMsiVerifyFinalGraph(VOID)", 1)[1].split(
            "#else", 1)[0]
        for proof in ("Plan.target_console_experiment", "Plan.stage!=3",
                      "Plan.warning_peer_gop", "GuDevicePathUnchanged(",
                      "g!=survivor->interface", "Gops(FALSE)",
                      "GuVerifyActivePeers(Plan.table)",
                      "original_text_driver&&current_text_driver",
                      "Interface(survivor->handle,&TextGuid,(VOID **)&peer_text)",
                      "peer_text->OutputString(peer_text,notice)",
                      "BenchGopLine(g,", "GLOBAL_CONOUT_CHANGED=0"):
            self.assertIn(proof, acquire)
        self.assertNotIn("->ConOut=", acquire)
        self.assertNotIn("->ConsoleOutHandle=", acquire)
        self.assertNotIn("SetVariable(", acquire)
        self.assertLess(sequence.index("GuMsiRelease()"),
                        sequence.index("GuMsiStopReleaseTryPeerConsole()"))
        self.assertLess(sequence.index("GuMsiStopReleaseTryPeerConsole()"),
                        sequence.index("GuMsiReattachAfterStop()"))
        self.assertIn("REATTACH_CONTINUES=1", sequence)
        self.assertIn("if(s==EFI_SUCCESS)s=BenchTargetConsoleRestored();", reattach)
        self.assertIn("if(s==EFI_SUCCESS)s=BenchTargetConsoleRestored();", final)
        self.assertIn("Interface(Plan.console,&TextGuid,&out)", self.release)
        self.assertIn("Interface(Plan.error,&TextGuid,&err)", self.release)

    def test_stop_only_console_gate_accepts_non_target_nvidia_survivor_but_requires_it(self):
        check = self.release.split("static EFI_STATUS ReadOnlyAmdConsoleOwnership", 1)[1].split("static EFI_STATUS ReadOnlyAmdConsoleGraph", 1)[0]
        variable = self.release.split("static EFI_STATUS AmdConsoleVariable", 1)[1].split("static EFI_STATUS ReadOnlyAmdConsoleOwnership", 1)[0]
        self.assertIn("#if defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_FIRMWARE_REATTACH)\n      int owner=GuDevicePathDisplayOwner", variable)
        self.assertIn("#else\n      int owner=GuDevicePathAmdDisplayOwner", variable)
        self.assertIn("#if defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_FIRMWARE_REATTACH)\n    if(GuDevicePathDisplayOwner", check)
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
        branch_end = self.app.index('#else\n#ifndef GU_INTERACTIVE_WINDOWS_ESP\n  stage="stage-windows-image"', branch_start)
        stop_branch = without_windows_handoff(self.app[branch_start:branch_end])
        self.assertNotIn("GuBootLoadWindows()", stop_branch)
        self.assertNotIn("GuBootWindows()", stop_branch)
        self.assertIn("OS_HANDOFF=NONE", stop_branch)
        self.assertIn("STOP_RELEASE_ONLY=REFUSED REASON=PRIOR_FAILED_GPU_STATE", self.boot)
        self.assertIn("s=GuBootReadPriorState(t,&boot_state,&boot_state_present);", self.app)

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
        branch_end = self.app.index('#else\n#ifndef GU_INTERACTIVE_WINDOWS_ESP\n  stage="stage-windows-image"', branch_start)
        stop_branch = without_windows_handoff(self.app[branch_start:branch_end])
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

    def test_stop_only_handoff_executes_only_after_success(self):
        start = self.app.rfind("#ifdef GU_STOP_RELEASE_ONLY_BENCH", 0,
                               self.app.index("STOP_RELEASE_ONLY_IMAGE=1"))
        end = self.app.index('#else\n#ifndef GU_INTERACTIVE_WINDOWS_ESP\n  stage="stage-windows-image"', start)
        branch = self.app[start:end] + "\n#endif\n"
        # Compile and execute the actual application success block with mock
        # services. A second GPU replaces the first active context. Failures
        # at each gate must prevent StartImage.
        harness = r'''
#include <assert.h>
#include <stddef.h>
typedef int EFI_STATUS;
typedef struct { int fault; } GU_PLATFORM;
typedef void EFI_SYSTEM_TABLE;
typedef void *EFI_HANDLE;
#define EFI_SUCCESS 0
#define EFI_NOT_READY 7
#define EFI_ABORTED 21
#define GU_STOP_RELEASE_ONLY_BENCH 1
#define GU_RUNTIME_SELECTION 1
static GU_PLATFORM first,last,*GuPlatformActive;
static int fail_at, calls[8], n;
static void GuLog(const char *fmt, ...) {(void)fmt;}
static void GuTracePhase(const char *stage) {(void)stage;}
static void GuBootSilence(void) {}
static void GuBootUnsilence(void) {}
static int check(int step) {calls[n++]=step;return fail_at==step?EFI_NOT_READY:0;}
static int GuExecuteStopReleaseSequence(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *t,EFI_HANDLE image,const char **stage) {
  (void)t;(void)image;(void)stage;assert(owner==&first);
  GuPlatformActive=&last;return check(1);
}
static int GuBootWatchdogCancel(void) {return check(2);}
static int GuBootBenchReportCompletion(void) {return 0;}
static int GuBootLoadWindows(void) {return check(3);}
static int GuVerifySelectedSequence(EFI_SYSTEM_TABLE *t) {(void)t;return check(4);}
static int GuBootClose(void) {return check(5);}
static int GuBootWindows(void) {return check(6);}
static void GuBootHalt(void) {(void)check(7);}
static int run(void) {
  EFI_STATUS s=0;GU_PLATFORM *owner=GuPlatformActive;
  EFI_SYSTEM_TABLE *t=NULL;EFI_HANDLE image=NULL;const char *stage=NULL;
''' + branch + r'''
failed: return s==0?EFI_NOT_READY:s;
}
static void reset(int fail) {n=0;fail_at=fail;first.fault=last.fault=0;GuPlatformActive=&first;}
int main(void) {
  reset(0);
#ifdef GU_STOP_RELEASE_WINDOWS_HANDOFF
  assert(run()==EFI_ABORTED && n==7);
  for(int i=0;i<7;i++)assert(calls[i]==i+1);
  for(int failure=1;failure<=5;failure++) {
    reset(failure);assert(run()!=EFI_SUCCESS && n==failure);
    for(int i=0;i<n;i++)assert(calls[i]==i+1);
  }
  reset(6);assert(run()==EFI_NOT_READY && n==7 && calls[6]==7);
  reset(0);last.fault=1;assert(run()!=EFI_SUCCESS && n==1);
#else
  assert(run()==EFI_SUCCESS && n==3 && calls[0]==1 && calls[1]==2 && calls[2]==5);
  reset(5);assert(run()!=EFI_SUCCESS && n==3);
#endif
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "stop_app.c"
            source.write_text(harness, encoding="utf-8")
            for enabled in (False, True):
                with self.subTest(handoff=enabled):
                    executable = Path(tmp) / ("handoff" if enabled else "return")
                    args = ["cc", "-std=c11", str(source), "-o", str(executable)]
                    if enabled:
                        args.insert(1, "-DGU_STOP_RELEASE_WINDOWS_HANDOFF")
                    subprocess.run(args, check=True, capture_output=True, text=True)
                    subprocess.run([str(executable)], check=True, capture_output=True, text=True)

    def test_handoff_variant_is_runtime_stop_only(self):
        variant = next(v for v in self.catalog["variants"]
                       if v["id"] == "app-stop-release-sequence-handoff")
        for switch in ("StopReleaseOnlyBench", "StopReleaseWindowsHandoff",
                       "FileOnlyBootLog", "RuntimeEsp", "RuntimeSelection",
                       "AcceptUnverifiedActuation"):
            self.assertTrue(variant["switches"][switch])
        self.assertEqual(variant["family"], "prepare")
        self.assertIn("OS_HANDOFF=WINDOWS_AFTER_SUCCESS", variant["attestations"])
        self.assertIn("FIRMWARE_TRANSACTION=0", variant["attestations"])
        self.assertIn("UNLOCK=0", variant["attestations"])
        self.assertIn("NEEDS_RUNTIME_SELECTION", self.sign_deploy)
        self.assertIn("/DGU_STOP_RELEASE_WINDOWS_HANDOFF", self.app_builder)
        self.assertIn("-StopReleaseWindowsHandoff requires the runtime Stop-only USB profile",
                      self.app_builder)

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
