/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure mocked services: no physical EFI/GPU calls; all error cases fail closed. */
#include "gpu_pci_observation_mock.h"
#include "../src/gpu_pci_observation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static EFI_PCI_IO_PROTOCOL cards[110];
static UINT8 path[20]={2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,0x7f,0xff,4,0};
static unsigned handle_count=5,locate_failure,read_failure,location_failure;
static unsigned path_failure,owner_failure,free_failure,oversize;
static unsigned warning_mode,alias_capture,shared_pool;
static VOID *capture_alias;
static unsigned pci_read_calls,owner_calls,free_calls,locate_calls;
static VOID *uncertain_locate,*uncertain_owners,*failed_frees[2];
static unsigned failed_free_count;
/* One deliberately reused allocation for the duplicate-reference negative. */
static union {
  EFI_HANDLE handles[5];
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entry;
} shared;
static EFI_STATUS locate(int type,EFI_GUID *g,VOID *p,UINTN *n,EFI_HANDLE **out) {
  (void)g;(void)p;locate_calls++;
  if(type!=AllHandles)return EFI_COMPROMISED_DATA;
  if(locate_failure==1)return EFI_DEVICE_ERROR;
  *n=oversize?GU_PCI_OBS_MAX_HANDLES+1:handle_count;
  if(alias_capture) {
    /* The reported handle array lives inside the caller's live capture. */
    *out=(EFI_HANDLE *)capture_alias;
    return EFI_SUCCESS;
  }
  if(shared_pool) {*out=&shared.handles[0];return EFI_SUCCESS;}
  *out=(EFI_HANDLE *)malloc((*n)*sizeof(**out));
  if(!*out)return EFI_DEVICE_ERROR;
  for(UINTN i=0;i<*n;i++)(*out)[i]=(EFI_HANDLE)(uintptr_t)(i+1);
  if(locate_failure==2){uncertain_locate=*out;return EFI_DEVICE_ERROR;}
  return EFI_SUCCESS;
}
static EFI_STATUS read_pci(EFI_PCI_IO_PROTOCOL *io,int width,UINT32 offset,UINTN count,VOID *value) {
  UINT32 *p=(UINT32 *)value;
  pci_read_calls++;
  if(width!=EfiPciIoWidthUint32 || offset || count!=16)return EFI_COMPROMISED_DATA;
  if(read_failure && io->index==0)return EFI_DEVICE_ERROR;
  if(warning_mode==1 && io->index==0)return EFI_WARN_STALE_DATA;
  memset(p,0,16*sizeof(UINT32));
  p[0]=io->index==0?0x268410deU:io->index==1?0x16381002U:io->index==2?0x14831022U:0x268410deU;
  p[2]=io->index==2?0x06040000U:io->index==3?0x04030000U:0x03000000U;
  p[11]=io->index==0?0x88e61043U:0;
  return EFI_SUCCESS;
}
static EFI_STATUS location(EFI_PCI_IO_PROTOCOL *io,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
  if(location_failure && io->index==1)return EFI_DEVICE_ERROR;
  if(warning_mode==5 && io->index==1)return EFI_WARN_UNKNOWN_GLYPH;
  *s=0;*b=(UINTN)(io->index+1);*d=0;*f=0;return EFI_SUCCESS;
}
static EFI_STATUS handle(EFI_HANDLE h,EFI_GUID *g,VOID **value) {
  size_t i=(size_t)(uintptr_t)h-1;
  if(g->Data1==1) {
    if(i==4)return EFI_UNSUPPORTED;
    if(i>=110)return EFI_NOT_FOUND;
    *value=&cards[i];return EFI_SUCCESS;
  }
  if(g->Data1==2) {
    if(path_failure && i==0)return EFI_NOT_FOUND;
    *value=path;
    if(warning_mode==2 && i==0)return EFI_WARN_UNKNOWN_GLYPH;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS owners(EFI_HANDLE h,EFI_GUID *g,
 EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
  (void)g;
  owner_calls++;
  if(owner_failure==1 && (size_t)(uintptr_t)h==1)return EFI_DEVICE_ERROR;
  if(warning_mode==4 && (size_t)(uintptr_t)h==1)return EFI_WARN_STALE_DATA;
  *n=1;
  if(shared_pool) {
    shared.entry.AgentHandle=(EFI_HANDLE)(uintptr_t)88;
    shared.entry.Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
    *out=&shared.entry;
  } else {
    *out=malloc(sizeof(**out));
    if(!*out)return EFI_DEVICE_ERROR;
    (*out)[0].AgentHandle=(EFI_HANDLE)(uintptr_t)88;
    (*out)[0].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
  }
  if(owner_failure==2 && (size_t)(uintptr_t)h==1){uncertain_owners=*out;return EFI_DEVICE_ERROR;}
  return EFI_SUCCESS;
}
static EFI_STATUS free_pool(VOID *p) {
  free_calls++;
  if(free_failure==2)return EFI_WARN_STALE_DATA;
  if(free_failure){if(failed_free_count<2)failed_frees[failed_free_count++]=p;return EFI_DEVICE_ERROR;}
  free(p);return EFI_SUCCESS;
}
static void setup(void) {
  handle_count=5;locate_failure=read_failure=location_failure=0;
  path_failure=owner_failure=free_failure=oversize=0;
  warning_mode=alias_capture=shared_pool=0;
  pci_read_calls=owner_calls=free_calls=locate_calls=0;
  uncertain_locate=uncertain_owners=NULL;failed_free_count=0;
  memset(&shared,0,sizeof(shared));
  for(unsigned i=0;i<5;i++)shared.handles[i]=(EFI_HANDLE)(uintptr_t)(i+1);
  for(unsigned i=0;i<110;i++) {
    cards[i].index=i;cards[i].Pci.Read=read_pci;cards[i].GetLocation=location;
  }
}
int main(void) {
  static GU_PCI_OBS_CAPTURE capture;
  EFI_BOOT_SERVICES boot={locate,handle,free_pool,owners};
  setup();
  CHECK(GuPciObserveReadOnly(&boot,&capture)==1);
  CHECK(capture.handle_count==5 && capture.pci_protocol_count==4);
  CHECK(capture.record_count==3 && capture.relevant_count==3);
  CHECK(capture.records[0].config[0]==0x268410deU);
  CHECK(capture.records[0].path_ok && capture.records[0].by_driver==1);
  CHECK(capture.records[0].first_agent==(EFI_HANDLE)(uintptr_t)88);
  CHECK(capture.records[0].index==0 && capture.records[2].index==2);
  CHECK(pci_read_calls==4 && owner_calls==3);
  CHECK(free_calls==4);
  CHECK(!capture.warnings && !capture.uncertain_duplicate && !capture.uncertain_alias);
  setup();read_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsRead && capture.first_failure_index==0);
  CHECK(capture.record_count==2 && capture.pci_protocol_count==4);
  setup();location_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsLocation && capture.first_failure_index==1);
  setup();path_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsPath && capture.record_count==3);
  CHECK(!capture.records[0].path_ok);
  setup();owner_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsOwners && !capture.records[0].owners_known);
  setup();free_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsFree && capture.failures>=1);
  CHECK(owner_calls==1 && pci_read_calls==1 && free_calls==2 && failed_free_count==2);
  CHECK(capture.uncertain_pool_count==2);
  CHECK(capture.uncertain_pool[0].address==failed_frees[0] &&
        capture.uncertain_pool[0].stage==GuPciObsFree &&
        capture.uncertain_pool[0].index==0 && capture.uncertain_pool[0].status==EFI_DEVICE_ERROR);
  CHECK(capture.uncertain_pool[1].address==failed_frees[1] &&
        capture.uncertain_pool[1].index==5 && capture.free_status==EFI_DEVICE_ERROR);
  free(failed_frees[0]);free(failed_frees[1]); /* Test teardown only. */
  setup();locate_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsLocate && capture.pci_protocol_count==0);
  CHECK(!capture.uncertain_pool_count && !free_calls);
  setup();locate_failure=2;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsLocate && !pci_read_calls && !owner_calls && !free_calls);
  CHECK(uncertain_locate && capture.uncertain_pool_count==1 &&
        capture.uncertain_pool[0].address==uncertain_locate &&
        capture.uncertain_pool[0].status==EFI_DEVICE_ERROR);
  free(uncertain_locate); /* Test teardown only. */
  setup();owner_failure=2;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsOwners && !capture.records[0].owners_known);
  CHECK(uncertain_owners && capture.uncertain_pool_count==1 &&
        capture.uncertain_pool[0].address==uncertain_owners &&
        capture.uncertain_pool[0].index==0 && capture.uncertain_pool[0].status==EFI_DEVICE_ERROR);
  CHECK(owner_calls==1 && pci_read_calls==1 && free_calls==1);
  free(uncertain_owners); /* Test teardown only. */
  setup();oversize=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.first_failure_stage==GuPciObsCount && capture.pci_protocol_count==0);
  setup();handle_count=101;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.truncated==1 && capture.record_count==GU_PCI_OBS_MAX_RECORDS);
  CHECK(capture.first_failure_stage==GuPciObsCount);
  setup();
  CHECK(!GuPciObserveReadOnly(NULL,&capture));
  CHECK(!GuPciObserveReadOnly(&boot,NULL));

  /* A UEFI warning is a success code without confirmed completion. It must
   * neither be accepted as observed evidence nor hidden from the report. */
  setup();warning_mode=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.warnings==1 && capture.first_warning_stage==GuPciObsRead);
  CHECK(capture.first_warning_index==0 && capture.first_warning_status==EFI_WARN_STALE_DATA);
  CHECK(capture.first_failure_status==EFI_WARN_STALE_DATA);
  setup();warning_mode=2;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.warnings==1 && capture.first_warning_stage==GuPciObsPath &&
        capture.first_warning_status==EFI_WARN_UNKNOWN_GLYPH);
  CHECK(!capture.records[0].path_ok);
  setup();warning_mode=4;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.warnings==1 && capture.first_warning_stage==GuPciObsOwners &&
        !capture.records[0].owners_known);
  setup();warning_mode=5;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.warnings==1 && capture.first_warning_stage==GuPciObsLocation &&
        capture.first_warning_index==1);
  setup();free_failure=2;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.warnings>=1 && capture.first_warning_stage==GuPciObsFree &&
        capture.first_warning_status==EFI_WARN_STALE_DATA);
  CHECK(capture.free_status==EFI_WARN_STALE_DATA && capture.uncertain_pool_count==2);

  /* A successful result that points inside the live capture is unverifiable:
   * it must be counted as an alias and never walked or released. */
  setup();alias_capture=1;capture_alias=&capture;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.uncertain_alias==1 && capture.uncertain_pool_count==0);
  CHECK(capture.first_failure_stage==GuPciObsCount &&
        capture.first_failure_status==EFI_COMPROMISED_DATA);
  CHECK(!pci_read_calls && !owner_calls && !free_calls);
  CHECK(capture.record_count==0 && !capture.pci_protocol_count);

  /* Successful but overlapping references to one live buffer are recorded
   * once, as one uncertain allocation rather than two owned buffers. */
  setup();shared_pool=1;free_failure=1;
  CHECK(!GuPciObserveReadOnly(&boot,&capture));
  CHECK(capture.uncertain_pool_count==1 && capture.uncertain_duplicate==1);
  CHECK(capture.uncertain_alias==0 && capture.failures==2 && free_calls==2);
  CHECK(capture.uncertain_pool[0].address==(VOID *)&shared);
  CHECK(capture.uncertain_pool[0].bytes==sizeof(shared.entry));
  CHECK(failed_free_count==2);
  puts("PASS independent passive PCI evidence and negative UEFI service mocks");
  printf("CHECKS=%u\n",checks);
  return 0;
}
