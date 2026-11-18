#include "compat/compat.h"

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

static BOOLEAN GuCompatIdentityValid(const struct pci_dev *D) {
  return Gu.compat_device_open&&!Gu.compat_device_closed&&GuCompatRecordMatches(&Gu,D);
}

/* Inspect a completed inactive owner without rebinding or touching hardware. */
BOOLEAN GuCompatCompletedPlatform(const GU_PLATFORM *platform) {
  if(!platform||platform->compat_device_open||!platform->compat_device_closed||
     platform->fault||platform->retained||
     !GuCompatRecordMatches(platform,platform->compat_device))return FALSE;
  const struct pci_dev *D=platform->compat_device;
  return D->refs==1&&!D->dev.scope&&list_empty(&D->dev.devres_head);
}
BOOLEAN GuCompatPciMatches(const struct pci_dev *D) {
  if(GuCompatIdentityValid(D))return TRUE;
  Gu.fault=TRUE;return FALSE;
}
static BOOLEAN GuCompatDeviceMatches(const struct device *D) {
  if(GuCompatIdentityValid(Gu.compat_device)&&D==&Gu.compat_device->dev)return TRUE;
  Gu.fault=TRUE;return FALSE;
}
/* Reserve CPU storage before driver release. No compatibility operation
 * can use the record until the coordinator opens the scope. */
EFI_STATUS GuCompatReserveDevice(VOID) {
  if(!Gu.bs||!Gu.pci||!Gu.handle||Gu.fault||Gu.retained)return EFI_NOT_READY;
  if(Gu.compat_device||Gu.compat_device_open||Gu.compat_device_closed)return EFI_ALREADY_STARTED;
  if(Gu.started||Gu.owned||Gu.armed)return EFI_NOT_READY;
  struct pci_dev *D=GuAlloc(sizeof(*D));
  if(!D)return EFI_OUT_OF_RESOURCES;
  D->owner=&Gu;D->owner_bs=Gu.bs;D->owner_pci=Gu.pci;D->owner_handle=Gu.handle;
  D->vendor=0x10de;D->device=Gu.device_id;
  D->mask_storage=0xffffffffffffULL;
  D->dev.dma_mask=&D->mask_storage;D->dev.coherent_dma_mask=0xfffffffffULL;
  Gu.compat_device=D;
  return EFI_SUCCESS;
}
EFI_STATUS GuCompatOpenDevice(struct pci_dev **out) {
  if(!out||*out)return EFI_INVALID_PARAMETER;
  if(Gu.fault||Gu.retained)return EFI_NOT_READY;
  if(Gu.compat_device_open||Gu.compat_device_closed)return EFI_ALREADY_STARTED;
  if(!Gu.compat_device||!Gu.owned||!Gu.armed||!Gu.display_ok)return EFI_NOT_READY;
  struct pci_dev *D=Gu.compat_device;
  if(!GuCompatRecordMatches(&Gu,D)||D->refs||D->dev.scope||
     !list_empty(&D->dev.devres_head)) {
    Gu.fault=TRUE;return EFI_ACCESS_DENIED;
  }
  D->current_state=PCI_D0;D->pm_cap=Gu.pm_cap;D->refs=1;
  Gu.compat_device_open=TRUE;*out=D;
  return EFI_SUCCESS;
}
EFI_STATUS GuCompatCloseDevice(struct pci_dev *D) {
  if(!GuCompatPciMatches(D))return EFI_ACCESS_DENIED;
  Gu.compat_device_open=FALSE;
  if(D->refs!=1||D->dev.scope||!list_empty(&D->dev.devres_head)||Gu.fault||Gu.retained) {
    Gu.fault=TRUE;return EFI_DEVICE_ERROR;
  }
  Gu.compat_device_closed=TRUE;
  return EFI_SUCCESS;
}
GU_ASSET *GuAssetEntries(VOID) {
  return Gu.asset_entries;
}
EFI_STATUS GuAssetReserveEntries(EFI_BOOT_SERVICES *bs) {
  if(!bs||!bs->AllocatePool||!bs->FreePool||!Gu.bs||Gu.bs!=bs||
     Gu.started||Gu.owned||Gu.armed||Gu.fault||Gu.retained)return EFI_NOT_READY;
  if(Gu.asset_entries_reserved||Gu.asset_load_attempted)return EFI_ALREADY_STARTED;
  for(UINTN i=0;i<GU_ASSET_COUNT;i++)
    if(Gu.asset_entries[i].name||Gu.asset_entries[i].firmware.data||
       Gu.asset_entries[i].firmware.size)return EFI_ALREADY_STARTED;
  Gu.asset_entries_reserved=TRUE;
  return EFI_SUCCESS;
}
void *devres_open_group(struct device *D,void *Id,int Flags) {
  (void)Flags;if(!GuCompatDeviceMatches(D)||Id || D->scope)return NULL;D->scope=true;return &D->scope;
}
int devres_release_group(struct device *D,void *Group) {
  if(!GuCompatDeviceMatches(D)||Group!=&D->scope || !D->scope)return -EINVAL;
  D->scope=false;return 0;
}
void usleep_range(unsigned long A,unsigned long B) {(void)B;GuStall(A);}
ktime_t ktime_get(void) {return Gu.ticks;}
u32 readl(const void *P) {
  UINT64 Address=(UINTN)P;
  if(Address<Gu.bar_base || Address-Gu.bar_base>0xffffffffULL){Gu.fault=TRUE;return ~0U;}
  return GuRead32((UINT32)(Address-Gu.bar_base));
}
void writel(u32 Value,void *P) {
  UINT64 Address=(UINTN)P;
  if(Address<Gu.bar_base || Address-Gu.bar_base>0xffffffffULL){Gu.fault=TRUE;return;}
  GuWrite32((UINT32)(Address-Gu.bar_base),Value);
}
VOID GuCopyFromIo(VOID *D,const VOID *S,size_t N) {
  UINT64 Address=(UINTN)S,Offset;UINTN Done=0;
  memset(D,0,N);
  if(Gu.fault || Address<Gu.bar_base || Address-Gu.bar_base>Gu.bar_size ||
     N>Gu.bar_size-(Address-Gu.bar_base)){Gu.fault=TRUE;return;}
  Offset=Address-Gu.bar_base;
  while(Done<N) {
    UINTN Count=N-Done<1024?N-Done:1024;
    if(EFI_ERROR(Gu.pci->Mem.Read(Gu.pci,EfiPciIoWidthUint8,0,Offset+Done,Count,(UINT8 *)D+Done))) {
      Gu.fault=TRUE;return;
    }
    Done+=Count;
  }
}
void *dma_alloc_coherent(struct device *D,size_t N,dma_addr_t *Address,int Flags) {
  VOID *Cpu=NULL;(void)Flags;
  if(!GuCompatDeviceMatches(D)||!D->dma_mask || *D->dma_mask!=0xffffffffULL || D->coherent_dma_mask!=0xffffffffULL)return NULL;
  if(EFI_ERROR(GuDmaAllocate(N,&Cpu,Address)))return NULL;
  return Cpu;
}
void dma_free_coherent(struct device *D,size_t N,void *P,dma_addr_t A) {
  (void)N;if(!GuCompatDeviceMatches(D))return;if(EFI_ERROR(GuDmaFree(P,A)))Gu.retained=TRUE;
}
int dma_set_mask(struct device *D,u64 Mask) {if(!GuCompatDeviceMatches(D)||!D->dma_mask)return -EINVAL;*D->dma_mask=Mask;return 0;}
int dma_set_coherent_mask(struct device *D,u64 Mask) {if(!GuCompatDeviceMatches(D))return -EINVAL;D->coherent_dma_mask=Mask;return 0;}
void dma_wmb(void) {
#if defined(_MSC_VER)
  _ReadWriteBarrier();
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
  if(Gu.pci->Flush && EFI_ERROR(Gu.pci->Flush(Gu.pci)))Gu.fault=TRUE;
}
void *kzalloc(size_t N,int Flags) {(void)Flags;return GuAlloc(N);}
void *kvmalloc(size_t N,int Flags) {(void)Flags;return GuAlloc(N);}
void *kvcalloc(size_t N,size_t S,int Flags) {
  (void)Flags;if(!S || N>(size_t)-1/S)return NULL;return GuAlloc(N*S);
}
int request_firmware(const struct firmware **Out,const char *Name,struct device *D) {
  UINTN I;if(!Out)return -EINVAL;*Out=NULL;
  if(!GuCompatDeviceMatches(D)||!Name)return -EINVAL;
  for(I=0;I<GU_ASSET_COUNT;I++)if(GuAssetEntries()[I].name && !strcmp(Name,GuAssetEntries()[I].name) &&
      GuAssetEntries()[I].firmware.data && GuAssetEntries()[I].firmware.size) {
    *Out=&GuAssetEntries()[I].firmware;return 0;
  }
  return -ENOENT;
}
void release_firmware(const struct firmware *F) {(void)F; /* Preloaded, app-owned immutable assets. */}
void __module_get(void *M) {(void)M;Gu.retained=TRUE;}
int pci_read_config_word(struct pci_dev *D,int O,u16 *V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,(UINT32)O,V))?-EIO:0;
}
int pci_read_config_dword(struct pci_dev *D,int O,u32 *V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)O,V))?-EIO:0;
}
int pci_write_config_word(struct pci_dev *D,int O,u16 V) {
  if(!GuCompatPciMatches(D))return -EINVAL;
  return EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,(UINT32)O,&V))?-EIO:0;
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
int pci_save_state(struct pci_dev *D) {if(!GuCompatPciMatches(D))return -EINVAL;return EFI_ERROR(GuSavePci())?-EIO:0;}
void pci_restore_state(struct pci_dev *D) {if(!GuCompatPciMatches(D))return;if(EFI_ERROR(GuRestorePci()))Gu.fault=TRUE;}
void pci_set_master(struct pci_dev *D) {
  if(GuCompatPciMatches(D))(void)GuEnableMaster();
}
void pci_clear_master(struct pci_dev *D) {if(GuCompatPciMatches(D))(void)GuClearMaster();}
int pcie_flr(struct pci_dev *D) {if(!GuCompatPciMatches(D))return -EINVAL;return EFI_ERROR(GuFlr())?-EIO:0;}
struct pci_dev *pci_dev_get(struct pci_dev *D) {if(!GuCompatPciMatches(D))return NULL;D->refs++;return D;}
void pci_dev_put(struct pci_dev *D) {if(GuCompatPciMatches(D))D->refs--;}
