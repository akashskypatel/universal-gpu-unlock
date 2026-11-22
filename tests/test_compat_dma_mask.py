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
        helpers=compat[compat.index("static BOOLEAN GuCompatRecordMatches("):compat.index("GU_ASSET *GuAssetEntries(")]
        scope=compat[compat.index("void *devres_open_group("):compat.index("/* A wait belongs")]
        timing=compat[compat.index("ktime_t GuCompatTime("):compat.index("/* Vendor access")]
        masks=compat[compat.index("int dma_set_mask("):compat.index("void GuCompatDmaBarrier(")]
        mmio=compat[compat.index("u32 GuCompatRead32("):compat.index("/* The read-only profile")]
        copy=compat[compat.index("BOOLEAN GuCopyFromIo("):compat.index("void *dma_alloc_coherent(")]
        barrier=compat[compat.index("void GuCompatDmaBarrier("):compat.index("void *GuCompatAlloc(")]
        scratch=compat[compat.index("void *GuCompatAlloc("):compat.index("int request_firmware(")]
        read=compat[compat.index("int pci_read_config_word("):compat.index("int pci_read_config_dword(")]
        refs=compat[compat.index("struct pci_dev *pci_dev_get("):]
        host=r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef uint16_t u16;
typedef uint64_t u64;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t ktime_t;
typedef uintptr_t UINTN;
typedef uint32_t u32;
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef void *EFI_HANDLE;
typedef struct EFI_BOOT_SERVICES {
  int id; EFI_STATUS (*Stall)(UINTN);
} EFI_BOOT_SERVICES;
typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
struct EFI_PCI_IO_PROTOCOL {
  int id; EFI_STATUS (*Flush)(EFI_PCI_IO_PROTOCOL *);
  struct { EFI_STATUS (*Read)(EFI_PCI_IO_PROTOCOL *,int,UINT8,UINT64,UINTN,VOID *); } Mem;
};
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
#define EfiPciIoWidthUint8 0
typedef struct {
  EFI_BOOT_SERVICES *bs;EFI_PCI_IO_PROTOCOL *pci;EFI_HANDLE handle;
  unsigned device_id,pm_cap;
  UINT64 bar_base,bar_size,ticks;
  int fault,retained,started,owned,armed,display_ok;
  int compat_device_open,compat_device_closed;
  struct pci_dev *compat_device;
} GU_PLATFORM;
static GU_PLATFORM platforms[2],*active=&platforms[0];
/* The trace is a port facility (src/boot_io.c) and this harness supplies the
 * port. Copies and delays are counted here, not printed: the trace lines are
 * inert so the chunk accounting this test is about stays exact. */
static void __attribute__((unused)) GuTracePhase(const char *phase){(void)phase;}
static void __attribute__((unused)) GuTraceDetail(const char *phase,const char *format,...){(void)phase;(void)format;}
static void __attribute__((unused)) GuTraceProgress(const char *phase,UINTN done,UINTN total){(void)phase;(void)done;(void)total;}
static void __attribute__((unused)) GuTraceDelay(UINT64 ticks){(void)ticks;}
#define Gu (*active)
#define GuPlatformActive active
static int allocs,reads,fail_alloc,wrong_pool_owner,change_active_after_alloc;
static int change_reservation_identity, fault_after_alloc,wrong_allocator_argument;
static int mmio_reads,mmio_writes,copies,flushes;
static int last_io_owner,stalls,fail_stall;
static EFI_STATUS Stall(UINTN us){
  (void)us;stalls++;
  return fail_stall?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static UINT32 GuRead32(UINT32 offset){mmio_reads++;last_io_owner=Gu.pci->id;return offset;}
static void GuWrite32(GU_PLATFORM *owner,UINT32 offset,UINT32 value){(void)offset;(void)value;mmio_writes++;last_io_owner=owner->pci->id;}
static int fail_copy_at;
static EFI_STATUS ReadMemory(EFI_PCI_IO_PROTOCOL *pci,int width,UINT8 bar,UINT64 offset,UINTN n,VOID *dst){
  if(width!=EfiPciIoWidthUint8||bar||offset>0x4000||n>1024||n>0x4000-offset)return EFI_DEVICE_ERROR;
  copies++;last_io_owner=pci->id;memset(dst,0x5a,n);
  return copies==fail_copy_at?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Flush(EFI_PCI_IO_PROTOCOL *pci){flushes++;last_io_owner=pci->id;return EFI_SUCCESS;}
static const void *pool[2];
static size_t pool_bytes[2];
static void *temporary[2];
static int scratch_allocs,scratch_frees;
static int GuCpuPoolCompatEmpty(const GU_PLATFORM *p){return !temporary[p-platforms];}
static void *GuCpuPoolAllocScoped(const struct pci_dev *scope,size_t n){
  if(scope!=Gu.compat_device||!n||temporary[active-platforms])return NULL;
  scratch_allocs++;return temporary[active-platforms]=calloc(1,n);
}
static int GuCpuPoolFreeScoped(const struct pci_dev *scope,void *ptr){
  if(!ptr)return 1;
  if(scope!=Gu.compat_device||ptr!=temporary[active-platforms])return 0;
  scratch_frees++;free(ptr);temporary[active-platforms]=NULL;return 1;
}
static void *GuAllocFor(GU_PLATFORM *owner,size_t n){
  if(owner!=active){wrong_allocator_argument++;return NULL;}
  allocs++;if(fail_alloc)return NULL;
  void *p=calloc(1,n);
  size_t slot=(size_t)(owner-platforms)^(size_t)wrong_pool_owner;
  pool[slot]=p;pool_bytes[slot]=n;
  if(change_active_after_alloc)active=&platforms[1];
  if(change_reservation_identity==1)owner->bs=NULL;
  if(change_reservation_identity==2)owner->pci=NULL;
  if(change_reservation_identity==3)owner->handle=NULL;
  if(change_reservation_identity==4)owner->device_id++;
  if(fault_after_alloc)owner->fault=1;
  return p;
}
static int GuCpuPoolOwns(const GU_PLATFORM *p,const void *m,size_t bytes){
  return (p==&platforms[0]||p==&platforms[1])&&m&&pool[p-platforms]==m&&bytes==pool_bytes[p-platforms];
}
static GU_PLATFORM *GuPlatformForCpuObject(const void *m,size_t bytes){
  GU_PLATFORM *found=NULL;
  for(int i=0;i<2;i++)if(GuCpuPoolOwns(&platforms[i],m,bytes)){
    if(found)return NULL;
    found=&platforms[i];
  }
  return found;
}
static EFI_STATUS GuConfigRead(int width,UINT32 offset,u16 *out){
  (void)width;(void)offset;reads++;*out=(u16)Gu.pci->id;return EFI_SUCCESS;
}
""" + structs + helpers + scope + timing + masks + mmio + copy + barrier + scratch + read + refs + r"""
#define CHECK(e) do {if(!(e)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);return 1;}}while(0)
int main(void){
  EFI_BOOT_SERVICES bs={1,Stall},foreign={2,Stall};
  EFI_PCI_IO_PROTOCOL pci[2]={{17,Flush,{ReadMemory}},{23,Flush,{ReadMemory}}};
  void *bar=(void *)(uintptr_t)0x1004;
  unsigned char buffer[4];
  for(int i=0;i<2;i++){
    platforms[i].bs=&bs;platforms[i].pci=&pci[i];platforms[i].handle=&pci[i];
    platforms[i].device_id=0x2684;platforms[i].pm_cap=7; /* repeated model */
    platforms[i].bar_base=0x1000; /* deliberately equal numeric BAR addresses */
    platforms[i].bar_size=0x4000;
  }
  struct pci_dev *a=NULL,*b=NULL,*extra=NULL;
  CHECK(GuCompatReserveDevice(NULL)==EFI_NOT_READY);
  CHECK(GuCompatReserveDevice(&platforms[1])==EFI_NOT_READY);
  CHECK(GuCompatOpenDevice(&platforms[1],&extra)==EFI_NOT_READY);
  CHECK(!extra&&!allocs&&!platforms[0].compat_device&&!platforms[1].compat_device);
  CHECK(GuCompatOpenDevice(&Gu,NULL)==EFI_INVALID_PARAMETER);
  CHECK(GuCompatOpenDevice(&Gu,&a)==EFI_NOT_READY&&!a&&allocs==0);
  Gu.owned=1;CHECK(GuCompatReserveDevice(&Gu)==EFI_NOT_READY&&allocs==0);Gu.owned=0;
  Gu.armed=1;CHECK(GuCompatReserveDevice(&Gu)==EFI_NOT_READY&&allocs==0);Gu.armed=0;
  Gu.started=1;CHECK(GuCompatReserveDevice(&Gu)==EFI_NOT_READY&&allocs==0);Gu.started=0;
  fail_alloc=1;CHECK(GuCompatReserveDevice(&Gu)==EFI_OUT_OF_RESOURCES&&!a&&!Gu.compat_device);
  fail_alloc=0;CHECK(GuCompatReserveDevice(&Gu)==EFI_SUCCESS&&Gu.compat_device&&!Gu.compat_device_open);
  CHECK(GuCompatReserveDevice(&Gu)==EFI_ALREADY_STARTED&&allocs==2);
  CHECK(!GuCompatCompletedPlatform(&Gu));
  CHECK(GuCompatOpenDevice(&Gu,&a)==EFI_NOT_READY&&!a&&allocs==2);
  u16 early=0;CHECK(pci_read_config_word(Gu.compat_device,0,&early)==-EINVAL&&reads==0&&Gu.fault);
  Gu.fault=0; /* Test-only reset. No production retry. */
  Gu.owned=Gu.armed=Gu.display_ok=1;
  Gu.pm_cap=9;
  CHECK(GuCompatOpenDevice(&Gu,&a)==EFI_SUCCESS&&a&&Gu.compat_device==a);
  CHECK(a->pm_cap==9&&allocs==2);
  CHECK(GuCompatTime(a)==0&&GuCompatDelay(a,100)&&GuCompatTime(a)==100&&stalls==1);
  CHECK(GuCompatRead32(a,bar)==4&&mmio_reads==1&&last_io_owner==17);
  GuCompatWrite32(a,12,bar);CHECK(mmio_writes==1&&last_io_owner==17);
  CHECK(GuCompatCopyFromIo(a,buffer,bar,sizeof(buffer)));CHECK(copies==1&&buffer[0]==0x5a&&last_io_owner==17);
  GuCompatDmaBarrier(a);CHECK(flushes==1&&last_io_owner==17);
  CHECK(a->owner==&Gu&&a->owner_bs==&bs&&a->owner_pci==&pci[0]&&a->owner_handle==&pci[0]);
  CHECK(GuCompatOpenDevice(&Gu,&extra)==EFI_ALREADY_STARTED&&!extra);
  CHECK(a->dev.dma_mask==&a->mask_storage&&a->refs==1&&list_empty(&a->dev.devres_head));
  CHECK(dma_set_mask(&a->dev,42)==0&&a->mask_storage==42);
  CHECK(dma_set_coherent_mask(&a->dev,41)==0&&a->dev.coherent_dma_mask==41);
  void *group=devres_open_group(&a->dev,NULL,0);
  CHECK(group&&devres_release_group(&a->dev,group)==0&&!a->dev.scope);
  CHECK(pci_dev_get(a)==a&&a->refs==2);pci_dev_put(a);CHECK(a->refs==1);
  u16 value=0;CHECK(pci_read_config_word(a,0,&value)==0&&value==17&&reads==1);
  CHECK(!GuCompatCompletedPlatform(&Gu));
  CHECK(!GuCompatAlloc(a,(size_t)-1,2)&&scratch_allocs==0);
  void *first=GuCompatAlloc(a,2,16);CHECK(first&&scratch_allocs==1);
  CHECK(!GuCpuPoolCompatEmpty(&Gu)&&GuCompatFree(a,first)&&scratch_frees==1);
  active=&platforms[1];
  CHECK(GuCompatReserveDevice(&platforms[0])==EFI_NOT_READY&&allocs==2);
  CHECK(GuCompatOpenDevice(&platforms[0],&extra)==EFI_NOT_READY&&!extra);
  CHECK(platforms[0].compat_device_open&&!platforms[0].fault&&!Gu.compat_device&&!Gu.fault);
  CHECK(GuCompatOwner(a)==&platforms[0]&&!platforms[0].fault);
  CHECK(!GuCompatPciMatches(a)&&GuCompatTime(a)==100&&!platforms[0].fault);
  CHECK(dma_set_mask(&a->dev,43)==0&&a->mask_storage==43);
  CHECK(pci_dev_get(a)==a&&a->refs==2);pci_dev_put(a);CHECK(a->refs==1);
  active=&platforms[0];
  CHECK(GuCompatCloseDevice(a)==EFI_SUCCESS&&!Gu.compat_device_open&&Gu.compat_device_closed&&Gu.compat_device==a);
  CHECK(GuCompatCompletedPlatform(&Gu));
  CHECK(pci_read_config_word(a,0,&value)==-EINVAL&&reads==1&&Gu.fault);
  Gu.fault=0; /* Test-only reset; production fault is sticky. */
  CHECK(GuCompatOpenDevice(&Gu,&extra)==EFI_ALREADY_STARTED&&!extra);
  active=&platforms[1];
  CHECK(GuCompatReserveDevice(&Gu)==EFI_SUCCESS&&allocs==3);
  CHECK(Gu.compat_device!=a&&!Gu.compat_device_open);
  Gu.owned=Gu.armed=Gu.display_ok=1;
  Gu.handle=&pci[0];
  CHECK(GuCompatOpenDevice(&Gu,&b)==EFI_ACCESS_DENIED&&!b&&Gu.fault&&allocs==3);
  Gu.handle=&pci[1];Gu.fault=0; /* Test-only reset. */
  CHECK(GuCompatOpenDevice(&Gu,&b)==EFI_SUCCESS&&a!=b&&a->dev.dma_mask!=b->dev.dma_mask&&allocs==3);
  CHECK(GuCompatOwner(b)==&platforms[1]&&!Gu.fault);
  CHECK(!GuCompatOwner(a)&&platforms[0].fault&&!Gu.fault);
  platforms[0].fault=0; /* Independent host cases. */
  b->refs=0;
  CHECK(!GuCompatOwner(b)&&Gu.fault);
  Gu.fault=0;b->refs=1;
  CHECK(GuCompatCompletedPlatform(&platforms[0])&&!GuCompatCompletedPlatform(&platforms[1]));
  CHECK(active==&platforms[1]);
  platforms[0].compat_device_closed=0;CHECK(!GuCompatCompletedPlatform(&platforms[0]));platforms[0].compat_device_closed=1;
  platforms[0].compat_device=b;CHECK(!GuCompatCompletedPlatform(&platforms[0]));platforms[0].compat_device=a;
  a->owner=&platforms[1];CHECK(!GuCompatCompletedPlatform(&platforms[0]));a->owner=&platforms[0];
  a->refs=2;CHECK(!GuCompatCompletedPlatform(&platforms[0]));a->refs=1;
  a->dev.scope=true;CHECK(!GuCompatCompletedPlatform(&platforms[0]));a->dev.scope=false;
  a->dev.devres_head.count=1;CHECK(!GuCompatCompletedPlatform(&platforms[0]));a->dev.devres_head.count=0;
  a->dev.dma_mask=&b->mask_storage;CHECK(!GuCompatCompletedPlatform(&platforms[0]));a->dev.dma_mask=&a->mask_storage;
  pool[0]=NULL;CHECK(!GuCompatCompletedPlatform(&platforms[0]));pool[0]=a;
  CHECK(GuCompatCompletedPlatform(&platforms[0])&&active==&platforms[1]);
  temporary[0]=&platforms[0];CHECK(!GuCompatCompletedPlatform(&platforms[0]));temporary[0]=NULL;
  void *second=GuCompatAlloc(b,1,16);CHECK(second&&scratch_allocs==2);
  CHECK(!GuCompatAlloc(a,1,16)&&scratch_allocs==2&&platforms[0].fault&&!Gu.fault);
  platforms[0].fault=0;
  CHECK(!GuCompatFree(a,second)&&scratch_frees==1&&temporary[1]==second&&platforms[0].fault&&!Gu.fault);
  platforms[0].fault=0;
  GuCompatRetain(a);CHECK(!Gu.retained&&platforms[0].fault&&!Gu.fault);
  platforms[0].fault=0; /* Continue independent fixture cases. */
  CHECK(GuCompatFree(b,second)&&scratch_frees==2&&GuCpuPoolCompatEmpty(&Gu));
  GuCompatRetain(b);CHECK(Gu.retained);
  Gu.retained=0; /* Test-only reset. */
  CHECK(pci_read_config_word(b,0,&value)==0&&value==23&&reads==2);
  CHECK(GuCompatTime(b)==0&&GuCompatDelay(b,200)&&GuCompatTime(b)==200&&stalls==2);
  CHECK(GuCompatTime(a)==0&&platforms[0].fault&&!GuCompatDelay(a,100)&&stalls==2&&!Gu.fault);
  CHECK(Gu.ticks==200&&platforms[0].ticks==100);
  platforms[0].fault=0; /* Test-only reset. */
  fail_stall=1;CHECK(!GuCompatDelay(b,25)&&Gu.fault&&Gu.retained&&Gu.ticks==200&&stalls==3);
  CHECK(!GuCompatDelay(b,25)&&stalls==3);
  fail_stall=0;Gu.fault=Gu.retained=0; /* Continue independent fixture cases. */
  CHECK(GuCompatRead32(b,bar)==4&&mmio_reads==2&&last_io_owner==23);
  GuCompatWrite32(b,12,bar);CHECK(mmio_writes==2&&last_io_owner==23);
  CHECK(GuCompatCopyFromIo(b,buffer,bar,sizeof(buffer)));CHECK(copies==2&&buffer[0]==0x5a&&last_io_owner==23);
  GuCompatDmaBarrier(b);CHECK(flushes==2&&last_io_owner==23);
  CHECK(GuCompatRead32(a,bar)==~0U&&mmio_reads==2&&platforms[0].fault&&!Gu.fault);
  GuCompatWrite32(a,12,bar);CHECK(mmio_writes==2);
  CHECK(!GuCompatCopyFromIo(a,buffer,bar,sizeof(buffer)));CHECK(copies==2&&buffer[0]==0&&buffer[3]==0);
  GuCompatDmaBarrier(a);CHECK(flushes==2&&last_io_owner==23);
  CHECK(platforms[0].fault&&!Gu.fault);
  platforms[0].fault=0;
  CHECK(pci_read_config_word(a,0,&value)==-EINVAL&&reads==2&&platforms[0].fault&&!Gu.fault);
  CHECK(dma_set_mask(&a->dev,99)==-EINVAL&&a->mask_storage==43);
  CHECK(dma_set_coherent_mask(&a->dev,99)==-EINVAL&&a->dev.coherent_dma_mask==41);
  CHECK(!devres_open_group(&a->dev,NULL,0)&&!a->dev.scope);
  CHECK(devres_release_group(&a->dev,group)==-EINVAL);
  CHECK(!pci_dev_get(a));pci_dev_put(a);CHECK(a->refs==1);
  CHECK(!pci_dev_get(NULL)&&pci_read_config_word(NULL,0,&value)==-EINVAL&&reads==2);
  CHECK(GuCompatCloseDevice(a)==EFI_ACCESS_DENIED&&Gu.compat_device_open);
  CHECK(platforms[0].compat_device==a&&!platforms[0].compat_device_open&&platforms[0].fault);
  platforms[0].fault=0;
  Gu.bs=&foreign;CHECK(!GuCompatPciMatches(b));Gu.bs=&bs;
  Gu.pci=&pci[0];CHECK(!GuCompatPciMatches(b));Gu.pci=&pci[1];
  Gu.handle=&pci[0];CHECK(!GuCompatPciMatches(b));Gu.handle=&pci[1];
  CHECK(GuCompatPciMatches(b));
  /* Move a multi-part read through the real owner-bound copy routine. */
  Gu.fault=0;
  unsigned char multi[2050];
  CHECK(GuCompatCopyFromIo(b,multi,bar,sizeof(multi))&&copies==5&&last_io_owner==23);
  CHECK(multi[0]==0x5a&&multi[2049]==0x5a);
  fail_copy_at=copies+2;
  CHECK(!GuCompatCopyFromIo(b,multi,bar,sizeof(multi))&&copies==7&&Gu.fault);
  for(size_t i=0;i<sizeof(multi);i++)CHECK(multi[i]==0);
  Gu.fault=0;fail_copy_at=0;
  CHECK(!GuCopyFromIo(&platforms[0],multi,bar,sizeof(multi))&&copies==7&&!Gu.fault&&platforms[0].fault);
  platforms[0].fault=0; /* Test-only reset after the stale-owner negative case. */
  Gu.fault=0;pci[1].Mem.Read=NULL;
  CHECK(!GuCompatCopyFromIo(b,multi,bar,sizeof(multi))&&copies==7&&Gu.fault);
  pci[1].Mem.Read=ReadMemory;Gu.fault=0;
  CHECK(!GuCompatCopyFromIo(b,multi,(void *)(uintptr_t)0x4fff,sizeof(multi))&&copies==7&&Gu.fault);
  /* Inject independent boundary cases without changing the completed peer. */
  Gu.fault=0;
  pci_dev_put(b);CHECK(b->refs==1&&Gu.fault&&a->refs==1);
  Gu.fault=0;b->refs=INT_MAX;
  CHECK(!pci_dev_get(b)&&b->refs==INT_MAX&&Gu.fault&&a->refs==1);
  Gu.fault=0;b->refs=0;
  CHECK(!pci_dev_get(b)&&b->refs==0&&Gu.fault);
  Gu.fault=0;b->refs=-1;
  pci_dev_put(b);CHECK(b->refs==-1&&Gu.fault);
  Gu.fault=0;b->refs=1; /* Independent host input, never a production reset. */
  CHECK(pci_dev_get(b)==b&&b->refs==2);
  pci_dev_put(b);CHECK(b->refs==1&&!Gu.fault);
  CHECK(pci_dev_get(b)==b&&b->refs==2);
  CHECK(GuCompatCloseDevice(b)==EFI_DEVICE_ERROR&&!Gu.compat_device_open&&!Gu.compat_device_closed);
  CHECK(!GuCompatCompletedPlatform(&platforms[1])&&GuCompatCompletedPlatform(&platforms[0]));
  CHECK(pci_read_config_word(b,0,&value)==-EINVAL&&reads==2);
  CHECK(allocs==3);
  /* A clean reference count is not sufficient if transaction memory remains. */
  Gu.fault=0;Gu.compat_device_open=1;b->refs=1; /* Independent close case. */
  void *leaked=GuCompatAlloc(b,1,8);CHECK(leaked&&scratch_allocs==3);
  CHECK(GuCompatCloseDevice(b)==EFI_DEVICE_ERROR&&!Gu.compat_device_closed&&Gu.fault);
  CHECK(!GuCompatCompletedPlatform(&Gu)&&temporary[1]==leaked);
  free(leaked);free(a);free(b); /* End of simulated boot. */
  /* Independent reservation failures: no record publication on allocation
     owner, selected owner, boot-service, PCI, handle, or device identity drift. */
  for(int which=0;which<7;which++){
    memset(platforms,0,sizeof(platforms));memset(pool,0,sizeof(pool));
    memset(pool_bytes,0,sizeof(pool_bytes));active=&platforms[0];
    Gu.bs=&bs;Gu.pci=&pci[0];Gu.handle=&pci[0];Gu.device_id=0x2684;
    wrong_pool_owner=(which==0);change_active_after_alloc=(which==1);
    change_reservation_identity=which>=2&&which<=5?which-1:0;
    fault_after_alloc=(which==6);
    CHECK(GuCompatReserveDevice(&platforms[0])==(which==6?EFI_DEVICE_ERROR:EFI_ACCESS_DENIED));
    CHECK(!platforms[0].compat_device&&!platforms[1].compat_device);
    CHECK(platforms[0].fault&&platforms[0].retained&&!platforms[1].fault&&!platforms[1].retained);
    CHECK(active==&platforms[which==1?1:0]&&!wrong_allocator_argument);
    struct pci_dev blank={0};
    CHECK(pool[which==0?1:0]&&!memcmp(pool[which==0?1:0],&blank,sizeof(blank)));
    free((void*)pool[which==0?1:0]); /* Test fixture teardown only. */
  }
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
        self.assertLess(execution.index("GuMsiPreflight("),execution.index("GuCompatReserveDevice(owner)"))
        self.assertLess(execution.index("GuCompatReserveDevice(owner)"),execution.index("GuMsiRelease()"))
        self.assertLess(execution.index("GuAd104ColdEntryFor(owner)"),execution.index("GuCompatOpenDevice(owner,&dev)"))
        self.assertLess(execution.index("GuCompatOpenDevice(owner,&dev)"),execution.index("gpu_unlock_610_load_shared(dev,"))
        self.assertLess(execution.index("gpu_unlock_610_release_shared(dev,&shared)"),execution.index("GuCompatCloseDevice(dev)"))
        self.assertLess(execution.index("GuCompatCloseDevice(dev)"),execution.index("GuMsiFinish()"))
        # Vendor I/O must carry its own work record even for repeated BAR addresses.
        for call in ("GuCompatRead32", "GuCompatWrite32", "GuCompatCopyFromIo", "GuCompatDmaBarrier", "GuCompatTime", "GuCompatDelay"):
            self.assertNotRegex(vendor, rf"\b{call}\((?!work->pdev,|work->pdev\))")
            self.assertIn(call + "(work->pdev",vendor)
        self.assertNotRegex(vendor,r"\b(?:readl|writel|memcpy_fromio|dma_wmb|ktime_get|usleep_range|kzalloc|kvmalloc|kvcalloc|kfree|kvfree|__module_get)\(")
        for call in ("GuCompatAlloc", "GuCompatFree", "GuCompatRetain"):
            self.assertIn(call + "(",vendor)
            self.assertNotRegex(vendor, rf"\b{call}\((?!work->pdev,|pdev,|pdev\))")
        load=vendor.split("int gpu_unlock_610_load_shared(",1)[1].split("int gpu_unlock_610_release_shared(",1)[0]
        run=vendor.split("int gpu_unlock_610_unlock(",1)[1]
        self.assertLess(load.index("GuCompatOwner(pdev)"),load.index("pdev->device"))
        self.assertLess(run.index("GuCompatOwner(pdev)"),run.index("GuCompatPciMatches(pdev)"))
        self.assertLess(run.index("GuCompatPciMatches(pdev)"),run.index("owner->transaction_attempted"))
        self.assertNotIn("GuStarted();",vendor)
        self.assertNotIn("GuQuiesced();",vendor)
        self.assertNotIn("GuEntrySelectorsMatch(",vendor)
        self.assertIn("GuAd104ColdEntryFor(owner)",vendor)
        self.assertNotIn("GuAd104ColdEntry()",vendor)
        self.assertLess(run.index("GuCompatOwner(pdev)"),run.index("pdev->device"))
        self.assertIn("shared->owner!=pdev",run)
        self.assertIn("if(!result&&released)result=released;",execution)
        self.assertLess(run.index("GuCompatOwner(pdev)"),run.index("owner->display_check()"))
        self.assertLess(run.index("if (!work->pdev)"),run.index("unlock_transaction_assets(work)"))
        self.assertNotRegex(vendor,r"\bGu\.|\bGuAssets\b")


    def test_borrowed_asset_record_is_device_scoped(self):
        """Run the real CPU-only asset request and borrow lifecycle."""
        compat=(HEADER.parents[1]/"compat.c").read_text(encoding="utf-8")
        vendor=(HEADER.parents[1]/"vendor/unlock_610.c").read_text(encoding="utf-8")
        header=(HEADER.parents[1]/"vendor/unlock_610.h").read_text(encoding="utf-8")
        record=header[header.index("struct gpu_unlock_610_shared {"):header.index("/* Load the reference")]
        request=compat[compat.index("int request_firmware("):compat.index("int pci_read_config_word(")]
        scoped=vendor[vendor.index("static int request_scoped("):vendor.index("static int request_asset(")]
        borrow=vendor[vendor.index("int gpu_unlock_610_load_shared("):vendor.index("int gpu_unlock_610_unlock(")]
        host=r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define TRUE 1
#define GU_ASSET_COUNT 5
#define GFP_KERNEL 0
#define dev_info(...) ((void)0)
typedef size_t UINTN;
struct firmware {size_t size;const unsigned char *data;};
struct device {int scope;};
struct pci_dev {struct device dev;int device;};
typedef struct {const char *name;struct firmware firmware;} GU_ASSET;
typedef struct {int fault;GU_ASSET asset_entries[5];} GU_PLATFORM;
static GU_PLATFORM contexts[2],*active=&contexts[0];
static struct pci_dev cards[2],*selected=&cards[0];
#define Gu (*active)
#define GuAssets (Gu.asset_entries)
static int releases,opens,fail_release,invalid_assets;
static int GuPlatformAssetsOwned(const GU_PLATFORM *p){
  return (p==&contexts[0]||p==&contexts[1])&&!invalid_assets;
}
static int GuAd104AssetsValid(const GU_PLATFORM *p){return GuPlatformAssetsOwned(p);}
static GU_PLATFORM *GuCompatOwner(const struct pci_dev *p){
  if(p==&cards[0])return &contexts[0];
  if(p==&cards[1])return &contexts[1];
  return NULL;
}
static int GuCompatPciMatches(const struct pci_dev *p){
  return p==selected&&GuCompatOwner(p)==active;
}
static GU_PLATFORM *GuCompatDeviceOwner(const struct device *d){
  if(d==&cards[0].dev)return &contexts[0];
  if(d==&cards[1].dev)return &contexts[1];
  return NULL;
}
static int gpu_is_supported_device(int d){return d==1;}
static const struct asset_names {const char *gsp;} names={"gsp"};
static const struct asset_names *unlock_assets(const struct pci_dev *p){(void)p;return &names;}
static void *devres_open_group(struct device *d,void *id,int flags){
  (void)id;(void)flags;
  if(GuCompatDeviceOwner(d)!=active||d->scope)return NULL;
  opens++;d->scope=1;return &d->scope;
}
static int devres_release_group(struct device *d,void *g){
  if(g!=&d->scope||!d->scope)return -EINVAL;
  releases++;
  if(fail_release)return -EIO;
  d->scope=0;return 0;
}
""" + record + request + scoped + borrow + r"""
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
  unsigned char bytes[2]={1,2};
  for(int i=0;i<2;i++){
    cards[i].device=1;
    contexts[i].asset_entries[2]=(GU_ASSET){"gsp",{1,&bytes[i]}};
  }
  struct gpu_unlock_610_shared a={0},b={0};
  CHECK(gpu_unlock_610_load_shared(&cards[0],&a)==0);
  CHECK(a.owner==&cards[0]&&a.gsp==&contexts[0].asset_entries[2].firmware&&opens==1&&releases==1);
  CHECK(gpu_unlock_610_load_shared(&cards[0],&a)==-EALREADY&&opens==1);
  active=&contexts[1];selected=&cards[1];
  CHECK(gpu_unlock_610_load_shared(&cards[1],&a)==-EALREADY&&opens==1);
  CHECK(gpu_unlock_610_release_shared(&cards[1],&a)==-EINVAL&&a.owner==&cards[0]);
  CHECK(gpu_unlock_610_load_shared(&cards[1],&b)==0&&b.owner==&cards[1]&&b.gsp!=a.gsp);
  CHECK(gpu_unlock_610_release_shared(&cards[0],&a)==-EINVAL&&a.gsp);
  CHECK(gpu_unlock_610_release_shared(&cards[1],&b)==0&&!b.owner&&!b.gsp);
  CHECK(gpu_unlock_610_release_shared(&cards[1],&b)==0);
  invalid_assets=1;
  const struct firmware *asset=NULL;
  CHECK(request_firmware(&asset,"gsp",&cards[1].dev)==-EIO&&!asset&&Gu.fault);
  invalid_assets=0;Gu.fault=0; /* Independent host case. */
  fail_release=1;
  CHECK(gpu_unlock_610_load_shared(&cards[1],&b)==-EIO&&!b.gsp&&!b.owner);
  CHECK(opens==3&&releases==3&&cards[1].dev.scope);
  CHECK(a.owner==&cards[0]&&a.gsp==&contexts[0].asset_entries[2].firmware);
  active=&contexts[0];selected=&cards[0];
  CHECK(gpu_unlock_610_release_shared(&cards[0],&a)==0&&!a.owner&&!a.gsp);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src=Path(directory)/"borrow.c"
            src.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                exe=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(src),"-o",str(exe)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == "__main__":
    unittest.main()
