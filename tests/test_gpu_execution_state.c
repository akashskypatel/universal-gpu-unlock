/* SPDX-License-Identifier: GPL-2.0-only */
#include "../src/gpu_execution_state.h"
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;} } while(0)
static GU_GPU_EXEC_OBSERVATION o[3];
static GU_GPU_EXEC_SEQUENCE s;
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
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
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
  CHECK(GuGpuExecInit(&s,o,2,duplicate,2)==GU_GPU_EXEC_INVALID);
  CHECK(s.halted && s.count==0);
  CHECK(GuGpuExecInit(&s,o,2,missing,2)==GU_GPU_EXEC_INVALID);
  CHECK(GuGpuExecInit(&s,o,2,order,0)==GU_GPU_EXEC_INVALID);
  CHECK(GuGpuExecInit(&s,o,2,NULL,2)==GU_GPU_EXEC_INVALID);
  o[0].identity.handle=o[1].identity.handle;
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);
  Reset();o[1].identity.pci=o[0].identity.pci;
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);
  Reset();o[1].identity.bus=o[0].identity.bus;
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);
  Reset();o[0].config[11]^=1U;
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_INVALID);

  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  o[0].config[4]^=0x1000U;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_DRIFT);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_DRIFT && s.halted);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  o[0].config[4]^=0x1000U; /* Non-target drift during first step. */
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_DRIFT);
  CHECK(s.halted && s.targets[1].phase==GU_GPU_EXEC_FAILED);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,0)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && s.targets[1].phase==GU_GPU_EXEC_FAILED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  o[1].identity.pci=(void *)(uintptr_t)0x3333U;
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_DRIFT);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  o[1].config[4]^=0x1000U;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_DRIFT);
  CHECK(GuGpuExecBegin(&s,1,o,2)==GU_GPU_EXEC_DRIFT);
  CHECK(s.halted);
  Reset();
  CHECK(GuGpuExecInit(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,3)==GU_GPU_EXEC_OK);
  o[2].config[4]^=0x1000U;
  CHECK(GuGpuExecFinish(&s,0,o,3,1)==GU_GPU_EXEC_DRIFT);
  /* State counts, selection order, and phase flags are checked again
   * before every read or transition, even if memory was corrupted. */
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
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
  s.active_step=GU_INVENTORY_MAX_NVIDIA;
  CHECK(GuGpuExecCheckNext(&s,o,2)==GU_GPU_EXEC_OK);

  /* An unselected NVIDIA peer must not silently gain an execution phase
   * or any backing resource owner. Both defects previously passed. */
  Reset();
  CHECK(GuGpuExecInit(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[2].phase=GU_GPU_EXEC_ACTIVE;
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[2].owners.platform=&o[2];
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[2].owners=(GU_GPU_EXEC_OWNERS){&o[2],&s.targets[2],&o[2].config};
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);

  /* A future selected step cannot reserve otherwise valid owner tokens
   * before its BeginOwned transition. */
  Reset();
  CHECK(GuGpuExecInit(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  s.targets[0].owners=(GU_GPU_EXEC_OWNERS){&o[0],&s.targets[0],&o[0].config};
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);
  CHECK(GuGpuExecBeginOwned(&s,0,o,3,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);

  /* An unexpected peer owner after step one also blocks step two. */
  Reset();
  CHECK(GuGpuExecInit(&s,o,3,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,3,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,3,1)==GU_GPU_EXEC_OK);
  s.targets[2].owners.platform=s.targets[1].owners.platform;
  CHECK(GuGpuExecCheckNext(&s,o,3)==GU_GPU_EXEC_BLOCKED);

  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBegin(&s,0,o,2)==GU_GPU_EXEC_OK);
  s.order[0]=GU_INVENTORY_MAX_NVIDIA; /* Abort itself must be bounded. */
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);
  /* A separate backing set is accepted. A reused platform, release plan,
   * or firmware asset store must fail and retain the sticky halt. */
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(s.targets[1].owners.platform==&o[1]);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[0],&s.targets[0],&o[0].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,1,o,2,1)==GU_GPU_EXEC_OK);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[1],&s.targets[0],&o[0].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && s.next_step==1);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&o[1],&o[1].config)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted && GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],NULL,&o[1].config)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,1,o,2,&o[0],&s.targets[0],&o[0].config)==GU_GPU_EXEC_BLOCKED);
  Reset();
  CHECK(GuGpuExecInit(&s,o,2,order,2)==GU_GPU_EXEC_OK);
  CHECK(GuGpuExecBeginOwned(&s,0,o,2,&o[1],&s.targets[1],&o[1].config)==GU_GPU_EXEC_OK);
  s.targets[1].owners.firmware_assets=s.targets[1].owners.platform;
  CHECK(GuGpuExecFinish(&s,0,o,2,1)==GU_GPU_EXEC_BLOCKED);
  CHECK(s.halted);
  printf("PASS independent ordered GPU execution state: %u checks\n",checks);
  return 0;
}
