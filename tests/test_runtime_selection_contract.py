"""Contract tests for runtime target selection (no compiled target list).

The build used to bake the operator's PCI addresses into the image, which meant
a moved card, a second slot, or a different machine required a rebuild. These
tests pin the replacement decision:

  1. the selection module parses and round-trips the operator text, and refuses
     every malformed form without leaving a partial list behind;
  2. the profile gates stay intact: runtime selection needs a real ordered
     execution profile, and is rejected next to preparation-only and synthetic
     builds;
  3. a runtime-selection image never reads a generated ``gpu-selection.h``, so
     the target count is no longer a build-time constant;
  4. the builder and the simulator both compile the runtime path, and the
     simulator's image carries no compiled list at all.

Nothing here observes a physical GPU: these are source-shape and host-logic
checks, not hardware evidence.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS = ROOT / "tests"
PORT = (SRC / "port.h").read_text(encoding="utf-8")
DISCOVERY = (SRC / "gpu_discovery.c").read_text(encoding="utf-8")
SELECTION = (SRC / "gpu_selection.c").read_text(encoding="utf-8")
MSVC = (SRC / "Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
BUILD_OPTIONS = (SRC / "Gpu-Execution-Sources.ps1").read_text(encoding="utf-8")
SIMULATOR = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")

# A runtime-selection image is still a real ordered execution build: the
# transaction profile plus one explicit stock profile stay mandatory.
BASE = (
    "-DGU_ACTIVE_SINGLE_TARGET",
    "-DGU_AD104_TRANSACTION",
    "-DGU_ENTRY_LATCHED_SS1_FULL",
)
PROBE = '#include "port.h"\nint gu_runtime_selection_probe;\n'


def compilers():
    found = [shutil.which("gcc"), shutil.which("clang")]
    return [path for path in found if path]


def run_cc(compiler, flags, source=PROBE, linked=False, outputs=()):
    """Compile a freestanding firmware probe (or a host unit) and report."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "probe.c"
        path.write_text(source, encoding="utf-8")
        command = [compiler, "-std=c11", "-fshort-wchar", "-ffreestanding",
                   "-DGU_UEFI_PORT", "-Wall", "-Wextra", "-Werror", *flags,
                   "-I", str(SRC),
                   "-I", str(SRC / "vendor" / "edk2" / "Include"),
                   "-I", str(SRC / "vendor" / "edk2" / "Include" / "X64")]
        if not linked:
            command.append("-fsyntax-only")
        command.append(str(path))
        done = subprocess.run(command, capture_output=True, text=True)
        return done.returncode, done.stdout + done.stderr


class SelectionUnit(unittest.TestCase):
    """The parser and the variable round-trip, executed on the host."""

    def test_host_unit_passes_under_every_compiler(self):
        available = compilers()
        if not available:
            self.skipTest("A host C compiler is required")
        for compiler in available:
            with tempfile.TemporaryDirectory() as tmp:
                binary = Path(tmp) / "test_gpu_selection"
                command = [
                    compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-pedantic", "-DGU_SELECTION_HOST_TEST",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I", str(SRC), "-I", str(TESTS),
                    str(TESTS / "test_gpu_selection.c"),
                    str(SRC / "gpu_selection.c"), "-o", str(binary),
                ]
                build = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                done = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertIn("checks OK", done.stdout)



class LiveSelectionMenu(unittest.TestCase):
    def test_live_choice_display_uses_the_same_location_and_reports_output_failure(self):
        app = (SRC / "app.c").read_text(encoding="utf-8")
        start = app.index("static EFI_STATUS ShowTargetChoice(")
        actual = app[start:app.index("#endif", start)]
        menu = app.split("static EFI_STATUS ResolveTargetsOnConsole(", 1)[1]
        self.assertLess(menu.index("s=ShowTargetChoice(t,i+1U,record);"),
                        menu.index("ReadKeyStroke(t->ConIn,&key)"))
        self.assertIn("if(s!=EFI_SUCCESS||!GuBootLogOpen())", menu)
        mock = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t CHAR16;
typedef size_t UINTN;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
typedef struct { unsigned segment,bus,device,function; uint32_t pci_id; } GU_GPU_RECORD;
typedef struct OUT OUT;
struct OUT { EFI_STATUS (*OutputString)(OUT *,CHAR16 *); };
typedef struct { OUT *ConOut; } EFI_SYSTEM_TABLE;
static int calls,fail;
static char shown[128];
static EFI_STATUS Output(OUT *self,CHAR16 *row) {
  (void)self;calls++;
  size_t n=0;while(row[n]&&n<sizeof(shown)-1U){shown[n]=(char)row[n];n++;}
  shown[n]=0;return fail?7:EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
  OUT output={Output};EFI_SYSTEM_TABLE table={&output};
  GU_GPU_RECORD first={0,4,0,0,0x220a10de},second={0,1,0,0,0x268410de};
  GU_GPU_RECORD varied={0x1234,0xab,0x1c,7,0x56781234};
  CHECK(ShowTargetChoice(&table,1,&first)==EFI_SUCCESS);
  CHECK(!strcmp(shown,"1: 0000:04:00.0 PCI=10de:220a\r\n"));
  CHECK(ShowTargetChoice(&table,2,&second)==EFI_SUCCESS);
  CHECK(!strcmp(shown,"2: 0000:01:00.0 PCI=10de:2684\r\n"));
  CHECK(ShowTargetChoice(&table,9,&varied)==EFI_SUCCESS);
  CHECK(!strcmp(shown,"9: 1234:ab:1c.7 PCI=1234:5678\r\n"));
  CHECK(calls==3);
  CHECK(ShowTargetChoice(&table,0,&second)==EFI_INVALID_PARAMETER);
  CHECK(ShowTargetChoice(&table,10,&second)==EFI_INVALID_PARAMETER);
  CHECK(calls==3);
  fail=1;CHECK(ShowTargetChoice(&table,2,&second)==7&&calls==4);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "choice.c"
            src.write_text(mock, encoding="utf-8")
            for compiler in compilers():
                with self.subTest(compiler=compiler):
                    binary = Path(tmp) / "choice"
                    build = subprocess.run([compiler, "-std=c11", "-fshort-wchar",
                                            "-Wall", "-Wextra", "-Werror", "-pedantic",
                                            str(src), "-o", str(binary)],
                                           capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    run = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


    def test_combined_requires_a_fresh_choice_and_never_uses_saved_selection(self):
        app = (SRC / "app.c").read_text(encoding="utf-8")
        start = app.index("static EFI_STATUS ResolveTargetsOnConsole(")
        end = app.index("\n#endif\n\n/* The outcomes", start)
        menu = app[start:end]
        self.assertIn("if(s!=EFI_SUCCESS) {stage=\"selection\";GuTracePhase(stage);goto failed;}", app)
        self.assertIn("REASON=FRESH_OPERATOR_CHOICE_REQUIRED", DISCOVERY)
        # Compile the actual menu with combined mode enabled. No NVRAM
        # read/write functions exist in this harness; a stale-variable
        # fallback or persisted choice would fail compilation.
        mock = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef uint16_t CHAR16;
typedef uint16_t UINT16;
typedef uint8_t UINT8;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_NOT_FOUND 14
#define EFI_DEVICE_ERROR 7
#define TRUE 1
#define GU_FIRMWARE_REATTACH 1
typedef struct { unsigned segment,bus,device,function; } GU_GPU_LOCATION;
typedef struct { GU_GPU_LOCATION list[128];size_t count;int all; } GU_SELECTION;
typedef struct { unsigned segment,bus,device,function,pci_id; } GU_GPU_RECORD;
typedef struct { struct { size_t count; GU_GPU_RECORD nvidia[2]; } inventory; } GU_GPU_PREPARED_SEQUENCE;
typedef struct { CHAR16 UnicodeChar;unsigned ScanCode; } EFI_INPUT_KEY;
typedef struct IN IN;
struct IN { EFI_STATUS (*ReadKeyStroke)(IN *,EFI_INPUT_KEY *); };
typedef struct { void (*Stall)(unsigned); } BS;
typedef struct { BS *BootServices;IN *ConIn; } EFI_SYSTEM_TABLE;
static unsigned count,shown,chosen,all,calls;
static CHAR16 key;
static int display_error;
static void GuLog(const char *fmt,...){(void)fmt;}
static void GuBootSay(const CHAR16 *msg){(void)msg;}
static int GuBootLogOpen(void){return 1;}
static EFI_STATUS GuPrepareGpuSequence(EFI_SYSTEM_TABLE *t,void *r,size_t n,GU_GPU_PREPARED_SEQUENCE *p){
  (void)t;(void)r;(void)n;p->inventory.count=count;
  p->inventory.nvidia[0]=(GU_GPU_RECORD){0,1,0,0,0x268410de};
  p->inventory.nvidia[1]=(GU_GPU_RECORD){0,4,0,0,0x220a10de};return 0;
}
static EFI_STATUS ShowTargetChoice(EFI_SYSTEM_TABLE *t,size_t i,const GU_GPU_RECORD *r){
  (void)t;(void)i;(void)r;shown++;return display_error;
}
static EFI_STATUS GuRuntimeSelectionSelectAll(void){all++;return 0;}
static EFI_STATUS GuRuntimeSelectionChoose(const GU_GPU_LOCATION *p){chosen=p->bus;return 0;}
static EFI_STATUS Read(IN *in,EFI_INPUT_KEY *out){
  (void)in;calls++;if(!key)return EFI_NOT_FOUND;
  out->UnicodeChar=key;out->ScanCode=0;return 0;
}
static void Stall(unsigned n){(void)n;}
""" + menu + r"""
int main(void){
  BS bs={Stall};IN in={Read};EFI_SYSTEM_TABLE t={&bs,&in};
  count=2;key='A';assert(ResolveTargetsOnConsole(&t)==0&&all==1&&shown==2);
  key='2';assert(ResolveTargetsOnConsole(&t)==0&&chosen==4);
  key='S';assert(ResolveTargetsOnConsole(&t)==EFI_NOT_FOUND&&all==1&&chosen==4);
  key=0;assert(ResolveTargetsOnConsole(&t)==EFI_NOT_FOUND&&all==1&&chosen==4);
  count=1;key='S';assert(ResolveTargetsOnConsole(&t)==EFI_NOT_FOUND&&all==1&&chosen==4);
  key='1';assert(ResolveTargetsOnConsole(&t)==0&&chosen==1);
  display_error=7;key='A';assert(ResolveTargetsOnConsole(&t)==7&&all==1);
  return 0;
}
"""
        for compiler in compilers():
            with self.subTest(compiler=compiler):
                with tempfile.TemporaryDirectory() as tmp:
                    src = Path(tmp) / "menu.c"
                    binary = Path(tmp) / "menu"
                    src.write_text(mock, encoding="utf-8")
                    build = subprocess.run([compiler, "-std=c11", "-fshort-wchar",
                                            "-Wall", "-Wextra", "-Werror", "-pedantic",
                                            str(src), "-o", str(binary)],
                                           capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    run = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


class ProfileGates(unittest.TestCase):
    """``port.h`` keeps runtime selection inside a real execution profile."""

    def setUp(self):
        self.compiler = (compilers() or [None])[0]
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def assertRefused(self, flags, reason):
        status, output = run_cc(self.compiler, flags)
        self.assertNotEqual(status, 0, "combination was admitted: %s" % (flags,))
        self.assertIn(reason, output)

    def test_runtime_selection_needs_ordered_execution(self):
        self.assertRefused(
            ("-DGU_RUNTIME_SELECTION", "-DGU_AD104_TRANSACTION",
             "-DGU_ENTRY_LATCHED_SS1_FULL"),
            "Runtime target selection requires the ordered selected-target execution mode")

    def test_runtime_selection_rejects_preparation_and_synthetic_profiles(self):
        for extra, name in (("-DGU_SYNTHETIC_COMPILE_ONLY", "synthetic"),
                            ("-DGU_ACTIVE_SEQUENCE_PREPARE_ONLY", "preparation-only")):
            self.assertRefused(
                BASE + ("-DGU_RUNTIME_SELECTION", extra),
                "Runtime target selection requires a real execution profile")

    def test_runtime_selection_with_the_waiver_compiles(self):
        status, output = run_cc(
            self.compiler,
            BASE + ("-DGU_RUNTIME_SELECTION", "-DGU_ACTIVE_UNVERIFIED_ACTUATION"))
        self.assertEqual(status, 0, output)

    def test_gate_is_documented_before_it_is_enforced(self):
        gate = PORT.index("#if defined(GU_RUNTIME_SELECTION)")
        comment = PORT.index("GU_RUNTIME_SELECTION moves the target list out of the build")
        self.assertLess(comment, gate)
        self.assertIn("#error Runtime target selection requires a real execution profile",
                      PORT)


class NoCompiledTargetList(unittest.TestCase):
    """The runtime profile must not depend on the generated header."""

    def test_discovery_selects_the_runtime_branch_first(self):
        self.assertIn('#if defined(GU_RUNTIME_SELECTION)', DISCOVERY)
        runtime = DISCOVERY.index('#if defined(GU_RUNTIME_SELECTION)')
        generated = DISCOVERY.index('#include "gpu-selection.h"')
        self.assertLess(runtime, generated,
                        "the compiled list is still required before the runtime branch")
        # The static gates that pinned the count belong to the compiled branch.
        self.assertLess(DISCOVERY.index("#include \"gpu-selection.h\""),
                        DISCOVERY.index("#error One-target active execution requires exactly one PCI address"))

    def test_runtime_count_is_not_a_build_constant(self):
        self.assertNotIn("GU_GPU_SELECTION_COUNT", DISCOVERY.split(
            'const GU_GPU_LOCATION *requested=NULL;')[0].split(
            '#if defined(GU_RUNTIME_SELECTION)')[-1])

    def test_all_policy_selects_only_supported_cards(self):
        self.assertIn("GuIsSupportedPciId(prepared->inventory.nvidia[i].pci_id)",
                      DISCOVERY)
        self.assertIn('ACTIVE_PREPARE=POLICY_ALL', DISCOVERY)
        # The bounded compatibility API keeps the default single-card policy.
        self.assertIn("SelectPreparedRecords(&view,requested,requested_count,FALSE)",
                      DISCOVERY)

    def test_variable_is_fork_owned_and_read_only_until_written(self):
        header = (SRC / "gpu_selection.h").read_text(encoding="utf-8")
        self.assertIn('#define GU_SELECTION_VARIABLE_NAME L"GuTargets"', header)
        self.assertIn("0x5c1f2b40, 0x8e21, 0x4ef7", SELECTION)
        # Reading never falls back to another card: absence is EFI_NOT_FOUND.
        self.assertIn("return s == EFI_NOT_FOUND ? EFI_SUCCESS : s", SELECTION)


class RuntimeDiscoveryCompiles(unittest.TestCase):
    """The adapter compiles with no target list, whatever a stray header says."""

    def setUp(self):
        self.compiler = (compilers() or [None])[0]
        if not self.compiler:
            self.skipTest("A host C compiler is required")

    def compile_discovery(self, extra=(), include=()):
        command = [self.compiler, "-std=c11", "-fsyntax-only", "-Wall",
                   "-Wextra", "-Werror", "-pedantic",
                   "-DGU_ACTIVE_SINGLE_TARGET", "-DGU_RUNTIME_SELECTION",
                   "-DGU_SELECTION_HOST_TEST", *extra, "-I", str(SRC),
                   *[arg for path in include for arg in ("-I", str(path))],
                   str(TESTS / "test_gpu_discovery.c")]
        done = subprocess.run(command, capture_output=True, text=True)
        return done.returncode, done.stdout + done.stderr

    def test_discovery_compiles_without_a_generated_header(self):
        status, output = self.compile_discovery()
        self.assertEqual(status, 0, output)

    def test_a_stale_multi_target_header_is_ignored(self):
        # The old failure mode: a three-address header refused to compile into
        # a non-synthetic image. With runtime selection the header is not read
        # at all, so the same tree builds and the runtime admission decides.
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "gpu-selection.h").write_text(
                "#define GU_GPU_SELECTION_COUNT 3U\n"
                "#define GU_GPU_SELECTION_LOCATIONS { { 0U,7U,0U,0U }, "
                "{ 0U,9U,0U,0U }, { 0U,3U,0U,0U } }\n", encoding="utf-8")
            status, output = self.compile_discovery(include=(tmp,))
            self.assertEqual(status, 0, output)


class BuilderWiring(unittest.TestCase):
    def test_msvc_builder_emits_the_define_once(self):
        self.assertEqual(MSVC.count("/DGU_RUNTIME_SELECTION"), 1)
        self.assertIn("' /DGU_RUNTIME_SELECTION'", MSVC)

    def test_msvc_builder_refuses_the_synthetic_combination(self):
        self.assertIn("Assert-GuExecutionBuildOptions ", MSVC)
        self.assertIn(
            "-RuntimeSelection cannot be combined with -SyntheticCompileProfile",
            BUILD_OPTIONS)
        self.assertIn(
            "-RuntimeSelection replaces the compiled target list", BUILD_OPTIONS)

    def test_msvc_builder_links_the_selection_module(self):
        from test_build_source_lists import builder_sources
        self.assertIn("gpu_selection.c", builder_sources("Build-Experimental-MSVC.ps1"))
        self.assertIn("RUNTIME_SELECTION=$([int][bool]$RuntimeSelection)", MSVC)

    def test_simulator_builds_without_a_compiled_list(self):
        self.assertIn('"-DGU_RUNTIME_SELECTION"', SIMULATOR)
        self.assertIn('"gpu_selection.c"', SIMULATOR)
        self.assertNotIn("SELECTION = ", SIMULATOR)
        self.assertNotIn('write_text(SELECTION', SIMULATOR)
        self.assertIn("stale.unlink()", SIMULATOR)

    def test_simulator_models_the_operator_variable(self):
        world = (ROOT / "sim" / "sim_world.c").read_text(encoding="utf-8")
        self.assertIn("SimWorldSetTargets", world)
        self.assertIn("0x5c1f2b40, 0x8e21, 0x4ef7", world)
        self.assertIn('WideEquals(name, "GuTargets")', world)


if __name__ == "__main__":
    unittest.main()
