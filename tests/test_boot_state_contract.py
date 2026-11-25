"""Contract tests for the persisted boot record and fail-closed recovery.

An unmatched INTENT prevents a repeated DXE transaction and causes the USB app
recovery route to run before GPU discovery. These contracts do not claim
physical hardware recovery evidence.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = ROOT / "tests"
APP = (SRC / "app.c").read_text(encoding="utf-8")
DXE = (SRC / "dxe_main.c").read_text(encoding="utf-8")
STATE = (SRC / "boot_state.c").read_text(encoding="utf-8")
HEADER = (SRC / "boot_state.h").read_text(encoding="utf-8")
BOOT_IO = (SRC / "boot_io.c").read_text(encoding="utf-8")
ENTRYPOINTS = (("app.c", APP), ("dxe_main.c", DXE), ("boot_io.c", BOOT_IO))


class RecoveryPolicy(unittest.TestCase):
    def test_dxe_latches_out_interrupted_attempt_before_discovery(self):
        recovery = DXE.index("DXE_RECOVERY=FAILED_OR_INTERRUPTED")
        disable = DXE.index("GuDisableDriverOption(t)")
        discover = DXE.index("GuDiscoverSingleGpu(image, t)")
        self.assertLess(recovery, discover)
        self.assertLess(disable, discover)
        self.assertIn("ACTION=SKIP_GPU", DXE)
        self.assertIn("GuBootStateRequiresRecovery(boot_state_present?&boot_state:NULL)", DXE)

    def test_usb_recovery_removes_dxe_before_discovery(self):
        recovery = APP.index("USB_RECOVERY=FAILED_OR_INTERRUPTED")
        remove = APP.index("GuRemoveDriverOption(t)")
        discover = APP.index("GuDiscoverSingleGpu(image,t)")
        self.assertLess(recovery, discover)
        self.assertLess(remove, discover)
        self.assertNotIn("GuBootStateDeleteReviewed(", APP)
        self.assertIn("BOOT_STATE_PRESERVED=1 AUTO_RETRY=0", APP)
        self.assertNotIn("ResetSystem(", APP[recovery:discover])
        self.assertIn("GuBootWindows()", APP)
        self.assertIn("GuBootStateRequiresRecovery(boot_state_present?&boot_state:NULL)", APP)

    def test_preparation_recovery_refuses_without_persistent_write(self):
        helper = BOOT_IO.split("EFI_STATUS GuBootReadPriorState(", 1)[1].split("\n}", 1)[0]
        self.assertIn("#if defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)", helper)
        self.assertIn("GuBootStateRequiresRecovery(*present?state:NULL)", helper)
        self.assertIn("PREPARATION_ONLY=REFUSED REASON=PRIOR_FAILED_GPU_STATE", helper)
        self.assertIn("STOP_RELEASE_ONLY=REFUSED REASON=PRIOR_FAILED_GPU_STATE", helper)
        self.assertIn("return closed==EFI_SUCCESS?EFI_ABORTED:closed;", helper)
        for forbidden in ("GuRemoveDriverOption", "GuDisableDriverOption", "GuBootWindows", "GuBootStateWrite", "SetVariable"):
            self.assertNotIn(forbidden, helper)
        for name, source, mutation in (("app", APP, "GuRemoveDriverOption(t)"),
                                       ("dxe", DXE, "GuDisableDriverOption(t)")):
            read = source.index("s=GuBootReadPriorState(t,&boot_state,&boot_state_present);")
            refusal = source.index("if(s!=EFI_SUCCESS)return s;", read)
            guard = source.index("#if !defined(GU_STOP_RELEASE_ONLY_BENCH) && !defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)", refusal)
            recovery = source.index("GuBootStateRequiresRecovery(boot_state_present?&boot_state:NULL)", guard)
            end = source.index("#endif", recovery)
            self.assertLess(read, refusal, name)
            self.assertLess(recovery, source.index(mutation, recovery), name)
            self.assertLess(source.index(mutation, recovery), end, name)
        maintenance = (SRC / "boot_state_maintenance.c").read_text(encoding="utf-8")
        self.assertIn("if(!GuBootStateRequiresRecovery(&reviewed))", maintenance)
        self.assertIn("if(!GuBootStateRequiresRecovery(Reviewed))", STATE)

    def test_recovery_only_disables_or_removes_owned_driver_option(self):
        self.assertIn("EFI_STATUS GuDisableDriverOption", (SRC / "driver_nvram.c").read_text())
        self.assertIn("return RemoveOwnedFromOrder(rs, order, count, own);", (SRC / "driver_nvram.c").read_text())
        self.assertIn("GuRemoveDriverOption(t)", APP)
        self.assertNotIn("GuExecutePreparedSequence", DXE.split("DXE_RECOVERY=FAILED_OR_INTERRUPTED", 1)[1].split("const char *stage", 1)[0])


class TwoPhaseWrites(unittest.TestCase):
    def test_intent_precedes_the_sequence_and_outcome_follows(self):
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            intent = text.index("GU_BOOT_STATE_INTENT")
            outcome = text.rindex("GU_BOOT_STATE_OUTCOME")
            sequence = min(text.index("GuExecutePreparedSequence"),
                           text.index("GuExecuteUnlock") if "GuExecuteUnlock" in text else 10**9)
            self.assertLess(intent, sequence, name)
            self.assertLess(sequence, outcome, name)

    def test_record_is_read_inside_boot_open_and_before_discovery(self):
        # One read in GuBootOpen covers the application, the DXE driver and the
        # simulator, which never runs UefiMain.
        boot_open = BOOT_IO.split("EFI_STATUS GuBootOpen", 1)[1].split("EFI_STATUS GuBootLoadAssets", 1)[0]
        self.assertIn("GuBootStateRead(Table,&previous)", boot_open)
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            self.assertLess(text.index("GuBootReadPriorState"), text.index("GuDiscoverSingleGpu"), name)

    def test_record_failures_block_active_execution_or_handover(self):
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            intent = text.index("s=GuBootStateRecord(")
            sequence = text.index("s=GuExecutePreparedSequence(", intent)
            outcome = text.index("EFI_STATUS recorded=GuBootStateRecord(", sequence)
            self.assertIn("if(s!=EFI_SUCCESS||!GuBootLogOpen())", text[intent:sequence], name)
            self.assertIn("if(s==EFI_SUCCESS&&recorded!=EFI_SUCCESS)s=recorded;", text[outcome:], name)
            close = text.index("s=GuBootClose()", sequence)
            self.assertLess(close, outcome, name)
            cancel = text.index("s=GuCompletePreparedSequence(s,&owner);", sequence)
            self.assertLess(cancel, close, name)
            self.assertNotIn("(VOID)GuBootStateRecord", text, name)
            self.assertIn("s=GuBootReadPriorState(t,&boot_state,&boot_state_present);\n  if(s!=EFI_SUCCESS)return s;", text[:intent], name)
        self.assertIn("GuBootStateRead(Table,&confirmed)", STATE)
        self.assertIn("memcmp(&confirmed,State,sizeof(confirmed))==0", STATE)

    def test_waived_active_attempt_preserves_intent_across_handover(self):
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            seq = text.index("s=GuExecutePreparedSequence(")
            close = text.index("s=GuBootClose()", seq)
            notice = text.index("EXPERIMENT_RESTART_BARRIER=RETAINED", seq)
            self.assertLess(notice, close, name)
            self.assertIn("AUTO_RETRY=0", text[notice:close], name)
        app = APP[APP.index('stage="close-USB-log";GuTracePhase(stage);s=GuBootClose();', APP.index("s=GuExecutePreparedSequence(")):]
        legacy = app.index("#ifndef GU_ACTIVE_UNVERIFIED_ACTUATION")
        outcome = app.index("GU_BOOT_STATE_OUTCOME", legacy)
        guard_end = app.index("#endif", outcome)
        start = app.index("s=GuBootWindows()", guard_end)
        self.assertLess(legacy, outcome)
        self.assertLess(outcome, guard_end)
        self.assertLess(guard_end, start)
        returned = app[start:app.index("#endif /* !GU_STOP_RELEASE_ONLY_BENCH */", start)]
        self.assertIn("#ifdef GU_ACTIVE_UNVERIFIED_ACTUATION", returned)
        self.assertLess(returned.index("GuBootHalt();return s;"), returned.index("recorded=GuBootStateRecord("))
        self.assertIn("#if !defined(GU_STOP_RELEASE_ONLY_BENCH) && !defined(GU_ACTIVE_UNVERIFIED_ACTUATION)", DXE)
        self.assertNotIn("GuBootStateDeleteReviewed(", APP)
        self.assertNotIn("GuBootStateDeleteReviewed(", DXE)

    def test_interrupted_boot_is_reported(self):
        self.assertIn("BOOT_STATE WARNING=previous attempt interrupted or awaiting owner acceptance", BOOT_IO)
        # A corrupt record is reported; active entry independently refuses it.
        self.assertIn("BOOT_STATE PRESENT=0 READ_STATUS=%#llx VALID=0", BOOT_IO)
        helper = BOOT_IO.split("EFI_STATUS GuBootReadPriorState(", 1)[1].split("\n}", 1)[0]
        self.assertIn("GuBootStateRead(table,state)", helper)
        self.assertIn("if(!*present&&status!=EFI_NOT_FOUND)", helper)
        self.assertIn("return closed==EFI_SUCCESS?status:closed;", helper)


class Validation(unittest.TestCase):
    def test_short_or_corrupt_records_are_refused(self):
        self.assertIn("if (size != sizeof(buffer)) return EFI_COMPROMISED_DATA;", STATE)
        self.assertIn("if (!Valid(&buffer)) return EFI_COMPROMISED_DATA;", STATE)
        self.assertIn("if(s==EFI_NOT_FOUND)return EFI_SUCCESS;", STATE)
        self.assertIn("return s==EFI_SUCCESS?EFI_COMPROMISED_DATA:s;", STATE)

    def test_host_unit_passes(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("A host C compiler is required")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "test_boot_state"
            build = subprocess.run(
                [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-DGU_BOOT_STATE_HOST_TEST",
                 "-fsanitize=undefined", "-fno-sanitize-recover=all",
                 "-I", str(SRC), "-I", str(TESTS),
                 str(TESTS / "test_boot_state.c"), str(SRC / "boot_state.c"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            done = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn("checks OK", done.stdout)


    def test_actual_shared_admission_read_and_refusal(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("A host C compiler is required")
        helper = "EFI_STATUS GuBootReadPriorState(" + BOOT_IO.split("EFI_STATUS GuBootReadPriorState(", 1)[1].split("\n}", 1)[0] + "\n}\n"
        predicate = "int GuBootStateRequiresRecovery(" + STATE.split("int GuBootStateRequiresRecovery(", 1)[1].split("\n}", 1)[0] + "\n}\n"
        source = r"""
#include <stddef.h>
#include <string.h>
#include <wchar.h>
typedef int EFI_STATUS;
typedef int BOOLEAN;
typedef struct { int unused; } EFI_SYSTEM_TABLE;
typedef struct { unsigned Phase; EFI_STATUS Status; } GU_BOOT_STATE;
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 7
#define EFI_NOT_FOUND 14
#define EFI_ABORTED 21
#define EFI_COMPROMISED_DATA 33
#define GU_BOOT_STATE_IDLE 0U
#define GU_BOOT_STATE_INTENT 1U
#define GU_BOOT_STATE_OUTCOME 2U
static EFI_STATUS read_status,close_status;
static GU_BOOT_STATE record;
static EFI_SYSTEM_TABLE table;
static int reads,closes,says;
static const char *logged;
static EFI_STATUS GuBootStateRead(EFI_SYSTEM_TABLE *t,GU_BOOT_STATE *out) {
  if(t!=&table)return EFI_DEVICE_ERROR;
  reads++;*out=record;return read_status;
}
static void GuLog(const char *text,...) {logged=text;}
static void GuBootSay(const wchar_t *text) {(void)text;says++;}
static EFI_STATUS GuBootClose(void) {closes++;return close_status;}
""" + predicate + helper + r"""
#define CHECK(x) do {if(!(x))return __LINE__;} while(0)
static int Run(EFI_STATUS read, unsigned phase, EFI_STATUS previous,
               EFI_STATUS close, EFI_STATUS expected, int expect_close,
               int expect_say,const char *token) {
  GU_BOOT_STATE out={0};BOOLEAN present=-1;
  reads=closes=says=0;logged=NULL;read_status=read;close_status=close;
  record.Phase=phase;record.Status=previous;
  CHECK(GuBootReadPriorState(&table,&out,&present)==expected);
  CHECK(reads==1&&closes==expect_close&&says==expect_say);
  CHECK(present==(read==EFI_SUCCESS));
  if(present)CHECK(out.Phase==phase&&out.Status==previous);
  if(token)CHECK(logged&&strstr(logged,token));else CHECK(!logged);
  return 0;
}
int main(void) {
  CHECK(!Run(EFI_NOT_FOUND,GU_BOOT_STATE_INTENT,EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,EFI_SUCCESS,0,0,NULL));
  CHECK(!Run(EFI_SUCCESS,GU_BOOT_STATE_IDLE,EFI_SUCCESS,EFI_DEVICE_ERROR,EFI_SUCCESS,0,0,NULL));
  CHECK(!Run(EFI_SUCCESS,GU_BOOT_STATE_OUTCOME,EFI_SUCCESS,EFI_SUCCESS,EFI_SUCCESS,0,0,NULL));
  CHECK(!Run(EFI_COMPROMISED_DATA,GU_BOOT_STATE_IDLE,EFI_SUCCESS,EFI_SUCCESS,EFI_COMPROMISED_DATA,1,0,"BOOT_STATE=REFUSED"));
  CHECK(!Run(EFI_DEVICE_ERROR,GU_BOOT_STATE_IDLE,EFI_SUCCESS,EFI_ABORTED,EFI_ABORTED,1,0,"BOOT_STATE=REFUSED"));
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_STOP_RELEASE_ONLY_BENCH)
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  const char *token="PREPARATION_ONLY=REFUSED";
#else
  const char *token="STOP_RELEASE_ONLY=REFUSED";
#endif
  CHECK(!Run(EFI_SUCCESS,GU_BOOT_STATE_INTENT,EFI_SUCCESS,EFI_SUCCESS,EFI_ABORTED,1,1,token));
  CHECK(!Run(EFI_SUCCESS,GU_BOOT_STATE_OUTCOME,EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,1,1,token));
#else
  CHECK(!Run(EFI_SUCCESS,GU_BOOT_STATE_INTENT,EFI_SUCCESS,EFI_DEVICE_ERROR,EFI_SUCCESS,0,0,NULL));
  CHECK(!Run(EFI_SUCCESS,GU_BOOT_STATE_OUTCOME,EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,EFI_SUCCESS,0,0,NULL));
#endif
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            code = Path(tmp) / "prior_state.c"
            code.write_text(source, encoding="utf-8")
            for flag in (None, "GU_ACTIVE_SEQUENCE_PREPARE_ONLY", "GU_STOP_RELEASE_ONLY_BENCH"):
                with self.subTest(flag=flag):
                    binary = Path(tmp) / "prior_state"
                    args = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
                    if flag:
                        args.append("-D" + flag)
                    result = subprocess.run(args + [str(code), "-o", str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    done = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(done.returncode, 0, done.stdout + done.stderr + f" line {done.returncode}")


class BuildWiring(unittest.TestCase):
    def test_both_builders_and_the_simulator_compile_the_module(self):
        for name in ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1"):
            from test_build_source_lists import builder_sources
            self.assertIn("boot_state.c", builder_sources(name), name)
        self.assertIn('"boot_state.c"', (ROOT / "tools" / "run_simulation.py").read_text(
            encoding="utf-8"))

    def test_attributes_survive_a_reboot(self):
        self.assertIn("EFI_VARIABLE_NON_VOLATILE", STATE)


class FixedApplicationOperations(unittest.TestCase):
    def test_actual_operation_dispatch_and_administrative_completion(self):
        """Compile the actual operation handlers without firmware or GPU access."""
        helper_start = APP.index("#ifdef GU_APP_OPERATION\n/* A fixed I/R image")
        helper_end = APP.index("EFI_STATUS EFIAPI UefiMain(", helper_start)
        dispatch_start = APP.index('#ifdef GU_APP_OPERATION\n  stage="build-selected-operation"')
        dispatch_end = APP.index("  if(!unlock) {", dispatch_start)
        guard_start = APP.index("#if defined(GU_APP_OPERATION)")
        guard_end = APP.index('#include "compat/compat.h"', guard_start)
        source = r"""
#include <stddef.h>
#include <wchar.h>
typedef int EFI_STATUS;
typedef int BOOLEAN;
typedef wchar_t CHAR16;
typedef void VOID;
typedef void *EFI_HANDLE;
#define EFI_SUCCESS 0
#define EFI_NOT_READY 5
#define EFI_DEVICE_ERROR 7
#define EFI_NOT_FOUND 14
#define EfiResetWarm 1
typedef struct { int fault; } GU_PLATFORM;
typedef struct { EFI_STATUS (*Stall)(unsigned); } BOOT;
typedef struct { void (*ResetSystem)(int,EFI_STATUS,unsigned,void *); } RUNTIME;
typedef struct { BOOT *BootServices; RUNTIME *RuntimeServices; } EFI_SYSTEM_TABLE;
static GU_PLATFORM platform,peer;
static GU_PLATFORM *GuPlatformActive;
static int menus,installs,removes,resets,closes,halts,cancels;
static int log_open,watchdog_status,operation_status,close_status;
static CHAR16 menu_key;
void GuLog(const char *fmt,...) {(void)fmt;}
void GuTracePhase(const char *stage) {(void)stage;}
void GuTraceDetail(const char *stage,const char *fmt,...) {(void)stage;(void)fmt;}
void GuBootSay(const CHAR16 *text) {(void)text;}
void GuBootHalt(void) {halts++;}
EFI_STATUS GuBootChoose(CHAR16 *choice) {menus++;*choice=menu_key;return EFI_SUCCESS;}
EFI_STATUS GuBootWatchdogCancel(void) {cancels++;return watchdog_status;}
int GuBootLogOpen(void) {return log_open;}
EFI_STATUS GuBootClose(void) {closes++;return close_status;}
EFI_STATUS GuInstallDriverOption(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  (void)image;(void)t;installs++;return operation_status;
}
EFI_STATUS GuRemoveDriverOption(EFI_SYSTEM_TABLE *t) {(void)t;removes++;return operation_status;}
const char *DxeInstallReason(EFI_STATUS status) {(void)status;return "mock";}
EFI_STATUS Stall(unsigned us) {(void)us;return EFI_SUCCESS;}
void ResetSystem(int type,EFI_STATUS status,unsigned size,void *data) {
  (void)type;(void)status;(void)size;(void)data;resets++;
}
static BOOT boot={Stall};
static RUNTIME rt={ResetSystem};
static EFI_SYSTEM_TABLE table={&boot,&rt};
""" + APP[guard_start:guard_end] + APP[helper_start:helper_end] + r"""
static int Run(void) {
  GU_PLATFORM *owner=&platform;
  EFI_SYSTEM_TABLE *t=&table;
  EFI_HANDLE image=&table;
  CHAR16 choice=L'S';
  EFI_STATUS s=EFI_SUCCESS;
  const char *stage="stage-windows-image";
""" + APP[dispatch_start:dispatch_end] + r"""
  /* Observe the existing U/P/S dispatch, not the transaction itself. */
  return unlock?(enable_s3?101:100):102;
failed:
  return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
}
static void Reset(CHAR16 key) {
  GuPlatformActive=&platform;platform.fault=0;menu_key=key;
  menus=installs=removes=resets=closes=halts=cancels=0;
  log_open=1;watchdog_status=operation_status=close_status=EFI_SUCCESS;
}
#define CHECK(c) do {if(!(c))return __LINE__;} while(0)
int main(void) {
#ifdef GU_APP_OPERATION
  CHAR16 key=(CHAR16)GU_APP_OPERATION;
  Reset(L'S');
  int result=Run();
  CHECK(menus==0);
  if(key==L'U'||key==L'P') {
    CHECK(result==(key==L'P'?101:100));
    CHECK(installs==0&&removes==0&&resets==0&&closes==0&&halts==0&&cancels==0);
  } else {
    CHECK(result==EFI_SUCCESS&&installs==(key==L'I')&&removes==(key==L'R'));
    CHECK(resets==0&&closes==1&&halts==1&&cancels==1);
    Reset(L'S');watchdog_status=EFI_DEVICE_ERROR;
    CHECK(Run()==EFI_DEVICE_ERROR&&installs==0&&removes==0&&resets==0);
    Reset(L'S');log_open=0;
    CHECK(Run()==EFI_DEVICE_ERROR&&installs==0&&removes==0&&resets==0);
    Reset(L'S');operation_status=EFI_NOT_FOUND;
    CHECK(Run()==EFI_NOT_FOUND&&halts==1&&closes==1&&resets==0);
    Reset(L'S');close_status=EFI_DEVICE_ERROR;
    CHECK(Run()==EFI_DEVICE_ERROR&&halts==1&&closes==1&&resets==0);
  }
  Reset(L'S');GuPlatformActive=&peer;
  CHECK(Run()==EFI_NOT_READY&&menus==0&&installs==0&&removes==0&&resets==0);
  Reset(L'S');platform.fault=1;
  CHECK(Run()==EFI_NOT_READY&&menus==0&&installs==0&&removes==0&&resets==0);
#else
  Reset(L'U');CHECK(Run()==100&&menus==1&&installs==0&&removes==0);
  Reset(L'P');CHECK(Run()==101&&menus==1&&installs==0&&removes==0);
  Reset(L'S');CHECK(Run()==102&&menus==1&&installs==0&&removes==0);
  Reset(L'I');CHECK(Run()==EFI_SUCCESS&&menus==1&&installs==1&&removes==0&&resets==1);
  Reset(L'R');CHECK(Run()==EFI_SUCCESS&&menus==1&&installs==0&&removes==1&&resets==1);
  Reset(L'I');GuPlatformActive=&peer;
  CHECK(Run()==EFI_DEVICE_ERROR&&menus==1&&installs==0&&removes==0&&resets==0);
  Reset(L'R');platform.fault=1;
  CHECK(Run()==EFI_DEVICE_ERROR&&menus==1&&installs==0&&removes==0&&resets==0);
#endif
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "operations.c"
            code.write_text(source, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                cc = shutil.which(compiler)
                if not cc:
                    self.skipTest("Host compilers are required")
                binary = Path(directory) / compiler
                for key in (None, "U", "P", "I", "R"):
                    with self.subTest(compiler=compiler, operation=key):
                        flags = [] if key is None else [
                            "-DGU_ACTIVE_UNVERIFIED_ACTUATION=1", f"-DGU_APP_OPERATION={ord(key)}"]
                        built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                                "-pedantic", *flags, str(code), "-o", str(binary)],
                                               capture_output=True, text=True)
                        self.assertEqual(built.returncode, 0, built.stderr)
                        run = subprocess.run([str(binary)], capture_output=True, text=True)
                        self.assertEqual(run.returncode, 0, run.stderr)


class Maintenance(unittest.TestCase):
    def test_independent_image_requires_review_and_confirmation(self):
        source = (SRC / "boot_state_maintenance.c").read_text(encoding="utf-8")
        builder = (SRC / "Build-Boot-State-Maintenance.ps1").read_text(encoding="utf-8")
        self.assertIn("$sources=@('boot_state_maintenance.c','boot_state.c','format.c','runtime.c')", builder)
        self.assertIn("Reviewed record bytes: %s", source)
        self.assertIn("s=GuBootStateRead(t,&reviewed);", source)
        confirm = source.index("s=Confirm(t);")
        watchdog = source.index("s=t->BootServices->SetWatchdogTimer(0,0,0,NULL);")
        delete = source.index("s=GuBootStateDeleteReviewed(t,&reviewed);")
        self.assertLess(source.index("Reviewed record bytes"), confirm)
        self.assertLess(confirm, watchdog)
        self.assertLess(watchdog, delete)
        self.assertIn("return s;", source[confirm:watchdog])
        self.assertIn("if(s!=EFI_SUCCESS)return s;", source[watchdog:delete])
        self.assertIn("for(;;) {}", source[delete:])
        for forbidden in ("StartImage(", "ResetSystem(", "GuInstallDriverOption(",
                          "GuExecutePreparedSequence(", "DisconnectController(",
                          "GuBootStateWrite(", "GuBootStateRecord("):
            self.assertNotIn(forbidden, source)

    def test_actual_confirmation_requires_single_d_and_service_success(self):
        source = (SRC / "boot_state_maintenance.c").read_text(encoding="utf-8")
        confirm = source[source.index("static EFI_STATUS Confirm("):
                         source.index("EFI_STATUS EFIAPI UefiMain(")]
        mock = r"""
#include <stddef.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
typedef int EFI_STATUS;
typedef size_t UINTN;
typedef wchar_t CHAR16;
typedef void *EFI_EVENT;
typedef struct { unsigned ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct Input Input;
struct Input {
  EFI_STATUS (*Reset)(Input *,int);
  EFI_STATUS (*ReadKeyStroke)(Input *,EFI_INPUT_KEY *);
  EFI_EVENT WaitForKey;
};
typedef struct { EFI_STATUS (*WaitForEvent)(UINTN,EFI_EVENT *,UINTN *); } Services;
typedef struct { Input *ConIn; Services *BootServices; } EFI_SYSTEM_TABLE;
#define FALSE 0
#define EFI_NOT_READY 3
#define SCAN_ESC 23
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 1
#define EFI_ABORTED 2
static int scenario,pos,reads;
static const CHAR16 *keys;
static EFI_STATUS Reset(Input *p,int flag) {
  (void)p;(void)flag;pos=reads=0;return scenario==1?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Say(EFI_SYSTEM_TABLE *t,const char *fmt,...) {
  (void)t;(void)fmt;return scenario==2?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Wait(UINTN count,EFI_EVENT *events,UINTN *index) {
  if(count!=1||!events||!*events)return EFI_DEVICE_ERROR;
  *index=scenario==4?1:0;
  return scenario==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Read(Input *p,EFI_INPUT_KEY *key) {
  (void)p;reads++;
  if(scenario==7&&reads==1)return EFI_NOT_READY;
  if(scenario==5||!keys[pos])return EFI_DEVICE_ERROR;
  key->ScanCode=scenario==6?1:0;
  if(keys[pos]==27){key->ScanCode=SCAN_ESC;key->UnicodeChar=0;pos++;}
  else key->UnicodeChar=keys[pos++];
  return EFI_SUCCESS;
}
""" + confirm + r"""
int main(void) {
  Input input={Reset,Read,&input};
  Services services={Wait};
  EFI_SYSTEM_TABLE table={&input,&services};
  keys=L"D";
  for(scenario=0;scenario<=7;scenario++) {
    EFI_STATUS status=Confirm(&table);
    if((status==EFI_SUCCESS)!=(scenario==0||scenario==7))return 1;
    if((scenario==0||scenario==7)&&pos!=1)return 2;
  }
  scenario=0;
  keys=L"d";if(Confirm(&table)!=EFI_SUCCESS||pos!=1)return 3;
  keys=L"r";if(Confirm(&table)!=EFI_ABORTED||pos!=1)return 4;
  keys=L"R";if(Confirm(&table)!=EFI_ABORTED||pos!=1)return 5;
  keys=L"REARM\r";if(Confirm(&table)!=EFI_ABORTED||pos!=1)return 6;
  keys=L"\r";if(Confirm(&table)!=EFI_ABORTED||pos!=1)return 7;
  keys=L"\x1b";if(Confirm(&table)!=EFI_ABORTED||pos!=1)return 8;
  keys=L"Xd";if(Confirm(&table)!=EFI_ABORTED||pos!=1)return 9;
  keys=L"";if(Confirm(&table)==EFI_SUCCESS)return 10;
  keys=L"d";scenario=7;if(Confirm(&table)!=EFI_SUCCESS||pos!=1||reads!=2)return 11;
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            code = Path(directory) / "confirm.c"
            code.write_text(mock, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                cc = shutil.which(compiler)
                if not cc:
                    self.skipTest("Host compilers are required")
                binary = Path(directory) / compiler
                built = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                        "-pedantic", str(code), "-o", str(binary)],
                                       capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                run = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
