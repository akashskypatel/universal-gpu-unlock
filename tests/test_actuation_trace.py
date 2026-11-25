"""The waived path must say where it is while it is there.

RUN0011 is the reason this file exists. A waived one-target boot wrote thirteen
lines, went black, and stayed black for over a minute inside a call that printed
nothing until it returned. The log named no phase, the screen named nothing, and
the two together are indistinguishable from a fault: the operator could not tell a
slow USB read from a hung BAR read, and the next person reading the file cannot
tell either.

These tests pin the properties that make that impossible to reintroduce:

  * every stage string the application uses is also a trace line, emitted at the
    moment the stage is entered rather than after it returns;
  * the trace line is flushed through GuLog. Legacy console traces bypass
    silence; combined preparation traces stop at the actuation boundary;
    successor preparation uses the verified direct peer; file-only traces never call the global console;
  * the calls that can block -- config space, BAR0, the 1 MiB PROM copy, the
    vendor's delay loop -- name themselves first;
  * the vendor wait is rate-limited by accumulated delay, so tracing it cannot
    become the slow thing;
  * a stopped run prints its STOP line to the screen, not only to the USB file.

Nothing here executes firmware: the format strings, their order and the console
exception are read out of the sources, and the model side is pinned by the
simulator's own checks (``sim/sim_main.c``).
"""

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def read(name):
    return (SRC / name).read_text(encoding="utf-8")


class TheTraceNamesThePhaseBeforeItBlocks(unittest.TestCase):
    def setUp(self):
        self.port = read("port.h")
        self.boot = read("boot_io.c")

    def test_the_emitters_are_declared(self):
        for name in ("GuTraceReset", "GuTracePhase", "GuTraceDetail",
                     "GuTraceProgress", "GuTraceDelay", "GuTraceScreen",
                     "GuTraceStop"):
            self.assertIn("VOID %s(" % name, self.port, name)

    def test_the_phase_line_is_flushed_and_carries_step_and_elapsed(self):
        self.assertIn('GuLog("TRACE STEP=%u ELAPSED=%s PHASE=%s\\n"', self.boot)
        # Flushed: GuLog writes and flushes one line at a time, so a log that ends
        # mid-phase is a real stop rather than a buffer that was never written.
        self.assertIn("Log->Write(Log,&bytes,buffer)", self.boot)
        self.assertIn("Log->Flush(Log)", self.boot)

    def test_console_mirror_respects_combined_actuation_boundary(self):
        start = self.boot.index("static VOID TraceSay(")
        body = self.boot[start:self.boot.index("\nVOID GuTraceScreen", start)]
        app = read("app.c")
        self.assertLess(app.index("[OK] Target selection accepted."),
                        app.index("s=GuDiscoverSingleGpu(image,t)"))
        self.assertLess(app.index("GuBootSilence();", app.index("Operator U/P")),
                        app.index("s=GuExecutePreparedSequence("))
        execution = read("unlock_execution.c")
        self.assertNotIn("GuBootUnsilence", execution)
        advance = execution.split("static EFI_STATUS GuPrepareNextSelectedGpu(", 1)[1].split("#endif\n\n#ifndef", 1)[0]
        self.assertLess(advance.index("GuSelectNextGpu("), advance.index("GuBootTraceSuccessorPreparation(TRUE)"))
        self.assertLess(advance.index("GuBootTraceSuccessorPreparation(TRUE)"), advance.index("GuPrepareBoundGpu(next,stage)"))
        self.assertLess(advance.index("GuPrepareBoundGpu(next,stage)"), advance.index("GuBootTraceSuccessorPreparation(FALSE)"))
        peer = read("msi_release.c").split("EFI_STATUS GuMsiStopReleaseTryPeerConsole(", 1)[1]
        self.assertIn("if(verified==EFI_SUCCESS)GuBootTraceRememberPeer(survivor->handle,peer_text);", peer)
        # Execute the actual output function for each profile. A file-only
        # image must not print; combined must not use the global console after silence.
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef void VOID;
typedef uint16_t CHAR16;
typedef size_t UINTN;
typedef struct OUT OUT;
struct OUT { int (*OutputString)(OUT *,CHAR16 *); };
typedef struct { OUT *ConOut; } SYS;
static SYS *Table;
static unsigned Silent;
static unsigned calls;
#ifdef GU_FIRMWARE_REATTACH
static unsigned peer_calls, preparing;
static void TracePeerSay(CHAR16 *wide) {
    assert(wide[0]=='p');
    if(preparing)peer_calls++;
}
#endif
static int Output(OUT *out,CHAR16 *text) {
    (void)out;assert(text[0]=='p');calls++;return 0;
}
""" + body + r"""
int main(void) {
    OUT out={Output};SYS table={&out};Table=&table;
    Silent=0;TraceSay("preparation");
#ifdef GU_DXE_FILE_ONLY_OUTPUT
    assert(calls==0);
#else
    assert(calls==1);
#endif
    Silent=1;TraceSay("post-release");
#if defined(GU_DXE_FILE_ONLY_OUTPUT)
    assert(calls==0);
#elif defined(GU_FIRMWARE_REATTACH)
    assert(calls==1 && peer_calls==0);
    preparing=1;TraceSay("preparation");
    assert(calls==1 && peer_calls==1);
    preparing=0;TraceSay("post-release");
    assert(calls==1 && peer_calls==1);
#else
    assert(calls==2);
#endif
    Table=NULL;TraceSay("preparation");
    return 0;
}
"""
        for compiler in ("gcc", "clang"):
            if not shutil.which(compiler):
                continue
            for flags in ((), ("-DGU_FIRMWARE_REATTACH",),
                          ("-DGU_DXE_FILE_ONLY_OUTPUT",)):
                with self.subTest(compiler=compiler, flags=flags):
                    with tempfile.TemporaryDirectory() as tmp:
                        src = pathlib.Path(tmp) / "trace.c"
                        binary = pathlib.Path(tmp) / "trace"
                        src.write_text(harness, encoding="utf-8")
                        built = subprocess.run(
                            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                             *flags, str(src), "-o", str(binary)],
                            capture_output=True, text=True)
                        self.assertEqual(built.returncode, 0, built.stderr)
                        run = subprocess.run([str(binary)], capture_output=True, text=True)
                        self.assertEqual(run.returncode, 0, run.stderr)


    def test_successor_trace_validates_direct_peer_and_disables_on_failure(self):
        start = self.boot.index("static EFI_HANDLE TracePeerHandle;")
        end = self.boot.index("#endif\n\nstatic VOID TraceSay", start)
        actual = self.boot[start:end]
        harness = r"""
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef void VOID;
typedef void *EFI_HANDLE;
typedef uintptr_t UINTN;
typedef uint16_t CHAR16;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef int EFI_GUID;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID 1
typedef struct OUT OUT;
struct OUT { int (*OutputString)(OUT *,CHAR16 *); };
typedef OUT EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct { int (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **); } BS;
typedef struct { BS *BootServices; } SYS;
static SYS *Table;
static unsigned lookups,calls,fail,logs;
static OUT *reported;
static int lookup(EFI_HANDLE h,EFI_GUID *g,VOID **out) {
    assert(h==(void *)1 && *g==1); lookups++; *out=reported;
    return fail==1?2:0;
}
static int output(OUT *o,CHAR16 *text) {
    assert(o==reported && text[0]=='x'); calls++; return fail==2?2:0;
}
static int other_output(OUT *o,CHAR16 *text) { (void)o;(void)text;assert(0);return 1; }
static void GuLog(const char *s) { assert(s);logs++; }
""" + actual + r"""
int main(void) {
    OUT peer={output},other={other_output};BS bs={lookup};SYS sys={&bs};Table=&sys;
    CHAR16 text[]={'x',0};
    GuBootTraceRememberPeer((void *)1,&peer);reported=&peer;
    TracePeerSay(text);assert(!lookups&&!calls);
    GuBootTraceSuccessorPreparation(TRUE);
    TracePeerSay(text);assert(lookups==1&&calls==1);
    GuBootTraceSuccessorPreparation(FALSE);
    TracePeerSay(text);assert(lookups==1&&calls==1);
    GuBootTraceSuccessorPreparation(TRUE);
    TracePeerSay(text);assert(lookups==1&&calls==1);
    for(int mode=0;mode<4;mode++) {
        GuBootTraceRememberPeer((void *)1,&peer);
        reported=mode==0?&other:&peer;
        fail=mode==1?1:mode==2?2:0;
        if(mode==3)peer.OutputString=other_output;
        unsigned before=calls, before_lookup=lookups;
        TracePeerSay(text);assert(lookups==before_lookup+1);
        assert(calls==before+(mode==2));
        TracePeerSay(text);assert(lookups==before_lookup+1);
        peer.OutputString=output;
    }
    assert(logs==4);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "peer.c"
            binary = pathlib.Path(tmp) / "peer"
            src.write_text(harness, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                if not shutil.which(compiler):
                    continue
                built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                        str(src), "-o", str(binary)], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                ran = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(ran.returncode, 0, ran.stderr)


    def test_combined_peer_completion_warns_about_next_preparation(self):
        release = read("msi_release.c")
        notice = release.split("static EFI_STATUS BenchShowReattachSuccess(VOID)", 1)[1].split(
            "#define GuWarningGpuCount", 1)[0]
        self.assertIn("NEXT STAGE MAY TAKE MINUTES", notice)
        self.assertIn("Plan.reattach_verified", notice)
        self.assertNotIn("->ConOut", notice)

    def test_the_trace_cannot_block_when_the_firmware_has_no_clock(self):
        body = self.boot.split("static BOOLEAN TraceClock(")[1].split("static const char *TraceElapsedText")[0]
        self.assertIn("if(!seconds||!rt||!rt->GetTime)return FALSE;", body)
        self.assertIn('return "NA";', self.boot)

    def test_optional_timestamp_clock_reports_only_valid_intervals(self):
        start = self.boot.index("typedef struct {UINT64 Frequency,EndValue;}")
        end = self.boot.index("static const char *TraceElapsedText(", start)
        actual = self.boot[start:end]
        host = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef void VOID;
typedef int BOOLEAN;
typedef int EFI_STATUS;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFIAPI
typedef struct {unsigned a,b,c;unsigned char d[8];} EFI_GUID;
typedef struct {
  EFI_STATUS (*LocateProtocol)(EFI_GUID *,void *,void **);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
static EFI_SYSTEM_TABLE *Table;
static size_t TraceFormat(char *out,size_t n,const char *fmt,...){
  va_list args;va_start(args,fmt);int result=vsnprintf(out,n,fmt,args);va_end(args);return (size_t)result;
}
''' + actual + r'''
static GU_TIMESTAMP_PROTOCOL protocol;
static int unavailable,properties_fail;
static UINT64 ticks;
static GU_TIMESTAMP_PROPERTIES properties={1000000ULL,UINT64_MAX};
static UINT64 Sample(void){return ticks;}
static UINT64 Other(void){return ticks;}
static EFI_STATUS Properties(GU_TIMESTAMP_PROPERTIES *p){*p=properties;return properties_fail;}
static EFI_STATUS Locate(EFI_GUID *g,void *r,void **p){
  (void)r;if(g!=&TimestampGuid||unavailable)return 1;*p=&protocol;return 0;
}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
  EFI_BOOT_SERVICES bs={Locate};EFI_SYSTEM_TABLE table={&bs};Table=&table;
  protocol.GetTimestamp=Sample;protocol.GetProperties=Properties;
  UINT64 start=0,end=0,ms=0;char value[24];
  unavailable=1;CHECK(!TraceFineInit()&&!TraceFineClock(&start));
  unavailable=0;properties_fail=1;CHECK(!TraceFineInit());
  properties_fail=0;properties.Frequency=0;CHECK(!TraceFineInit());
  properties.Frequency=1000000;properties.EndValue=1000;CHECK(!TraceFineInit());
  properties.EndValue=UINT64_MAX;CHECK(TraceFineInit());
  TraceBootFineValid=TraceFineClock(&TraceBootFineStart);CHECK(TraceBootFineValid);
  ticks=100000;CHECK(TraceFineClock(&start));
  ticks=350000;CHECK(TraceFineClock(&end)&&TraceFineDelta(start,end,&ms)&&ms==250);
  CHECK(strcmp(TraceMsText(TRUE,ms,value,sizeof(value)),"250")==0);
  CHECK(!TraceFineDelta(end,start,&ms));
  CHECK(!TraceFineDelta(0,86400ULL*properties.Frequency,&ms));
  CHECK(strcmp(TraceMsText(FALSE,0,value,sizeof(value)),"NA")==0);
  protocol.GetTimestamp=Other;CHECK(!TraceFineClock(&end));
  protocol.GetTimestamp=Sample;unavailable=1;CHECK(!TraceFineClock(&end));
  return 0;
}
'''
        if not shutil.which("gcc"):
            self.skipTest("gcc unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            source = pathlib.Path(tmp) / "timestamp.c"
            binary = pathlib.Path(tmp) / "timestamp"
            source.write_text(host, encoding="utf-8")
            built = subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_vendor_wait_is_rate_limited_by_accumulated_delay(self):
        body = self.boot.split("VOID GuTraceDelay(UINT64 ticks) {")[1].split("\n}")[0]
        self.assertIn("if(ticks<TraceDelayShown+250000ULL)return;", body)
        self.assertIn('PHASE=vendor-wait ACCUMULATED_US=%llu', body)

    def test_long_vendor_waits_pulse_on_screen_and_asset_reads_count_up(self):
        delay = self.boot.split("VOID GuTraceDelay(UINT64 ticks) {")[1].split("\n}")[0]
        self.assertIn('".","..","..."', delay)
        self.assertIn('"\\r[gu] %u vendor-wait %s', delay)
        self.assertIn("TraceSay(line);", delay)
        self.assertIn('GuTraceProgress("asset-read-bytes",at,expected)', self.boot)
        self.assertIn('"[gu] %s %u of %u\\r\\n"', self.boot)
        stop = self.boot.split("VOID GuTraceStop(")[1].split("\n}")[0]
        self.assertIn("if(TraceSpinnerActive)TraceSay(\"\\r\\n\");", stop)


class EveryStageIsAlsoATraceLine(unittest.TestCase):
    """`*stage=` is what a refusal prints at the end; the trace is what a stall
    prints at the moment it stalls. One macro, so they cannot drift."""

    def test_the_macro_sets_and_traces_together(self):
        # It lives in the header, so a harness that splices one function out of
        # its file still gets the same definition (tests/test_unlock_execution_
        # contract.py does exactly that).
        self.assertIn("#define GU_PHASE(NAME) do { *stage=(NAME); GuTracePhase(NAME); } while(0)",
                      read("boot_io.h"))
        self.assertIn("#define GU_TRACE_BEGIN() GuTraceReset()", read("boot_io.h"))
        self.assertIn('#include "boot_io.h"', read("unlock_execution.c"))

    def test_no_stage_in_the_transaction_is_set_behind_the_macro_s_back(self):
        source = read("unlock_execution.c")
        # The preparation's three stages are converted too, so no bare assignment
        # remains anywhere in this file.
        self.assertNotIn('*stage="', source)
        for phase in ("preflight", "compatibility-reserve", "driver-release",
                      "cold-entry", "compatibility-open",
                      "shared-firmware-assets", "firmware-transaction",
                      "release-exclusive-claim", "selected-gpu-state-begin",
                      "selected-gpu-state-finish", "assets",
                      "read-only-diagnostic", "read-only-cold-profile"):
            self.assertIn('GU_PHASE("%s")' % phase, source, phase)

    def test_the_application_traces_its_own_stages_and_the_operator_key(self):
        source = read("app.c")
        for phase in ("prepare", "stage-windows-image", "manual-choice",
                      "close-USB-log", "start-Windows"):
            self.assertIn(phase, source)
        self.assertIn('GuTraceDetail("manual-choice","KEY=%c TRANSACTION=%u S3_SCRIPT=%u"', source)
        self.assertIn('GuTraceDetail("prepare","STATUS=%#llx PROFILE_VERIFIED=%u PROM_VERIFIED=%u"', source)

    def test_a_stopped_run_says_so_on_the_screen(self):
        source = read("app.c")
        self.assertIn('GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\\n"', source)
        stop = source.index("GuTraceStop(stage,(UINT64)s);")
        self.assertGreater(stop, source.index("STOP: stage=%s"))


class TheTimerIsReportedBeforeTheLongBand(unittest.TestCase):
    """The bound is the only thing that ends a hung preparation on its own, so a
    run that stalls before the operator menu still has to say whether this boot
    had one. RUN0011 did not: its watchdog line would have been printed after the
    keypress it never reached."""

    def test_the_bound_is_reported_before_the_preparation(self):
        source = read("app.c")
        bound = source.index('WATCHDOG BOUND=%u SECONDS ACTIVE=1 PHASE=pre-prepare')
        prepare = source.index('GuTracePhase("prepare");')
        self.assertLess(bound, prepare)
        self.assertIn('WATCHDOG BOUND=0 SECONDS ACTIVE=0 REASON=NO_BUILD_THRESHOLD PHASE=pre-prepare', source)


class TheCallsThatCanBlockNameThemselves(unittest.TestCase):
    """Ordered, not merely present: the trace has to come first or it reports the
    phase that just finished, which is exactly what RUN0011's log already did."""

    def assert_traced_before(self, source, phase, call):
        traced = source.index('GuTracePhase("%s")' % phase)
        self.assertLess(traced, source.index(call), "%s must be traced before %s" % (phase, call))

    def test_the_diagnostic_names_each_of_its_three_device_calls(self):
        source = read("profile.c")
        self.assert_traced_before(source, "diag-config", "BOOLEAN config_ok=ConfigFor(owner);")
        self.assert_traced_before(source, "diag-registers", "BOOLEAN regs_ok=config_ok&&RegistersFor(")
        self.assert_traced_before(source, "diag-assets", "BOOLEAN assets_ok=GuAd104AssetsValid(owner);")
        self.assert_traced_before(source, "diag-prom-copy", "GuCopyFromIo(owner,prom,")
        self.assert_traced_before(source, "diag-stability", "BOOLEAN cfg2=ConfigFor(owner);")

    def test_the_profile_read_names_itself_too(self):
        source = read("profile.c")
        self.assert_traced_before(source, "profile-prom-copy", "GuCopyFromIo(&Gu,prom,")
        self.assert_traced_before(source, "profile-latch-selectors", "GuLatchEntrySelectors(ss0,ss1)")

    def test_the_asset_band_names_each_file_it_opens(self):
        source = read("boot_io.c")
        band = source.split('GuTracePhase("asset-load");')[1].split("return status;")[0]
        self.assertIn('GuTraceDetail("asset-read","INDEX=%u NAME=%s BYTES=%u"', band)
        self.assertIn('GuTraceProgress("asset-read"', band)

    def test_the_prom_copy_reports_progress_while_it_copies(self):
        source = read("compat.c")
        self.assertIn('GuTraceProgress("io-copy",Done,N)', source)
        # One line per 64 KiB, not per chunk: 1024 chunks of 1 KiB would bury the
        # log it is supposed to make readable.
        self.assertIn("if((Done&0xffffU)==0)", source)

    def test_the_vendor_delay_hook_reports_through_the_same_counter(self):
        self.assertIn("GuTraceDelay(owner->ticks);", read("compat.c"))


class TheTraceIsInTheLogAndNotOnlyInTheSource(unittest.TestCase):
    """The model side: the simulator asserts the tokens appear in a captured
    application log for the transaction scenarios, which is the only place this
    repository can show the lines being emitted rather than merely written."""

    def test_the_simulator_requires_the_actuating_trace(self):
        source = (ROOT / "sim" / "sim_main.c").read_text(encoding="utf-8")
        self.assertIn('if (strcmp(stage, "transaction") == 0) {', source)
        for token in ('Check(SimLogContains("TRACE STEP=")',
                      'PHASE=driver-release',
                      'PHASE=firmware-transaction'):
            self.assertIn(token, source, token)


if __name__ == "__main__":
    unittest.main()
