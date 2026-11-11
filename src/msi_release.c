/* SPDX-License-Identifier: GPL-2.0-only */
/* MSI Z790 GODLIKE/AD102 ownership adapter (was MSI Z490/AD104). Snapshot/delta strategy adapted from
 * impacifist/Windows-GPU-UNLOCK 9352abd internal-boot/boot/prepare-console.c.
 * Driver identities/code were captured in CAP0001 (Z790, 2026-10-25). No driver code/data patch.
 * No firmware variables, controller reconnects or reset fallback are used.
 */
#include "msi_release.h"
#include "port.h"
#include "boot_io.h"
#include "profile.h"
#include "sha256.h"
#include "gpu_device_path.h"
#include "gpu_discovery.h"
#include "pci_master_policy.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>

#define MAX_EDGES 4096
#define MAX_SURVIVOR_GOPS 16
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;} EDGE;
typedef struct {UINTN count;EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,gop,out_agent,gfx_agent,nv_agent;
  EFI_HANDLE console,input,error,pointer_agent,key_agent,usb_host,usb_agent,idle[8];
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT32 config[64];UINT64 attributes;
  EFI_HANDLE survivors[MAX_SURVIVOR_GOPS];
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE survivor_modes[MAX_SURVIVOR_GOPS];
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION survivor_info[MAX_SURVIVOR_GOPS];
  UINTN survivor_count;
  UINTN stage,reported_additions,reported_removals;BOOLEAN ready,mutated,claimed,hardware_verified,headless,master_normalized;
} PLAN;
static PLAN Plan;
static GRAPH Original,Current;
static BOOLEAN OriginalUsb[MAX_EDGES];
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
static EFI_GUID PointerGuid={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0,0x90,0x27,0x3f,0xc1,0x4d}};
/* CAP0001: the Z790 pointer splitter also consumes an absolute pointer on the same USB device. */
static EFI_GUID AbsPointerGuid={0x8d59d32b,0xc655,0x4ae9,{0x9b,0x15,0xf2,0x59,0x04,0x99,0x2a,0x43}};
static EFI_GUID UsbHcGuid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
static const UINT8 UsbHostPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,2,1,1,1,6,0,0,0,1,1,6,0,0,8,1,1,6,0,1,0,127,255,4,0};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const UINT8 PciPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,3,1,1,6,0,0,0,127,255,4,0};
static const UINT8 GopPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,3,1,1,6,0,0,0,2,3,8,0,0,3,1,0x80,127,255,4,0};
static const UINT8 OutFile[]={4,6,20,0,0x7d,0x49,0x8a,0x62,0xf6,0x2b,0x64,0x42,0x87,0x41,6,0x9d,0xbd,0x33,0x99,0xd6,127,255,4,0};
static const UINT8 GfxFile[]={4,6,20,0,0xdd,0xab,0xe7,0x43,0x52,0xe3,0xfb,0x4c,0xa2,0x30,0x4c,0xdc,0x1d,0x35,0x0e,0x5c,127,255,4,0};
static const UINT8 NvFile[]={4,4,28,0,'n',0,'v',0,'g',0,'o',0,'p',0,'-',0,'a',0,'d',0,'1',0,'x',0,'x',0,0,0,127,255,4,0};
/* CAP0001: NVIDIA GOP also creates two idle output children (_ADR 0x80010320/0x80010340)
 * with no consumers. They are pinned exactly and vanish with the stage-3 GOP stop. */
static const UINT8 IdlePath[1][36]={
  {2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,3,1,1,6,0,0,0,2,3,8,0,0x20,3,1,0x80,127,255,4,0}};

static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return Equal(a,b,16);}
static BOOLEAN PointerProto(const EFI_GUID *g){return Guid(g,&PointerGuid)||Guid(g,&AbsPointerGuid);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
/* Match exact, bounded, single-instance paths; never accept a prefix alone. */
static BOOLEAN Path(EFI_HANDLE h,const UINT8 *bytes,UINTN count) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at+4<=count) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>count-at||!Equal(p,bytes+at,n))return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE)return n==4&&p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at+4==count;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return FALSE;
}
static EFI_STATUS Snapshot(GRAPH *g) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *handles=NULL;UINTN count=0;g->count=0;
  EFI_STATUS s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>4096))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN pc=0;
    s=b->ProtocolsPerHandle(handles[i],&protocols,&pc);
    if(s==EFI_SUCCESS&&(!protocols||!pc||pc>64))s=EFI_BAD_BUFFER_SIZE;
    for(UINTN j=0;s==EFI_SUCCESS&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      s=b->OpenProtocolInformation(handles[i],protocols[j],&entries,&ec);
      if(s==EFI_SUCCESS&&(ec>128||(ec&&!entries)))s=EFI_BAD_BUFFER_SIZE;
      for(UINTN k=0;s==EFI_SUCCESS&&k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        BOOLEAN own=Plan.claimed&&handles[i]==Plan.pci&&e->AgentHandle==Plan.self&&
          !e->ControllerHandle&&e->Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->OpenCount==1;
        if(g->count==MAX_EDGES||!e->AgentHandle||(!e->ControllerHandle&&!own)||!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        g->edges[g->count++]=(EDGE){handles[i],e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount};
      }
      if(entries&&b->FreePool(entries)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    }
    if(protocols&&b->FreePool(protocols)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  }
  if(handles&&b->FreePool(handles)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
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
  for(UINTN k=0;k<8;k++) {
    if(stage>=3&&Plan.idle[k]&&e->handle==Plan.idle[k])return TRUE;
  }
  return FALSE;
}
/* Original unrelated relationships must remain present. New relationships can
 * appear during USB enumeration; they cannot involve any protected endpoint.
 * A protocol GUID alone never authorizes access to the GPU or its owners. */
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  if(!h)return FALSE;
  if(h==Plan.pci||h==Plan.gop||h==Plan.self||
     GuIsPreparedPeerHandle(h)||
     h==Plan.console||h==Plan.input||h==Plan.error)return TRUE;
  for(UINTN k=0;k<8;k++){if(Plan.idle[k]&&h==Plan.idle[k])return TRUE;}
  for(UINTN k=0;k<Plan.survivor_count;k++)
    if(Plan.survivors[k]==h)return TRUE;
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
  memset(OriginalUsb,0,sizeof(OriginalUsb));
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
    if(child||device){OriginalUsb[i]=TRUE;dynamic++;}
  }
  GuLog("MSI USB baseline: host=%#llx bus_agent=%#llx dynamic_edges=%zu; host ownership pinned\n",
    (unsigned long long)(UINTN)Plan.usb_host,(unsigned long long)(UINTN)Plan.usb_agent,(size_t)dynamic);
  return EFI_SUCCESS;
}
static BOOLEAN UsbRemoved(UINTN i) {
  if(!OriginalUsb[i])return FALSE;
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
static EFI_STATUS Driver(UINTN which) {
  EFI_HANDLE a=which==0?Plan.out_agent:which==1?Plan.gfx_agent:Plan.nv_agent;
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
  if(!im->ImageBase||im->ImageSize<4096||im->ImageCodeType!=EfiBootServicesCode||
     (UINTN)d->Stop < (UINTN)im->ImageBase||(UINTN)d->Stop >= (UINTN)im->ImageBase + im->ImageSize) {
    GuLog("Driver(%zu) image check failed: base=%#llx size=%zu stop=%#llx\n",
      (size_t)which, (unsigned long long)(UINTN)im->ImageBase, (size_t)im->ImageSize, (unsigned long long)(UINTN)d->Stop);
    return EFI_UNSUPPORTED;
  }
  GuLog("Driver(%zu) verified OK: ver=%#x size=%zu\n", (size_t)which, d->Version, (size_t)im->ImageSize);
  return EFI_SUCCESS;
}
static EFI_STATUS Alive(VOID) {
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
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  EFI_HANDLE *handles=NULL;
  UINTN count=0,physical=0,target_count=0;
  BOOLEAN seen[MAX_SURVIVOR_GOPS]={FALSE};
  EFI_STATUS s=b->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  if(s==EFI_NOT_FOUND && !first && Plan.stage>=3 && !Plan.survivor_count)
    return EFI_SUCCESS;
  if(s!=EFI_SUCCESS)return s;
  if((count&&!handles)||count>4096u)s=EFI_BAD_BUFFER_SIZE;
  if(first) {
    Plan.gop=NULL;
    Plan.survivor_count=0;
    Plan.headless=FALSE;
  }
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
    BOOLEAN on_target=FALSE;
    s=Interface(handles[i],&GopGuid,(VOID **)&g);
    if(s!=EFI_SUCCESS)break;
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
        Plan.gop=handles[i];
        Plan.gop_interface=g;
        Plan.mode=*g->Mode;
        Plan.info=*g->Mode->Info;
      } else if(Plan.stage>=3 || Plan.headless || handles[i]!=Plan.gop ||
                !Equal(g->Mode,&Plan.mode,sizeof(Plan.mode))||
                !Equal(g->Mode->Info,&Plan.info,sizeof(Plan.info))) {
        s=EFI_NOT_READY;break;
      }
    } else if(first) {
      if(Plan.survivor_count==MAX_SURVIVOR_GOPS){s=EFI_BAD_BUFFER_SIZE;break;}
      UINTN k=Plan.survivor_count++;
      Plan.survivors[k]=handles[i];
      Plan.survivor_modes[k]=*g->Mode;
      Plan.survivor_info[k]=*g->Mode->Info;
    } else {
      UINTN k=0;
      while(k<Plan.survivor_count && Plan.survivors[k]!=handles[i])k++;
      if(k==Plan.survivor_count||seen[k]||
         !Equal(g->Mode,&Plan.survivor_modes[k],sizeof(*g->Mode))||
         !Equal(g->Mode->Info,&Plan.survivor_info[k],sizeof(*g->Mode->Info))){
        s=EFI_NOT_READY;break;
      }
      seen[k]=TRUE;
    }
  }
  if(handles && b->FreePool(handles)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  if(s!=EFI_SUCCESS)return s;
  if(first){
    Plan.headless=target_count==0;
    if(Plan.headless && !Plan.survivor_count)return EFI_NOT_READY;
    GuLog("TARGET_GOP=%zu SURVIVOR_GOPS=%zu HEADLESS=%u PHYSICAL=%zu\n",
          (size_t)target_count,(size_t)Plan.survivor_count,
          (unsigned)Plan.headless,(size_t)physical);
  } else {
    for(UINTN k=0;k<Plan.survivor_count;k++)
      if(!seen[k])return EFI_NOT_READY;
    if(Plan.headless && target_count)return EFI_NOT_READY;
    if(!Plan.headless && Plan.stage<3 && target_count!=1)return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
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
static EFI_STATUS Bar(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;UINT64 support=0;
  if(!Gu.pci->GetBarAttributes)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,(VOID **)&d);
  if(s==EFI_SUCCESS&&(!d||d->Desc!=0x8a||d->Len!=43||d->ResType||
     d->AddrRangeMin!=Gu.bar_base||d->AddrRangeMax!=(Gu.bar_base+0xffffff)||
     d->AddrTranslationOffset||d->AddrLen!=0x1000000))s=EFI_UNSUPPORTED;
  if(d&&Plan.table->BootServices->FreePool(d)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
/* An idle output: exact pinned path, distinct, and its only relationship is the
 * single NVIDIA child edge that names it. Anything else refuses the run. */
static BOOLEAN IdleOutput(EFI_HANDLE h) {
  if(ProtectedHandle(h)||h==Plan.gop)return FALSE;
  for(UINTN k=0;k<8;k++) {
    if(!Plan.idle[k]){Plan.idle[k]=h;return TRUE;}
    if(Plan.idle[k]==h)return TRUE;
  }
  return FALSE;
}
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0,child=0,idle=0,text=0,virtual=0,gfx=0;
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
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8&&IdleOutput(e->controller)){
        idle++;
      }
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8){
        for(UINTN k=0;k<8;k++) {
          if(!Plan.idle[k]){Plan.idle[k]=e->controller;break;}
          if(Plan.idle[k]==e->controller)break;
        }
        child++;
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
  if(pci<1||(child<1&&idle<1)||text<1||gfx<1||!Plan.nv_agent||!Plan.out_agent||!Plan.gfx_agent||
     Plan.nv_agent==Plan.out_agent||Plan.out_agent==Plan.gfx_agent||Plan.gfx_agent==Plan.nv_agent) {
    GuLog("Consumers check: pci=%zu child=%zu idle=%zu text=%zu virtual=%zu gfx=%zu\n",
      pci, child, idle, text, virtual, gfx);
    return EFI_ACCESS_DENIED;
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
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci) {
  if(Plan.mutated||Plan.ready)return EFI_ALREADY_STARTED;
  memset(&Plan,0,sizeof(Plan));Plan.table=t;Plan.self=self;Plan.pci=pci;
  if(!t||!t->BootServices||!self||!pci||Gu.handle!=pci||Gu.image!=self||Gu.bs!=t->BootServices||
     !Gu.profile_verified||!Gu.prom_verified||Gu.started||Gu.owned||Gu.armed)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=t->BootServices;
  if(!b->DisconnectController||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL||
     !b->ProtocolsPerHandle||!b->OpenProtocolInformation||!b->LocateHandleBuffer||!b->HandleProtocol)return EFI_UNSUPPORTED;
  Plan.console=t->ConsoleOutHandle;Plan.input=t->ConsoleInHandle;Plan.error=t->StandardErrorHandle;
  Plan.text=t->ConOut;Plan.err=t->StdErr;Plan.keys=t->ConIn;
  if(!Plan.console||!Plan.input||!Plan.error||!Plan.text||!Plan.err||!Plan.keys||
     !Plan.keys->ReadKeyStroke||!Plan.keys->Reset||!Plan.keys->WaitForKey)return EFI_UNSUPPORTED;
  Plan.key_reset=(UINTN)Plan.keys->Reset;Plan.key_read=(UINTN)Plan.keys->ReadKeyStroke;Plan.key_wait=Plan.keys->WaitForKey;
  if(!pci||!GuAd104ColdEntry())return EFI_NOT_READY;
  EFI_STATUS s=GuVerifyActivePeers(t);
  if(s==EFI_SUCCESS)s=Gops(TRUE);
  if(s!=EFI_SUCCESS) GuLog("Preflight failed at Gops(): %#llx\n", (unsigned long long)s);
  if(s==EFI_SUCCESS) { s=Alive(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at Alive(): %#llx\n", (unsigned long long)s); }
  if(s==EFI_SUCCESS) { s=Bar(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at Bar(): %#llx\n", (unsigned long long)s); }
  if(s==EFI_SUCCESS) { s=ReadConfigMode(Plan.config,TRUE); if(s!=EFI_SUCCESS) GuLog("Preflight failed at ReadConfig(): %#llx\n", (unsigned long long)s); }
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
  if(s==EFI_SUCCESS) { s=Consumers(); if(s!=EFI_SUCCESS) GuLog("Preflight failed at Consumers(): %#llx\n", (unsigned long long)s); }
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
/* Change only the selected card's Bus Master bit after vendor Stop.
 * No raw PCI writes, no new bus master enable, and no unverified peer drift. */
static EFI_STATUS NormalizeMasterAfterStop(VOID) {
  UINT32 config[64];UINT64 before=0,after=0;
  const UINT64 master=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
  if(!Plan.ready||!Plan.mutated||Plan.stage!=3||Plan.claimed||
     Gu.owned||Gu.fault||Gu.flrs!=0||Plan.master_normalized)return EFI_NOT_READY;
  if(GuVerifyActivePeers(Plan.table)!=EFI_SUCCESS)return EFI_NOT_READY;
  EFI_STATUS s=ReadConfigMode(config,TRUE);
  if(s!=EFI_SUCCESS||!ConfigStable(config))return EFI_NOT_READY;
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
  if(s==EFI_SUCCESS&&!ConfigStable(config))s=EFI_COMPROMISED_DATA;
  if(s==EFI_SUCCESS)s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&after);
  if(s!=EFI_SUCCESS||(after&master)||
     (after&~master)!=(before&~master)||
     GuVerifyActivePeers(Plan.table)!=EFI_SUCCESS) {
    Gu.retained=TRUE;
    return s==EFI_SUCCESS?EFI_COMPROMISED_DATA:s;
  }
  Plan.master_normalized=TRUE;
  NOTE("POST_STOP_BUS_MASTER=DISABLED VERIFIED=1 PREVIOUS=%u\n",
       (unsigned)!!(before&master));
  return EFI_SUCCESS;
}
static EFI_STATUS ClaimRestore(VOID) {
  if(!Plan.master_normalized)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 c[64];UINT64 mask=0,attrs=0,supported=0;
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
  if(s==EFI_SUCCESS) {
    UINT64 exp_pre = Plan.attributes & ~(mask|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    UINT64 chk_pre = attrs & (EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
    GuLog("ClaimRestore: pre-enable attrs=%#llx (plan=%#llx mask=%#llx exp=%#llx)\n",
      (unsigned long long)attrs, (unsigned long long)Plan.attributes, (unsigned long long)mask, (unsigned long long)exp_pre);
    if((attrs & EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
       (chk_pre != exp_pre && chk_pre != Plan.attributes && chk_pre != 0 && chk_pre != EFI_PCI_IO_ATTRIBUTE_IO)) {
      GuLog("ClaimRestore failed at pre-enable attrs: attrs=%#llx exp=%#llx\n", (unsigned long long)attrs, (unsigned long long)exp_pre);
      s=EFI_NOT_READY;
    }
  }
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&mask)!=mask) {
    GuLog("ClaimRestore failed at supported: supported=%#llx missing mask=%#llx\n", (unsigned long long)supported, (unsigned long long)mask);
    s=EFI_UNSUPPORTED;
  }
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,TRUE)) {
    GuLog("ClaimRestore failed at Expected(3, TRUE)\n");
    GraphDiff(3, TRUE);
    s=EFI_NOT_READY;
  }
  if(s==EFI_SUCCESS&&mask) {
    s=io->Attributes(io,EfiPciIoAttributeOperationEnable,mask,NULL);
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
  if(s==EFI_SUCCESS&&!GuAd104ColdEntry()) {
    GuLog("ClaimRestore failed at GuAd104ColdEntry\n");
    s=EFI_NOT_READY;
  }
  GuLog("MSI exclusive claim/decode result: status=%#llx\n",(unsigned long long)s);return s;
}
EFI_STATUS GuMsiRelease(VOID) {
  if(!Plan.ready||Plan.mutated||Gu.started||Gu.owned)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  for(UINTN stage=Plan.headless?3u:1u;stage<=3;stage++) {
    GuLog("MSI driver detach intent: stage=%zu\n",(size_t)stage);
    EFI_STATUS s=Alive();UINT32 c[64];
    if(s==EFI_SUCCESS)s=ReadConfigMode(c,TRUE);
    if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS&&!GuAd104ColdEntry())s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Gops(FALSE);
    if(s==EFI_SUCCESS)s=GuVerifyActivePeers(Plan.table);
    EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
    if(s==EFI_SUCCESS)s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage-1,FALSE)) {
      NOTE("GuMsiRelease: pre-detach Expected(%zu) failed\n", (size_t)(stage-1));
      GraphDiff(stage-1, FALSE);
      s=EFI_NOT_READY;
    }
    if(s==EFI_SUCCESS)s=Driver(stage-1);
    b->RestoreTPL(old);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    /* Record before the callback: an error may follow a partial driver stop. */
    Plan.mutated=TRUE;
    s=b->DisconnectController(stage==3?Plan.pci:Plan.gop,
      stage==1?Plan.out_agent:stage==2?Plan.gfx_agent:Plan.nv_agent,NULL);
    GuLog("MSI driver detach return: stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    Plan.stage=stage;
    s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage,FALSE)) {
      NOTE("GuMsiRelease: post-detach Expected(%zu) failed\n", (size_t)stage);
      GraphDiff(stage, FALSE);
      s=EFI_NOT_READY;
    }
    if(s==EFI_SUCCESS)s=Alive();
    if(s==EFI_SUCCESS)s=Gops(FALSE);
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
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  UINT64 chk_attrs=0, exp_attrs=Plan.attributes & (EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER);
  if(!Plan.ready||Plan.stage!=3||!Plan.claimed||!Gu.owned||Gu.fault||
     !Plan.master_normalized)failed="state";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(Gops(FALSE)!=EFI_SUCCESS)failed="GOP";
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
/* Keep the original architecture, SEC2, selector, PLM and chip readbacks.
 * Only perform MMIO while this image owns the PCI protocol. */
static BOOLEAN GuMsiHardwareVerifiedBeforeClose(VOID) {
  if(!Plan.ready||!Plan.claimed||!Gu.owned||Gu.fault)return FALSE;
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  UINT32 architecture=(chip>>24)&0x0fU;
  BOOLEAN chip_ok=chip!=0xffffffff&&(chip&0xffff0000)!=0xbadf0000&&
    (Gu.is_turing?architecture==0x06U:Gu.is_ampere?architecture==0x07U:architecture==0x09U);
  BOOLEAN valid=!Gu.fault&&chip_ok&&(cpu&0x10)&&cpu!=0xffffffff&&
    (cpu&0xffff0000)!=0xbadf0000&&!(hi>>4)&&ss0==0x88888888&&ss1==8&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
  NOTE("MSI pre-close silicon verification: valid=%u chip=%#x cpu=%#x hi=%#x ss0=%#x ss1=%#x readout=%#x plm=%#x\n",
       valid,chip,cpu,hi,ss0,ss1,readout,plm);
  return valid;
}
EFI_STATUS GuMsiFinish(VOID) {
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
  if(s!=EFI_SUCCESS){
    Plan.hardware_verified=FALSE;
    NOTE("MSI finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  }
  return s;
}
BOOLEAN GuMsiMutated(VOID){return Plan.mutated;}
BOOLEAN GuMsiReadyForWindows(VOID) {
  if(!Plan.ready||Plan.stage!=3||Plan.claimed||Gu.owned||Gu.fault||
     !Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn()||!Plan.hardware_verified)return FALSE;
  UINT32 c[64];
  /* GuMsiFinish already verified the entire post-close protocol graph.
   * Do not scan it a second time or read BAR0 after CloseProtocol. */
  return Alive()==EFI_SUCCESS&&Gops(FALSE)==EFI_SUCCESS&&
         ReadConfig(c)==EFI_SUCCESS&&ConfigStable(c)&&
    GuVerifyActivePeers(Plan.table)==EFI_SUCCESS;
}
