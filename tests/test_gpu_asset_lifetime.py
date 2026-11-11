"""Compile and exercise the actual asset loader and per-card manifest selector.

Host tests replace only EFI services and file reads. No firmware data, GPU,
PCI write, or EFI boot operation is executed.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOOT = (ROOT / "src/boot_io.c").read_text(encoding="utf-8")
PROFILE = (ROOT / "src/profile.c").read_text(encoding="utf-8")
PORT = (ROOT / "src/port.h").read_text(encoding="utf-8")


def body(source, start, end):
    left = source.find(start)
    right = source.find(end, left)
    if left < 0 or right <= left:
        raise AssertionError("production function boundaries changed")
    return source[left:right]


LOADER = body(BOOT, "EFI_STATUS GuBootLoadAssets(VOID) {",
              "\nstatic EFI_STATUS ReadWindowsBinary(")
MANIFEST = body(PROFILE, "static const GU_ARCH_ASSETS AdaAssets=",
                "BOOLEAN GuAd104AssetsValid(VOID) {")
PREDICATES = body(PORT, "#define GU_PORT_MATCH(id)",
                  "static inline BOOLEAN GuIsAdaPciId(")


class AssetLifetimeTests(unittest.TestCase):
    def compile_run(self, code, cases):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "asset_host.c"
            binary = Path(tmp) / "asset_host"
            source.write_text(code, encoding="utf-8")
            compiler = os.environ.get("CC", "gcc")
            compiled = subprocess.run(
                [compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                 "-Werror", "-pedantic", "-I", str(ROOT / "src"),
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for case in cases:
                with self.subTest(compiler=compiler, case=case):
                    executed = subprocess.run(
                        [str(binary), str(case)], capture_output=True, text=True)
                    self.assertEqual(executed.returncode, 0,
                                     executed.stdout + executed.stderr)

    def test_actual_selected_asset_storage_and_failure_rollback(self):
        compat = (ROOT / "src/compat.c").read_text(encoding="utf-8")
        asset_api = body(compat, "GU_ASSET GuAssets[5];",
                         "\nvoid *devres_open_group(")
        selected = body(BOOT, "static BOOLEAN AssetsLoadAttempted;",
                        "\nstatic EFI_GUID LoadedGuid=")
        host = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef int BOOLEAN;
typedef uintptr_t EFI_STATUS;
typedef size_t UINTN;
struct firmware {size_t size;const unsigned char *data;};
typedef struct {const char *name;struct firmware firmware;} GU_ASSET;
#define GU_ASSET_COUNT 5U
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_ACCESS_DENIED 3
#define EFI_ALREADY_STARTED 4
#define EFI_NOT_READY 5
#define EFI_OUT_OF_RESOURCES 6
#define EfiLoaderData 7
typedef struct EFI_BOOT_SERVICES {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
static struct {
  EFI_BOOT_SERVICES *bs;
  int started,owned,armed,fault,retained;
} Gu;
static int alloc_count,free_count,fail_alloc;
static EFI_STATUS Allocate(int kind,size_t bytes,VOID **out) {
  if(kind!=EfiLoaderData || !out)return EFI_INVALID_PARAMETER;
  alloc_count++;
  if(fail_alloc)return EFI_OUT_OF_RESOURCES;
  *out=calloc(1,bytes);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *ptr) {free_count++;free(ptr);return EFI_SUCCESS;}
""" + asset_api + selected + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  static EFI_BOOT_SERVICES bs={Allocate,Free};
  Gu.bs=&bs;
  int mode=argc>1?argv[1][0]-'0':-1;
  if(mode==0) {
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_SUCCESS);
    CHECK(SelectedAssets && alloc_count==1 && GuAssetEntries()!=GuAssets);
    CHECK(GuAssetEntries()==SelectedAssets->entries);
    CHECK(SelectedAssetAttempted==&SelectedAssets->attempted);
    AssetsLoadAttempted=TRUE;
    CHECK(SelectedAssets->attempted && GuBootReserveSelectedAssets(&bs)==EFI_ALREADY_STARTED);
    CHECK(GuAssetSelectEntries(GuAssets)==EFI_ALREADY_STARTED);
  } else if(mode==1) {
    GuAssets[0].name="original";
    GU_ASSET other[GU_ASSET_COUNT]={0};
    CHECK(GuAssetSelectEntries(other)==EFI_ALREADY_STARTED);
    CHECK(GuAssetEntries()==GuAssets);
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_ALREADY_STARTED);
    CHECK(!SelectedAssets && alloc_count==1 && free_count==1);
  } else if(mode==2) {
    fail_alloc=1;
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_OUT_OF_RESOURCES);
    CHECK(!SelectedAssets && GuAssetEntries()==GuAssets);
    fail_alloc=0;
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_SUCCESS && alloc_count==2);
  } else if(mode==3) {
    Gu.started=1;
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_NOT_READY && alloc_count==0);
    Gu.started=0;Gu.retained=1;
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_NOT_READY && alloc_count==0);
  } else if(mode==4) {
    GU_ASSET other[GU_ASSET_COUNT]={0};
    other[0].name="foreign";
    CHECK(GuAssetSelectEntries(other)==EFI_ACCESS_DENIED);
    CHECK(GuAssetEntries()==GuAssets);
  } else if(mode==5) {
    bs.FreePool=NULL;
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_NOT_READY && alloc_count==0);
    bs.FreePool=Free;
    CHECK(GuBootReserveSelectedAssets(&bs)==EFI_SUCCESS);
  } else return 3;
  return 0;
}
"""
        self.compile_run(host, range(6))

    def test_loader_prevents_cross_target_asset_replacement(self):
        source = r'''
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
typedef void VOID;
typedef int EFI_STATUS;
typedef unsigned long UINTN;
typedef int BOOLEAN;
typedef wchar_t CHAR16;
#define TRUE 1
#define FALSE 0
#define GU_ASSET_COUNT 5U
#define EFI_SUCCESS 0
#define EFI_NOT_READY 2
#define EFI_ALREADY_STARTED 3
#define EFI_UNSUPPORTED 4
#define EFI_NOT_FOUND 5
#define EFI_CRC_ERROR 6
typedef struct {const CHAR16 *path;const char *name;const char *sha256;UINTN bytes;} GU_ARCH_ASSET;
typedef struct {GU_ARCH_ASSET entries[GU_ASSET_COUNT];const CHAR16 *alternate_gsp_path;} GU_ARCH_ASSETS;
struct firmware {UINTN size;const VOID *data;};
typedef struct {const char *name;struct firmware firmware;} LOADED;
static LOADED GuAssets[GU_ASSET_COUNT];
static struct {int started,fault,retained;} Gu;
static VOID *Root=(VOID *)(uintptr_t)1;
static BOOLEAN AssetsLoadAttempted;
static int read_calls,freed,fail_at,corrupt_at,available=1;
static const GU_ARCH_ASSETS bundle={{
  {L"a","A","1",8},{L"b","B","2",8},{L"c","C","3",8},
  {L"d","D","4",8},{L"e","E","5",8}},NULL};
static const GU_ARCH_ASSETS *GuGetArchAssets(VOID) {return available?&bundle:NULL;}
static EFI_STATUS ReadFile(VOID *root,const CHAR16 *path,UINTN bytes,VOID **out) {
  (void)root;(void)path;
  read_calls++;
  if(read_calls==fail_at)return EFI_NOT_FOUND;
  *out=malloc(bytes);
  return *out?EFI_SUCCESS:EFI_NOT_READY;
}
static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(const GU_ARCH_ASSET *asset,const CHAR16 *path,VOID **out) {
  (void)asset;(void)path;(void)out;return EFI_NOT_FOUND;
}
static BOOLEAN GuHashMatches(const VOID *data,UINTN bytes,const char *hash) {
  (void)bytes;(void)hash;return data!=NULL && read_calls!=corrupt_at;
}
static BOOLEAN GuAd104AssetsValid(VOID) {
  for(unsigned i=0;i<GU_ASSET_COUNT;i++)
    if(!GuAssets[i].name||!GuAssets[i].firmware.data)return FALSE;
  return TRUE;
}
static void GuFree(VOID *p) {if(p){freed++;free(p);}}
static void GuLog(const char *fmt,...) {(void)fmt;}
''' + LOADER + r'''
#define CHECK(e) do {if(!(e)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);return 1;}} while(0)
int main(int argc,char **argv) {
  int mode=argc>1?atoi(argv[1]):-1;
  if(mode==0) {
    CHECK(GuBootLoadAssets()==EFI_SUCCESS);
    CHECK(read_calls==5 && GuAssets[0].name!=NULL);
    const VOID *saved=GuAssets[0].firmware.data;
    CHECK(GuBootLoadAssets()==EFI_NOT_READY);
    CHECK(read_calls==5 && GuAssets[0].firmware.data==saved);
  } else if(mode==1) {
    fail_at=3;
    CHECK(GuBootLoadAssets()==EFI_NOT_FOUND);
    CHECK(read_calls==3 && freed==2);
    for(int i=0;i<5;i++)CHECK(!GuAssets[i].name&&!GuAssets[i].firmware.data);
    fail_at=0;
    CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==3);
  } else if(mode==2) {
    Gu.retained=1;CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==0);
    Gu.retained=0;CHECK(GuBootLoadAssets()==EFI_SUCCESS && read_calls==5);
  } else if(mode==3) {
    GuAssets[1].name="existing";
    CHECK(GuBootLoadAssets()==EFI_ALREADY_STARTED && read_calls==0);
    CHECK(GuAssets[1].name!=NULL && !AssetsLoadAttempted);
  } else if(mode==4) {
    Gu.started=1;CHECK(GuBootLoadAssets()==EFI_NOT_READY && !AssetsLoadAttempted);
    Gu.started=0;CHECK(GuBootLoadAssets()==EFI_SUCCESS);
  } else if(mode==5) {
    available=0;CHECK(GuBootLoadAssets()==EFI_UNSUPPORTED && !AssetsLoadAttempted);
    available=1;CHECK(GuBootLoadAssets()==EFI_SUCCESS);
  } else if(mode==6) {
    Gu.fault=1;CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==0);
    Gu.fault=0;CHECK(GuBootLoadAssets()==EFI_SUCCESS);
  } else if(mode==7) {
    corrupt_at=2;CHECK(GuBootLoadAssets()==EFI_CRC_ERROR);
    CHECK(read_calls==2 && freed==2);
    corrupt_at=0;CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==2);
  } else if(mode==8) {
    GuAssets[2].firmware.data=(VOID *)(uintptr_t)0x1234;
    CHECK(GuBootLoadAssets()==EFI_ALREADY_STARTED && read_calls==0);
  } else if(mode==9) {
    GuAssets[2].firmware.size=7;
    CHECK(GuBootLoadAssets()==EFI_ALREADY_STARTED && read_calls==0);
  } else if(mode==10) {
    Root=NULL;CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==0);
  } else return 3;
  return 0;
}
'''
        self.compile_run(source, range(11))

    def test_parent_supported_reference_manifests(self):
        source = r'''
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include "gpu_device_ids.h"
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef unsigned long UINTN;
typedef wchar_t CHAR16;
typedef void VOID;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define GU_ASSET_COUNT 5U
typedef struct {const CHAR16 *path;const char *name;const char *sha256;UINTN bytes;} GU_ARCH_ASSET;
typedef struct {GU_ARCH_ASSET entries[GU_ASSET_COUNT];const CHAR16 *alternate_gsp_path;} GU_ARCH_ASSETS;
static struct {int is_turing,is_ampere;UINT16 device_id;} Gu;
''' + PREDICATES + MANIFEST + r'''
#define CHECK(x) do {if(!(x))return __LINE__;} while(0)
int main(void) {
  const GU_ARCH_ASSETS *ada=GuGetArchAssetsForPciId(0x268410deU);
  const GU_ARCH_ASSETS *ampere=GuGetArchAssetsForPciId(0x220a10deU);
  const GU_ARCH_ASSETS *turing=GuGetArchAssetsForPciId(0x1e0710deU);
  CHECK(ada && ampere && turing && ada!=ampere && ada!=turing && ampere!=turing);
  CHECK(ada->entries[0].bytes==57672U);
  CHECK(ampere->entries[0].bytes==61256U);
  CHECK(turing->entries[0].bytes==59976U);
  CHECK(turing->entries[2].bytes==29352832U);
  CHECK(turing->entries[4].bytes==76U);
  CHECK(!GuGetArchAssetsForPciId(0x26841002U));
  CHECK(!GuGetArchAssetsForPciId(0x220a10eeU));
  CHECK(!GuGetArchAssetsForPciId(0x999910deU));
  CHECK(GuIsSupportedDeviceId(0x2684U));
  CHECK(GuIsSupportedDeviceId(0x2782U));
  CHECK(GuIsSupportedDeviceId(0x2414U));
  CHECK(GuIsSupportedDeviceId(0x1e07U));
  CHECK(!GuIsSupportedDeviceId(0x9999U));
  CHECK(GuGetArchAssetsForPciId(0x270210deU)==ada);
  CHECK(GuGetArchAssetsForPciId(0x278210deU)==ada);
  CHECK(GuGetArchAssetsForPciId(0x288210deU)==ada);
  CHECK(GuGetArchAssetsForPciId(0x250310deU)==ampere);
  CHECK(GuGetArchAssetsForPciId(0x248410deU)==ampere);
  CHECK(GuGetArchAssetsForPciId(0x241410deU)==ampere);
  CHECK(GuGetArchAssetsForPciId(0x1f4710deU)==turing);
  CHECK(GuGetArchAssetsForPciId(0x1e8110deU)==turing);
  Gu.device_id=0x2684U;CHECK(GuGetArchAssets()==ada);
  Gu.is_ampere=1;CHECK(!GuGetArchAssets());
  Gu.is_ampere=0;Gu.device_id=0x220aU;
  CHECK(!GuGetArchAssets());
  Gu.is_ampere=1;CHECK(GuGetArchAssets()==ampere);
  Gu.is_turing=1;CHECK(!GuGetArchAssets());
  Gu.is_ampere=0;Gu.device_id=0x1e07U;CHECK(GuGetArchAssets()==turing);
  Gu.is_turing=0;CHECK(!GuGetArchAssets());
  return 0;
}
'''
        self.compile_run(source, (0,))


if __name__ == "__main__":
    unittest.main()
