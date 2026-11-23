/* SPDX-License-Identifier: GPL-2.0-only */
/* Selected NVIDIA controller ownership adapter. The initial shared-driver
 * graph came from MSI Z790/AD102 CAP0001 (2026-10-25); other boards still
 * require independent Stop/console/peer/recovery evidence before live use.
 * Snapshot/delta checks derive controller paths and driver agents at runtime.
 * No firmware variables or reset fallback are used. Normal transaction builds
 * do not reconnect controllers; the explicitly named stop/release-only bench
 * build adds a separate ConnectController + graph-verification path.
 */
#include "msi_release.h"
#include "stop_contract.h"
#include "port.h"
#include "boot_io.h"
#include "profile.h"
#include "sha256.h"
#include "gpu_device_path.h"
#include "gpu_discovery.h"
#include "pci_master_policy.h"
#include "checks.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>

#define MAX_SURVIVOR_PATH_BYTES 1024
/* One boot-local record per surviving graphics output protocol (GOP). */
typedef struct {
  EFI_HANDLE handle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT8 path[MAX_SURVIVOR_PATH_BYTES];
  UINTN path_bytes;
  BOOLEAN seen;
} SURVIVOR_GOP;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
/* Saved before Stop. Never read the old child handle after firmware removes it.
 * A new handle is accepted only with the same full PCI child device path. */
#define TARGET_CHILD_LIMIT 128U
typedef struct {
  EFI_HANDLE old_handle,live_handle;
  UINT8 path[MAX_SURVIVOR_PATH_BYTES];
  UINTN path_bytes;
  BOOLEAN is_gop;
} TARGET_CHILD;
#endif

#define GRAPH_EDGE_LIMIT 16384
#define GRAPH_INITIAL_CAPACITY 64
/* The limit bounds pool use and the quadratic graph comparisons, not GPU count. */
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;BOOLEAN usb_baseline,idle_output;} EDGE;
typedef struct {UINTN count,capacity;EDGE *edges;} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,gop,out_agent,gfx_agent,nv_agent;
  EFI_HANDLE console,input,error,pointer_agent,key_agent,usb_host,usb_agent;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT32 config[64];UINT64 attributes;UINT16 pcie_cap;
  /* Immutable identity and path bytes survive for the context lifetime.
   * Only the per-observation seen flag can change after capture. */
  SURVIVOR_GOP *survivors;
  UINTN survivor_count,survivor_capacity;
  BOOLEAN survivor_baseline_valid;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  TARGET_CHILD *target_children;
  UINTN target_child_count,target_child_capacity;
  BOOLEAN reconnecting;
#endif
  UINTN stage,reported_additions,reported_removals;BOOLEAN ready,mutated,claimed,hardware_verified,headless,master_normalized,reattach_verified;
  BOOLEAN consoleless_target_gop;
} PLAN;
/* Keep the release plan and its protocol graphs in one ownership context.
 * Never free a context that may describe a partially stopped driver or a
 * retained PCI claim. The existing one-shot admission remains unchanged. */
typedef struct GU_MSI_OWNED_STATE {
  PLAN plan;
  GRAPH original,current;
  const VOID *owner;
  EFI_BOOT_SERVICES *owner_bs;
  EFI_PCI_IO_PROTOCOL *owner_pci;
  EFI_HANDLE owner_handle,owner_image;
} GU_MSI_OWNED_STATE;
#define CurrentMsi (Gu.release_state)
#define Plan (CurrentMsi->plan)
#define Original (CurrentMsi->original)
#define Current (CurrentMsi->current)
#ifdef GU_STOP_RELEASE_ONLY_BENCH
static BOOLEAN StopFileOnlyOutput;
BOOLEAN GuMsiStopReleaseFileOnly(VOID) {return StopFileOnlyOutput;}
BOOLEAN GuMsiStopReleaseOnlyGopTarget(VOID) {return Plan.consoleless_target_gop;}
BOOLEAN GuMsiStopReleaseConsoleRestored(VOID) {
  return GuSelectedGpuCount()==1U&&Plan.consoleless_target_gop&&Plan.reattach_verified;
}
VOID GuMsiStopReleaseRequireFileOnly(VOID) {StopFileOnlyOutput=TRUE;}
#endif

/* A graph buffer is part of this owner's CPU pool, not a shared resource.
 * The caller has already validated the enclosing release record. */
static BOOLEAN GraphStorageOwned(const GU_PLATFORM *platform,const GRAPH *g) {
  if(g->count>g->capacity||g->capacity>GRAPH_EDGE_LIMIT||
     !!g->edges!=!!g->capacity)return FALSE;
  return !g->capacity||GuCpuPoolOwns(platform,g->edges,g->capacity*sizeof(*g->edges));
}
/* The saved display baseline is a separate allocation of the same owner. */
static BOOLEAN SurvivorStorageOwned(const GU_PLATFORM *platform,const PLAN *plan) {
  if(plan->survivor_count>plan->survivor_capacity||plan->survivor_capacity>4096u||
     !!plan->survivors!=!!plan->survivor_capacity)return FALSE;
  if(!plan->survivor_capacity)return !plan->survivor_baseline_valid;
  return GuCpuPoolOwns(platform,plan->survivors,
                       plan->survivor_capacity*sizeof(*plan->survivors));
}
/* Prove pool membership before dereferencing a retained release record.
 * This also works on a completed inactive platform without rebinding it. */
#ifdef GU_STOP_RELEASE_ONLY_BENCH
static BOOLEAN TargetChildStorageOwned(const GU_PLATFORM *platform,const PLAN *plan) {
  if(plan->target_child_count>plan->target_child_capacity||
     plan->target_child_capacity>TARGET_CHILD_LIMIT||
     !!plan->target_children!=!!plan->target_child_capacity)return FALSE;
  return !plan->target_child_capacity||
    GuCpuPoolOwns(platform,plan->target_children,
                  plan->target_child_capacity*sizeof(*plan->target_children));
}
#endif
static BOOLEAN ReleaseStateMatches(const GU_PLATFORM *platform) {
  if(!platform||platform->release_owner_invalid||!platform->release_state_reserved||
     !platform->bs||!platform->pci||!platform->handle||!platform->image)return FALSE;
  const GU_MSI_OWNED_STATE *state=platform->release_state;
  return GuCpuPoolOwns(platform,state,sizeof(*state))&&
    state->owner==platform&&state->owner_bs==platform->bs&&
    state->owner_pci==platform->pci&&state->owner_handle==platform->handle&&
    state->owner_image==platform->image&&
    GraphStorageOwned(platform,&state->original)&&GraphStorageOwned(platform,&state->current)&&
    SurvivorStorageOwned(platform,&state->plan)&&
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    TargetChildStorageOwned(platform,&state->plan)&&
    (const VOID *)state->plan.target_children!=state&&
    (!state->plan.target_children||
      ((const VOID *)state->plan.target_children!=state->original.edges&&
       (const VOID *)state->plan.target_children!=state->current.edges&&
       (const VOID *)state->plan.target_children!=state->plan.survivors))&&
#endif
    (const VOID *)state->original.edges!=state&&(const VOID *)state->current.edges!=state&&
    (const VOID *)state->plan.survivors!=state&&
    (!state->original.edges||state->original.edges!=state->current.edges)&&
    (!state->plan.survivors||
     ((const VOID *)state->plan.survivors!=state->original.edges&&
      (const VOID *)state->plan.survivors!=state->current.edges));
}
/* A lost or substituted context is an unsafe state, not an unused context. */
static BOOLEAN ReleaseOwnerMatches(VOID) {
  if(Gu.release_owner_invalid)return FALSE;
  if(!Gu.release_state_reserved && !CurrentMsi)return FALSE;
  if(!ReleaseStateMatches(&Gu)) {
    Gu.release_owner_invalid=TRUE;
    Gu.fault=TRUE;
    Gu.retained=TRUE;
    return FALSE;
  }
  return TRUE;
}

EFI_STATUS GuMsiReserveSelectedState(EFI_BOOT_SERVICES *bs) {
  if((Gu.release_state_reserved || Gu.release_state) && !ReleaseOwnerMatches())
    return EFI_NOT_READY;
  if(!bs||!bs->AllocatePool||!bs->FreePool||!Gu.handle||!Gu.pci||!Gu.image||Gu.bs!=bs ||
#ifdef GU_STOP_RELEASE_ONLY_BENCH
     !Gu.stop_release_ready||
#else
     !Gu.profile_verified||!Gu.prom_verified||
#endif
     Gu.started||Gu.owned||Gu.armed||Gu.fault||Gu.retained)return EFI_NOT_READY;
  if(Gu.release_state_reserved)return EFI_ALREADY_STARTED;
  GU_MSI_OWNED_STATE *state=GuAlloc(sizeof(*state));
  if(!state)return Gu.cpu_pool_failure_status!=EFI_SUCCESS?
    Gu.cpu_pool_failure_status:EFI_OUT_OF_RESOURCES;
  state->owner=&Gu;state->owner_bs=bs;state->owner_pci=Gu.pci;
  state->owner_handle=Gu.handle;state->owner_image=Gu.image;
  /* Publish only after the complete allocation succeeds. */
  Gu.release_state=state;Gu.release_state_reserved=TRUE;
  return EFI_SUCCESS;
}
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
/* Standard UEFI EDID Discovered interface on a video-output child. Check
 * all 128 GUID bits, not the Data1 value printed by a diagnostic. */
static EFI_GUID EdidDiscoveredGuid={0x1c0c34f6,0xd380,0x41fa,
                                   {0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}};
#endif
static EFI_GUID PointerGuid={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0,0x90,0x27,0x3f,0xc1,0x4d}};
/* CAP0001: the Z790 pointer splitter also consumes an absolute pointer on the same USB device. */
static EFI_GUID AbsPointerGuid={0x8d59d32b,0xc655,0x4ae9,{0x9b,0x15,0xf2,0x59,0x04,0x99,0x2a,0x43}};
static EFI_GUID UsbHcGuid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return Equal(a,b,16);}
static BOOLEAN PointerProto(const EFI_GUID *g){return Guid(g,&PointerGuid)||Guid(g,&AbsPointerGuid);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
/* A service error with a nonnull output does not prove a valid allocation.
 * Do not inspect or release that output. Independently known outer buffers
 * still need their one release attempt even after a nested failure. */
static EFI_STATUS ObservationOutput(EFI_STATUS status,VOID **out) {
  if(status!=EFI_SUCCESS && *out) {
    GuRecordCpuPoolFailure(status,*out);
    *out=NULL;
    return EFI_DEVICE_ERROR;
  }
  return status;
}
static EFI_STATUS ReleaseObservation(EFI_BOOT_SERVICES *bs,VOID *address,EFI_STATUS prior) {
  if(!address)return prior;
  EFI_STATUS status=bs&&bs->FreePool?bs->FreePool(address):EFI_UNSUPPORTED;
  if(status!=EFI_SUCCESS) {
    GuRecordCpuPoolFailure(status,address);
    return EFI_DEVICE_ERROR;
  }
  return prior;
}
/* Validate the enclosing owner before accessing either graph. */
static BOOLEAN SelectedGraph(GRAPH *g) {
  return ReleaseOwnerMatches()&&(g==&Original||g==&Current);
}
/* Keep the old graph if allocation fails with no uncertain ownership.
 * Consume its reference before attempting release. Never retry an uncertain
 * release or publish the candidate graph after an allocator failure. */
static EFI_STATUS GraphAppend(EFI_BOOT_SERVICES *b,GRAPH *g,EDGE edge) {
  if(!b||b!=Gu.bs||!b->AllocatePool||!b->FreePool||!g)return EFI_INVALID_PARAMETER;
  if(!SelectedGraph(g))return EFI_ACCESS_DENIED;
  if(g->count==GRAPH_EDGE_LIMIT)return EFI_BAD_BUFFER_SIZE;
  if(g->count==g->capacity) {
    UINTN capacity=g->capacity?g->capacity*2u:GRAPH_INITIAL_CAPACITY;
    if(capacity>GRAPH_EDGE_LIMIT)capacity=GRAPH_EDGE_LIMIT;
    EDGE *grown=GuAlloc(capacity*sizeof(*grown));
    if(!grown)return Gu.cpu_pool_failure_status!=EFI_SUCCESS?
      Gu.cpu_pool_failure_status:EFI_OUT_OF_RESOURCES;
    UINTN used=g->count;
    if(used)memcpy(grown,g->edges,used*sizeof(*grown));
    EDGE *previous=g->edges;
    g->edges=NULL;g->capacity=0;g->count=0;
    GuFree(previous);
    if(Gu.cpu_pool_failure_status!=EFI_SUCCESS||Gu.retained)
      return EFI_DEVICE_ERROR;
    g->edges=grown;g->capacity=capacity;g->count=used;
  }
  g->edges[g->count++]=edge;
  return EFI_SUCCESS;
}
static EFI_STATUS Snapshot(GRAPH *g) {
  if(!SelectedGraph(g))return EFI_ACCESS_DENIED;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *handles=NULL;UINTN count=0;g->count=0;
  EFI_STATUS s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  s=ObservationOutput(s,(VOID **)&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>4096))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN pc=0;
    s=b->ProtocolsPerHandle(handles[i],&protocols,&pc);
    s=ObservationOutput(s,(VOID **)&protocols);
    if(s==EFI_SUCCESS&&(!protocols||!pc||pc>64))s=EFI_BAD_BUFFER_SIZE;
    for(UINTN j=0;s==EFI_SUCCESS&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      s=b->OpenProtocolInformation(handles[i],protocols[j],&entries,&ec);
      s=ObservationOutput(s,(VOID **)&entries);
      if(s==EFI_SUCCESS&&(ec>128||(ec&&!entries)))s=EFI_BAD_BUFFER_SIZE;
      for(UINTN k=0;s==EFI_SUCCESS&&k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        BOOLEAN own=Plan.claimed&&handles[i]==Plan.pci&&e->AgentHandle==Plan.self&&
          !e->ControllerHandle&&e->Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->OpenCount==1;
        if(!e->AgentHandle||(!e->ControllerHandle&&!own)||!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        s=GraphAppend(b,g,(EDGE){handles[i],e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount,FALSE,FALSE});
      }
      s=ReleaseObservation(b,entries,s);
    }
    s=ReleaseObservation(b,protocols,s);
  }
  s=ReleaseObservation(b,handles,s);
  /* An incomplete capture must never appear to be a complete graph. */
  if(s!=EFI_SUCCESS)g->count=0;
  return s;
}
static BOOLEAN SameEdge(const EDGE *a,const EDGE *b) {
  return a->handle==b->handle&&a->agent==b->agent&&a->controller==b->controller&&
    a->attributes==b->attributes&&a->count==b->count&&Guid(&a->protocol,&b->protocol);
}
static BOOLEAN Removed(const EDGE *e,UINTN stage) {
  if(stage>=1&&Plan.gop&&(e->handle==Plan.gop||e->controller==Plan.gop)&&(e->agent==Plan.out_agent||Guid(&e->protocol,&TextGuid)))return TRUE;
  if(stage>=2&&Plan.gop&&(e->handle==Plan.gop||e->controller==Plan.gop)&&(e->agent==Plan.gfx_agent||Guid(&e->protocol,&GopGuid)))return TRUE;
  if(stage>=3&&(e->handle==Plan.pci||(Plan.gop&&e->handle==Plan.gop)||e->controller==Plan.pci||(Plan.gop&&e->controller==Plan.gop))&&e->agent==Plan.nv_agent)return TRUE;
  if(stage>=3&&Plan.gop&&e->handle==Plan.gop)return TRUE;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  /* A driver-bound EDID Discovered interface is a dependent of its verified
   * target video-output child. UEFI destroys children before removing the
   * parent driver. Allow this exact captured dependent to disappear only
   * at stage 3, not from a sibling, peer or unrelated protocol. */
  if(stage>=3&&e->idle_output&&e->handle==e->controller&&
     e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&e->count==1&&
     Guid(&e->protocol,&EdidDiscoveredGuid))return TRUE;
#endif
  /* Other idle-child protocol edges are never assumed removable. */
  return FALSE;
}
/* Original unrelated relationships must remain present. New relationships can
 * appear during USB enumeration; they cannot involve any protected endpoint.
 * A protocol GUID alone never authorizes access to the GPU or its owners. */
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  if(!h)return FALSE;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  for(UINTN k=0;k<Plan.target_child_count;k++)
    if(Plan.target_children[k].live_handle==h)return TRUE;
#endif
  if(h==Plan.pci||h==Plan.gop||h==Plan.self||
     GuIsPreparedPeerHandle(h)||
     h==Plan.console||h==Plan.input||h==Plan.error)return TRUE;
  for(UINTN k=0;k<Original.count;k++)
    if(Original.edges[k].idle_output&&h==Original.edges[k].controller)return TRUE;
  for(UINTN k=0;k<Plan.survivor_count;k++)
    if(Plan.survivors[k].handle==h)return TRUE;
  return FALSE;
}
static BOOLEAN ProtectedEdge(const EDGE *e) {
  return ProtectedHandle(e->handle)||ProtectedHandle(e->controller);
}
static BOOLEAN OwnEdge(const EDGE *e) {
  return e->handle==Plan.pci&&e->agent==Plan.self&&!e->controller&&
    Guid(&e->protocol,&PciGuid)&&e->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->count==1;
}
static BOOLEAN KnownEdge(const EDGE *e,UINTN stage) {
  for(UINTN i=0;i<Original.count;i++)
    if(!Removed(&Original.edges[i],stage)&&SameEdge(&Original.edges[i],e))return TRUE;
  return FALSE;
}
/* A late USB mouse attaches to the existing virtual console (RUN0005).
 * Only the preexisting pointer splitter may add this exact child relationship,
 * and it must also own that physical pointer BY_DRIVER on a non-GPU USB path.
 * No exception is based on a GUID prefix or on a raw handle address alone. */
static BOOLEAN UsbPointerPath(EFI_HANDLE h) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS||!p)return FALSE;
  UINTN at=0;BOOLEAN is_usb=FALSE;
  for(UINTN nodes=0;nodes<32&&at+4<=4096;nodes++) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>4096-at)return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE)return is_usb&&n==4&&p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE;
    if(p->Type==MESSAGING_DEVICE_PATH&&p->SubType==MSG_USB_DP)is_usb=TRUE;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return FALSE;
}
static BOOLEAN PointerOwner(const GRAPH *g,const EDGE *e) {
  UINTN owners=0;
  for(UINTN i=0;i<g->count;i++) {
    const EDGE *o=&g->edges[i];
    if(o->handle==e->handle&&Guid(&o->protocol,&e->protocol)&&
       o->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if(o->agent!=e->agent||o->controller!=e->handle||o->count!=1)return FALSE;
      owners++;
    }
  }
  return owners==1;
}
static BOOLEAN PointerAddition(const EDGE *e) {
  BOOLEAN splitter=(PointerProto(&e->protocol)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&
    !ProtectedHandle(e->handle)&&!ProtectedHandle(e->agent)&&
    e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->count==1&&
    PointerOwner(&Current,e)&&UsbPointerPath(e->handle);
}
static EFI_STATUS PointerBaseline(VOID) {
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    BOOLEAN pointer=PointerProto(&e->protocol),key=Guid(&e->protocol,&InputGuid);
    if((!pointer&&!key)||e->controller!=Plan.console||e->attributes!=8)continue;
    EFI_HANDLE *agent=pointer?&Plan.pointer_agent:&Plan.key_agent;
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||
       !PointerOwner(&Original,e)||
       (*agent&&e->agent!=*agent))continue;
    *agent=e->agent;
  }
  return EFI_SUCCESS;
}
/* Freeze which ORIGINAL relationships belong to this board's USB branch
 * while all their handles/paths still exist. Later removal never consults a
 * possibly freed or recycled handle. The USB host's driver/PCI ownership and
 * every GPU/display/application relationship remain mandatory. */
static BOOLEAN UsbConsoleChild(const EDGE *e,const GRAPH *g) {
  BOOLEAN splitter=(PointerProto(&e->protocol)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&e->attributes==8&&e->count==1&&PointerOwner(g,e);
}
static EFI_STATUS UsbBaseline(VOID) {
  for(UINTN i=0;i<Original.count;i++)Original.edges[i].usb_baseline=FALSE;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(!Guid(&e->protocol,&UsbHcGuid)||e->attributes!=0x10||e->controller!=e->handle||
       e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))continue;
    if(!Plan.usb_host){Plan.usb_host=e->handle;Plan.usb_agent=e->agent;}
  }
  if(!Plan.usb_host||!Plan.usb_agent){
    GuLog("MSI USB baseline: no host found; continuing without USB tracking\n");
    return EFI_SUCCESS;
  }
  UINTN dynamic=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))continue;
    BOOLEAN child=e->handle==Plan.usb_host&&e->agent==Plan.usb_agent&&
      Guid(&e->protocol,&UsbHcGuid)&&e->attributes==8&&!ProtectedHandle(e->controller)&&
      UsbPointerPath(e->controller);
    BOOLEAN physical=UsbPointerPath(e->handle);
    BOOLEAN device=physical&&((e->attributes==0x10&&e->controller==e->handle)||
      (e->attributes==8&&!ProtectedHandle(e->controller)&&UsbPointerPath(e->controller))||
      UsbConsoleChild(e,&Original));
    if(child||device){Original.edges[i].usb_baseline=TRUE;dynamic++;}
  }
  GuLog("MSI USB baseline: host=%#llx bus_agent=%#llx dynamic_edges=%zu; host ownership pinned\n",
    (unsigned long long)(UINTN)Plan.usb_host,(unsigned long long)(UINTN)Plan.usb_agent,(size_t)dynamic);
  return EFI_SUCCESS;
}
static BOOLEAN UsbRemoved(UINTN i) {
  if(!Original.edges[i].usb_baseline)return FALSE;
  const EDGE *e=&Original.edges[i];
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *c=&Current.edges[j];
    if(c->handle==e->handle&&c->agent==e->agent&&c->controller==e->controller&&Guid(&c->protocol,&e->protocol))return FALSE;
  }
  return TRUE;
}
static BOOLEAN Expected(UINTN stage,BOOLEAN claimed) {
  UINTN own=0;
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],stage))continue;
    /* On multi-socket, server, and workstation motherboards (e.g. ASUS WS C621E SAGE with BMC AST2500),
     * only edges touching protected GPU/GOP/console endpoints must be asserted; unrelated platform edges
     * (BMC, IPMI, secondary root bridges) may adapt during console detach without compromising GPU isolation. */
    if(!ProtectedEdge(&Original.edges[i]))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i)))return FALSE;
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(OwnEdge(e)){own++;continue;}
    if(!KnownEdge(e,stage)&&ProtectedEdge(e)&&!PointerAddition(e))return FALSE;
    /* Even unrelated new relationships must not be duplicated. */
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k]))return FALSE;
  }
  return own==(claimed?1u:0u);
}
/* v0.5.1 diagnostics: GuLog faults once the USB log is closed, so log only while it is open. */
#define NOTE(...) do{if(GuBootLogOpen())GuLog(__VA_ARGS__);}while(0)
static VOID NoteEdge(const char *kind,const EDGE *e) {
  NOTE("MSI graph %s: handle=%#llx agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",kind,
    (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,
    e->protocol.Data1,e->attributes,e->count);
}
/* Name the first stage-3 edge that is missing or unexpected after a failed Expected(). */
static VOID GraphDiff(UINTN stage,BOOLEAN claimed) {
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],stage))continue;
    if(!ProtectedEdge(&Original.edges[i]))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i))){NoteEdge(matches?"duplicate":"missing",&Original.edges[i]);return;}
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownEdge(e,stage)&&!(claimed&&OwnEdge(e))&&ProtectedEdge(e)&&!PointerAddition(e)){NoteEdge("unexpected",e);return;}
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k])){NoteEdge("duplicate",e);return;}
  }
}
static VOID ReportAdditions(VOID) {
  UINTN added=0,removed=0;
  for(UINTN i=0;i<Original.count;i++)if(UsbRemoved(i))removed++;
  if(removed!=Plan.reported_removals&&GuBootLogOpen()) {
    NOTE("MSI graph: %zu original USB relationships absent; host/GPU ownership and console callbacks unchanged\n",(size_t)removed);
    Plan.reported_removals=removed;
  }
  for(UINTN j=0;j<Current.count;j++)
    if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3))added++;
  if(added&&added!=Plan.reported_additions&&GuBootLogOpen()) {
    NOTE("MSI graph: accepted %zu new edges (unrelated or verified USB pointer); GPU ownership unchanged\n",(size_t)added);
    for(UINTN j=0,shown=0;j<Current.count&&shown<8;j++)
      if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3)){NoteEdge("new-unrelated",&Current.edges[j]);shown++;}
    Plan.reported_additions=added;
  }
}
/* Validate a driver before its Stop can be invoked by firmware. A child
 * display consumer has the same binding/image requirements as the three
 * controllers already present in this release plan. */
static EFI_STATUS ValidateDriverAgent(EFI_HANDLE a,UINTN which) {
  EFI_DRIVER_BINDING_PROTOCOL *d=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(a,&BindingGuid,(VOID **)&d);
  if(s!=EFI_SUCCESS||!d->Stop||d->DriverBindingHandle!=a||!d->ImageHandle) {
    GuLog("Driver(%zu) binding check failed: a=%#llx s=%#llx stop=%#llx bh=%#llx ih=%#llx ver=%#x\n",
      (size_t)which, (unsigned long long)(UINTN)a, (unsigned long long)s,
      (unsigned long long)(UINTN)(d?d->Stop:NULL), (unsigned long long)(UINTN)(d?d->DriverBindingHandle:NULL),
      (unsigned long long)(UINTN)(d?d->ImageHandle:NULL), d?d->Version:0);
    return EFI_UNSUPPORTED;
  }
  s=Interface(d->ImageHandle,&ImageGuid,(VOID **)&im);
  if(s!=EFI_SUCCESS) {
    GuLog("Driver(%zu) ImageGuid failed: %#llx\n", (size_t)which, (unsigned long long)s);
    return s;
  }
  /* Ordinary selected driver agents retain the original Boot Services code
   * requirement. A display-child EDID consumer can itself be a UEFI Runtime
   * Services driver. Accept only a spec-defined code/data memory-type pair
   * for that dependent, never an application, data or unknown image type. */
  BOOLEAN code_type_ok=im->ImageCodeType==EfiBootServicesCode;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(which==3U) {
    code_type_ok=
      (im->ImageCodeType==EfiBootServicesCode&&
       im->ImageDataType==EfiBootServicesData)||
      (im->ImageCodeType==EfiRuntimeServicesCode&&
       im->ImageDataType==EfiRuntimeServicesData);
  }
#endif
  UINTN image_base=(UINTN)im->ImageBase;
  UINTN stop=(UINTN)d->Stop;
  BOOLEAN stop_in_image=image_base&&stop>=image_base&&
                        (UINT64)(stop-image_base)<im->ImageSize;
  if(im->ImageSize<4096||!code_type_ok||!stop_in_image) {
    GuLog("Driver(%zu) image check failed: base=%#llx size=%zu stop=%#llx "
          "code_type=%u data_type=%u code_type_ok=%u stop_in_image=%u\n",
      (size_t)which,(unsigned long long)image_base,(size_t)im->ImageSize,
      (unsigned long long)stop,(unsigned)im->ImageCodeType,
      (unsigned)im->ImageDataType,(unsigned)code_type_ok,
      (unsigned)stop_in_image);
    return EFI_UNSUPPORTED;
  }
  GuLog("Driver(%zu) verified OK: ver=%#x size=%zu code_type=%u data_type=%u "
        "stop_in_image=1\n",(size_t)which,d->Version,(size_t)im->ImageSize,
        (unsigned)im->ImageCodeType,(unsigned)im->ImageDataType);
  return EFI_SUCCESS;
}
static EFI_STATUS Driver(UINTN which) {
  return ValidateDriverAgent(
    which==0?Plan.out_agent:which==1?Plan.gfx_agent:Plan.nv_agent,which);
}
static EFI_STATUS Alive(VOID) {
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(Plan.consoleless_target_gop)return EFI_SUCCESS;
#endif
  EFI_SYSTEM_TABLE *t=Plan.table;VOID *in=NULL,*out=NULL,*err=NULL;
  if(t->ConsoleOutHandle!=Plan.console||t->ConsoleInHandle!=Plan.input||t->StandardErrorHandle!=Plan.error||
     t->ConIn!=Plan.keys||t->ConOut!=Plan.text||t->StdErr!=Plan.err||!t->ConIn||
     (UINTN)t->ConIn->Reset!=Plan.key_reset||(UINTN)t->ConIn->ReadKeyStroke!=Plan.key_read||
     t->ConIn->WaitForKey!=Plan.key_wait)return EFI_NOT_READY;
  EFI_STATUS s=Interface(Plan.input,&InputGuid,&in);
  if(s==EFI_SUCCESS)s=Interface(Plan.console,&TextGuid,&out);
  if(s==EFI_SUCCESS)s=Interface(Plan.error,&TextGuid,&err);
  return s==EFI_SUCCESS&&(in!=Plan.keys||out!=Plan.text||err!=Plan.err)?EFI_NOT_READY:s;
}
/* An AMD or second NVIDIA GOP may enumerate before the target GPU's GOP.
 * The target is identified by the PCI device-path prefix, never GOP order. */
static EFI_STATUS GopOnTarget(EFI_HANDLE gop,BOOLEAN *target) {
  EFI_DEVICE_PATH_PROTOCOL *gpu_path=NULL,*gop_path=NULL;
  if(!target)return EFI_INVALID_PARAMETER;
  *target=FALSE;
  if(gop==Plan.pci){*target=TRUE;return EFI_SUCCESS;}
  EFI_STATUS s=Interface(Plan.pci,&PathGuid,(VOID **)&gpu_path);
  if(s!=EFI_SUCCESS)return s;
  s=Interface(gop,&PathGuid,(VOID **)&gop_path);
  if(s!=EFI_SUCCESS)return s;
  int result=GuDevicePathHasPrefix((const unsigned char *)gpu_path,4096u,
                                   (const unsigned char *)gop_path,4096u);
  if(result<0)return EFI_COMPROMISED_DATA;
  *target=result==1;
  return EFI_SUCCESS;
}
static EFI_STATUS Gops(BOOLEAN first) {
  if(!ReleaseOwnerMatches())return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  EFI_HANDLE *handles=NULL;
  UINTN count=0,physical=0,target_count=0;
  if(!b||!b->LocateHandleBuffer||!b->AllocatePool||!b->FreePool)
    return EFI_UNSUPPORTED;
  if(first && (Plan.survivors||Plan.survivor_capacity||
               Plan.survivor_count||Plan.survivor_baseline_valid))
    return EFI_ALREADY_STARTED;
  if(!first && (!Plan.survivor_baseline_valid||
                !Plan.survivors||Plan.survivor_count>Plan.survivor_capacity||
                !Plan.survivor_capacity||Plan.survivor_capacity>4096u))
    return EFI_NOT_READY;
  EFI_STATUS s=b->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  s=ObservationOutput(s,(VOID **)&handles);
  if(s==EFI_NOT_FOUND && !handles && !first && Plan.stage>=3 && !Plan.survivor_count)
    return EFI_SUCCESS;
  if(s==EFI_SUCCESS&&(!count||!handles||count>4096u))s=EFI_BAD_BUFFER_SIZE;
  if(s==EFI_SUCCESS && first) {
    SURVIVOR_GOP *records=GuAlloc(count*sizeof(*records));
    if(!records)s=Gu.cpu_pool_failure_status!=EFI_SUCCESS?
      Gu.cpu_pool_failure_status:EFI_OUT_OF_RESOURCES;
    else {
      Plan.survivors=records;
      Plan.survivor_capacity=count;
    }
    Plan.gop=NULL;
    Plan.headless=FALSE;
  } else if(s==EFI_SUCCESS) {
    for(UINTN k=0;k<Plan.survivor_count;k++)Plan.survivors[k].seen=FALSE;
  }
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
    BOOLEAN on_target=FALSE;
    s=Interface(handles[i],&GopGuid,(VOID **)&g);
    if(s!=EFI_SUCCESS||!g){if(s==EFI_SUCCESS)s=EFI_COMPROMISED_DATA;break;}
    if(!g->Mode||!g->Mode->Info||
       g->Mode->SizeOfInfo<sizeof(*g->Mode->Info)||
       !g->Mode->FrameBufferBase||!g->Mode->FrameBufferSize)
      continue;
    physical++;
    s=GopOnTarget(handles[i],&on_target);
    if(s!=EFI_SUCCESS)break;
    if(on_target) {
      target_count++;
      if(first) {
        if(Plan.gop){s=EFI_UNSUPPORTED;break;}
        for(UINTN k=0;k<Plan.survivor_count;k++)
          if(Plan.survivors[k].interface==g){s=EFI_COMPROMISED_DATA;break;}
        if(s!=EFI_SUCCESS)break;
        Plan.gop=handles[i];
        Plan.gop_interface=g;
        Plan.mode=*g->Mode;
        Plan.info=*g->Mode->Info;
      } else {
#ifdef GU_STOP_RELEASE_ONLY_BENCH
        BOOLEAN remapped=FALSE;
        if(Plan.reconnecting&&Plan.stage==0) {
          for(UINTN k=0;k<Plan.target_child_count;k++)
            if(Plan.target_children[k].is_gop&&
               Plan.target_children[k].live_handle==handles[i])
              remapped=TRUE;
          if(!remapped||!g->Mode->MaxMode||
             g->Mode->Mode>=g->Mode->MaxMode||
             !g->Mode->Info->HorizontalResolution||
             !g->Mode->Info->VerticalResolution||
             g->Mode->Info->PixelsPerScanLine<
               g->Mode->Info->HorizontalResolution) {
            s=EFI_NOT_READY;break;
          }
        } else
#endif
        if(Plan.stage>=3 || Plan.headless || handles[i]!=Plan.gop ||
           g!=Plan.gop_interface||
           !Equal(g->Mode,&Plan.mode,sizeof(Plan.mode))||
           !Equal(g->Mode->Info,&Plan.info,sizeof(Plan.info))) {
          s=EFI_NOT_READY;break;
        }
      }
    } else {
      EFI_DEVICE_PATH_PROTOCOL *live_path=NULL;
      s=Interface(handles[i],&PathGuid,(VOID **)&live_path);
      if(s!=EFI_SUCCESS||!live_path){s=EFI_NOT_READY;break;}
      if(first) {
        if(Plan.survivor_count==Plan.survivor_capacity){s=EFI_BAD_BUFFER_SIZE;break;}
        if(g==Plan.gop_interface){s=EFI_COMPROMISED_DATA;break;}
        for(UINTN k=0;k<Plan.survivor_count;k++)
          if(Plan.survivors[k].handle==handles[i]||Plan.survivors[k].interface==g){
            s=EFI_COMPROMISED_DATA;break;
          }
        if(s!=EFI_SUCCESS)break;
        UINTN path_bytes=(UINTN)GuDevicePathLength(
            (const unsigned char *)live_path,4096u);
        if(!path_bytes||path_bytes>MAX_SURVIVOR_PATH_BYTES){
          s=EFI_BAD_BUFFER_SIZE;break;
        }
        SURVIVOR_GOP *record=&Plan.survivors[Plan.survivor_count++];
        record->handle=handles[i];
        record->interface=g;
        record->mode=*g->Mode;
        record->info=*g->Mode->Info;
        record->path_bytes=path_bytes;
        memcpy(record->path,live_path,path_bytes);
      } else {
        UINTN k=0;
        while(k<Plan.survivor_count && Plan.survivors[k].handle!=handles[i])k++;
        if(k==Plan.survivor_count){s=EFI_NOT_READY;break;}
        SURVIVOR_GOP *record=&Plan.survivors[k];
        if(record->seen||g!=record->interface||
           !GuDevicePathUnchanged(record->path,
              (size_t)record->path_bytes,
              (const unsigned char *)live_path,4096u)||
           !Equal(g->Mode,&record->mode,sizeof(*g->Mode))||
           !Equal(g->Mode->Info,&record->info,sizeof(*g->Mode->Info))){
          s=EFI_NOT_READY;break;
        }
        record->seen=TRUE;
      }
    }
  }
  s=ReleaseObservation(b,handles,s);
  if(s!=EFI_SUCCESS)return s;
  if(first){
    Plan.headless=target_count==0;
    if(Plan.headless && !Plan.survivor_count)return EFI_NOT_READY;
    Plan.survivor_baseline_valid=TRUE;
    GuLog("TARGET_GOP=%zu SURVIVOR_GOPS=%zu HEADLESS=%u PHYSICAL=%zu\n",
          (size_t)target_count,(size_t)Plan.survivor_count,
          (unsigned)Plan.headless,(size_t)physical);
  } else {
    for(UINTN k=0;k<Plan.survivor_count;k++)
      if(!Plan.survivors[k].seen)return EFI_NOT_READY;
    if(Plan.headless && target_count)return EFI_NOT_READY;
    if(!Plan.headless && Plan.stage<3 && target_count!=1)return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}

/* This is a read-only identity screen, NOT a vendor Stop contract. Match
 * complete current-boot PCI controller paths, not a historic MSI board path
 * or a non-target GOP count. The NVIDIA peer is never an AMD survivor. */
static EFI_GUID GlobalVarGuid={
  0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}
};
static EFI_STATUS AmdConsoleVariable(
    const CHAR16 *name,const char *variable_label,
    const GU_DEVICE_PATH_DISPLAY_OWNER *owners,UINTN owner_count)
{
  UINT8 data[8u*192u];
  UINTN used=sizeof(data),at=0,start=0,paths=0,nodes=0;
  UINT32 attrs=0;
  EFI_RUNTIME_SERVICES *rt=Plan.table->RuntimeServices;
  if(!rt||!rt->GetVariable)return EFI_UNSUPPORTED;
  EFI_STATUS s=rt->GetVariable((CHAR16 *)name,&GlobalVarGuid,&attrs,&used,data);
  if(s!=EFI_SUCCESS||!used||used>sizeof(data))return EFI_NOT_READY;
  while(at+4<=used && nodes++<64) {
    UINTN n=(UINTN)data[at+2]+((UINTN)data[at+3]<<8);
    if(n<4||n>used-at)return EFI_COMPROMISED_DATA;
    if(data[at]==0x7f) {
      UINT8 subtype=data[at+1];
      if(n!=4||(subtype!=1&&subtype!=0xff)||
         at+n-start>192u||paths==8)return EFI_COMPROMISED_DATA;
      /* Normalize only this private GetVariable copy to END_ENTIRE. */
      data[at+1]=0xff;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
      int owner=GuDevicePathDisplayOwner(
          data+start,at+n-start,owners,(size_t)owner_count);
#else
      int owner=GuDevicePathAmdDisplayOwner(
          data+start,at+n-start,owners,(size_t)owner_count);
#endif
      UINTN matching_gops=0;
      for(UINTN k=0;k<Plan.survivor_count;k++) {
        EFI_DEVICE_PATH_PROTOCOL *gop_path=NULL;
        if(Interface(Plan.survivors[k].handle,&PathGuid,(VOID **)&gop_path)!=EFI_SUCCESS)
          return EFI_NOT_READY;
        int match=GuDevicePathHasPrefix(
            (const unsigned char *)gop_path,4096u,
            data+start,(size_t)(at+n-start));
        if(match<0)return EFI_COMPROMISED_DATA;
        if(match)matching_gops++;
      }
#ifdef GU_STOP_RELEASE_ONLY_BENCH
      /* Read-only diagnosis: a selected GPU can be the console owner even
       * when a second GOP is present. Observe that condition without
       * weakening the independent-survivor admission rule. */
      int target_pci_match=-1;
      if(owner!=1||matching_gops!=1) {
        EFI_DEVICE_PATH_PROTOCOL *target_path=NULL;
        if(Interface(Plan.pci,&PathGuid,(VOID **)&target_path)==EFI_SUCCESS&&target_path)
          target_pci_match=GuDevicePathHasPrefix(
              (const unsigned char *)target_path,4096u,
              data+start,(size_t)(at+n-start));
      }
#else
      (void)variable_label;
#endif
      data[at+1]=subtype;
      if(owner!=1||matching_gops!=1) {
#ifdef GU_STOP_RELEASE_ONLY_BENCH
        GuLog("STOP_RELEASE_CONSOLE=REFUSED VARIABLE=%s REASON=NOT_UNIQUE_NON_TARGET_SURVIVOR OWNER=%d GOP_MATCHES=%u TARGET_PCI_MATCH=%d\n",
              variable_label,owner,(unsigned)matching_gops,target_pci_match);
#endif
        return EFI_ACCESS_DENIED;
      }
      paths++;start=at+n;
      if(subtype==0xff)return start==used&&paths?EFI_SUCCESS:EFI_COMPROMISED_DATA;
    }
    at+=n;
  }
  return EFI_COMPROMISED_DATA;
}
static EFI_STATUS ReadOnlyAmdConsoleOwnership(VOID)
{
#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) && !defined(GU_STOP_RELEASE_ONLY_BENCH)
  GuLog("UNVERIFIED_BENCH_OVERRIDE GATE=INDEPENDENT_AMD_CONSOLE RESULT=BYPASSED\n");
  return EFI_SUCCESS;
#else
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(Plan.consoleless_target_gop) {
    GuLog("STOP_RELEASE_CONSOLE_REQUIREMENT=NONE TARGET_IS_ONLY_GOP=1 PREFLIGHT_CONSOLE_IO=0\n");
    return EFI_SUCCESS;
  }
#endif
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  EFI_HANDLE *handles=NULL;UINTN count=0,display_count=0;
#ifndef GU_STOP_RELEASE_ONLY_BENCH
  UINTN amd_count=0;
#endif
  GU_DEVICE_PATH_DISPLAY_OWNER *owners=NULL;
  EFI_STATUS s;
  if(!Plan.survivor_baseline_valid||!Plan.survivor_count) {
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    GuLog("STOP_RELEASE_CONSOLE=REFUSED REASON=NO_INDEPENDENT_SURVIVOR_GOP TARGET_GOP=%u SURVIVOR_GOPS=%u\n",
          (unsigned)(Plan.gop!=NULL),(unsigned)Plan.survivor_count);
#endif
    return EFI_NOT_READY;
  }
  if(!b||!b->LocateHandleBuffer||!b->AllocatePool||!b->FreePool)
    return EFI_NOT_READY;
  s=b->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  s=ObservationOutput(s,(VOID **)&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>GU_DISPLAY_OWNER_MAX))s=EFI_BAD_BUFFER_SIZE;
  if(s==EFI_SUCCESS) {
    s=b->AllocatePool(EfiLoaderData,count*sizeof(*owners),(VOID **)&owners);
    s=ObservationOutput(s,(VOID **)&owners);
    if(s==EFI_SUCCESS&&!owners)s=EFI_OUT_OF_RESOURCES;
  }
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_PCI_IO_PROTOCOL *io=NULL;
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;
    UINT32 h[3],again[3];
    UINTN seg=0,bus=0,dev=0,fun=0;
    s=Interface(handles[i],&PciGuid,(VOID **)&io);
    if(s!=EFI_SUCCESS||!io||!io->Pci.Read||!io->GetLocation) {
      s=EFI_NOT_READY;break;
    }
    s=io->GetLocation(io,&seg,&bus,&dev,&fun);
    if(s!=EFI_SUCCESS||seg||bus>255u||dev>31u||fun>7u) {
      s=EFI_NOT_READY;break;
    }
    s=io->Pci.Read(io,EfiPciIoWidthUint32,0,3,h);
    if(s==EFI_SUCCESS)s=io->Pci.Read(io,EfiPciIoWidthUint32,0,3,again);
    if(s!=EFI_SUCCESS||h[0]!=again[0]||h[2]!=again[2]) {
      s=EFI_NOT_READY;break;
    }
    UINT16 cls=(UINT16)((h[2]>>16)&0xffffu);
    if((cls&0xff00u)!=0x0300u)continue;
    if(display_count==count){s=EFI_BAD_BUFFER_SIZE;break;}
    s=Interface(handles[i],&PathGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS||!p){s=EFI_NOT_READY;break;}
    owners[display_count++]=(GU_DEVICE_PATH_DISPLAY_OWNER){
        (const unsigned char *)p,4096u,(UINT16)(h[0]&0xffffu),cls};
#ifndef GU_STOP_RELEASE_ONLY_BENCH
    if((h[0]&0xffffu)==0x1002u)amd_count++;
#endif
  }
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(s==EFI_SUCCESS&&!display_count)s=EFI_ACCESS_DENIED;
#else
  if(s==EFI_SUCCESS&&(!amd_count||!display_count))s=EFI_ACCESS_DENIED;
#endif
  for(UINTN k=0;s==EFI_SUCCESS&&k<Plan.survivor_count;k++) {
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;
    s=Interface(Plan.survivors[k].handle,&PathGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS)break;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    if(GuDevicePathDisplayOwner(
          (const unsigned char *)p,4096u,owners,(size_t)display_count)!=1)
#else
    if(GuDevicePathAmdDisplayOwner(
          (const unsigned char *)p,4096u,owners,(size_t)display_count)!=1)
#endif
      s=EFI_ACCESS_DENIED;
  }
  if(s==EFI_SUCCESS)s=AmdConsoleVariable(L"ConOut","ConOut",owners,display_count);
  if(s==EFI_SUCCESS)s=AmdConsoleVariable(L"ErrOut","ErrOut",owners,display_count);
  s=ReleaseObservation(b,owners,s);
  s=ReleaseObservation(b,handles,s);
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(s==EFI_SUCCESS)
    GuLog("STOP_RELEASE_CONSOLE=INDEPENDENT_SURVIVOR_VERIFIED POLICY=NON_TARGET_DISPLAY GOPS=%u\n",
          (unsigned)Plan.survivor_count);
#endif
  return s;
#endif
}
/* Require graph evidence that the currently bound text console actually
 * consumes only verified non-target survivor GOPs. The unlock/handoff policy
 * requires AMD ownership; Stop/release-only also admits a distinct NVIDIA
 * survivor, but never the selected target's GOP. */
static EFI_STATUS ReadOnlyAmdConsoleGraph(const GRAPH *graph)
{
#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) && !defined(GU_STOP_RELEASE_ONLY_BENCH)
  (void)graph;
  GuLog("UNVERIFIED_BENCH_OVERRIDE GATE=LIVE_AMD_TEXTOUT_GRAPH RESULT=BYPASSED\n");
  return EFI_SUCCESS;
#else
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(Plan.consoleless_target_gop) {
    (void)graph;
    GuLog("STOP_RELEASE_TEXT_GRAPH=NOT_REQUIRED TARGET_IS_ONLY_GOP=1 PREFLIGHT_CONSOLE_IO=0\n");
    return EFI_SUCCESS;
  }
#endif
  BOOLEAN out=FALSE,err=FALSE;
  if(!graph||!Plan.console||!Plan.error)return EFI_NOT_READY;
  for(UINTN i=0;i<graph->count;i++) {
    const EDGE *e=&graph->edges[i];
    if(!Guid(&e->protocol,&TextGuid)||
       e->attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||
       (e->controller!=Plan.console&&e->controller!=Plan.error))
      continue;
    BOOLEAN survivor=FALSE;
    for(UINTN k=0;k<Plan.survivor_count;k++)
      if(e->handle==Plan.survivors[k].handle)survivor=TRUE;
    if(!survivor||e->count!=1)return EFI_ACCESS_DENIED;
    if(e->controller==Plan.console)out=TRUE;
    if(e->controller==Plan.error)err=TRUE;
  }
  return out&&err?EFI_SUCCESS:EFI_NOT_READY;
#endif
}

static EFI_STATUS ReadConfigMode(UINT32 c[64],BOOLEAN permit_initial_master) {
  EFI_PCI_IO_PROTOCOL *io=NULL;UINTN seg,bus,dev,fun;
  EFI_STATUS s=Interface(Plan.pci,&PciGuid,(VOID **)&io);
  if(s!=EFI_SUCCESS||io!=Gu.pci||!io->Pci.Read||!io->GetLocation)return EFI_NOT_READY;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s!=EFI_SUCCESS||seg||bus>255||dev||fun)return EFI_UNSUPPORTED;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s==EFI_SUCCESS&&(!GuTargetConfig(c,seg,bus,dev,fun)||
     (!permit_initial_master&&(c[1]&4))))s=EFI_UNSUPPORTED;
  return s;
}
static EFI_STATUS ReadConfig(UINT32 c[64]) {return ReadConfigMode(c,FALSE);}
#ifdef GU_STOP_RELEASE_ONLY_BENCH
/* A successful ConnectController is not proof that the same PCI function
 * was recognized. Read the current BDF and immutable device/class/subsystem
 * identity without a new exclusive claim. PCI command/status is dynamic. */
static EFI_STATUS ReattachedTargetPciRecognized(VOID) {
  UINT32 c[64]={0};
  EFI_STATUS s=ReadConfigMode(c,TRUE);
  if(s!=EFI_SUCCESS) {
    GuLog("MSI reattach PCI identity refused: READ_STATUS=%#llx\n",
          (unsigned long long)s);
    return s;
  }
  if(c[0]!=Plan.config[0]||
     (c[2]&0xffffff00U)!=(Plan.config[2]&0xffffff00U)||
     c[0x2cU/4U]!=Plan.config[0x2cU/4U]) {
    GuLog("MSI reattach PCI identity refused: BEFORE=%#x AFTER=%#x "
          "CLASS_BEFORE=%#x CLASS_AFTER=%#x SUB_BEFORE=%#x SUB_AFTER=%#x\n",
          Plan.config[0],c[0],Plan.config[2],c[2],
          Plan.config[0x2cU/4U],c[0x2cU/4U]);
    return EFI_COMPROMISED_DATA;
  }
  GuLog("MSI reattach target PCI recognized: DEVICE=%#x "
        "CLASS=%#x SUB=%#x SAME_FUNCTION=1\n",
        c[0],c[2],c[0x2cU/4U]);
  return EFI_SUCCESS;
}
#endif
/* RUN0002 did not identify PCI drift: do not assume it caused the stop.
 * Retain v0.5.0's narrow rule: only preexisting CED/URD may clear after FLR. */
static BOOLEAN ConfigStable(const UINT32 c[64]) {
  for(UINTN i=0;i<64;i++) {
    UINT32 drift=c[i]^Plan.config[i];
    if(!drift)continue;
    BOOLEAN allowed=Gu.flrs==1&&i==0x80/4&&
      !(c[i]&~Plan.config[i])&&!(drift&~0x00090000u);
    /* Only a transition from Bus Master enabled to disabled is expected
     * during pre-FLR quiescence. Never allow a new DMA enable or another
     * PCI COMMAND change under this exception. */
    if(i==1&&Gu.flrs<=1&&drift==4u&&
       GuPciMasterClearOnly(Plan.config[i],c[i]))allowed=TRUE;
    if(!allowed) {
      NOTE("PCI config drift refused: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
        (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
      return FALSE;
    }
    NOTE("PCI status drift tolerated: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
      (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
  }
  return TRUE;
}
/* The Bus Master enable is the only DMA control this adapter owns. Clearing it
 * stops new transactions; it says nothing about the ones already posted, and
 * this image cannot consult an IOMMU, read back an engine queue or trace a bus.
 * The one standard observable left is the PCIe Device Status "Transaction
 * Pending" bit, which is polled a bounded number of times: a transaction that
 * never drains keeps the caller's buffers retained instead of claiming
 * quiescence. The bound is a count of observations, not a measured budget. */
#define GU_TX_PENDING_POLLS 5U
static BOOLEAN TxPending(const UINT32 c[64],UINT16 pcie) {
  if(!pcie)return FALSE;
  UINTN off=(UINTN)pcie+0x0aU;
  return ((c[off/4U]>>((off&3U)*8U))&0x20U)!=0;
}
/* Device Status is read-only, and its Transaction Pending bit is expected to
 * move while the card drains. It is therefore exempt from the post-Stop drift
 * rule and checked explicitly instead; no control bit is exempt, and every
 * other byte of the snapshot must still match what preflight captured. */
static BOOLEAN ConfigStablePostStop(const UINT32 c[64]) {
  if(!Plan.pcie_cap)return ConfigStable(c);
  UINT32 copy[64];
  UINTN off=(UINTN)Plan.pcie_cap+0x0aU;
  memcpy(copy,c,sizeof(copy));
  copy[off/4U]&=~(0x20U<<((off&3U)*8U));
  return ConfigStable(copy);
}
static EFI_STATUS WaitTxPendingClear(UINTN *polls) {
  UINT32 c[64];
  EFI_STATUS s;
  *polls=0;
  if(!Plan.pcie_cap) {
    NOTE("POST_STOP_QUIESCENCE=BUS_MASTER_ONLY TX_PENDING=UNOBSERVED POLLS=0 IOMMU=UNMANAGED QUEUE_READBACK=0 BUS_TRACE=0\n");
    return EFI_SUCCESS;
  }
  for(UINTN i=0;i<GU_TX_PENDING_POLLS;i++) {
    ++*polls;
    s=ReadConfig(c);
    if(s!=EFI_SUCCESS)return s;
    if(!ConfigStablePostStop(c))return EFI_COMPROMISED_DATA;
    if(!TxPending(c,Plan.pcie_cap)) {
      NOTE("POST_STOP_QUIESCENCE=BUS_MASTER_ONLY TX_PENDING_CLEAR=1 POLLS=%u IOMMU=UNMANAGED QUEUE_READBACK=0 BUS_TRACE=0\n",(unsigned)*polls);
      return EFI_SUCCESS;
    }
    if(i+1U>=GU_TX_PENDING_POLLS)break;
    if(!Plan.table||!Plan.table->BootServices||!Plan.table->BootServices->Stall)break;
    Plan.table->BootServices->Stall(1000U);
  }
  NOTE("POST_STOP_QUIESCENCE=REFUSED REASON=TX_PENDING_STUCK POLLS=%u IOMMU=UNMANAGED QUEUE_READBACK=0 BUS_TRACE=0\n",(unsigned)*polls);
  return EFI_NOT_READY;
}
static EFI_STATUS Bar(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;UINT64 support=0;
  if(!Gu.pci->GetBarAttributes)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,(VOID **)&d);
  s=ObservationOutput(s,(VOID **)&d);
  if(s==EFI_SUCCESS&&(!d||d->Desc!=0x8a||d->Len!=43||d->ResType||
     d->AddrRangeMin!=Gu.bar_base||
     d->AddrRangeMax!=(Gu.bar_base+Gu.bar_size-1)||
     d->AddrTranslationOffset||d->AddrLen!=Gu.bar_size||
     Gu.bar_size<GU_BAR_MIN_BYTES||
     Gu.bar_size>~(UINT64)0-Gu.bar_base))s=EFI_UNSUPPORTED;
  return ReleaseObservation(Plan.table->BootServices,d,s);
}
/* The immutable original graph is the child inventory. An idle output
 * must belong to this controller and have exactly one relationship.
 * Never classify a protected peer, console or other agent as removable. */
static EFI_STATUS ClassifyIdleOutput(EDGE *e) {
  /* RUN0039 reaches this check for a GA102 child that is not Plan.gop.
   * An EFI_ACCESS_DENIED alone cannot distinguish a protected console
   * handle, shared protocol consumer, or unrelated output. Describe the
   * exact rejected original relationship, but do not waive its protection. */
  if(!e) {
    GuLog("STOP_RELEASE_IDLE_REFUSED REASON=NULL_EDGE\n");
    return EFI_ACCESS_DENIED;
  }
  EFI_HANDLE h=e->controller;
  if(e->handle!=Plan.pci||!Guid(&e->protocol,&PathGuid)||
     e->attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||
     !Plan.nv_agent||e->agent!=Plan.nv_agent||e->count!=1||!h) {
    GuLog("STOP_RELEASE_IDLE_REFUSED REASON=NOT_EXACT_TARGET_CHILD CHILD=%#llx "
          "PARENT=%#llx AGENT=%#llx NVIDIA_AGENT=%#llx ATTR=%#x COUNT=%u\n",
          (unsigned long long)(UINTN)h,(unsigned long long)(UINTN)e->handle,
          (unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)Plan.nv_agent,
          e->attributes,e->count);
    return EFI_ACCESS_DENIED;
  }
  if(ProtectedHandle(h)) {
    GuLog("STOP_RELEASE_IDLE_REFUSED REASON=PROTECTED_HANDLE CHILD=%#llx "
          "CONSOLE=%u ERROR=%u TARGET_GOP=%u PCI=%u PEER=%u\n",
          (unsigned long long)(UINTN)h,(unsigned)(h==Plan.console),
          (unsigned)(h==Plan.error),(unsigned)(h==Plan.gop),
          (unsigned)(h==Plan.pci),(unsigned)GuIsPreparedPeerHandle(h));
    return EFI_ACCESS_DENIED;
  }
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  UINTN edid_index=Original.count;
#endif
  for(UINTN k=0;k<Original.count;k++) {
    const EDGE *other=&Original.edges[k];
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    /* RUN0040: child 0x74ed2918 had an EDID Discovered BY_DRIVER open
     * with handle==controller==child and count 1. This is a normal UEFI
     * video-output dependent, not a second parent or GPU owner. Require
     * the exact standard GUID, a distinct agent, and no second consumer.
     * The complete device path and driver image are validated below. */
    if(other!=e&&other->handle==h&&other->controller==h&&
       Guid(&other->protocol,&EdidDiscoveredGuid)&&
       other->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&other->count==1&&
       other->agent&&other->agent!=h&&other->agent!=Plan.self&&
       !ProtectedHandle(other->agent)&&edid_index==Original.count) {
      edid_index=k;
      continue;
    }
#endif
    if(other!=e&&(other->handle==h||other->agent==h||other->controller==h)) {
      GuLog("STOP_RELEASE_IDLE_REFUSED REASON=SHARED_OR_CONSUMED_CHILD CHILD=%#llx "
            "EDGE=%zu HANDLE=%#llx AGENT=%#llx CONTROLLER=%#llx "
            "GUID=%#x ATTR=%#x COUNT=%u REL_HANDLE=%u REL_AGENT=%u REL_CONTROLLER=%u\n",
            (unsigned long long)(UINTN)h,(size_t)k,
            (unsigned long long)(UINTN)other->handle,
            (unsigned long long)(UINTN)other->agent,
            (unsigned long long)(UINTN)other->controller,
            other->protocol.Data1,other->attributes,other->count,
            (unsigned)(other->handle==h),(unsigned)(other->agent==h),
            (unsigned)(other->controller==h));
      return EFI_ACCESS_DENIED;
    }
  }
  BOOLEAN on_target=FALSE;
  EFI_STATUS s=GopOnTarget(h,&on_target);
  if(s!=EFI_SUCCESS||!on_target) {
    GuLog("STOP_RELEASE_IDLE_REFUSED REASON=TARGET_PATH_NOT_PROVEN "
          "CHILD=%#llx STATUS=%#llx ON_TARGET=%u\n",
          (unsigned long long)(UINTN)h,(unsigned long long)s,(unsigned)on_target);
    return s==EFI_SUCCESS?EFI_ACCESS_DENIED:s;
  }
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(edid_index<Original.count) {
    EDGE *dependent=&Original.edges[edid_index];
    /* Even a standard EDID driver must have a real Stop() implementation.
     * Reject an agent that also manages a prepared peer PCI controller. */
    for(UINTN k=0;k<Original.count;k++) {
      const EDGE *peer=&Original.edges[k];
      if(peer->agent==dependent->agent&&
         (GuIsPreparedPeerHandle(peer->handle)||
          GuIsPreparedPeerHandle(peer->controller))) {
        GuLog("STOP_RELEASE_IDLE_REFUSED REASON=EDID_AGENT_OWNS_PEER "
              "CHILD=%#llx AGENT=%#llx EDGE=%zu\n",
              (unsigned long long)(UINTN)h,
              (unsigned long long)(UINTN)dependent->agent,(size_t)k);
        return EFI_ACCESS_DENIED;
      }
    }
    s=ValidateDriverAgent(dependent->agent,3U);
    if(s!=EFI_SUCCESS) {
      GuLog("STOP_RELEASE_IDLE_REFUSED REASON=EDID_DRIVER_NOT_VERIFIED "
            "CHILD=%#llx AGENT=%#llx STATUS=%#llx\n",
            (unsigned long long)(UINTN)h,
            (unsigned long long)(UINTN)dependent->agent,
            (unsigned long long)s);
      return s;
    }
    /* Mark both exact baseline edges only after every ownership check.
     * Stage-0 reattachment still requires the same original relationships. */
    dependent->idle_output=TRUE;
    GuLog("STOP_RELEASE_IDLE_EDID=ACCEPTED CHILD=%#llx AGENT=%#llx "
          "EDGE=%zu GUID=EDID_DISCOVERED ATTR=BY_DRIVER "
          "ON_TARGET=1 PEER_AGENT=0\n",
          (unsigned long long)(UINTN)h,
          (unsigned long long)(UINTN)dependent->agent,(size_t)edid_index);
  }
#endif
  GuLog("STOP_RELEASE_IDLE_ACCEPTED CHILD=%#llx RELATIONSHIP=%s\n",
        (unsigned long long)(UINTN)h,
#ifdef GU_STOP_RELEASE_ONLY_BENCH
        edid_index<Original.count?"EXACT_TARGET_EDID_DRIVER":"UNIQUE_TARGET_ONLY"
#else
        "UNIQUE_TARGET_ONLY"
#endif
       );
  e->idle_output=TRUE;
  return EFI_SUCCESS;
}
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0,child=0,idle=0,console_child=0,text=0,virtual=0,gfx=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci) {
      if(Guid(&e->protocol,&PciGuid)&&(e->attributes&EFI_OPEN_PROTOCOL_BY_DRIVER)&&e->controller==Plan.pci){
        Plan.nv_agent=e->agent;
        pci++;
      }
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8&&e->controller==Plan.gop){
        child++;
      }
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8){
        if(Plan.consoleless_target_gop&&
           (e->controller==Plan.console||e->controller==Plan.error))
          console_child++;
        else idle++;
      }
    } else if(e->handle==Plan.gop) {
      if(Guid(&e->protocol,&TextGuid)&&(e->attributes&EFI_OPEN_PROTOCOL_BY_DRIVER)&&e->controller==Plan.gop){
        Plan.out_agent=e->agent;
        text++;
      }
      else if(Guid(&e->protocol,&TextGuid)&&e->attributes==8&&e->controller==Plan.console){
        virtual++;
      }
      else if(Guid(&e->protocol,&GopGuid)&&(e->attributes&EFI_OPEN_PROTOCOL_BY_DRIVER)&&e->controller==Plan.gop){
        Plan.gfx_agent=e->agent;
        gfx++;
      }
    }
  }
  if(Plan.headless) {
    /* Exactly one original NVIDIA driver owns this exact target PCI handle.
     * No NVIDIA target GOP, display output, or child controller may remain.
     * Other NVIDIA and AMD endpoints are tracked as protected survivors. */
    if(pci!=1||!Plan.nv_agent||Plan.nv_agent==Plan.self||
       Plan.nv_agent==Plan.console||Plan.nv_agent==Plan.error||
       Plan.gop||child||idle||text||gfx||virtual)return EFI_ACCESS_DENIED;
    for(UINTN i=0;i<Original.count;i++){
      const EDGE *e=&Original.edges[i];
      if(e->handle==Plan.pci&&
         (e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||
          (Guid(&e->protocol,&PathGuid)&&e->attributes==8)))
        return EFI_ACCESS_DENIED;
    }
    GuLog("HEADLESS_STAGE3=ELIGIBLE TARGET_DRIVER=%#llx "
          "NON_TARGET_GOPS=%zu BEFORE_STOP=1\n",
          (unsigned long long)(UINTN)Plan.nv_agent,(size_t)Plan.survivor_count);
    return Driver(2);
  }
  if(pci<1||(child<1&&idle<1&&console_child<1)||text<1||gfx<1||!Plan.nv_agent||!Plan.out_agent||!Plan.gfx_agent||
     Plan.nv_agent==Plan.out_agent||Plan.out_agent==Plan.gfx_agent||Plan.gfx_agent==Plan.nv_agent) {
    GuLog("Consumers check: pci=%zu child=%zu idle=%zu console_child=%zu text=%zu virtual=%zu gfx=%zu\n",
      pci, child, idle, console_child, text, virtual, gfx);
    return EFI_ACCESS_DENIED;
  }
  /* A sole-GOP target may own the configured firmware text console. Its
   * controller relationship is target-owned, not an independent idle output;
   * keep the handle protected but let the target Stop remove that driver edge. */
  if(console_child) {
    for(UINTN i=0;i<Original.count;i++) {
      const EDGE *e=&Original.edges[i];
      if(e->handle==Plan.pci&&Guid(&e->protocol,&PathGuid)&&e->attributes==8&&
         (e->controller==Plan.console||e->controller==Plan.error)&&
         (e->agent!=Plan.nv_agent||e->count!=1)) {
        GuLog("Consumers refused target-console child: AGENT=%#llx TARGET_DRIVER=%#llx COUNT=%u\n",
              (unsigned long long)(UINTN)e->agent,
              (unsigned long long)(UINTN)Plan.nv_agent,(unsigned)e->count);
        return EFI_ACCESS_DENIED;
      }
    }
  }
  /* The driver agent is now known, irrespective of enumeration order.
   * The headless path above still refuses every child. */
  if(idle&&pci!=1) {
    GuLog("Consumers refused idle outputs with ambiguous target driver count: PCI_DRIVERS=%u IDLE_OUTPUTS=%u\n",
          (unsigned)pci,(unsigned)idle);
    return EFI_ACCESS_DENIED;
  }
  for(UINTN i=0;i<Original.count;i++) {
    EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci&&Guid(&e->protocol,&PathGuid)&&
       e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->controller!=Plan.gop&&
       !(Plan.consoleless_target_gop&&
         (e->controller==Plan.console||e->controller==Plan.error))) {
      EFI_STATUS s=ClassifyIdleOutput(e);
      if(s!=EFI_SUCCESS) {
        GuLog("Consumers refused idle output: CONTROLLER=%#llx STATUS=%#llx\n",
              (unsigned long long)(UINTN)e->controller,(unsigned long long)s);
        return s;
      }
    }
  }
  for(UINTN i=0;i<3;i++){
    EFI_STATUS s=Driver(i);
    if(s!=EFI_SUCCESS){
      GuLog("Consumers: Driver(%zu) failed: %#llx\n",(size_t)i,(unsigned long long)s);
      return s;
    }
  }
  return EFI_SUCCESS;
}
/* Stop-only reattachment identity: stable full PCI child paths, not the
 * firmware's allocation addresses. All GPU binding roles remain protected. */
#ifdef GU_STOP_RELEASE_ONLY_BENCH
static EFI_STATUS TargetChildBaseline(VOID) {
  if(Plan.target_children||Plan.target_child_count||Plan.target_child_capacity)
    return EFI_ALREADY_STARTED;
  UINTN count=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci&&e->agent==Plan.nv_agent&&
       Guid(&e->protocol,&PathGuid)&&
       e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->count==1&&
       e->controller&&e->controller!=Plan.console&&e->controller!=Plan.error)
      count++;
  }
  if(count>TARGET_CHILD_LIMIT)return EFI_BAD_BUFFER_SIZE;
  if(!count)return Plan.headless?EFI_SUCCESS:EFI_NOT_READY;
  TARGET_CHILD *saved=GuAlloc(count*sizeof(*saved));
  if(!saved)return Gu.cpu_pool_failure_status!=EFI_SUCCESS?
    Gu.cpu_pool_failure_status:EFI_OUT_OF_RESOURCES;
  memset(saved,0,count*sizeof(*saved));
  Plan.target_children=saved;Plan.target_child_capacity=count;
  UINTN gops=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle!=Plan.pci||e->agent!=Plan.nv_agent||
       !Guid(&e->protocol,&PathGuid)||
       e->attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||e->count!=1||
       !e->controller||e->controller==Plan.console||
       e->controller==Plan.error)continue;
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;BOOLEAN on_target=FALSE;
    EFI_STATUS s=GopOnTarget(e->controller,&on_target);
    if(s!=EFI_SUCCESS||!on_target)return s==EFI_SUCCESS?EFI_ACCESS_DENIED:s;
    s=Interface(e->controller,&PathGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS||!p)return s==EFI_SUCCESS?EFI_NOT_READY:s;
    UINTN n=(UINTN)GuDevicePathLength((const unsigned char *)p,4096u);
    if(!n||n>MAX_SURVIVOR_PATH_BYTES)return EFI_BAD_BUFFER_SIZE;
    for(UINTN k=0;k<Plan.target_child_count;k++)
      if(saved[k].old_handle==e->controller||
         (saved[k].path_bytes==n&&Equal(saved[k].path,p,n)))
        return EFI_COMPROMISED_DATA;
    TARGET_CHILD *entry=&saved[Plan.target_child_count++];
    entry->old_handle=e->controller;entry->path_bytes=n;
    entry->is_gop=e->controller==Plan.gop;
    memcpy(entry->path,p,n);
    if(entry->is_gop)gops++;
  }
  if(Plan.target_child_count!=count||
     (!!gops!=!!Plan.gop)||gops>1)return EFI_NOT_READY;
  GuLog("MSI target child baseline: count=%zu target_gops=%zu paths=COMPLETE\n",
        (size_t)count,(size_t)gops);
  return EFI_SUCCESS;
}
static EFI_HANDLE ReattachedChild(EFI_HANDLE previous) {
  for(UINTN i=0;i<Plan.target_child_count;i++)
    if(Plan.target_children[i].old_handle==previous)
      return Plan.target_children[i].live_handle;
  return previous;
}
static EFI_STATUS ReconcileTargetChildren(VOID) {
  if(!Plan.reconnecting||Plan.stage!=0)return EFI_NOT_READY;
  for(UINTN i=0;i<Plan.target_child_count;i++)
    Plan.target_children[i].live_handle=NULL;
  for(UINTN i=0;i<Current.count;i++) {
    const EDGE *e=&Current.edges[i];
    if(e->handle!=Plan.pci||!Guid(&e->protocol,&PathGuid)||
       e->attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||
       e->count!=1||!e->controller)continue;
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;BOOLEAN on_target=FALSE;
    EFI_STATUS s=GopOnTarget(e->controller,&on_target);
    if(s!=EFI_SUCCESS)return s;
    if(!on_target)continue;
    s=Interface(e->controller,&PathGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS||!p)return s==EFI_SUCCESS?EFI_NOT_READY:s;
    UINTN found=Plan.target_child_count;
    for(UINTN k=0;k<Plan.target_child_count;k++) {
      const TARGET_CHILD *r=&Plan.target_children[k];
      if(GuDevicePathUnchanged(r->path,(size_t)r->path_bytes,
             (const unsigned char *)p,4096u)) {
        if(found!=Plan.target_child_count)return EFI_COMPROMISED_DATA;
        found=k;
      }
    }
    if(found==Plan.target_child_count) {
      GuLog("MSI reattach child refused: REASON=UNKNOWN_TARGET_PATH "
            "LIVE=%#llx AGENT=%#llx\n",
            (unsigned long long)(UINTN)e->controller,
            (unsigned long long)(UINTN)e->agent);
      return EFI_ACCESS_DENIED;
    }
    TARGET_CHILD *r=&Plan.target_children[found];
    if(e->agent!=Plan.nv_agent||r->live_handle) {
      GuLog("MSI reattach child refused: REASON=AGENT_OR_DUPLICATE "
            "OLD=%#llx LIVE=%#llx OLD_AGENT=%#llx NEW_AGENT=%#llx\n",
            (unsigned long long)(UINTN)r->old_handle,
            (unsigned long long)(UINTN)e->controller,
            (unsigned long long)(UINTN)Plan.nv_agent,
            (unsigned long long)(UINTN)e->agent);
      return EFI_ACCESS_DENIED;
    }
    for(UINTN k=0;k<Plan.target_child_count;k++)
      if(Plan.target_children[k].live_handle==e->controller)
        return EFI_COMPROMISED_DATA;
    r->live_handle=e->controller;
  }
  for(UINTN k=0;k<Plan.target_child_count;k++) {
    const TARGET_CHILD *r=&Plan.target_children[k];
    if(!r->live_handle) {
      GuLog("MSI reattach child missing: OLD=%#llx GOP=%u PATH_BYTES=%zu\n",
            (unsigned long long)(UINTN)r->old_handle,
            (unsigned)r->is_gop,(size_t)r->path_bytes);
      return EFI_NOT_READY;
    }
    GuLog("MSI reattach child path matched: OLD=%#llx LIVE=%#llx "
          "GOP=%u FULL_PATH=1 SAME_HANDLE=%u\n",
          (unsigned long long)(UINTN)r->old_handle,
          (unsigned long long)(UINTN)r->live_handle,
          (unsigned)r->is_gop,(unsigned)(r->old_handle==r->live_handle));
  }
  return EFI_SUCCESS;
}
static BOOLEAN SameReattachedEdge(const EDGE *a,const EDGE *b) {
  return b->handle==ReattachedChild(a->handle)&&
    b->controller==ReattachedChild(a->controller)&&
    a->agent==b->agent&&a->attributes==b->attributes&&
    a->count==b->count&&Guid(&a->protocol,&b->protocol);
}
static BOOLEAN KnownReattachedEdge(const EDGE *e) {
  for(UINTN i=0;i<Original.count;i++)
    if(SameReattachedEdge(&Original.edges[i],e))return TRUE;
  return FALSE;
}
static BOOLEAN ReattachedGraphExpected(VOID) {
  if(!Plan.reconnecting||Plan.stage!=0)return FALSE;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *o=&Original.edges[i];
    if(!ProtectedEdge(o))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)
      if(SameReattachedEdge(o,&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i))) {
      NoteEdge("reattach-missing-or-duplicate",o);
      return FALSE;
    }
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownReattachedEdge(e)&&ProtectedEdge(e)&&!PointerAddition(e)){
      NoteEdge("reattach-foreign",e);return FALSE;
    }
    for(UINTN k=j+1;k<Current.count;k++)
      if(SameEdge(e,&Current.edges[k])){NoteEdge("reattach-duplicate",e);return FALSE;}
  }
  return TRUE;
}
static EFI_STATUS ReattachedDriversVerified(VOID) {
  for(UINTN role=0;role<3;role++) {
    EFI_STATUS s=Driver(role);
    if(s!=EFI_SUCCESS)return s;
  }
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(!e->idle_output||!Guid(&e->protocol,&EdidDiscoveredGuid))continue;
    EFI_STATUS s=ValidateDriverAgent(e->agent,3U);
    if(s!=EFI_SUCCESS)return s;
  }
  GuLog("MSI reattach drivers verified: NVIDIA=1 GRAPHICS=1 TEXT=1 EDID=1\n");
  return EFI_SUCCESS;
}
#endif
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci) {
  if(!ReleaseOwnerMatches())return EFI_NOT_READY;
  /* A failed preflight may have observed or modified driver context.
   * Do not erase its saved ownership graph on a later target attempt. */
  if(Plan.table||Plan.mutated||Plan.ready||Plan.claimed)return EFI_ALREADY_STARTED;
  memset(&Plan,0,sizeof(Plan));Plan.table=t;Plan.self=self;Plan.pci=pci;
  if(!t||!t->BootServices||!self||!pci||Gu.handle!=pci||Gu.image!=self||Gu.bs!=t->BootServices||
#ifdef GU_STOP_RELEASE_ONLY_BENCH
     !Gu.stop_release_ready||
#else
     !Gu.profile_verified||!Gu.prom_verified||
#endif
     Gu.started||Gu.owned||Gu.armed)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=t->BootServices;
  if(!b->DisconnectController||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL||
     !b->ProtocolsPerHandle||!b->OpenProtocolInformation||!b->LocateHandleBuffer||!b->HandleProtocol||
     !b->AllocatePool||!b->FreePool)return EFI_UNSUPPORTED;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(!b->ConnectController)return EFI_UNSUPPORTED;
#endif
  EFI_STATUS s=EFI_SUCCESS;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  /* Observe GOP topology before deciding whether this target's own output is
   * the only console route. That mode must never touch UEFI console protocols. */
  s=GuVerifyActivePeers(t);
  if(s==EFI_SUCCESS)s=Gops(TRUE);
  if(s!=EFI_SUCCESS) {
    GuLog("Preflight failed at peer/GOP observation: %#llx\n",(unsigned long long)s);
    return s;
  }
  if(Plan.gop&&!Plan.survivor_count) {
    Plan.consoleless_target_gop=TRUE;
    StopFileOnlyOutput=TRUE;
    GuLog("STOP_RELEASE_CONSOLE_REQUIREMENT=NONE TARGET_IS_ONLY_GOP=1 "
          "TARGET_GOP=1 SURVIVOR_GOPS=0 PREFLIGHT_CONSOLE_IO=0 FILE_LOG=1\n");
  }
#endif
  /* Retain system-table handles for graph protection even when console
   * admission is waived. Reading these fields performs no console I/O. */
  Plan.console=t->ConsoleOutHandle;Plan.input=t->ConsoleInHandle;Plan.error=t->StandardErrorHandle;
  Plan.text=t->ConOut;Plan.err=t->StdErr;Plan.keys=t->ConIn;
  if(!Plan.consoleless_target_gop) {
    if(!Plan.console||!Plan.input||!Plan.error||!Plan.text||!Plan.err||!Plan.keys||
       !Plan.keys->ReadKeyStroke||!Plan.keys->Reset||!Plan.keys->WaitForKey)
      return EFI_UNSUPPORTED;
    Plan.key_reset=(UINTN)Plan.keys->Reset;Plan.key_read=(UINTN)Plan.keys->ReadKeyStroke;
    Plan.key_wait=Plan.keys->WaitForKey;
  }
#ifndef GU_STOP_RELEASE_ONLY_BENCH
  if(!pci||!GuAd104ColdEntry())return EFI_NOT_READY;
#else
  if(!pci||!Gu.stop_release_ready)return EFI_NOT_READY;
#endif
#ifndef GU_STOP_RELEASE_ONLY_BENCH
  s=GuVerifyActivePeers(t);
  if(s==EFI_SUCCESS)s=Gops(TRUE);
  if(s!=EFI_SUCCESS)GuLog("Preflight failed at peer/GOP observation: %#llx\n",(unsigned long long)s);
#endif
  if(s==EFI_SUCCESS&&!Plan.consoleless_target_gop) {
    s=Alive();
    if(s!=EFI_SUCCESS)GuLog("Preflight failed at Alive(): %#llx\n",(unsigned long long)s);
  }
  if(s==EFI_SUCCESS) { s=Bar(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at Bar(): %#llx\n", (unsigned long long)s); }
  if(s==EFI_SUCCESS) { s=ReadConfigMode(Plan.config,TRUE); if(s!=EFI_SUCCESS) GuLog("Preflight failed at ReadConfig(): %#llx\n", (unsigned long long)s); }
  if(s==EFI_SUCCESS) {
    /* Keep the PCIe capability offset of the admitted card so the post-Stop
     * quiescence observation does not have to walk the chain again. A card the
     * policy will not describe leaves it zero, which disables that observation
     * rather than guessing an offset; the range check keeps a malformed result
     * from pointing the Device Status read past the snapshot. */
    GU_CAPS caps;
    if(GuCheckConfigInitialMaster(Plan.config,0,0,0,0,&caps)&&caps.pcie&&
       (UINTN)caps.pcie+0x0cU<=(UINTN)sizeof(Plan.config))Plan.pcie_cap=caps.pcie;
  }
  if(s==EFI_SUCCESS) { s=Gu.pci->Attributes?Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Plan.attributes):EFI_UNSUPPORTED; if(s!=EFI_SUCCESS) GuLog("Preflight failed at Attributes: %#llx\n", (unsigned long long)s); }
  UINT64 decode=(Plan.config[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|
                (Plan.config[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0)|
                (Plan.config[1]&4?EFI_PCI_IO_ATTRIBUTE_BUS_MASTER:0);
  UINT64 allowed_attrs=decode|EFI_PCI_IO_ATTRIBUTE_IO;
  UINT64 checked_attrs=Plan.attributes&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
  if(s==EFI_SUCCESS&&checked_attrs!=decode&&checked_attrs!=allowed_attrs) {
    GuLog("Preflight failed at decode check: attrs=%#llx decode=%#llx\n", (unsigned long long)Plan.attributes, (unsigned long long)decode);
    s=EFI_NOT_READY;
  }
  if(s==EFI_SUCCESS) { s=Snapshot(&Original); if(s!=EFI_SUCCESS) GuLog("Preflight failed at Snapshot(): %#llx\n", (unsigned long long)s); }
  if(s==EFI_SUCCESS) {
    s=ReadOnlyAmdConsoleOwnership();
    if(s!=EFI_SUCCESS)GuLog("Preflight refused independent survivor display ownership: %#llx\n",(unsigned long long)s);
  }
  if(s==EFI_SUCCESS) {
    s=ReadOnlyAmdConsoleGraph(&Original);
    if(s!=EFI_SUCCESS)GuLog("Preflight refused active survivor console graph: %#llx\n",(unsigned long long)s);
  }
  if(s==EFI_SUCCESS) { s=Consumers(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at Consumers(): %#llx\n", (unsigned long long)s); }
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(s==EFI_SUCCESS) { s=TargetChildBaseline(); if(s!=EFI_SUCCESS)
    GuLog("Preflight failed at TargetChildBaseline(): %#llx\n",(unsigned long long)s); }
#endif
  if(s==EFI_SUCCESS) { s=PointerBaseline(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at PointerBaseline(): %#llx\n", (unsigned long long)s); }
  if(s==EFI_SUCCESS) { s=UsbBaseline(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at UsbBaseline(): %#llx\n", (unsigned long long)s); }
  GuLog("MSI preflight: status=%#llx graph=%zu code_profiles=3\n",(unsigned long long)s,(size_t)Original.count);
  if(s==EFI_SUCCESS)GuLog("MSI endpoints: pci=%#llx gop=%#llx console=%#llx pointer_agent=%#llx\n",
    (unsigned long long)(UINTN)Plan.pci,(unsigned long long)(UINTN)Plan.gop,
    (unsigned long long)(UINTN)Plan.console,(unsigned long long)(UINTN)Plan.pointer_agent);
  if(s==EFI_SUCCESS)Plan.ready=TRUE;return s;
}
static BOOLEAN DecodeMask(const UINT32 c[64],UINT64 *mask) {
  *mask=0;UINT32 missing=Plan.config[1]&~c[1]&3;
  if((Plan.config[1]&2)!=2||(c[1]&4)||
     !GuPciMasterClearOnly(Plan.config[1],c[1]|missing))return FALSE;
  for(UINTN i=0;i<64;i++)if(i!=1&&c[i]!=Plan.config[i])return FALSE;
  *mask=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);return TRUE;
}
/* The final vendor Stop can clear PCI Command decode bits, which this image
 * restores through config space before claiming PCI_IO exclusively. That raw
 * config restore does not restore PCI_IO's own enabled-attribute state. Include
 * any original IO/MEM attribute missing from the protocol readback, even when
 * DecodeMask reports no remaining config-register delta. Never include BusMaster. */
static UINT64 ClaimRestoreAttributeMask(UINT64 original,UINT64 current) {
  const UINT64 decode=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  return original&decode&~current;
}
/* Change only the selected card's Bus Master bit after vendor Stop.
 * No raw PCI writes, no new bus master enable, and no unverified peer drift. */
/* The vendor's Stop() is entitled to disable the card's IO and memory decode,
 * and on this bench it does: RUN0012 and RUN0013 are the first boots that got
 * this far, and both show the PCI Command word going 0x0003 -> 0x0000 at
 * 0000:01:00.0 as the driver detaches. The post-Stop drift rule then refused the
 * image's own release step: a driver that was told to let go did what it is
 * supposed to, and the image called that drift.
 *
 * The rule is not relaxed. Every other word still has to match the capture, the
 * only tolerated difference is those two enable bits being CLEAR (never set), and
 * the Command register is written back to the captured value and read back before
 * the rest of the path is allowed to compare anything. So the snapshot becomes
 * true again instead of being excused, and a change the image did not cause still
 * refuses exactly as it did before.
 */
static BOOLEAN RestoreDecodeClearedByDetach(UINT32 config[64]) {
  EFI_PCI_IO_PROTOCOL *io=NULL;
  if(!Plan.pci)return FALSE;
  if(Interface(Plan.pci,&PciGuid,(VOID **)&io)!=EFI_SUCCESS||io!=Gu.pci||
     !io->Pci.Write||!io->Pci.Read)return FALSE;
  if(Plan.stage!=3||Plan.claimed||Gu.flrs)return FALSE;
  UINT32 drift=config[1]^Plan.config[1];
  /* Only the Command word may differ, only in bits 0..1, and only by clearing
   * them: a bit that changed must be absent from the value read now. */
  if(!drift||(drift&~0x3U)||(config[1]&drift))return FALSE;
  /* Status is read-only: a change there is not the detach's to explain. */
  if((config[1]>>16)!=(Plan.config[1]>>16))return FALSE;
  for(UINTN i=0;i<64;i++)
    if(i!=1&&config[i]!=Plan.config[i])return FALSE;
  UINT32 before=config[1];
  UINT16 want=(UINT16)(Plan.config[1]&0xffffU);
  UINT16 value=want;
  EFI_STATUS s=io->Pci.Write(io,EfiPciIoWidthUint16,4,1,&value);
  if(EFI_ERROR(s)) {
    NOTE("PCI decode restore refused: offset=0x4 captured=%#x now=%#x status=%#llx\n",
      Plan.config[1],before,(unsigned long long)s);
    return FALSE;
  }
  UINT16 readback=0;
  s=io->Pci.Read(io,EfiPciIoWidthUint16,4,1,&readback);
  if(EFI_ERROR(s)||readback!=want) {
    NOTE("PCI decode restore refused: offset=0x4 captured=%#x now=%#x readback=%#x status=%#llx\n",
      Plan.config[1],before,(unsigned)readback,(unsigned long long)s);
    return FALSE;
  }
  config[1]=(config[1]&0xffff0000U)|readback;
  NOTE("PCI decode restored: offset=0x4 captured=%#x before=%#x after=%#x "
       "ACCEPTED=1 CAUSED_BY=driver-detach\n",
    Plan.config[1],before,config[1]);
  return TRUE;
}

static EFI_STATUS NormalizeMasterAfterStop(VOID) {
  UINT32 config[64];UINT64 before=0,after=0;
  const UINT64 master=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  if(!Plan.ready||!Plan.mutated||Plan.stage!=3||Plan.claimed||
     Gu.owned||Gu.fault||Gu.flrs!=0||Plan.master_normalized)return EFI_NOT_READY;
  if(GuVerifyActivePeers(Plan.table)!=EFI_SUCCESS)return EFI_NOT_READY;
  EFI_STATUS s=ReadConfigMode(config,TRUE);
  /* One explained exception, and only one: our own final detach cleared the
   * card's IO and memory decode. Restore it through the same config space, then
   * run the unchanged rule over the restored snapshot. */
  if(s==EFI_SUCCESS&&!ConfigStablePostStop(config)&&RestoreDecodeClearedByDetach(config)) {
    s=ReadConfigMode(config,TRUE);
  }
  if(s!=EFI_SUCCESS||!ConfigStablePostStop(config))return EFI_NOT_READY;
  if(!Gu.pci->Attributes)return EFI_UNSUPPORTED;
  s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&before);
  if(s!=EFI_SUCCESS)return s;
  if(!!(before&master)!=!!(config[1]&4u))return EFI_COMPROMISED_DATA;
  if((config[1]&4u)&&!(Plan.config[1]&4u))return EFI_ACCESS_DENIED;
  if(config[1]&4u) {
    s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationDisable,master,NULL);
    if(s!=EFI_SUCCESS){Gu.retained=TRUE;return s;}
  }
  s=ReadConfig(config);
  if(s==EFI_SUCCESS&&!ConfigStablePostStop(config))s=EFI_COMPROMISED_DATA;
  if(s==EFI_SUCCESS)s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&after);
  if(s!=EFI_SUCCESS||(after&master)||
     (after&~master)!=(before&~master)||
     GuVerifyActivePeers(Plan.table)!=EFI_SUCCESS) {
    Gu.retained=TRUE;
    return s==EFI_SUCCESS?EFI_COMPROMISED_DATA:s;
  }
  /* The bit is clear in both views; the device may still be draining. */
  {
    UINTN polls=0;
    s=WaitTxPendingClear(&polls);
    if(s!=EFI_SUCCESS) {Gu.retained=TRUE;return s;}
  }
  Plan.master_normalized=TRUE;
  NOTE("POST_STOP_BUS_MASTER=DISABLED VERIFIED=1 PREVIOUS=%u\n",
       (unsigned)!!(before&master));
  return EFI_SUCCESS;
}
static EFI_STATUS ClaimRestore(VOID) {
  if(!Plan.master_normalized)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 c[64];UINT64 mask=0,attrs=0,supported=0,enable_mask=0;
  EFI_STATUS s=ReadConfig(c);if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  GuLog("MSI decode restore intent: status=%#llx IO_MEM_mask=%#llx\n",(unsigned long long)s,(unsigned long long)mask);
  EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!DecodeMask(c,&mask)) {
    GuLog("ClaimRestore failed at DecodeMask 2\n");
    s=EFI_NOT_READY;
  }
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,FALSE)) {
    GuLog("ClaimRestore failed at Expected(3, FALSE)\n");
    GraphDiff(3, FALSE);
    s=EFI_NOT_READY;
  }
  if(s==EFI_SUCCESS)s=b->OpenProtocol(Plan.pci,&PciGuid,(VOID **)&io,Plan.self,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s!=EFI_SUCCESS) GuLog("ClaimRestore failed at OpenProtocol: %#llx\n", (unsigned long long)s);
  if(s==EFI_SUCCESS){Plan.claimed=TRUE;Gu.owned=TRUE;if(io!=Gu.pci){GuLog("ClaimRestore failed: io != Gu.pci\n");s=EFI_COMPROMISED_DATA;}}
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS)enable_mask=ClaimRestoreAttributeMask(Plan.attributes,attrs);
  if(s==EFI_SUCCESS) {
    UINT64 exp_pre = Plan.attributes & ~(mask|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    UINT64 chk_pre = attrs & (EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    GuLog("ClaimRestore: pre-enable attrs=%#llx (plan=%#llx config_mask=%#llx enable_mask=%#llx exp=%#llx)\n",
      (unsigned long long)attrs, (unsigned long long)Plan.attributes,
      (unsigned long long)mask, (unsigned long long)enable_mask,
      (unsigned long long)exp_pre);
    if((attrs & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
       (chk_pre != exp_pre && chk_pre != Plan.attributes && chk_pre != 0 && chk_pre != EFI_PCI_IO_ATTRIBUTE_IO)) {
      GuLog("ClaimRestore failed at pre-enable attrs: attrs=%#llx exp=%#llx\n", (unsigned long long)attrs, (unsigned long long)exp_pre);
      s=EFI_NOT_READY;
    }
  }
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&enable_mask)!=enable_mask) {
    GuLog("ClaimRestore failed at supported: supported=%#llx missing mask=%#llx\n", (unsigned long long)supported, (unsigned long long)enable_mask);
    s=EFI_UNSUPPORTED;
  }
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,TRUE)) {
    GuLog("ClaimRestore failed at Expected(3, TRUE)\n");
    GraphDiff(3, TRUE);
    s=EFI_NOT_READY;
  }
  if(s==EFI_SUCCESS&&enable_mask) {
    s=io->Attributes(io,EfiPciIoAttributeOperationEnable,enable_mask,NULL);
    if(s!=EFI_SUCCESS) GuLog("ClaimRestore failed at Attributes Enable: %#llx\n", (unsigned long long)s);
  }
  b->RestoreTPL(old);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c))) {
    GuLog("ClaimRestore: config change detected after restore:\n");
    for(UINTN i=0;i<64;i++) {
      if(c[i]!=Plan.config[i]) GuLog("  c[%u] was=%#x now=%#x\n", (unsigned)i, Plan.config[i], c[i]);
    }
    if(!ConfigStable(c)) {
      GuLog("ClaimRestore failed at ConfigStable\n");
      s=EFI_NOT_READY;
    }
  }
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS) {
    GuLog("ClaimRestore: post-enable attrs=%#llx (mem=%d bm=%d)\n",
      (unsigned long long)attrs, (int)!!(attrs & EFI_PCI_IO_ATTRIBUTE_MEMORY), (int)!!(attrs & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER));
    if((attrs & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) || !(attrs & EFI_PCI_IO_ATTRIBUTE_MEMORY) ||
       (attrs & ~(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY))) {
      GuLog("ClaimRestore failed at post-enable attrs check: attrs=%#llx\n", (unsigned long long)attrs);
      s=EFI_NOT_READY;
    } else {
      Plan.attributes = attrs;
    }
  }
  if(s==EFI_SUCCESS) {
    s=Bar();
    if(s!=EFI_SUCCESS) GuLog("ClaimRestore failed at Bar(): %#llx\n", (unsigned long long)s);
  }
#ifndef GU_STOP_RELEASE_ONLY_BENCH
  if(s==EFI_SUCCESS&&!GuAd104ColdEntry()) {
    GuLog("ClaimRestore failed at GuAd104ColdEntry\n");
    s=EFI_NOT_READY;
  }
#endif
  GuLog("MSI exclusive claim/decode result: status=%#llx\n",(unsigned long long)s);return s;
}
/* Pre-Stop state capture.
 *
 * The plan's step 3 asks for one record, taken immediately before the vendor
 * Stop callback, of every fact the admission depends on. Each field below is
 * either something this function just verified or one of the six facts no code
 * in this tree can establish; the six stay FALSE and are named in MISSING, so the
 * line cannot imply a check that never happened. */
static void StopContractLog(UINTN Stage, BOOLEAN Owner, BOOLEAN Cold,
                            BOOLEAN Console, BOOLEAN Peers) {
#if defined(GU_UNVERIFIED_BENCH_CONDITIONS) && !defined(GU_STOP_RELEASE_ONLY_BENCH)
  /* Do not log a bypassed AMD-console check as verified evidence. */
  Console=FALSE;
#endif
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(Plan.consoleless_target_gop)Console=FALSE;
#endif
  GU_STOP_OBSERVATION observation = {0};
  observation.DeviceId=(GU_SC_UINT32)Gu.device_id;
  observation.Owner=Owner?GU_SC_TRUE:GU_SC_FALSE;
  observation.Cold=Cold?GU_SC_TRUE:GU_SC_FALSE;
  observation.Console=Console?GU_SC_TRUE:GU_SC_FALSE;
  observation.Peers=Peers?GU_SC_TRUE:GU_SC_FALSE;
  /* TARGET_GOP, LOG, RECOVERY, SIGNED_BOOTER, BOUND and DMA are left false:
   * nothing in this image observes them, and a capture that guessed would be
   * worse than one that says so. */
  UINT32 missing=GuStopContractMissing(&observation);
  CHAR8 text[128];
  GuStopContractDescribe(missing,text,sizeof(text));
  GuLog("STOP_CONTRACT device=%04x stage=%zu OWNER=%d COLD=%d CONSOLE=%d PEERS=%d "
        "TARGET_GOP=%d LOG=%d RECOVERY=%d SIGNED_BOOTER=%d BOUND=%d DMA=%d "
        "DECISION=%s MISSING=%s\n",
        (unsigned)Gu.device_id,Stage,(int)observation.Owner,(int)observation.Cold,
        (int)observation.Console,(int)observation.Peers,
        (int)observation.TargetGopGone,(int)observation.LogClosed,
        (int)observation.Recovery,(int)observation.SignedBooter,
        (int)observation.Bound,(int)observation.DmaIdle,
        GuStopContractDecision(missing),text);
}

EFI_STATUS GuMsiRelease(VOID) {
  if(!ReleaseOwnerMatches())return EFI_NOT_READY;
  if(!Plan.ready||Plan.mutated||Gu.started||Gu.owned)return EFI_NOT_READY;
  /* The exact stock AD102/GA102 NVIDIA zero-child Stop path is not safe:
   * it can write BAR0 and enter a poll without a proven local bound.
   * Profile hashes and a structurally valid FWSEC descriptor do not prove
   * Windows 617.42 signed Booter acceptance or physical cold recovery.
   * Reject before any firmware callback, independent of entrypoint/build. */
#if !GU_ACTUATION_ADMITTED
  if(Gu.device_id==0x2684U||Gu.device_id==0x220aU) {
    /* Owner held at entry. Everything else is established by checks this refusal
     * deliberately precedes, so the capture claims only what it knows: an
     * unclaimed fact is listed as missing, never assumed. */
    StopContractLog(0,TRUE,FALSE,FALSE,FALSE);
    GuLog("MSI release refused for original AD102/GA102: vendor Stop and signed loader not verified\n");
    return EFI_UNSUPPORTED;
  }
#else
  /* Explicit admission. The refusal is replaced by a recorded acceptance:
   * nothing about the vendor Stop bound, signed Windows 617.42 Booter
   * acceptance or physical recovery is verified, and a simulated run says so. */
  if(Gu.device_id==0x2684U||Gu.device_id==0x220aU) {
    StopContractLog(0,TRUE,FALSE,FALSE,FALSE);
#if GU_ACTUATION_SIMULATED
    GuLog("MSI release SIMULATED for original AD102/GA102 device=%04x: "
          "ENVIRONMENT=HOST_SIMULATOR VENDOR_STOP_BOUND=UNVERIFIED "
          "SIGNED_BOOTER=UNVERIFIED RECOVERY=UNVERIFIED\n",
          (unsigned)Gu.device_id);
#else
    GuLog("MSI release WAIVED for original AD102/GA102 device=%04x: "
          "VENDOR_STOP_BOUND=UNVERIFIED SIGNED_BOOTER=UNVERIFIED "
          "RECOVERY=UNVERIFIED WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION\n",
          (unsigned)Gu.device_id);
#endif
  }
#endif
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  for(UINTN stage=Plan.headless?3u:1u;stage<=3;stage++) {
    GuLog("MSI driver detach intent: stage=%zu\n",(size_t)stage);
    EFI_STATUS s=Alive();UINT32 c[64];
    if(s==EFI_SUCCESS)s=ReadConfigMode(c,TRUE);
    if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
#ifndef GU_STOP_RELEASE_ONLY_BENCH
    if(s==EFI_SUCCESS&&!GuAd104ColdEntry())s=EFI_NOT_READY;
#endif
    if(s==EFI_SUCCESS)s=Gops(FALSE);
    if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleOwnership();
    if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
    EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
    if(s==EFI_SUCCESS)s=Snapshot(&Current);
    if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleGraph(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage-1,FALSE)) {
      NOTE("GuMsiRelease: pre-detach Expected(%zu) failed\n", (size_t)(stage-1));
      GraphDiff(stage-1, FALSE);
      s=EFI_NOT_READY;
    }
    /* Everything above just held: owner, cold selectors, console ownership and
     * the peer graph. That is the pre-Stop capture, taken before the callback
     * that can write BAR0 and poll. */
    if(s==EFI_SUCCESS)StopContractLog(stage-1,TRUE,TRUE,
                                       !Plan.consoleless_target_gop,TRUE);
    if(s==EFI_SUCCESS)s=Driver(stage-1);
    b->RestoreTPL(old);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    /* A failed file write or flush is not an acceptable record of a new
     * proprietary Stop callback. Reattach remains separately reachable if
     * an earlier stage already stopped a driver. */
    if(!GuBootLogOpen())return EFI_DEVICE_ERROR;
#endif
    /* Record before the callback: an error may follow a partial driver stop. */
    Plan.mutated=TRUE;
    s=b->DisconnectController(stage==3?Plan.pci:Plan.gop,
      stage==1?Plan.out_agent:stage==2?Plan.gfx_agent:Plan.nv_agent,NULL);
    GuLog("MSI driver detach return: stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    Plan.stage=stage;
    s=Snapshot(&Current);
    if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleGraph(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage,FALSE)) {
      NOTE("GuMsiRelease: post-detach Expected(%zu) failed\n", (size_t)stage);
      GraphDiff(stage, FALSE);
      s=EFI_NOT_READY;
    }
    if(s==EFI_SUCCESS)s=Alive();
    if(s==EFI_SUCCESS)s=Gops(FALSE);
    /* A vendor Stop may change the surviving PCI controller even when
     * its GOP handle and console graph still appear unchanged. */
    if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleOwnership();
    if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
    if(s!=EFI_SUCCESS)return s;
  }
  EFI_STATUS s=NormalizeMasterAfterStop();
  if(s!=EFI_SUCCESS)return s;
  s=GuVerifyActivePeers(Plan.table);
  if(s!=EFI_SUCCESS)return s;
  s=ClaimRestore();
  if(s==EFI_SUCCESS&&!GuMsiReleased())s=EFI_NOT_READY;
  return s;
}
BOOLEAN GuMsiReleased(VOID) {
  if(!ReleaseOwnerMatches())return FALSE;
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  UINT64 chk_attrs=0, exp_attrs=Plan.attributes & (EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
  if(!Plan.ready||Plan.stage!=3||!Plan.claimed||!Gu.owned||Gu.fault||
     !Plan.master_normalized)failed="state";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(Gops(FALSE)!=EFI_SUCCESS)failed="GOP";
  else if(ReadOnlyAmdConsoleOwnership()!=EFI_SUCCESS)failed="amd-ownership";
  else if(ReadConfig(c)!=EFI_SUCCESS)failed="config-read";
  else if(!ConfigStable(c))failed="config-drift";
  else if(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs)!=EFI_SUCCESS)failed="attributes-get";
  else if(!(attrs & EFI_PCI_IO_ATTRIBUTE_MEMORY))failed="attributes-no-mem";
  else if(attrs & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)failed="bus-master";
  else if(attrs & ~(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY))failed="attributes-extra";
  else if(Snapshot(&Current)!=EFI_SUCCESS)failed="graph-snapshot";
  else if(!Expected(3,TRUE)){failed="graph";GraphDiff(3, TRUE);}
  if(failed)NOTE("MSI release check failed: %s attributes=%#llx expected=%#llx graph=%zu original=%zu fault=%u flrs=%u\n",
    failed,(unsigned long long)attrs,(unsigned long long)Plan.attributes,(size_t)Current.count,(size_t)Original.count,Gu.fault,Gu.flrs);
  if(!failed)ReportAdditions();
  return !failed;
}

#ifdef GU_STOP_RELEASE_ONLY_BENCH
/* Draw the warning through the surviving, selected NVIDIA peer's GOP.
 * This does not call ConOut, connect any driver or write a display mode.
 * A successful Blt confirms only a firmware API return, not visible pixels. */
typedef struct {char symbol;UINT8 row[7];} GU_BENCH_GLYPH;
static const GU_BENCH_GLYPH BenchGlyphs[]={
  {'0',{0xe,0x11,0x13,0x15,0x19,0x11,0xe}},
  {'1',{0x4,0xc,0x4,0x4,0x4,0x4,0xe}},
  {'2',{0xe,0x11,0x1,0x2,0x4,0x8,0x1f}},
  {'3',{0x1e,0x1,0x1,0xe,0x1,0x1,0x1e}},
  {'4',{0x2,0x6,0xa,0x12,0x1f,0x2,0x2}},
  {'5',{0x1f,0x10,0x10,0x1e,0x1,0x1,0x1e}},
  {'6',{0xf,0x10,0x10,0x1e,0x11,0x11,0xe}},
  {'7',{0x1f,0x1,0x2,0x4,0x8,0x8,0x8}},
  {'8',{0xe,0x11,0x11,0xe,0x11,0x11,0xe}},
  {'9',{0xe,0x11,0x11,0xf,0x1,0x1,0x1e}},
  {'A',{0xe,0x11,0x11,0x1f,0x11,0x11,0x11}},
  {'B',{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}},
  {'C',{0xf,0x10,0x10,0x10,0x10,0x10,0xf}},
  {'D',{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}},
  {'E',{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}},
  {'F',{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}},
  {'G',{0xf,0x10,0x10,0x17,0x11,0x11,0xe}},
  {'H',{0x11,0x11,0x11,0x1f,0x11,0x11,0x11}},
  {'I',{0x1f,0x4,0x4,0x4,0x4,0x4,0x1f}},
  {'J',{0x7,0x2,0x2,0x2,0x12,0x12,0xc}},
  {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
  {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1f}},
  {'M',{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}},
  {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
  {'O',{0xe,0x11,0x11,0x11,0x11,0x11,0xe}},
  {'P',{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}},
  {'Q',{0xe,0x11,0x11,0x11,0x15,0x12,0xd}},
  {'R',{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}},
  {'S',{0xf,0x10,0x10,0xe,0x1,0x1,0x1e}},
  {'T',{0x1f,0x4,0x4,0x4,0x4,0x4,0x4}},
  {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0xe}},
  {'V',{0x11,0x11,0x11,0x11,0x11,0xa,0x4}},
  {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0xa}},
  {'X',{0x11,0x11,0xa,0x4,0xa,0x11,0x11}},
  {'Y',{0x11,0x11,0xa,0x4,0x4,0x4,0x4}},
  {'Z',{0x1f,0x1,0x2,0x4,0x8,0x10,0x1f}},
  {':',{0x0,0x4,0x4,0x0,0x4,0x4,0x0}},
  {'.',{0x0,0x0,0x0,0x0,0x0,0xc,0xc}},
  {'-',{0x0,0x0,0x0,0x1f,0x0,0x0,0x0}}
};
static const UINT8 *BenchGlyph(char symbol) {
  for(UINTN i=0;i<sizeof(BenchGlyphs)/sizeof(BenchGlyphs[0]);i++)
    if(BenchGlyphs[i].symbol==symbol)return BenchGlyphs[i].row;
  return NULL;
}
static EFI_STATUS BenchGopLine(EFI_GRAPHICS_OUTPUT_PROTOCOL *g,
                               UINTN x,UINTN y,UINTN scale,
                               const char *line) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL pixels[15U*21U];
  const EFI_GRAPHICS_OUTPUT_BLT_PIXEL white={255,255,255,0};
  const EFI_GRAPHICS_OUTPUT_BLT_PIXEL dark={24,16,80,0};
  if(!g||!g->Blt||!g->Mode||!g->Mode->Info||!line||
     scale<1U||scale>3U)return EFI_INVALID_PARAMETER;
  UINTN width=5U*scale,height=7U*scale,advance=6U*scale;
  for(UINTN index=0;line[index];index++) {
    if(index>80U||x+advance>g->Mode->Info->HorizontalResolution)
      return EFI_BAD_BUFFER_SIZE;
    if(line[index]==' ') {x+=advance;continue;}
    const UINT8 *rows=BenchGlyph(line[index]);
    if(!rows)return EFI_UNSUPPORTED;
    for(UINTN dy=0;dy<height;dy++)
      for(UINTN dx=0;dx<width;dx++)
        pixels[dy*width+dx]=(rows[dy/scale]&(1u<<(4U-dx/scale)))?
                             white:dark;
    EFI_STATUS s=g->Blt(g,pixels,EfiBltBufferToVideo,0,0,x,y,
                        width,height,width*sizeof(*pixels));
    if(s!=EFI_SUCCESS)return s;
    x+=advance;
  }
  return EFI_SUCCESS;
}
static char BenchHex(UINTN v) {
  return "0123456789ABCDEF"[v&15U];
}
static VOID BenchHexAt(char *text,UINTN at,UINTN digits,UINTN value) {
  for(UINTN i=0;i<digits;i++)
    text[at+i]=BenchHex(value>>((digits-1U-i)*4U));
}
EFI_STATUS GuMsiStopReleaseWarnSurvivor(VOID) {
  if(!ReleaseOwnerMatches()||!Plan.ready||Plan.mutated||Plan.stage||
     !Plan.survivor_baseline_valid||!GuBootLogOpen()||
     GuSelectedGpuCount()<2U)return EFI_NOT_READY;
  EFI_STATUS s=Gops(FALSE);
  if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
  if(s!=EFI_SUCCESS)return s;
  SURVIVOR_GOP *chosen=NULL;
  GU_GPU_RECORD selected={0};
  for(size_t i=0;i<GuSelectedGpuCount();i++) {
    s=GuSelectedGpuRecordAt(i,&selected);
    if(s!=EFI_SUCCESS)return s;
    if(selected.handle==Plan.pci)continue;
    if((selected.pci_id&0xffffU)!=0x10deU||!selected.handle||
       selected.pci==Gu.pci)return EFI_ACCESS_DENIED;
    EFI_DEVICE_PATH_PROTOCOL *peer_path=NULL;
    s=Interface((EFI_HANDLE)selected.handle,&PathGuid,(VOID **)&peer_path);
    if(s!=EFI_SUCCESS||!peer_path)return EFI_NOT_READY;
    UINTN matched=0;
    for(UINTN k=0;k<Plan.survivor_count;k++) {
      int owner=GuDevicePathHasPrefix(
          (const unsigned char *)peer_path,4096U,
          Plan.survivors[k].path,Plan.survivors[k].path_bytes);
      if(owner<0)return EFI_COMPROMISED_DATA;
      if(owner) {matched++;if(!chosen)chosen=&Plan.survivors[k];}
    }
    if(matched && chosen)break;
  }
  if(!chosen||!chosen->interface||!chosen->interface->Blt||
     !chosen->interface->Mode||!chosen->interface->Mode->Info)
    return EFI_NOT_READY;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
  s=Interface(chosen->handle,&GopGuid,(VOID **)&g);
  if(s!=EFI_SUCCESS||g!=chosen->interface||!g->Mode||
     !Equal(g->Mode,&chosen->mode,sizeof(chosen->mode))||
     !Equal(g->Mode->Info,&chosen->info,sizeof(chosen->info)))
    return EFI_NOT_READY;
  const UINTN width=g->Mode->Info->HorizontalResolution;
  const UINTN height=g->Mode->Info->VerticalResolution;
  if(width<600U||height<210U)return EFI_UNSUPPORTED;
  const UINTN scale=width>=1000U?3U:2U;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL dark={24,16,80,0};
  s=g->Blt(g,&dark,EfiBltVideoFill,0,0,12,12,580,180,0);
  if(s==EFI_SUCCESS)s=BenchGopLine(g,24,26,scale,"WARNING GPU DRIVER STOP TEST");
  char target[]="TARGET 0000:00:00.0";
  if(s==EFI_SUCCESS) {
    if(Gu.discovery_identity.segment>0xffffU||
       Gu.discovery_identity.bus>255U||
       Gu.discovery_identity.device>31U||
       Gu.discovery_identity.function>7U)return EFI_NOT_READY;
    BenchHexAt(target,7U,4U,Gu.discovery_identity.segment);
    BenchHexAt(target,12U,2U,Gu.discovery_identity.bus);
    BenchHexAt(target,15U,2U,Gu.discovery_identity.device);
    BenchHexAt(target,18U,1U,Gu.discovery_identity.function);
    s=BenchGopLine(g,24,67,scale,target);
  }
  if(s==EFI_SUCCESS)s=BenchGopLine(g,24,108,scale,"DISPLAY MAY GO DARK");
  if(s==EFI_SUCCESS)s=BenchGopLine(g,24,149,scale,"RECOVERY MAY NEED POWER OFF");
  GuLog("STOP_RELEASE_WARNING_GOP=%s TARGET=%04x:%02x:%02x.%x "
        "PEER=%04x:%02x:%02x.%x DISPLAY_API_STATUS=%#llx "
        "PHYSICAL_PIXELS_VERIFIED=0 CONSOLE_IO=0\n",
        s==EFI_SUCCESS?"BLT_SUCCESS":"BLT_FAILED",
        (unsigned)Gu.discovery_identity.segment,
        (unsigned)Gu.discovery_identity.bus,
        (unsigned)Gu.discovery_identity.device,
        (unsigned)Gu.discovery_identity.function,
        (unsigned)selected.segment,(unsigned)selected.bus,
        (unsigned)selected.device,(unsigned)selected.function,
        (unsigned long long)s);
  if(s!=EFI_SUCCESS)return s;
  /* Rendering is not an exception to any Stop or survivor validation. */
  return GuVerifyActivePeers(Plan.table);
}
#endif

#ifdef GU_STOP_RELEASE_ONLY_BENCH
EFI_STATUS GuMsiReattachAfterStop(VOID) {
  if(!ReleaseOwnerMatches()||!Plan.ready||Plan.stage!=3||
     !Plan.claimed||!Gu.owned||!Plan.mutated||Gu.fault||
     !GuMsiReleased())return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  if(!b||!b->CloseProtocol||!b->ConnectController)return EFI_UNSUPPORTED;
  EFI_STATUS s=b->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS) {
    NOTE("MSI reattach close-exclusive failed status=%#llx GPU_ACTION=HALT\n",
         (unsigned long long)s);
    Gu.fault=TRUE;Gu.retained=TRUE;
    return s;
  }
  Plan.claimed=FALSE;Gu.owned=FALSE;Plan.stage=0;
  Plan.reconnecting=TRUE;
  /* Reconnect only after releasing our exclusive PCI I/O claim. Recursive
   * connection lets the firmware bind the original graphics stack and its
   * children. It is an attempt, not a guarantee; verify the observed graph. */
  s=b->ConnectController(Plan.pci,NULL,NULL,TRUE);
  NOTE("MSI controller reconnect status=%#llx recursive=1\n",
       (unsigned long long)s);
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS)s=ReconcileTargetChildren();
  if(s==EFI_SUCCESS&&!ReattachedGraphExpected())s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleGraph(&Current);
  if(s==EFI_SUCCESS)s=Gops(FALSE);
  if(s==EFI_SUCCESS)s=ReattachedDriversVerified();
  if(s==EFI_SUCCESS)s=ReattachedTargetPciRecognized();
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleOwnership();
  if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
  if(s!=EFI_SUCCESS||Gu.fault) {
    NOTE("MSI reattach verification failed status=%#llx GRAPH_RESTORED=0\n",
         (unsigned long long)(s==EFI_SUCCESS?EFI_DEVICE_ERROR:s));
    Gu.fault=TRUE;Gu.retained=TRUE;
    return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  }
  Plan.reattach_verified=TRUE;
  /* The graph and GOP identity have been checked. This is not proof that a
   * monitor is electrically connected or displaying pixels. A selected GOP
   * is restored only when the target was not headless before the Stop. */
  NOTE("MSI reattach verification passed GRAPH_RESTORED=1 "
       "TARGET_GOP_RECOGNIZED=%u TARGET_CHILD_PATHS_MATCHED=%zu "
       "DRIVERS_VERIFIED=1 SURVIVOR_GOPS_MATCHED=%zu "
       "DISPLAY_OUTPUT_TESTED=0 FIRMWARE_TRANSACTION=0\n",
       (unsigned)!Plan.headless,(size_t)Plan.target_child_count,
       (size_t)Plan.survivor_count);
  return EFI_SUCCESS;
}
#endif
/* Keep the original architecture, SEC2, selector, PLM and chip readbacks.
 * Only perform MMIO while this image owns the PCI protocol. */
static BOOLEAN GuMsiHardwareVerifiedBeforeClose(VOID) {
  if(!Plan.ready||!Plan.claimed||!Gu.owned||Gu.fault)return FALSE;
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  UINT32 architecture=(chip>>24)&0x0fU;
  BOOLEAN chip_ok=chip!=0xffffffff&&(chip&0xffff0000)!=0xbadf0000&&
    (Gu.is_turing?architecture==0x06U:Gu.is_ampere?architecture==0x07U:architecture==0x09U);
  /* SS1 is the profile's business: the unlocking policies leave the unlock
   * value there, entry-latched-preserve leaves the value it found. The readback
   * is compared against the same contract the transaction was built under. */
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
  UINT32 entry0=0,entry1=0;
  BOOLEAN ss1_ok=GuGetEntrySelectorsFor(&Gu,&entry0,&entry1)&&ss1==entry1;
  const char *ss1_contract="preserved";
#else
  BOOLEAN ss1_ok=ss1==8;
  const char *ss1_contract="unlocked";
#endif
  (void)ss1_contract;
  BOOLEAN valid=!Gu.fault&&chip_ok&&(cpu&0x10)&&cpu!=0xffffffff&&
    (cpu&0xffff0000)!=0xbadf0000&&!(hi>>4)&&ss0==0x88888888&&ss1_ok&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
  NOTE("MSI pre-close silicon verification: valid=%u chip=%#x cpu=%#x hi=%#x ss0=%#x ss1=%#x ss1_contract=%s readout=%#x plm=%#x\n",
       valid,chip,cpu,hi,ss0,ss1,ss1_contract,readout,plm);
  return valid;
}
EFI_STATUS GuMsiFinish(VOID) {
  if(!ReleaseOwnerMatches())return EFI_NOT_READY;
  Plan.hardware_verified=FALSE;
  if(!GuSafeToReturn()||!Gu.quiesced||Gu.flrs!=1||
     !Plan.master_normalized||!GuMsiReleased()){
    NOTE("MSI finish refused before close\n");return EFI_NOT_READY;
  }
  if(GuVerifyActivePeers(Plan.table)!=EFI_SUCCESS)return EFI_NOT_READY;
  if(!GuMsiHardwareVerifiedBeforeClose()){
    NOTE("MSI finish refused: pre-close silicon check failed\n");return EFI_DEVICE_ERROR;
  }
  /* A failed CloseProtocol or post-close graph check invalidates the latch. */
  Plan.hardware_verified=TRUE;
  EFI_STATUS s=Plan.table->BootServices->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS){
    Plan.hardware_verified=FALSE;
    NOTE("MSI finish close status=%#llx\n",(unsigned long long)s);
    return s;
  }
  Plan.claimed=FALSE;Gu.owned=FALSE;
  s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,FALSE)){s=EFI_NOT_READY;GraphDiff(3,FALSE);}
  /* A successful CloseProtocol does not prove that the surviving GOP
   * handles still refer to their pre-Stop paths. Reject drift here rather
   * than return success and defer the failure until Windows handover. */
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=Gops(FALSE);
  if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleOwnership();
  if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
  if(s!=EFI_SUCCESS){
    Plan.hardware_verified=FALSE;
    NOTE("MSI finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  }
  return s;
}
BOOLEAN GuMsiMutated(VOID){
  if(!ReleaseOwnerMatches())return Gu.release_owner_invalid;
  return Plan.mutated;
}
BOOLEAN GuMsiReadyForWindows(VOID) {
  if(!ReleaseOwnerMatches())return FALSE;
  if(!Plan.ready||Plan.stage!=3||Plan.claimed||Gu.owned||Gu.fault||
     !Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn()||!Plan.hardware_verified){
    /* A failed state prerequisite is as final as a failed PCI/GOP check.
     * Never permit a later call to revive an earlier successful finish. */
    Plan.hardware_verified=FALSE;
    return FALSE;
  }
  UINT32 c[64];
  /* GuMsiFinish already verified the entire post-close protocol graph.
   * Do not scan it a second time or read BAR0 after CloseProtocol. */
  BOOLEAN ready=Alive()==EFI_SUCCESS&&Gops(FALSE)==EFI_SUCCESS&&
         ReadOnlyAmdConsoleOwnership()==EFI_SUCCESS&&
         ReadConfig(c)==EFI_SUCCESS&&ConfigStable(c)&&
         GuVerifyActivePeers(Plan.table)==EFI_SUCCESS;
  /* Once the final handover has observed a lost survivor, peer or PCI
   * identity, no later recheck may restore approval in this transaction. */
  if(!ready)Plan.hardware_verified=FALSE;
  return ready;
}

const VOID *GuMsiPlanOwnerToken(VOID) {return ReleaseOwnerMatches()?CurrentMsi:NULL;}

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Historical proof for a registered, completed context. Do not use its
 * old live graph after another target has legitimately changed the graph. */
BOOLEAN GuMsiCompletedPlatform(const GU_PLATFORM *platform,const VOID *token) {
  if(!platform||!token||platform->release_state!=token||
     !ReleaseStateMatches(platform))return FALSE;
  const PLAN *plan=&platform->release_state->plan;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  return plan->table && plan->pci==platform->handle && plan->self==platform->image &&
    plan->ready && plan->mutated && plan->stage==0 &&
    plan->survivor_baseline_valid && !plan->claimed &&
    plan->reattach_verified && !plan->hardware_verified;
#else
  return plan->table && plan->pci==platform->handle && plan->self==platform->image &&
    plan->ready && plan->mutated && plan->stage==3 &&
    plan->survivor_baseline_valid &&
    !plan->claimed && plan->master_normalized && plan->hardware_verified;
#endif
}

/* Check the live graph against the last target's baseline, which already
 * includes the earlier completed transactions. Never rebind an old context
 * or read device registers after the exclusive claim has closed. */
BOOLEAN GuMsiVerifyFinalGraph(VOID) {
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(!ReleaseOwnerMatches()||!Plan.reattach_verified||Plan.stage!=0||
     Plan.claimed||Gu.owned||Gu.fault)return FALSE;
  EFI_STATUS s=Snapshot(&Current);
  if(s==EFI_SUCCESS)s=ReconcileTargetChildren();
  if(s==EFI_SUCCESS&&!ReattachedGraphExpected())s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleGraph(&Current);
  if(s==EFI_SUCCESS)s=Gops(FALSE);
  if(s==EFI_SUCCESS)s=ReattachedDriversVerified();
  if(s==EFI_SUCCESS)s=ReattachedTargetPciRecognized();
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=ReadOnlyAmdConsoleOwnership();
  if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
  if(s!=EFI_SUCCESS) {
    Plan.reattach_verified=FALSE;
    return FALSE;
  }
  return TRUE;
#else
  if(!GuMsiReadyForWindows())return FALSE;
  if(Snapshot(&Current)!=EFI_SUCCESS||!Expected(3,FALSE)||
     ReadOnlyAmdConsoleGraph(&Current)!=EFI_SUCCESS) {
    Plan.hardware_verified=FALSE;
    return FALSE;
  }
  return TRUE;
#endif
}
#endif
