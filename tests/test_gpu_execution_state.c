/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/gpu_execution_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;} } while(0)
static GU_GPU_EXEC_OBSERVATION o[3];
static GU_GPU_EXEC_SEQUENCE s;
static GU_GPU_EXEC_TARGET target_storage[GU_INVENTORY_MAX_NVIDIA];
static size_t order_storage[GU_INVENTORY_MAX_NVIDIA];
static int Init(GU_GPU_EXEC_SEQUENCE *state,const GU_GPU_EXEC_OBSERVATION *all,
                size_t count,const size_t *selection,size_t selected_count) {
  return GuGpuExecInit(state,all,count,selection,selected_count,
                       target_storage,order_storage,GU_INVENTORY_MAX_NVIDIA);
}
static const size_t order[2]={1,0},duplicate[2]={1,1},missing[2]={1,3};
static void Reset(void) {
  memset(o,0,sizeof(o));
  memset(&s,0,sizeof(s));
  for(size_t i=0;i<3;i++) {
    GU_GPU_RECORD *r=&o[i].identity;
    r->pci_id=(i==0?0x268410deU:i==1?0x220a10deU:0x250310deU);
    r->subsystem_id=0x12340000U+(uint32_t)i;
    r->class_code=0x0300U;r->bus=(uint32_t)(i+1);
    r->handle=(void *)(uintptr_t)(0x1000U+16U*i);
    r->pci=(void *)(uintptr_t)(0x2000U+16U*i);
    o[i].config[0]=r->pci_id;
    o[i].config[2]=0x03000000U;
    o[i].config[11]=r->subsystem_id;
    o[i].config[4]=0xf0000000U+(uint32_t)(i*0x100000U);
  }
}
int main(void) {
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(s.order[0]==1 && s.order[1]==0);
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(s.targets[1].phase==GU_GPU_EXEC_ACTIVE);
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  o[1].config[4]^=0x1000U;
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(s.targets[1].phase==GU_GPU_EXEC_COMPLETE);
  CHECK(s.targets[0].phase==GU_GPU_EXEC_PENDING);
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_OK);
  o[0].config[4]^=0x2000U;
  CHECK(GuGpuExecFinish(&s,1,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(s.next_step==2 && !s.halted);
  CHECK(s.targets[0].phase==GU_GPU_EXEC_COMPLETE);
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);

  Reset();
  CHECK(Init(&s,o,2,duplicate,2)==GU_GPU_EXEC_INVALID);
  CHECK(s.halted && s.count==0);
  CHECK(Init(&s,o,2,missing,2)==GU_GPU_EXEC_INVALID);
  CHECK(Init(&s,o,2,order,0)==GU_GPU_EXEC_INVALID);
  CHECK(Init(&s,o,2,NULL,2)==GU_GPU_EXEC_INVALID);
  o[0].identity.handle=o[1].identity.handle;
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);
  Reset();o[1].identity.pci=o[0].identity.pci;
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);
  Reset();o[1].identity.bus=o[0].identity.bus;
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);
  Reset();o[0].config[11]^=1U;
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);

  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  o[0].config[4]^=0x1000U;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_DRIFT);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_DRIFT && s.halted);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  o[0].config[4]^=0x1000U; /* Non-target drift during first step. */
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_DRIFT);
  CHECK(s.halted && s.targets[1].phase==GU_GPU_EXEC_FAILED);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,0)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && s.targets[1].phase==GU_GPU_EXEC_FAILED);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  o[1].identity.pci=(void *)(uintptr_t)0x3333U;
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_DRIFT);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  o[1].config[4]^=0x1000U;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_DRIFT);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_DRIFT);
  CHECK(s.halted);
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,3)==GU_GPU_EXEC_OK);
  o[2].config[4]^=0x1000U;
  CHECK(GuGpuExecFinish(&s,0,o,3,1)==GU_GPU_EXEC_DRIFT);
  /* State counts, selection order, and phase flags are checked again
   * before every read or transition, even if memory was corrupted. */
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  s.count=GU_INVENTORY_MAX_NVIDIA+1;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.count=2;s.selected_count=GU_INVENTORY_MAX_NVIDIA+1;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.selected_count=2;s.next_step=GU_INVENTORY_MAX_NVIDIA+1;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.next_step=0;s.order[0]=GU_INVENTORY_MAX_NVIDIA;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.order[0]=1;s.order[1]=1;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.order[1]=0;s.targets[1].phase=GU_GPU_EXEC_COMPLETE;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.targets[1].phase=GU_GPU_EXEC_PENDING;
  s.active_step=GU_INVENTORY_MAX_NVIDIA-1;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  s.active_step=GU_GPU_EXEC_NO_STEP;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_OK);

  /* An unselected NVIDIA peer must not silently gain an execution phase
   * or any backing resource owner. Both defects previously passed. */
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[2].phase=GU_GPU_EXEC_ACTIVE;
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[2].owners.platform=&o[2];
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[2].owners=(GU_GPU_EXEC_OWNERS){&o[2],&s.targets[2],&o[2].config};
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);

  /* A future selected step cannot reserve otherwise valid owner tokens
   * before its BeginOwned transition. */
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[0].owners=(GU_GPU_EXEC_OWNERS){&o[0],&s.targets[0],&o[0].config};
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  CHECK(GuGpuExecBeginOwned(&s,0,o,3,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);

  /* An unexpected peer owner after step one also blocks step two. */
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,3,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,3,1)==GU_GPU_EXEC_OK);
  s.targets[2].owners.platform=s.targets[1].owners.platform;
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);

  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  s.order[0]=GU_INVENTORY_MAX_NVIDIA; /* Abort itself must be bounded. */
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);
  /* A separate backing set is accepted. A reused platform, release plan,
   * or firmware asset store must fail and retain the sticky halt. */
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(s.targets[1].owners.platform==&o[1]);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[0],&s.targets[0],&o[0].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,1,o,2,1)==GU_GPU_EXEC_OK);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[1],&s.targets[0],&o[0].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && s.next_step==1);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&o[1],&o[1].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],NULL,&o[1].config)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[0],&s.targets[0],&o[0].config)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  s.targets[1].owners.firmware_assets=s.targets[1].owners.platform;
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);
  /* A sequence must not switch from real owner records to ownerless
   * model transitions, or the reverse, after completing its first step. */
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && s.next_step==1);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[0],&s.targets[0],&o[0].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && s.next_step==1);
  /* Final observations must include both selected devices and every peer.
   * This is a CPU model check, not a physical handover or firmware test. */
  Reset();
  CHECK(Init(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  CHECK(GuGpuExecBeginOwned(&s,0,o,3,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  o[1].config[4]^=0x1000U;
  CHECK(GuGpuExecFinish(&s,0,o,3,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  CHECK(GuGpuExecBeginOwned(&s,1,o,3,&o[0],&s.targets[0],&o[0].config)==GU_GPU_EXEC_OK);
  o[0].config[4]^=0x2000U;
  CHECK(GuGpuExecFinish(&s,1,o,3,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_OK);
  for(size_t i=0;i<3;i++) {
    o[i].config[4]^=0x4000U;
    CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_DRIFT);
    o[i].config[4]^=0x4000U;
  }
  CHECK(GuGpuExecCheckComplete(&s,o,2)==GU_GPU_EXEC_INVALID);
  o[0].identity.pci=(void *)(uintptr_t)0x3333U;
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_DRIFT);
  o[0].identity.pci=s.targets[0].identity.pci;
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_OK);
  memset(&s.targets[0].owners,0,sizeof(s.targets[0].owners));
  memset(&s.targets[1].owners,0,sizeof(s.targets[1].owners));
  CHECK(GuGpuExecCheckComplete(&s,o,3)==GU_GPU_EXEC_BLOCKED);

  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  memset(&s.targets[1].owners,0,sizeof(s.targets[1].owners));
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  memset(&s.targets[1].owners,0,sizeof(s.targets[1].owners));
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);
  Reset();
  CHECK(Init(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,1,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecCheckComplete(&s,o,2)==GU_GPU_EXEC_OK);
  GuGpuExecAbort(&s);
  CHECK(GuGpuExecCheckComplete(&s,o,2)==GU_GPU_EXEC_BLOCKED);
  /* Runtime-sized storage has no dependency on the legacy 16-device array.
   * These are observations and CPU owner references, not hardware actions. */
  {
    const size_t n=GU_INVENTORY_MAX_NVIDIA+2;
    GU_GPU_EXEC_OBSERVATION *all=calloc(n,sizeof(*all));
    GU_GPU_EXEC_TARGET *targets=calloc(n,sizeof(*targets));
    size_t *selected=calloc(n,sizeof(*selected));
    size_t *steps=calloc(n,sizeof(*steps));
    GU_GPU_EXEC_SEQUENCE dynamic;
    CHECK(all && targets && selected && steps);
    for(size_t i=0;i<n;i++) {
      GU_GPU_RECORD *id=&all[i].identity;
      id->pci_id=(i%3==0?0x268410deU:i%3==1?0x220a10deU:0x1e0710deU);
      id->subsystem_id=0x123410deU;id->class_code=0x0300U;id->bus=(uint32_t)(i+1);
      id->handle=(void *)(uintptr_t)(0x1000U+16U*i);
      id->pci=(void *)(uintptr_t)(0x2000U+16U*i);
      all[i].config[0]=id->pci_id;
      all[i].config[2]=0x03000000U;
      all[i].config[11]=id->subsystem_id;
      selected[i]=n-1-i;
    }
    CHECK(GuGpuExecInit(&dynamic,all,n,selected,n,targets,steps,n-1)==GU_GPU_EXEC_INVALID);
    CHECK(dynamic.halted && dynamic.count==0);
    CHECK(GuGpuExecInit(&dynamic,all,n,selected,n,targets,steps,n)==GU_GPU_EXEC_OK);
    CHECK(dynamic.targets==targets && dynamic.order==steps && dynamic.capacity==n);
    for(size_t i=0;i<n;i++) {
      size_t target=selected[i];
      CHECK(GuGpuExecBeginOwned(&dynamic,i,all,n,&all[target],&targets[target],&all[target].config)==GU_GPU_EXEC_OK);
      all[target].config[4]=(uint32_t)(0x1000U+i);
      CHECK(GuGpuExecFinish(&dynamic,i,all,n,1)==GU_GPU_EXEC_OK);
      CHECK(targets[target].phase==GU_GPU_EXEC_COMPLETE);
    }
    CHECK(GuGpuExecCheckComplete(&dynamic,all,n)==GU_GPU_EXEC_OK);
    all[n-1].config[4]^=0x100000U;
    CHECK(GuGpuExecCheckComplete(&dynamic,all,n)==GU_GPU_EXEC_DRIFT);
    free(steps);free(selected);free(targets);free(all);
  }
  printf("PASS independent ordered GPU execution state: %u checks\n",checks);
  return 0;
}
