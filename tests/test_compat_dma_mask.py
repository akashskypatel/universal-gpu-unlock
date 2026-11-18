"""Compile the production DMA mask macro with real host C compilers.

A 64-bit shift must not run for a 64-bit DMA address mask.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HEADER = Path(__file__).resolve().parents[1] / "src/compat/compat.h"


class DmaMaskTests(unittest.TestCase):
    def test_production_macro_at_64_bit_boundary(self):
        source = HEADER.read_text(encoding="utf-8")
        matches = re.findall(r"^#define\s+DMA_BIT_MASK\(n\)\s+.*$", source, re.MULTILINE)
        self.assertEqual(len(matches), 1, "Expected one production DMA_BIT_MASK macro")
        program = r"""
#include <stdint.h>
""" + matches[0] + r"""
int main(void) {
    const unsigned widths[] = {0, 1, 16, 32, 63, 64};
    const uint64_t expected[] = {
        0ull, 1ull, 65535ull, 0xffffffffull,
        0x7fffffffffffffffull, 0xffffffffffffffffull
    };
    for (unsigned i = 0; i < sizeof(widths)/sizeof(widths[0]); ++i) {
        volatile unsigned n = widths[i];
        if ((uint64_t)DMA_BIT_MASK(n) != expected[i])
            return (int)i + 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "dma_mask.c"
            src.write_text(program, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                with self.subTest(compiler=compiler):
                    binary = Path(directory) / ("dma_mask_" + compiler)
                    built = subprocess.run(
                        [compiler, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                         "-pedantic", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                         str(src), "-o", str(binary)],
                        capture_output=True, text=True, check=False)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    checked = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


    def test_retained_device_identity_rejects_stale_context(self):
        """Run actual CPU identity helpers and selected wrappers without device I/O."""
        header=HEADER.read_text(encoding="utf-8")
        compat=(HEADER.parents[1]/"compat.c").read_text(encoding="utf-8")
        execution=(HEADER.parents[1]/"unlock_execution.c").read_text(encoding="utf-8")
        vendor=(HEADER.parents[1]/"vendor/unlock_610.c").read_text(encoding="utf-8")
        structs=header[header.index("struct list_head {"):header.index("/* CPU-only transaction identity.")]
        helpers=compat[compat.index("static BOOLEAN GuCompatIdentityValid("):compat.index("GU_ASSET *GuAssetEntries(")]
        scope=compat[compat.index("void *devres_open_group("):compat.index("void usleep_range(")]
        masks=compat[compat.index("int dma_set_mask("):compat.index("void dma_wmb(")]
        read=compat[compat.index("int pci_read_config_word("):compat.index("int pci_read_config_dword(")]
        refs=compat[compat.index("struct pci_dev *pci_dev_get("):]
        host=r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
typedef void VOID;
typedef uint16_t u16;
typedef uint64_t u64;
typedef uint32_t UINT32;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef void *EFI_HANDLE;
typedef struct {int id;} EFI_BOOT_SERVICES;
typedef struct {int id;} EFI_PCI_IO_PROTOCOL;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 1
#define EFI_NOT_READY 2
#define EFI_ALREADY_STARTED 3
#define EFI_OUT_OF_RESOURCES 4
#define EFI_ACCESS_DENIED 5
#define EFI_DEVICE_ERROR 6
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define PCI_D0 0
#define EINVAL 22
#define EIO 5
#define EfiPciIoWidthUint16 1
typedef struct {
  EFI_BOOT_SERVICES *bs;EFI_PCI_IO_PROTOCOL *pci;EFI_HANDLE handle;
  unsigned device_id,pm_cap;
  int fault,retained,compat_device_open;
  struct pci_dev *compat_device;
} GU_PLATFORM;
static GU_PLATFORM platforms[2],*active=&platforms[0];
#define Gu (*active)
static int allocs,reads,fail_alloc;
static void *GuAlloc(size_t n){allocs++;return fail_alloc?NULL:calloc(1,n);}
static EFI_STATUS GuConfigRead(int width,UINT32 offset,u16 *out){
  (void)width;(void)offset;reads++;*out=(u16)Gu.pci->id;return EFI_SUCCESS;
}
""" + structs + helpers + scope + masks + read + refs + r"""
#define CHECK(e) do {if(!(e)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);return 1;}}while(0)
int main(void){
  EFI_BOOT_SERVICES bs={1},foreign={2};
  EFI_PCI_IO_PROTOCOL pci[2]={{17},{23}};
  for(int i=0;i<2;i++){
    platforms[i].bs=&bs;platforms[i].pci=&pci[i];platforms[i].handle=&pci[i];
    platforms[i].device_id=0x2684;platforms[i].pm_cap=7; /* repeated model */
  }
  struct pci_dev *a=NULL,*b=NULL,*extra=NULL;
  CHECK(GuCompatOpenDevice(NULL)==EFI_INVALID_PARAMETER);
  fail_alloc=1;CHECK(GuCompatOpenDevice(&a)==EFI_OUT_OF_RESOURCES&&!a&&!Gu.compat_device);
  fail_alloc=0;CHECK(GuCompatOpenDevice(&a)==EFI_SUCCESS&&a&&Gu.compat_device==a);
  CHECK(a->owner==&Gu&&a->owner_bs==&bs&&a->owner_pci==&pci[0]&&a->owner_handle==&pci[0]);
  CHECK(GuCompatOpenDevice(&extra)==EFI_ALREADY_STARTED&&!extra);
  CHECK(a->dev.dma_mask==&a->mask_storage&&a->refs==1&&list_empty(&a->dev.devres_head));
  CHECK(dma_set_mask(&a->dev,42)==0&&a->mask_storage==42);
  CHECK(dma_set_coherent_mask(&a->dev,41)==0&&a->dev.coherent_dma_mask==41);
  void *group=devres_open_group(&a->dev,NULL,0);
  CHECK(group&&devres_release_group(&a->dev,group)==0&&!a->dev.scope);
  CHECK(pci_dev_get(a)==a&&a->refs==2);pci_dev_put(a);CHECK(a->refs==1);
  u16 value=0;CHECK(pci_read_config_word(a,0,&value)==0&&value==17&&reads==1);
  CHECK(GuCompatCloseDevice(a)==EFI_SUCCESS&&!Gu.compat_device_open&&Gu.compat_device==a);
  CHECK(pci_read_config_word(a,0,&value)==-EINVAL&&reads==1&&Gu.fault);
  Gu.fault=0; /* Test-only reset; production fault is sticky. */
  CHECK(GuCompatOpenDevice(&extra)==EFI_ALREADY_STARTED&&!extra);
  active=&platforms[1];
  CHECK(GuCompatOpenDevice(&b)==EFI_SUCCESS&&a!=b&&a->dev.dma_mask!=b->dev.dma_mask);
  CHECK(pci_read_config_word(b,0,&value)==0&&value==23&&reads==2);
  CHECK(pci_read_config_word(a,0,&value)==-EINVAL&&reads==2&&Gu.fault);
  CHECK(dma_set_mask(&a->dev,99)==-EINVAL&&a->mask_storage==42);
  CHECK(dma_set_coherent_mask(&a->dev,99)==-EINVAL&&a->dev.coherent_dma_mask==41);
  CHECK(!devres_open_group(&a->dev,NULL,0)&&!a->dev.scope);
  CHECK(devres_release_group(&a->dev,group)==-EINVAL);
  CHECK(!pci_dev_get(a));pci_dev_put(a);CHECK(a->refs==1);
  CHECK(!pci_dev_get(NULL)&&pci_read_config_word(NULL,0,&value)==-EINVAL&&reads==2);
  CHECK(GuCompatCloseDevice(a)==EFI_ACCESS_DENIED&&Gu.compat_device_open);
  CHECK(platforms[0].compat_device==a&&!platforms[0].compat_device_open&&!platforms[0].fault);
  Gu.bs=&foreign;CHECK(!GuCompatPciMatches(b));Gu.bs=&bs;
  Gu.pci=&pci[0];CHECK(!GuCompatPciMatches(b));Gu.pci=&pci[1];
  Gu.handle=&pci[0];CHECK(!GuCompatPciMatches(b));Gu.handle=&pci[1];
  CHECK(GuCompatPciMatches(b));
  CHECK(pci_dev_get(b)==b&&b->refs==2);
  CHECK(GuCompatCloseDevice(b)==EFI_DEVICE_ERROR&&!Gu.compat_device_open);
  CHECK(pci_read_config_word(b,0,&value)==-EINVAL&&reads==2);
  CHECK(allocs==3);
  free(a);free(b); /* End of simulated boot. */
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src=Path(directory)/"identity.c"
            src.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                exe=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(src),"-o",str(exe)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertLess(execution.index("GuCompatOpenDevice(&dev)"),execution.index("gpu_unlock_610_load_shared(dev,"))
        self.assertLess(execution.index("gpu_unlock_610_release_shared(&shared)"),execution.index("GuCompatCloseDevice(dev)"))
        self.assertLess(execution.index("GuCompatCloseDevice(dev)"),execution.index("GuMsiFinish()"))
        load=vendor.split("int gpu_unlock_610_load_shared(",1)[1].split("void gpu_unlock_610_release_shared(",1)[0]
        run=vendor.split("int gpu_unlock_610_unlock(",1)[1]
        self.assertLess(load.index("GuCompatPciMatches(pdev)"),load.index("pdev->device"))
        self.assertLess(run.index("GuCompatPciMatches(pdev)"),run.index("pdev->device"))
        self.assertLess(run.index("GuCompatPciMatches(pdev)"),run.index("Gu.display_check()"))
        self.assertLess(run.index("if (!work->pdev)"),run.index("unlock_transaction_assets(work)"))


if __name__ == "__main__":
    unittest.main()
