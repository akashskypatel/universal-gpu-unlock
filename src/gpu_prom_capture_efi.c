/* SPDX-License-Identifier: GPL-2.0-only */
/* Unlinked diagnostic-only EFI adapter for the exact 1-MiB BAR0 PROM window
 * in Windows-GPU-UNLOCK internal-boot/boot/probe.c. This is NOT a source
 * for the 2,048,000-byte IFR v3 ROM backups, and is not enabled on hardware. */
#include "gpu_prom_capture_efi.h"
#include "sha256.h"
#include <IndustryStandard/Acpi.h>
#include <string.h>

static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;

typedef struct { EFI_PCI_IO_PROTOCOL *io; } GU_PROM_PCI_READER;

static int Read32(void *opaque, uint32_t offset, uint32_t *word) {
  GU_PROM_PCI_READER *reader=(GU_PROM_PCI_READER *)opaque;
  return reader && reader->io && reader->io->Mem.Read && word &&
    reader->io->Mem.Read(reader->io,EfiPciIoWidthUint32,0,offset,1,word)
       == EFI_SUCCESS;
}

/* Original probe's stricter 16-MiB MEM/BME0 BAR0 resource envelope.
 * This is necessary but not sufficient for authorization of MMIO. */
static int BarEnvelope(EFI_SYSTEM_TABLE *table, EFI_PCI_IO_PROTOCOL *io,
                       const UINT32 config[64]) {
  VOID *raw=NULL;
  EFI_STATUS status, released=EFI_SUCCESS;
  int valid=0;
  if (!table || !table->BootServices || !table->BootServices->FreePool ||
      !io || !io->GetBarAttributes || !io->Mem.Read || !config ||
      (config[1]&6U)!=2U || !config[4] ||
      (config[4]&0xffffffU)!=0)
    return 0;
  status=io->GetBarAttributes(io,0,NULL,&raw);
  if(status==EFI_SUCCESS && raw) {
    const EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=
        (const EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)raw;
    const UINT8 *end=(const UINT8 *)(d+1);
    valid=d->Desc==ACPI_ADDRESS_SPACE_DESCRIPTOR &&
      d->Len==sizeof(*d)-3U && d->ResType==0 &&
      d->AddrSpaceGranularity==32 &&
      d->AddrRangeMin==(UINT64)config[4] &&
      d->AddrLen==0x1000000U && d->AddrTranslationOffset==0 &&
      (d->AddrRangeMax==(UINT64)config[4]+0xffffffU ||
       d->AddrRangeMax==0xffffffU) &&
      end[0]==ACPI_END_TAG_DESCRIPTOR;
    if(valid && end[1]) {
      const UINT8 *p=(const UINT8 *)d;
      UINT8 sum=0;UINTN i;
      for(i=0;i<sizeof(*d)+2U;i++)sum=(UINT8)(sum+p[i]);
      if(sum)valid=0;
    }
  }
  if(raw) {
    released=table->BootServices->FreePool(raw);
    if(EFI_ERROR(released))valid=0;
  }
  return valid;
}

EFI_STATUS GuPromCapturePrepared(EFI_SYSTEM_TABLE *table,
    const GU_GPU_PREPARED_SEQUENCE *prepared, size_t selected_step,
    UINT8 *output, UINTN output_size, UINT8 digest[32]) {
  UINT32 first[64],after[64];
  EFI_PCI_IO_PROTOCOL *io=NULL,*again=NULL;
  GU_PROM_PCI_READER context;
  GU_PROM_READER reader;
  EFI_STATUS status;
  if(digest)memset(digest,0,32);
  if(output && output_size==GU_PROM_CAPTURE_BYTES)
    memset(output,0,output_size);
  if(!table || !prepared || !output || !digest ||
     output_size!=GU_PROM_CAPTURE_BYTES) return EFI_INVALID_PARAMETER;
  status=GuRefreshPreparedGpu(table,prepared,selected_step,first,&io);
  if(status!=EFI_SUCCESS || !io) return EFI_UNSUPPORTED;
  /* Only the two explicitly investigated PCI identities; neither family
   * is thereby asserted to have safe ROM MMIO read semantics. */
  if(!((first[0]==0x268410deU && first[11]==0x88e61043U) ||
       (first[0]==0x220a10deU && first[11]==0xb61219daU)) ||
     (first[2]>>16)!=0x0300U || !BarEnvelope(table,io,first))
    return EFI_UNSUPPORTED;
  context.io=io;reader.context=&context;reader.read32=Read32;
  if(!GuPromCapture(&reader,output,output_size))return EFI_DEVICE_ERROR;
  status=GuRefreshPreparedGpu(table,prepared,selected_step,after,&again);
  if(status!=EFI_SUCCESS || io!=again ||
     memcmp(first,after,sizeof(first))!=0) {
    memset(output,0,output_size);
    return EFI_UNSUPPORTED;
  }
  GuSha256(output,output_size,digest);
  return EFI_SUCCESS;
}

EFI_STATUS GuPromSaveNew(EFI_FILE_PROTOCOL *root, const GU_GPU_RECORD *target,
    const UINT8 *output, UINTN output_size) {
  static const CHAR16 digits[]=L"0123456789ABCDEF";
  CHAR16 name[]=L"\\PROM-0000-00-00-0.BIN";
  EFI_FILE_PROTOCOL *file=NULL,*prior=NULL;
  EFI_STATUS status,closed;
  UINTN written;
  UINTN i;
  if(!root || !root->Open || !target || !output ||
     output_size!=GU_PROM_CAPTURE_BYTES ||
     target->segment>0xffffU || target->bus>255U ||
     target->device>31U || target->function>7U)
    return EFI_INVALID_PARAMETER;
  for(i=0;i<4;i++)name[6+i]=digits[(target->segment>>(12U-4U*(UINT32)i))&15U];
  for(i=0;i<2;i++) {
    name[11+i]=digits[(target->bus>>(4U-4U*(UINT32)i))&15U];
    name[14+i]=digits[(target->device>>(4U-4U*(UINT32)i))&15U];
  }
  name[17]=(CHAR16)(L'0'+target->function);
  status=root->Open(root,&prior,name,EFI_FILE_MODE_READ,0);
  if(status!=EFI_NOT_FOUND || prior) {
    if(prior && prior->Close)(VOID)prior->Close(prior);
    return status==EFI_SUCCESS?EFI_ALREADY_STARTED:
           status==EFI_NOT_FOUND?EFI_COMPROMISED_DATA:status;
  }
  status=root->Open(root,&file,name,
                    EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(EFI_ERROR(status) || !file)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  if(!file->Write || !file->Close)status=EFI_UNSUPPORTED;
  else {
    written=output_size;
    status=file->Write(file,&written,(VOID *)output);
    if(!EFI_ERROR(status) && written!=output_size)status=EFI_END_OF_FILE;
    if(!EFI_ERROR(status) &&
       (!file->Flush || EFI_ERROR(file->Flush(file))))status=EFI_DEVICE_ERROR;
  }
  if(EFI_ERROR(status) && file->Delete) {
    (VOID)file->Delete(file); /* Best effort; deleted handle is invalid. */
    return status;
  }
  if(!file->Close)return EFI_UNSUPPORTED;
  closed=file->Close(file);
  if(!EFI_ERROR(status))status=closed;
  return status;
}
