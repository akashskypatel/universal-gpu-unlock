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
