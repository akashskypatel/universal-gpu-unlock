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
    found = re.search(r"\b(?:EFI_STATUS|BOOLEAN)\s+" + name + r"\s*\([^;]*?\)\s*\{", SOURCE)
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
    def test_mutating_low_level_api_requires_platform_owner(self):
        header=(ROOT / "src/port.h").read_text(encoding="utf-8")
        for signature in (
            "GuDmaAllocate(GU_PLATFORM *owner,",
            "GuDmaFree(GU_PLATFORM *owner,",
            "GuWrite32(GU_PLATFORM *owner,",
            "GuConfigWrite(GU_PLATFORM *owner,",
            "GuSavePci(GU_PLATFORM *owner)",
            "GuRestorePci(GU_PLATFORM *owner)",
            "GuFlr(GU_PLATFORM *owner)",
            "GuClearMaster(GU_PLATFORM *owner)",
            "GuEnableMaster(GU_PLATFORM *owner)",
        ):
            self.assertIn(signature, header)
        self.assertIn("owner!=GuPlatformActive", SOURCE)

    def test_selected_card_owns_the_one_use_selector_write(self):
        first = SOURCE.index("VOID GuWrite32(")
        last = SOURCE.index("\nstatic EFI_STATUS RawReadFor(", first)
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
  GuWrite32(GuPlatformActive,0x823820U,8U);
  CHECK(!a.fault && a.ss1_write_consumed && io0.writes==1);
  GuWrite32(GuPlatformActive,0x823820U,8U);
  CHECK(a.fault && io0.writes==1);
  GuPlatformActive=&b;
  a.fault=FALSE;
  GuWrite32(&a,0x823820U,8U);
  CHECK(a.fault && !b.fault && io1.writes==0);
  GuWrite32(GuPlatformActive,0x823820U,8U);
  CHECK(b.fault && !b.ss1_write_consumed && io1.writes==0);
  b.fault=FALSE;b.selector_phase=GU_SELECTOR_PHASE_SS1_WRITE;
  io1.fail=1;
  GuWrite32(GuPlatformActive,0x823820U,8U);
  CHECK(b.fault && b.ss1_write_consumed && io1.writes==1);
  GuWrite32(GuPlatformActive,0x823820U,8U);
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
        first = SOURCE.index("BOOLEAN GuGetEntrySelectorsFor(")
        middle = SOURCE.index("#endif\n#ifdef GU_ENTRY_LATCHED_SS1_FULL", first)
        last = SOURCE.index("#endif\nEFI_STATUS GuInit(", middle)
        actual_latch = SOURCE[first:middle]
        actual_phase = SOURCE[SOURCE.index("BOOLEAN GuSelectorPhaseFor(", middle):last]
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
static UINT32 GuRead32For(GU_PLATFORM *platform,UINT32 offset) {
  return offset==0x82381cU?platform->mmio_ss0:platform->mmio_ss1;
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
  CHECK(GuSelectorPhaseFor(GuPlatformActive,GU_SELECTOR_PHASE_SS0_WRITTEN));
  CHECK(a.selector_phase==GU_SELECTOR_PHASE_SS0_WRITTEN);
  GuPlatformActive=&b;
  CHECK(b.selector_phase==GU_SELECTOR_PHASE_ENTRY);
  CHECK(!GuSelectorPhaseFor(GuPlatformActive,GU_SELECTOR_PHASE_TARGET) && b.fault);
  GuPlatformActive=&a;
  CHECK(!a.fault && a.selector_phase==GU_SELECTOR_PHASE_SS0_WRITTEN);
  CHECK(GuSelectorPhaseFor(GuPlatformActive,GU_SELECTOR_PHASE_SS1_WRITE));
  a.ss1_write_consumed=TRUE;a.mmio_ss1=8U;
  CHECK(GuSelectorPhaseFor(GuPlatformActive,GU_SELECTOR_PHASE_TARGET));
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
        start = SOURCE.index("typedef struct GU_PLATFORM_ALLOCATION {")
        end = SOURCE.index("\n/* Allocate CPU-only state", start)
        registry = SOURCE[start:end]
        original = registry + "\n" + function("GuAllocatePlatform") + "\n" + function("GuBindAllocatedPlatform") + "\n" + function("GuDiscardUnboundPlatform") + "\n" + function("GuInit")
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
  EFI_STATUS (*FreePool)(VOID *);
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
static GU_PLATFORM UnboundPlatform;
GU_PLATFORM *GuPlatformActive=&UnboundPlatform;
#define Gu (*GuPlatformActive)
static const GU_PLATFORM EmptyPlatform;
static unsigned allocations,fail_alloc,empty_alloc,frees,fail_free,free_calls,ambiguous_alloc;
static VOID *failed_release;
static EFI_STATUS Allocate(int kind,size_t bytes,VOID **out) {
  if(kind!=EfiLoaderData || !out)return EFI_INVALID_PARAMETER;
  allocations++;
  if(fail_alloc)return EFI_OUT_OF_RESOURCES;
  *out=empty_alloc?NULL:calloc(1,bytes);
  if(ambiguous_alloc&&*out)return EFI_OUT_OF_RESOURCES;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *ptr) {
  free_calls++;
  if(fail_free){failed_release=ptr;return EFI_ACCESS_DENIED;}
  free(ptr);frees++;return EFI_SUCCESS;
}
""" + original + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  int mode=argc>1?atoi(argv[1]):-1;
  EFI_BOOT_SERVICES bs={Allocate,Free};
  EFI_PCI_IO_PROTOCOL first={1},second={2};
  if(mode==0) {
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    CHECK(GuPlatformActive!=&UnboundPlatform && allocations==1);
    CHECK(Gu.bs==&bs && Gu.pci==&first);
    Gu.retained=1;Gu.allocations=(VOID *)(uintptr_t)0x1234U;
    GU_PLATFORM *saved=GuPlatformActive;
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
    CHECK(GuPlatformActive==saved && allocations==1 && Gu.retained);
  } else if(mode==1) {
    fail_alloc=1;
    CHECK(GuInit(&bs,&first)==EFI_OUT_OF_RESOURCES);
    CHECK(GuPlatformActive==&UnboundPlatform && !AnyPublishedPlatform());
    fail_alloc=0;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    CHECK(GuPlatformActive!=&UnboundPlatform && allocations==2);
  } else if(mode==2) {
    Gu.fault=1;
    CHECK(GuInit(&bs,&first)==EFI_ACCESS_DENIED);
    CHECK(allocations==0 && GuPlatformActive==&UnboundPlatform);
    Gu.fault=0;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
  } else if(mode==3) {
    empty_alloc=1;
    CHECK(GuInit(&bs,&first)==EFI_OUT_OF_RESOURCES);
    CHECK(!AnyPublishedPlatform() && GuPlatformActive==&UnboundPlatform);
    empty_alloc=0;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
  } else if(mode==4) {
    bs.AllocatePool=NULL;
    CHECK(GuInit(&bs,&first)==EFI_UNSUPPORTED);
    CHECK(!AnyPublishedPlatform() && allocations==0);
    bs.AllocatePool=Allocate;
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    memset(&Gu,0,sizeof(Gu));
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
  } else if(mode==5) {
    GU_PLATFORM *contexts[4]={0};
    EFI_PCI_IO_PROTOCOL cards[4]={{1},{2},{3},{4}};
    for(size_t i=0;i<4;i++) {
      CHECK(GuAllocatePlatform(&bs,&cards[i],&contexts[i])==EFI_SUCCESS);
      CHECK(contexts[i]->bs==&bs && contexts[i]->pci==&cards[i]);
      CHECK(GuPlatformActive==&UnboundPlatform && !AnyPublishedPlatform());
      for(size_t j=0;j<i;j++)CHECK(contexts[j]!=contexts[i]);
      contexts[i]->fault=(int)i;
    }
    CHECK(GuAllocatePlatform(&bs,&first,&contexts[0])==EFI_ALREADY_STARTED);
    CHECK(allocations==4 && contexts[0]->pci==&cards[0]);
    GU_PLATFORM *failed=NULL;
    fail_alloc=1;
    CHECK(GuAllocatePlatform(&bs,&first,&failed)==EFI_OUT_OF_RESOURCES && !failed);
    fail_alloc=0;
    CHECK(GuAllocatePlatform(&bs,&first,NULL)==EFI_INVALID_PARAMETER);
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    CHECK(GuPlatformActive!=contexts[0] && Gu.pci==&first);
    for(size_t i=0;i<4;i++) {
      CHECK(contexts[i]->fault==(int)i && contexts[i]->pci==&cards[i]);
    }
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
  } else if(mode==6) {
    GU_PLATFORM *a=NULL,*b=NULL,*active=NULL;
    CHECK(GuAllocatePlatform(&bs,&first,&a)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&second,&b)==EFI_SUCCESS);
    CHECK(a!=b && a->pci==&first && b->pci==&second);
    CHECK(GuDiscardUnboundPlatform(NULL)==EFI_INVALID_PARAMETER);
    b->fault=1;
    CHECK(GuDiscardUnboundPlatform(&b)==EFI_ACCESS_DENIED && b && !frees);
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_SUCCESS && !a && frees==1);
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_INVALID_PARAMETER);
    b->fault=0;
    GU_PLATFORM *saved=b;
    bs.FreePool=NULL;
    CHECK(GuDiscardUnboundPlatform(&b)==EFI_UNSUPPORTED && b==saved);
    bs.FreePool=Free;
    CHECK(GuDiscardUnboundPlatform(&b)==EFI_SUCCESS && !b && frees==2);
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    active=GuPlatformActive;
    CHECK(GuDiscardUnboundPlatform(&active)==EFI_ACCESS_DENIED &&
          active==GuPlatformActive && frees==2);
    active=&UnboundPlatform;
    CHECK(GuDiscardUnboundPlatform(&active)==EFI_ACCESS_DENIED && frees==2);
  } else if(mode==7) {
    EFI_BOOT_SERVICES other={Allocate,Free};
    GU_PLATFORM *a=NULL,*b=NULL,*duplicate=NULL;
    CHECK(GuAllocatePlatform(&bs,&first,&a)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&second,&b)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&first,&duplicate)==EFI_ALREADY_STARTED);
    CHECK(!duplicate && allocations==2);
    CHECK(GuInit(&bs,&first)==EFI_ALREADY_STARTED && !AnyPublishedPlatform());
    CHECK(GuPlatformActive==&UnboundPlatform);
    /* The original PCI and allocator identities remain authoritative. */
    a->pci=&second;
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_ACCESS_DENIED && a && !frees);
    CHECK(GuAllocatePlatform(&bs,&first,&duplicate)==EFI_ALREADY_STARTED && !duplicate);
    a->pci=&first;a->bs=&other;
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_ACCESS_DENIED && a && !frees);
    a->bs=&bs;
    GU_PLATFORM foreign={0};
    foreign.bs=&bs;foreign.pci=&first;
    GU_PLATFORM *unknown=&foreign;
    CHECK(GuDiscardUnboundPlatform(&unknown)==EFI_ACCESS_DENIED && unknown==&foreign);
    /* Remove an interior node, then reuse only its original PCI identity. */
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_SUCCESS && !a);
    CHECK(GuAllocatePlatform(&bs,&first,&a)==EFI_SUCCESS && a);
    CHECK(GuAllocatePlatform(&bs,&second,&duplicate)==EFI_ALREADY_STARTED && !duplicate);
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_SUCCESS && !a);
    CHECK(GuDiscardUnboundPlatform(&b)==EFI_SUCCESS && !b);
    CHECK(!PlatformAllocations && frees==3);
    CHECK(GuInit(&bs,&first)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&first,&duplicate)==EFI_ALREADY_STARTED && !duplicate);
  } else if(mode==8) {
    EFI_BOOT_SERVICES other={Allocate,Free};
    GU_PLATFORM *a=NULL,*b=NULL,*unknown=NULL;
    CHECK(GuAllocatePlatform(&bs,&first,&a)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&second,&b)==EFI_SUCCESS);
    GU_PLATFORM *saved=a;
    CHECK(GuBindAllocatedPlatform(NULL,&first,&a)==EFI_INVALID_PARAMETER);
    CHECK(GuBindAllocatedPlatform(&bs,&first,NULL)==EFI_INVALID_PARAMETER);
    CHECK(GuBindAllocatedPlatform(&bs,&first,&unknown)==EFI_INVALID_PARAMETER);
    CHECK(GuBindAllocatedPlatform(&bs,&second,&a)==EFI_ACCESS_DENIED);
    CHECK(GuBindAllocatedPlatform(&other,&first,&a)==EFI_ACCESS_DENIED);
    a->fault=1;
    CHECK(GuBindAllocatedPlatform(&bs,&first,&a)==EFI_ACCESS_DENIED);
    a->fault=0;
    a->pci=&second;
    CHECK(GuBindAllocatedPlatform(&bs,&first,&a)==EFI_ACCESS_DENIED);
    a->pci=&first;
    GU_PLATFORM foreign={0};
    foreign.bs=&bs;foreign.pci=&first;
    unknown=&foreign;
    CHECK(GuBindAllocatedPlatform(&bs,&first,&unknown)==EFI_ACCESS_DENIED && unknown==&foreign);
    Gu.fault=1;
    CHECK(GuBindAllocatedPlatform(&bs,&first,&a)==EFI_ACCESS_DENIED);
    Gu.fault=0;
    CHECK(a==saved && !AnyPublishedPlatform() && GuPlatformActive==&UnboundPlatform);
    b->fault=1;
    CHECK(GuBindAllocatedPlatform(&bs,&first,&a)==EFI_SUCCESS);
    CHECK(!a && GuPlatformActive==saved && AnyPublishedPlatform() && allocations==2);
    CHECK(b->fault && b->pci==&second);
    Gu.retained=1;
    CHECK(GuBindAllocatedPlatform(&bs,&second,&b)==EFI_ALREADY_STARTED);
    CHECK(b && b->fault && GuPlatformActive==saved && Gu.retained);
    CHECK(GuDiscardUnboundPlatform(&saved)==EFI_ACCESS_DENIED && saved==GuPlatformActive);
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
  } else if(mode==9) {
    GU_PLATFORM *first_owner=NULL,*second_owner=NULL;
    CHECK(GuAllocatePlatform(&bs,&first,&first_owner)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&second,&second_owner)==EFI_SUCCESS);
    GU_PLATFORM *published=first_owner;
    CHECK(GuBindAllocatedPlatform(&bs,&first,&first_owner)==EFI_SUCCESS && !first_owner);
    CHECK(AnyPublishedPlatform() && GuPlatformActive==published);
    /* Simulate accidental loss of the active reference while the old
     * platform still looks pristine. No production code performs this. */
    GuPlatformActive=&UnboundPlatform;
    CHECK(AnyPublishedPlatform());
    CHECK(GuDiscardUnboundPlatform(&published)==EFI_ACCESS_DENIED && published && !frees);
    CHECK(GuBindAllocatedPlatform(&bs,&second,&second_owner)==EFI_ALREADY_STARTED);
    CHECK(second_owner && GuPlatformActive==&UnboundPlatform);
    CHECK(GuInit(&bs,&second)==EFI_ALREADY_STARTED);
    CHECK(GuDiscardUnboundPlatform(&second_owner)==EFI_SUCCESS && !second_owner);
    CHECK(AnyPublishedPlatform() && frees==1);
  } else if(mode==10) {
    GU_PLATFORM *a=NULL,*b=NULL;
    CHECK(GuAllocatePlatform(&bs,&first,&a)==EFI_SUCCESS);
    ambiguous_alloc=1;
    CHECK(GuAllocatePlatform(&bs,&second,&b)==EFI_OUT_OF_RESOURCES&&!b);
    CHECK(PlatformRegistryStatus==EFI_OUT_OF_RESOURCES&&PlatformRegistryFailureAddress);
    CHECK(PlatformAllocations&&&PlatformAllocations->platform==a&&!PlatformAllocations->next);
    CHECK(GuBindAllocatedPlatform(&bs,&first,&a)==EFI_OUT_OF_RESOURCES&&a);
    CHECK(GuAllocatePlatform(&bs,&second,&b)==EFI_OUT_OF_RESOURCES&&!b&&allocations==2);
    CHECK(GuPlatformActive==&UnboundPlatform&&!AnyPublishedPlatform());
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_SUCCESS&&!a&&free_calls==1);
    CHECK(!PlatformAllocations);
    free(PlatformRegistryFailureAddress); /* Host fixture teardown. */
  } else if(mode==11) {
    GU_PLATFORM *a=NULL,*b=NULL,*extra=NULL;
    CHECK(GuAllocatePlatform(&bs,&first,&a)==EFI_SUCCESS);
    CHECK(GuAllocatePlatform(&bs,&second,&b)==EFI_SUCCESS);
    GU_PLATFORM *alias=a;
    fail_free=1;
    CHECK(GuDiscardUnboundPlatform(&a)==EFI_ACCESS_DENIED&&!a&&free_calls==1);
    CHECK(PlatformRegistryStatus==EFI_ACCESS_DENIED&&PlatformRegistryFailureAddress==failed_release);
    CHECK(PlatformAllocations&&&PlatformAllocations->platform==b&&!PlatformAllocations->next);
    /* The firmware could have consumed the failed allocation. Never retry or scan it. */
    CHECK(GuDiscardUnboundPlatform(&alias)==EFI_ACCESS_DENIED&&free_calls==1);
    CHECK(GuAllocatePlatform(&bs,&first,&extra)==EFI_ACCESS_DENIED&&!extra&&allocations==2);
    CHECK(GuBindAllocatedPlatform(&bs,&second,&b)==EFI_ACCESS_DENIED&&b);
    CHECK(GuPlatformActive==&UnboundPlatform&&!AnyPublishedPlatform());
    fail_free=0;
    CHECK(GuDiscardUnboundPlatform(&b)==EFI_SUCCESS&&!b&&free_calls==2);
    CHECK(!PlatformAllocations);
    free(failed_release); /* Host fixture teardown, not a production retry. */
  } else return 3;
  /* Test process termination only. Do not use as a production unbind. */
  while(PlatformAllocations) {
    GU_PLATFORM_ALLOCATION *next=PlatformAllocations->next;
    free(PlatformAllocations);PlatformAllocations=next;
  }
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
            for mode in range(12):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), str(mode)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

    def test_registry_advances_only_after_verified_completed_context(self):
        start = SOURCE.index("typedef struct GU_PLATFORM_ALLOCATION {")
        end = SOURCE.index("\n/* Allocate CPU-only state", start)
        registry = SOURCE[start:end]
        actual = registry + "\n" + "\n".join(
            function(name) for name in ("GuAllocatePlatform", "GuBindAllocatedPlatform",
            "GuVerifyCompletedPlatforms", "GuAnyPlatformNeedsContainment",
            "GuAdvanceAllocatedPlatform", "GuDiscardUnboundPlatform"))
        host = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_execution_state.h"
typedef void VOID;
typedef int BOOLEAN;
typedef uintptr_t EFI_STATUS;
typedef uint16_t UINT16;
typedef struct {int a;} EFI_PCI_IO_PROTOCOL;
typedef struct EFI_BOOT_SERVICES {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  VOID *handle,*image,*allocations,*release_state;
  unsigned char asset_entries[5];
  GU_GPU_RECORD discovery_identity;
  uint16_t device_id;
  int profile_verified,prom_verified,release_state_reserved,release_owner_invalid;
  int asset_entries_reserved,asset_load_attempted,fault,retained,owned,quiesced;
  unsigned dma_count,master_attributes_pending,flrs;
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
#define EFI_NOT_READY 7
static GU_PLATFORM UnboundPlatform;
static GU_PLATFORM *GuPlatformActive=&UnboundPlatform;
#define Gu (*GuPlatformActive)
static const GU_PLATFORM EmptyPlatform;
static unsigned ready_calls;
static int ready=1;
static const GU_PLATFORM *invalid_proof,*invalid_compat,*invalid_assets;
static int GuPlatformAssetsOwned(const GU_PLATFORM *platform){return platform!=invalid_assets;}
static int GuCompatCompletedPlatform(const GU_PLATFORM *platform){return platform!=invalid_compat;}
static int GuMsiCompletedPlatform(const GU_PLATFORM *platform,const VOID *token){
  return platform!=invalid_proof && platform->release_state==token;
}
static EFI_STATUS Allocate(int kind,size_t bytes,VOID **out) {
  if(kind!=EfiLoaderData||!out)return EFI_INVALID_PARAMETER;
  *out=calloc(1,bytes);return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *ptr){free(ptr);return EFI_SUCCESS;}
static int GuMsiReadyForWindows(void){ready_calls++;return ready;}
static const VOID *GuMsiPlanOwnerToken(void){return Gu.release_state;}
static int GuSafeToReturn(void){return !Gu.fault&&!Gu.retained&&!Gu.allocations&&
    !Gu.dma_count&&!Gu.master_attributes_pending;}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Allocate,Free};
  EFI_PCI_IO_PROTOCOL pci[4]={{1},{2},{3},{4}};
  int handles[4]={0},release[4]={0},image=1;
  GU_PLATFORM *owner[4]={0},*saved[4]={0};
  GU_GPU_EXEC_OBSERVATION observed[4]={0};
  GU_GPU_EXEC_TARGET records[4];
  size_t order[4],requested[3]={1,0,2};
  GU_GPU_EXEC_SEQUENCE sequence;
  for(size_t i=0;i<4;i++) {
    CHECK(GuAllocatePlatform(&bs,&pci[i],&owner[i])==EFI_SUCCESS);
    saved[i]=owner[i];
    observed[i].identity=(GU_GPU_RECORD){.pci_id=i==0?0x268410deU:0x220a10deU,
       .subsystem_id=0x123410deU,.class_code=0x0300,.bus=(uint32_t)(i+1),
       .handle=&handles[i],.pci=&pci[i]};
    observed[i].config[0]=observed[i].identity.pci_id;
    observed[i].config[2]=0x03000000U;
    observed[i].config[11]=0x123410deU;
  }
  CHECK(GuGpuExecInit(&sequence,observed,4,requested,3,records,order,4)==GU_GPU_EXEC_OK);
  CHECK(GuBindAllocatedPlatform(&bs,&pci[1],&owner[1])==EFI_SUCCESS);
  CHECK(!owner[1]&&GuPlatformActive==saved[1]);
  CHECK(!GuAnyPlatformNeedsContainment());
  for(size_t step=0;step<3;step++) {
    CHECK(GuAnyPlatformNeedsContainment()==(step!=0));
    size_t i=requested[step];
    CHECK(GuPlatformActive==saved[i]&&Gu.pci==&pci[i]);
    Gu.handle=&handles[i];Gu.image=&image;
    Gu.discovery_identity=observed[i].identity;
    Gu.device_id=(uint16_t)(observed[i].identity.pci_id>>16);
    Gu.profile_verified=Gu.prom_verified=Gu.release_state_reserved=
      Gu.asset_entries_reserved=Gu.asset_load_attempted=1;
    Gu.release_state=&release[i];
    CHECK(GuGpuExecBeginOwned(&sequence,step,observed,4,&Gu,Gu.release_state,
                             Gu.asset_entries)==GU_GPU_EXEC_OK);
    if(step<2)
      CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[requested[step+1]],&sequence,
        observed,4)==EFI_NOT_READY);
    Gu.quiesced=1;Gu.flrs=1;
    observed[i].config[4]=(uint32_t)(0x2000U+step);
    CHECK(GuGpuExecFinish(&sequence,step,observed,4,1)==GU_GPU_EXEC_OK);
    if(step==2)break;
    size_t next=requested[step+1];
    GU_PLATFORM prior=Gu;
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[3],&sequence,observed,4)==EFI_ACCESS_DENIED);
    CHECK(GuPlatformActive==saved[i]&&owner[3]);
    Gu.retained=1;
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
    Gu.retained=0;
    Gu.release_state=&release[3];
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
    Gu.release_state=&release[i];
    observed[3].config[4]^=1;
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_NOT_READY);
    observed[3].config[4]^=1;
    if(step==1) {
      invalid_proof=saved[1];
      CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
      invalid_proof=NULL;
      invalid_compat=saved[1];
      CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
      invalid_compat=NULL;
      invalid_assets=saved[1];
      CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
      invalid_assets=NULL;
      saved[1]->discovery_identity.bus^=1;
      CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
      saved[1]->discovery_identity.bus^=1;
      saved[1]->fault=1;
      CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
      saved[1]->fault=0;
    }
    PlatformRegistryStatus=EFI_ACCESS_DENIED; /* Independent negative observation. */
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
    CHECK(GuPlatformActive==saved[i]&&owner[next]);
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    PlatformRegistryStatus=EFI_SUCCESS; /* Test only, never reset in production. */
    owner[next]->fault=1;
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_ACCESS_DENIED);
    owner[next]->fault=0;
    ready=0;
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_NOT_READY);
    ready=1; /* cases model separate observations, not a hardware recovery */
    CHECK(GuPlatformActive==saved[i]&&owner[next]&&memcmp(&Gu,&prior,sizeof(prior))==0);
    CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[next],&sequence,observed,4)==EFI_SUCCESS);
    CHECK(!owner[next]&&GuPlatformActive==saved[next]&&memcmp(saved[i],&prior,sizeof(prior))==0);
    CHECK(GuDiscardUnboundPlatform(&saved[i])==EFI_ACCESS_DENIED&&saved[i]);
    CHECK(GuBindAllocatedPlatform(&bs,&pci[3],&owner[3])==EFI_ALREADY_STARTED);
  }
  CHECK(GuGpuExecCheckComplete(&sequence,observed,4)==GU_GPU_EXEC_OK);
  CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_SUCCESS);
  invalid_proof=saved[1];
  CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
  CHECK(GuPlatformActive==saved[2]);
  invalid_proof=NULL;
  for(size_t i=0;i<3;i++){
    invalid_compat=saved[requested[i]];
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    CHECK(GuPlatformActive==saved[2]);
  }
  invalid_compat=NULL;
  for(size_t i=0;i<3;i++){
    GU_PLATFORM *p=saved[requested[i]];
    GU_GPU_RECORD identity=p->discovery_identity;
    invalid_assets=p;
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    invalid_assets=NULL;
    p->asset_load_attempted=0;
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    p->asset_load_attempted=1;
    p->discovery_identity.subsystem_id^=1;
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    p->discovery_identity=identity;
    p->discovery_identity.pci=saved[3]->pci;
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    p->discovery_identity=identity;
    p->device_id^=1;
    CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
    p->device_id^=1;
    CHECK(GuPlatformActive==saved[2]);
  }
  saved[0]->fault=1;
  CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
  saved[0]->fault=0;
  CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_SUCCESS);
  CHECK(GuAnyPlatformNeedsContainment());
  GuPlatformActive=&UnboundPlatform;
  CHECK(GuAnyPlatformNeedsContainment());
  CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
  GuPlatformActive=saved[2];
  CHECK(GuAdvanceAllocatedPlatform(&bs,&owner[3],&sequence,observed,4)==EFI_NOT_READY);
  CHECK(ready_calls==4);
  CHECK(saved[1]->release_state==&release[1]&&saved[0]->release_state==&release[0]);
  CHECK(GuDiscardUnboundPlatform(&owner[3])==EFI_SUCCESS);
  PlatformRegistryStatus=EFI_ACCESS_DENIED;
  CHECK(GuVerifyCompletedPlatforms(&bs,&sequence,observed,4)==EFI_ACCESS_DENIED);
  CHECK(GuPlatformActive==saved[2]);
  /* Test process termination: production retains published allocations. */
  while(PlatformAllocations){
    GU_PLATFORM_ALLOCATION *next=PlatformAllocations->next;
    free(PlatformAllocations);PlatformAllocations=next;
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sequence_binding.c"
            executable = Path(directory) / "sequence_binding"
            source.write_text(host, encoding="utf-8")
            result = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "src"),
                 str(source), str(ROOT / "src/gpu_execution_state.c"), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            checked = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_original_platform_rebind_guard(self):
        body = function("GuInit")
        self.assertIn("BOOLEAN published,retired;", SOURCE)
        self.assertNotIn("static BOOLEAN TargetBound;", SOURCE)
        self.assertIn("static const GU_PLATFORM EmptyPlatform;", SOURCE)
        binding = function("GuBindAllocatedPlatform")
        self.assertIn("entry->published=TRUE;", binding)
        self.assertLess(binding.index("entry->published=TRUE;"), binding.index("GuPlatformActive=&entry->platform;"))
        self.assertLess(body.index("GuAllocatePlatform("), body.index("GuBindAllocatedPlatform("))
        self.assertIn("if(AnyPublishedPlatform())return EFI_ALREADY_STARTED;", body)
        self.assertIn("if(AnyPublishedPlatform())return EFI_ALREADY_STARTED;", binding)
        start = SOURCE.index("typedef struct GU_PLATFORM_ALLOCATION {")
        end = SOURCE.index("\n/* Allocate CPU-only state", start)
        registry = SOURCE[start:end]
        body = registry + "\n" + function("GuAllocatePlatform") + "\n" + binding + "\n" + function("GuDiscardUnboundPlatform") + "\n" + body
        host = r"""
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef void VOID;
typedef int BOOLEAN;
typedef uintptr_t EFI_STATUS;
typedef struct { EFI_STATUS (*AllocatePool)(int,size_t,VOID **); EFI_STATUS (*FreePool)(VOID *); } EFI_BOOT_SERVICES;
typedef struct { int a; } EFI_PCI_IO_PROTOCOL;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  int owned, started, retained, fault, saved;
  void *allocations;
  uint32_t flrs, dma_count;
} GU_PLATFORM;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_ACCESS_DENIED 3
#define EFI_ALREADY_STARTED 4
#define EFI_UNSUPPORTED 5
#define EFI_OUT_OF_RESOURCES 6
#define EfiLoaderData 7
static GU_PLATFORM UnboundPlatform;
static GU_PLATFORM *GuPlatformActive=&UnboundPlatform;
#define Gu (*GuPlatformActive)
static EFI_STATUS Allocate(int kind,size_t bytes,VOID **out) {
  if(kind!=EfiLoaderData || !out)return EFI_INVALID_PARAMETER;
  *out=calloc(1,bytes);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *p) {free(p);return EFI_SUCCESS;}
static const GU_PLATFORM EmptyPlatform;
""" + body + r"""
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s\\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES boot={Allocate,Free};
  EFI_PCI_IO_PROTOCOL first={1}, second={2};
  int scenario=argc>1?argv[1][0]-'0':-1;
  if(scenario==0) {
    CHECK(GuInit(NULL,&first)==EFI_INVALID_PARAMETER);
    CHECK(GuInit(&boot,NULL)==EFI_INVALID_PARAMETER);
    CHECK(GuInit(&boot,&first)==EFI_SUCCESS);
    CHECK(Gu.pci==&first && Gu.bs==&boot);
    CHECK(GuPlatformActive!=&UnboundPlatform);
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
  /* Test process termination only. Do not use as a production unbind. */
  while(PlatformAllocations) {
    GU_PLATFORM_ALLOCATION *next=PlatformAllocations->next;
    free(PlatformAllocations);PlatformAllocations=next;
  }
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
        right = SOURCE.index("BOOLEAN GuSafeToReturnFor(", left)
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
        start = MSI_SOURCE.index("typedef struct GU_MSI_OWNED_STATE {")
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
typedef void *EFI_HANDLE;
typedef struct {int tag;} EFI_PCI_IO_PROTOCOL;
#define TRUE 1
#define FALSE 0
typedef struct EFI_BOOT_SERVICES {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *bs;EFI_HANDLE handle,image;
  EFI_PCI_IO_PROTOCOL *pci;
  int profile_verified,prom_verified,started,owned,armed,fault,retained;
  struct GU_MSI_OWNED_STATE *release_state;
  BOOLEAN release_state_reserved,release_owner_invalid;
  EFI_STATUS cpu_pool_failure_status;
} GU_PLATFORM;
static GU_PLATFORM Gu;
typedef struct {int dummy;} SURVIVOR_GOP;
typedef struct {VOID *table;int mutated,claimed,ready;
  SURVIVOR_GOP *survivors;size_t survivor_count,survivor_capacity;
  int survivor_baseline_valid;} PLAN;
#define GRAPH_EDGE_LIMIT 16384
typedef struct {size_t count,capacity;int *edges;} GRAPH;
#define EfiLoaderData 1
#define EFI_SUCCESS 0
#define EFI_NOT_READY 2
#define EFI_ALREADY_STARTED 3
#define EFI_OUT_OF_RESOURCES 4
static int fail_pool,allocations;
static EFI_STATUS Allocate(int type,size_t n,VOID **out) {
  if(type!=EfiLoaderData||!out)return EFI_NOT_READY;
  allocations++;
  if(fail_pool)return EFI_OUT_OF_RESOURCES;
  *out=calloc(1,n);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static void *pool_record;
static size_t pool_bytes;
static void *GuAlloc(size_t n){
  if(Gu.bs->AllocatePool(EfiLoaderData,n,&pool_record)!=EFI_SUCCESS)return NULL;
  pool_bytes=n;return pool_record;
}
static int GuCpuPoolOwns(const GU_PLATFORM *p,const void *address,size_t n){
  return p==&Gu&&address&&address==pool_record&&n==pool_bytes&&!Gu.cpu_pool_failure_status;
}
static EFI_STATUS Free(VOID *p){free(p);return EFI_SUCCESS;}
""" + production + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES bs={Allocate,Free};
  EFI_PCI_IO_PROTOCOL pci={0};
  Gu.bs=&bs;Gu.handle=(VOID *)(uintptr_t)1;Gu.image=&bs;Gu.pci=&pci;
  Gu.profile_verified=Gu.prom_verified=1;
  int mode=argc>1?argv[1][0]-'0':-1;
  if(mode==0) {
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS);
    CHECK(CurrentMsi && Gu.release_state_reserved && ReleaseOwnerMatches());
    CHECK(&Original!=&Current && !Plan.table && !Original.edges && !Current.edges);
    CHECK(!Original.count && !Original.capacity && !Current.count && !Current.capacity);
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_ALREADY_STARTED && allocations==1);
  } else if(mode==1) {
    Gu.profile_verified=0;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
    CHECK(!CurrentMsi && !Gu.release_state_reserved && allocations==0);
  } else if(mode==2) {
    fail_pool=1;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_OUT_OF_RESOURCES);
    CHECK(!CurrentMsi && !Gu.release_state_reserved);
    fail_pool=0;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS && allocations==2);
  } else if(mode==3) {
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_SUCCESS);
    Plan.table=(VOID *)(uintptr_t)1;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_ALREADY_STARTED && allocations==1);
    CHECK(Plan.table==(VOID *)(uintptr_t)1);
  } else if(mode==4) {
    bs.AllocatePool=NULL;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY);
    CHECK(!CurrentMsi && !Gu.release_state_reserved);
  } else if(mode==5) {
    Gu.retained=1;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY && allocations==0);
  } else if(mode==6) {
    Gu.release_state_reserved=1;
    Gu.release_state=(void *)(uintptr_t)1;
    CHECK(!ReleaseOwnerMatches()&&Gu.release_owner_invalid&&Gu.fault&&Gu.retained);
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY&&!allocations);
  } else if(mode==7) {
    bs.FreePool=NULL;
    CHECK(GuMsiReserveSelectedState(&bs)==EFI_NOT_READY&&!allocations);
  } else return 3;
  free(pool_record);
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
            for mode in range(8):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), str(mode)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

    def test_scratch_pool_rejects_null_service_and_null_success(self):
        """Compile the real CPU pool allocation and release paths."""
        start = SOURCE.index("typedef struct GU_CPU_POOL {")
        end = SOURCE.index("\nVOID GuStartedFor(", start)
        actual = SOURCE[start:end]
        host = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void VOID;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_OUT_OF_RESOURCES 2
#define EFI_ACCESS_DENIED 3
#define EFI_DEVICE_ERROR 4
#define EfiLoaderData 1
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef struct {
  EFI_STATUS (*AllocatePool)(int,size_t,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct {
  EFI_BOOT_SERVICES *bs;
  struct GU_CPU_POOL *cpu_pool;
  VOID *compat_device,*release_state;
  int compat_device_open,compat_device_closed;
  VOID *cpu_pool_failure_address;
  EFI_STATUS cpu_pool_failure_status;
  int fault,retained;
} GU_PLATFORM;
typedef struct GU_PLATFORM_ALLOCATION {
  GU_PLATFORM platform;
  int published,retired;
  struct GU_PLATFORM_ALLOCATION *next;
} GU_PLATFORM_ALLOCATION;
static GU_PLATFORM_ALLOCATION *PlatformAllocations;
static EFI_STATUS PlatformRegistryStatus;
static GU_PLATFORM platforms[2],*active=&platforms[0];
#define Gu (*active)
#define GuPlatformActive active
static EFI_BOOT_SERVICES foreign;
static int mode,calls,frees,free_error;
static void *allocated,*released;
static EFI_STATUS Allocate(int kind,size_t n,VOID **out) {
  if(kind!=EfiLoaderData || !n)return EFI_OUT_OF_RESOURCES;
  calls++;
  if(mode==3)return EFI_OUT_OF_RESOURCES;
  if(mode==2)return EFI_SUCCESS; /* Misbehaving successful null output. */
  *out=allocated=malloc(n);
  if(mode==17||mode==18)active=&platforms[1];
  if(mode==21)platforms[0].bs=&foreign;
  if(mode==8||mode==18)return EFI_OUT_OF_RESOURCES; /* Error with a nonnull output. */
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *p){
  frees++;released=p;
  if(mode==19||mode==20)active=&platforms[1];
  if(mode==22)platforms[0].bs=&foreign;
  if(free_error)return EFI_DEVICE_ERROR;
  free(p);return EFI_SUCCESS;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d mode %d: %s\n",__LINE__,mode,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES bs={Allocate,Free};
  mode=argc>1?atoi(argv[1]):-1;
  if(mode<0||mode>24)return 3;
  foreign=bs;platforms[1].bs=&bs;
  if(mode)Gu.bs=&bs;
  if(mode==1)bs.AllocatePool=NULL;
  if(mode==9)bs.FreePool=NULL;
  if(mode==0||mode==1||mode==2||mode==3||mode==9){
    CHECK(GuAlloc(32)==NULL);
    CHECK(calls==(mode==2||mode==3));
    CHECK(!Gu.cpu_pool&&!Gu.fault&&!Gu.retained);
  }else if(mode==4){
    VOID *ptr=GuAlloc(32);
    CHECK(ptr&&calls==1&&Gu.cpu_pool==allocated&&Gu.cpu_pool->owner==&Gu);
    CHECK(Gu.cpu_pool->bytes==32&&ptr==(VOID *)(Gu.cpu_pool+1));
    CHECK(GuCpuPoolOwns(&Gu,ptr,32)&&!GuCpuPoolOwns(&Gu,ptr,31)&&!GuCpuPoolOwns(&platforms[1],ptr,32));
    for(size_t i=0;i<32;i++)CHECK(((unsigned char *)ptr)[i]==0);
    VOID *another=GuAlloc(7);
    CHECK(another&&Gu.cpu_pool->next==(GU_CPU_POOL *)((unsigned char *)ptr-sizeof(GU_CPU_POOL)));
    void *head=Gu.cpu_pool;
    void *first=Gu.cpu_pool->next;
    GuFree(ptr);CHECK(frees==1&&released==first&&Gu.cpu_pool==head&&!Gu.cpu_pool->next);
    GuFree(another);CHECK(frees==2&&released==head&&!Gu.cpu_pool&&!Gu.fault);
    GuFree(NULL);CHECK(frees==2);
  }else if(mode==5){
    VOID *a=GuAlloc(32);CHECK(a);
    GU_CPU_POOL *a_base=Gu.cpu_pool;
    active=&platforms[1];Gu.bs=&bs;
    VOID *b=GuAlloc(16);CHECK(b);
    GU_CPU_POOL *b_base=Gu.cpu_pool;
    GuFree(a);
    CHECK(!frees&&Gu.fault&&Gu.retained&&Gu.cpu_pool_failure_status==EFI_ACCESS_DENIED);
    CHECK(Gu.cpu_pool==b_base&&Gu.cpu_pool_failure_address==a);
    CHECK(platforms[0].cpu_pool==a_base&&!platforms[0].fault&&!platforms[0].retained);
    GuFree(b);CHECK(!frees);
    active=&platforms[0];GuFree(a);
    CHECK(frees==1&&released==a_base&&!Gu.cpu_pool&&!Gu.fault);
    free(b_base); /* Host fixture teardown, not a production release retry. */
  }else if(mode==6){
    VOID *a=GuAlloc(32);CHECK(a);
    GU_CPU_POOL *base=Gu.cpu_pool;
    VOID *b=GuAlloc(16);CHECK(b);
    GU_CPU_POOL *remaining=Gu.cpu_pool;
    free_error=1;GuFree(a);
    CHECK(frees==1&&released==base&&Gu.cpu_pool_failure_address==base);
    CHECK(Gu.cpu_pool==remaining&&!Gu.cpu_pool->next);
    CHECK(Gu.cpu_pool_failure_status==EFI_DEVICE_ERROR&&Gu.fault&&Gu.retained);
    free_error=0;GuFree(a);GuFree(b);
    CHECK(frees==1&&GuAlloc(4)==NULL&&calls==2);
    free(base);free(remaining); /* End of simulated boot only. */
  }else if(mode==7){
    EFI_BOOT_SERVICES foreign={Allocate,Free};
    VOID *p=GuAlloc(16);CHECK(p);
    GU_CPU_POOL *base=Gu.cpu_pool;
    Gu.bs=&foreign;GuFree(p);
    CHECK(!frees&&Gu.cpu_pool==base&&Gu.cpu_pool_failure_status==EFI_ACCESS_DENIED);
    CHECK(Gu.cpu_pool_failure_address==p&&Gu.retained&&Gu.fault);
    free(base);
  }else if(mode==8){
    CHECK(GuAlloc(32)==NULL&&allocated&&calls==1);
    CHECK(!Gu.cpu_pool&&Gu.cpu_pool_failure_address==allocated&&Gu.retained&&Gu.fault);
    CHECK(Gu.cpu_pool_failure_status==EFI_OUT_OF_RESOURCES&&!frees);
    CHECK(GuAlloc(8)==NULL&&calls==1);
    free(allocated);
  }else if(mode==10){
    CHECK(GuAlloc((size_t)-1)==NULL&&!calls);
    VOID *p=GuAlloc(1);CHECK(p);
    void *base=Gu.cpu_pool;
    GuFree(p);CHECK(frees==1&&released==base);
    GuFree(p);CHECK(frees==1&&Gu.cpu_pool_failure_status==EFI_ACCESS_DENIED);
  }else if(mode==11||mode==16){
    VOID *ptr=GuAlloc(16);CHECK(ptr);
    if(mode==11)Gu.compat_device=ptr;else Gu.release_state=ptr;
    CHECK(GuCpuPoolOwns(&Gu,ptr,16));GuFree(ptr);
    CHECK(!frees&&Gu.retained&&Gu.cpu_pool_failure_status==EFI_ACCESS_DENIED&&Gu.cpu_pool_failure_address==ptr);
    CHECK(Gu.cpu_pool&&(mode==11?Gu.compat_device:Gu.release_state)==ptr&&!GuCpuPoolOwns(&Gu,ptr,16));
    free(Gu.cpu_pool);
  }else if(mode==17||mode==18||mode==21){
    CHECK(GuAlloc(32)==NULL&&allocated&&calls==1&&!frees);
    CHECK(!platforms[0].cpu_pool&&!platforms[1].cpu_pool);
    CHECK(platforms[0].fault&&platforms[0].retained&&!platforms[1].fault&&!platforms[1].retained);
    CHECK(platforms[0].cpu_pool_failure_address==allocated);
    CHECK(platforms[0].cpu_pool_failure_status==(mode==18?EFI_OUT_OF_RESOURCES:EFI_ACCESS_DENIED));
    CHECK(!platforms[1].cpu_pool_failure_status);
    active=&platforms[0];CHECK(!GuAlloc(8)&&calls==1);
    free(allocated); /* Test fixture teardown only. */
  }else if(mode==19||mode==20||mode==22){
    struct pci_dev *scope=(struct pci_dev *)GuAlloc(8);CHECK(scope);
    Gu.compat_device=scope;Gu.compat_device_open=1;
    VOID *temporary=GuCpuPoolAllocScoped(scope,32);CHECK(temporary);
    GU_CPU_POOL *base=Gu.cpu_pool,*remaining=base->next;
    free_error=mode==20;
    CHECK(!GuCpuPoolFreeScoped(scope,temporary)&&frees==1&&released==base);
    CHECK(platforms[0].cpu_pool==remaining&&!platforms[1].cpu_pool);
    CHECK(platforms[0].fault&&platforms[0].retained&&!platforms[1].fault&&!platforms[1].retained);
    CHECK(platforms[0].cpu_pool_failure_status==(mode==20?EFI_DEVICE_ERROR:EFI_ACCESS_DENIED));
    CHECK(platforms[0].cpu_pool_failure_address==base&&!platforms[1].cpu_pool_failure_status);
    active=&platforms[0];Gu.bs=&bs;CHECK(!GuCpuPoolFreeScoped(scope,temporary)&&frees==1);
    if(free_error)free(base);
    free(remaining); /* Test fixture teardown only. */
  }else if(mode==23||mode==24){
    GU_PLATFORM *first=&platforms[0],*second=&platforms[1];
    CHECK(!GuAllocFor(NULL,8)&&!GuAllocFor(second,8)&&!calls);
    CHECK(!GuFreeFor(NULL,NULL)&&!GuFreeFor(second,NULL)&&!frees);
    VOID *a=GuAllocFor(first,32);CHECK(a&&calls==1&&GuCpuPoolOwns(first,a,32));
    if(mode==23){
      CHECK(GuFreeFor(first,a)&&frees==1&&!first->cpu_pool&&!first->fault);
    }else{
      active=second;
      VOID *b=GuAllocFor(second,16);CHECK(b&&calls==2&&GuCpuPoolOwns(second,b,16));
      CHECK(!GuAllocFor(first,8)&&!GuFreeFor(first,a)&&!GuFreeFor(first,b));
      CHECK(calls==2&&!frees&&GuCpuPoolOwns(first,a,32)&&GuCpuPoolOwns(second,b,16));
      CHECK(!first->fault&&!first->retained&&!second->fault&&!second->retained);
      CHECK(GuFreeFor(second,b)&&frees==1&&!second->cpu_pool);
      active=first;CHECK(GuFreeFor(first,a)&&frees==2&&!first->cpu_pool);
    }
  }else{
    struct pci_dev *scope=(struct pci_dev *)GuAlloc(8);
    CHECK(scope);Gu.compat_device=scope;Gu.compat_device_open=1;
    CHECK(GuCpuPoolCompatEmpty(&Gu));
    VOID *general=GuAlloc(16);
    VOID *temporary=GuCpuPoolAllocScoped(scope,32);
    CHECK(general&&temporary&&Gu.cpu_pool->scope==scope&&!GuCpuPoolCompatEmpty(&Gu));
    CHECK(GuCpuPoolOwns(&Gu,general,16)&&!GuCpuPoolOwns(&Gu,temporary,32));
    if(mode==12){
      CHECK(GuCpuPoolFreeScoped(scope,temporary)&&frees==1&&GuCpuPoolCompatEmpty(&Gu));
      GuFree(general);CHECK(frees==2&&!Gu.fault&&GuCpuPoolCompatEmpty(&Gu));
      free(Gu.cpu_pool);
    }else if(mode==13){
      GuFree(temporary);
      CHECK(!frees&&Gu.retained&&!GuCpuPoolCompatEmpty(&Gu)&&Gu.cpu_pool->scope==scope);
      while(Gu.cpu_pool){GU_CPU_POOL *old=Gu.cpu_pool;Gu.cpu_pool=old->next;free(old);}
    }else if(mode==14){
      CHECK(!GuCpuPoolFreeScoped(scope,general)&&!frees&&Gu.retained&&!GuCpuPoolCompatEmpty(&Gu));
      while(Gu.cpu_pool){GU_CPU_POOL *old=Gu.cpu_pool;Gu.cpu_pool=old->next;free(old);}
    }else{
      GU_CPU_POOL *previous=Gu.cpu_pool;
      active=&platforms[1];Gu.bs=&bs;
      CHECK(!GuCpuPoolAllocScoped(scope,8)&&!GuCpuPoolFreeScoped(scope,temporary));
      CHECK(Gu.fault&&!Gu.retained&&!frees&&!Gu.cpu_pool&&calls==3);
      CHECK(platforms[0].cpu_pool==previous&&!platforms[0].fault&&!GuCpuPoolCompatEmpty(&platforms[0]));
      active=&platforms[0];
      CHECK(GuCpuPoolFreeScoped(scope,temporary)&&GuCpuPoolCompatEmpty(&Gu));
      GuFree(general);free(Gu.cpu_pool);
    }
  }
  CHECK(GuAlloc(0)==NULL);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "scratch_pool.c"
            binary = Path(directory) / "scratch_pool"
            source.write_text(host, encoding="utf-8")
            compiled = subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-pedantic",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for mode in range(25):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), str(mode)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)

    def test_real_flr_never_skips_failed_firmware_delay(self):
        """Test the production reset and stall functions with fake EFI calls."""
        stall_start = SOURCE.index("VOID GuStall(GU_PLATFORM *owner,UINTN Microseconds) {")
        stall_end = SOURCE.index("\nEFI_STATUS GuEnableMaster(", stall_start)
        actual = SOURCE[stall_start:stall_end] + "\n" + function("GuFlr")
        host = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef void VOID;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef int EFI_STATUS;
typedef struct {EFI_STATUS (*Stall)(UINTN);} EFI_BOOT_SERVICES;
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 1
#define EFI_NOT_READY 2
#define EFI_ACCESS_DENIED 3
#define EFI_DEVICE_ERROR 4
#define EFI_TIMEOUT 5
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define TRUE 1
#define GU_PREFLIGHT_ONLY 0
#define EfiPciIoWidthUint32 3
#define EfiPciIoWidthUint16 2
typedef struct GU_PLATFORM {
  EFI_BOOT_SERVICES *bs;
  UINTN ticks;
  UINT16 pcie_cap;
  int profile_verified,saved,flrs,fault,owned,armed,display_ok;
  int retained,started;
} GU_PLATFORM;
static GU_PLATFORM Gu;
#define GuPlatformActive (&Gu)
static int mode,stall_calls,wait_ms_calls,reset_calls,raw_reads,pending_reads;
static EFI_STATUS FakeStall(UINTN us) {
  stall_calls++;
  if(us==1000)wait_ms_calls++;
  if(mode==2 && us==1000 && !reset_calls)return EFI_DEVICE_ERROR;
  if(mode==3 && us==100000)return EFI_DEVICE_ERROR;
  if(mode==4 && us==1000 && reset_calls)return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static EFI_STATUS GuConfigReadFor(GU_PLATFORM *owner,int width,UINT32 offset,VOID *out) {
  (void)owner;
  if(width==EfiPciIoWidthUint32 && offset==0) {
    *(UINT32 *)out=0x268410deU;return EFI_SUCCESS;
  }
  if(width==EfiPciIoWidthUint16 && offset==4) {
    *(UINT16 *)out=2U;return EFI_SUCCESS;
  }
  if(width==EfiPciIoWidthUint32 && offset==0x44) {
    *(UINT32 *)out=1U<<28;return EFI_SUCCESS;
  }
  if(width==EfiPciIoWidthUint16 && offset==0x4a) {
    *(UINT16 *)out=(mode==2 && pending_reads++==0)?0x20U:0U;
    return EFI_SUCCESS;
  }
  if(width==EfiPciIoWidthUint16 && offset==0x48) {
    *(UINT16 *)out=0U;return EFI_SUCCESS;
  }
  return EFI_DEVICE_ERROR;
}
static EFI_STATUS GuConfigWrite(GU_PLATFORM *owner,int width,UINT32 offset,VOID *value) {
  (void)owner;
  if(width!=EfiPciIoWidthUint16 || offset!=0x48 ||
     !(*(UINT16 *)value & 0x8000U))return EFI_DEVICE_ERROR;
  reset_calls++;
  return mode==6?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS RawReadFor(GU_PLATFORM *owner,int width,UINT32 offset,VOID *out) {
  (void)owner;
  if(width!=EfiPciIoWidthUint32 || offset!=0)return EFI_DEVICE_ERROR;
  raw_reads++;
  *(UINT32 *)out=(mode==4 && raw_reads==1)?0xffffffffU:0x268410deU;
  return EFI_SUCCESS;
}
static int GuIsSupportedPciId(UINT32 id) {return id==0x268410deU;}
static void GuStartedFor(GU_PLATFORM *owner) {owner->started=1;}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d mode %d: %s\n",__LINE__,mode,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES service={FakeStall};
  mode=argc>1?atoi(argv[1]):-1;
  if(mode<0 || mode>8)return 2;
  Gu.bs=&service;
  Gu.pcie_cap=0x40;
  Gu.profile_verified=Gu.saved=Gu.owned=Gu.armed=Gu.display_ok=1;
  if(mode==1 || mode==7)service.Stall=NULL;
  if(mode==5)Gu.bs=NULL;
  if(mode==7 || mode==8) {
    GuStall(&Gu,1000);
    CHECK(Gu.fault==(mode==7));
    CHECK(Gu.retained==(mode==7));
    CHECK(stall_calls==(mode==8));
    CHECK(Gu.ticks==(unsigned)(mode==8?1000:0));
    return 0;
  }
  EFI_STATUS status=GuFlr(&Gu);
  if(mode==0) {
    CHECK(status==EFI_SUCCESS);
    CHECK(reset_calls==1 && raw_reads==1 && Gu.flrs==1);
    CHECK(stall_calls==1 && Gu.ticks==100000 && !Gu.fault && !Gu.retained);
  } else if(mode==1 || mode==5) {
    CHECK(status==EFI_NOT_READY);
    CHECK(!reset_calls && !raw_reads && !stall_calls && !Gu.flrs);
  } else if(mode==2) {
    CHECK(status==EFI_DEVICE_ERROR);
    CHECK(!reset_calls && !raw_reads && !Gu.flrs);
    CHECK(Gu.fault && Gu.retained && !Gu.ticks && wait_ms_calls==1);
  } else if(mode==3) {
    CHECK(status==EFI_DEVICE_ERROR);
    CHECK(reset_calls==1 && !raw_reads && Gu.flrs==1 && Gu.started);
    CHECK(Gu.fault && Gu.retained && !Gu.ticks && stall_calls==1);
  } else if(mode==4) {
    CHECK(status==EFI_DEVICE_ERROR);
    CHECK(reset_calls==1 && raw_reads==1 && Gu.flrs==1 && Gu.started);
    CHECK(Gu.fault && Gu.retained && Gu.ticks==100000);
    CHECK(wait_ms_calls==1);
  } else if(mode==6) {
    CHECK(status==EFI_DEVICE_ERROR);
    CHECK(reset_calls==1 && !raw_reads && Gu.flrs==1 && Gu.started);
    CHECK(stall_calls==1 && Gu.ticks==100000);
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "flr_delay.c"
            binary = Path(directory) / "flr_delay"
            source.write_text(host, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                exe = Path(directory) / ("flr_delay_" + compiler)
                compiled = subprocess.run(
                    [compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                     "-Werror", "-pedantic", str(source), "-o", str(exe)],
                    capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                for mode in range(9):
                    with self.subTest(compiler=compiler, mode=mode):
                        executed = subprocess.run(
                            [str(exe), str(mode)], capture_output=True, text=True)
                        self.assertEqual(executed.returncode, 0,
                                         executed.stdout + executed.stderr)

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
#define EFI_ACCESS_DENIED 6
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define AllocateAnyPages 1
/* The production allocator asks for a buffer below an explicit 32-bit ceiling;
   the mock ignores the type, so only the name has to exist. */
#define AllocateMaxAddress 1
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
typedef struct GU_PLATFORM {
  EFI_PCI_IO_PROTOCOL *pci;
  GU_DMA *allocations;
  int profile_verified,armed,display_ok,fault,retained,owned;
  unsigned dma_count;
} GU_PLATFORM;
static GU_PLATFORM Gu;
#define GuPlatformActive (&Gu)
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
  EFI_STATUS result=GuDmaAllocate(&Gu,4096,&cpu,&dma);
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
    int retain=(mode==1||mode==2||mode==3||mode==4||mode==5||
                mode==6||mode==8||mode==9);
    CHECK((Gu.retained!=0)==retain);
    CHECK((Gu.allocations!=NULL)==(retain && mode!=2));
    CHECK(unmaps==(unsigned)(mode>=7&&mode<=9));
    CHECK(buffer_frees==(unsigned)(mode==7||mode==9));
    CHECK(pool_frees==(unsigned)(mode==0||mode==2||mode==7));
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

    def test_dma_descriptor_error_is_not_released_buffer_success(self):
        """Execute the actual descriptor cleanup against fake, non-device resources."""
        actual=function("GuDmaFree")
        host=r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef void VOID;
typedef int EFI_STATUS;
typedef uint64_t UINT64;
typedef size_t UINTN;
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 1
#define EFI_ACCESS_DENIED 2
#define EFI_DEVICE_ERROR 3
#define EFI_INVALID_PARAMETER 4
#define EFI_ERROR(s) ((s)!=EFI_SUCCESS)
#define TRUE 1
#define FALSE 0
typedef struct GU_DMA {
  struct GU_DMA *next;
  VOID *cpu,*mapping;
  UINT64 dma;
  UINTN bytes,pages;
  int mapped;
} GU_DMA;
typedef struct EFI_PCI_IO_PROTOCOL EFI_PCI_IO_PROTOCOL;
struct EFI_PCI_IO_PROTOCOL {
  EFI_STATUS (*Unmap)(EFI_PCI_IO_PROTOCOL *,VOID *);
  EFI_STATUS (*FreeBuffer)(EFI_PCI_IO_PROTOCOL *,UINTN,VOID *);
};
typedef struct GU_PLATFORM {
  EFI_PCI_IO_PROTOCOL *pci;
  GU_DMA *allocations;
  unsigned dma_count;
  int retained,master_attributes_pending,started,quiesced,fault;
  EFI_STATUS cpu_pool_failure_status;
} GU_PLATFORM;
static GU_PLATFORM Gu;
#define GuPlatformActive (&Gu)
static int mode,unmaps,buffer_frees,pool_frees;
static int buffer,mapping;
static EFI_STATUS Unmap(EFI_PCI_IO_PROTOCOL *io,VOID *token){
  (void)io;unmaps++;return token!=&mapping||mode==1?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Release(EFI_PCI_IO_PROTOCOL *io,UINTN pages,VOID *cpu){
  (void)io;buffer_frees++;return pages!=1||cpu!=&buffer||mode==2?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static VOID GuFree(VOID *p){
  (void)p;pool_frees++;
  if(mode==3){Gu.cpu_pool_failure_status=EFI_DEVICE_ERROR;Gu.retained=Gu.fault=1;}
}
""" +actual+r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %d: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void){
  EFI_PCI_IO_PROTOCOL io={Unmap,Release};
  for(mode=0;mode<5;mode++){
    GU_DMA entry={NULL,&buffer,&mapping,0x1000,4096,1,1};
    Gu.pci=&io;Gu.allocations=&entry;Gu.dma_count=1;
    Gu.retained=Gu.master_attributes_pending=Gu.started=Gu.fault=0;
    Gu.quiesced=1;Gu.cpu_pool_failure_status=EFI_SUCCESS;unmaps=buffer_frees=pool_frees=0;
    if(mode==4)Gu.retained=1;
    EFI_STATUS s=GuDmaFree(&Gu,&buffer,0x1000);
    CHECK(s==(mode==0?EFI_SUCCESS:mode==4?EFI_ACCESS_DENIED:EFI_DEVICE_ERROR));
    CHECK(unmaps==(mode!=4)&&buffer_frees==(mode==0||mode==2||mode==3));
    CHECK(pool_frees==(mode==0||mode==3));
    if(mode==0||mode==3){
      CHECK(!Gu.allocations&&!Gu.dma_count);
      CHECK(GuDmaFree(&Gu,&buffer,0x1000)==EFI_NOT_FOUND);
      CHECK(unmaps==1&&buffer_frees==1&&pool_frees==1);
      CHECK(Gu.retained==(mode==3));
    }else CHECK(Gu.allocations==&entry&&Gu.dma_count==1&&Gu.retained);
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src=Path(directory)/"dma_descriptor.c"
            src.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                exe=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(src),"-o",str(exe)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

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
