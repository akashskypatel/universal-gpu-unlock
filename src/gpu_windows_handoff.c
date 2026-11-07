/* SPDX-License-Identifier: GPL-2.0-only */
/* Passive diagnostic -> explicitly pinned Windows Boot Manager, no reset. */
#if !defined(GU_GPU_DIAGNOSTIC_ONLY) || !defined(GU_TOPO_WINDOWS_HANDOFF) || \
    !defined(GU_TOPO_SAVE_LOG) || defined(GU_FULL_UNLOCK)
#error Windows handoff is only available to a file-logging passive diagnostic
#endif
#ifdef GU_WINDOWS_HANDOFF_HOST_TEST
#include "../tests/gpu_windows_handoff_mock.h"
#else
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>
#endif
#include "gpu_windows_handoff.h"
#include "gpu_windows_path.h"
#ifdef GU_WINDOWS_HANDOFF_HOST_TEST
/* Synthetic, not the physical system's partition GUID. */
#define GU_WINDOWS_ESP_GUID_INITIALIZER \
  {0xa005858c,0x4487,0x4ee2,{0x97,0x6e,0x17,0x10,0xff,0x48,0x4c,0x31}}
#else
#include "windows-esp-guid.h"  /* Generated in build_diagnostic; never committed. */
#endif
#include <string.h>

static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static const EFI_GUID WindowsGuid=GU_WINDOWS_ESP_GUID_INITIALIZER;

static EFI_STATUS WindowsVolume(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                                EFI_HANDLE *out) {
  EFI_LOADED_IMAGE_PROTOCOL *self=NULL;
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i,matches=0;
  EFI_STATUS s,freed;
  *out=NULL;
  s=bs->HandleProtocol(image,&LoadedGuid,(VOID **)&self);
  if(EFI_ERROR(s) || !self || !self->DeviceHandle)return EFI_NOT_FOUND;
  s=bs->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&count,&handles);
  if(EFI_ERROR(s))return s;
  if(!handles || !count || count>256) {
    if(handles) (VOID)bs->FreePool(handles);
    return EFI_COMPROMISED_DATA;
  }
  for(i=0;i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    if(handles[i]==self->DeviceHandle)continue; /* Never chainload the USB. */
    s=bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path);
    if(EFI_ERROR(s) || !path)continue;
    if(GuWindowsEspPathMatches((const uint8_t *)path,4096,
                               (const uint8_t *)&WindowsGuid)) {
      matches++;
      *out=handles[i];
    }
  }
  freed=bs->FreePool(handles);
  if(EFI_ERROR(freed))return freed;
  if(matches!=1) { *out=NULL;return matches?EFI_COMPROMISED_DATA:EFI_NOT_FOUND; }
  return EFI_SUCCESS;
}

static EFI_STATUS WindowsPath(EFI_BOOT_SERVICES *bs, EFI_HANDLE volume,
                              EFI_DEVICE_PATH_PROTOCOL **out) {
  static const CHAR16 filename[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
  EFI_DEVICE_PATH_PROTOCOL *base=NULL,*node,*end;
  UINTN chars=0,bytes,total,appended;
  UINT8 *data=NULL;
  EFI_STATUS s;
  *out=NULL;
  s=bs->HandleProtocol(volume,&PathGuid,(VOID **)&base);
  if(EFI_ERROR(s) || !base)return EFI_NOT_FOUND;
  bytes=(UINTN)GuWindowsPathLength((const uint8_t *)base,4096);
  if(!bytes || !GuWindowsEspPathMatches((const uint8_t *)base,4096,
                                         (const uint8_t *)&WindowsGuid))
    return EFI_COMPROMISED_DATA;
  while(filename[chars] && chars<128)chars++;
  if(!chars || chars==128)return EFI_COMPROMISED_DATA;
  appended=4+(chars+1)*sizeof(CHAR16);
  if(appended>65535U || bytes>4096U || bytes+appended>8192U)return EFI_BAD_BUFFER_SIZE;
  total=bytes-4+appended+4;
  s=bs->AllocatePool(EfiBootServicesData,total,(VOID **)&data);
  if(EFI_ERROR(s) || !data)return EFI_OUT_OF_RESOURCES;
  memcpy(data,base,bytes-4);
  node=(EFI_DEVICE_PATH_PROTOCOL *)(data+bytes-4);
  node->Type=MEDIA_DEVICE_PATH;node->SubType=MEDIA_FILEPATH_DP;
  node->Length[0]=(UINT8)appended;node->Length[1]=(UINT8)(appended>>8);
  memcpy(data+bytes,(const void *)filename,(chars+1)*sizeof(CHAR16));
  end=(EFI_DEVICE_PATH_PROTOCOL *)(data+bytes-4+appended);
  end->Type=END_DEVICE_PATH_TYPE;end->SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
  end->Length[0]=4;end->Length[1]=0;
  *out=(EFI_DEVICE_PATH_PROTOCOL *)data;
  return EFI_SUCCESS;
}

EFI_STATUS GuWindowsBootManager(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  EFI_BOOT_SERVICES *bs;
  EFI_HANDLE volume=NULL,child=NULL;
  EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_STATUS s,freed;
  if(!image || !table || !table->BootServices)return EFI_INVALID_PARAMETER;
  bs=table->BootServices;
  if(!bs->LocateHandleBuffer || !bs->HandleProtocol || !bs->FreePool ||
     !bs->AllocatePool || !bs->LoadImage || !bs->StartImage)return EFI_UNSUPPORTED;
  s=WindowsVolume(image,bs,&volume);
  if(EFI_ERROR(s))return s;
  s=WindowsPath(bs,volume,&path);
  if(EFI_ERROR(s))return s;
  s=bs->LoadImage(FALSE,image,path,NULL,0,&child);
  freed=bs->FreePool(path);
  if(EFI_ERROR(freed))return freed;
  if(EFI_ERROR(s) || !child)return EFI_ERROR(s)?s:EFI_NOT_FOUND;
  s=bs->HandleProtocol(child,&LoadedGuid,(VOID **)&loaded);
  if(EFI_ERROR(s) || !loaded || loaded->DeviceHandle!=volume)
    return EFI_COMPROMISED_DATA;
  /* Never rewrite BootNext/BootOrder or restart; Windows owns normal startup. */
  return bs->StartImage(child,NULL,NULL);
}
