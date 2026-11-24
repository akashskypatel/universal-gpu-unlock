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
        self.assertIn("boot_state.Status != EFI_SUCCESS", DXE)

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
        self.assertIn("boot_state.Status!=EFI_SUCCESS", APP)

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
            self.assertLess(text.index("GuBootStateRead"), text.index("GuDiscoverSingleGpu"), name)

    def test_record_failures_block_active_execution_or_handover(self):
        for name, text in (("app.c", APP), ("dxe_main.c", DXE)):
            intent = text.index("s=GuBootStateRecord(")
            sequence = text.index("s=GuExecutePreparedSequence(", intent)
            outcome = text.index("EFI_STATUS recorded=GuBootStateRecord(", sequence)
            self.assertIn("if(s!=EFI_SUCCESS||!GuBootLogOpen())", text[intent:sequence], name)
            self.assertIn("if(s==EFI_SUCCESS&&recorded!=EFI_SUCCESS)s=recorded;", text[outcome:], name)
            close = text.index("s=GuBootClose()", sequence)
            self.assertLess(close, outcome, name)
            cancel = text.index("EFI_STATUS cancelled=GuBootWatchdogCancel()", sequence)
            self.assertLess(cancel, close, name)
            self.assertNotIn("(VOID)GuBootStateRecord", text, name)
            self.assertIn("boot_state_status!=EFI_NOT_FOUND", text[:intent], name)
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
        self.assertIn("boot_state_status!=EFI_NOT_FOUND", APP)
        self.assertIn("boot_state_status!=EFI_NOT_FOUND", DXE)


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


class BuildWiring(unittest.TestCase):
    def test_both_builders_and_the_simulator_compile_the_module(self):
        for name in ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1"):
            text = (SRC / name).read_text(encoding="utf-8")
            self.assertIn("'boot_state.c'", text, name)
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

    def test_actual_confirmation_requires_exact_input_and_service_success(self):
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
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 1
#define EFI_ABORTED 2
static int scenario,pos;
static const CHAR16 *keys;
static EFI_STATUS Reset(Input *p,int flag) {
  (void)p;(void)flag;pos=0;return scenario==1?EFI_DEVICE_ERROR:EFI_SUCCESS;
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
  (void)p;if(scenario==5||!keys[pos])return EFI_DEVICE_ERROR;
  key->ScanCode=scenario==6?1:0;key->UnicodeChar=keys[pos++];return EFI_SUCCESS;
}
""" + confirm + r"""
int main(void) {
  Input input={Reset,Read,&input};
  Services services={Wait};
  EFI_SYSTEM_TABLE table={&input,&services};
  keys=L"REARM\r";
  for(scenario=0;scenario<=6;scenario++) {
    EFI_STATUS status=Confirm(&table);
    if((status==EFI_SUCCESS)!=(scenario==0))return 1;
  }
  scenario=0;
  keys=L"REARX\r";if(Confirm(&table)==EFI_SUCCESS)return 2;
  keys=L"REARM";if(Confirm(&table)==EFI_SUCCESS)return 3;
  keys=L"rearm\r";if(Confirm(&table)==EFI_SUCCESS)return 4;
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
