"""Compile and execute the original production GuInit body in a host harness.

Tests the real one-shot binding code, not a copied implementation.
No EFI services, GPU register access, or firmware mutation occurs.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/port.c").read_text(encoding="utf-8")
MSI_SOURCE = (ROOT / "src/msi_release.c").read_text(encoding="utf-8")


def function(name):
    import re
    found = re.search(r"\bEFI_STATUS\s+" + name + r"\s*\([^;]*?\)\s*\{", SOURCE)
    if not found:
        raise AssertionError(f"missing {name}")
    start = found.start()
    cursor, depth = found.end(), 1
    while cursor < len(SOURCE) and depth:
        depth += (SOURCE[cursor] == "{") - (SOURCE[cursor] == "}")
        cursor += 1
    if depth:
        raise AssertionError("unclosed function")
    return SOURCE[start:cursor]


class TargetBindTest(unittest.TestCase):
    def test_selected_card_owns_the_one_use_selector_write(self):
        first = SOURCE.index("VOID GuWrite32(")
        last = SOURCE.index("\nstatic EFI_STATUS RawRead(", first)
        actual = SOURCE[first:last]
        host = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef void VOID;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t EFI_STATUS;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define GU_PREFLIGHT_ONLY 0
#define GU_ENTRY_LATCHED_SS1_FULL 1
#define GU_SELECTOR_PHASE_SS1_WRITE 2U
#define EfiPciIoWidthUint32 2
#define EFI_SUCCESS 0
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
typedef struct MOCK_PCI EFI_PCI_IO_PROTOCOL;
typedef struct {
  EFI_STATUS (*Write)(EFI_PCI_IO_PROTOCOL *,int,int,UINT32,int,const VOID *);
} MEMORY;
struct MOCK_PCI { MEMORY Mem; int writes,fail; };
typedef struct {
  EFI_PCI_IO_PROTOCOL *pci;
  UINT64 bar_size;
  BOOLEAN profile_verified,armed,owned,display_ok,fault,ss1_write_consumed;
  UINT32 selector_phase,mmio_writes;
} GU_PLATFORM;
static GU_PLATFORM a,b;
static GU_PLATFORM *GuPlatformActive=&a;
#define Gu (*GuPlatformActive)
static EFI_STATUS Write(EFI_PCI_IO_PROTOCOL *pci,int width,int bar,
                        UINT32 offset,int count,const VOID *value) {
  if(width!=EfiPciIoWidthUint32 || bar!=0 || count!=1 ||
     offset!=0x823820U || *(const UINT32 *)value!=8U)return 1;
  pci->writes++;
  return pci->fail?1:EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
  EFI_PCI_IO_PROTOCOL io0={{Write},0,0},io1={{Write},0,0};
  a.pci=&io0;b.pci=&io1;
  a.bar_size=b.bar_size=0x1000000U;
  a.profile_verified=b.profile_verified=TRUE;
  a.armed=b.armed=a.owned=b.owned=TRUE;
  a.display_ok=b.display_ok=TRUE;
  a.selector_phase=GU_SELECTOR_PHASE_SS1_WRITE;
  GuWrite32(0x823820U,8U);
  CHECK(!a.fault && a.ss1_write_consumed && io0.writes==1);
  GuWrite32(0x823820U,8U);
  CHECK(a.fault && io0.writes==1);
  GuPlatformActive=&b;
  GuWrite32(0x823820U,8U);
  CHECK(b.fault && !b.ss1_write_consumed && io1.writes==0);
  b.fault=FALSE;b.selector_phase=GU_SELECTOR_PHASE_SS1_WRITE;
  io1.fail=1;
  GuWrite32(0x823820U,8U);
  CHECK(b.fault && b.ss1_write_consumed && io1.writes==1);
  GuWrite32(0x823820U,8U);
  CHECK(io1.writes==1);
  GuPlatformActive=&a;
  CHECK(a.fault && a.ss1_write_consumed && io0.writes==1);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "selector_write_isolation.c"
            binary = Path(directory) / "selector_write_isolation"
            source.write_text(host, encoding="utf-8")
            compiled = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0,
                             executed.stdout + executed.stderr)

    def test_selector_latches_and_write_phase_are_card_private(self):
        first = SOURCE.index("BOOLEAN GuGetEntrySelectors(")
        middle = SOURCE.index("#endif\n#ifdef GU_ENTRY_LATCHED_SS1_FULL", first)
        last = SOURCE.index("#endif\nEFI_STATUS GuInit(", middle)
        actual_latch = SOURCE[first:middle]
        actual_phase = SOURCE[SOURCE.index("BOOLEAN GuSelectorPhase(", middle):last]
        host = r"""
#include <stdint.h>
#include <stdio.h>
typedef uint32_t UINT32;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define GU_SELECTOR_PHASE_ENTRY 0U
#define GU_SELECTOR_PHASE_SS0_WRITTEN 1U
#define GU_SELECTOR_PHASE_SS1_WRITE 2U
#define GU_SELECTOR_PHASE_TARGET 3U
typedef struct {
  BOOLEAN entry_latched,ss1_write_consumed;
  BOOLEAN prom_verified,profile_verified,started,fault,armed,owned;
  UINT32 entry_ss0,entry_ss1,selector_phase,mmio_ss0,mmio_ss1;
} GU_PLATFORM;
static GU_PLATFORM a,b;
static GU_PLATFORM *GuPlatformActive=&a;
#define Gu (*GuPlatformActive)
static UINT32 GuRead32(UINT32 offset) {
  return offset==0x82381cU?Gu.mmio_ss0:Gu.mmio_ss1;
}
""" + actual_latch + actual_phase + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
  UINT32 ss0=0,ss1=0;
  a.prom_verified=b.prom_verified=1;
  CHECK(GuLatchEntrySelectors(0x03100062U,2));
  CHECK(GuGetEntrySelectors(&ss0,&ss1) && ss0==0x03100062U && ss1==2);
  GuPlatformActive=&b;
  CHECK(!GuGetEntrySelectors(&ss0,&ss1));
  CHECK(GuLatchEntrySelectors(0x03000062U,4));
  CHECK(GuGetEntrySelectors(&ss0,&ss1) && ss0==0x03000062U && ss1==4);
  GuPlatformActive=&a;
  CHECK(GuGetEntrySelectors(&ss0,&ss1) && ss0==0x03100062U && ss1==2);
  CHECK(GuLatchEntrySelectors(0x03100062U,2));
  CHECK(!GuLatchEntrySelectors(0x03000062U,2));
  a.profile_verified=a.armed=a.owned=1;
  a.mmio_ss0=0x88888888U;a.mmio_ss1=2U;
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_SS0_WRITTEN));
  CHECK(a.selector_phase==GU_SELECTOR_PHASE_SS0_WRITTEN);
  GuPlatformActive=&b;
  CHECK(b.selector_phase==GU_SELECTOR_PHASE_ENTRY);
  CHECK(!GuSelectorPhase(GU_SELECTOR_PHASE_TARGET) && b.fault);
  GuPlatformActive=&a;
  CHECK(!a.fault && a.selector_phase==GU_SELECTOR_PHASE_SS0_WRITTEN);
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_SS1_WRITE));
  a.ss1_write_consumed=TRUE;a.mmio_ss1=8U;
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_TARGET));
  CHECK(a.selector_phase==GU_SELECTOR_PHASE_TARGET);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "selector_isolation.c"
            binary = Path(directory) / "selector_isolation"
            source.write_text(host, encoding="utf-8")
            compiled = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run([str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(executed.returncode, 0,
                             executed.stdout + executed.stderr)

    def test_selected_platform_uses_new_backing_and_retains_one_shot(self):
        original = function("GuInit")
        host = r"""
#define GU_ACTIVE_SINGLE_TARGET 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef int BOOLEAN;
typedef uintptr_t EFI_STATUS;
typedef struct {int a;} EFI_PCI_IO_PROTOCOL;
typedef struct EFI_BOOT_SERVICES {
  EFI_STATUS (*AllocatePool)(int, size_t, VOID **);
} EFI_BOOT_SERVICES;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  int fault,started,retained,owned;
  VOID *allocations;
} GU_PLATFORM;
#define TRUE 1
#define FALSE 0
#define EfiLoaderData 2
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_ACCESS_DENIED 3
#define EFI_ALREADY_STARTED 4
#define EFI_UNSUPPORTED 5
#define EFI_OUT_OF_RESOURCES 6
static GU_PLATFORM LegacyPlatform;
GU_PLATFORM *GuPlatformActive=&LegacyPlatform;
#define Gu (*GuPlatformActive)
static BOOLEAN TargetBound;
static const GU_PLATFORM EmptyPlatform;
static unsigned allocations,fail_alloc,empty_alloc;
static EFI_STATUS Allocate(int kind,size_t bytes,VOID **out) {
  if(kind!=EfiLoaderData || !out)return EFI_INVALID_PARAMETER;
  allocations++;
  if(fail_alloc)return EFI_OUT_OF_RESOURCES;
  *out=empty_alloc?NULL:calloc(1,bytes);
  return EFI_SUCCESS;
}
""" + original + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  int mode=argc>1?argv[1][0]-'0':-1;
  EFI_BOOT_SERVICES bs={Allocate};
  EFI_PCI_IO_PROTOCOL first={1},second={2};
  if(mode==0) {
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    CHECK(GuPlatformActive!=&LegacyPlatform && allocations==1);
    CHECK(Gu.bs==&bs && Gu.pci==&first);
    Gu.retained=1;Gu.allocations=(VOID *)(uintptr_t)0x1234U;
    GU_PLATFORM *saved=GuPlatformActive;
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
    CHECK(GuPlatformActive==saved && allocations==1 && Gu.retained);
  } else if(mode==1) {
    fail_alloc=1;
    CHECK(GuInit(&bs,&first)==EFI_OUT_OF_RESOURCES);
    CHECK(GuPlatformActive==&LegacyPlatform && !TargetBound);
    fail_alloc=0;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    CHECK(GuPlatformActive!=&LegacyPlatform && allocations==2);
  } else if(mode==2) {
    Gu.fault=1;
    CHECK(GuInit(&bs,&first)==EFI_ACCESS_DENIED);
    CHECK(allocations==0 && GuPlatformActive==&LegacyPlatform);
    Gu.fault=0;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
  } else if(mode==3) {
    empty_alloc=1;
    CHECK(GuInit(&bs,&first)==EFI_OUT_OF_RESOURCES);
    CHECK(!TargetBound && GuPlatformActive==&LegacyPlatform);
    empty_alloc=0;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
  } else if(mode==4) {
    bs.AllocatePool=NULL;
    CHECK(GuInit(&bs,&first)==EFI_UNSUPPORTED);
    CHECK(!TargetBound && allocations==0);
    bs.AllocatePool=Allocate;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    memset(&Gu,0,sizeof(Gu));
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
  } else return 3;
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "selected_platform.c"
            executable = Path(directory) / "selected_platform"
            source.write_text(host, encoding="utf-8")
            result = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in range(5):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), str(mode)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

    def test_original_platform_rebind_guard(self):
        body = function("GuInit")
        declaration = "static BOOLEAN TargetBound;"
        self.assertIn(declaration, SOURCE)
        self.assertIn("static const GU_PLATFORM EmptyPlatform;", SOURCE)
        self.assertIn("TargetBound=TRUE;", body)
        self.assertLess(body.index("TargetBound=TRUE;"), body.index("memset(&Gu"))
        host = r"""
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef void VOID;
typedef int BOOLEAN;
typedef uintptr_t EFI_STATUS;
typedef struct { int a; } EFI_BOOT_SERVICES;
typedef struct { int a; } EFI_PCI_IO_PROTOCOL;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  int owned, started, retained, fault, saved;
  void *allocations;
  uint32_t flrs, dma_count;
} GU_PLATFORM;
#define TRUE 1
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_ACCESS_DENIED 3
#define EFI_ALREADY_STARTED 4
GU_PLATFORM Gu;
static BOOLEAN TargetBound;
static const GU_PLATFORM EmptyPlatform;
""" + body + r"""
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s\\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES boot={1};
  EFI_PCI_IO_PROTOCOL first={1}, second={2};
  int scenario=argc>1?argv[1][0]-'0':-1;
  if(scenario==0) {
    CHECK(GuInit(NULL,&first)==EFI_INVALID_PARAMETER);
    CHECK(GuInit(&boot,NULL)==EFI_INVALID_PARAMETER);
    CHECK(GuInit(&boot,&first)==EFI_SUCCESS);
    CHECK(Gu.pci==&first && Gu.bs==&boot);
    Gu.started=1;Gu.retained=1;Gu.flrs=1;
    GU_PLATFORM old=Gu;
    CHECK(GuInit(&boot,&second)==EFI_ALREADY_STARTED);
    CHECK(memcmp(&Gu,&old,sizeof(Gu))==0);
  } else if(scenario==1) {
    Gu.allocations=(void *)(uintptr_t)0x1234;
    GU_PLATFORM old=Gu;
    CHECK(GuInit(&boot,&first)==EFI_ACCESS_DENIED);
    CHECK(memcmp(&Gu,&old,sizeof(Gu))==0);
    Gu.allocations=NULL;
    CHECK(GuInit(&boot,&first)==EFI_SUCCESS);
    CHECK(Gu.pci==&first);
  } else if(scenario==2) {
    CHECK(GuInit(&boot,&first)==EFI_SUCCESS);
    memset(&Gu,0,sizeof(Gu));
    CHECK(GuInit(&boot,&second)==EFI_ALREADY_STARTED);
    CHECK(Gu.pci==NULL);
  } else return 3;
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "lifetime.c"
            executable = Path(directory) / "lifetime"
            source.write_text(host, encoding="utf-8")
            compile_result = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            for scenario in ("0", "1", "2"):
                with self.subTest(scenario=scenario):
                    result = subprocess.run(
                        [str(executable), scenario], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_generic_pci_function_location(self):
        """Compile the actual production predicate with nonzero BDF slots."""
        left = SOURCE.index("BOOLEAN GuTargetConfig(")
        right = SOURCE.index("BOOLEAN GuSafeToReturn(", left)
        pred = SOURCE[left:right]
        self.assertNotIn("&& !D && !F", pred)
        header = r"""
#include <stdio.h>
#include <stdint.h>
typedef unsigned long UINTN;
typedef uint32_t UINT32;
typedef int BOOLEAN;
static int GuIsSupportedPciId(UINT32 id) {
    return id==0x268410deU || id==0x1e0710deU;
}
"""
        check = r"""
#define CHECK(x) do {if(!(x))return __LINE__;}while(0)
int main(void) {
  UINT32 c[64]={0};
  c[0]=0x268410deU;c[2]=0x03000000U;
  CHECK(GuTargetConfig(c,0,0,0,0));
  CHECK(GuTargetConfig(c,1,7,4,1));   /* An actual nonzero PCI segment/slot. */
  CHECK(GuTargetConfig(c,65535,255,31,7));
  CHECK(!GuTargetConfig(c,65536,7,4,1));
  CHECK(!GuTargetConfig(c,1,256,4,1));
  CHECK(!GuTargetConfig(c,1,7,32,1));
  CHECK(!GuTargetConfig(c,1,7,4,8));
  CHECK(!GuTargetConfig(NULL,1,7,4,1));
  c[0]=0x999910deU;CHECK(!GuTargetConfig(c,1,7,4,1));
  c[0]=0x1e0710deU;CHECK(GuTargetConfig(c,1,7,4,1));
  c[2]=0x02000000U;CHECK(!GuTargetConfig(c,1,7,4,1));
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "location.c"
            binary = Path(td) / "location"
            source.write_text(header + pred + check, encoding="utf-8")
            compiled = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2", "-Wall",
                 "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_config_validator_does_not_assume_segment_zero(self):
        check = (ROOT / "src/checks.c").read_text(encoding="utf-8")
        self.assertIn("Segment > 0xffffU", check)
        self.assertNotIn("Segment != 0", check)

    def test_selected_release_plan_owns_independent_graphs(self):
        start = MSI_SOURCE.index("EFI_STATUS GuMsiReserveSelectedState(")
        end = MSI_SOURCE.index("\nstatic EFI_GUID PciGuid=", start)
        production = MSI_SOURCE[start:end]
        host = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef int BOOLEAN;
typedef uintptr_t EFI_STATUS;
typedef struct EFI_BOOT_SERVICES {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *bs;VOID *handle;
  int profile_verified,prom_verified,started,owned,armed,fault,retained;
} GU_PLATFORM;
static GU_PLATFORM Gu;
typedef struct {VOID *table;int mutated,claimed,ready;} PLAN;
typedef struct {int edges[4];} GRAPH;
#define MAX_EDGES 4
#define EfiLoaderData 1
#define EFI_SUCCESS 0
#define EFI_NOT_READY 2
#define EFI_ALREADY_STARTED 3
#define EFI_OUT_OF_RESOURCES 4
static PLAN InitialPlan;
static GRAPH InitialOriginal,InitialCurrent;
static BOOLEAN InitialUsb[MAX_EDGES];
typedef struct {PLAN plan;GRAPH original,current;BOOLEAN original_usb[MAX_EDGES];} GU_MSI_OWNED_STATE;
static GU_MSI_OWNED_STATE *SelectedMsi;
static PLAN *CurrentPlan=&InitialPlan;
static GRAPH *CurrentOriginal=&InitialOriginal,*CurrentGraph=&InitialCurrent;
static BOOLEAN (*CurrentOriginalUsb)[MAX_EDGES]=&InitialUsb;
#define Plan (*CurrentPlan)
#define Original (*CurrentOriginal)
#define Current (*CurrentGraph)
#define OriginalUsb (*CurrentOriginalUsb)
static int fail_pool,allocations;
static EFI_STATUS Allocate(int type,size_t n,VOID **out) {
  if(type!=EfiLoaderData||!out)return EFI_NOT_READY;
  allocations++;
  if(fail_pool)return EFI_OUT_OF_RESOURCES;
  *out=calloc(1,n);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
""" + production + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES bs={Allocate};
  Gu.bs=&bs;Gu.handle=(VOID *)(uintptr_t)1;
  Gu.profile_verified=Gu.prom_verified=1;
  int mode=argc>1?argv[1][0]-'0':-1;
  if(mode==0) {
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS);
    CHECK(SelectedMsi && CurrentPlan!=&InitialPlan);
    CHECK(CurrentOriginal!=&InitialOriginal && CurrentGraph!=&InitialCurrent);
    CHECK(CurrentOriginalUsb!=&InitialUsb);
    CHECK(CurrentPlan==&SelectedMsi->plan && CurrentOriginal==&SelectedMsi->original);
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_ALREADY_STARTED && allocations==1);
  } else if(mode==1) {
    Gu.profile_verified=0;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
    CHECK(!SelectedMsi && allocations==0 && CurrentPlan==&InitialPlan);
  } else if(mode==2) {
    fail_pool=1;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_OUT_OF_RESOURCES);
    CHECK(!SelectedMsi && CurrentPlan==&InitialPlan);
    fail_pool=0;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS && allocations==2);
  } else if(mode==3) {
    Plan.table=(VOID *)(uintptr_t)1;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_ALREADY_STARTED);
    CHECK(!SelectedMsi && allocations==0);
  } else if(mode==4) {
    bs.AllocatePool=NULL;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
    CHECK(!SelectedMsi);
  } else if(mode==5) {
    Gu.retained=1;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY && allocations==0);
  } else return 3;
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "msi_plan_lifetime.c"
            binary = Path(directory) / "msi_plan_lifetime"
            source.write_text(host, encoding="utf-8")
            compiled = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for mode in range(6):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), str(mode)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

    def test_dma_failure_retains_uncertain_firmware_outputs(self):
        """Exercise actual GuDmaAllocate against independent EFI error mocks."""
        actual = function("GuDmaAllocate")
        host = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef size_t UINTN;
typedef uint64_t UINT64;
typedef int EFI_STATUS;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define GU_PREFLIGHT_ONLY 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_UNSUPPORTED 4
#define EFI_DEVICE_ERROR 5
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define AllocateAnyPages 1
#define EfiBootServicesData 2
#define EfiPciIoOperationBusMasterCommonBuffer 3
typedef struct GU_DMA GU_DMA;
struct GU_DMA {
  GU_DMA *next;
  VOID *cpu, *mapping;
  UINT64 dma;
  UINTN bytes, pages;
  BOOLEAN mapped;
};
typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
struct EFI_PCI_IO_PROTOCOL {
  EFI_STATUS (*AllocateBuffer)(EFI_PCI_IO_PROTOCOL *,int,int,UINTN,VOID **,int);
  EFI_STATUS (*Map)(EFI_PCI_IO_PROTOCOL *,int,VOID *,UINTN *,UINT64 *,VOID **);
  EFI_STATUS (*Unmap)(EFI_PCI_IO_PROTOCOL *,VOID *);
  EFI_STATUS (*FreeBuffer)(EFI_PCI_IO_PROTOCOL *,UINTN,VOID *);
};
static struct {
  EFI_PCI_IO_PROTOCOL *pci;
  GU_DMA *allocations;
  int profile_verified,armed,display_ok,fault,retained,owned;
  unsigned dma_count;
} Gu;
static unsigned char gpu_buffer[8192];
static int mapping_token,mode;
static unsigned pool_frees,buffer_frees,unmaps;
static VOID *GuAlloc(size_t size) {return calloc(1,size);}
static VOID GuFree(VOID *ptr) {
  if(!Gu.retained) {pool_frees++;free(ptr);}
}
static VOID GuLog(const char *fmt,...) {(void)fmt;}
static EFI_STATUS FakeAllocate(EFI_PCI_IO_PROTOCOL *io,int pages,int type,
                               UINTN count,VOID **out,int attrs) {
  (void)io;(void)pages;(void)type;(void)attrs;
  if(count!=1)return EFI_INVALID_PARAMETER;
  if(mode==0) return EFI_DEVICE_ERROR;
  if(mode==1) {*out=gpu_buffer;return EFI_DEVICE_ERROR;}
  if(mode==2) return EFI_SUCCESS;
  *out=gpu_buffer;return EFI_SUCCESS;
}
static EFI_STATUS FakeMap(EFI_PCI_IO_PROTOCOL *io,int operation,VOID *cpu,
                          UINTN *length,UINT64 *dma,VOID **mapping) {
  (void)io;
  if(operation!=EfiPciIoOperationBusMasterCommonBuffer ||
     cpu!=gpu_buffer || *length!=4096)return EFI_INVALID_PARAMETER;
  if(mode==3)return EFI_DEVICE_ERROR;
  if(mode==4) {*mapping=&mapping_token;return EFI_DEVICE_ERROR;}
  if(mode==5) {*dma=0x1000;return EFI_DEVICE_ERROR;}
  if(mode==6) {*length-=256;return EFI_DEVICE_ERROR;}
  *mapping=&mapping_token;
  *dma=mode==10?0x1000ULL:0x100000000ULL;
  return EFI_SUCCESS;
}
static EFI_STATUS FakeUnmap(EFI_PCI_IO_PROTOCOL *io,VOID *mapping) {
  (void)io;
  unmaps++;
  if(mapping!=&mapping_token || mode==8)return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static EFI_STATUS FakeFreeBuffer(EFI_PCI_IO_PROTOCOL *io,UINTN pages,VOID *cpu) {
  (void)io;
  buffer_frees++;
  if(pages!=1 || cpu!=gpu_buffer || mode==9)return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d mode %d: %s\n",__LINE__,mode,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  static EFI_PCI_IO_PROTOCOL io={FakeAllocate,FakeMap,FakeUnmap,FakeFreeBuffer};
  mode=argc>1?atoi(argv[1]):-1;
  if(mode<0||mode>10)return 3;
  Gu.pci=&io;
  Gu.profile_verified=Gu.armed=Gu.display_ok=Gu.owned=TRUE;
  VOID *cpu=(VOID *)(uintptr_t)1;
  UINT64 dma=1;
  EFI_STATUS result=GuDmaAllocate(4096,&cpu,&dma);
  EFI_STATUS expected=mode==2?EFI_OUT_OF_RESOURCES:
                      mode==7||mode==8||mode==9?EFI_UNSUPPORTED:
                      mode==10?EFI_SUCCESS:EFI_DEVICE_ERROR;
  CHECK(result==expected);
  if(mode==10) {
    CHECK(cpu==gpu_buffer && dma==0x1000 && Gu.dma_count==1);
    CHECK(Gu.allocations!=NULL && !Gu.retained);
    CHECK(!unmaps && !buffer_frees);
  } else {
    CHECK(cpu==NULL && dma==0 && Gu.dma_count==0);
    int retain=(mode==1||mode==2||mode==4||mode==5||
                mode==6||mode==8||mode==9);
    CHECK((Gu.retained!=0)==retain);
    CHECK((Gu.allocations!=NULL)==(retain && mode!=2));
    CHECK(unmaps==(unsigned)(mode>=7&&mode<=9));
    CHECK(buffer_frees==(unsigned)(mode==3||mode==7||mode==9));
    CHECK(pool_frees==(unsigned)(mode==0||mode==2||mode==3||mode==7));
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "dma_failure.c"
            binary = Path(directory) / "dma_failure"
            source.write_text(host, encoding="utf-8")
            compiled = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for mode in range(11):
                with self.subTest(mode=mode):
                    executed = subprocess.run([str(binary), str(mode)],
                                              capture_output=True, text=True)
                    self.assertEqual(executed.returncode, 0,
                                     executed.stdout + executed.stderr)

    def test_release_plan_is_not_reset_after_attempt(self):
        start = MSI_SOURCE.index("EFI_STATUS GuMsiPreflight(")
        stop = MSI_SOURCE.index("static BOOLEAN DecodeMask(", start)
        body = MSI_SOURCE[start:stop]
        self.assertIn("Plan.table", body)
        guard = "if(Plan.table||Plan.mutated||Plan.ready||Plan.claimed)return EFI_ALREADY_STARTED;"
        self.assertIn(guard, body)
        self.assertLess(body.index(guard), body.index("memset(&Plan"))


if __name__ == "__main__":
    unittest.main()
