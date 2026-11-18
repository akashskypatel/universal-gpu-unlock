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
                "BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform) {")
ASSET_VERIFY = body(PROFILE, "BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform) {",
                    "BOOLEAN GuAd104PromMatches(")
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

    def test_actual_per_platform_asset_storage_and_reservation(self):
        compat = (ROOT / "src/compat.c").read_text(encoding="utf-8")
        asset_api = body(compat, "GU_ASSET *GuAssetEntries(VOID) {",
                         "\nvoid *devres_open_group(")
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
typedef struct {
  EFI_BOOT_SERVICES *bs;
  int started,owned,armed,fault,retained,asset_load_attempted,asset_entries_reserved;
  GU_ASSET asset_entries[GU_ASSET_COUNT];
} GU_PLATFORM;
static GU_PLATFORM platforms[2],*current=&platforms[0];
#define Gu (*current)
#define GuPlatformActive current
static int alloc_count,free_count,fail_alloc;
static EFI_STATUS Allocate(int kind,size_t bytes,VOID **out) {
  if(kind!=EfiLoaderData || !out)return EFI_INVALID_PARAMETER;
  alloc_count++;
  if(fail_alloc)return EFI_OUT_OF_RESOURCES;
  *out=calloc(1,bytes);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *ptr) {free_count++;free(ptr);return EFI_SUCCESS;}
""" + asset_api + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  static EFI_BOOT_SERVICES bs={Allocate,Free};
  Gu.bs=&bs;platforms[1].bs=&bs;
  CHECK(GuAssetReserveEntries(NULL,&bs)==EFI_NOT_READY);
  CHECK(GuAssetReserveEntries(&platforms[1],&bs)==EFI_NOT_READY);
  CHECK(!platforms[0].asset_entries_reserved&&!platforms[1].asset_entries_reserved&&!alloc_count);
  int mode=argc>1?argv[1][0]-'0':-1;
  if(mode==0) {
    GU_ASSET *before=GuAssetEntries();
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_SUCCESS);
    CHECK(Gu.asset_entries_reserved && GuAssetEntries()==before);
    CHECK(GuAssetEntries()==Gu.asset_entries && alloc_count==0 && free_count==0);
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_ALREADY_STARTED);
    Gu.asset_load_attempted=TRUE;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_ALREADY_STARTED);
  } else if(mode==1) {
    GuAssetEntries()[0].name="original";
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_ALREADY_STARTED);
    CHECK(!strcmp(GuAssetEntries()[0].name,"original"));
    CHECK(!Gu.asset_entries_reserved && alloc_count==0 && free_count==0);
  } else if(mode==2) {
    /* Reserving inline metadata must not request or depend on a new pool. */
    fail_alloc=1;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_SUCCESS);
    CHECK(Gu.asset_entries_reserved && alloc_count==0);
  } else if(mode==3) {
    Gu.started=1;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_NOT_READY && alloc_count==0);
    Gu.started=0;Gu.retained=1;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_NOT_READY && alloc_count==0);
    CHECK(!Gu.asset_entries_reserved);
  } else if(mode==4) {
    GuAssetEntries()[0].firmware.size=7;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_ALREADY_STARTED);
    CHECK(!Gu.asset_entries_reserved && GuAssetEntries()[0].firmware.size==7);
  } else if(mode==5) {
    bs.FreePool=NULL;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_NOT_READY && alloc_count==0);
    bs.FreePool=Free;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_SUCCESS);
  } else if(mode==6) {
    GU_ASSET *first=GuAssetEntries();
    first[0].name="first";
    current=&platforms[1];Gu.bs=&bs;
    GU_ASSET *second=GuAssetEntries();
    /* A populated first table must not be reachable by a fresh platform. */
    CHECK(second!=first && !second[0].name && !Gu.asset_load_attempted);
    CHECK(GuAssetReserveEntries(&platforms[0],&bs)==EFI_NOT_READY);
    CHECK(!Gu.asset_entries_reserved&&!platforms[0].asset_entries_reserved);
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_SUCCESS);
    second[0].name="second";Gu.asset_load_attempted=TRUE;
    current=&platforms[0];
    CHECK(GuAssetEntries()==first && !Gu.asset_load_attempted);
    CHECK(!strcmp(GuAssetEntries()[0].name,"first"));
    CHECK(!Gu.asset_entries_reserved);
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_ALREADY_STARTED);
    current=&platforms[1];
    CHECK(GuAssetEntries()==second && Gu.asset_load_attempted);
    CHECK(Gu.asset_entries_reserved && !strcmp(GuAssetEntries()[0].name,"second"));
    CHECK(alloc_count==0 && free_count==0);
  } else if(mode==7) {
    Gu.asset_load_attempted=TRUE;
    CHECK(GuAssetReserveEntries(&Gu,&bs)==EFI_ALREADY_STARTED);
    CHECK(!Gu.asset_entries_reserved && alloc_count==0);
  } else if(mode==8) {
    EFI_BOOT_SERVICES other=bs;
    CHECK(GuAssetReserveEntries(&Gu,&other)==EFI_NOT_READY);
    CHECK(!Gu.asset_entries_reserved && alloc_count==0);
  } else return 3;
  return 0;
}
"""
        self.compile_run(host, range(9))

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
typedef struct {int started,fault,retained,asset_load_attempted;} GU_PLATFORM;
static GU_PLATFORM Gu;
static VOID *Root=(VOID *)(uintptr_t)1;
static VOID *UnreleasedFilesystemHandles;
static EFI_STATUS BootFileStatus;
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
static BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform) {
  (void)platform;
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
    CHECK(GuAssets[1].name!=NULL && !Gu.asset_load_attempted);
  } else if(mode==4) {
    Gu.started=1;CHECK(GuBootLoadAssets()==EFI_NOT_READY && !Gu.asset_load_attempted);
    Gu.started=0;CHECK(GuBootLoadAssets()==EFI_SUCCESS);
  } else if(mode==5) {
    available=0;CHECK(GuBootLoadAssets()==EFI_UNSUPPORTED && !Gu.asset_load_attempted);
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
  } else if(mode==11) {
    UnreleasedFilesystemHandles=(VOID *)(uintptr_t)0x1234;
    CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==0 && !Gu.asset_load_attempted);
  } else if(mode==12) {
    BootFileStatus=EFI_NOT_READY;
    CHECK(GuBootLoadAssets()==EFI_NOT_READY && read_calls==0 && !Gu.asset_load_attempted);
  } else return 3;
  return 0;
}
'''
        self.compile_run(source, range(13))

    def test_actual_profile_rejects_wrong_gpu_and_failed_preparation(self):
        config=body(PROFILE,"static BOOLEAN ConfigFor(GU_PLATFORM *platform) {","/* The TU10x PMC")
        prepare=PROFILE[PROFILE.index("EFI_STATUS GuAd104Prepare(VOID) {"):]
        host=r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpu_inventory.h"
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef size_t UINTN;
typedef void VOID;
typedef int BOOLEAN;
typedef int EFI_STATUS;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_READY 1
#define EFI_DEVICE_ERROR 2
#define EFI_UNSUPPORTED 3
#define EFI_CRC_ERROR 4
#define EFI_OUT_OF_RESOURCES 5
#define EFI_COMPROMISED_DATA 6
#define EFI_ERROR(x) ((x)!=EFI_SUCCESS)
#define EfiPciIoWidthUint32 1
/* The production BAR gate: any firmware-assigned window of at least 16 MiB. */
#define GU_BAR_MIN_BYTES 0x1000000ULL
static uint64_t GuBarBaseFromConfig(const UINT32 *c) {
  /* Same decode as port.h: 32-bit BAR0 at 0x10, 64-bit when type bits say so. */
  uint64_t base = (uint64_t)(c[4] & 0xfffffff0U);
  if (((c[4] >> 1) & 3U) == 2U) base |= ((uint64_t)c[5]) << 32;
  return base;
}
typedef struct mock_pci mock_pci;
struct mock_pci {
  EFI_STATUS (*GetLocation)(mock_pci *,UINTN *,UINTN *,UINTN *,UINTN *);
  struct {int Read;} Pci;
  UINT32 config[64];
  UINTN segment,bus,device,function;
};
typedef struct {int pm,pcie,pcie_version,msi,msix;} GU_CAPS;
typedef struct {
  mock_pci *pci;void *handle;GU_GPU_RECORD discovery_identity;
  UINT16 device_id;
  uint64_t bar_base,bar_size;
  int pm_cap,pcie_cap,pcie_version,msi_cap,msix_cap;
  int fault,retained,started,is_turing,profile_verified,prom_verified;
  void *allocations;
} GU_PLATFORM;
static GU_PLATFORM cards[2],*active=&cards[0];
#define Gu (*active)
#define GuPlatformActive active
static mock_pci pci[2];
static int locations,reads,parses,rom_checks,latches,free_fails,read_fails;
static EFI_STATUS location(mock_pci *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
  locations++;*s=p->segment;*b=p->bus;*d=p->device;*f=p->function;return EFI_SUCCESS;
}
static EFI_STATUS GuConfigReadFor(GU_PLATFORM *platform,int width,UINT32 off,UINT32 *out) {
  (void)width;
  if(!platform||platform!=active){if(platform)platform->fault=1;return EFI_NOT_READY;}
  if(platform->fault)return EFI_DEVICE_ERROR;
  reads++;*out=platform->pci->config[off/4];return EFI_SUCCESS;
}
static BOOLEAN GuCheckConfig(UINT32 *c,UINTN s,UINTN b,UINTN d,UINTN f,GU_CAPS *caps) {
  (void)c;(void)s;(void)b;(void)d;(void)f;
  *caps=(GU_CAPS){1,2,3,4,5};return TRUE;
}
static BOOLEAN GuIsSupportedPciId(UINT32 id){return id==0x268410deU||id==0x220a10deU;}
static void GuLog(const char *fmt,...){(void)fmt;}
static BOOLEAN RegistersFor(GU_PLATFORM *platform,UINT32 *a,UINT32 *b){
  if(!platform||platform!=active){if(platform)platform->fault=1;return FALSE;}
  *a=1;*b=2;return !platform->fault;
}
static BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform){(void)platform;return TRUE;}
static void *GuAlloc(size_t n){return malloc(n);}
static void GuFree(void *p){free(p);if(free_fails)Gu.fault=Gu.retained=1;}
static BOOLEAN GuCopyFromIo(GU_PLATFORM *owner,void *p,const void *source,size_t bytes){
  (void)p;(void)source;(void)bytes;
  if(read_fails){owner->fault=1;return FALSE;}
  return TRUE;
}
static BOOLEAN GuAd104PromMatches(const GU_PLATFORM *platform,const void *p,size_t bytes){
  (void)platform;
  (void)p;(void)bytes;rom_checks++;return TRUE;
}
struct gpu_unlock_fwsec {int valid;};
static int gpu_unlock_vbios_fwsec(const void *p,size_t n,struct gpu_unlock_fwsec *fw){
  (void)p;(void)n;parses++;fw->valid=1;return 0;
}
static BOOLEAN GuAd104FwsecMatches(struct gpu_unlock_fwsec *fw){return fw->valid;}
static BOOLEAN GuLatchEntrySelectors(UINT32 a,UINT32 b){(void)a;(void)b;latches++;return TRUE;}
""" + config + prepare + r"""
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv){
  int mode=argc>1?atoi(argv[1]):-1;
  for(unsigned i=0;i<2;i++){
    active=&cards[i];pci[i].GetLocation=location;pci[i].Pci.Read=1;
    pci[i].segment=1;pci[i].bus=3+i;pci[i].device=2;pci[i].function=1;
    pci[i].config[0]=0x268410deU;pci[i].config[11]=0x123410deU;
    pci[i].config[2]=0x03000000U;pci[i].config[4]=0x80000000U;
    Gu.pci=&pci[i];Gu.handle=&cards[i];Gu.device_id=0x2684U;
    Gu.bar_base=0x80000000U;Gu.bar_size=0x1000000U;
    Gu.pm_cap=1;Gu.pcie_cap=2;Gu.pcie_version=3;Gu.msi_cap=4;Gu.msix_cap=5;
    Gu.discovery_identity=(GU_GPU_RECORD){.pci_id=pci[i].config[0],.subsystem_id=pci[i].config[11],
      .class_code=0x0300,.segment=1,.bus=3+i,.device=2,.function=1,
      .handle=Gu.handle,.pci=Gu.pci};
    CHECK(ConfigFor(&Gu));
  }
  CHECK(locations==2&&reads==128);
  unsigned before_reads=(unsigned)reads;
  CHECK(!ConfigFor(&cards[0])&&cards[0].fault&&reads==(int)before_reads);
  cards[0].fault=0;
  switch(mode){
    case 0:Gu.pci=&pci[0];break;
    case 1:Gu.handle=cards[0].handle;break;
    case 2:pci[1].bus=3;break;
    case 3:pci[1].segment=0;break;
    case 4:pci[1].device=3;break;
    case 5:pci[1].function=0;break;
    case 6:pci[1].config[0]=0x220a10deU;break;
    case 7:pci[1].config[11]^=1;break;
    case 8:pci[1].config[2]=0x03020000U;break;
    case 9:Gu.device_id=0x220aU;break;
    case 10:free_fails=1;break;
    case 11:read_fails=1;break;
    case 12:break;
    default:return 2;
  }
  if(mode<10){
    CHECK(!ConfigFor(&Gu));
    if(mode<6||mode==9)CHECK(reads==128);
    if(mode==0||mode==1||mode==9)CHECK(locations==2);
    CHECK(!parses&&!latches);
  }else{
    EFI_STATUS s=GuAd104Prepare();
    if(mode==12){
      CHECK(s==EFI_SUCCESS&&Gu.profile_verified&&Gu.prom_verified&&latches==1&&parses==1);
    }else{
      CHECK(s==EFI_DEVICE_ERROR&&!Gu.profile_verified&&!Gu.prom_verified&&!latches);
      if(mode==11)CHECK(!parses&&!rom_checks);
      else CHECK(parses==1&&rom_checks==1&&Gu.retained);
    }
  }
  CHECK(!cards[0].fault&&!cards[0].profile_verified&&!cards[0].prom_verified);
  return 0;
}
"""
        self.compile_run(host,range(13))

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
struct firmware {size_t size;const unsigned char *data;};
typedef struct {const char *name;struct firmware firmware;} GU_ASSET;
typedef struct {int is_turing,is_ampere;UINT16 device_id;GU_ASSET asset_entries[GU_ASSET_COUNT];} GU_PLATFORM;
static GU_PLATFORM platforms[2],*active=&platforms[0];
#define Gu (*active)
#define GuAssets (Gu.asset_entries)
static unsigned char data[2][GU_ASSET_COUNT];
static int hash_calls;
static int GuCpuPoolOwns(const GU_PLATFORM *owner,const void *p,size_t bytes) {
    unsigned index=(unsigned)(owner-platforms);
    for(unsigned i=0;i<GU_ASSET_COUNT;i++)
        if(p==&data[index][i]&&bytes==owner->asset_entries[i].firmware.size)return 1;
    return 0;
}
static int GuHashMatches(const void *p,size_t bytes,const char *hash) {
    (void)p;(void)bytes;(void)hash;hash_calls++;return 1;
}
#include <string.h>
''' + PREDICATES + MANIFEST + ASSET_VERIFY + r'''
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
  /* Same model, same manifest, independently allocated bytes. */
  for(unsigned n=0;n<2;n++) {
    active=&platforms[n];Gu.device_id=0x2684;Gu.is_turing=Gu.is_ampere=0;
    for(unsigned i=0;i<GU_ASSET_COUNT;i++) {
      GuAssets[i].name=ada->entries[i].name;
      GuAssets[i].firmware=(struct firmware){ada->entries[i].bytes,&data[n][i]};
    }
    CHECK(GuAd104AssetsValid(&Gu));
  }
  CHECK(hash_calls==10);
  CHECK(GuAd104AssetsValid(&platforms[0])&&active==&platforms[1]&&hash_calls==15);
  CHECK(GuPlatformAssetsOwned(&platforms[0])&&GuPlatformAssetsOwned(&platforms[1]));
  CHECK(active==&platforms[1]&&hash_calls==15);
  const unsigned char *own=GuAssets[0].firmware.data;
  GuAssets[0].firmware.data=platforms[0].asset_entries[0].firmware.data;
  CHECK(!GuAd104AssetsValid(&Gu)&&hash_calls==15); /* reject before content access */
  CHECK(!GuPlatformAssetsOwned(&platforms[1])&&GuPlatformAssetsOwned(&platforms[0]));
  GuAssets[0].firmware.data=own;
  const char *name=platforms[0].asset_entries[0].name;
  platforms[0].asset_entries[0].name=(const char *)(uintptr_t)1;
  CHECK(!GuPlatformAssetsOwned(&platforms[0])&&GuPlatformAssetsOwned(&platforms[1]));
  platforms[0].asset_entries[0].name=name;
  platforms[0].asset_entries[0].firmware.size--;
  CHECK(!GuPlatformAssetsOwned(&platforms[0])&&active==&platforms[1]&&hash_calls==15);
  platforms[0].asset_entries[0].firmware.size++;
  CHECK(GuAd104AssetsValid(&Gu)&&hash_calls==20);
  active=&platforms[0];CHECK(GuAd104AssetsValid(&Gu)&&hash_calls==25);
  return 0;
}
'''
        self.compile_run(source, (0,))


if __name__ == "__main__":
    unittest.main()
