/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_discovery.h"
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
#include "gpu-selection.h" /* Generated operator-supplied PCI addresses. */
#if GU_GPU_SELECTION_COUNT < 1 || GU_GPU_SELECTION_COUNT > GU_INVENTORY_MAX_HANDLES
#error Invalid active target count
#endif
#if defined(GU_ACTIVE_SINGLE_TARGET) && GU_GPU_SELECTION_COUNT != 1 && !defined(GU_SYNTHETIC_COMPILE_ONLY)
#error One-target active execution requires exactly one PCI address
#endif
static const GU_GPU_LOCATION ActiveRequested[GU_GPU_SELECTION_COUNT] =
    GU_GPU_SELECTION_LOCATIONS;
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
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
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

static EFI_STATUS AllocateDiscoveryStorage(EFI_BOOT_SERVICES *bs,size_t capacity,
                                           GU_GPU_DISCOVERY_STORAGE *storage) {
  if(!bs||!bs->AllocatePool||!bs->FreePool||!storage||storage->allocation||
     !capacity||capacity>GU_INVENTORY_MAX_HANDLES)return EFI_INVALID_PARAMETER;
  const size_t types[]={
    sizeof(GU_GPU_RECORD),sizeof(GU_GPU_RECORD),sizeof(size_t),
    sizeof(GU_GPU_REFRESHED_TARGET),sizeof(GU_GPU_EXEC_OBSERVATION),
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
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
  if(s!=EFI_SUCCESS)return s;
  if(!allocation)return EFI_OUT_OF_RESOURCES;
  memset(allocation,0,total);
  unsigned char *base=(unsigned char *)allocation;
  storage->allocation=allocation;storage->capacity=capacity;
  storage->records=(GU_GPU_RECORD *)(base+offsets[0]);
  storage->live=(GU_GPU_RECORD *)(base+offsets[1]);
  storage->indices=(size_t *)(base+offsets[2]);
  storage->scanned=(GU_GPU_REFRESHED_TARGET *)(base+offsets[3]);
  storage->observed=(GU_GPU_EXEC_OBSERVATION *)(base+offsets[4]);
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
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
static GU_GPU_EXEC_TARGET *ActiveSequenceTargets;
static size_t *ActiveSequenceOrder;
/* Observation workspace belongs to the published ordered record. */
static GU_GPU_EXEC_OBSERVATION *ActiveObservations;
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

/* The firmware owns the handle buffer until its matching FreePool call.
 * All inventory buffers below belong to the caller. */
static EFI_STATUS LocatePciHandles(EFI_SYSTEM_TABLE *table,EFI_HANDLE **handles,UINTN *count){
  if(!table||!table->BootServices||!table->BootServices->LocateHandleBuffer||
     !table->BootServices->HandleProtocol||!table->BootServices->FreePool)
    return EFI_INVALID_PARAMETER;
  *handles=NULL;*count=0;
  EFI_STATUS s=table->BootServices->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,count,handles);
  if(s!=EFI_SUCCESS)return EFI_UNSUPPORTED;
  if(!*handles||!*count||*count>GU_INVENTORY_MAX_HANDLES){
    if(*handles)(VOID)table->BootServices->FreePool(*handles);
    *handles=NULL;*count=0;
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
  EFI_STATUS released=table->BootServices->FreePool(handles);
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

static EFI_STATUS SelectPreparedRecords(GU_GPU_PREPARED_VIEW *prepared,
                                         const GU_GPU_LOCATION *requested,
                                         size_t requested_count) {
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
    if(requested_count>count)return EFI_UNSUPPORTED;
    int result=GuFindGpuRecordSequence(prepared->inventory.nvidia,count,requested,
                                       requested_count,prepared->selected_indices);
    if(result!=GU_INVENTORY_OK){
      GuLog("ACTIVE_PREPARE=REFUSED SELECTION_CODE=%d REQUESTED=%zu\n",result,requested_count);
      return EFI_UNSUPPORTED;
    }
    prepared->selected_count=requested_count;
  }else if(count==1){
    prepared->selected_count=1;prepared->selected_indices[0]=0;
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
  if(s==EFI_SUCCESS)s=SelectPreparedRecords(&view,requested,requested_count);
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
  if(!ActiveInventoryValid || !ActiveSequenceValid ||
     !ActiveSequenceTargets || !ActiveSequenceOrder || !ActiveObservations ||
     ActiveSequence.targets!=ActiveSequenceTargets ||
     ActiveSequence.order!=ActiveSequenceOrder ||
     !ActiveDiscovery.allocation ||
     ActiveInventory.inventory.nvidia!=ActiveDiscovery.records ||
     ActiveInventory.selected_indices!=ActiveDiscovery.indices ||
     ActiveInventory.inventory.capacity!=ActiveDiscovery.capacity ||
     ActiveObservations!=ActiveDiscovery.observed ||
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
      !Gu.asset_entries_reserved ||
      bound->owners.firmware_assets!=Gu.asset_entries))
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

/* Initialize the newly bound platform from the exact checked PCI snapshot.
 * Reuse this for the initial and every subsequent context. */
static void ConfigureBoundPlatform(EFI_HANDLE image,const GU_GPU_RECORD *selected,
                                   const UINT32 config[64],const GU_CAPS *caps) {
  Gu.image=image;
  Gu.handle=(EFI_HANDLE)selected->handle;
  Gu.device_id=(UINT16)(selected->pci_id >> 16);
  Gu.is_ampere=GuIsAmpereDeviceId(Gu.device_id);
  Gu.is_turing=GuIsTuringDeviceId(Gu.device_id);
  Gu.bar_base=(config[4] & ~0xfU);
  Gu.bar_size=0x1000000;
  Gu.pm_cap=caps->pm;
  Gu.pcie_cap=caps->pcie;
  Gu.pcie_version=caps->pcie_version;
  Gu.msi_cap=caps->msi;
  Gu.msix_cap=caps->msix;
}

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Re-enumerate and read the COMPLETE NVIDIA inventory twice. A later
 * selected-card step must not trust a previous firmware/PCI snapshot. */
static EFI_STATUS ActiveStepObservations(EFI_SYSTEM_TABLE *table) {
  if(!ActiveInventoryValid||!ActiveSequenceValid||!table||!table->BootServices||
     !ActiveObservations||ActiveObservations!=ActiveDiscovery.observed)
    return EFI_INVALID_PARAMETER;
  EFI_STATUS s=RefreshGpuRecords(table,&ActiveInventory,ActiveDiscovery.live,
                                  ActiveDiscovery.scanned,ActiveDiscovery.capacity);
  if(s!=EFI_SUCCESS)return EFI_UNSUPPORTED;
  for(size_t i=0;i<ActiveInventory.inventory.count;i++){
    ActiveObservations[i].identity=ActiveInventory.inventory.nvidia[i];
    memcpy(ActiveObservations[i].config,ActiveDiscovery.scanned[i].config,
           sizeof(ActiveObservations[i].config));
  }
  return EFI_SUCCESS;
}
void GuAbortSelectedGpuStep(void) {
  if(!ActiveSequenceValid)return;
  GuGpuExecAbort(&ActiveSequence);
  GuLog("SELECTED_GPU_STEP=HALTED NEXT=%zu MUTATION_AUTHORIZED=0\\n",
        ActiveSequence.next_step);
}
/* Verify the backing objects, not only the caller's claim about them. */
static int SelectedOwnersMatch(const void *platform,const void *release_plan,
                               const void *firmware_assets) {
  return platform==&Gu && Gu.release_state && release_plan==Gu.release_state &&
         Gu.asset_entries_reserved && firmware_assets==Gu.asset_entries;
}
EFI_STATUS GuBeginSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                  const void *platform,
                                  const void *release_plan,
                                  const void *firmware_assets) {
  if(!ActiveSequenceValid || !ActiveInventoryValid ||
     !table || !table->BootServices ||
     Gu.bs!=table->BootServices || !Gu.handle || !Gu.pci ||
     !Gu.profile_verified || !Gu.prom_verified ||
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
  GU_GPU_EXEC_OBSERVATION *observed=ActiveObservations;
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
  GU_GPU_EXEC_OBSERVATION *observed=ActiveObservations;
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
  GU_GPU_EXEC_OBSERVATION *observed=ActiveObservations;
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
 * Production discovery and build admission still permit only one target. */
EFI_STATUS GuSelectNextGpu(EFI_SYSTEM_TABLE *table) {
  size_t step,target;
  if(!table||!table->BootServices||Gu.bs!=table->BootServices||!Gu.image||
     !ResolveSelectedBinding(&step,&target)||
     ActiveSequence.targets[target].phase!=GU_GPU_EXEC_COMPLETE||
     step+1!=ActiveSequence.next_step||
     ActiveSequence.next_step>=ActiveSequence.selected_count)
    return EFI_NOT_READY;
  GU_GPU_EXEC_OBSERVATION *observed=ActiveObservations;
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
  ConfigureBoundPlatform(image,selected,config,&caps);
  GuLog("SELECTED_GPU_STEP=BOUND ORDER=%zu TARGET=%zu\n",ActiveSequence.next_step,next);
  return EFI_SUCCESS;
}

/* Allocate one execution record per observed device. Keep the old sequence
 * intact until the existing one-shot bind succeeds. Retain published storage
 * for the remaining boot: completion and handover consume these records. */
static EFI_STATUS BindPreparedSequence(EFI_SYSTEM_TABLE *table,
                                       EFI_PCI_IO_PROTOCOL *io,
                                       const GU_GPU_PREPARED_VIEW *prepared,
                                       GU_GPU_DISCOVERY_STORAGE *storage) {
  if(!table || !table->BootServices || !io || !prepared || !storage ||
     !storage->allocation || !storage->observed ||
     prepared->inventory.nvidia!=storage->records ||
     prepared->selected_indices!=storage->indices ||
     prepared->inventory.capacity!=storage->capacity ||
     storage->capacity>GU_INVENTORY_MAX_HANDLES ||
     !prepared->inventory.count || prepared->inventory.count>storage->capacity ||
     !prepared->selected_count || prepared->selected_count>prepared->inventory.count ||
     prepared->selected_indices[0]>=prepared->inventory.count ||
     prepared->inventory.nvidia[prepared->selected_indices[0]].pci!=io)
    return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *bs=table->BootServices;
  if(!bs->AllocatePool || !bs->FreePool)return EFI_UNSUPPORTED;
  if(ActiveSequenceValid || ActiveInventoryValid)return EFI_ALREADY_STARTED;
  size_t count=prepared->inventory.count;
  GU_GPU_EXEC_SEQUENCE candidate;
  GU_GPU_EXEC_TARGET *targets=NULL;
  size_t *order=NULL;
  const GU_GPU_EXEC_OBSERVATION *observations=storage->observed;
  EFI_STATUS s=bs->AllocatePool(EfiLoaderData,count*sizeof(*targets),(VOID **)&targets);
  if(s==EFI_SUCCESS && !targets)s=EFI_OUT_OF_RESOURCES;
  if(s==EFI_SUCCESS) {
    s=bs->AllocatePool(EfiLoaderData,count*sizeof(*order),(VOID **)&order);
    if(s==EFI_SUCCESS && !order)s=EFI_OUT_OF_RESOURCES;
  }
  if(s==EFI_SUCCESS &&
     (GuGpuExecInit(&candidate,observations,count,prepared->selected_indices,
                   prepared->selected_count,targets,order,count)!=GU_GPU_EXEC_OK ||
      GuGpuExecCheckNext(&candidate,observations,count)!=GU_GPU_EXEC_OK))
    s=EFI_UNSUPPORTED;
  if(s==EFI_SUCCESS)s=GuInit(table->BootServices,io);
  if(s!=EFI_SUCCESS) {
    EFI_STATUS released=EFI_SUCCESS;
    if(order) {
      EFI_STATUS r=bs->FreePool(order);
      if(r!=EFI_SUCCESS)released=r;
    }
    if(targets) {
      EFI_STATUS r=bs->FreePool(targets);
      if(r!=EFI_SUCCESS)released=r;
    }
    if(released!=EFI_SUCCESS) {
      GuLog("ACTIVE_SEQUENCE_STORAGE_FREE_STATUS=%#llx\n",(unsigned long long)released);
      return released;
    }
    return s;
  }
  ActiveInventory=*prepared;
  ActiveSequenceTargets=targets;
  ActiveSequenceOrder=order;
  ActiveDiscovery=*storage;
  memset(storage,0,sizeof(*storage)); /* Transfer pool ownership once. */
  ActiveObservations=ActiveDiscovery.observed;
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

static EFI_STATUS PreparePlatformContexts(EFI_BOOT_SERVICES *bs,
                                          const GU_GPU_EXEC_SEQUENCE *sequence) {
  if(!bs || !bs->AllocatePool || !bs->FreePool || !sequence ||
     !sequence->targets || !sequence->order || !sequence->count ||
     sequence->count>sequence->capacity ||
     sequence->capacity>GU_INVENTORY_MAX_HANDLES ||
     !sequence->selected_count || sequence->selected_count>sequence->count)
    return EFI_INVALID_PARAMETER;
  if(PreparedContexts)return EFI_ALREADY_STARTED;
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
  if(status!=EFI_SUCCESS)return status;
  if(!contexts)return EFI_OUT_OF_RESOURCES;
  memset(contexts,0,count*sizeof(*contexts));
  PreparedContexts=contexts;
  PreparedContextCount=count;
  for(size_t i=0;i<count;i++) {
    EFI_PCI_IO_PROTOCOL *pci=(EFI_PCI_IO_PROTOCOL *)
        sequence->targets[sequence->order[i]].identity.pci;
    status=GuAllocatePlatform(bs,pci,&contexts[i]);
    if(status!=EFI_SUCCESS)break;
    if(!contexts[i] || contexts[i]->bs!=bs || contexts[i]->pci!=pci) {
      status=EFI_UNSUPPORTED;
      break;
    }
    GuLog("ACTIVE_CONTEXT[%zu]=PREPARED CPU_ONLY=1 MUTATION_AUTHORIZED=0\n",i);
  }
  EFI_STATUS cleanup=EFI_SUCCESS;
  for(size_t i=0;i<PreparedContextCount;i++) {
    if(!contexts[i])continue;
    EFI_STATUS released=GuDiscardUnboundPlatform(&contexts[i]);
    if(released!=EFI_SUCCESS && cleanup==EFI_SUCCESS)cleanup=released;
  }
  if(cleanup==EFI_SUCCESS) {
    cleanup=bs->FreePool(contexts);
    if(cleanup==EFI_SUCCESS) {
      PreparedContexts=NULL;
      PreparedContextCount=0;
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
  if(UnreleasedDiscoveryAllocation)return EFI_ALREADY_STARTED;
  EFI_STATUS s=LocatePciHandles(table,&handles,&handle_count);
  if(s!=EFI_SUCCESS)return s;
  s=AllocateDiscoveryStorage(table->BootServices,(size_t)handle_count,&storage);
  if(s!=EFI_SUCCESS){
    EFI_STATUS released=table->BootServices->FreePool(handles);
    return released==EFI_SUCCESS?s:released;
  }
  GU_GPU_PREPARED_VIEW prepared=DiscoveryView(&storage);
  s=CollectLocatedGpuRecords(table,handles,handle_count,prepared.inventory.nvidia,
                             prepared.inventory.capacity,&prepared.inventory.count);
  if(s!=EFI_SUCCESS)goto done;
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
  s=SelectPreparedRecords(&prepared,ActiveRequested,GU_GPU_SELECTION_COUNT);
#else
  s=SelectPreparedRecords(&prepared,NULL,0);
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
    s=EFI_UNSUPPORTED;goto done;
  }
#ifndef GU_SYNTHETIC_COMPILE_ONLY
  if(prepared.selected_count!=1){s=EFI_UNSUPPORTED;goto done;}
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
#else
  s=FreeDiscoveryStorage(table->BootServices,&storage);
  if(s!=EFI_SUCCESS)return s;
  s=GuInit(table->BootServices,io);
  if(s!=EFI_SUCCESS)return s;
#endif
  ConfigureBoundPlatform(image,&selected,config,&caps);
  return EFI_SUCCESS;
done:
  {
    EFI_STATUS released=FreeDiscoveryStorage(table->BootServices,&storage);
    return released==EFI_SUCCESS?s:released;
  }
}
