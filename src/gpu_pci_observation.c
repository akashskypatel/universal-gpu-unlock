/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GU_GPU_DIAGNOSTIC_ONLY
#error The broad PCI evidence collector must only link into passive diagnostics
#endif
#include "gpu_pci_observation.h"
#include <string.h>
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;

static void Problem(GU_PCI_OBS_CAPTURE *out,GU_PCI_OBS_STAGE stage,
                    UINTN index,EFI_STATUS status) {
  if(!out->failures) {
    out->first_failure_stage=stage;
    out->first_failure_index=index;
    out->first_failure_status=status;
  }
  out->failures++;
}

/* Copy a single, complete EFI device path without guessing the pointer size.
 * Malformed or multi-instance paths remain UNKNOWN and are never normalized. */
static int CopyPath(const VOID *path, GU_PCI_OBS_RECORD *record) {
  const UINT8 *p=(const UINT8 *)path;
  UINTN at=0,n;
  if(!p)return 0;
  while(at+4<=GU_PCI_OBS_PATH_BYTES) {
    n=(UINTN)p[at+2] | ((UINTN)p[at+3]<<8);
    if(n<4 || n>GU_PCI_OBS_PATH_BYTES-at)return 0;
    if(p[at]==0x7f) {
      if(p[at+1]!=0xff || n!=4)return 0;
      memcpy(record->path+at,p+at,n);
      record->path_bytes=at+n;
      record->path_ok=1;
      return 1;
    }
    memcpy(record->path+at,p+at,n);
    at+=n;
  }
  return 0;
}

int GuPciObserveReadOnly(EFI_BOOT_SERVICES *boot, GU_PCI_OBS_CAPTURE *out) {
  EFI_HANDLE *handles=NULL;
  EFI_STATUS status;
  UINTN count=0,i;
  int valid=1;
  if(!boot || !out)return 0;
  memset(out,0,sizeof(*out));
  if(!boot->LocateHandleBuffer || !boot->HandleProtocol || !boot->FreePool) {
    Problem(out,GuPciObsInterface,0,EFI_UNSUPPORTED);
    return 0;
  }
  status=boot->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  out->locate_status=status;out->handle_count=count;
  if(EFI_ERROR(status)) {
    Problem(out,GuPciObsLocate,0,status);valid=0;
  } else if(!count || count>GU_PCI_OBS_MAX_HANDLES || !handles) {
    Problem(out,GuPciObsCount,0,EFI_BAD_BUFFER_SIZE);valid=0;
  }
  if(valid)for(i=0;i<count;i++) {
    EFI_PCI_IO_PROTOCOL *io=NULL;
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *owners=NULL;
    EFI_STATUS s,release;
    UINTN segment=0,bus=0,device=0,function=0,owner_count=0,j;
    UINT32 config[16];
    GU_PCI_OBS_RECORD *record;
    int target;
    s=boot->HandleProtocol(handles[i],&PciGuid,(VOID **)&io);
    if(s==EFI_NOT_FOUND || s==EFI_UNSUPPORTED)continue;
    if(EFI_ERROR(s) || !io || !io->Pci.Read || !io->GetLocation) {
      Problem(out,GuPciObsInterface,i,EFI_ERROR(s)?s:EFI_UNSUPPORTED);
      continue;
    }
    out->pci_protocol_count++;
    s=io->Pci.Read(io,EfiPciIoWidthUint32,0,16,config);
    if(EFI_ERROR(s)) {Problem(out,GuPciObsRead,i,s);continue;}
    s=io->GetLocation(io,&segment,&bus,&device,&function);
    if(EFI_ERROR(s) || segment>65535U || bus>255U || device>31U || function>7U) {
      Problem(out,GuPciObsLocation,i,EFI_ERROR(s)?s:EFI_COMPROMISED_DATA);
      continue;
    }
    /* Display GPUs and their PCI bridges are the only detailed records.
     * Scanning always continues; a truncated report is explicitly PARTIAL. */
    target=((config[2]>>16)==0x0604U) ||
           ((config[2]>>24)==0x03U &&
            ((config[0]&0xffffU)==0x10deU ||
             (config[0]&0xffffU)==0x1002U ||
             (config[0]&0xffffU)==0x1022U));
    if(!target)continue;
    out->relevant_count++;
    if(out->record_count==GU_PCI_OBS_MAX_RECORDS) {
      out->truncated=1;continue;
    }
    record=&out->records[out->record_count++];
    record->index=i;record->segment=segment;record->bus=bus;
    record->device=device;record->function=function;
    record->location_ok=1;memcpy(record->config,config,sizeof(config));
    s=boot->HandleProtocol(handles[i],&PathGuid,(VOID **)&path);
    if(EFI_ERROR(s) || !CopyPath(path,record))
      Problem(out,GuPciObsPath,i,EFI_ERROR(s)?s:EFI_COMPROMISED_DATA);
    if(!boot->OpenProtocolInformation)
      Problem(out,GuPciObsOwners,i,EFI_UNSUPPORTED);
    else {
      s=boot->OpenProtocolInformation(handles[i],&PciGuid,&owners,&owner_count);
      if(EFI_ERROR(s) || owner_count>GU_PCI_OBS_MAX_OPENERS ||
         (owner_count && !owners)) {
        Problem(out,GuPciObsOwners,i,EFI_ERROR(s)?s:EFI_COMPROMISED_DATA);
      } else {
        record->owners_known=1;
        for(j=0;j<owner_count;j++)if(owners[j].Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER) {
          if(!record->by_driver)record->first_agent=owners[j].AgentHandle;
          record->by_driver++;
        }
      }
      if(owners) {
        release=boot->FreePool(owners);
        if(EFI_ERROR(release))Problem(out,GuPciObsFree,i,release);
      }
    }
  }
  if(valid && !out->pci_protocol_count)
    Problem(out,GuPciObsCount,0,EFI_NOT_FOUND);
  if(out->truncated)Problem(out,GuPciObsCount,out->record_count,EFI_BAD_BUFFER_SIZE);
  if(handles) {
    out->free_status=boot->FreePool(handles);
    if(EFI_ERROR(out->free_status))
      Problem(out,GuPciObsFree,count,out->free_status);
  }
  return valid && !out->failures;
}
