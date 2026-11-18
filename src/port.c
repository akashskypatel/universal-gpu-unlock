/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #include "port.h"
#ifdef GU_ACTIVE_SINGLE_TARGET
#include "msi_release.h"
#include "profile.h"
#endif
VOID *memset(VOID *, int, size_t);
VOID *memcpy(VOID *, const VOID *, size_t);
int memcmp(const VOID *, const VOID *, size_t);
static GU_PLATFORM UnboundPlatform;
GU_PLATFORM *GuPlatformActive=&UnboundPlatform;
/* The allocation registry retains every published platform. The current
 * one-shot admission checks that registry, not a mutable active pointer. */
static BOOLEAN AnyPublishedPlatform(VOID);
static const GU_PLATFORM EmptyPlatform;
#ifdef GU_ENTRY_LATCHED
BOOLEAN GuGetEntrySelectorsFor(const GU_PLATFORM *platform,UINT32 *Ss0,UINT32 *Ss1) {
  if(!platform||!platform->entry_latched||!Ss0||!Ss1)return FALSE;
  *Ss0=platform->entry_ss0;*Ss1=platform->entry_ss1;return TRUE;
}
BOOLEAN GuGetEntrySelectors(UINT32 *Ss0,UINT32 *Ss1) {
  return GuGetEntrySelectorsFor(&Gu,Ss0,Ss1);
}
BOOLEAN GuEntrySelectorsMatchFor(const GU_PLATFORM *platform,UINT32 Ss0,UINT32 Ss1) {
  return platform&&platform->entry_latched&&platform->prom_verified&&!platform->fault&&
    Ss0==platform->entry_ss0&&Ss1==platform->entry_ss1;
}
BOOLEAN GuEntrySelectorsMatch(UINT32 Ss0,UINT32 Ss1) {
  return GuEntrySelectorsMatchFor(&Gu,Ss0,Ss1);
}
BOOLEAN GuLatchEntrySelectorsFor(GU_PLATFORM *platform,UINT32 Ss0,UINT32 Ss1) {
  if(!platform||platform->fault||!platform->prom_verified||platform->started||
     Ss0==0xffffffffU||(Ss0&0xffff0000U)==0xbadf0000U||
     Ss0==0x88888888U||Ss1>0xfU)return FALSE;
  if(platform->entry_latched)return GuEntrySelectorsMatchFor(platform,Ss0,Ss1);
  platform->entry_ss0=Ss0;platform->entry_ss1=Ss1;platform->entry_latched=TRUE;return TRUE;
}
BOOLEAN GuLatchEntrySelectors(UINT32 Ss0,UINT32 Ss1) {
  return GuLatchEntrySelectorsFor(&Gu,Ss0,Ss1);
}
#endif
#ifdef GU_ENTRY_LATCHED_SS1_FULL
BOOLEAN GuSelectorPhaseFor(GU_PLATFORM *platform,UINT32 Phase) {
  if(!platform)return FALSE;
  if(platform!=GuPlatformActive){platform->fault=TRUE;return FALSE;}
  UINT32 entry0,entry1,ss0,ss1;
  /* ENTRY is established only by GuInit, never by this transition API. */
  if(!((platform->selector_phase==GU_SELECTOR_PHASE_ENTRY && Phase==GU_SELECTOR_PHASE_SS0_WRITTEN && !platform->ss1_write_consumed) ||
       (platform->selector_phase==GU_SELECTOR_PHASE_SS0_WRITTEN && Phase==GU_SELECTOR_PHASE_SS1_WRITE && !platform->ss1_write_consumed) ||
       (platform->selector_phase==GU_SELECTOR_PHASE_SS1_WRITE && Phase==GU_SELECTOR_PHASE_TARGET && platform->ss1_write_consumed))) {
    platform->fault=TRUE;return FALSE;
  }
  if(!platform->profile_verified || !platform->armed || !platform->owned || platform->fault || !platform->prom_verified ||
     !GuGetEntrySelectorsFor(platform,&entry0,&entry1))return FALSE;
  ss0=GuRead32For(platform,0x82381c);ss1=GuRead32For(platform,0x823820);
  if(platform->fault || ss0!=0x88888888U ||
     (Phase==GU_SELECTOR_PHASE_SS0_WRITTEN && ss1!=entry1) ||
     (Phase==GU_SELECTOR_PHASE_SS1_WRITE && ss1!=entry1) ||
     (Phase==GU_SELECTOR_PHASE_TARGET && ss1!=0x8U)) {
    platform->fault=TRUE;return FALSE;
  }
  (void)entry0;
  platform->selector_phase=Phase;return TRUE;
}
#endif
EFI_STATUS GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci) {
  if(!Bs || !Pci)return EFI_INVALID_PARAMETER;
  if(AnyPublishedPlatform())return EFI_ALREADY_STARTED;
  /* Do not discard any state recorded before the first bind. */
  if(memcmp(&Gu,&EmptyPlatform,sizeof(Gu))!=0)return EFI_ACCESS_DENIED;
  GU_PLATFORM *selected=NULL;
  EFI_STATUS status=GuAllocatePlatform(Bs,Pci,&selected);
  if(status!=EFI_SUCCESS)return status;
  status=GuBindAllocatedPlatform(Bs,Pci,&selected);
  if(status!=EFI_SUCCESS) {
    EFI_STATUS released=GuDiscardUnboundPlatform(&selected);
    if(released!=EFI_SUCCESS)return released;
  }
  return status;
}

/* Each PCI protocol has at most one allocated platform in this image.
 * Only the boot entry path changes this list. Preserve immutable allocation
 * identity separately from the mutable platform state. */
typedef struct GU_PLATFORM_ALLOCATION {
  GU_PLATFORM platform;
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  BOOLEAN published,retired;
  struct GU_PLATFORM_ALLOCATION *next;
} GU_PLATFORM_ALLOCATION;
static GU_PLATFORM_ALLOCATION *PlatformAllocations;
/* An ambiguous pool result must not leave untrusted memory in the registry.
 * Keep the first address/status as CPU-only diagnostic evidence. */
static EFI_STATUS PlatformRegistryStatus;
static VOID *PlatformRegistryFailureAddress;
static VOID RecordPlatformRegistryFailure(EFI_STATUS status,VOID *address) {
  if(PlatformRegistryStatus==EFI_SUCCESS) {
    PlatformRegistryStatus=status;
    PlatformRegistryFailureAddress=address;
  }
}

/* Publication is irreversible for this boot image. A context that has
 * ever owned the active slot cannot be disposed as an unused candidate. */
static BOOLEAN AnyPublishedPlatform(VOID) {
  for(const GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;entry;entry=entry->next)
    if(entry->published||entry->retired)return TRUE;
  return FALSE;
}

static BOOLEAN PlatformPristine(const GU_PLATFORM_ALLOCATION *entry) {
  if(entry->published||entry->retired)return FALSE;
  GU_PLATFORM pristine;
  memset(&pristine,0,sizeof(pristine));
  pristine.bs=entry->bs;pristine.pci=entry->pci;
#ifdef GU_ENTRY_LATCHED_SS1_FULL
  pristine.selector_phase=GU_SELECTOR_PHASE_ENTRY;
#endif
  return memcmp(&entry->platform,&pristine,sizeof(pristine))==0;
}

/* Allocate CPU-only state without publishing or changing the active card. */
EFI_STATUS GuAllocatePlatform(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci,
                              GU_PLATFORM **Out) {
  if(!Bs || !Pci || !Out)return EFI_INVALID_PARAMETER;
  if(*Out)return EFI_ALREADY_STARTED;
  if(PlatformRegistryStatus!=EFI_SUCCESS)return PlatformRegistryStatus;
  if(!Bs->AllocatePool)return EFI_UNSUPPORTED;
  for(GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;entry;entry=entry->next)
    if(entry->pci==Pci)return EFI_ALREADY_STARTED;
  GU_PLATFORM_ALLOCATION *entry=NULL;
  EFI_STATUS status=Bs->AllocatePool(EfiLoaderData,sizeof(*entry),
                                    (VOID **)&entry);
  if(status!=EFI_SUCCESS) {
    if(entry)RecordPlatformRegistryFailure(status,entry);
    return status;
  }
  if(!entry)return EFI_OUT_OF_RESOURCES;
  memset(entry,0,sizeof(*entry));
  GU_PLATFORM *platform=&entry->platform;
  entry->bs=Bs;entry->pci=Pci;
  platform->bs=Bs;platform->pci=Pci;
#ifdef GU_ENTRY_LATCHED_SS1_FULL
  platform->selector_phase=GU_SELECTOR_PHASE_ENTRY;
#endif
  entry->next=PlatformAllocations;
  PlatformAllocations=entry;
  *Out=platform;
  return EFI_SUCCESS;
}

/* Transfer only the first, registered, unused platform to the active slot.
 * The caller has already checked the device; this operation does not claim
 * a PCI protocol or grant permission for hardware operations. */
EFI_STATUS GuBindAllocatedPlatform(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci,
                                   GU_PLATFORM **Owner) {
  if(!Bs || !Pci || !Owner || !*Owner)return EFI_INVALID_PARAMETER;
  if(PlatformRegistryStatus!=EFI_SUCCESS)return PlatformRegistryStatus;
  if(AnyPublishedPlatform())return EFI_ALREADY_STARTED;
  if(GuPlatformActive!=&UnboundPlatform ||
     memcmp(&Gu,&EmptyPlatform,sizeof(Gu))!=0)return EFI_ACCESS_DENIED;
  GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;
  while(entry && &entry->platform!=*Owner)entry=entry->next;
  if(!entry || entry->bs!=Bs || entry->pci!=Pci ||
     !PlatformPristine(entry))return EFI_ACCESS_DENIED;
  entry->published=TRUE; /* Spend before publishing the bound platform. */
  GuPlatformActive=&entry->platform;
  *Owner=NULL;
  return EFI_SUCCESS;
}

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Shared by advance and final handover. Validate the record before reading
 * its order, and every completed context without changing GuPlatformActive. */
EFI_STATUS GuVerifyCompletedPlatforms(EFI_BOOT_SERVICES *bs,
                                      const GU_GPU_EXEC_SEQUENCE *sequence,
                                      const GU_GPU_EXEC_OBSERVATION *observations,
                                      size_t count) {
  if(PlatformRegistryStatus!=EFI_SUCCESS)return PlatformRegistryStatus;
  if(!bs||!sequence||!observations||!sequence->next_step||!sequence->owned_mode)
    return EFI_NOT_READY;
  if(GuGpuExecCheckNext(sequence,observations,count)!=GU_GPU_EXEC_OK &&
     GuGpuExecCheckComplete(sequence,observations,count)!=GU_GPU_EXEC_OK)
    return EFI_NOT_READY;
  GU_PLATFORM_ALLOCATION *current=NULL;
  size_t published=0;
  for(GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;entry;entry=entry->next) {
    if(entry->published||entry->retired)published++;
    if(&entry->platform==GuPlatformActive)current=entry;
  }
  if(!current||!current->published||current->retired||
     published!=sequence->next_step)return EFI_ACCESS_DENIED;
  /* Never trust an owner token until the registry resolves its address.
   * All earlier contexts must remain complete, clean and independently
   * allocated. A fault on a retained peer must block the next selection. */
  for(size_t i=0;i<sequence->next_step;i++) {
    const GU_GPU_EXEC_TARGET *record=&sequence->targets[sequence->order[i]];
    GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;
    while(entry && &entry->platform!=record->owners.platform)entry=entry->next;
    if(!entry||!entry->published||entry->bs!=bs||
       entry->pci!=record->identity.pci||
       record->phase!=GU_GPU_EXEC_COMPLETE||
       (i+1==sequence->next_step?(entry!=current||entry->retired):(entry==current||!entry->retired)))
      return EFI_ACCESS_DENIED;
    const GU_PLATFORM *platform=&entry->platform;
    if(platform->bs!=bs||platform->pci!=entry->pci||
       platform->handle!=record->identity.handle||!platform->image||
       platform->image!=Gu.image||
       !GuGpuRecordMatches(&platform->discovery_identity,&record->identity)||
       platform->device_id!=(UINT16)(record->identity.pci_id>>16)||
       !platform->profile_verified||!platform->prom_verified||
       !platform->release_state_reserved||platform->release_owner_invalid||
       !platform->release_state||platform->release_state!=record->owners.release_plan||
       !platform->asset_entries_reserved||!platform->asset_load_attempted||
       platform->asset_entries!=record->owners.firmware_assets||
       !GuPlatformAssetsOwned(platform)||
       platform->fault||platform->retained||platform->owned||
       platform->allocations||platform->dma_count||platform->master_attributes_pending||
       !platform->quiesced||platform->flrs!=1||
       !GuCompatCompletedPlatform(platform)||
       !GuMsiCompletedPlatform(platform,record->owners.release_plan))
      return EFI_ACCESS_DENIED;
  }
  return EFI_SUCCESS;
}

/* Retirement is permanent evidence of earlier hardware work. A failure
 * on a fresh context must not hide that work or allow stock handover. */
BOOLEAN GuAnyPlatformNeedsContainment(VOID) {
  BOOLEAN found=FALSE;
  for(const GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;entry;entry=entry->next) {
    if(!entry->published&&!entry->retired)continue;
    if(entry->retired||&entry->platform!=GuPlatformActive)return TRUE;
    found=TRUE;
  }
  return !found&&GuPlatformActive!=&UnboundPlatform;
}

/* A completed platform remains registered for the entire boot. Transfer
 * the active CPU binding only after independent MSI handover verification
 * and a checked next step. No hardware ownership is acquired here. */
EFI_STATUS GuAdvanceAllocatedPlatform(EFI_BOOT_SERVICES *bs,GU_PLATFORM **owner,
                                      const GU_GPU_EXEC_SEQUENCE *sequence,
                                      const GU_GPU_EXEC_OBSERVATION *observations,
                                      size_t count) {
  if(!bs||!owner||!*owner||!sequence||!observations)return EFI_INVALID_PARAMETER;
  if(PlatformRegistryStatus!=EFI_SUCCESS)return PlatformRegistryStatus;
  if(GuGpuExecCheckNext(sequence,observations,count)!=GU_GPU_EXEC_OK ||
     !sequence->owned_mode || !sequence->next_step)
    return EFI_NOT_READY;
  GU_PLATFORM_ALLOCATION *current=NULL,*candidate=NULL;
  size_t published=0;
  for(GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;entry;entry=entry->next) {
    if(entry->published)published++;
    if(&entry->platform==GuPlatformActive)current=entry;
    if(&entry->platform==*owner)candidate=entry;
  }
  size_t next=sequence->order[sequence->next_step];
  if(!current||!candidate||current==candidate||!current->published||
     current->retired||candidate->bs!=bs||!PlatformPristine(candidate)||
     candidate->pci!=sequence->targets[next].identity.pci||
     published!=sequence->next_step)
    return EFI_ACCESS_DENIED;
  if(GuVerifyCompletedPlatforms(bs,sequence,observations,count)!=EFI_SUCCESS)
    return EFI_ACCESS_DENIED;
  /* This is the serialized boot entry path. The previous GPU's live
   * post-close proof must pass before the active pointer can change. */
  const VOID *release=sequence->targets[sequence->order[sequence->next_step-1]].owners.release_plan;
  if(GuMsiPlanOwnerToken()!=release||!GuMsiReadyForWindows()||
     GuPlatformActive!=&current->platform||GuMsiPlanOwnerToken()!=release||
     !GuSafeToReturn()||Gu.fault||Gu.owned||Gu.release_owner_invalid||
     *owner!=&candidate->platform||!PlatformPristine(candidate))
    return EFI_NOT_READY;
  current->retired=TRUE;
  candidate->published=TRUE;
  GuPlatformActive=&candidate->platform;
  *owner=NULL;
  return EFI_SUCCESS;
}
#endif

EFI_STATUS GuDiscardUnboundPlatform(GU_PLATFORM **Out) {
  if(!Out || !*Out)return EFI_INVALID_PARAMETER;
  GU_PLATFORM *platform=*Out;
  if(platform==GuPlatformActive || platform==&UnboundPlatform)
    return EFI_ACCESS_DENIED;
  GU_PLATFORM_ALLOCATION **link=&PlatformAllocations;
  while(*link && &(*link)->platform!=platform)link=&(*link)->next;
  if(!*link)return EFI_ACCESS_DENIED;
  GU_PLATFORM_ALLOCATION *entry=*link;
  if(!entry->bs->FreePool)return EFI_UNSUPPORTED;
  /* Only a pristine object from GuAllocatePlatform is disposable.
   * No saved configuration, allocations, protocol state or errors may
   * be discarded. This is not an active-platform teardown operation. */
  if(!PlatformPristine(entry))return EFI_ACCESS_DENIED;
  /* Consume the confirmed candidate before calling firmware. An error is
   * not proof that the original pool still exists. Do not retry or dereference. */
  EFI_BOOT_SERVICES *bs=entry->bs;
  *link=entry->next;
  *Out=NULL;
  EFI_STATUS status=bs->FreePool(entry);
  if(status!=EFI_SUCCESS)RecordPlatformRegistryFailure(status,entry);
  return status;
}

BOOLEAN GuTargetConfig(const UINT32 *C, UINTN S, UINTN B, UINTN D, UINTN F) {
  return C && S <= 0xffffU && B < 256 && D < 32 && F < 8 &&
    GuIsSupportedPciId(C[0]) &&
    ((C[2]>>16)==0x0300U || (C[2]>>16)==0x0302U) && !((C[3]>>16)&0x7f);
}
BOOLEAN GuSafeToReturnFor(const GU_PLATFORM *platform) {
  return platform&&!platform->retained&&!platform->allocations&&!platform->dma_count&&
    !platform->master_attributes_pending&&
    (!platform->started||(platform->quiesced&&!platform->fault));
}
BOOLEAN GuSafeToReturn(VOID) {return GuSafeToReturnFor(&Gu);}
/* CPU pool records belong to a single platform. The payload follows a
 * naturally aligned header on the supported x64 ABI. A nonnull scope
 * identifies temporary compatibility transaction memory. Resolve a release
 * through the owner's list, not an untrusted payload. */
typedef struct GU_CPU_POOL {
  struct GU_CPU_POOL *next;
  GU_PLATFORM *owner;
  EFI_BOOT_SERVICES *bs;
  size_t bytes;
  const struct pci_dev *scope;
} GU_CPU_POOL;

BOOLEAN GuCpuPoolOwns(const GU_PLATFORM *platform,const VOID *memory,size_t bytes) {
  if(!platform||!memory||!bytes||!platform->bs||
     platform->cpu_pool_failure_status!=EFI_SUCCESS)return FALSE;
  for(const GU_CPU_POOL *entry=platform->cpu_pool;entry;entry=entry->next)
    if((const VOID *)(entry+1)==memory)
      return entry->owner==platform&&entry->bs==platform->bs&&entry->bytes==bytes&&!entry->scope;
  return FALSE;
}

GU_PLATFORM *GuPlatformForCpuObject(const VOID *memory,size_t bytes) {
  if(!memory||!bytes||PlatformRegistryStatus!=EFI_SUCCESS)return NULL;
  GU_PLATFORM *owner=NULL;
  for(GU_PLATFORM_ALLOCATION *entry=PlatformAllocations;entry;entry=entry->next) {
    if(!(entry->published||entry->retired)||
       !GuCpuPoolOwns(&entry->platform,memory,bytes))continue;
    if(owner)return NULL;
    owner=&entry->platform;
  }
  return owner;
}

/* General CPU allocations may survive compatibility scope closure, but
 * temporary transaction allocations must not. Check inactive owners too. */
BOOLEAN GuCpuPoolCompatEmpty(const GU_PLATFORM *platform) {
  if(!platform||!platform->bs||platform->cpu_pool_failure_status!=EFI_SUCCESS)return FALSE;
  for(const GU_CPU_POOL *entry=platform->cpu_pool;entry;entry=entry->next)
    if(entry->owner!=platform||entry->bs!=platform->bs||!entry->bytes||entry->scope)return FALSE;
  return TRUE;
}
/* Keep the original owner when a firmware service returns an uncertain
 * result. Never attribute an error to a different active platform. */
static VOID RecordCpuPoolFailureFor(GU_PLATFORM *owner,EFI_STATUS status,VOID *address) {
  if(owner->cpu_pool_failure_status==EFI_SUCCESS) {
    owner->cpu_pool_failure_status=status;
    owner->cpu_pool_failure_address=address;
  }
  owner->fault=TRUE;
  owner->retained=TRUE;
}
VOID GuRecordCpuPoolFailure(EFI_STATUS status,VOID *address) {
  RecordCpuPoolFailureFor(&Gu,status,address);
}

static VOID *PoolAllocate(GU_PLATFORM *owner,const struct pci_dev *scope,size_t N) {
  GU_CPU_POOL *entry=NULL;
  if(!owner||owner!=GuPlatformActive||!N||N>(size_t)-1-sizeof(*entry)||owner->retained||
     owner->cpu_pool_failure_status!=EFI_SUCCESS||!owner->bs||
     !owner->bs->AllocatePool||!owner->bs->FreePool)return NULL;
  EFI_BOOT_SERVICES *bs=owner->bs;
  EFI_STATUS status=bs->AllocatePool(EfiLoaderData,sizeof(*entry)+N,(VOID **)&entry);
  if(owner!=GuPlatformActive||owner->bs!=bs||owner->retained||
     owner->cpu_pool_failure_status!=EFI_SUCCESS) {
    RecordCpuPoolFailureFor(owner,status==EFI_SUCCESS?EFI_ACCESS_DENIED:status,entry);
    return NULL;
  }
  if(status!=EFI_SUCCESS) {
    if(entry)RecordCpuPoolFailureFor(owner,status,entry);
    return NULL;
  }
  if(!entry)return NULL;
  entry->next=owner->cpu_pool;
  entry->owner=owner;
  entry->bs=bs;
  entry->bytes=N;
  entry->scope=scope;
  owner->cpu_pool=entry;
  VOID *memory=entry+1;
  memset(memory,0,N);
  return memory;
}

static BOOLEAN PoolFree(GU_PLATFORM *owner,const struct pci_dev *scope,VOID *Memory) {
  if(!Memory)return TRUE;
  if(!owner||owner!=GuPlatformActive||owner->retained||
     owner->cpu_pool_failure_status!=EFI_SUCCESS)return FALSE;
  /* Published ownership records must not be recycled in this boot. */
  if(Memory==owner->compat_device||Memory==(VOID *)owner->release_state) {
    RecordCpuPoolFailureFor(owner,EFI_ACCESS_DENIED,Memory);
    return FALSE;
  }
  GU_CPU_POOL **link=&owner->cpu_pool;
  while(*link&&(VOID *)(*link+1)!=Memory)link=&(*link)->next;
  GU_CPU_POOL *entry=*link;
  if(!entry||entry->owner!=owner||entry->bs!=owner->bs||entry->scope!=scope||
     !entry->bs||!entry->bs->FreePool) {
    RecordCpuPoolFailureFor(owner,EFI_ACCESS_DENIED,Memory);
    return FALSE;
  }
  /* Unlink before calling firmware. Never dereference or retry the
   * consumed reference, even if the owner or service identity changes. */
  EFI_BOOT_SERVICES *bs=entry->bs;
  *link=entry->next;
  EFI_STATUS status=bs->FreePool(entry);
  if(status!=EFI_SUCCESS)RecordCpuPoolFailureFor(owner,status,entry);
  if(owner!=GuPlatformActive||owner->bs!=bs) {
    RecordCpuPoolFailureFor(owner,EFI_ACCESS_DENIED,entry);
    return FALSE;
  }
  return status==EFI_SUCCESS;
}
/* CPU-only access for callers with an already identified active owner. */
VOID *GuAllocFor(GU_PLATFORM *owner,size_t N) {
  return PoolAllocate(owner,NULL,N);
}
BOOLEAN GuFreeFor(GU_PLATFORM *owner,VOID *Memory) {
  if(!owner||owner!=GuPlatformActive)return FALSE;
  return PoolFree(owner,NULL,Memory);
}
VOID *GuAlloc(size_t N) {return PoolAllocate(&Gu,NULL,N);}
VOID GuFree(VOID *Memory) {(VOID)PoolFree(&Gu,NULL,Memory);}
static BOOLEAN PoolScopeMatches(GU_PLATFORM *owner,const struct pci_dev *scope) {
  if(scope&&scope==owner->compat_device&&owner->compat_device_open&&!owner->compat_device_closed)return TRUE;
  owner->fault=TRUE;
  return FALSE;
}
VOID *GuCpuPoolAllocScoped(const struct pci_dev *scope,size_t bytes) {
  GU_PLATFORM *owner=GuPlatformActive;
  if(!owner||!PoolScopeMatches(owner,scope)||owner->fault)return NULL;
  return PoolAllocate(owner,scope,bytes);
}
BOOLEAN GuCpuPoolFreeScoped(const struct pci_dev *scope,VOID *memory) {
  GU_PLATFORM *owner=GuPlatformActive;
  if(!owner||!PoolScopeMatches(owner,scope))return FALSE;
  return PoolFree(owner,scope,memory);
}
VOID GuStartedFor(GU_PLATFORM *platform) {
  if(!platform)return;
  platform->started=TRUE;platform->quiesced=FALSE;
}
VOID GuStarted(VOID) {GuStartedFor(&Gu);}
VOID GuQuiescedFor(GU_PLATFORM *platform) {
  if(platform&&!platform->fault)platform->quiesced=TRUE;
}
VOID GuQuiesced(VOID) {GuQuiescedFor(&Gu);}
EFI_STATUS GuDmaAllocate(GU_PLATFORM *owner,UINTN N,VOID **C,UINT64 *D) {
  EFI_STATUS Status; GU_DMA *Entry; UINTN Mapped;
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  if(!C || !D)return EFI_INVALID_PARAMETER;
  *C=NULL;*D=0;
  if(GU_PREFLIGHT_ONLY || !owner->profile_verified || !owner->armed || !owner->display_ok || !N || N>(128U<<20) || owner->fault || owner->retained || !owner->pci || !owner->owned)return EFI_INVALID_PARAMETER;
  Entry=GuAlloc(sizeof(*Entry));if(!Entry)return EFI_OUT_OF_RESOURCES;
  Entry->pages=(N+4095)/4096;Entry->bytes=Entry->pages*4096;
  /* The falcon DMA registers carry 32-bit addresses. Ask for a buffer inside
   * that window instead of relying on where AllocateAnyPages happens to land,
   * which on a large-memory machine is routinely above 4 GiB. The sentinel is
   * page-unaligned, so a real allocation can never be mistaken for "the
   * firmware did not touch the output". */
  {
    /* Take ownership of any output the firmware wrote, including one above
     * the ceiling: the mapped address below decides usability, and a buffer
     * that exists must never be dropped. */
    VOID *Buffer=(VOID *)(UINTN)~(UINTN)0;
    Status=owner->pci->AllocateBuffer(owner->pci,AllocateMaxAddress,EfiBootServicesData,
      Entry->pages,&Buffer,0);
    if(Buffer&&Buffer!=(VOID *)(UINTN)~(UINTN)0)Entry->cpu=Buffer;
  }
  if(EFI_ERROR(Status) || !Entry->cpu) {
    /* The service may report an error after writing an output pointer.
     * Never discard ownership of a possibly allocated DMA-capable buffer. */
    if(Entry->cpu) {
      Entry->next=owner->allocations;owner->allocations=Entry;owner->retained=TRUE;
    } else {
      GuFree(Entry);
      if(!EFI_ERROR(Status))owner->retained=TRUE; /* Success without a buffer. */
    }
    return EFI_ERROR(Status)?Status:EFI_OUT_OF_RESOURCES;
  }
  memset(Entry->cpu,0,Entry->bytes);Mapped=Entry->bytes;
  Status=owner->pci->Map(owner->pci,EfiPciIoOperationBusMasterCommonBuffer,Entry->cpu,
    &Mapped,&Entry->dma,&Entry->mapping);
  /* A failed Map call cannot prove that a device mapping was absent,
   * even when the firmware left every output unchanged. Never free a
   * buffer that the GPU may still be able to access. */
  if(EFI_ERROR(Status)) {
    Entry->next=owner->allocations;owner->allocations=Entry;owner->retained=TRUE;
    return Status;
  }
  Entry->mapped=TRUE;
  if(Mapped!=Entry->bytes || !Entry->dma ||
     (Entry->dma&4095) || Entry->dma>0xffffffffULL || Entry->bytes-1>0xffffffffULL-Entry->dma) {
    Status=EFI_UNSUPPORTED;
    if(Entry->mapped && EFI_ERROR(owner->pci->Unmap(owner->pci,Entry->mapping))) {
      Entry->next=owner->allocations;owner->allocations=Entry;owner->retained=TRUE;return Status;
    }
    if(EFI_ERROR(owner->pci->FreeBuffer(owner->pci,Entry->pages,Entry->cpu))) {
      Entry->mapped=FALSE;Entry->next=owner->allocations;owner->allocations=Entry;owner->retained=TRUE;return Status;
    }
    GuFree(Entry);return Status;
  }
  Entry->next=owner->allocations;owner->allocations=Entry;owner->dma_count++;
  *C=Entry->cpu;*D=Entry->dma;
  if(!(owner->dma_count%2048))GuLog("DMA buffers mapped: %u\n",owner->dma_count);
  return EFI_SUCCESS;
}
EFI_STATUS GuDmaFree(GU_PLATFORM *owner,VOID *C,UINT64 D) {
  GU_DMA **Link;GU_DMA *Entry;EFI_STATUS Status;
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  Link=&owner->allocations;
  while(*Link && ((*Link)->cpu!=C || (*Link)->dma!=D))Link=&(*Link)->next;
  if(!*Link)return EFI_NOT_FOUND;
  Entry=*Link;
  if(owner->retained || owner->master_attributes_pending || (owner->started && (!owner->quiesced || owner->fault))) {
    owner->retained=TRUE;return EFI_ACCESS_DENIED;
  }
  if(Entry->mapped) {
    Status=owner->pci->Unmap(owner->pci,Entry->mapping);
    if(EFI_ERROR(Status)) {owner->retained=TRUE;return Status;}
    Entry->mapped=FALSE;
  }
  Status=owner->pci->FreeBuffer(owner->pci,Entry->pages,Entry->cpu);
  if(EFI_ERROR(Status)) {owner->retained=TRUE;return Status;}
  /* GPU-visible buffer release succeeded. Remove its descriptor once;
   * report a CPU-pool release error without retrying either release. */
  *Link=Entry->next;
  if(owner->dma_count)owner->dma_count--;
  GuFree(Entry);
  return owner->cpu_pool_failure_status;
}
UINT32 GuRead32For(GU_PLATFORM *platform,UINT32 O) {
  UINT32 Value=0xffffffffU;
  if(!platform)return Value;
  if(platform!=GuPlatformActive){platform->fault=TRUE;return Value;}
  if(platform->fault)return Value;
  if(!platform->pci||(O&3)||(UINT64)O+4>platform->bar_size||
     EFI_ERROR(platform->pci->Mem.Read(platform->pci,EfiPciIoWidthUint32,0,O,1,&Value))) {
    platform->fault=TRUE;return 0xffffffffU;
  }
  return Value;
}
UINT32 GuRead32(UINT32 O) {return GuRead32For(&Gu,O);}
VOID GuWrite32(GU_PLATFORM *owner,UINT32 O,UINT32 V) {
  if(!owner)return;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return;}
  if(owner->fault)return;
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
  if(O==0x823820){owner->fault=TRUE;return;}
#endif
  if(GU_PREFLIGHT_ONLY || !owner->profile_verified || !owner->pci || !owner->armed || !owner->owned || !owner->display_ok || (O&3) ||
      (UINT64)O+4>owner->bar_size) {owner->fault=TRUE;return;}
#ifdef GU_ENTRY_LATCHED_SS1_FULL
  if(O==0x823820 && (V!=0x8U || owner->selector_phase!=GU_SELECTOR_PHASE_SS1_WRITE || owner->ss1_write_consumed)) {
    owner->fault=TRUE;return;
  }
  /* Spend before the external callback: reentry and errors cannot retry. */
  if(O==0x823820)owner->ss1_write_consumed=TRUE;
#endif
  owner->mmio_writes++;
  if(EFI_ERROR(owner->pci->Mem.Write(owner->pci,EfiPciIoWidthUint32,0,O,1,&V)))owner->fault=TRUE;
}
static EFI_STATUS RawReadFor(GU_PLATFORM *owner,EFI_PCI_IO_PROTOCOL_WIDTH W,UINT32 O,VOID *V) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  if(!owner->pci || W>EfiPciIoWidthUint32 || O>256-((UINT32)1<<W))return EFI_INVALID_PARAMETER;
  return owner->pci->Pci.Read(owner->pci,W,O,1,V);
}
EFI_STATUS GuConfigReadFor(GU_PLATFORM *platform,EFI_PCI_IO_PROTOCOL_WIDTH W,
                              UINT32 O,VOID *V) {
  EFI_STATUS Status;
  if(!platform)return EFI_INVALID_PARAMETER;
  if(platform!=GuPlatformActive){platform->fault=TRUE;return EFI_ACCESS_DENIED;}
  if(platform->fault)return EFI_DEVICE_ERROR;
  if(!platform->pci||W>EfiPciIoWidthUint32||O>256-((UINT32)1<<W)) {
    platform->fault=TRUE;return EFI_INVALID_PARAMETER;
  }
  Status=platform->pci->Pci.Read(platform->pci,W,O,1,V);
  if(EFI_ERROR(Status))platform->fault=TRUE;
  return Status;
}
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH W,UINT32 O,VOID *V) {
  return GuConfigReadFor(&Gu,W,O,V);
}
EFI_STATUS GuConfigWrite(GU_PLATFORM *owner,EFI_PCI_IO_PROTOCOL_WIDTH W,UINT32 O,VOID *V) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  EFI_STATUS Status;
  if(owner->fault)return EFI_DEVICE_ERROR;
  if(GU_PREFLIGHT_ONLY || !owner->profile_verified || !owner->pci || !owner->owned || !owner->armed || !owner->display_ok || W>EfiPciIoWidthUint32 ||
     O>256-((UINT32)1<<W)) {owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  Status=owner->pci->Pci.Write(owner->pci,W,O,1,V);
  if(EFI_ERROR(Status))owner->fault=TRUE;return Status;
}
VOID GuStall(GU_PLATFORM *owner,UINTN Microseconds) {
  if(!owner)return;
  if(owner!=GuPlatformActive){owner->fault=TRUE;owner->retained=TRUE;return;}
  /* This delay is a safety boundary after PCI function-level reset.
   * A failed firmware stall is not proof that the required time elapsed. */
  if(!owner->bs || !owner->bs->Stall ||
     EFI_ERROR(owner->bs->Stall(Microseconds))) {
    owner->fault=TRUE;
    owner->retained=TRUE;
    return;
  }
  owner->ticks+=Microseconds;
}
EFI_STATUS GuEnableMaster(GU_PLATFORM *owner) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;
  UINT64 Supported=0,Before=0,After=0,Decode;
  EFI_STATUS Status=EFI_ACCESS_DENIED;
  if(GU_PREFLIGHT_ONLY || !owner->profile_verified || owner->fault || owner->retained || owner->started || owner->master_attributes_pending ||
     !owner->pci || !owner->pci->Attributes || !owner->owned || !owner->armed || !owner->display_ok)goto fail;
  Status=RawReadFor(owner,EfiPciIoWidthUint32,0,&Id);
  if(EFI_ERROR(Status) || !GuIsSupportedPciId(Id))goto fail;
  Status=RawReadFor(owner,EfiPciIoWidthUint16,4,&Command);
  if(EFI_ERROR(Status) || Command==0xffff || (Command&4) || !(Command&2))goto fail;
  Status=owner->pci->Attributes(owner->pci,EfiPciIoAttributeOperationSupported,0,&Supported);
  if(EFI_ERROR(Status) || !(Supported&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=owner->pci->Attributes(owner->pci,EfiPciIoAttributeOperationGet,0,&Before);
  Decode=(Command&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Command&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(EFI_ERROR(Status) || (Before&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
     (Before&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY))!=Decode)goto fail;
  GuLog("Bus Master enable intent: command=%#x attributes=%#llx\n",
    Command,(unsigned long long)Before);
  if(owner->fault){Status=EFI_DEVICE_ERROR;goto fail;}
  owner->master_original_attributes=Before;
  owner->master_original_command=Command;
  owner->master_pci=owner->pci;
#ifdef GU_ENTRY_LATCHED
  /* No logging/callback window between this final comparison and enabling BME. */
  UINT32 Ss0=GuRead32For(owner,0x82381c),Ss1=GuRead32For(owner,0x823820);
  if(!GuEntrySelectorsMatchFor(owner,Ss0,Ss1)){Status=EFI_NOT_READY;goto fail;}
#endif
  /* Track before the call: an error may be returned after a hardware change. */
  owner->master_attributes_pending=TRUE;
  Status=owner->pci->Attributes(owner->pci,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,NULL);
  if(EFI_ERROR(Status))goto fail;
  Status=owner->pci->Attributes(owner->pci,EfiPciIoAttributeOperationGet,0,&After);
  if(EFI_ERROR(Status) || After!=(Before|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=RawReadFor(owner,EfiPciIoWidthUint16,4,&Now);
  if(EFI_ERROR(Status) || Now!=(UINT16)(Command|4))goto fail;
  GuLog("Bus Master enabled: command=%#x attributes=%#llx matched=1\n",
    Now,(unsigned long long)After);
  if(owner->fault){Status=EFI_DEVICE_ERROR;goto fail;}
  return EFI_SUCCESS;
fail:
  owner->fault=TRUE;
  if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  GuLog("Bus Master enable failed: status=%#llx command=%#x readback=%#x managed=%u\n",
    (unsigned long long)Status,Command,Now,owner->master_attributes_pending);
  return Status;
}
EFI_STATUS GuClearMaster(GU_PLATFORM *owner) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;owner->retained=TRUE;return EFI_ACCESS_DENIED;}
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;EFI_STATUS Status;
  EFI_STATUS AttributeStatus=EFI_SUCCESS;UINT64 Attributes=~0ULL;
  if(GU_PREFLIGHT_ONLY)return EFI_ACCESS_DENIED;
  /* Containment remains permitted after a latched I/O failure. */
  Status=RawReadFor(owner,EfiPciIoWidthUint32,0,&Id);
  if(EFI_ERROR(Status) || !GuIsSupportedPciId(Id))goto fail;
  if(owner->master_attributes_pending) {
    if(owner->pci!=owner->master_pci)AttributeStatus=EFI_COMPROMISED_DATA;
    else if(!owner->pci->Attributes)AttributeStatus=EFI_UNSUPPORTED;
    else {
      AttributeStatus=owner->pci->Attributes(owner->pci,EfiPciIoAttributeOperationDisable,
        EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,NULL);
      EFI_STATUS ReadStatus=owner->pci->Attributes(owner->pci,EfiPciIoAttributeOperationGet,0,&Attributes);
      if(!EFI_ERROR(AttributeStatus) && EFI_ERROR(ReadStatus))AttributeStatus=ReadStatus;
      if(!EFI_ERROR(AttributeStatus) && Attributes!=owner->master_original_attributes)
        AttributeStatus=EFI_DEVICE_ERROR;
    }
    owner->master_last_attributes=Attributes;
    if(EFI_ERROR(AttributeStatus) && !EFI_ERROR(owner->master_attribute_status))
      owner->master_attribute_status=AttributeStatus;
  }
  /* Always check/clear hardware even if the protocol call failed or lied. */
  Status=RawReadFor(owner,EfiPciIoWidthUint16,4,&Command);
  if(EFI_ERROR(Status) || Command==0xffff)goto fail;
  if(Command&4) {
    Command&=(UINT16)~4U;
    Status=owner->pci->Pci.Write(owner->pci,EfiPciIoWidthUint16,4,1,&Command);
    if(EFI_ERROR(Status))goto fail;
  }
  Status=RawReadFor(owner,EfiPciIoWidthUint16,4,&Now);
  if(EFI_ERROR(Status) || Now==0xffff || (Now&4))goto fail;
  /* Do not silently repair unrelated bits changed by the bus driver's API. */
  if(owner->master_attributes_pending && Now!=owner->master_original_command)goto fail;
  owner->master_last_command=Now;
  if(EFI_ERROR(AttributeStatus)) {
    owner->fault=TRUE;owner->retained=TRUE;return AttributeStatus;
  }
  owner->master_attributes_pending=FALSE;
  return EFI_SUCCESS;
fail:
  owner->master_last_command=Now;
  if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(owner->master_hardware_status))owner->master_hardware_status=Status;
  owner->fault=TRUE;owner->retained=TRUE;return Status;
}
EFI_STATUS GuSavePci(GU_PLATFORM *owner) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  UINTN I;
  if(owner->saved || owner->fault || !owner->owned || owner->pcie_cap<0x40 || owner->pcie_cap>0xd4)return EFI_NOT_READY;
  for(I=0;I<64;I++)
    if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint32,(UINT32)I*4,&owner->saved_config[I])))return EFI_DEVICE_ERROR;
  if(!GuIsSupportedPciId(owner->saved_config[0]) ||
     (owner->saved_config[1]&4))return EFI_ACCESS_DENIED;
  if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,owner->pcie_cap+8,&owner->saved_devctl)))return EFI_DEVICE_ERROR;
  if(owner->saved_devctl&0x8000)return EFI_NOT_READY;
  if(owner->pcie_version>=2 && EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,
     owner->pcie_cap+0x28,&owner->saved_devctl2)))return EFI_DEVICE_ERROR;
  owner->saved=TRUE;return EFI_SUCCESS;
}
EFI_STATUS GuFlr(GU_PLATFORM *owner) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  UINT32 Identity=0,Capability=0;UINT16 Command=0xffff,Pending=0xffff,Ctl=0;
  UINTN Wait;EFI_STATUS ResetStatus;
  if(GU_PREFLIGHT_ONLY || !owner->profile_verified || !owner->saved || owner->flrs || owner->fault || !owner->owned || !owner->armed || !owner->display_ok ||
     !owner->bs || !owner->bs->Stall)return EFI_NOT_READY;
  if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint32,0,&Identity)) || !GuIsSupportedPciId(Identity) ||
     EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,4,&Command)) || Command==0xffff ||
     (Command&4) || !(Command&2) ||
     EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint32,owner->pcie_cap+4,&Capability)) ||
     !(Capability&(1U<<28)))return EFI_ACCESS_DENIED;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,owner->pcie_cap+10,&Pending)) || Pending==0xffff)return EFI_DEVICE_ERROR;
    if(!(Pending&0x20))break;
    if(Wait==1000000)return EFI_TIMEOUT;
    GuStall(owner,1000);
    if(owner->fault)return EFI_DEVICE_ERROR;
  }
  if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,owner->pcie_cap+8,&Ctl)))return EFI_DEVICE_ERROR;
  if(Ctl&0x8000)return EFI_NOT_READY;
#ifdef GU_ENTRY_LATCHED
  UINT32 Ss0=GuRead32For(owner,0x82381c),Ss1=GuRead32For(owner,0x823820),Entry0,Entry1;
  if(!owner->prom_verified || owner->fault || !GuGetEntrySelectorsFor(owner,&Entry0,&Entry1) ||
     Ss0!=0x88888888U ||
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
     Ss1!=Entry1
#else
     Ss1!=0x8U || owner->selector_phase!=GU_SELECTOR_PHASE_TARGET
#endif
     )return EFI_NOT_READY;
#endif
  Ctl|=0x8000;owner->flrs++;GuStartedFor(owner);
  ResetStatus=GuConfigWrite(owner,EfiPciIoWidthUint16,owner->pcie_cap+8,&Ctl);
  /* No access of any kind to this function for at least 100 ms. */
  GuStall(owner,100000);
  if(owner->fault || EFI_ERROR(ResetStatus))return EFI_DEVICE_ERROR;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    EFI_STATUS Status=RawReadFor(owner,EfiPciIoWidthUint32,0,&Identity);
    if(!EFI_ERROR(Status) && GuIsSupportedPciId(Identity))return EFI_SUCCESS;
    if(!EFI_ERROR(Status) && Identity!=0xffffffffU && Identity!=0)break;
    if(Wait<1000000) {
      GuStall(owner,1000);
      if(owner->fault)return EFI_DEVICE_ERROR;
    }
  }
  owner->fault=TRUE;return EFI_DEVICE_ERROR;
}
EFI_STATUS GuRestorePci(GU_PLATFORM *owner) {
  if(!owner)return EFI_INVALID_PARAMETER;
  if(owner!=GuPlatformActive){owner->fault=TRUE;return EFI_ACCESS_DENIED;}
  UINT16 Command=0,BytePair=0;UINT32 Value=0;UINT8 Line;INTN Offset;
  if(!owner->saved || owner->flrs!=1 || owner->fault)return EFI_NOT_READY;
  if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,4,&Command)) || Command==0xffff || (Command&4))return EFI_DEVICE_ERROR;
  Command&=(UINT16)~7U;
  if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint16,4,&Command)) ||
     EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,4,&BytePair)) || (BytePair&7))return EFI_DEVICE_ERROR;
  /* Decode disabled; restore high BAR dwords before their low dwords. */
  for(Offset=0x24;Offset>=0x10;Offset-=4) {
    Value=owner->saved_config[Offset/4];
    if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint32,(UINT32)Offset,&Value)))return EFI_DEVICE_ERROR;
  }
  Value=owner->saved_config[0x30/4]&~1U; /* Never enable expansion ROM decoding. */
  if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint32,0x30,&Value)))return EFI_DEVICE_ERROR;
  BytePair=(UINT16)owner->saved_config[3];
  if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint16,0x0c,&BytePair)))return EFI_DEVICE_ERROR;
  Line=(UINT8)owner->saved_config[0x3c/4];
  if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint8,0x3c,&Line)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint16,owner->pcie_cap+8,&owner->saved_devctl)))return EFI_DEVICE_ERROR;
  if(owner->pcie_version>=2 && EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint16,
     owner->pcie_cap+0x28,&owner->saved_devctl2)))return EFI_DEVICE_ERROR;
  Command=(UINT16)owner->saved_config[1]&~4U;
  if(EFI_ERROR(GuConfigWrite(owner,EfiPciIoWidthUint16,4,&Command)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint16,4,&BytePair)) || BytePair!=Command)return EFI_DEVICE_ERROR;
  for(Offset=0x10;Offset<=0x24;Offset+=4) {
    if(EFI_ERROR(GuConfigReadFor(owner,EfiPciIoWidthUint32,(UINT32)Offset,&Value)) ||
       Value!=owner->saved_config[Offset/4])return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
