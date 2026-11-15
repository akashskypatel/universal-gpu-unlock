/* SPDX-License-Identifier: GPL-2.0-only */
/* One explicitly armed AD102 Stop experiment. Not an unlock, not a release
 * adapter or BIOS patch. Optional Windows handoff is separately built
 * and may run only after the original Stop and peer/console checks pass.
 * Original Stop or Windows start can hang; manual hard power recovery
 * remains necessary if the boot cannot continue. */
#if !defined(GU_EXPERIMENTAL_STOP_OBSERVATION) || defined(GU_GPU_DIAGNOSTIC_ONLY) || \
    defined(GU_AD104_TRANSACTION) || defined(GU_FULL_UNLOCK)
#error Only the separate experimental Stop observer may compile this file
#endif
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#ifdef GU_STOP_WINDOWS_HANDOFF
#include "gpu_windows_handoff.h"
#endif
#include "sha256.h"
#include "format.h"
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#if defined(GU_STOP_BOTH_NVIDIA) && defined(GU_STOP_TARGET_3080)
#error Dual Stop must start with the RTX4090-first target configuration
#endif
#if defined(GU_STOP_BOTH_NVIDIA) && !defined(GU_STOP_WINDOWS_HANDOFF)
#error Dual Stop requires a separately authorized pinned Windows handoff
#endif

typedef struct {
  EFI_HANDLE handle,owner;
  EFI_PCI_IO_PROTOCOL *io;
  EFI_DEVICE_PATH_PROTOCOL *path;
  UINTN path_size;
} GPU;

static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID DriverGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,
                            {0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}};
static EFI_SYSTEM_TABLE *Sys;
static EFI_FILE_PROTOCOL *Root,*Log;
static GPU Cards[3]; /* 0=selected NVIDIA, 1=untouched NVIDIA peer, 2=AMD */
/* Set only by a successful exact GOP inventory on this boot. */
static UINTN AcceptedPeerGopCount=(UINTN)-1;
#ifdef GU_STOP_WINDOWS_HANDOFF
static EFI_HANDLE PreparedWindowsImage=NULL;
#endif
#ifdef GU_STOP_TARGET_3080
/* GA102 original driver and PCI ROM, only on the reviewed MSI B850 board. */
static const char TargetName[]="3080", TargetBdf[]="0000:04:00.0";
static const UINT32 Ids[3]={0x220a10deU,0x268410deU,0x13c01002U};
static const UINT32 Subs[3]={0xb61219daU,0x88e61043U,0x7e561462U};
static const UINTN Buses[3]={4,1,15}, ExactPathBytes[3]={40,28,28};
static const UINTN ExactRomSize=148992U, ExactImageSize=158432U, ExactStopRva=0x678U;
static const char OriginalImageSha[]="a9da4af0bf448feb458f55da926329e135b2739e043dd0bfda69915747b728e9";
static const char * const ExactPaths[3]={
"b96b0411dcd378e6433c609f94f6828ea18d8713f5055e4d76ffe9c4901b2908",
"8e2b676ed8eccbb582ca6a80d6bf39196dcc574a1c3c4cc73d35b53c2d4de312",
"fcb89e38b347b79e6fd126ade23dab174a5c5bfb9bfe7686b1072835986038e0"
};
#else
static const char TargetName[]="4090", TargetBdf[]="0000:01:00.0";
static const UINT32 Ids[3]={0x268410deU,0x220a10deU,0x13c01002U};
static const UINT32 Subs[3]={0x88e61043U,0xb61219daU,0x7e561462U};
static const UINTN Buses[3]={1,4,15}, ExactPathBytes[3]={28,40,28};
static const UINTN ExactRomSize=150016U, ExactImageSize=162016U, ExactStopRva=0x6ecU;
static const char OriginalImageSha[]="0fbd44f3455d4df5ade4bc738dbbc73fbd213487a7456a953af9e8ccbc14c2ba";
static const char * const ExactPaths[3]={
"8e2b676ed8eccbb582ca6a80d6bf39196dcc574a1c3c4cc73d35b53c2d4de312",
"b96b0411dcd378e6433c609f94f6828ea18d8713f5055e4d76ffe9c4901b2908",
"fcb89e38b347b79e6fd126ade23dab174a5c5bfb9bfe7686b1072835986038e0"
};
#endif
#ifdef GU_STOP_BOTH_NVIDIA
static const UINTN PeerExactRomSize=148992U, PeerExactImageSize=158432U;
static const UINTN PeerExactStopRva=0x678U;
static const char PeerOriginalImageSha[]="a9da4af0bf448feb458f55da926329e135b2739e043dd0bfda69915747b728e9";
#endif
static EFI_STATUS Flush(VOID) {
  return Log && Log->Flush ? Log->Flush(Log) : EFI_UNSUPPORTED;
}
static void Say(const char *format,...) {
  char line[360];
  CHAR16 wide[361];
  UINTN i,n;
  va_list args;
  va_start(args,format);
  n=(UINTN)GuFormatV(line,sizeof(line),format,args);
  va_end(args);
  if(n==SIZE_MAX || n>=sizeof(line))return;
  for(i=0;i<n;i++)wide[i]=(CHAR16)(UINT8)line[i];
  wide[n]=0;
  if(Sys && Sys->ConOut && Sys->ConOut->OutputString)
    (VOID)Sys->ConOut->OutputString(Sys->ConOut,wide);
  if(Log && Log->Write) {
    UINTN wrote=n;
    if(EFI_ERROR(Log->Write(Log,&wrote,line)) || wrote!=n) {
      /* A failed persistent write must prevent the Stop attempt. */
      if(Sys && Sys->ConOut && Sys->ConOut->OutputString) {
        static CHAR16 error[]=L"LOG_WRITE_FAILED - do not proceed\r\n";
        (VOID)Sys->ConOut->OutputString(Sys->ConOut,error);
      }
      /* Close log and clear its token. No Stop path can run afterward. */
      if(Log->Close)(VOID)Log->Close(Log);
      Log=NULL;
    }
  }
}
static EFI_STATUS StartLog(EFI_HANDLE image) {
  EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  EFI_STATUS s;
  UINTN i;
#ifdef GU_STOP_BOTH_NVIDIA
  CHAR16 file_name[]=L"\\STOP-BOTH-0000.TXT";
#elif defined(GU_STOP_TARGET_3080)
  CHAR16 file_name[]=L"\\STOP-3080-0000.TXT";
#else
  CHAR16 file_name[]=L"\\STOP-4090-0000.TXT";
#endif
  if(!Sys || !Sys->BootServices)return EFI_INVALID_PARAMETER;
  s=Sys->BootServices->HandleProtocol(image,&LoadedGuid,(VOID **)&loaded);
  if(EFI_ERROR(s)||!loaded)return EFI_UNSUPPORTED;
  s=Sys->BootServices->HandleProtocol(loaded->DeviceHandle,&FsGuid,(VOID **)&fs);
  if(EFI_ERROR(s)||!fs || !fs->OpenVolume)return EFI_UNSUPPORTED;
  s=fs->OpenVolume(fs,&Root);
  if(EFI_ERROR(s)||!Root||!Root->Open||!Root->Close)return EFI_UNSUPPORTED;
  for(i=0;i<10000;i++) {
    EFI_FILE_PROTOCOL *prior=NULL;
    file_name[11]=(CHAR16)('0'+i/1000);
    file_name[12]=(CHAR16)('0'+(i/100)%10);
    file_name[13]=(CHAR16)('0'+(i/10)%10);
    file_name[14]=(CHAR16)('0'+i%10);
    s=Root->Open(Root,&prior,file_name,EFI_FILE_MODE_READ,0);
    if(s==EFI_SUCCESS) {
      if(!prior || !prior->Close || EFI_ERROR(prior->Close(prior)))
        return EFI_DEVICE_ERROR;
      continue;
    }
    if(s!=EFI_NOT_FOUND)return s;
    s=Root->Open(Root,&Log,file_name,
                 EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(EFI_ERROR(s)||!Log||!Log->Write||!Log->Flush||!Log->Close)
      return EFI_UNSUPPORTED;
#ifdef GU_STOP_BOTH_NVIDIA
    Say("LOG_FILE=STOP-BOTH-%04u.TXT EXPERIMENT=DUAL_STOP_ONLY\r\n",(unsigned)i);
#else
    Say("LOG_FILE=STOP-%s-%04u.TXT EXPERIMENT=STOP_ONLY\r\n",TargetName,(unsigned)i);
#endif
    return Flush();
  }
  return EFI_VOLUME_FULL;
}
/* Device paths are firmware-owned: only inspect bounded UEFI nodes. */
static UINTN PathBytes(const EFI_DEVICE_PATH_PROTOCOL *path) {
  UINTN at=0,nodes;
  if(!path)return 0;
  for(nodes=0;nodes<64 && at+4<=192;nodes++) {
    const UINT8 *p=(const UINT8 *)path+at;
    UINTN n=(UINTN)p[2]+((UINTN)p[3]<<8);
    if(n<4 || n>192-at)return 0;
    if(p[0]==0x7f)
      return p[1]==0xff && n==4 ? at+4 : 0;
    at+=n;
  }
  return 0;
}
static int OnController(const GPU *card,const EFI_DEVICE_PATH_PROTOCOL *path) {
  UINTN n=PathBytes(path);
  return n && card->path_size>=10 && n>=card->path_size &&
         !memcmp(card->path,path,card->path_size-4);
}
static EFI_STATUS Owner(GPU *card) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
  UINTN count=0,i,owners=0,by_driver=0,child=0,exclusive=0,invalid=0;
  UINTN slot=card==&Cards[0]?0U:card==&Cards[1]?1U:2U;
  EFI_HANDLE agent=NULL;
  EFI_STATUS s=bs->OpenProtocolInformation(card->handle,&PciGuid,&entries,&count);
  Say("STOP_PREFLIGHT_PCI_OPENS SLOT=%zu STATUS=%#llx COUNT=%zu\r\n",
      (size_t)slot,(unsigned long long)s,(size_t)count);
  if(EFI_ERROR(s)||count>64||(count&&!entries))return EFI_COMPROMISED_DATA;
  for(i=0;i<count;i++) {
    const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[i];
    if(e->Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER)by_driver++;
    if(e->Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)child++;
    if(e->Attributes & EFI_OPEN_PROTOCOL_EXCLUSIVE)exclusive++;
    /* Inspect only: no OpenProtocol/CloseProtocol or driver callback. */
    Say("STOP_PREFLIGHT_PCI_OPEN SLOT=%zu INDEX=%zu ATTRS=%#x COUNT=%u CONTROLLER_MATCH=%u AGENT_NONZERO=%u\r\n",
        (size_t)slot,(size_t)i,(unsigned)e->Attributes,(unsigned)e->OpenCount,
        (unsigned)(e->ControllerHandle==card->handle),
        (unsigned)(e->AgentHandle!=NULL));
    if(e->Attributes==EFI_OPEN_PROTOCOL_BY_DRIVER && e->OpenCount==1 &&
       e->ControllerHandle==card->handle && e->AgentHandle) {
      owners++;agent=e->AgentHandle;
    } else if(e->Attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
      /* The selected NVIDIA must be child-free. Child-controller opens on
       * the untouched AMD or peer device are not second PCI drivers. */
      if(slot==0
#ifdef GU_STOP_BOTH_NVIDIA
          || slot==1
#endif
          || e->OpenCount!=1 || !e->AgentHandle ||
         !e->ControllerHandle || e->ControllerHandle==card->handle)
        invalid=1;
    }
    if(e->Attributes & EFI_OPEN_PROTOCOL_EXCLUSIVE)invalid=1;
  }
  Say("STOP_PREFLIGHT_PCI_OWNER SLOT=%zu BY_DRIVER=%zu VALID_OWNER=%zu CHILD=%zu EXCLUSIVE=%zu INVALID=%zu\r\n",
      (size_t)slot,(size_t)by_driver,(size_t)owners,(size_t)child,(size_t)exclusive,(size_t)invalid);
  if(entries && EFI_ERROR(bs->FreePool(entries)))return EFI_DEVICE_ERROR;
  if(owners!=1 || by_driver!=1 || invalid)return EFI_ACCESS_DENIED;
  card->owner=agent;
  return EFI_SUCCESS;
}
/* A bound child or a second protocol driver on the target is not headless.
 * Read every installed target-protocol open record; do not invoke callbacks. */
static EFI_STATUS TargetHasNoChildrenFor(GPU *target) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_GUID **protocols=NULL;
  UINTN count=0,i,j;
  EFI_STATUS s=bs->ProtocolsPerHandle(target->handle,&protocols,&count);
  Say("STOP_PREFLIGHT_TARGET_PROTOCOLS STATUS=%#llx COUNT=%zu\r\n",
      (unsigned long long)s,(size_t)count);
  if(EFI_ERROR(s)||count<2||count>64||!protocols)return EFI_ACCESS_DENIED;
  for(i=0;i<count;i++) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
    UINTN edges=0;
    if(!protocols[i]) {s=EFI_COMPROMISED_DATA;break;}
    s=bs->OpenProtocolInformation(target->handle,protocols[i],
                                  &entries,&edges);
    Say("STOP_PREFLIGHT_TARGET_PROTOCOL INDEX=%zu PCI=%u STATUS=%#llx OPENS=%zu\r\n",
        (size_t)i,(unsigned)(memcmp(protocols[i],&PciGuid,sizeof(PciGuid))==0),
        (unsigned long long)s,(size_t)edges);
    if(EFI_ERROR(s)||edges>128||(edges&&!entries))s=EFI_COMPROMISED_DATA;
    else for(j=0;j<edges;j++) {
      UINT32 attr=entries[j].Attributes;
      if(attr==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
         attr==EFI_OPEN_PROTOCOL_EXCLUSIVE ||
         (attr==EFI_OPEN_PROTOCOL_BY_DRIVER &&
          memcmp(protocols[i],&PciGuid,sizeof(PciGuid))!=0)) {
        Say("STOP_PREFLIGHT_TARGET_OPEN_BLOCKER PROTOCOL=%zu INDEX=%zu ATTRS=%#x OPEN_COUNT=%u\r\n",
            (size_t)i,(size_t)j,(unsigned)attr,(unsigned)entries[j].OpenCount);
        s=EFI_ACCESS_DENIED;
      }
    }
    if(entries && EFI_ERROR(bs->FreePool(entries)))s=EFI_DEVICE_ERROR;
    if(EFI_ERROR(s))break;
  }
  if(protocols && EFI_ERROR(bs->FreePool(protocols)))s=EFI_DEVICE_ERROR;
  return s;
}
static EFI_STATUS TargetHasNoChildren(VOID) {
  return TargetHasNoChildrenFor(&Cards[0]);
}

static EFI_STATUS Inventory(VOID) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i,found[3]={0},display=0;
  EFI_STATUS s=bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  Say("STOP_PREFLIGHT_PCI_ENUM STATUS=%#llx COUNT=%zu\r\n",
      (unsigned long long)s,(size_t)count);
  if(EFI_ERROR(s)||!handles||!count||count>4096)return EFI_UNSUPPORTED;
  for(i=0;i<count;i++) {
    EFI_PCI_IO_PROTOCOL *io=NULL;
    UINTN seg=0,bus=0,device=0,function=0;
    UINT32 config[16]={0};
    UINTN j;
    s=bs->HandleProtocol(handles[i],&PciGuid,(VOID **)&io);
    if(EFI_ERROR(s)||!io||!io->Pci.Read||!io->GetLocation) {
      Say("STOP_PREFLIGHT_PCI_ERROR INDEX=%zu STAGE=HANDLE_PCI STATUS=%#llx\r\n",
          (size_t)i,(unsigned long long)s);
      break;
    }
    s=io->GetLocation(io,&seg,&bus,&device,&function);
    if(EFI_ERROR(s)) {
      Say("STOP_PREFLIGHT_PCI_ERROR INDEX=%zu STAGE=GET_LOCATION STATUS=%#llx\r\n",
          (size_t)i,(unsigned long long)s);
      break;
    }
    s=io->Pci.Read(io,EfiPciIoWidthUint32,0,16,config);
    if(EFI_ERROR(s)) {
      Say("STOP_PREFLIGHT_PCI_ERROR INDEX=%zu STAGE=READ_CONFIG STATUS=%#llx\r\n",
          (size_t)i,(unsigned long long)s);
      break;
    }
    if((config[2]>>16)!=0x0300U)continue;
    display++;
    for(j=0;j<3;j++)
      if(seg==0 && bus==Buses[j] && device==0 && function==0 &&
         config[0]==Ids[j] && config[11]==Subs[j])break;
    if(j==3 || ++found[j]!=1){s=EFI_SECURITY_VIOLATION;break;}
    Cards[j].io=io;
    Cards[j].handle=handles[i];
    s=bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&Cards[j].path);
    if(EFI_ERROR(s)||!Cards[j].path ||
       !(Cards[j].path_size=PathBytes(Cards[j].path))) {
      Say("STOP_PREFLIGHT_PCI_ERROR INDEX=%zu STAGE=DEVICE_PATH SLOT=%zu STATUS=%#llx\r\n",
          (size_t)i,(size_t)j,(unsigned long long)s);
      break;
    }
  }
  if(handles && EFI_ERROR(bs->FreePool(handles)))s=EFI_DEVICE_ERROR;
  if(EFI_ERROR(s))return s;
  Say("STOP_PREFLIGHT_PCI_IDENTITY_COUNTS DISPLAY=%zu GPU0=%zu GPU1=%zu AMD=%zu\r\n",
      (size_t)display,(size_t)found[0],(size_t)found[1],(size_t)found[2]);
  if(display!=3 || found[0]!=1 || found[1]!=1 || found[2]!=1 ||
     Cards[0].path_size!=ExactPathBytes[0] ||
     Cards[1].path_size!=ExactPathBytes[1] ||
     Cards[2].path_size!=ExactPathBytes[2] || !Cards[0].io->RomImage ||
     Cards[0].io->RomSize!=ExactRomSize
#ifdef GU_STOP_BOTH_NVIDIA
      || !Cards[1].io->RomImage || Cards[1].io->RomSize!=PeerExactRomSize
#endif
      )return EFI_SECURITY_VIOLATION;
  for(i=0;i<3;i++) {
    if(!GuHashMatches(Cards[i].path,Cards[i].path_size,ExactPaths[i]))
      return EFI_SECURITY_VIOLATION;
    s=Owner(&Cards[i]);
    Say("STOP_PREFLIGHT_OWNER_RESULT SLOT=%zu STATUS=%#llx\r\n",
        (size_t)i,(unsigned long long)s);
    if(EFI_ERROR(s))return s;
  }
  s=TargetHasNoChildren();
  Say("STOP_PREFLIGHT_CHILD_RESULT STATUS=%#llx\r\n",(unsigned long long)s);
  return s;
}
static EFI_STATUS Gops(VOID) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i,j,each[3]={0},unknown=0,invalid=0;
  EFI_STATUS s=bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  AcceptedPeerGopCount=(UINTN)-1;
  Say("STOP_PREFLIGHT_GOP_ENUM STATUS=%#llx COUNT=%zu\r\n",
      (unsigned long long)s,(size_t)count);
  if(EFI_ERROR(s)||count==0||count>64||!handles) {
    if(handles && EFI_ERROR(bs->FreePool(handles)))return EFI_DEVICE_ERROR;
    return EFI_ACCESS_DENIED;
  }
  /* The physical 0022 and 0005 AMD-only boots have one AMD GOP.
   * The historic peer+AMD boot has two GOPs. Never allow a target GOP. */
  for(i=0;i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    UINTN n=0,matches=0,match=3;
    EFI_STATUS path_status=bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path);
    if(!EFI_ERROR(path_status))n=PathBytes(path);
    if(!EFI_ERROR(path_status) && n) {
      for(j=0;j<3;j++)if(OnController(&Cards[j],path)) {
        match=j;
        matches++;
      }
    }
    Say("STOP_PREFLIGHT_GOP_ENTRY INDEX=%zu PATH_STATUS=%#llx PATH_BYTES=%zu MATCH_SLOT=%zu MATCH_COUNT=%zu\r\n",
        (size_t)i,(unsigned long long)path_status,(size_t)n,
        (size_t)match,(size_t)matches);
    if(EFI_ERROR(path_status)||!n||matches!=1) {
      invalid++;
      if(!matches)unknown++;
    } else {
      each[match]++;
    }
  }
  Say("STOP_PREFLIGHT_GOP_COUNTS TARGET=%zu PEER=%zu AMD=%zu UNKNOWN=%zu INVALID=%zu TOTAL=%zu EXPECTED_TOTAL=1_OR_2\r\n",
      (size_t)each[0],(size_t)each[1],(size_t)each[2],
      (size_t)unknown,(size_t)invalid,(size_t)count);
  if(EFI_ERROR(bs->FreePool(handles)))return EFI_DEVICE_ERROR;
#ifdef GU_STOP_TARGET_3080
  /* Only physical 0032 AMD-only GOP/console and zero NVIDIA GOPs. */
  if(invalid || count!=1 || each[0]!=0 || each[1]!=0 || each[2]!=1)
    return EFI_ACCESS_DENIED;
#elif defined(GU_STOP_BOTH_NVIDIA)
  if(invalid || count!=1 || each[0]!=0 || each[1]!=0 || each[2]!=1)
    return EFI_ACCESS_DENIED;
#else
  if(invalid || each[0]!=0 || each[2]!=1 ||
     !((count==1 && each[1]==0) || (count==2 && each[1]==1)))
    return EFI_ACCESS_DENIED;
#endif
  AcceptedPeerGopCount=each[1];
  return EFI_SUCCESS;
}
static EFI_STATUS ConsoleVar(CHAR16 *name) {
  UINT8 bytes[512];
  UINTN n=sizeof(bytes),pos=0,start=0,each[3]={0},instances=0;
  UINT32 attr=0;
  EFI_STATUS s=Sys->RuntimeServices->GetVariable(name,&GlobalGuid,
                                                 &attr,&n,bytes);
  Say("STOP_PREFLIGHT_CONSOLE_VARIABLE NAME=%s STATUS=%#llx SIZE=%zu ATTRS=%#x EXPECTED_SIZE=36_OR_84\r\n",
      name[0]=='C'?"ConOut":"ErrOut",(unsigned long long)s,(size_t)n,(unsigned)attr);
  /* Match the two original observed complete console layouts. A single
   * AMD GOP requires a single AMD console instance; the former two-GOP
   * configuration still requires both AMD and peer console instances. */
  if(EFI_ERROR(s)|| (attr & EFI_VARIABLE_BOOTSERVICE_ACCESS)==0 ||
     !((AcceptedPeerGopCount==0 && n==36) ||
       (AcceptedPeerGopCount==1 && n==84)))return EFI_ACCESS_DENIED;
  while(pos<n) {
    UINTN sz,j;
    if(n-pos<4)return EFI_COMPROMISED_DATA;
    sz=(UINTN)bytes[pos+2]+((UINTN)bytes[pos+3]<<8);
    if(sz<4||sz>n-pos)return EFI_COMPROMISED_DATA;
    if(bytes[pos]==0x7f) {
      if(sz!=4 || (bytes[pos+1]!=0x01 && bytes[pos+1]!=0xff))
        return EFI_COMPROMISED_DATA;
      instances++;
      for(j=0;j<3;j++) {
        if(Cards[j].path_size-4<=pos-start &&
           !memcmp(bytes+start,Cards[j].path,Cards[j].path_size-4))
          break;
      }
      if(j==3 || ++each[j]>1)return EFI_ACCESS_DENIED;
      if(bytes[pos+1]==0xff && pos+4!=n)return EFI_COMPROMISED_DATA;
      if(bytes[pos+1]==0x01 && pos+4==n)return EFI_COMPROMISED_DATA;
      start=pos+4;
    }
    pos+=sz;
  }
  Say("STOP_PREFLIGHT_CONSOLE_COUNTS NAME=%s INSTANCES=%zu TARGET=%zu PEER=%zu AMD=%zu\r\n",
      name[0]=='C'?"ConOut":"ErrOut",(size_t)instances,(size_t)each[0],
      (size_t)each[1],(size_t)each[2]);
  return each[0]==0 && each[2]==1 &&
         instances==AcceptedPeerGopCount+1 &&
         each[1]==AcceptedPeerGopCount ? EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS ExactOriginalDriverFor(GPU *card,UINTN image_size,UINTN stop_rva,
                                         const char *sha) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
  EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
  EFI_STATUS s=bs->HandleProtocol(card->owner,&DriverGuid,
                                  (VOID **)&binding);
  if(EFI_ERROR(s)||!binding||binding->ImageHandle!=card->owner ||
     binding->DriverBindingHandle!=card->owner)return EFI_ACCESS_DENIED;
  s=bs->HandleProtocol(card->owner,&LoadedGuid,(VOID **)&image);
  if(EFI_ERROR(s)||!image||!image->ImageBase||
     image->ImageSize!=image_size)return EFI_ACCESS_DENIED;
  if((UINTN)image->ImageBase>(UINTN)-1-image_size ||
     binding->Stop!=(EFI_DRIVER_BINDING_STOP)((UINT8 *)image->ImageBase+stop_rva) ||
     !GuHashMatches(image->ImageBase,(UINTN)image->ImageSize,
                    sha))return EFI_SECURITY_VIOLATION;
  return EFI_SUCCESS;
}
static EFI_STATUS ExactOriginalDriver(VOID) {
  return ExactOriginalDriverFor(&Cards[0],ExactImageSize,ExactStopRva,
                                OriginalImageSha);
}
#ifdef GU_STOP_WINDOWS_HANDOFF
/* Post-Stop Owner() denies both a truly unbound device and an invalid owner
 * graph. For Windows admission distinguish these cases explicitly. */
static EFI_STATUS TargetHasNoDriverAfterStopFor(GPU *target) {
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
  UINTN count=0,i,remaining=0,invalid=0;
  EFI_STATUS s=Sys->BootServices->OpenProtocolInformation(
      target->handle,&PciGuid,&entries,&count);
  if(EFI_ERROR(s) || count==0 || count>64 || !entries) {
    if(entries)(VOID)Sys->BootServices->FreePool(entries);
    return EFI_ACCESS_DENIED;
  }
  for(i=0;i<count;i++) {
    const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[i];
    /* Only observed ordinary GET/HANDLE open references are allowed. */
    if((e->Attributes!=EFI_OPEN_PROTOCOL_GET_PROTOCOL &&
        e->Attributes!=EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL) ||
       !e->AgentHandle || !e->OpenCount || e->ControllerHandle)
      invalid++;
    if(e->Attributes==EFI_OPEN_PROTOCOL_BY_DRIVER ||
       e->Attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
       (e->Attributes & EFI_OPEN_PROTOCOL_EXCLUSIVE))remaining++;
  }
  Say("POST_STOP_TARGET_PCI_UNBOUND STATUS=%#llx OPENS=%zu DRIVER_OR_CHILD=%zu INVALID=%zu\r\n",
      (unsigned long long)s,(size_t)count,
      (size_t)remaining,(size_t)invalid);
  if(EFI_ERROR(Sys->BootServices->FreePool(entries)))return EFI_DEVICE_ERROR;
  return remaining==0 && invalid==0?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS TargetHasNoDriverAfterStop(VOID) {
  return TargetHasNoDriverAfterStopFor(&Cards[0]);
}
#endif
static VOID HaltAfterAttempt(VOID) {
#ifdef GU_STOP_WINDOWS_HANDOFF
  Say("WINDOWS_HANDOFF_NOT_COMPLETED=1 POWER_OFF_REQUIRED=1 DO_NOT_RETRY=1\r\n");
#else
  Say("NO_WINDOWS_HANDOFF=1 POWER_OFF_REQUIRED=1 DO_NOT_RETRY=1\r\n");
#endif
  if(Log) {
    (VOID)Flush();
    (VOID)Log->Close(Log);
    Log=NULL;
  }
  if(Root) {
    (VOID)Root->Close(Root);
    Root=NULL;
  }
  /* Prevent Shell/Boot Manager continuation after any Stop attempt. */
  for(;;)(VOID)Sys->BootServices->Stall(1000000);
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *table) {
  EFI_STATUS s;
  EFI_HANDLE expected_agent,expected_peer,expected_amd;
  if(!table||!table->BootServices||!table->RuntimeServices ||
     !table->ConOut)return EFI_INVALID_PARAMETER;
  Sys=table;
  s=StartLog(image);
  if(EFI_ERROR(s))return s; /* No persistent pre-Stop evidence, no Stop. */
#ifdef GU_STOP_BOTH_NVIDIA
  Say("EXPERIMENT=EXACT_DUAL_NVIDIA_STOP TARGETS=0000:01:00.0,0000:04:00.0 AMD=PROTECTED\r\n");
#else
  Say("EXPERIMENT=EXACT_%s_STOP_ONLY TARGET=%s\r\n",TargetName,TargetBdf);
#endif
#ifdef GU_STOP_BOTH_NVIDIA
  Say("CAN_UNLOCK=0 CAN_FLASH=0 CAN_FLR=0 SELECTED_STOP_COUNT=2 CAN_STOP_AMD=0\r\n");
#else
  Say("CAN_UNLOCK=0 CAN_FLASH=0 CAN_FLR=0 CAN_STOP_OTHER_CARD=0\r\n");
#endif
  s=Inventory();
  Say("IDENTITY_STATUS=%#llx INVENTORY_ACCEPTED=%u\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s))goto refuse;
  s=Gops();
  Say("GOP_STATUS=%#llx GOP_POLICY_ACCEPTED=%u EXPECTED_TARGET_GOP=0 EXPECTED_PEER_GOP=0_OR_1 EXPECTED_AMD_GOP=1\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s)) {
    /* Read-only console context for a refused GOP check. Never proceed
     * to driver validation, confirmation, or Stop after this failure. */
    EFI_STATUS conout=ConsoleVar(L"ConOut");
    EFI_STATUS errout=ConsoleVar(L"ErrOut");
    Say("STOP_PREFLIGHT_CONSOLE_AFTER_GOP_REFUSAL CONOUT=%#llx ERROUT=%#llx\r\n",
        (unsigned long long)conout,(unsigned long long)errout);
    goto refuse;
  }
  s=ConsoleVar(L"ConOut");
  if(s==EFI_SUCCESS)s=ConsoleVar(L"ErrOut");
  Say("CONSOLE_STATUS=%#llx CONSOLE_POLICY_ACCEPTED=%u EXPECTED_TARGET_CONSOLE=0 EXPECTED_PEER=GOP_MATCH EXPECTED_AMD=1\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s))goto refuse;
  s=ExactOriginalDriver();
  Say("ORIGINAL_DRIVER_STATUS=%#llx DRIVER_VERIFIED=%u\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s))goto refuse;
#ifdef GU_STOP_BOTH_NVIDIA
  s=TargetHasNoChildrenFor(&Cards[1]);
  Say("DUAL_SECOND_TARGET_CHILD_PREFLIGHT STATUS=%#llx\r\n",(unsigned long long)s);
  if(EFI_ERROR(s))goto refuse;
  s=ExactOriginalDriverFor(&Cards[1],PeerExactImageSize,PeerExactStopRva,
                           PeerOriginalImageSha);
  Say("DUAL_SECOND_ORIGINAL_DRIVER STATUS=%#llx\r\n",(unsigned long long)s);
  if(EFI_ERROR(s))goto refuse;
#endif
  expected_agent=Cards[0].owner;
  expected_peer=Cards[1].owner;
  expected_amd=Cards[2].owner;
  Say("WARNING: NVIDIA Stop can write GPU registers and hang forever.\r\n");
  Say("On hang or return: hold physical power until OFF, remove AC power,\r\n");
  Say("then restore power. Recovery is NOT verified.\r\n");
  Say("STOP_ARMED_AT_BUILD=1 AUTOMATIC_AFTER_PREFLIGHT=1 NO_KEY_INPUT=1\r\n");
  s=Flush();
  if(EFI_ERROR(s)||!Log)goto refuse;
#ifdef GU_STOP_WINDOWS_HANDOFF
  /* Load the pinned Windows Boot Manager under the firmware's current
   * image-authentication policy. Do not start it before NVIDIA Stop. */
  s=GuWindowsBootManagerPrepare(image,table,&PreparedWindowsImage);
  Say("WINDOWS_BOOT_MANAGER_PREPARED STATUS=%#llx IMAGE_NONZERO=%u\r\n",
      (unsigned long long)s,(unsigned)(PreparedWindowsImage!=NULL));
  if(EFI_ERROR(s)||!PreparedWindowsImage)goto refuse;
#endif
  s=Owner(&Cards[0]);
  if(s==EFI_SUCCESS && Cards[0].owner!=expected_agent)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Owner(&Cards[1]);
  if(s==EFI_SUCCESS && Cards[1].owner!=expected_peer)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Owner(&Cards[2]);
  if(s==EFI_SUCCESS && Cards[2].owner!=expected_amd)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=TargetHasNoChildren();
#ifdef GU_STOP_BOTH_NVIDIA
  if(s==EFI_SUCCESS)s=TargetHasNoChildrenFor(&Cards[1]);
  if(s==EFI_SUCCESS)s=ExactOriginalDriverFor(&Cards[1],
      PeerExactImageSize,PeerExactStopRva,PeerOriginalImageSha);
#endif
  if(s==EFI_SUCCESS)s=Gops();
  if(s==EFI_SUCCESS)s=ConsoleVar(L"ConOut");
  if(s==EFI_SUCCESS)s=ConsoleVar(L"ErrOut");
  if(s==EFI_SUCCESS)s=ExactOriginalDriver();
  if(EFI_ERROR(s))goto refuse;
  Say("STOP_ATTEMPT=IMMINENT TARGET=%s BDF=%s\r\n",TargetName,TargetBdf);
  Say("STOP_SERVICE=DisconnectController DRIVER=PINNED_ORIGINAL IMAGE=SHA_MATCH\r\n");
  s=Flush();
  if(EFI_ERROR(s)||!Log)goto refuse;
  /* The intent log must be CLOSED before the vendor call. A Flush alone
   * does not guarantee the file name survives an abrupt power removal. */
  s=Log->Close(Log);
  Log=NULL;
  if(EFI_ERROR(s))goto refuse;
  s=Root->Close(Root);
  Root=NULL;
  if(EFI_ERROR(s))goto refuse;
  /* This is the ONLY deliberate hardware-mutating call in this image.
   * Do not retry, reconnect, reset the GPU or touch the peer/AMD. */
  s=table->BootServices->DisconnectController(Cards[0].handle,
                                               expected_agent,NULL);
  /* A new numbered file exists only if the original Stop returns. */
  {
    EFI_STATUS postlog=StartLog(image);
    Say("POST_STOP_LOG_CREATE=%#llx\r\n",(unsigned long long)postlog);
#ifdef GU_STOP_BOTH_NVIDIA
    if(EFI_ERROR(postlog)||!Log||!Root)HaltAfterAttempt();
#endif
  }
  Say("STOP_RETURNED=1 EFI_STATUS=%#llx\r\n",(unsigned long long)s);
#ifdef GU_STOP_BOTH_NVIDIA
  /* A failed first Stop, missing log, changed peer or damaged AMD console
   * permanently forbids the second Stop and Windows handover on this boot. */
  if(s!=EFI_SUCCESS)HaltAfterAttempt();
  {
    EFI_STATUS unbound=TargetHasNoDriverAfterStopFor(&Cards[0]);
    EFI_STATUS first_owner=Owner(&Cards[0]);
    EFI_STATUS peer_owner=Owner(&Cards[1]);
    EFI_STATUS amd_owner=Owner(&Cards[2]);
    EFI_STATUS gop=Gops();
    EFI_STATUS conout=ConsoleVar(L"ConOut");
    EFI_STATUS errout=ConsoleVar(L"ErrOut");
    EFI_STATUS child=TargetHasNoChildrenFor(&Cards[1]);
    EFI_STATUS source=ExactOriginalDriverFor(&Cards[1],
        PeerExactImageSize,PeerExactStopRva,PeerOriginalImageSha);
    BOOLEAN safe=unbound==EFI_SUCCESS && first_owner==EFI_ACCESS_DENIED &&
        peer_owner==EFI_SUCCESS && Cards[1].owner==expected_peer &&
        amd_owner==EFI_SUCCESS && Cards[2].owner==expected_amd &&
        gop==EFI_SUCCESS && conout==EFI_SUCCESS && errout==EFI_SUCCESS &&
        child==EFI_SUCCESS && source==EFI_SUCCESS;
    Say("DUAL_FIRST_STOP_CHECK ELIGIBLE=%u FIRST_UNBOUND=%u PEER_OWNER=%u AMD_OWNER=%u AMD_CONSOLE=%u SECOND_CHILD_FREE=%u SECOND_IMAGE_VALID=%u\r\n",
        (unsigned)safe,(unsigned)(unbound==EFI_SUCCESS),
        (unsigned)(peer_owner==EFI_SUCCESS && Cards[1].owner==expected_peer),
        (unsigned)(amd_owner==EFI_SUCCESS && Cards[2].owner==expected_amd),
        (unsigned)(gop==EFI_SUCCESS && conout==EFI_SUCCESS && errout==EFI_SUCCESS),
        (unsigned)(child==EFI_SUCCESS),(unsigned)(source==EFI_SUCCESS));
    if(!safe)HaltAfterAttempt();
  }
  Say("STOP_ATTEMPT=IMMINENT TARGET=3080 BDF=0000:04:00.0\r\n");
  Say("STOP_SERVICE=DisconnectController DRIVER=PINNED_ORIGINAL SECOND_CARD=1\r\n");
  s=Flush();
  if(EFI_ERROR(s)||!Log)HaltAfterAttempt();
  s=Log->Close(Log);
  Log=NULL;
  if(EFI_ERROR(s))HaltAfterAttempt();
  s=Root->Close(Root);
  Root=NULL;
  if(EFI_ERROR(s))HaltAfterAttempt();
  s=table->BootServices->DisconnectController(Cards[1].handle,
                                               expected_peer,NULL);
  {
    EFI_STATUS postlog=StartLog(image);
    Say("DUAL_SECOND_POST_STOP_LOG_CREATE=%#llx\r\n",
        (unsigned long long)postlog);
    if(EFI_ERROR(postlog)||!Log||!Root)HaltAfterAttempt();
  }
  Say("DUAL_SECOND_STOP_RETURNED=1 EFI_STATUS=%#llx\r\n",
      (unsigned long long)s);
#endif
  if(s==EFI_SUCCESS) {
    EFI_STATUS post_target=Owner(&Cards[0]);
    EFI_STATUS post_peer=Owner(&Cards[1]);
    EFI_STATUS post_amd=Owner(&Cards[2]);
    EFI_STATUS post_gop=Gops();
    EFI_STATUS post_conout=ConsoleVar(L"ConOut");
    EFI_STATUS post_errout=ConsoleVar(L"ErrOut");
    Say("POST_STOP_TARGET_PCI_OWNER=%#llx NO_SOLE_OWNER_IF_ACCESS_DENIED=1\r\n",
        (unsigned long long)post_target);
    Say("POST_STOP_PEER_OWNER=%#llx UNCHANGED=%u AMD_OWNER=%#llx UNCHANGED=%u\r\n",
        (unsigned long long)post_peer,
        (unsigned)(post_peer==EFI_SUCCESS && Cards[1].owner==expected_peer),
        (unsigned long long)post_amd,
        (unsigned)(post_amd==EFI_SUCCESS && Cards[2].owner==expected_amd));
    Say("POST_STOP_GOP=%#llx CONOUT=%#llx ERROUT=%#llx\r\n",
        (unsigned long long)post_gop,
        (unsigned long long)post_conout,(unsigned long long)post_errout);
#ifdef GU_STOP_WINDOWS_HANDOFF
    EFI_STATUS post_unbound=TargetHasNoDriverAfterStop();
#ifdef GU_STOP_BOTH_NVIDIA
    EFI_STATUS second_unbound=TargetHasNoDriverAfterStopFor(&Cards[1]);
    BOOLEAN eligible=post_unbound==EFI_SUCCESS &&
        second_unbound==EFI_SUCCESS &&
        post_target==EFI_ACCESS_DENIED && post_peer==EFI_ACCESS_DENIED &&
        post_amd==EFI_SUCCESS && Cards[2].owner==expected_amd &&
        post_gop==EFI_SUCCESS &&
        post_conout==EFI_SUCCESS && post_errout==EFI_SUCCESS &&
        PreparedWindowsImage!=NULL;
    Say("DUAL_POST_STOP_ELIGIBLE=%u GPU4090_UNBOUND=%u GPU3080_UNBOUND=%u AMD_UNCHANGED=%u\r\n",
        (unsigned)eligible,(unsigned)(post_unbound==EFI_SUCCESS),
        (unsigned)(second_unbound==EFI_SUCCESS),
        (unsigned)(post_amd==EFI_SUCCESS && Cards[2].owner==expected_amd));
#else
    BOOLEAN eligible=post_unbound==EFI_SUCCESS &&
        post_target==EFI_ACCESS_DENIED &&
        post_peer==EFI_SUCCESS && Cards[1].owner==expected_peer &&
        post_amd==EFI_SUCCESS && Cards[2].owner==expected_amd &&
        post_gop==EFI_SUCCESS &&
        post_conout==EFI_SUCCESS && post_errout==EFI_SUCCESS &&
        PreparedWindowsImage!=NULL;
#endif
#ifndef GU_STOP_BOTH_NVIDIA
    Say("WINDOWS_HANDOFF_POST_STOP_ELIGIBLE=%u TARGET_UNBOUND=%u PEER_UNCHANGED=%u AMD_UNCHANGED=%u\r\n",
        (unsigned)eligible,(unsigned)(post_unbound==EFI_SUCCESS),
        (unsigned)(post_peer==EFI_SUCCESS && Cards[1].owner==expected_peer),
        (unsigned)(post_amd==EFI_SUCCESS && Cards[2].owner==expected_amd));
#endif
    if(eligible && Log && Root) {
      /* The result record is closed before StartImage, which may never
       * return. Never hand control back to the UEFI Shell after Stop. */
      Say("WINDOWS_HANDOFF=STARTING_PINNED_BOOT_MANAGER\r\n");
      EFI_STATUS record=Flush();
      if(record==EFI_SUCCESS) {
        record=Log->Close(Log);
        Log=NULL;
      }
      if(record==EFI_SUCCESS) {
        record=Root->Close(Root);
        Root=NULL;
      }
      if(record==EFI_SUCCESS) {
        EFI_STATUS started=GuWindowsBootManagerStartPrepared(
            PreparedWindowsImage,table);
        /* A Boot Manager return is a failed/ended handoff, not permission
         * to reconnect firmware drivers or retry the original Stop. */
        (VOID)StartLog(image);
        Say("WINDOWS_HANDOFF_RETURNED=1 EFI_STATUS=%#llx POWER_OFF_REQUIRED=1\r\n",
            (unsigned long long)started);
      } else {
        Say("WINDOWS_HANDOFF_LOG_CLOSE_FAILED=%#llx\r\n",
            (unsigned long long)record);
      }
    }
#endif
  }
  HaltAfterAttempt();
refuse:
  Say("STOP_ATTEMPT=REFUSED STATUS=%#llx GPU_MUTATION=NONE\r\n",
      (unsigned long long)s);
  (VOID)Flush();
  if(Log && Log->Close)(VOID)Log->Close(Log);
  if(Root && Root->Close)(VOID)Root->Close(Root);
  return EFI_ABORTED;
}