/* SPDX-License-Identifier: GPL-2.0-only */

/* USB log/assets only. Windows ESP is opened read-only. No boot variables. */

#include "boot_io.h"
#include "version.h"
#include "gpu_windows_path.h"

#include "boot_profile.h"

#include "compat/compat.h"

#include "format.h"

#include "profile.h"

#include "sha256.h"

#include <Protocol/LoadedImage.h>

#include <Protocol/SimpleFileSystem.h>

#include <Guid/FileInfo.h>



static EFI_SYSTEM_TABLE *Table;

static EFI_HANDLE Self,Windows,OwnVolume,TargetEspVolume;

static EFI_FILE_PROTOCOL *Root,*Log;
static EFI_HANDLE *UnreleasedFilesystemHandles;
/* At most five live file references: USB root/log, retained match root,
 * current candidate root and candidate file. Stop opening on failure. */
static EFI_FILE_PROTOCOL *UnreleasedBootFiles[5];
static UINTN UnreleasedBootFileCount;
static EFI_STATUS BootFileStatus;
/* Windows staging owns at most two temporary buffers, independent of GPU selection.
 * Retain uncertain addresses for diagnosis only. Never dereference or retry them. */
static VOID *UnreleasedBootBuffers[2];
static UINTN UnreleasedBootBufferCount;
static CHAR16 ActiveLogPath[]=L"\\AD102-UNLOCK\\RUN0000.TXT";

static BOOLEAN Silent;
/* The platform owns the asset table and its one-attempt latch. */

static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,InfoGuid=EFI_FILE_INFO_ID;

static const EFI_GUID WindowsPartition=GU_WINDOWS_PARTITION_INITIALIZER;

static const CHAR16 WindowsName[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

static const CHAR16 * const WindowsCandidates[] = {
  L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
  L"EFI\\Microsoft\\Boot\\bootmgfw.efi"
};

static UINTN GuStrLen(const CHAR16 *s) {
  UINTN len = 0;
  while (s && s[len]) len++;
  return len;
}



static BOOLEAN Inspect(const EFI_DEVICE_PATH_PROTOCOL *p,UINTN *size,BOOLEAN *usb,BOOLEAN *windows) {
  UINTN at=0;*usb=FALSE;*windows=FALSE;
  if(!p)return FALSE;
  for(UINTN nodes=0;nodes<64&&at+4<=4096;nodes++) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>4096-at)return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE) {
      if(n!=4||p->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE)return FALSE;
      *size=at+4;return TRUE;
    }
    if(p->Type==MESSAGING_DEVICE_PATH&&p->SubType==MSG_USB_DP&&n>=sizeof(USB_DEVICE_PATH))*usb=TRUE;
    if(p->Type==MEDIA_DEVICE_PATH) {
      *windows=TRUE;
    }
    at+=n;p=(const VOID *)((const UINT8 *)p+n);
  }
  return FALSE;
}

VOID GuBootSay(const CHAR16 *text) {

  if(!Silent&&Table&&Table->ConOut&&Table->ConOut->OutputString)

    Table->ConOut->OutputString(Table->ConOut,(CHAR16 *)text);

}

VOID GuBootSilence(VOID){Silent=TRUE;}

VOID GuBootUnsilence(VOID){Silent=FALSE;}

VOID GuLog(const char *format,...) {

  if(!Log)return;

  CHAR8 buffer[2048];va_list args;va_start(args,format);

  size_t n=GuFormatV(buffer,sizeof(buffer),format,args);va_end(args);

  if(n==(size_t)-1||n>=sizeof(buffer))return;

  UINTN bytes=n;EFI_STATUS s=Log->Write(Log,&bytes,buffer);

  if(s==EFI_SUCCESS&&bytes==n)Log->Flush(Log);

}

VOID GuLogText(const char *text){GuLog("%s",text);}

BOOLEAN GuBootLogOpen(VOID){return Log!=NULL;}
EFI_HANDLE GuBootGetTargetEspVolume(VOID){return TargetEspVolume;}
const CHAR16 *GuBootGetLogPath(VOID){return Log?ActiveLogPath:NULL;}

/* Consume each local reference once. Retain failed reference addresses
 * for diagnosis only; never dereference or retry them. */
static EFI_STATUS CloseBootFile(EFI_FILE_PROTOCOL **file) {
  if(!file||!*file)return EFI_SUCCESS;
  EFI_FILE_PROTOCOL *f=*file;
  *file=NULL;
  EFI_STATUS s=f->Close?f->Close(f):EFI_UNSUPPORTED;
  if(s!=EFI_SUCCESS) {
    if(UnreleasedBootFileCount<sizeof(UnreleasedBootFiles)/sizeof(UnreleasedBootFiles[0]))
      UnreleasedBootFiles[UnreleasedBootFileCount++]=f;
    BootFileStatus=EFI_DEVICE_ERROR;
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS GuBootOpen(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {

  if(Table)return EFI_ALREADY_STARTED;

  if(!image||!t||!t->BootServices)return EFI_INVALID_PARAMETER;

  Table=t;Self=image;

  EFI_BOOT_SERVICES *b=t->BootServices;EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;

  EFI_STATUS s=b->HandleProtocol(image,&LoadedGuid,(VOID **)&loaded);

  if(s!=EFI_SUCCESS||!loaded||!loaded->DeviceHandle)return EFI_UNSUPPORTED;

  OwnVolume=loaded->DeviceHandle;s=b->HandleProtocol(OwnVolume,&PathGuid,(VOID **)&path);

  UINTN length=0;BOOLEAN usb=FALSE,windows=FALSE;

  if(s!=EFI_SUCCESS||!Inspect(path,&length,&usb,&windows)||!usb)return EFI_ACCESS_DENIED;

  s=b->HandleProtocol(OwnVolume,&FsGuid,(VOID **)&fs);if(s!=EFI_SUCCESS||!fs)return EFI_UNSUPPORTED;

  s=fs->OpenVolume(fs,&Root);if(s!=EFI_SUCCESS||!Root){s=EFI_DEVICE_ERROR;goto failed;}

  EFI_FILE_PROTOCOL *dir=NULL;

  s=Root->Open(Root,&dir,L"\\AD102-UNLOCK",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);

  if(s!=EFI_SUCCESS||!dir){s=EFI_DEVICE_ERROR;goto failed;}

  for(UINTN slot=1;slot<=9999;slot++) {

    CHAR16 name[]=L"RUN0000.TXT";UINTN number=slot;

    for(UINTN at=7;at>3;){at--;name[at]=(CHAR16)('0'+number%10);number/=10;}

    EFI_FILE_PROTOCOL *existing=NULL;s=dir->Open(dir,&existing,name,EFI_FILE_MODE_READ,0);

    if(s==EFI_SUCCESS&&existing){s=CloseBootFile(&existing);if(s!=EFI_SUCCESS)break;continue;}

    if(s!=EFI_NOT_FOUND)break;

    s=dir->Open(dir,&Log,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(s==EFI_SUCCESS&&Log) {
      for(UINTN k=0;k<sizeof(name)/sizeof(CHAR16);k++) ActiveLogPath[14+k]=name[k];
    }
    break;

  }

  EFI_STATUS closed=CloseBootFile(&dir);
  if(closed!=EFI_SUCCESS){s=closed;goto failed;}
  if(s!=EFI_SUCCESS||!Log){if(s==EFI_SUCCESS)s=EFI_OUT_OF_RESOURCES;goto failed;}

#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  /* Preparation reads PCI identity and writes only its removable USB log.
   * Do not change the firmware watchdog or any active boot state. */
  return EFI_SUCCESS;
#endif
  if(!b->SetWatchdogTimer){s=EFI_UNSUPPORTED;goto failed;}
  s=b->SetWatchdogTimer(0,0,0,NULL);
  if(s==EFI_SUCCESS)return s;
failed:
  {
    EFI_STATUS released=GuBootClose();
    return released==EFI_SUCCESS?s:released;
  }
}

/* Boot-wide CPU memory. The caller retains each confirmed reference until
 * one checked release. An error with nonnull output is uncertain ownership. */
static EFI_STATUS BootBufferFailure(VOID *address) {
  if(UnreleasedBootBufferCount<sizeof(UnreleasedBootBuffers)/sizeof(UnreleasedBootBuffers[0]))
    UnreleasedBootBuffers[UnreleasedBootBufferCount++]=address;
  BootFileStatus=EFI_DEVICE_ERROR;
  return BootFileStatus;
}
static EFI_STATUS AllocateBootBuffer(EFI_BOOT_SERVICES *bs,UINTN size,VOID **out) {
  if(!out||*out||!size)return EFI_INVALID_PARAMETER;
  if(BootFileStatus!=EFI_SUCCESS)return BootFileStatus;
  if(!bs||!bs->AllocatePool||!bs->FreePool)return EFI_UNSUPPORTED;
  VOID *p=NULL;
  EFI_STATUS s=bs->AllocatePool(EfiLoaderData,size,&p);
  if(s!=EFI_SUCCESS)return p?BootBufferFailure(p):s;
  if(!p)return EFI_OUT_OF_RESOURCES;
  *out=p;
  return EFI_SUCCESS;
}
static EFI_STATUS ReleaseBootBuffer(EFI_BOOT_SERVICES *bs,VOID **out) {
  if(!out||!*out)return EFI_SUCCESS;
  VOID *p=*out;
  *out=NULL;
  if(!bs||!bs->FreePool||bs->FreePool(p)!=EFI_SUCCESS)
    return BootBufferFailure(p);
  return EFI_SUCCESS;
}

static EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *root,const CHAR16 *name,UINTN expected,VOID **data) {

  *data=NULL;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  if(BootFileStatus!=EFI_SUCCESS)return BootFileStatus;

  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);

  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;

  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);

  EFI_FILE_INFO *info=(VOID *)info_words;

  if(s==EFI_SUCCESS&&(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||

     info->FileSize!=expected||(info->Attribute&EFI_FILE_DIRECTORY)))s=EFI_COMPROMISED_DATA;

  VOID *p=NULL;

  if(s==EFI_SUCCESS){p=GuAlloc(expected);if(!p)s=EFI_OUT_OF_RESOURCES;}

  for(UINTN at=0;s==EFI_SUCCESS&&at<expected;) {

    UINTN n=expected-at;if(n>1048576)n=1048576;UINTN wanted=n;

    s=f->Read(f,&n,(UINT8 *)p+at);if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;at+=n;

  }

  if(s==EFI_SUCCESS){UINT8 tail;UINTN n=1;s=f->Read(f,&n,&tail);if(s==EFI_SUCCESS&&n)s=EFI_COMPROMISED_DATA;}

  EFI_STATUS closed=CloseBootFile(&f);if(closed!=EFI_SUCCESS)s=closed;

  if(s!=EFI_SUCCESS){GuFree(p);return s;}*data=p;return EFI_SUCCESS;

}

/* Both asset fallback and Windows staging use the same boot-local census.
 * A failed release is not a missing file and must never trigger a fallback. */
static EFI_STATUS ReleaseBootFilesystemHandles(EFI_BOOT_SERVICES *bs,EFI_HANDLE **handles) {
  if(!handles||!*handles)return EFI_SUCCESS;
  EFI_HANDLE *buffer=*handles;
  *handles=NULL;
  EFI_STATUS s=bs->FreePool(buffer);
  if(s!=EFI_SUCCESS) {
    UnreleasedFilesystemHandles=buffer;
    GuLog("FILESYSTEM_HANDLE_BUFFER_FREE_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS LocateBootFilesystems(EFI_BOOT_SERVICES *bs,EFI_HANDLE **handles,UINTN *count) {
  if(!handles||!count)return EFI_INVALID_PARAMETER;
  *handles=NULL;*count=0;
  if(UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS)return EFI_NOT_READY;
  if(!bs||!bs->LocateHandleBuffer||!bs->FreePool)return EFI_UNSUPPORTED;
  EFI_STATUS s=bs->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,count,handles);
  if(s!=EFI_SUCCESS){*handles=NULL;*count=0;return s;}
  if(!*handles||!*count||*count>128U) {
    s=ReleaseBootFilesystemHandles(bs,handles);
    *count=0;
    return s==EFI_SUCCESS?EFI_UNSUPPORTED:s;
  }
  return EFI_SUCCESS;
}

/* Only the explicitly configured Windows GPT ESP may supply a missing
 * firmware file. Never search arbitrary partitions or write to this ESP. */
static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(const GU_ARCH_ASSET *asset,
                                                    const CHAR16 *path,VOID **out) {
  if(!asset||!path||!out||!Table||!Table->BootServices)return EFI_INVALID_PARAMETER;
  *out=NULL;
  EFI_BOOT_SERVICES *b=Table->BootServices;
  EFI_HANDLE *handles=NULL,matched=NULL;
  UINTN count=0,matches=0;
  EFI_STATUS s=LocateBootFilesystems(b,&handles,&count);
  if(s!=EFI_SUCCESS)return s;
  for(UINTN i=0;i<count;i++) {
    if(handles[i]==OwnVolume)continue;
    EFI_DEVICE_PATH_PROTOCOL *dp=NULL;
    UINTN bytes=0;BOOLEAN usb=FALSE,media=FALSE;
    if(b->HandleProtocol(handles[i],&PathGuid,(VOID **)&dp)==EFI_SUCCESS&&
       Inspect(dp,&bytes,&usb,&media)&&!usb&&
       GuWindowsEspPathMatches((const uint8_t *)dp,bytes,
                              (const uint8_t *)&WindowsPartition)) {
      matches++;
      matched=handles[i];
    }
  }
  EFI_STATUS freed=ReleaseBootFilesystemHandles(b,&handles);
  if(freed!=EFI_SUCCESS)return freed;
  if(matches!=1U)return matches?EFI_UNSUPPORTED:EFI_NOT_FOUND;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  s=b->HandleProtocol(matched,&FsGuid,(VOID **)&fs);
  if(s!=EFI_SUCCESS||!fs)return EFI_UNSUPPORTED;
  EFI_FILE_PROTOCOL *root=NULL;
  s=fs->OpenVolume(fs,&root);
  if(s!=EFI_SUCCESS||!root)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  s=ReadFile(root,path,asset->bytes,out);
  EFI_STATUS closed=CloseBootFile(&root);
  if(closed!=EFI_SUCCESS)s=closed;
  if(s==EFI_SUCCESS&&!GuHashMatches(*out,asset->bytes,asset->sha256))
    s=EFI_CRC_ERROR;
  if(s!=EFI_SUCCESS&&*out){GuFree(*out);*out=NULL;}
  return s;
}

EFI_STATUS GuBootLoadAssets(VOID) {
  if(!Root||UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS||Gu.started||Gu.fault||Gu.retained||Gu.asset_load_attempted)
    return EFI_NOT_READY;
  /* Do not overwrite or silently free data published by another owner. */
  for(UINTN i=0;i<GU_ASSET_COUNT;i++)
    if(GuAssets[i].name||GuAssets[i].firmware.data||
       GuAssets[i].firmware.size)return EFI_ALREADY_STARTED;
  const GU_ARCH_ASSETS *arch=GuGetArchAssets();
  if(!arch)return EFI_UNSUPPORTED;
#ifdef GU_ACTIVE_SINGLE_TARGET
  /* Build an independently owned immutable firmware table first. */
  EFI_STATUS reserved=GuAssetReserveEntries(Gu.bs);
  if(reserved!=EFI_SUCCESS)return reserved;
#endif
  Gu.asset_load_attempted=TRUE; /* Latch before the first file operation. */
  EFI_STATUS status=EFI_SUCCESS;
  for(UINTN i=0;i<GU_ASSET_COUNT;i++) {
    const GU_ARCH_ASSET *a=&arch->entries[i];
    VOID *p=NULL;
    status=ReadFile(Root,a->path,a->bytes,&p);
    if(status==EFI_NOT_FOUND&&i==2&&arch->alternate_gsp_path)
      status=ReadFile(Root,arch->alternate_gsp_path,a->bytes,&p);
    if(status==EFI_NOT_FOUND)
      status=ReadPinnedAssetFromConfiguredEsp(a,a->path,&p);
    if(status==EFI_NOT_FOUND&&i==2&&arch->alternate_gsp_path)
      status=ReadPinnedAssetFromConfiguredEsp(a,arch->alternate_gsp_path,&p);
    if(status!=EFI_SUCCESS) {
      GuLog("Pinned asset unavailable from USB/configured ESP: %s status=%#llx\n",
            a->name,(unsigned long long)status);
      break;
    }
    /* Verify every fallback immediately; do not accept matching filenames
     * from the internal partition without the original signed-data digest. */
    if(!GuHashMatches(p,a->bytes,a->sha256)) {
      GuFree(p);
      status=EFI_CRC_ERROR;
      break;
    }
    GuAssets[i].name=a->name;
    GuAssets[i].firmware=(struct firmware){a->bytes,p};
  }
  if(status==EFI_SUCCESS&&!GuAd104AssetsValid(&Gu))status=EFI_CRC_ERROR;
  if(status!=EFI_SUCCESS) {
    for(UINTN i=0;i<GU_ASSET_COUNT;i++) {
      if(GuAssets[i].firmware.data)GuFree((VOID *)GuAssets[i].firmware.data);
      GuAssets[i].name=NULL;
      GuAssets[i].firmware=(struct firmware){0,NULL};
    }
  }
  return status;
}

static EFI_STATUS ReadWindowsBinary(EFI_BOOT_SERVICES *bs,EFI_FILE_PROTOCOL *root,const CHAR16 *name,UINTN *file_size,VOID **data) {

  *data=NULL;*file_size=0;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  if(BootFileStatus!=EFI_SUCCESS)return BootFileStatus;

  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);

  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;

  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);

  EFI_FILE_INFO *info=(VOID *)info_words;

  UINTN sz=0;
  if(s==EFI_SUCCESS) {
    if(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||
       info->FileSize<65536||info->FileSize>33554432||
       (info->Attribute&EFI_FILE_DIRECTORY))
      s=EFI_COMPROMISED_DATA;
    else
      sz=(UINTN)info->FileSize;
  }
  VOID *p=NULL;

  if(s==EFI_SUCCESS)s=AllocateBootBuffer(bs,sz,&p);

  for(UINTN at=0;s==EFI_SUCCESS&&at<sz;) {

    UINTN n=sz-at;if(n>1048576)n=1048576;UINTN wanted=n;

    s=f->Read(f,&n,(UINT8 *)p+at);if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;at+=n;

  }

  EFI_STATUS closed=CloseBootFile(&f);if(closed!=EFI_SUCCESS)s=closed;

  if(s==EFI_SUCCESS&&(((UINT8 *)p)[0]!='M'||((UINT8 *)p)[1]!='Z'))s=EFI_COMPROMISED_DATA;
  if(s!=EFI_SUCCESS) {
    EFI_STATUS released=ReleaseBootBuffer(bs,&p);
    return released==EFI_SUCCESS?s:released;
  }
  *data=p;*file_size=sz;return EFI_SUCCESS;

}

EFI_STATUS GuBootLoadWindows(VOID) {
  if(Windows||!Table||UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS||Gu.started||TargetEspVolume)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Table->BootServices;EFI_HANDLE *h=NULL,volume=NULL;UINTN count=0,matches=0,path_bytes=0;
  EFI_DEVICE_PATH_PROTOCOL *base=NULL;
  const CHAR16 *selected_name=NULL;
  EFI_STATUS s=LocateBootFilesystems(b,&h,&count);
  if(s!=EFI_SUCCESS)return s;
  EFI_FILE_PROTOCOL *matched_root=NULL;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(h[i]==OwnVolume) continue;
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;UINTN bytes=0;BOOLEAN usb=FALSE,windows=FALSE;
    if(b->HandleProtocol(h[i],&PathGuid,(VOID **)&p)==EFI_SUCCESS&&
       Inspect(p,&bytes,&usb,&windows)&&!usb&&
       GuWindowsEspPathMatches((const uint8_t *)p,bytes,(const uint8_t *)&WindowsPartition)) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *tfs=NULL;EFI_FILE_PROTOCOL *troot=NULL;
      if(b->HandleProtocol(h[i],&FsGuid,(VOID **)&tfs)==EFI_SUCCESS&&tfs&&tfs->OpenVolume(tfs,&troot)==EFI_SUCCESS&&troot) {
        const CHAR16 *found_name=NULL;
        for(UINTN c_idx=0;c_idx<sizeof(WindowsCandidates)/sizeof(WindowsCandidates[0]);c_idx++) {
          EFI_FILE_PROTOCOL *tf=NULL;
          if(troot->Open(troot,&tf,(CHAR16 *)WindowsCandidates[c_idx],EFI_FILE_MODE_READ,0)==EFI_SUCCESS&&tf) {
            s=CloseBootFile(&tf);
            if(s!=EFI_SUCCESS)break;
            found_name=WindowsCandidates[c_idx];
            break;
          }
        }
        if(found_name) {
          if(!matches) {
            volume=h[i]; base=p; path_bytes=bytes; matched_root=troot; troot=NULL;
            selected_name=found_name;
          }
          matches++;
          GuLog("GuBootLoadWindows: candidate %zu volume=%p found '%ls' (match #%zu)\n", i, (void *)h[i], found_name, matches);
        }
        EFI_STATUS closed=CloseBootFile(&troot);
        if(closed!=EFI_SUCCESS)s=closed;
      }
    }
  }
  EFI_STATUS released=ReleaseBootFilesystemHandles(b,&h);
  if(released!=EFI_SUCCESS)s=released;
  if(s!=EFI_SUCCESS||matches<1||!volume||!matched_root||!selected_name) {
    EFI_STATUS closed=CloseBootFile(&matched_root);
    if(closed!=EFI_SUCCESS)s=closed;
    GuLog("GuBootLoadWindows failed: matches=%zu volume=%p s=%#llx\n", (size_t)matches, (void *)volume, (unsigned long long)s);
    return s==EFI_SUCCESS?EFI_NOT_FOUND:s;
  }
  if(matches!=1) {
    GuLog("GuBootLoadWindows refused: pinned partition maps to %zu distinct filesystem handles\n",(size_t)matches);
    EFI_STATUS closed=CloseBootFile(&matched_root);
    return closed==EFI_SUCCESS?EFI_UNSUPPORTED:closed;
  }
  EFI_FILE_PROTOCOL *root=matched_root;
  VOID *bytes=NULL;UINTN actual_size=0;
  s=ReadWindowsBinary(b,root,selected_name,&actual_size,&bytes);
  EFI_STATUS closed=CloseBootFile(&root);if(closed!=EFI_SUCCESS)s=closed;
  UINTN name_bytes=(GuStrLen(selected_name)+1)*sizeof(CHAR16);
  UINTN node_bytes=4+name_bytes,total=path_bytes-4+node_bytes+4;
  VOID *path=NULL;
  if(s==EFI_SUCCESS)s=AllocateBootBuffer(b,total,&path);
  if(s==EFI_SUCCESS) {
    memcpy(path,base,path_bytes-4);UINT8 *node=(UINT8 *)path+path_bytes-4;
    node[0]=MEDIA_DEVICE_PATH;node[1]=MEDIA_FILEPATH_DP;node[2]=(UINT8)node_bytes;node[3]=(UINT8)(node_bytes>>8);
    memcpy(node+4,selected_name,name_bytes);node+=node_bytes;
    node[0]=END_DEVICE_PATH_TYPE;node[1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;node[2]=4;node[3]=0;
    s=b->LoadImage(FALSE,Self,(VOID *)path,bytes,actual_size,&Windows);
  }
  /* The image buffer and device path have independent confirmed references.
   * Consume both once even when an earlier release fails. */
  EFI_STATUS bytes_released=ReleaseBootBuffer(b,&bytes);
  EFI_STATUS path_released=ReleaseBootBuffer(b,&path);
  if(bytes_released!=EFI_SUCCESS||path_released!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS) {
    EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
    if(b->HandleProtocol(Windows,&LoadedGuid,(VOID **)&im)==EFI_SUCCESS&&im&&im->DeviceHandle!=volume) {
      im->DeviceHandle=volume;
    }
  }
  if(s==EFI_SUCCESS)TargetEspVolume=volume;
  GuLog("Windows image staged: status=%#llx matches=%zu pinned_GPT=1 path='%ls'\n",(unsigned long long)s,(size_t)matches,selected_name);
  return s;
}

EFI_STATUS GuBootChoose(CHAR16 *choice) {

  if(!choice||!Table||!Table->ConIn||!Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;

  *choice=L'S';

  GuBootSay(

    L"\r\n=======================================================\r\n"

    L" " GU_FORK_VERSION_WIDE L" (Multi-Gen Edition)\r\n"

    L"=======================================================\r\n"

    L"  [U] Experimental selected GPU transaction (RAM-only)\r\n"

    L"  [P] Experimental GPU transaction with S3 resume script\r\n"

    L"  [I] Install DXE Driver to UEFI NVRAM (Auto-boot / S3)\r\n"

    L"  [R] Remove DXE Driver from UEFI NVRAM\r\n"

    L"  [S] Stock Windows (Skip unlock, normal boot)\r\n\r\n"

    L"Select mode [U/P/I/R/S] (default: Stock Windows in 30s): "

  );

  for(UINTN i=0;i<300;i++) {
    if(i%10==0) {
      UINTN seconds=(300-i+9)/10;
      CHAR16 digits[]=L"00s (S=stock Windows)  ";
      digits[0]=(CHAR16)(L'0'+seconds/10);
      digits[1]=(CHAR16)(L'0'+seconds%10);
      GuBootSay(L"\rStock Windows in ");
      GuBootSay(digits);
    }
    EFI_INPUT_KEY key;EFI_STATUS s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);

    if(s==EFI_SUCCESS) {

      if(key.UnicodeChar=='u'||key.UnicodeChar=='U'){*choice=L'U';return EFI_SUCCESS;}

      if(key.UnicodeChar=='p'||key.UnicodeChar=='P'){*choice=L'P';return EFI_SUCCESS;}

      if(key.UnicodeChar=='i'||key.UnicodeChar=='I'){*choice=L'I';return EFI_SUCCESS;}

      if(key.UnicodeChar=='r'||key.UnicodeChar=='R'){*choice=L'R';return EFI_SUCCESS;}

      if(key.ScanCode==0x17||key.UnicodeChar==27||key.UnicodeChar=='s'||key.UnicodeChar=='S'){*choice=L'S';return EFI_SUCCESS;}

    } else if(s!=EFI_NOT_READY)return s;

    s=Table->BootServices->Stall(100000);if(s!=EFI_SUCCESS)return s;

  }

  return EFI_SUCCESS;

}

EFI_STATUS GuBootClose(VOID) {

  if(Log){
    EFI_STATUS flushed=Log->Flush?Log->Flush(Log):EFI_UNSUPPORTED;
    if(flushed!=EFI_SUCCESS)BootFileStatus=EFI_DEVICE_ERROR;
    (VOID)CloseBootFile(&Log);
  }
  (VOID)CloseBootFile(&Root);
  return BootFileStatus;

}

EFI_STATUS GuBootWindows(VOID) {

  if(!Windows||!Table||Root||Log||UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS)return EFI_NOT_READY;

  return Table->BootServices->StartImage(Windows,NULL,NULL);

}

VOID GuBootHalt(VOID) {

  GuBootSay(L"\r\nSTOPPED. Power OFF fully; next boot use Windows Boot Manager.\r\n");

  #if defined(_MSC_VER)

  for(;;);

#else

  for(;;)__asm__ volatile("pause");

#endif

}

