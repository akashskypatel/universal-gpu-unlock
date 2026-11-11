/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_execution_state.h"
#include <string.h>

static int Same(const GU_GPU_RECORD *a,const GU_GPU_RECORD *b) {
  return a->pci_id==b->pci_id && a->subsystem_id==b->subsystem_id &&
    a->class_code==b->class_code && a->segment==b->segment &&
    a->bus==b->bus && a->device==b->device && a->function==b->function &&
    a->handle==b->handle && a->pci==b->pci;
}
static int Valid(const GU_GPU_EXEC_OBSERVATION *o) {
  const GU_GPU_RECORD *r=&o->identity;
  return r->handle && r->pci && (r->pci_id&0xffffU)==0x10deU &&
    (r->class_code&0xff00U)==0x0300U &&
    r->segment<=0xffffU && r->bus<=255U &&
    r->device<=31U && r->function<=7U &&
    o->config[0]==r->pci_id && (o->config[2]>>16)==r->class_code &&
    o->config[11]==r->subsystem_id;
}
static int Alias(const GU_GPU_RECORD *a,const GU_GPU_RECORD *b) {
  return a->handle==b->handle || a->pci==b->pci ||
    (a->segment==b->segment && a->bus==b->bus &&
     a->device==b->device && a->function==b->function);
}
int GuGpuExecInit(GU_GPU_EXEC_SEQUENCE *s,const GU_GPU_EXEC_OBSERVATION *all,
                  size_t count,const size_t *order,size_t selected_count) {
  if(!s)return GU_GPU_EXEC_INVALID;
  memset(s,0,sizeof(*s));
  s->halted=1;
  s->active_step=GU_INVENTORY_MAX_NVIDIA;
  if(!all || !order || !count || count>GU_INVENTORY_MAX_NVIDIA ||
     !selected_count || selected_count>count)return GU_GPU_EXEC_INVALID;
  for(size_t i=0;i<count;i++) {
    if(!Valid(&all[i]))return GU_GPU_EXEC_INVALID;
    for(size_t j=0;j<i;j++)
      if(Alias(&all[i].identity,&all[j].identity))return GU_GPU_EXEC_INVALID;
  }
  for(size_t i=0;i<selected_count;i++) {
    if(order[i]>=count)return GU_GPU_EXEC_INVALID;
    for(size_t j=0;j<i;j++)
      if(order[i]==order[j])return GU_GPU_EXEC_INVALID;
  }
  for(size_t i=0;i<count;i++) {
    s->targets[i].identity=all[i].identity;
    memcpy(s->targets[i].expected_config,all[i].config,
           sizeof(s->targets[i].expected_config));
  }
  for(size_t i=0;i<selected_count;i++)s->order[i]=order[i];
  s->count=count;s->selected_count=selected_count;s->halted=0;
  return GU_GPU_EXEC_OK;
}
/* Fixed full inventory order includes unselected NVIDIA cards; a completed
 * card must match its verified post-state when a later step starts. */
static int Verify(const GU_GPU_EXEC_SEQUENCE *s,
                  const GU_GPU_EXEC_OBSERVATION *all,size_t count,
                  size_t changing) {
  if(!s || !all || !s->count || count!=s->count)return GU_GPU_EXEC_INVALID;
  for(size_t i=0;i<count;i++) {
    if(!Valid(&all[i]) || !Same(&s->targets[i].identity,&all[i].identity))
      return GU_GPU_EXEC_DRIFT;
    if(i!=changing &&
       memcmp(s->targets[i].expected_config,all[i].config,
              sizeof(all[i].config))!=0)return GU_GPU_EXEC_DRIFT;
  }
  return GU_GPU_EXEC_OK;
}
int GuGpuExecCheckNext(const GU_GPU_EXEC_SEQUENCE *s,
                       const GU_GPU_EXEC_OBSERVATION *all,size_t count) {
  if(!s || s->halted || s->next_step>=s->selected_count ||
     s->active_step!=GU_INVENTORY_MAX_NVIDIA ||
     s->targets[s->order[s->next_step]].phase!=GU_GPU_EXEC_PENDING)
    return GU_GPU_EXEC_BLOCKED;
  return Verify(s,all,count,GU_INVENTORY_MAX_NVIDIA);
}
void GuGpuExecAbort(GU_GPU_EXEC_SEQUENCE *s) {
  if(!s)return;
  if(s->active_step<s->selected_count)
    s->targets[s->order[s->active_step]].phase=GU_GPU_EXEC_FAILED;
  s->halted=1;
}
int GuGpuExecBegin(GU_GPU_EXEC_SEQUENCE *s,size_t step,
                   const GU_GPU_EXEC_OBSERVATION *all,size_t count) {
  int rc=GuGpuExecCheckNext(s,all,count);
  if(rc!=GU_GPU_EXEC_OK || !s || step!=s->next_step) {
    GuGpuExecAbort(s);
    return rc!=GU_GPU_EXEC_OK?rc:GU_GPU_EXEC_BLOCKED;
  }
  s->active_step=step;
  s->targets[s->order[step]].phase=GU_GPU_EXEC_ACTIVE;
  return GU_GPU_EXEC_OK;
}
int GuGpuExecFinish(GU_GPU_EXEC_SEQUENCE *s,size_t step,
                    const GU_GPU_EXEC_OBSERVATION *all,size_t count,
                    int cleanup_and_handover_verified) {
  if(!s || s->halted || step!=s->active_step ||
     step!=s->next_step || step>=s->selected_count) {
    GuGpuExecAbort(s);return GU_GPU_EXEC_BLOCKED;
  }
  size_t target=s->order[step];
  if(s->targets[target].phase!=GU_GPU_EXEC_ACTIVE ||
     !cleanup_and_handover_verified) {
    GuGpuExecAbort(s);return GU_GPU_EXEC_BLOCKED;
  }
  int rc=Verify(s,all,count,target);
  if(rc!=GU_GPU_EXEC_OK) {GuGpuExecAbort(s);return rc;}
  memcpy(s->targets[target].expected_config,all[target].config,
         sizeof(all[target].config));
  s->targets[target].phase=GU_GPU_EXEC_COMPLETE;
  s->active_step=GU_INVENTORY_MAX_NVIDIA;
  s->next_step++;
  return GU_GPU_EXEC_OK;
}
