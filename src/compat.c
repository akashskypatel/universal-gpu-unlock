#include "compat/compat.h"
#include "profile.h"
#include <limits.h>

/* The current serialized platform owns exactly one compatibility record.
 * Compare the registered address before examining a caller's reference.
 * Retain the closed record so a later card can never reuse its address. */
static BOOLEAN GuCompatRecordMatches(const GU_PLATFORM *platform,const struct pci_dev *D) {
  /* Prove allocation membership before reading a retained record. */
  return platform&&D&&D==platform->compat_device&&
    GuCpuPoolOwns(platform,D,sizeof(*D))&&
    D->owner==platform&&D->owner_bs==platform->bs&&
    D->owner_pci==platform->pci&&D->owner_handle==platform->handle&&
    platform->bs&&platform->pci&&platform->handle&&
    D->vendor==0x10de&&D->device==platform->device_id&&
    D->dev.dma_mask==&D->mask_storage;
}

static BOOLEAN GuCompatIdentityValid(const GU_PLATFORM *platform,const struct pci_dev *D) {
  return platform&&platform->compat_device_open&&!platform->compat_device_closed&&
    GuCompatRecordMatches(platform,D)&&D->refs>=1;
}

/* Inspect a completed inactive owner without rebinding or touching hardware. */
BOOLEAN GuCompatCompletedPlatform(const GU_PLATFORM *platform) {
  if(!platform||platform->compat_device_open||!platform->compat_device_closed||
     platform->fault||platform->retained||
     !GuCompatRecordMatches(platform,platform->compat_device))return FALSE;
  const struct pci_dev *D=platform->compat_device;
  return D->refs==1&&!D->dev.scope&&list_empty(&D->dev.devres_head)&&
    GuCpuPoolCompatEmpty(platform);
}
GU_PLATFORM *GuCompatOwner(const struct pci_dev *D) {
  GU_PLATFORM *owner=GuPlatformForCpuObject(D,D?sizeof(*D):0);
  if(owner&&GuCompatIdentityValid(owner,D))return owner;
  if(owner)owner->fault=TRUE;
  return NULL;
}
BOOLEAN GuCompatPciMatches(const struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  return owner&&owner==GuPlatformActive;
}
static GU_PLATFORM *GuCompatDeviceOwner(const struct device *D) {
  if(!D)return NULL;
  const struct pci_dev *pdev=(const struct pci_dev *)((const UINT8 *)D-
                              offsetof(struct pci_dev,dev));
  GU_PLATFORM *owner=GuCompatOwner(pdev);
  return owner&&D==&pdev->dev?owner:NULL;
}
/* Reserve CPU storage before driver release. No compatibility operation
 * can use the record until the coordinator opens the scope. */
EFI_STATUS GuCompatReserveDevice(GU_PLATFORM *owner) {
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
  if(!owner->bs||!owner->pci||!owner->handle||owner->fault||owner->retained)return EFI_NOT_READY;
  if(owner->compat_device||owner->compat_device_open||owner->compat_device_closed)return EFI_ALREADY_STARTED;
  if(owner->started||owner->owned||owner->armed)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *bs=owner->bs;
  EFI_PCI_IO_PROTOCOL *pci=owner->pci;
  EFI_HANDLE handle=owner->handle;
  UINT32 device_id=owner->device_id;
  struct pci_dev *D=GuAllocFor(owner,sizeof(*D));
  if(owner!=GuPlatformActive||owner->bs!=bs||owner->pci!=pci||
     owner->handle!=handle||owner->device_id!=device_id||
     owner->compat_device||owner->compat_device_open||owner->compat_device_closed||
     owner->started||owner->owned||owner->armed||
     (D&&!GuCpuPoolOwns(owner,D,sizeof(*D)))) {
    owner->fault=TRUE;owner->retained=TRUE;
    return EFI_ACCESS_DENIED;
  }
  if(owner->fault||owner->retained) {
    if(D) {owner->fault=TRUE;owner->retained=TRUE;}
    return EFI_DEVICE_ERROR;
  }
  if(!D)return EFI_OUT_OF_RESOURCES;
  D->owner=owner;D->owner_bs=bs;D->owner_pci=pci;D->owner_handle=handle;
  D->vendor=0x10de;D->device=device_id;
  D->mask_storage=0xffffffffffffULL;
  D->dev.dma_mask=&D->mask_storage;D->dev.coherent_dma_mask=0xfffffffffULL;
  owner->compat_device=D;
  return EFI_SUCCESS;
}
EFI_STATUS GuCompatOpenDevice(GU_PLATFORM *owner,struct pci_dev **out) {
  if(!out||*out)return EFI_INVALID_PARAMETER;
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
  if(owner->fault||owner->retained)return EFI_NOT_READY;
  if(owner->compat_device_open||owner->compat_device_closed)return EFI_ALREADY_STARTED;
  if(!owner->compat_device||!owner->owned||!owner->armed||!owner->display_ok)return EFI_NOT_READY;
  struct pci_dev *D=owner->compat_device;
  if(!GuCompatRecordMatches(owner,D)||D->refs||D->dev.scope||
     !list_empty(&D->dev.devres_head)||!GuCpuPoolCompatEmpty(owner)) {
    owner->fault=TRUE;return EFI_ACCESS_DENIED;
  }
  D->current_state=PCI_D0;D->pm_cap=owner->pm_cap;D->refs=1;
  owner->compat_device_open=TRUE;*out=D;
  return EFI_SUCCESS;
}
EFI_STATUS GuCompatCloseDevice(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner)return EFI_ACCESS_DENIED;
  owner->compat_device_open=FALSE;
  if(D->refs!=1||D->dev.scope||!list_empty(&D->dev.devres_head)||
     owner->fault||owner->retained||!GuCpuPoolCompatEmpty(owner)) {
    owner->fault=TRUE;return EFI_DEVICE_ERROR;
  }
  owner->compat_device_closed=TRUE;
  return EFI_SUCCESS;
}
GU_ASSET *GuAssetEntries(VOID) {
  return Gu.asset_entries;
}
EFI_STATUS GuAssetReserveEntries(GU_PLATFORM *owner,EFI_BOOT_SERVICES *bs) {
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
  if(!bs||!bs->AllocatePool||!bs->FreePool||!owner->bs||owner->bs!=bs||
     owner->started||owner->owned||owner->armed||owner->fault||owner->retained)return EFI_NOT_READY;
  if(owner->asset_entries_reserved||owner->asset_load_attempted)return EFI_ALREADY_STARTED;
  for(UINTN i=0;i<GU_ASSET_COUNT;i++)
    if(owner->asset_entries[i].name||owner->asset_entries[i].firmware.data||
       owner->asset_entries[i].firmware.size)return EFI_ALREADY_STARTED;
  owner->asset_entries_reserved=TRUE;
  return EFI_SUCCESS;
}
void *devres_open_group(struct device *D,void *Id,int Flags) {
  (void)Flags;
  if(!GuCompatDeviceOwner(D)||Id||D->scope)return NULL;
  D->scope=true;return &D->scope;
}
int devres_release_group(struct device *D,void *Group) {
  if(!GuCompatDeviceOwner(D)||Group!=&D->scope||!D->scope)return -EINVAL;
  D->scope=false;return 0;
}
/* A wait belongs to the device that initiated it. Do not advance another
 * platform's clock or continue polling when firmware could not delay. */
ktime_t GuCompatTime(const struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  return owner?owner->ticks:0;
}
BOOLEAN GuCompatDelay(const struct pci_dev *D,UINTN Microseconds) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner->fault||owner->retained||!owner->bs||!owner->bs->Stall)return FALSE;
  if(EFI_ERROR(owner->bs->Stall(Microseconds))) {
    owner->fault=TRUE;owner->retained=TRUE;return FALSE;
  }
  owner->ticks+=Microseconds;
  return !owner->fault&&!owner->retained;
}
/* Vendor access must carry the transaction's retained device reference.
 * A numeric BAR address alone cannot identify a physical GPU. */
u32 GuCompatRead32(const struct pci_dev *D,const void *P) {
  if(!GuCompatPciMatches(D))return ~0U;
  UINT64 Address=(UINTN)P;
  if(Address<Gu.bar_base || Address-Gu.bar_base>0xffffffffULL){Gu.fault=TRUE;return ~0U;}
  return GuRead32((UINT32)(Address-Gu.bar_base));
}
void GuCompatWrite32(const struct pci_dev *D,u32 Value,void *P) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner!=GuPlatformActive)return;
  UINT64 Address=(UINTN)P;
  if(Address<owner->bar_base || Address-owner->bar_base>0xffffffffULL){owner->fault=TRUE;return;}
  GuWrite32(owner,(UINT32)(Address-owner->bar_base),Value);
}
/* The read-only profile binds this operation to its selected owner before
 * a compatibility scope exists. Never switch protocols within a read. */
BOOLEAN GuCopyFromIo(GU_PLATFORM *owner,VOID *D,const VOID *S,size_t N) {
  UINT64 Address=(UINTN)S,Offset;UINTN Done=0;
  if(D)memset(D,0,N);
  if(!owner)return FALSE;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return FALSE;}
  if(!D||owner->fault||owner->retained||!owner->pci||!owner->pci->Mem.Read||
     Address<owner->bar_base||Address-owner->bar_base>owner->bar_size||
     N>owner->bar_size-(Address-owner->bar_base)){owner->fault=TRUE;return FALSE;}
  EFI_PCI_IO_PROTOCOL *pci=owner->pci;
  Offset=Address-owner->bar_base;
  while(Done<N) {
    UINTN Count=N-Done<1024?N-Done:1024;
    EFI_STATUS status=pci->Mem.Read(pci,EfiPciIoWidthUint8,0,Offset+Done,Count,(UINT8 *)D+Done);
    if(EFI_ERROR(status)||owner!=GuPlatformActive||owner->pci!=pci||
       owner->fault||owner->retained) {
      memset(D,0,N);owner->fault=TRUE;return FALSE;
    }
    Done+=Count;
  }
  return TRUE;
}
BOOLEAN GuCompatCopyFromIo(const struct pci_dev *D,VOID *Destination,const VOID *Source,size_t N) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner) {
    if(Destination)memset(Destination,0,N);
    return FALSE;
  }
  return GuCopyFromIo(owner,Destination,Source,N);
}
void *dma_alloc_coherent(struct device *D,size_t N,dma_addr_t *Address,int Flags) {
  VOID *Cpu=NULL;(void)Flags;
  GU_PLATFORM *owner=GuCompatDeviceOwner(D);
  if(!owner||owner!=GuPlatformActive||!D->dma_mask||
     *D->dma_mask!=0xffffffffULL||D->coherent_dma_mask!=0xffffffffULL)return NULL;
  if(EFI_ERROR(GuDmaAllocate(owner,N,&Cpu,Address)))return NULL;
  return Cpu;
}
void dma_free_coherent(struct device *D,size_t N,void *P,dma_addr_t A) {
  (void)N;
  GU_PLATFORM *owner=GuCompatDeviceOwner(D);
  if(!owner||owner!=GuPlatformActive)return;
  if(EFI_ERROR(GuDmaFree(owner,P,A)))owner->retained=TRUE;
}
int dma_set_mask(struct device *D,u64 Mask) {
  if(!GuCompatDeviceOwner(D)||!D->dma_mask)return -EINVAL;
  *D->dma_mask=Mask;return 0;
}
int dma_set_coherent_mask(struct device *D,u64 Mask) {
  if(!GuCompatDeviceOwner(D))return -EINVAL;
  D->coherent_dma_mask=Mask;return 0;
}
void GuCompatDmaBarrier(const struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner!=GuPlatformActive)return;
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
  if(owner->pci->Flush && EFI_ERROR(owner->pci->Flush(owner->pci)))owner->fault=TRUE;
}
void *GuCompatAlloc(const struct pci_dev *D,size_t N,size_t S) {
  if(!GuCompatPciMatches(D)||!S||N>(size_t)-1/S)return NULL;
  return GuCpuPoolAllocScoped(D,N*S);
}
BOOLEAN GuCompatFree(const struct pci_dev *D,void *P) {
  if(!GuCompatPciMatches(D))return FALSE;
  return GuCpuPoolFreeScoped(D,P);
}
void GuCompatRetain(const struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(owner)owner->retained=TRUE;
}
int request_firmware(const struct firmware **Out,const char *Name,struct device *D) {
  UINTN I;if(!Out)return -EINVAL;*Out=NULL;
  GU_PLATFORM *owner=GuCompatDeviceOwner(D);
  if(!owner||!Name)return -EINVAL;
  if(!GuPlatformAssetsOwned(owner)){owner->fault=TRUE;return -EIO;}
  for(I=0;I<GU_ASSET_COUNT;I++)
    if(owner->asset_entries[I].name&&!strcmp(Name,owner->asset_entries[I].name)&&
       owner->asset_entries[I].firmware.data&&owner->asset_entries[I].firmware.size) {
      *Out=&owner->asset_entries[I].firmware;return 0;
    }
  return -ENOENT;
}
void release_firmware(const struct firmware *F) {(void)F; /* Preloaded, app-owned immutable assets. */}
int pci_read_config_word(struct pci_dev *D,int O,u16 *V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,(UINT32)O,V))?-EIO:0;
}
int pci_read_config_dword(struct pci_dev *D,int O,u32 *V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)O,V))?-EIO:0;
}
int pci_write_config_word(struct pci_dev *D,int O,u16 V) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner!=GuPlatformActive)return -EINVAL;
  return EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint16,(UINT32)O,&V))?-EIO:0;
}
int pcie_capability_read_word(struct pci_dev *D,int O,u16 *V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return Gu.pcie_cap?pci_read_config_word(D,Gu.pcie_cap+O,V):-EIO;
}
int pcie_capability_read_dword(struct pci_dev *D,int O,u32 *V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return Gu.pcie_cap?pci_read_config_dword(D,Gu.pcie_cap+O,V):-EIO;
}
int pci_is_pcie(struct pci_dev *D) {return GuCompatPciMatches(D)&&Gu.pcie_cap!=0;}
int pci_select_bars(struct pci_dev *D,int R) {(void)R;return GuCompatPciMatches(D)&&Gu.bar_size?1:0;}
int pci_request_region(struct pci_dev *D,int Bar,const char *Name) {
  (void)Name;return GuCompatPciMatches(D)&&!Bar && Gu.owned && Gu.display_ok && !Gu.fault?0:-EBUSY;
}
void pci_release_region(struct pci_dev *D,int Bar) {(VOID)GuCompatPciMatches(D);(void)Bar; /* App releases PCI protocol after all cleanup. */}
int aperture_remove_conflicting_pci_devices(struct pci_dev *D,const char *Name) {
  (void)Name;
  if(!GuCompatPciMatches(D))return -EINVAL;
  if(Gu.display_check)Gu.display_ok=Gu.display_check();
  return Gu.display_ok?0:-EBUSY;
}
int pci_enable_device_mem(struct pci_dev *D) {return GuCompatPciMatches(D)?-EOPNOTSUPP:-EINVAL;}
void pci_disable_device(struct pci_dev *D) {(VOID)GuCompatPciMatches(D);}
resource_size_t pci_resource_len(struct pci_dev *D,int Bar) {return !GuCompatPciMatches(D)||Bar?0:Gu.bar_size;}
void *pci_iomap(struct pci_dev *D,int Bar,unsigned long N) {
  (void)N;return GuCompatPciMatches(D)&&!Bar && Gu.owned && !Gu.fault?(VOID *)(UINTN)Gu.bar_base:NULL;
}
void pci_iounmap(struct pci_dev *D,void *P) {(VOID)GuCompatPciMatches(D);(void)P;}
int pci_save_state(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner!=GuPlatformActive)return -EINVAL;
  return EFI_ERROR(GuSavePci(owner))?-EIO:0;
}
void pci_restore_state(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner!=GuPlatformActive)return;
  if(EFI_ERROR(GuRestorePci(owner)))owner->fault=TRUE;
}
void pci_set_master(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(owner&&owner==GuPlatformActive)(void)GuEnableMaster(owner);
}
void pci_clear_master(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(owner&&owner==GuPlatformActive)(void)GuClearMaster(owner);
}
int pcie_flr(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner||owner!=GuPlatformActive)return -EINVAL;
  return EFI_ERROR(GuFlr(owner))?-EIO:0;
}
/* The coordinator's base reference survives until the end of this boot.
 * A failed borrow/release must not change its count or revive a bad record. */
struct pci_dev *pci_dev_get(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner)return NULL;
  if(D->refs<1||D->refs==INT_MAX){owner->fault=TRUE;return NULL;}
  D->refs++;return D;
}
void pci_dev_put(struct pci_dev *D) {
  GU_PLATFORM *owner=GuCompatOwner(D);
  if(!owner)return;
  if(D->refs<=1){owner->fault=TRUE;return;}
  D->refs--;
}
