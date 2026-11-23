/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_discovery.h"
#include "gpu_bar.h"
#include "gpu_execution_state.h"
#include <string.h>
#ifndef GU_DISCOVERY_HOST_TEST
#include "port.h"
#include "checks.h"
#ifdef GU_ACTIVE_SINGLE_TARGET
#include "msi_release.h"
#endif
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) && GU_PREFLIGHT_ONLY != 1
#error Preparation-only active EFI must also prohibit GPU writes, DMA and FLR
#endif
#if defined(GU_ACTIVE_SINGLE_TARGET) && GU_PREFLIGHT_ONLY != 0 && !defined(GU_SYNTHETIC_COMPILE_ONLY)
#error One-target active execution requires explicit transaction mode
#endif
#if defined(GU_ACTIVE_SINGLE_TARGET) && defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
#error Active and preparation-only profiles are mutually exclusive
#endif
#endif
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
#if defined(GU_RUNTIME_SELECTION)
/* The operator supplies the targets at boot ("GuTargets" variable), so the
 * count is not a build-time constant. The runtime admission below is the same
 * one the compiled list is subject to: a request is not a permission. */
#include "gpu_selection.h"
#else
#include "gpu-selection.h" /* Generated operator-supplied PCI addresses. */
#if GU_GPU_SELECTION_COUNT < 1 || GU_GPU_SELECTION_COUNT > GU_INVENTORY_MAX_HANDLES
#error Invalid active target count
#endif
#if defined(GU_ACTIVE_SINGLE_TARGET) && GU_GPU_SELECTION_COUNT != 1 && \
    !defined(GU_SYNTHETIC_COMPILE_ONLY) && !defined(GU_ACTIVE_UNVERIFIED_ACTUATION)
#error One-target active execution requires exactly one PCI address
#endif
static const GU_GPU_LOCATION ActiveRequested[GU_GPU_SELECTION_COUNT] =
    GU_GPU_SELECTION_LOCATIONS;
#endif
#endif

/* Admission is one decision with two provenances (hardware waiver or the
 * hosted simulator). Keep the fallback so host-test translation units that do
 * not include port.h compile the same refusal path. */
#ifndef GU_ACTUATION_ADMITTED
#define GU_ACTUATION_ADMITTED 0
#endif
#ifndef GU_ACTUATION_SIMULATED
#define GU_ACTUATION_SIMULATED 0
#endif

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
/* Caller-owned view of a runtime-sized record set. The selected order is
 * separate from enumeration order and never rewritten for full-set reads. */
typedef struct {
  struct {size_t count,capacity;GU_GPU_RECORD *nvidia;} inventory;
  size_t selected_count;
  size_t *selected_indices;
} GU_GPU_PREPARED_VIEW;

typedef struct {
  VOID *allocation;
  size_t capacity;
  GU_GPU_RECORD *records,*live;
  size_t *indices;
  GU_GPU_REFRESHED_TARGET *scanned;
  GU_GPU_EXEC_OBSERVATION *observed;
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
  GU_GPU_EXEC_TARGET *targets;
  size_t *order;
#endif
} GU_GPU_DISCOVERY_STORAGE;

/* One pool block, partitioned at multiples of a union size so every view
 * is suitably aligned. Bound total pool use by firmware handle count. */
typedef union {
  GU_GPU_RECORD record;
  GU_GPU_REFRESHED_TARGET snapshot;
  GU_GPU_EXEC_OBSERVATION observation;
  GU_GPU_EXEC_TARGET target;
  size_t index;
} GU_DISCOVERY_ALIGNMENT;
static VOID *UnreleasedDiscoveryAllocation;
static EFI_HANDLE *UnreleasedPciHandles;

static EFI_STATUS AllocateDiscoveryStorage(EFI_BOOT_SERVICES *bs,size_t capacity,
                                           GU_GPU_DISCOVERY_STORAGE *storage) {
  if(!bs||!bs->AllocatePool||!bs->FreePool||!storage||storage->allocation||
     !capacity||capacity>GU_INVENTORY_MAX_HANDLES)return EFI_INVALID_PARAMETER;
  if(UnreleasedDiscoveryAllocation)return EFI_ALREADY_STARTED;
  const size_t types[]={
    sizeof(GU_GPU_RECORD),sizeof(GU_GPU_RECORD),sizeof(size_t),
    sizeof(GU_GPU_REFRESHED_TARGET),sizeof(GU_GPU_EXEC_OBSERVATION),
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
    sizeof(GU_GPU_EXEC_TARGET),sizeof(size_t),
#endif
  };
  size_t offsets[sizeof(types)/sizeof(types[0])],total=0;
  const size_t quantum=sizeof(GU_DISCOVERY_ALIGNMENT);
  for(size_t i=0;i<sizeof(types)/sizeof(types[0]);i++){
    if(capacity>(SIZE_MAX-quantum+1)/types[i])return EFI_OUT_OF_RESOURCES;
    size_t bytes=((capacity*types[i]+quantum-1)/quantum)*quantum;
    if(bytes>SIZE_MAX-total)return EFI_OUT_OF_RESOURCES;
    offsets[i]=total;total+=bytes;
  }
  VOID *allocation=NULL;
  EFI_STATUS s=bs->AllocatePool(EfiLoaderData,total,&allocation);
  if(s!=EFI_SUCCESS) {
    if(allocation) {
      /* An error does not establish ownership of the returned address.
       * Do not initialize, publish or release this unverified buffer. */
      UnreleasedDiscoveryAllocation=allocation;
      GuLog("DISCOVERY_STORAGE_ALLOC_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
      return EFI_DEVICE_ERROR;
    }
    return s;
  }
  if(!allocation)return EFI_OUT_OF_RESOURCES;
  memset(allocation,0,total);
  unsigned char *base=(unsigned char *)allocation;
  storage->allocation=allocation;storage->capacity=capacity;
  storage->records=(GU_GPU_RECORD *)(base+offsets[0]);
  storage->live=(GU_GPU_RECORD *)(base+offsets[1]);
  storage->indices=(size_t *)(base+offsets[2]);
  storage->scanned=(GU_GPU_REFRESHED_TARGET *)(base+offsets[3]);
  storage->observed=(GU_GPU_EXEC_OBSERVATION *)(base+offsets[4]);
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
  storage->targets=(GU_GPU_EXEC_TARGET *)(base+offsets[5]);
  storage->order=(size_t *)(base+offsets[6]);
#endif
  return EFI_SUCCESS;
}

static EFI_STATUS FreeDiscoveryStorage(EFI_BOOT_SERVICES *bs,
                                       GU_GPU_DISCOVERY_STORAGE *storage) {
  if(!storage->allocation)return EFI_SUCCESS;
  EFI_STATUS s=bs->FreePool(storage->allocation);
  if(s!=EFI_SUCCESS) {
    /* Never retry an uncertain release or lose its reference. */
    UnreleasedDiscoveryAllocation=storage->allocation;
    GuLog("DISCOVERY_STORAGE_FREE_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
  }
  memset(storage,0,sizeof(*storage));
  return s;
}

static GU_GPU_PREPARED_VIEW DiscoveryView(const GU_GPU_DISCOVERY_STORAGE *storage) {
  GU_GPU_PREPARED_VIEW view={0};
  view.inventory.capacity=storage->capacity;
  view.inventory.nvidia=storage->records;
  view.selected_indices=storage->indices;
  return view;
}

#ifdef GU_ACTIVE_SINGLE_TARGET
static GU_GPU_PREPARED_VIEW ActiveInventory;
static GU_GPU_DISCOVERY_STORAGE ActiveDiscovery;
static int ActiveInventoryValid;
/* Ordered transaction ownership is independent of admission. Compile-only
 * images can validate a longer selection; neither EFI entrypoint runs. */
static GU_GPU_EXEC_SEQUENCE ActiveSequence;
static int ActiveSequenceValid;
#endif


typedef struct {
  EFI_BOOT_SERVICES *services;
  EFI_HANDLE *handles;
} GU_PCI_READER_CONTEXT;

static int ReadPciIdentity(void *opaque, size_t index, GU_GPU_RECORD *record) {
  GU_PCI_READER_CONTEXT *ctx = (GU_PCI_READER_CONTEXT *)opaque;
  EFI_PCI_IO_PROTOCOL *io = NULL;
  EFI_STATUS s;
  UINT32 id = 0, pci_class = 0, subsystem = 0;
  UINTN segment, bus, device, function;
  if (!ctx || !record || !ctx->handles || !ctx->services) return -1;
  s = ctx->services->HandleProtocol(ctx->handles[index], &PciGuid, (VOID **)&io);
  if (EFI_ERROR(s) || !io || !io->Pci.Read) return -1;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 0, 1, &id);
  if (EFI_ERROR(s)) return -1;
  record->handle = ctx->handles[index];
  record->pci = io;
  record->pci_id = id;
  if ((id & 0xffffU) != 0x10deU) return 0;
  if (!io->GetLocation) return -1;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 8, 1, &pci_class);
  if (EFI_ERROR(s)) return -1;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 0x2c, 1, &subsystem);
  if (EFI_ERROR(s)) return -1;
  s = io->GetLocation(io, &segment, &bus, &device, &function);
  if (EFI_ERROR(s) || segment > 0xffffU || bus > 255U ||
      device > 31U || function > 7U) return -1;
  record->class_code = (UINT16)(pci_class >> 16);
  record->subsystem_id = subsystem;
  record->segment = (UINT32)segment;
  record->bus = (UINT32)bus;
  record->device = (UINT32)device;
  record->function = (UINT32)function;
  return 0;
}

/* A successful LocateHandleBuffer transfers one pool buffer to the caller.
 * If its release is uncertain, retain the address and refuse later census
 * operations. Never retry FreePool or read a buffer after its release attempt. */
static EFI_STATUS ReleasePciHandles(EFI_BOOT_SERVICES *bs,EFI_HANDLE **handles) {
  if(!handles||!*handles)return EFI_SUCCESS;
  EFI_HANDLE *buffer=*handles;
  *handles=NULL;
  EFI_STATUS s=bs->FreePool(buffer);
  if(s!=EFI_SUCCESS) {
    UnreleasedPciHandles=buffer;
    GuLog("PCI_HANDLE_BUFFER_FREE_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
  }
  return s;
}

/* All inventory buffers below belong to the caller. */
static EFI_STATUS LocatePciHandles(EFI_SYSTEM_TABLE *table,EFI_HANDLE **handles,UINTN *count){
  if(!table||!table->BootServices||!table->BootServices->LocateHandleBuffer||
     !table->BootServices->HandleProtocol||!table->BootServices->FreePool)
    return EFI_INVALID_PARAMETER;
  *handles=NULL;*count=0;
  if(UnreleasedPciHandles)return EFI_ALREADY_STARTED;
  EFI_STATUS s=table->BootServices->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,count,handles);
  if(s!=EFI_SUCCESS) {
    /* Do not inspect or release an unverified output on service failure. */
    if(*handles) {
      UnreleasedPciHandles=*handles;
      GuLog("PCI_HANDLE_BUFFER_LOCATE_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
    }
    *handles=NULL;*count=0;
    return UnreleasedPciHandles?EFI_DEVICE_ERROR:EFI_UNSUPPORTED;
  }
  if(!*handles||!*count||*count>GU_INVENTORY_MAX_HANDLES){
    (VOID)ReleasePciHandles(table->BootServices,handles);
    *count=0;
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS CollectLocatedGpuRecords(EFI_SYSTEM_TABLE *table,EFI_HANDLE *handles,
                                            UINTN handle_count,GU_GPU_RECORD *records,
                                            size_t capacity,size_t *count) {
  GU_PCI_READER_CONTEXT context={table->BootServices,handles};
  int result=GuCollectGpuRecords((size_t)handle_count,ReadPciIdentity,&context,
                                 records,capacity,count);
  EFI_STATUS released=ReleasePciHandles(table->BootServices,&handles);
  if(result!=GU_INVENTORY_OK||released!=EFI_SUCCESS) {
    *count=0;
    GuLog("ACTIVE_PREPARE=REFUSED INVENTORY_CODE=%d FREE=%#llx\n",
          result,(unsigned long long)released);
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS ReadGpuRecords(EFI_SYSTEM_TABLE *table,GU_GPU_RECORD *records,
                                  size_t capacity,size_t *count) {
  EFI_HANDLE *handles=NULL;UINTN handle_count=0;
  *count=0;
  EFI_STATUS s=LocatePciHandles(table,&handles,&handle_count);
  if(s!=EFI_SUCCESS)return s;
  return CollectLocatedGpuRecords(table,handles,handle_count,records,capacity,count);
}

#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
#if defined(GU_RUNTIME_SELECTION)
/* The variable is read once per boot and kept: the list pointer escapes this
 * call into the prepared view. */
static GU_SELECTION g_runtime_selection;
static BOOLEAN g_runtime_selection_loaded;
EFI_STATUS GuRuntimeSelectionSelectAll(VOID) {
  if(g_runtime_selection_loaded)return EFI_ALREADY_STARTED;
  memset(&g_runtime_selection,0,sizeof(g_runtime_selection));
  g_runtime_selection.all=TRUE;
  g_runtime_selection_loaded=TRUE;
  GuLog("ACTIVE_SELECTION=POLICY_ALL SOURCE=LIVE_BOOT_INVENTORY NVRAM=UNCHANGED\n");
  return EFI_SUCCESS;
}
static EFI_STATUS ResolveRuntimeSelection(EFI_SYSTEM_TABLE *table,
                                          const GU_GPU_LOCATION **requested,
                                          size_t *requested_count,BOOLEAN *all) {
  if(!requested||!requested_count||!all)return EFI_INVALID_PARAMETER;
  if(!g_runtime_selection_loaded) {
    EFI_STATUS s=GuSelectionRead(table,&g_runtime_selection);
    if(s==EFI_NOT_FOUND) {
      memset(&g_runtime_selection,0,sizeof(g_runtime_selection));
      s=EFI_SUCCESS;
    }
    if(s!=EFI_SUCCESS) {
      GuLog("ACTIVE_SELECTION=REFUSED REASON=VARIABLE_STATUS STATUS=%#llx\n",
            (unsigned long long)s);
      return s;
    }
    g_runtime_selection_loaded=TRUE;
    if(g_runtime_selection.all)
      GuLog("ACTIVE_SELECTION=POLICY_ALL SOURCE=VARIABLE\n");
    else if(g_runtime_selection.count)
      GuLog("ACTIVE_SELECTION=REQUESTED COUNT=%zu SOURCE=VARIABLE\n",
            g_runtime_selection.count);
    else
      GuLog("ACTIVE_SELECTION=UNSET SOURCE=NONE AUTO_SINGLE_ONLY=1\n");
  }
  *requested=g_runtime_selection.count?g_runtime_selection.list:NULL;
  *requested_count=g_runtime_selection.count;
  *all=g_runtime_selection.all;
  return EFI_SUCCESS;
}
#endif
#endif

static EFI_STATUS SelectPreparedRecords(GU_GPU_PREPARED_VIEW *prepared,
                                         const GU_GPU_LOCATION *requested,
                                         size_t requested_count,
                                         BOOLEAN all) {
  size_t count=prepared->inventory.count;
  prepared->selected_count=0;
  GuLog("ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=%zu REQUESTED=%zu READ_ONLY=1\n",
        count,requested_count);
  for(size_t i=0;i<count;i++){
    const GU_GPU_RECORD *p=&prepared->inventory.nvidia[i];
    GuLog("ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x LEGACY_UNLOCK_DEVICE_ID=%u\n",
          i,(unsigned)p->segment,(unsigned)p->bus,(unsigned)p->device,
          (unsigned)p->function,(unsigned)(p->pci_id&65535U),(unsigned)(p->pci_id>>16),
          (unsigned)(p->subsystem_id&65535U),(unsigned)(p->subsystem_id>>16),
          (unsigned)p->class_code,(unsigned)GuIsSupportedPciId(p->pci_id));
  }
  if(requested_count){
    if(requested_count>count){
      GuLog("ACTIVE_PREPARE=REFUSED REASON=REQUESTED_EXCEEDS_INVENTORY REQUESTED=%zu NVIDIA_COUNT=%zu\n",
            requested_count,count);
      return EFI_UNSUPPORTED;
    }
    int result=GuFindGpuRecordSequence(prepared->inventory.nvidia,count,requested,
                                       requested_count,prepared->selected_indices);
    if(result!=GU_INVENTORY_OK){
      GuLog("ACTIVE_PREPARE=REFUSED SELECTION_CODE=%d REQUESTED=%zu\n",result,requested_count);
      return EFI_UNSUPPORTED;
    }
    prepared->selected_count=requested_count;
  }else if(all){
    /* "ALL" is an explicit operator policy, not a default: every supported
     * NVIDIA function present at boot, in enumeration order. */
    for(size_t i=0;i<count;i++)
      if(GuIsSupportedPciId(prepared->inventory.nvidia[i].pci_id))
        prepared->selected_indices[prepared->selected_count++]=i;
    if(!prepared->selected_count){
      GuLog("ACTIVE_PREPARE=REFUSED REASON=NO_SUPPORTED_CARD NVIDIA_COUNT=%zu REQUESTED=ALL\n",
            count);
      return EFI_UNSUPPORTED;
    }
    GuLog("ACTIVE_PREPARE=POLICY_ALL SUPPORTED=%zu NVIDIA_COUNT=%zu\n",
          prepared->selected_count,count);
  }else if(count==1){
    prepared->selected_count=1;prepared->selected_indices[0]=0;
  }else if(!count){
    /* Nothing was discovered. This helper answers "which records were named",
     * not "may anything be prepared", so an empty machine stays a success
     * with an empty selection and the reason is printed for the caller. */
    GuLog("ACTIVE_PREPARE=NO_SELECTION REASON=NO_NVIDIA_CARD NVIDIA_COUNT=0\n");
  }else{
    /* Two or more NVIDIA display functions and no explicit selection. Picking
     * one here would be this code choosing a target on a multi-card machine,
     * so nothing is selected and the log says which situation this is: the
     * entrypoint refuses an empty selection, and until this line existed the
     * only trace was a COMPLETE with SELECTED=0. */
    GuLog("ACTIVE_PREPARE=NO_SELECTION REASON=SELECTION_AMBIGUOUS NVIDIA_COUNT=%zu SELECTED=0\n",
          count);
  }
  /* A selected card this image was not built for is named here; the refusal
   * itself belongs to the entrypoint, which is the only place that decides
   * whether a card may be prepared. */
  for(size_t i=0;i<prepared->selected_count;i++){
    const GU_GPU_RECORD *p=&prepared->inventory.nvidia[prepared->selected_indices[i]];
    if(!GuIsSupportedPciId(p->pci_id)){
      GuLog("ACTIVE_PREPARE=SELECTED_UNSUPPORTED_CARD BDF=%04x:%02x:%02x.%u ID=%04x:%04x\n",
            (unsigned)p->segment,(unsigned)p->bus,(unsigned)p->device,
            (unsigned)p->function,(unsigned)(p->pci_id&65535U),(unsigned)(p->pci_id>>16));
    }
  }
  for(size_t i=0;i<prepared->selected_count;i++){
    const GU_GPU_RECORD *p=&prepared->inventory.nvidia[prepared->selected_indices[i]];
    GuLog("ACTIVE_PREPARED_STEP[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x LEGACY_UNLOCK_DEVICE_ID=%u READ_ONLY=1\n",
          i,(unsigned)p->segment,(unsigned)p->bus,(unsigned)p->device,
          (unsigned)p->function,(unsigned)(p->pci_id&65535U),(unsigned)(p->pci_id>>16),
          (unsigned)(p->subsystem_id&65535U),(unsigned)(p->subsystem_id>>16),
          (unsigned)GuIsSupportedPciId(p->pci_id));
  }
  GuLog("ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0\n",
        prepared->selected_count,count);
  return EFI_SUCCESS;
}

/* Compatibility adapter for existing bounded callers. The actual boot
 * entrypoint uses the same collection and selection logic with pool views. */
EFI_STATUS GuPrepareGpuSequence(EFI_SYSTEM_TABLE *table,
                                const GU_GPU_LOCATION *requested,
                                size_t requested_count,
                                GU_GPU_PREPARED_SEQUENCE *prepared) {
  if(!prepared)return EFI_INVALID_PARAMETER;
  memset(prepared,0,sizeof(*prepared));
  if(requested_count>GU_INVENTORY_MAX_NVIDIA||
     ((requested_count!=0)!=(requested!=NULL)))return EFI_INVALID_PARAMETER;
  GU_GPU_PREPARED_VIEW view={{0,GU_INVENTORY_MAX_NVIDIA,prepared->inventory.nvidia},
                              0,prepared->selected_indices};
  EFI_STATUS s=ReadGpuRecords(table,view.inventory.nvidia,view.inventory.capacity,
                             &view.inventory.count);
  if(s==EFI_SUCCESS)s=SelectPreparedRecords(&view,requested,requested_count,FALSE);
  if(s!=EFI_SUCCESS){memset(prepared,0,sizeof(*prepared));return s;}
  prepared->inventory.count=view.inventory.count;
  prepared->selected_count=view.selected_count;
  return EFI_SUCCESS;
}

/* Revalidate the exact saved record with no write or ownership change. */
static EFI_STATUS RefreshGpuRecord(EFI_SYSTEM_TABLE *table,const GU_GPU_RECORD *expected,
                                    UINT32 config[64],EFI_PCI_IO_PROTOCOL **pci) {
  EFI_PCI_IO_PROTOCOL *current=NULL;
  UINT32 snapshot[64];
  UINTN segment,bus,device,function;
  if(pci)*pci=NULL;
  if(config)memset(config,0,sizeof(snapshot));
  if(!table||!table->BootServices||!table->BootServices->HandleProtocol||
     !expected||!config||!pci)return EFI_INVALID_PARAMETER;
  if(!expected->handle||!expected->pci||expected->segment>0xffffU||
     expected->bus>255U||expected->device>31U||expected->function>7U)
    return EFI_UNSUPPORTED;
  EFI_STATUS s=table->BootServices->HandleProtocol((EFI_HANDLE)expected->handle,
                                                   &PciGuid,(VOID **)&current);
  if(s!=EFI_SUCCESS||!current||current!=expected->pci||
     !current->GetLocation||!current->Pci.Read)return EFI_UNSUPPORTED;
  s=current->GetLocation(current,&segment,&bus,&device,&function);
  if(s!=EFI_SUCCESS||segment!=expected->segment||bus!=expected->bus||
     device!=expected->device||function!=expected->function)return EFI_UNSUPPORTED;
  s=current->Pci.Read(current,EfiPciIoWidthUint32,0,64,snapshot);
  if(s!=EFI_SUCCESS||snapshot[0]!=expected->pci_id||
     (UINT16)(snapshot[2]>>16)!=expected->class_code||
     snapshot[0x2c/4]!=expected->subsystem_id)return EFI_UNSUPPORTED;
  memcpy(config,snapshot,sizeof(snapshot));*pci=current;
  return EFI_SUCCESS;
}

EFI_STATUS GuRefreshPreparedGpu(EFI_SYSTEM_TABLE *table,
                                const GU_GPU_PREPARED_SEQUENCE *prepared,
                                size_t step, UINT32 config[64],
                                EFI_PCI_IO_PROTOCOL **pci) {
  if(!prepared||!prepared->inventory.count||
     prepared->inventory.count>GU_INVENTORY_MAX_NVIDIA||
     prepared->selected_count>prepared->inventory.count||
     step>=prepared->selected_count||prepared->selected_indices[step]>=prepared->inventory.count){
    if(pci)*pci=NULL;
    if(config)memset(config,0,64*sizeof(*config));
    return EFI_INVALID_PARAMETER;
  }
  return RefreshGpuRecord(table,&prepared->inventory.nvidia[prepared->selected_indices[step]],config,pci);
}

/* Check all saved identities and the complete live set. The live scratch
 * buffer has no ownership role and is never published as a saved record. */
static EFI_STATUS VerifyLiveGpuRecords(EFI_SYSTEM_TABLE *table,const GU_GPU_PREPARED_VIEW *prepared,
                                       GU_GPU_RECORD *live,size_t capacity) {
  if(!table||!table->BootServices||!prepared||!live||
     !prepared->inventory.nvidia||!prepared->selected_indices||
     !prepared->inventory.count||prepared->inventory.count>prepared->inventory.capacity||
     prepared->inventory.capacity>GU_INVENTORY_MAX_HANDLES||
     prepared->inventory.count>capacity||capacity>GU_INVENTORY_MAX_HANDLES||
     !prepared->selected_count||prepared->selected_count>prepared->inventory.count)
    return EFI_INVALID_PARAMETER;
  size_t count=prepared->inventory.count;
  const GU_GPU_RECORD *records=prepared->inventory.nvidia;
  for(size_t i=0;i<count;i++){
    const GU_GPU_RECORD *r=&records[i];
    if(!r->handle||!r->pci||(r->pci_id&0xffffU)!=0x10deU||
       (r->class_code&0xff00U)!=0x0300U||r->segment>0xffffU||
       r->bus>255U||r->device>31U||r->function>7U)return EFI_INVALID_PARAMETER;
    for(size_t j=0;j<i;j++){
      const GU_GPU_RECORD *p=&records[j];
      if(r->handle==p->handle||r->pci==p->pci||
         (r->segment==p->segment&&r->bus==p->bus&&
          r->device==p->device&&r->function==p->function))return EFI_INVALID_PARAMETER;
    }
  }
  for(size_t i=0;i<prepared->selected_count;i++){
    if(prepared->selected_indices[i]>=count)return EFI_INVALID_PARAMETER;
    for(size_t j=0;j<i;j++)
      if(prepared->selected_indices[i]==prepared->selected_indices[j])return EFI_INVALID_PARAMETER;
  }
  size_t live_count=0;
  EFI_STATUS s=ReadGpuRecords(table,live,capacity,&live_count);
  if(s!=EFI_SUCCESS||live_count!=count)return EFI_UNSUPPORTED;
  for(size_t i=0;i<count;i++){
    const GU_GPU_RECORD *expected=&records[i];
    size_t matches=0;
    for(size_t j=0;j<live_count;j++){
      const GU_GPU_RECORD *r=&live[j];
      if(r->segment!=expected->segment||r->bus!=expected->bus||
         r->device!=expected->device||r->function!=expected->function)continue;
      if(r->handle!=expected->handle||r->pci!=expected->pci||
         r->pci_id!=expected->pci_id||r->subsystem_id!=expected->subsystem_id||
         r->class_code!=expected->class_code)return EFI_UNSUPPORTED;
      matches++;
    }
    if(matches!=1)return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

/* Snapshot all records in inventory order, then confirm all snapshots.
 * The caller may publish their contents only after both passes succeed. */
static EFI_STATUS RefreshGpuRecords(EFI_SYSTEM_TABLE *table,const GU_GPU_PREPARED_VIEW *prepared,
                                    GU_GPU_RECORD *live,GU_GPU_REFRESHED_TARGET *scanned,
                                    size_t capacity) {
  if(!scanned)return EFI_INVALID_PARAMETER;
  EFI_STATUS s=VerifyLiveGpuRecords(table,prepared,live,capacity);
  if(s!=EFI_SUCCESS)return s;
  for(size_t i=0;i<prepared->inventory.count;i++){
    s=RefreshGpuRecord(table,&prepared->inventory.nvidia[i],scanned[i].config,&scanned[i].pci);
    if(s!=EFI_SUCCESS)return s;
  }
  for(size_t i=0;i<prepared->inventory.count;i++){
    UINT32 confirm[64];EFI_PCI_IO_PROTOCOL *io=NULL;
    s=RefreshGpuRecord(table,&prepared->inventory.nvidia[i],confirm,&io);
    if(s!=EFI_SUCCESS||io!=scanned[i].pci||
       memcmp(confirm,scanned[i].config,sizeof(confirm))!=0)return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

/* Existing bounded API: its caller-owned output is blank on any failure.
 * Active discovery does not allocate these fixed compatibility buffers. */
EFI_STATUS GuRefreshPreparedSequence(EFI_SYSTEM_TABLE *table,
                                     const GU_GPU_PREPARED_SEQUENCE *prepared,
                                     GU_GPU_REFRESHED_SEQUENCE *refreshed) {
  if(!refreshed)return EFI_INVALID_PARAMETER;
  memset(refreshed,0,sizeof(*refreshed));
  if(!prepared)return EFI_INVALID_PARAMETER;
  GU_GPU_PREPARED_VIEW view={{prepared->inventory.count,GU_INVENTORY_MAX_NVIDIA,
                              (GU_GPU_RECORD *)prepared->inventory.nvidia},
                             prepared->selected_count,(size_t *)prepared->selected_indices};
  GU_GPU_RECORD live[GU_INVENTORY_MAX_NVIDIA];
  GU_GPU_REFRESHED_TARGET scanned[GU_INVENTORY_MAX_NVIDIA];
  EFI_STATUS s=RefreshGpuRecords(table,&view,live,scanned,GU_INVENTORY_MAX_NVIDIA);
  if(s!=EFI_SUCCESS)return s;
  for(size_t i=0;i<view.selected_count;i++)
    refreshed->targets[i]=scanned[view.selected_indices[i]];
  refreshed->count=view.selected_count;
  return EFI_SUCCESS;
}

/* Verify the live PCI set and its expected per-device state. */
#ifdef GU_ACTIVE_SINGLE_TARGET
/* One source of target and step identity: the ordered execution record.
 * Check the retained backing and current PCI binding before trusting it. */
static int ResolveSelectedBinding(size_t *step,size_t *target) {
  if(UnreleasedPciHandles || !ActiveInventoryValid || !ActiveSequenceValid ||
     !ActiveDiscovery.allocation || !ActiveDiscovery.targets ||
     !ActiveDiscovery.order || !ActiveDiscovery.observed ||
     ActiveSequence.targets!=ActiveDiscovery.targets ||
     ActiveSequence.order!=ActiveDiscovery.order ||
     ActiveInventory.inventory.nvidia!=ActiveDiscovery.records ||
     ActiveInventory.selected_indices!=ActiveDiscovery.indices ||
     ActiveInventory.inventory.capacity!=ActiveDiscovery.capacity ||
     !ActiveInventory.inventory.count ||
     ActiveInventory.inventory.count>ActiveDiscovery.capacity ||
     ActiveDiscovery.capacity>GU_INVENTORY_MAX_HANDLES ||
     ActiveSequence.count!=ActiveInventory.inventory.count ||
     ActiveSequence.count!=ActiveSequence.capacity ||
     GuGpuExecResolveBound(&ActiveSequence,Gu.handle,Gu.pci,step,target)
       !=GU_GPU_EXEC_OK)
    return 0;
  const GU_GPU_RECORD *r=&ActiveInventory.inventory.nvidia[*target];
  return r->handle==Gu.handle && r->pci==Gu.pci;
}
#endif

int GuIsPreparedPeerHandle(EFI_HANDLE handle) {
#ifdef GU_ACTIVE_SINGLE_TARGET
  size_t step,target;
  if(!ActiveInventoryValid||!handle)return 0;
  /* An invalid binding cannot make a peer eligible for removal. */
  if(!ResolveSelectedBinding(&step,&target))return 1;
  for(size_t i=0;i<ActiveInventory.inventory.count;i++)
    if(i!=target&&ActiveInventory.inventory.nvidia[i].handle==handle)
      return 1;
#else
  (VOID)handle;
#endif
  return 0;
}

EFI_STATUS GuVerifyActivePeers(EFI_SYSTEM_TABLE *table) {
#ifndef GU_ACTIVE_SINGLE_TARGET
  (VOID)table;
  return EFI_SUCCESS;
#else
  size_t i,step,target;
  EFI_STATUS s;
  if(!table || !table->BootServices || Gu.bs!=table->BootServices ||
     !ResolveSelectedBinding(&step,&target))
    return EFI_UNSUPPORTED;
  const GU_GPU_EXEC_TARGET *bound=&ActiveSequence.targets[target];
  if(bound->phase!=GU_GPU_EXEC_PENDING &&
     (bound->owners.platform!=&Gu ||
      bound->owners.release_plan!=Gu.release_state ||
#ifdef GU_STOP_RELEASE_ONLY_BENCH
      !Gu.stop_release_ready ||
      bound->owners.firmware_assets!=&Gu.stop_release_ready))
#else
      !Gu.asset_entries_reserved ||
      bound->owners.firmware_assets!=Gu.asset_entries))
#endif
    return EFI_UNSUPPORTED;
  s=VerifyLiveGpuRecords(table,&ActiveInventory,ActiveDiscovery.live,ActiveDiscovery.capacity);
  if(s!=EFI_SUCCESS)return EFI_UNSUPPORTED;
  for(unsigned pass=0;pass<2;pass++) {
    for(i=0;i<ActiveInventory.inventory.count;i++) {
      UINT32 now[64];
      EFI_PCI_IO_PROTOCOL *io=NULL;
      s=RefreshGpuRecord(table,&ActiveInventory.inventory.nvidia[i],now,&io);
      if(s!=EFI_SUCCESS || io!=ActiveInventory.inventory.nvidia[i].pci)return EFI_UNSUPPORTED;
      /* The execution record is the single source of expected PCI state.
       * Pending peers retain their initial state. Completed cards retain
       * the state recorded by the successful Finish transition. */
      if((i!=target || bound->phase==GU_GPU_EXEC_COMPLETE) &&
         memcmp(now,ActiveSequence.targets[i].expected_config,sizeof(now))!=0) {
        GuLog("ACTIVE_PEER=DRIFT INDEX=%zu PASS=%u REFUSED=1\n",i,pass);
        return EFI_UNSUPPORTED;
      }
    }
  }
  return EFI_SUCCESS;
#endif
}

/* Read the firmware-assigned BAR0 window. The config-space dwords only
 * cross-check it: a 64-bit BAR may be placed above 4 GiB and a Resizable BAR
 * may be larger than the 16 MiB this code addresses, so neither the low dword
 * nor a fixed 16 MiB size is an authority. */
static EFI_STATUS ReadBarWindow(EFI_PCI_IO_PROTOCOL *pci,UINT64 *Base,UINT64 *Size) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;
  UINT64 Supports=0;
  if(!pci||!pci->GetBarAttributes||!Base||!Size)return EFI_INVALID_PARAMETER;
  *Base=0;*Size=0;
  EFI_STATUS s=pci->GetBarAttributes(pci,0,&Supports,(VOID **)&d);
  UINT64 base=0,size=0;
  if(s==EFI_SUCCESS) {
    if(!d||d->Desc!=0x8a||d->Len!=43||d->ResType||d->AddrTranslationOffset||
       !d->AddrLen||(d->AddrLen&(d->AddrLen-1U))||d->AddrLen<GU_BAR_MIN_BYTES||
       (d->AddrRangeMin&(d->AddrLen-1U))||
       d->AddrRangeMax!=d->AddrRangeMin+d->AddrLen-1U||
       d->AddrRangeMin>~(UINT64)0-d->AddrLen) s=EFI_UNSUPPORTED;
    else { base=d->AddrRangeMin;size=d->AddrLen; }
  }
  /* The descriptor is firmware-allocated. A failed release must not be
   * reported as a hardware fault; it is a leak, and the caller decides. */
  if(d&&Gu.bs&&Gu.bs->FreePool)(VOID)Gu.bs->FreePool(d);
  if(s!=EFI_SUCCESS)return s;
  *Base=base;*Size=size;
  return EFI_SUCCESS;
}

/* Initialize the newly bound platform from the exact checked PCI snapshot.
 * Reuse this for the initial and every subsequent context. */
static EFI_STATUS ConfigureBoundPlatform(EFI_HANDLE image,const GU_GPU_RECORD *selected,
                                         const UINT32 config[64],const GU_CAPS *caps) {
  UINT64 Base=0,Size=0;
  EFI_STATUS s=ReadBarWindow(Gu.pci,&Base,&Size);
  if(s!=EFI_SUCCESS) {
    GuLog("BAR0 window unavailable: GetBarAttributes status=%#llx\n",
          (unsigned long long)s);
    return s;
  }
  if(GuBarBaseFromConfig(config)!=Base) {
    GuLog("BAR0 config/attribute mismatch: config=%#llx attributes=%#llx\n",
          (unsigned long long)GuBarBaseFromConfig(config),(unsigned long long)Base);
    return EFI_UNSUPPORTED;
  }
  Gu.image=image;
  Gu.discovery_identity=*selected;
  Gu.handle=(EFI_HANDLE)selected->handle;
  Gu.device_id=(UINT16)(selected->pci_id >> 16);
  Gu.is_ampere=GuIsAmpereDeviceId(Gu.device_id);
  Gu.is_turing=GuIsTuringDeviceId(Gu.device_id);
  Gu.bar_base=Base;
  Gu.bar_size=Size;
  Gu.pm_cap=caps->pm;
  Gu.pcie_cap=caps->pcie;
  Gu.pcie_version=caps->pcie_version;
  Gu.msi_cap=caps->msi;
  Gu.msix_cap=caps->msix;
  return EFI_SUCCESS;
}

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Re-enumerate and read the COMPLETE NVIDIA inventory twice. A later
 * selected-card step must not trust a previous firmware/PCI snapshot. */
static EFI_STATUS ActiveStepObservations(EFI_SYSTEM_TABLE *table) {
  if(!ActiveInventoryValid||!ActiveSequenceValid||!table||!table->BootServices||
     !ActiveDiscovery.allocation||!ActiveDiscovery.observed)
    return EFI_INVALID_PARAMETER;
  EFI_STATUS s=RefreshGpuRecords(table,&ActiveInventory,ActiveDiscovery.live,
                                  ActiveDiscovery.scanned,ActiveDiscovery.capacity);
  if(s!=EFI_SUCCESS)return EFI_UNSUPPORTED;
  for(size_t i=0;i<ActiveInventory.inventory.count;i++){
    ActiveDiscovery.observed[i].identity=ActiveInventory.inventory.nvidia[i];
    memcpy(ActiveDiscovery.observed[i].config,ActiveDiscovery.scanned[i].config,
           sizeof(ActiveDiscovery.observed[i].config));
  }
  return EFI_SUCCESS;
}
void GuAbortSelectedGpuStep(void) {
  if(!ActiveSequenceValid)return;
  GuGpuExecAbort(&ActiveSequence);
  GuLog("SELECTED_GPU_STEP=HALTED NEXT=%zu MUTATION_AUTHORIZED=0\n",
        ActiveSequence.next_step);
}
/* Verify the backing objects, not only the caller's claim about them. */
static int SelectedOwnersMatch(const void *platform,const void *release_plan,
                               const void *firmware_assets) {
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  return platform==&Gu && Gu.release_state && release_plan==Gu.release_state &&
         Gu.stop_release_ready && firmware_assets==&Gu.stop_release_ready;
#else
  return platform==&Gu && Gu.release_state && release_plan==Gu.release_state &&
         Gu.asset_entries_reserved && firmware_assets==Gu.asset_entries;
#endif
}
EFI_STATUS GuBeginSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                  const void *platform,
                                  const void *release_plan,
                                  const void *firmware_assets) {
  if(!ActiveSequenceValid || !ActiveInventoryValid ||
     !table || !table->BootServices ||
     Gu.bs!=table->BootServices || !Gu.handle || !Gu.pci ||
#ifdef GU_STOP_RELEASE_ONLY_BENCH
     !Gu.stop_release_ready ||
#else
     !Gu.profile_verified || !Gu.prom_verified ||
#endif
     !SelectedOwnersMatch(platform,release_plan,firmware_assets) ||
     Gu.started || Gu.fault || Gu.retained || Gu.owned || Gu.armed)
    return EFI_NOT_READY;
  size_t step,target;
  if(!ResolveSelectedBinding(&step,&target) ||
     step!=ActiveSequence.next_step ||
     ActiveSequence.targets[target].phase!=GU_GPU_EXEC_PENDING) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  GU_GPU_EXEC_OBSERVATION *observed=ActiveDiscovery.observed;
  EFI_STATUS s=ActiveStepObservations(table);
  if(s!=EFI_SUCCESS ||
     GuGpuExecBeginOwned(&ActiveSequence,step,observed,
                         ActiveInventory.inventory.count,
                         platform,release_plan,firmware_assets)
       !=GU_GPU_EXEC_OK) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  GuLog("SELECTED_GPU_STEP=ACTIVE ORDER=%zu TARGET=%zu PEERS=%zu\n",
        step,target,ActiveInventory.inventory.count);
  return EFI_SUCCESS;
}
EFI_STATUS GuFinishSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                   int cleanup_and_handover_verified,
                                   const void *platform,
                                   const void *release_plan,
                                   const void *firmware_assets) {
  size_t step,target;
  if(!cleanup_and_handover_verified || !table ||
     !table->BootServices || Gu.bs!=table->BootServices ||
     !ResolveSelectedBinding(&step,&target) ||
     step!=ActiveSequence.active_step ||
     !platform || !release_plan || !firmware_assets ||
     !SelectedOwnersMatch(platform,release_plan,firmware_assets) ||
     ActiveSequence.targets[target].owners.platform!=platform ||
     ActiveSequence.targets[target].owners.release_plan!=release_plan ||
     ActiveSequence.targets[target].owners.firmware_assets!=firmware_assets) {
    /* A post-transaction check must not trust CPU owner tokens alone:
     * compare them to the actual live resource objects again. */
    GuAbortSelectedGpuStep();
    return EFI_NOT_READY;
  }
  GU_GPU_EXEC_OBSERVATION *observed=ActiveDiscovery.observed;
  EFI_STATUS s=ActiveStepObservations(table);
  if(s!=EFI_SUCCESS ||
     GuGpuExecFinish(&ActiveSequence,step,observed,
                     ActiveInventory.inventory.count,1)!=GU_GPU_EXEC_OK) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  GuLog("SELECTED_GPU_STEP=COMPLETE NEXT=%zu PEERS_VERIFIED=%zu\n",
        ActiveSequence.next_step,ActiveInventory.inventory.count);
  return EFI_SUCCESS;
}

/* All selected targets must have completed. Check the registry and fresh
 * full PCI snapshots on both sides of the latest live graph observation.
 * No post-close MMIO or new claim is permitted. */
EFI_STATUS GuVerifySelectedSequence(EFI_SYSTEM_TABLE *table) {
  size_t step,target;
  if(!table||!table->BootServices||Gu.bs!=table->BootServices||
     !ResolveSelectedBinding(&step,&target)||
     ActiveSequence.next_step!=ActiveSequence.selected_count||
     step+1!=ActiveSequence.selected_count) {
    GuAbortSelectedGpuStep();
    return EFI_NOT_READY;
  }
  GU_GPU_EXEC_OBSERVATION *observed=ActiveDiscovery.observed;
  EFI_STATUS s=ActiveStepObservations(table);
  if(s!=EFI_SUCCESS||
     GuGpuExecCheckComplete(&ActiveSequence,observed,ActiveInventory.inventory.count)!=GU_GPU_EXEC_OK||
     GuVerifyCompletedPlatforms(table->BootServices,&ActiveSequence,observed,
                                ActiveInventory.inventory.count)!=EFI_SUCCESS||
     !GuMsiVerifyFinalGraph()) {
    GuAbortSelectedGpuStep();
    return EFI_NOT_READY;
  }
  s=ActiveStepObservations(table);
  if(s!=EFI_SUCCESS||
     GuGpuExecCheckComplete(&ActiveSequence,observed,ActiveInventory.inventory.count)!=GU_GPU_EXEC_OK||
     GuVerifyCompletedPlatforms(table->BootServices,&ActiveSequence,observed,
                                ActiveInventory.inventory.count)!=EFI_SUCCESS) {
    GuAbortSelectedGpuStep();
    return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}

/* Report whether the checked completed binding has a successor. Invalid
 * state is not the same as successful exhaustion of the sequence. */
EFI_STATUS GuSelectedSequenceHasNext(EFI_SYSTEM_TABLE *table,int *more) {
  if(!more)return EFI_INVALID_PARAMETER;
  *more=0;
  size_t step,target;
  if(!table||!table->BootServices||Gu.bs!=table->BootServices||
     !ResolveSelectedBinding(&step,&target)||
     ActiveSequence.targets[target].phase!=GU_GPU_EXEC_COMPLETE||
     step+1!=ActiveSequence.next_step||
     ActiveSequence.next_step>ActiveSequence.selected_count) {
    GuAbortSelectedGpuStep();
    return EFI_NOT_READY;
  }
  *more=ActiveSequence.next_step<ActiveSequence.selected_count;
  return EFI_SUCCESS;
}

/* Select the next checked context after the previous step completed.
 * This is an intermediate CPU binding, not an unlock or handover grant.
 * Build admission permits only one target unless the operator passes
 * -AcceptUnverifiedActuation; the ordered list then runs one card at a time
 * and each step still requires its predecessor's verified completion. */
EFI_STATUS GuSelectNextGpu(EFI_SYSTEM_TABLE *table) {
  size_t step,target;
  if(!table||!table->BootServices||Gu.bs!=table->BootServices||!Gu.image||
     !ResolveSelectedBinding(&step,&target)||
     ActiveSequence.targets[target].phase!=GU_GPU_EXEC_COMPLETE||
     step+1!=ActiveSequence.next_step||
     ActiveSequence.next_step>=ActiveSequence.selected_count)
    return EFI_NOT_READY;
  GU_GPU_EXEC_OBSERVATION *observed=ActiveDiscovery.observed;
  GU_CAPS caps;
  EFI_STATUS s=ActiveStepObservations(table);
  if(s!=EFI_SUCCESS ||
     GuGpuExecCheckNext(&ActiveSequence,observed,ActiveInventory.inventory.count)
       !=GU_GPU_EXEC_OK) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  size_t next=ActiveSequence.order[ActiveSequence.next_step];
  const GU_GPU_RECORD *selected=&observed[next].identity;
  const UINT32 *config=observed[next].config;
  if(!GuIsSupportedPciId(selected->pci_id)||
     !GuCheckConfigInitialMaster(config,selected->segment,selected->bus,
                                 selected->device,selected->function,&caps)) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  EFI_HANDLE image=Gu.image;
  EFI_BOOT_SERVICES *bs=table->BootServices;
  GU_PLATFORM *owner=NULL;
  s=GuAllocatePlatform(bs,(EFI_PCI_IO_PROTOCOL *)selected->pci,&owner);
  if(s==EFI_SUCCESS)
    s=GuAdvanceAllocatedPlatform(bs,&owner,&ActiveSequence,observed,
                                 ActiveInventory.inventory.count);
  if(s!=EFI_SUCCESS) {
    GuAbortSelectedGpuStep();
    if(owner) {
      EFI_STATUS released=GuDiscardUnboundPlatform(&owner);
      if(released!=EFI_SUCCESS)return released;
    }
    return s;
  }
  s=ConfigureBoundPlatform(image,selected,config,&caps);
  if(s!=EFI_SUCCESS) {
    GuAbortSelectedGpuStep();
    return s;
  }
  GuLog("SELECTED_GPU_STEP=BOUND ORDER=%zu TARGET=%zu\n",ActiveSequence.next_step,next);
  return EFI_SUCCESS;
}

size_t GuSelectedGpuCount(VOID) {
  return ActiveInventoryValid&&ActiveSequenceValid?
    ActiveSequence.selected_count:0;
}
EFI_STATUS GuSelectedGpuRecordAt(size_t index,GU_GPU_RECORD *out) {
  if(!out||!ActiveInventoryValid||!ActiveSequenceValid||
     !ActiveInventory.inventory.nvidia||!ActiveInventory.selected_indices||
     index>=ActiveSequence.selected_count||
     index>=ActiveInventory.selected_count)return EFI_NOT_READY;
  size_t record=ActiveInventory.selected_indices[index];
  if(record>=ActiveInventory.inventory.count)return EFI_DEVICE_ERROR;
  *out=ActiveInventory.inventory.nvidia[record];
  return GuIsSupportedPciId(out->pci_id)?EFI_SUCCESS:EFI_DEVICE_ERROR;
}

/* Allocate one execution record per observed device. Keep the old sequence
 * intact until the existing one-shot bind succeeds. Retain published storage
 * for the remaining boot: completion and handover consume these records. */
static EFI_STATUS BindPreparedSequence(EFI_SYSTEM_TABLE *table,
                                       EFI_PCI_IO_PROTOCOL *io,
                                       const GU_GPU_PREPARED_VIEW *prepared,
                                       GU_GPU_DISCOVERY_STORAGE *storage) {
  if(!table || !table->BootServices || !io || !prepared || !storage ||
     !storage->allocation || !storage->observed || !storage->targets || !storage->order ||
     prepared->inventory.nvidia!=storage->records ||
     prepared->selected_indices!=storage->indices ||
     prepared->inventory.capacity!=storage->capacity ||
     storage->capacity>GU_INVENTORY_MAX_HANDLES ||
     !prepared->inventory.count || prepared->inventory.count>storage->capacity ||
     !prepared->selected_count || prepared->selected_count>prepared->inventory.count ||
     prepared->selected_indices[0]>=prepared->inventory.count ||
     prepared->inventory.nvidia[prepared->selected_indices[0]].pci!=io)
    return EFI_INVALID_PARAMETER;
  if(ActiveSequenceValid || ActiveInventoryValid || UnreleasedDiscoveryAllocation)
    return EFI_ALREADY_STARTED;
  size_t count=prepared->inventory.count;
  GU_GPU_EXEC_SEQUENCE candidate;
  /* All backing arrays share the candidate allocation. On refusal the
   * caller owns it and releases it once, or retains it if FreePool fails. */
  if(GuGpuExecInit(&candidate,storage->observed,count,prepared->selected_indices,
                   prepared->selected_count,storage->targets,storage->order,count)!=GU_GPU_EXEC_OK ||
     GuGpuExecCheckNext(&candidate,storage->observed,count)!=GU_GPU_EXEC_OK)
    return EFI_UNSUPPORTED;
  EFI_STATUS s;
  s=GuInit(table->BootServices,io);
  if(s!=EFI_SUCCESS)return s;
  ActiveInventory=*prepared;
  ActiveDiscovery=*storage;
  memset(storage,0,sizeof(*storage)); /* Transfer pool ownership once. */
  ActiveSequence=candidate;
  ActiveSequenceValid=1;
  ActiveInventoryValid=1;
  return EFI_SUCCESS;
}

#endif

#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
/* Owned CPU-only candidates. Retain the table and any failed releases
 * until boot termination. Never publish a candidate as the active GPU. */
static GU_PLATFORM **PreparedContexts;
static size_t PreparedContextCount;
/* An uncertain table address is diagnostic evidence, not an owned array. */
static VOID *UnreleasedPreparedContextTable;
/* Do not publish, inspect or dispose of a failed candidate output. */
static UINTN UnconfirmedPreparedContextAddress;

static EFI_STATUS PreparePlatformContexts(EFI_BOOT_SERVICES *bs,
                                          const GU_GPU_EXEC_SEQUENCE *sequence) {
  if(!bs || !bs->AllocatePool || !bs->FreePool || !sequence ||
     !sequence->targets || !sequence->order || !sequence->count ||
     sequence->count>sequence->capacity ||
     sequence->capacity>GU_INVENTORY_MAX_HANDLES ||
     !sequence->selected_count || sequence->selected_count>sequence->count)
    return EFI_INVALID_PARAMETER;
  if(PreparedContexts||UnreleasedPreparedContextTable||UnconfirmedPreparedContextAddress)
    return EFI_ALREADY_STARTED;
  size_t count=sequence->selected_count;
  /* The sequence was checked by GuGpuExecCheckNext. Confirm all indices
   * and PCI references before allocating any candidate. */
  for(size_t i=0;i<count;i++) {
    size_t index=sequence->order[i];
    if(index>=sequence->count || !sequence->targets[index].identity.pci)
      return EFI_INVALID_PARAMETER;
    for(size_t j=0;j<i;j++)
      if(sequence->targets[sequence->order[j]].identity.pci==
         sequence->targets[index].identity.pci)return EFI_INVALID_PARAMETER;
  }
  GU_PLATFORM **contexts=NULL;
  EFI_STATUS status=bs->AllocatePool(EfiLoaderData,count*sizeof(*contexts),
                                     (VOID **)&contexts);
  if(status!=EFI_SUCCESS) {
    if(contexts) {
      UnreleasedPreparedContextTable=contexts;
      GuLog("ACTIVE_CONTEXT_TABLE_UNCERTAIN ADDRESS=%p STATUS=%#llx\n",
            (VOID *)contexts,(unsigned long long)status);
      return EFI_DEVICE_ERROR;
    }
    return status;
  }
  if(!contexts)return EFI_OUT_OF_RESOURCES;
  memset(contexts,0,count*sizeof(*contexts));
  PreparedContexts=contexts;
  PreparedContextCount=count;
  for(size_t i=0;i<count;i++) {
    EFI_PCI_IO_PROTOCOL *pci=(EFI_PCI_IO_PROTOCOL *)
        sequence->targets[sequence->order[i]].identity.pci;
    GU_PLATFORM *candidate=NULL;
    status=GuAllocatePlatform(bs,pci,&candidate);
    if(status!=EFI_SUCCESS) {
      if(candidate) {
        UnconfirmedPreparedContextAddress=(UINTN)candidate;
        GuLog("ACTIVE_CONTEXT[%zu]_ALLOCATION_UNCERTAIN ADDRESS=%p STATUS=%#llx\n",
              i,(VOID *)candidate,(unsigned long long)status);
        status=EFI_DEVICE_ERROR;
      }
      break;
    }
    if(!candidate){status=EFI_OUT_OF_RESOURCES;break;}
    int alias=candidate==GuPlatformActive;
    for(size_t j=0;j<i;j++)if(contexts[j]==candidate)alias=1;
    if(alias || candidate->bs!=bs || candidate->pci!=pci) {
      UnconfirmedPreparedContextAddress=(UINTN)candidate;
      GuLog("ACTIVE_CONTEXT[%zu]_OWNER_UNCONFIRMED ADDRESS=%p ALIAS=%u\n",
            i,(VOID *)candidate,(unsigned)alias);
      status=EFI_DEVICE_ERROR;
      break;
    }
    contexts[i]=candidate;
    GuLog("ACTIVE_CONTEXT[%zu]=PREPARED CPU_ONLY=1 MUTATION_AUTHORIZED=0\n",i);
  }
  EFI_STATUS cleanup=EFI_SUCCESS;
  for(size_t i=0;i<PreparedContextCount;i++) {
    if(!contexts[i])continue;
    GU_PLATFORM *candidate=contexts[i];
    EFI_STATUS released=GuDiscardUnboundPlatform(&contexts[i]);
    if(released!=EFI_SUCCESS) {
      GuLog("ACTIVE_CONTEXT[%zu]_FREE_STATUS=%#llx ADDRESS=%p REFERENCE_RETAINED=%u\n",
            i,(unsigned long long)released,(VOID *)candidate,(unsigned)(contexts[i]!=NULL));
      if(cleanup==EFI_SUCCESS)cleanup=released;
    }
  }
  if(cleanup==EFI_SUCCESS) {
    /* Consume the published table reference before the release attempt. */
    PreparedContexts=NULL;
    PreparedContextCount=0;
    cleanup=bs->FreePool(contexts);
    if(cleanup!=EFI_SUCCESS) {
      UnreleasedPreparedContextTable=contexts;
      GuLog("ACTIVE_CONTEXT_TABLE_FREE_UNCERTAIN ADDRESS=%p STATUS=%#llx\n",
            (VOID *)contexts,(unsigned long long)cleanup);
    }
  }
  if(cleanup!=EFI_SUCCESS) {
    GuLog("ACTIVE_CONTEXT_STORAGE_FREE_STATUS=%#llx RETAINED=1\n",
          (unsigned long long)cleanup);
    return cleanup;
  }
  return status;
}
#endif

EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  GU_GPU_DISCOVERY_STORAGE storage={0};
  EFI_HANDLE *handles=NULL;UINTN handle_count=0;
  EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 config[64];
  GU_CAPS caps;
  if(!image||!table||!table->BootServices)return EFI_INVALID_PARAMETER;
  if(UnreleasedDiscoveryAllocation||UnreleasedPciHandles)return EFI_ALREADY_STARTED;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  if(PreparedContexts||UnreleasedPreparedContextTable||UnconfirmedPreparedContextAddress)
    return EFI_ALREADY_STARTED;
#endif
  EFI_STATUS s=LocatePciHandles(table,&handles,&handle_count);
  if(s!=EFI_SUCCESS)return s;
  s=AllocateDiscoveryStorage(table->BootServices,(size_t)handle_count,&storage);
  if(s!=EFI_SUCCESS){
    EFI_STATUS released=ReleasePciHandles(table->BootServices,&handles);
    return released==EFI_SUCCESS?s:released;
  }
  GU_GPU_PREPARED_VIEW prepared=DiscoveryView(&storage);
  s=CollectLocatedGpuRecords(table,handles,handle_count,prepared.inventory.nvidia,
                             prepared.inventory.capacity,&prepared.inventory.count);
  if(s!=EFI_SUCCESS)goto done;
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
  {
#if defined(GU_RUNTIME_SELECTION)
    const GU_GPU_LOCATION *requested=NULL;
    size_t requested_count=0;
    BOOLEAN all=FALSE;
    s=ResolveRuntimeSelection(table,&requested,&requested_count,&all);
    if(s!=EFI_SUCCESS)goto done;
#else
    const GU_GPU_LOCATION *requested=ActiveRequested;
    size_t requested_count=GU_GPU_SELECTION_COUNT;
    BOOLEAN all=FALSE;
#endif
    s=SelectPreparedRecords(&prepared,requested,requested_count,all);
  }
#else
  s=SelectPreparedRecords(&prepared,NULL,0,FALSE);
#endif
  if(s!=EFI_SUCCESS)goto done;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  {
    GU_GPU_EXEC_SEQUENCE sequence;
    s=RefreshGpuRecords(table,&prepared,storage.live,storage.scanned,storage.capacity);
    if(s!=EFI_SUCCESS){
      GuLog("ACTIVE_REFRESH=REFUSED STATUS=%#llx BEFORE_GU_INIT=1\n",(unsigned long long)s);
      s=EFI_UNSUPPORTED;goto done;
    }
    for(size_t i=0;i<prepared.inventory.count;i++){
      storage.observed[i].identity=prepared.inventory.nvidia[i];
      memcpy(storage.observed[i].config,storage.scanned[i].config,
             sizeof(storage.observed[i].config));
    }
    if(GuGpuExecInit(&sequence,storage.observed,prepared.inventory.count,
                     prepared.selected_indices,prepared.selected_count,
                     storage.targets,storage.order,storage.capacity)!=GU_GPU_EXEC_OK||
       GuGpuExecCheckNext(&sequence,storage.observed,prepared.inventory.count)!=GU_GPU_EXEC_OK){
      GuLog("ACTIVE_SEQUENCE=REFUSED REASON=STATE_OR_PEER_DRIFT BEFORE_GU_INIT=1\n");
      s=EFI_UNSUPPORTED;goto done;
    }
    s=PreparePlatformContexts(table->BootServices,&sequence);
    if(s!=EFI_SUCCESS){
      GuLog("ACTIVE_CONTEXTS=REFUSED STATUS=%#llx BEFORE_GU_INIT=1\n",(unsigned long long)s);
      goto done;
    }
    GuLog("ACTIVE_SEQUENCE=PREPARED_NOT_STARTED SELECTED=%zu TRACKED=%zu NEXT=0 MUTATION_AUTHORIZED=0\n",
          sequence.selected_count,sequence.count);
    GuLog("ACTIVE_REFRESH=PASSIVE_REVALIDATED TARGETS=%zu MUTATION_AUTHORIZED=0\n",prepared.inventory.count);

    /* The same link policy the waived path applies, printed here as well: this
     * image reads the snapshot and says what the admission would decide, so a
     * bench can be corrected with a boot that cannot write instead of with one
     * that may. Nothing is refused here - this image refuses anyway - and the
     * verdict is a word in the log, not an action. */
    {
      size_t observed=0;
      for(size_t n=0;n<prepared.selected_count;n++) {
        size_t index=prepared.selected_indices[n];
        if(index>=prepared.inventory.count)continue;
        const GU_GPU_RECORD *record=&storage.observed[index].identity;
        GU_CAPS link_caps;
        if(!GuCheckConfigInitialMaster(storage.observed[index].config,record->segment,
                                       record->bus,record->device,record->function,&link_caps)) {
          GuLog("ACTIVE_LINK[%zu] BDF=%04x:%02x:%02x.%x PCI_LINK=CONFIG_REFUSED ADMISSION=UNKNOWN\n",
                observed,(unsigned)record->segment,(unsigned)record->bus,
                (unsigned)record->device,(unsigned)record->function);
          observed++;
          continue;
        }
        GU_LINK link = {0};   /* a log that never reached the check prints zeros, not stack */
        GU_LINK_VERDICT verdict=GuCheckLinkState(storage.observed[index].config,
                                                 link_caps.pcie,&link);
        GuLog("ACTIVE_LINK[%zu] BDF=%04x:%02x:%02x.%x PCI_LINK=%s WIDTH=%u MAX_WIDTH=%u "
              "SPEED=%u MAX_SPEED=%u ASPM=%u TRAINED=%u UP=%u ERRORS=%u ERROR_CE=%u "
              "ERROR_NONFATAL=%u ERROR_FATAL=%u ERROR_UR=%u "
              "OK_ERRORS=%u OK_ASPM=%u OK_TRAINED=%u OK_LINK=%u UP_WEIGHED=0 ADMISSION=%s\n",
              observed,(unsigned)record->segment,(unsigned)record->bus,
              (unsigned)record->device,(unsigned)record->function,
              GuLinkReason(verdict),(unsigned)link.width,(unsigned)link.max_width,
              (unsigned)link.speed,(unsigned)link.max_speed,(unsigned)link.aspm,
              (unsigned)link.trained,(unsigned)link.up,(unsigned)link.errors,
              (unsigned)(link.errors&1U),(unsigned)((link.errors>>1U)&1U),
              (unsigned)((link.errors>>2U)&1U),(unsigned)((link.errors>>3U)&1U),
              /* One per condition the verdict weighs, so a refusal says how many
               * things are wrong instead of only the first. Only the two
               * uncorrected error bits count, and DLLLA is not weighed at all -
               * see GuCheckLinkState for the evidence. */
              (unsigned)((link.errors&0x06U)==0),(unsigned)(link.aspm==0),
              (unsigned)(link.trained!=0),
              (unsigned)(link.width!=0&&link.speed!=0&&link.speed!=0x0fU),
              GuLinkAccepted(verdict)?"ALLOW":"REFUSE");
        observed++;
      }
    }
  }
  GuLog("ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\n");
  s=EFI_UNSUPPORTED;goto done;
#endif
#ifndef GU_ACTIVE_SINGLE_TARGET
  if(prepared.inventory.count>1){
    GuLog("ACTIVE_EXECUTION=REFUSED REASON=MULTI_NVIDIA_GLOBAL_STATE NVIDIA_COUNT=%zu SELECTED=%zu BEFORE_GU_INIT=1\n",
          prepared.inventory.count,prepared.selected_count);
    s=EFI_UNSUPPORTED;goto done;
  }
  if(prepared.inventory.count!=1||prepared.selected_count!=1||prepared.selected_indices[0]!=0){
    s=EFI_UNSUPPORTED;goto done;
  }
#else
  if(!prepared.selected_count||prepared.selected_count>prepared.inventory.count||
     prepared.selected_indices[0]>=prepared.inventory.count){
    GuLog("ACTIVE_EXECUTION=REFUSED REASON=NO_SELECTED_TARGET SELECTED=%zu NVIDIA_COUNT=%zu BEFORE_GU_INIT=1\n",
          prepared.selected_count,prepared.inventory.count);
    s=EFI_UNSUPPORTED;goto done;
  }
#ifndef GU_SYNTHETIC_COMPILE_ONLY
  /* Without the explicit waiver only one card may be admitted at a time. The
   * waiver admits an ordered list: the shared coordinator still executes one
   * transaction at a time and requires each predecessor's completion record. */
  if(prepared.selected_count!=1 && !GU_ACTUATION_ADMITTED){
    GuLog("ACTIVE_EXECUTION=REFUSED REASON=MULTI_TARGET_WITHOUT_WAIVER SELECTED=%zu\n",
          prepared.selected_count);
    s=EFI_UNSUPPORTED;goto done;
  }
#endif
#endif
  /* Copy the selected identity: non-active discovery releases scratch
   * before GuInit and the active path transfers its backing on success. */
  GU_GPU_RECORD selected=prepared.inventory.nvidia[prepared.selected_indices[0]];
  if(!GuIsSupportedPciId(selected.pci_id)){s=EFI_UNSUPPORTED;goto done;}
  s=RefreshGpuRecord(table,&selected,config,&io);
  if(s!=EFI_SUCCESS){s=EFI_UNSUPPORTED;goto done;}
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(!GuCheckConfigInitialMaster(config,selected.segment,selected.bus,
                                  selected.device,selected.function,&caps)){
    s=EFI_UNSUPPORTED;goto done;
  }
#else
  if(!GuCheckConfig(config,selected.segment,selected.bus,selected.device,selected.function,&caps)){
    s=EFI_UNSUPPORTED;goto done;
  }
#endif
  /* The capability chain says the card is a display function in a state this
   * image may touch. It says nothing about the link the card sits on. Observe
   * it once, name what it looks like, and refuse the states an unlock must not
   * start on: an uncorrected PCIe error, an ASPM request this image does not
   * manage, a link that is not trained or that negotiated no width or speed.
   * Correctable errors, unsupported requests and DLLLA are recorded, not
   * weighed, for the reasons GuCheckLinkState gives. AER lives in extended config space,
   * which this snapshot does not reach, so it is named as unobserved. */
  {
    GU_LINK link = {0};   /* a log that never reached the check prints zeros, not stack */
    GU_LINK_VERDICT verdict=GuCheckLinkState(config,caps.pcie,&link);
    GuLog("PCI_LINK=%s WIDTH=%u MAX_WIDTH=%u SPEED=%u MAX_SPEED=%u ASPM=%u "
          "TRAINED=%u UP=%u ERRORS=%u ERROR_CE=%u ERROR_NONFATAL=%u ERROR_FATAL=%u "
          "ERROR_UR=%u OK_ERRORS=%u OK_ASPM=%u OK_TRAINED=%u OK_LINK=%u UP_WEIGHED=0 "
          "HOTPLUG=%u SURPRISE=%u AER=UNOBSERVED "
          "UNHANDLED=HOTPLUG,AER BDF=%04x:%02x:%02x.%x\n",
          GuLinkReason(verdict),(unsigned)link.width,(unsigned)link.max_width,
          (unsigned)link.speed,(unsigned)link.max_speed,(unsigned)link.aspm,
          (unsigned)link.trained,(unsigned)link.up,(unsigned)link.errors,
          (unsigned)(link.errors&1U),(unsigned)((link.errors>>1U)&1U),
          (unsigned)((link.errors>>2U)&1U),(unsigned)((link.errors>>3U)&1U),
          (unsigned)((link.errors&0x06U)==0),(unsigned)(link.aspm==0),
          (unsigned)(link.trained!=0),
          (unsigned)(link.width!=0&&link.speed!=0&&link.speed!=0x0fU),
          (unsigned)link.hotplug,(unsigned)link.surprise,
          (unsigned)selected.segment,(unsigned)selected.bus,
          (unsigned)selected.device,(unsigned)selected.function);
    if(!GuLinkAccepted(verdict)){
      GuLog("ACTIVE_EXECUTION=REFUSED REASON=PCIE_LINK LINK_REASON=%s BDF=%04x:%02x:%02x.%x\n",
            GuLinkReason(verdict),(unsigned)selected.segment,(unsigned)selected.bus,
            (unsigned)selected.device,(unsigned)selected.function);
      /* Nothing was mutated and nothing allocated, so nothing is retained:
       * this refusal keeps the pre-mutation state the caller started in. */
      s=EFI_UNSUPPORTED;goto done;
    }
  }
#ifdef GU_ACTIVE_SINGLE_TARGET
  s=RefreshGpuRecords(table,&prepared,storage.live,storage.scanned,storage.capacity);
  size_t chosen=prepared.selected_indices[0];
  if(s!=EFI_SUCCESS||storage.scanned[chosen].pci!=io||
     memcmp(storage.scanned[chosen].config,config,sizeof(config))!=0){
    s=EFI_UNSUPPORTED;goto done;
  }
  for(size_t i=0;i<prepared.inventory.count;i++){
    storage.observed[i].identity=prepared.inventory.nvidia[i];
    memcpy(storage.observed[i].config,storage.scanned[i].config,
           sizeof(storage.observed[i].config));
  }
  s=BindPreparedSequence(table,io,&prepared,&storage);
  if(s!=EFI_SUCCESS)goto done;
#if GU_ACTUATION_ADMITTED
  /* Admission authorises the write; it does not make the outcome eligible.
   * Eligibility stays false so a log cannot be mistaken for a qualification,
   * and a simulated run is marked as such. */
#if GU_ACTUATION_SIMULATED
  GuLog("ACTIVE_EXECUTION=SIMULATED SELECTED=%zu ORDERED_SEQUENTIAL=1 "
        "MUTATION_AUTHORIZED=1 ENVIRONMENT=HOST_SIMULATOR CAN_DETACH=0 "
        "CAN_UNLOCK=0 CAN_FLASH=0 RECOVERY_VERIFIED=0 SIGNED_BOOTER_UNVERIFIED=1\n",
        prepared.selected_count);
#else
  GuLog("ACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=%zu ORDERED_SEQUENTIAL=1 "
        "MUTATION_AUTHORIZED=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 "
        "RECOVERY_VERIFIED=0 SIGNED_BOOTER_UNVERIFIED=1 "
        "WAIVER=GU_ACTIVE_UNVERIFIED_ACTUATION\n",prepared.selected_count);
#endif
#endif
#else
  s=FreeDiscoveryStorage(table->BootServices,&storage);
  if(s!=EFI_SUCCESS)return s;
  s=GuInit(table->BootServices,io);
  if(s!=EFI_SUCCESS)return s;
#endif
  s=ConfigureBoundPlatform(image,&selected,config,&caps);
  if(s!=EFI_SUCCESS)goto done;
  return EFI_SUCCESS;
done:
  {
    EFI_STATUS released=FreeDiscoveryStorage(table->BootServices,&storage);
    return released==EFI_SUCCESS?s:released;
  }
}
