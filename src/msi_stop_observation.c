/* SPDX-License-Identifier: GPL-2.0-only */
/* One explicitly armed AD102 Stop experiment. Not an unlock, not a release
 * adapter, not a BIOS patch. A post-Stop return is NEVER Windows handoff.
 * Firmware may hang inside original NVIDIA Stop without a local timeout.
 * A manual complete power-off is required after the single attempt. */
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
#include "sha256.h"
#include "format.h"
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

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
static GPU Cards[3]; /* 0=4090, 1=3080, 2=AMD; exact identities required */
static const UINT32 Ids[3]={0x268410deU,0x220a10deU,0x13c01002U};
static const UINT32 Subs[3]={0x88e61043U,0xb61219daU,0x7e561462U};
static const UINTN Buses[3]={1,4,15};
static const UINTN RomSize4090=150016U;
static const char Image4090Sha[]=
  "0fbd44f3455d4df5ade4bc738dbbc73fbd213487a7456a953af9e8ccbc14c2ba";
/* Exact controller paths captured in physical diagnostic 0029. */
static const char * const ExactPaths[3]={
  "8e2b676ed8eccbb582ca6a80d6bf39196dcc574a1c3c4cc73d35b53c2d4de312",
  "b96b0411dcd378e6433c609f94f6828ea18d8713f5055e4d76ffe9c4901b2908",
  "fcb89e38b347b79e6fd126ade23dab174a5c5bfb9bfe7686b1072835986038e0"
};

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
  CHAR16 file_name[]=L"\\STOP-4090-0000.TXT";
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
    Say("LOG_FILE=STOP-4090-%04u.TXT EXPERIMENT=STOP_ONLY\r\n",(unsigned)i);
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
      /* The selected 4090 must be child-free. Child-controller opens on
       * the untouched AMD or 3080 device are not second PCI drivers. */
      if(slot==0 || e->OpenCount!=1 || !e->AgentHandle ||
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
static EFI_STATUS TargetHasNoChildren(VOID) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_GUID **protocols=NULL;
  UINTN count=0,i,j;
  EFI_STATUS s=bs->ProtocolsPerHandle(Cards[0].handle,&protocols,&count);
  Say("STOP_PREFLIGHT_TARGET_PROTOCOLS STATUS=%#llx COUNT=%zu\r\n",
      (unsigned long long)s,(size_t)count);
  if(EFI_ERROR(s)||count<2||count>64||!protocols)return EFI_ACCESS_DENIED;
  for(i=0;i<count;i++) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
    UINTN edges=0;
    if(!protocols[i]) {s=EFI_COMPROMISED_DATA;break;}
    s=bs->OpenProtocolInformation(Cards[0].handle,protocols[i],
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
     Cards[0].path_size!=28 || Cards[1].path_size!=40 ||
     Cards[2].path_size!=28 || !Cards[0].io->RomImage ||
     Cards[0].io->RomSize!=RomSize4090)return EFI_SECURITY_VIOLATION;
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
  Say("STOP_PREFLIGHT_GOP_ENUM STATUS=%#llx COUNT=%zu\r\n",
      (unsigned long long)s,(size_t)count);
  if(EFI_ERROR(s)||count==0||count>64||!handles) {
    if(handles && EFI_ERROR(bs->FreePool(handles)))return EFI_DEVICE_ERROR;
    return EFI_ACCESS_DENIED;
  }
  /* Inspect all bounded GOP handles even if the total is not two. The
   * admission condition below is unchanged: no 4090 GOP, one peer, one AMD. */
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
  Say("STOP_PREFLIGHT_GOP_COUNTS TARGET=%zu PEER=%zu AMD=%zu UNKNOWN=%zu INVALID=%zu TOTAL=%zu EXPECTED_TOTAL=2\r\n",
      (size_t)each[0],(size_t)each[1],(size_t)each[2],
      (size_t)unknown,(size_t)invalid,(size_t)count);
  if(EFI_ERROR(bs->FreePool(handles)))return EFI_DEVICE_ERROR;
  return count==2 && invalid==0 &&
         each[0]==0 && each[1]==1 && each[2]==1 ?
         EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS ConsoleVar(CHAR16 *name) {
  UINT8 bytes[512];
  UINTN n=sizeof(bytes),pos=0,start=0,each[3]={0},instances=0;
  UINT32 attr=0;
  EFI_STATUS s=Sys->RuntimeServices->GetVariable(name,&GlobalGuid,
                                                 &attr,&n,bytes);
  Say("STOP_PREFLIGHT_CONSOLE_VARIABLE NAME=%s STATUS=%#llx SIZE=%zu ATTRS=%#x EXPECTED_SIZE=84\r\n",
      name[0]=='C'?"ConOut":"ErrOut",(unsigned long long)s,(size_t)n,(unsigned)attr);
  if(EFI_ERROR(s)||n!=84 || (attr & EFI_VARIABLE_BOOTSERVICE_ACCESS)==0)
    return EFI_ACCESS_DENIED;
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
  return instances==2 && each[0]==0 && each[1]==1 && each[2]==1 ?
         EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS ExactOriginalDriver(VOID) {
  EFI_BOOT_SERVICES *bs=Sys->BootServices;
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
  EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
  EFI_STATUS s=bs->HandleProtocol(Cards[0].owner,&DriverGuid,
                                  (VOID **)&binding);
  if(EFI_ERROR(s)||!binding||binding->ImageHandle!=Cards[0].owner ||
     binding->DriverBindingHandle!=Cards[0].owner)return EFI_ACCESS_DENIED;
  s=bs->HandleProtocol(Cards[0].owner,&LoadedGuid,(VOID **)&image);
  if(EFI_ERROR(s)||!image||!image->ImageBase||
     image->ImageSize!=162016U)return EFI_ACCESS_DENIED;
  if((UINTN)image->ImageBase>(UINTN)-1-162016U ||
     binding->Stop!=(EFI_DRIVER_BINDING_STOP)((UINT8 *)image->ImageBase+0x6ecU) ||
     !GuHashMatches(image->ImageBase,(UINTN)image->ImageSize,
                    Image4090Sha))return EFI_SECURITY_VIOLATION;
  return EFI_SUCCESS;
}
static EFI_STATUS KeyY(VOID) {
  EFI_INPUT_KEY key;
  EFI_STATUS s;
  UINTN event=0;
  if(!Sys->ConIn || !Sys->ConIn->ReadKeyStroke ||
     !Sys->ConIn->WaitForKey || !Sys->BootServices->WaitForEvent)
    return EFI_UNSUPPORTED;
  s=Sys->BootServices->WaitForEvent(1,&Sys->ConIn->WaitForKey,&event);
  if(EFI_ERROR(s))return s;
  s=Sys->ConIn->ReadKeyStroke(Sys->ConIn,&key);
  return s==EFI_SUCCESS && (key.UnicodeChar=='Y' || key.UnicodeChar=='y')
         ? EFI_SUCCESS:EFI_ABORTED;
}
static VOID HaltAfterAttempt(VOID) {
  Say("NO_WINDOWS_HANDOFF=1 POWER_OFF_REQUIRED=1 DO_NOT_RETRY=1\r\n");
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
  Say("EXPERIMENT=EXACT_4090_STOP_ONLY TARGET=0000:01:00.0\r\n");
  Say("CAN_UNLOCK=0 CAN_FLASH=0 CAN_FLR=0 CAN_STOP_OTHER_CARD=0\r\n");
  s=Inventory();
  Say("IDENTITY_STATUS=%#llx INVENTORY_ACCEPTED=%u\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s))goto refuse;
  s=Gops();
  Say("GOP_STATUS=%#llx GOP_POLICY_ACCEPTED=%u EXPECTED_TARGET_GOP=0 EXPECTED_PEER_GOP=1 EXPECTED_AMD_GOP=1\r\n",
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
  Say("CONSOLE_STATUS=%#llx CONSOLE_POLICY_ACCEPTED=%u EXPECTED_TARGET_CONSOLE=0 EXPECTED_PEER_AND_AMD=1\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s))goto refuse;
  s=ExactOriginalDriver();
  Say("ORIGINAL_DRIVER_STATUS=%#llx DRIVER_VERIFIED=%u\r\n",
      (unsigned long long)s,(unsigned)(s==EFI_SUCCESS));
  if(EFI_ERROR(s))goto refuse;
  expected_agent=Cards[0].owner;
  expected_peer=Cards[1].owner;
  expected_amd=Cards[2].owner;
  Say("WARNING: NVIDIA Stop can write GPU registers and hang forever.\r\n");
  Say("On hang or return: hold physical power until OFF, remove AC power,\r\n");
  Say("then restore power. Recovery is NOT verified. Press Y to attempt once;\r\n");
  Say("any other key cancels without a Stop call.\r\n");
  s=Flush();
  if(EFI_ERROR(s))goto refuse;
  s=KeyY();
  if(EFI_ERROR(s))goto refuse;
  s=Owner(&Cards[0]);
  if(s==EFI_SUCCESS && Cards[0].owner!=expected_agent)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Owner(&Cards[1]);
  if(s==EFI_SUCCESS && Cards[1].owner!=expected_peer)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Owner(&Cards[2]);
  if(s==EFI_SUCCESS && Cards[2].owner!=expected_amd)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=TargetHasNoChildren();
  if(s==EFI_SUCCESS)s=Gops();
  if(s==EFI_SUCCESS)s=ConsoleVar(L"ConOut");
  if(s==EFI_SUCCESS)s=ConsoleVar(L"ErrOut");
  if(s==EFI_SUCCESS)s=ExactOriginalDriver();
  if(EFI_ERROR(s))goto refuse;
  Say("STOP_ATTEMPT=IMMINENT TARGET=4090 BDF=0000:01:00.0\r\n");
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
   * Do not retry, reconnect, reset the GPU or touch the RTX 3080/AMD. */
  s=table->BootServices->DisconnectController(Cards[0].handle,
                                               expected_agent,NULL);
  /* This second file exists only if the original Stop returns. */
  {
    EFI_STATUS postlog=StartLog(image);
    Say("POST_STOP_LOG_CREATE=%#llx\r\n",(unsigned long long)postlog);
  }
  Say("STOP_RETURNED=1 EFI_STATUS=%#llx\r\n",(unsigned long long)s);
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