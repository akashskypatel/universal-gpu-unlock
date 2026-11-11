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
