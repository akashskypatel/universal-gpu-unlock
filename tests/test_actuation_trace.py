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
  * the trace line is flushed (through ``GuLog``) and mirrored to the console even
    while the image is otherwise silent, because ``GuBootSilence`` is what left
    the bench staring at a black screen;
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

    def test_the_console_mirror_does_not_honour_silence(self):
        body = self.boot.split("static VOID TraceSay(")[1].split("\nVOID GuTraceScreen")[0]
        self.assertIn("Table->ConOut->OutputString", body)
        # GuBootSay is the silenced path; the mirror must not go through it and
        # must not consult Silent, or the screen stays black and the fix is gone.
        self.assertNotIn("GuBootSay", body)
        self.assertNotIn("Silent", body)

    def test_the_trace_cannot_block_when_the_firmware_has_no_clock(self):
        body = self.boot.split("static BOOLEAN TraceClock(")[1].split("static const char *TraceElapsedText")[0]
        self.assertIn("if(!seconds||!rt||!rt->GetTime)return FALSE;", body)
        self.assertIn('return "NA";', self.boot)

    def test_the_vendor_wait_is_rate_limited_by_accumulated_delay(self):
        body = self.boot.split("VOID GuTraceDelay(UINT64 ticks) {")[1].split("\n}")[0]
        self.assertIn("if(ticks<TraceDelayShown+250000ULL)return;", body)
        self.assertIn('PHASE=vendor-wait ACCUMULATED_US=%llu', body)


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
