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
                  size_t count,const size_t *order,size_t selected_count,
                  GU_GPU_EXEC_TARGET *target_storage,size_t *order_storage,
                  size_t capacity) {
  if(!s)return GU_GPU_EXEC_INVALID;
  memset(s,0,sizeof(*s));
  s->halted=1;
  s->active_step=GU_GPU_EXEC_NO_STEP;
  if(!all || !order || !target_storage || !order_storage || !capacity ||
     capacity>GU_INVENTORY_MAX_HANDLES || !count || count>capacity ||
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
  s->capacity=capacity;s->targets=target_storage;s->order=order_storage;
  memset(s->targets,0,capacity*sizeof(*s->targets));
  memset(s->order,0,capacity*sizeof(*s->order));
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
/* Treat the record as untrusted at every state transition. A malformed
 * order or count must never become an out-of-bounds read or allow a
 * completed target to be silently marked pending. */
static int DistinctOwners(const GU_GPU_EXEC_OWNERS *o) {
  return o->platform && o->release_plan && o->firmware_assets &&
    o->platform!=o->release_plan && o->platform!=o->firmware_assets &&
    o->release_plan!=o->firmware_assets;
}
static int SharedOwners(const GU_GPU_EXEC_OWNERS *a,
                        const GU_GPU_EXEC_OWNERS *b) {
  const void *p[3]={a->platform,a->release_plan,a->firmware_assets};
  const void *q[3]={b->platform,b->release_plan,b->firmware_assets};
  for(size_t i=0;i<3;i++)for(size_t j=0;j<3;j++)
    if(p[i] && p[i]==q[j])return 1;
  return 0;
}
static int StorageValid(const GU_GPU_EXEC_SEQUENCE *s) {
  return s && s->targets && s->order && s->capacity &&
         s->capacity<=GU_INVENTORY_MAX_HANDLES;
}
static int StateValid(const GU_GPU_EXEC_SEQUENCE *s) {
  if(!StorageValid(s) || !s->count || s->count>s->capacity ||
     !s->selected_count || s->selected_count>s->count ||
     s->next_step>s->selected_count ||
     (s->owned_mode!=0 && s->owned_mode!=1) ||
     (s->active_step!=GU_GPU_EXEC_NO_STEP &&
      (s->active_step!=s->next_step ||
       s->active_step>=s->selected_count)))
    return 0;
  if(s->owned_mode && !s->next_step &&
     s->active_step==GU_GPU_EXEC_NO_STEP)return 0;
  for(size_t i=0;i<s->selected_count;i++) {
    size_t target=s->order[i];
    if(target>=s->count)return 0;
    for(size_t j=0;j<i;j++)if(target==s->order[j])return 0;
    GU_GPU_EXEC_PHASE expected=i<s->next_step?GU_GPU_EXEC_COMPLETE:
      (i==s->next_step && s->active_step==i)?
        GU_GPU_EXEC_ACTIVE:GU_GPU_EXEC_PENDING;
    if(s->targets[target].phase!=expected)return 0;
    const GU_GPU_EXEC_OWNERS *owner=&s->targets[target].owners;
    if(expected==GU_GPU_EXEC_PENDING &&
       (owner->platform||owner->release_plan||owner->firmware_assets))
      return 0;
    if((owner->platform||owner->release_plan||owner->firmware_assets) &&
       !DistinctOwners(owner))return 0;
    /* The mode is independent of the owner references. Losing all of
     * the references must not convert an owned step into a model step. */
    if(expected!=GU_GPU_EXEC_PENDING &&
       !!owner->platform!=s->owned_mode)return 0;
    for(size_t j=0;j<i;j++)
      if(SharedOwners(owner,&s->targets[s->order[j]].owners))return 0;
  }
  /* Every other NVIDIA card must remain unowned and untouched by this
   * ordered execution. These peers are not in the selected-step loop. */
  for(size_t i=0;i<s->count;i++) {
    size_t j=0;
    for(;j<s->selected_count;j++)
      if(s->order[j]==i)break;
    if(j==s->selected_count) {
      const GU_GPU_EXEC_TARGET *peer=&s->targets[i];
      if(peer->phase!=GU_GPU_EXEC_PENDING ||
         peer->owners.platform || peer->owners.release_plan ||
         peer->owners.firmware_assets)return 0;
    }
  }
  return 1;
}
int GuGpuExecResolveBound(const GU_GPU_EXEC_SEQUENCE *s,
                         const void *handle,const void *pci,
                         size_t *step,size_t *target) {
  if(!step||!target||step==target)return GU_GPU_EXEC_INVALID;
  *step=GU_GPU_EXEC_NO_STEP;*target=GU_GPU_EXEC_NO_STEP;
  if(!handle||!pci)return GU_GPU_EXEC_INVALID;
  if(!StateValid(s)||s->halted)return GU_GPU_EXEC_BLOCKED;
  /* During execution, only the active step can own the binding. Between
   * steps, the previous completed context may still be bound for its
   * final local observation, or the next pending context may be bound. */
  size_t first=s->active_step!=GU_GPU_EXEC_NO_STEP?s->active_step:
    (s->next_step?s->next_step-1:0);
  size_t end=s->active_step!=GU_GPU_EXEC_NO_STEP?s->active_step+1:
    (s->next_step<s->selected_count?s->next_step+1:s->selected_count);
  for(size_t i=first;i<end;i++) {
    size_t index=s->order[i];
    const GU_GPU_RECORD *r=&s->targets[index].identity;
    if(r->handle==handle && r->pci==pci) {
      *step=i;*target=index;return GU_GPU_EXEC_OK;
    }
  }
  return GU_GPU_EXEC_BLOCKED;
}
int GuGpuExecCheckNext(const GU_GPU_EXEC_SEQUENCE *s,
                       const GU_GPU_EXEC_OBSERVATION *all,size_t count) {
  if(!StateValid(s) || s->halted || s->next_step>=s->selected_count ||
     s->active_step!=GU_GPU_EXEC_NO_STEP)
    return GU_GPU_EXEC_BLOCKED;
  return Verify(s,all,count,GU_GPU_EXEC_NO_STEP);
}
int GuGpuExecCheckComplete(const GU_GPU_EXEC_SEQUENCE *s,
                           const GU_GPU_EXEC_OBSERVATION *all,size_t count) {
  if(!StateValid(s) || s->halted || s->next_step!=s->selected_count ||
     s->active_step!=GU_GPU_EXEC_NO_STEP)
    return GU_GPU_EXEC_BLOCKED;
  return Verify(s,all,count,GU_GPU_EXEC_NO_STEP);
}
void GuGpuExecAbort(GU_GPU_EXEC_SEQUENCE *s) {
  if(!s)return;
  if(StorageValid(s) && s->active_step<s->selected_count &&
     s->active_step<s->capacity &&
     s->order[s->active_step]<s->count &&
     s->order[s->active_step]<s->capacity)
    s->targets[s->order[s->active_step]].phase=GU_GPU_EXEC_FAILED;
  s->halted=1;
}
int GuGpuExecBegin(GU_GPU_EXEC_SEQUENCE *s,size_t step,
                   const GU_GPU_EXEC_OBSERVATION *all,size_t count) {
  int rc=GuGpuExecCheckNext(s,all,count);
  if(rc!=GU_GPU_EXEC_OK || !s || step!=s->next_step ||
     s->owned_mode) {
    GuGpuExecAbort(s);
    return rc!=GU_GPU_EXEC_OK?rc:GU_GPU_EXEC_BLOCKED;
  }
  s->active_step=step;
  s->targets[s->order[step]].phase=GU_GPU_EXEC_ACTIVE;
  return GU_GPU_EXEC_OK;
}
/* Spend the claim before Begin, preserving it after cleanup or failure.
 * Shared global backing storage cannot be used for a second GPU step. */
int GuGpuExecBeginOwned(GU_GPU_EXEC_SEQUENCE *s,size_t step,
                        const GU_GPU_EXEC_OBSERVATION *all,size_t count,
                        const void *platform,const void *release_plan,
                        const void *firmware_assets) {
  int rc=GuGpuExecCheckNext(s,all,count);
  if(rc!=GU_GPU_EXEC_OK || step!=s->next_step ||
     (step!=0 && !s->owned_mode)) {
    GuGpuExecAbort(s);
    return rc!=GU_GPU_EXEC_OK?rc:GU_GPU_EXEC_BLOCKED;
  }
  GU_GPU_EXEC_OWNERS next={platform,release_plan,firmware_assets};
  GU_GPU_EXEC_OWNERS *owner=&s->targets[s->order[step]].owners;
  if(!DistinctOwners(&next) || owner->platform || owner->release_plan ||
     owner->firmware_assets) {
    GuGpuExecAbort(s);return GU_GPU_EXEC_BLOCKED;
  }
  for(size_t i=0;i<s->selected_count;i++)
    if(i!=step && SharedOwners(&next,&s->targets[s->order[i]].owners)) {
      GuGpuExecAbort(s);return GU_GPU_EXEC_BLOCKED;
    }
  *owner=next;
  s->owned_mode=1;
  /* The claim is spent. Commit the already checked transition directly.
   * Calling GuGpuExecBegin here repeats CheckNext while this owner is still
   * pending, which correctly fails the new pending-owner invariant. */
  s->active_step=step;
  s->targets[s->order[step]].phase=GU_GPU_EXEC_ACTIVE;
  return GU_GPU_EXEC_OK;
}
int GuGpuExecFinish(GU_GPU_EXEC_SEQUENCE *s,size_t step,
                    const GU_GPU_EXEC_OBSERVATION *all,size_t count,
                    int cleanup_and_handover_verified) {
  if(!StateValid(s) || s->halted || step!=s->active_step ||
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
  s->active_step=GU_GPU_EXEC_NO_STEP;
  s->next_step++;
  return GU_GPU_EXEC_OK;
}
