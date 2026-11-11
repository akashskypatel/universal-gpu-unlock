/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_discovery.h"
#include "gpu_execution_state.h"
#include <string.h>
#ifndef GU_DISCOVERY_HOST_TEST
#include "port.h"
#include "checks.h"
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
#if GU_GPU_SELECTION_COUNT < 1 || GU_GPU_SELECTION_COUNT > GU_INVENTORY_MAX_NVIDIA
#error Invalid active target count
#endif
#if defined(GU_ACTIVE_SINGLE_TARGET) && GU_GPU_SELECTION_COUNT != 1
#error One-target active execution requires exactly one PCI address
#endif
static const GU_GPU_LOCATION ActiveRequested[GU_GPU_SELECTION_COUNT] =
    GU_GPU_SELECTION_LOCATIONS;
#endif

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
#ifdef GU_ACTIVE_SINGLE_TARGET
static GU_GPU_PREPARED_SEQUENCE ActiveInventory;
static UINT32 ActiveInitialConfig[GU_INVENTORY_MAX_NVIDIA][64];
static size_t ActiveTargetIndex;
static int ActiveInventoryValid;
/* This one-target runtime now publishes its status into the same ordered
 * per-GPU state model used by read-only multi-card preparation. It still
 * cannot bind a second active Gu/MSI/assets context in this EFI image. */
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

/* Shared active/DXE read-only preparation. In the absence of an explicit
 * request, multiple NVIDIA GPUs are enumerated but not implicitly selected.
 * There is intentionally no call to GuInit, no PCI configuration writes,
 * no driver binding changes, no BAR access and no GPU reset here. */
EFI_STATUS GuPrepareGpuSequence(EFI_SYSTEM_TABLE *table,
                                const GU_GPU_LOCATION *requested,
                                size_t requested_count,
                                GU_GPU_PREPARED_SEQUENCE *prepared) {
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;
  GU_PCI_READER_CONTEXT context;
  GU_GPU_INVENTORY inventory;
  size_t i;
  EFI_STATUS status, released;
  int result;
  if (!prepared) return EFI_INVALID_PARAMETER;
  memset(prepared, 0, sizeof(*prepared));
  if (!table || !table->BootServices ||
      !table->BootServices->LocateHandleBuffer ||
      !table->BootServices->HandleProtocol ||
      !table->BootServices->FreePool ||
      requested_count > GU_INVENTORY_MAX_NVIDIA ||
      ((requested_count != 0) != (requested != NULL)))
    return EFI_INVALID_PARAMETER;

  status = table->BootServices->LocateHandleBuffer(ByProtocol, &PciGuid,
                                                    NULL, &count, &handles);
  if (EFI_ERROR(status)) return EFI_UNSUPPORTED;
  if (!handles || !count || count > GU_INVENTORY_MAX_HANDLES) {
    if (handles) (VOID)table->BootServices->FreePool(handles);
    return EFI_UNSUPPORTED;
  }
  context.services = table->BootServices;
  context.handles = handles;
  result = GuCollectGpuInventory((size_t)count, ReadPciIdentity, &context,
                                 &inventory);
  released = table->BootServices->FreePool(handles);
  if (result != GU_INVENTORY_OK || EFI_ERROR(released)) {
    GuLog("ACTIVE_PREPARE=REFUSED INVENTORY_CODE=%d FREE=%#llx\n",
          result, (unsigned long long)released);
    return EFI_UNSUPPORTED;
  }

  GuLog("ACTIVE_PREPARE=INVENTORIED NVIDIA_COUNT=%zu REQUESTED=%zu READ_ONLY=1\n",
        inventory.count, requested_count);
  for (i = 0; i < inventory.count; i++) {
    const GU_GPU_RECORD *p = &inventory.nvidia[i];
    GuLog("ACTIVE_GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x LEGACY_UNLOCK_DEVICE_ID=%u\n",
          i, (unsigned)p->segment, (unsigned)p->bus, (unsigned)p->device,
          (unsigned)p->function, (unsigned)(p->pci_id & 65535U),
          (unsigned)(p->pci_id >> 16), (unsigned)(p->subsystem_id & 65535U),
          (unsigned)(p->subsystem_id >> 16), (unsigned)p->class_code,
          (unsigned)GuIsSupportedPciId(p->pci_id));
  }
  if (requested_count != 0) {
    result = GuFindGpuSequence(&inventory, requested, requested_count,
                               prepared->selected_indices);
    if (result != GU_INVENTORY_OK) {
      GuLog("ACTIVE_PREPARE=REFUSED SELECTION_CODE=%d REQUESTED=%zu\n",
            result, requested_count);
      return EFI_UNSUPPORTED;
    }
    prepared->selected_count = requested_count;
  } else if (inventory.count == 1) {
    prepared->selected_count = 1;
    prepared->selected_indices[0] = 0;
  }
  prepared->inventory = inventory;
  for (i = 0; i < prepared->selected_count; i++) {
    const GU_GPU_RECORD *p = &prepared->inventory.nvidia[prepared->selected_indices[i]];
    GuLog("ACTIVE_PREPARED_STEP[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x LEGACY_UNLOCK_DEVICE_ID=%u READ_ONLY=1\n",
          i, (unsigned)p->segment, (unsigned)p->bus, (unsigned)p->device,
          (unsigned)p->function, (unsigned)(p->pci_id & 65535U),
          (unsigned)(p->pci_id >> 16), (unsigned)(p->subsystem_id & 65535U),
          (unsigned)(p->subsystem_id >> 16), (unsigned)GuIsSupportedPciId(p->pci_id));
  }
  GuLog("ACTIVE_PREPARE=COMPLETE SELECTED=%zu NVIDIA_COUNT=%zu MUTATION_AUTHORIZED=0\n",
        prepared->selected_count, prepared->inventory.count);
  return EFI_SUCCESS;
}

/* Refresh a prepared target's complete PCI identity without touching Gu or
 * authorizing mutation. A saved EFI handle is only an observation from the
 * current boot: it must still resolve to the same PCI_IO instance and BDF.
 * The complete 256-byte config snapshot is returned only on success. */
EFI_STATUS GuRefreshPreparedGpu(EFI_SYSTEM_TABLE *table,
                                const GU_GPU_PREPARED_SEQUENCE *prepared,
                                size_t step, UINT32 config[64],
                                EFI_PCI_IO_PROTOCOL **pci) {
  EFI_PCI_IO_PROTOCOL *current = NULL;
  const GU_GPU_RECORD *expected;
  UINT32 snapshot[64];
  UINTN segment, bus, device, function;
  EFI_STATUS status;
  if (pci) *pci = NULL;
  if (config) memset(config, 0, sizeof(snapshot));
  if (!table || !table->BootServices ||
      !table->BootServices->HandleProtocol || !prepared || !config || !pci ||
      prepared->inventory.count == 0 ||
      prepared->inventory.count > GU_INVENTORY_MAX_NVIDIA ||
      prepared->selected_count > prepared->inventory.count ||
      step >= prepared->selected_count ||
      prepared->selected_indices[step] >= prepared->inventory.count)
    return EFI_INVALID_PARAMETER;

  expected = &prepared->inventory.nvidia[prepared->selected_indices[step]];
  if (!expected->handle || !expected->pci ||
      expected->segment > 0xffffU || expected->bus > 255U ||
      expected->device > 31U || expected->function > 7U)
    return EFI_UNSUPPORTED;
  status = table->BootServices->HandleProtocol((EFI_HANDLE)expected->handle,
                                                &PciGuid, (VOID **)&current);
  if (EFI_ERROR(status) || !current || current != expected->pci ||
      !current->GetLocation || !current->Pci.Read)
    return EFI_UNSUPPORTED;
  status = current->GetLocation(current, &segment, &bus, &device, &function);
  if (EFI_ERROR(status) || segment != expected->segment ||
      bus != expected->bus || device != expected->device ||
      function != expected->function)
    return EFI_UNSUPPORTED;
  status = current->Pci.Read(current, EfiPciIoWidthUint32, 0, 64, snapshot);
  if (EFI_ERROR(status) || snapshot[0] != expected->pci_id ||
      (UINT16)(snapshot[2] >> 16) != expected->class_code ||
      snapshot[0x2c / 4] != expected->subsystem_id)
    return EFI_UNSUPPORTED;
  memcpy(config, snapshot, sizeof(snapshot));
  *pci = current;
  return EFI_SUCCESS;
}

/* Publish only requested targets in order after read-only full-set
 * NVIDIA re-enumeration and two PCI snapshot passes on every present target
 * and non-target. Refuse added/missing/rebound cards and saved aliases.
 * No atomic hardware snapshot, ownership release or GPU mutation is implied
 * by a successful observation. */
EFI_STATUS GuRefreshPreparedSequence(EFI_SYSTEM_TABLE *table,
                                     const GU_GPU_PREPARED_SEQUENCE *prepared,
                                     GU_GPU_REFRESHED_SEQUENCE *refreshed) {
  GU_GPU_PREPARED_SEQUENCE all, live;
  GU_GPU_REFRESHED_TARGET scanned[GU_INVENTORY_MAX_NVIDIA];
  size_t i, j;
  EFI_STATUS status;
  if (!refreshed) return EFI_INVALID_PARAMETER;
  memset(refreshed, 0, sizeof(*refreshed));
  if (!table || !table->BootServices ||
      !table->BootServices->LocateHandleBuffer ||
      !table->BootServices->HandleProtocol ||
      !table->BootServices->FreePool ||
      !prepared || !prepared->inventory.count ||
      prepared->inventory.count > GU_INVENTORY_MAX_NVIDIA ||
      !prepared->selected_count ||
      prepared->selected_count > prepared->inventory.count)
    return EFI_INVALID_PARAMETER;

  /* Validate identity independence across the FULL saved inventory,
   * not just selected records. A stale/corrupted non-target must fail closed. */
  for (i = 0; i < prepared->inventory.count; ++i) {
    const GU_GPU_RECORD *record = &prepared->inventory.nvidia[i];
    if (!record->handle || !record->pci ||
        (record->pci_id & 0xffffU) != 0x10deU ||
        (record->class_code & 0xff00U) != 0x0300U ||
        record->segment > 0xffffU || record->bus > 255U ||
        record->device > 31U || record->function > 7U)
      return EFI_INVALID_PARAMETER;
    for (j = 0; j < i; ++j) {
      const GU_GPU_RECORD *prior = &prepared->inventory.nvidia[j];
      if (record->handle == prior->handle || record->pci == prior->pci ||
          (record->segment == prior->segment && record->bus == prior->bus &&
           record->device == prior->device &&
           record->function == prior->function))
        return EFI_INVALID_PARAMETER;
    }
  }
  for (i = 0; i < prepared->selected_count; ++i) {
    if (prepared->selected_indices[i] >= prepared->inventory.count)
      return EFI_INVALID_PARAMETER;
    for (j = 0; j < i; ++j)
      if (prepared->selected_indices[i] == prepared->selected_indices[j])
        return EFI_INVALID_PARAMETER;
  }

  /* A previously saved inventory must still enumerate the same complete
   * NVIDIA set. Matching by BDF permits UEFI handle enumeration reorder but
   * not an added, missing, replaced or re-bound NVIDIA controller.
   * This is a new read-only enumeration; it cannot eliminate later races. */
  status = GuPrepareGpuSequence(table, NULL, 0, &live);
  if (status != EFI_SUCCESS ||
      live.inventory.count != prepared->inventory.count)
    return EFI_UNSUPPORTED;
  for (i = 0; i < prepared->inventory.count; ++i) {
    const GU_GPU_RECORD *expected = &prepared->inventory.nvidia[i];
    size_t matches = 0;
    for (j = 0; j < live.inventory.count; ++j) {
      const GU_GPU_RECORD *observed = &live.inventory.nvidia[j];
      if (observed->segment != expected->segment ||
          observed->bus != expected->bus ||
          observed->device != expected->device ||
          observed->function != expected->function)
        continue;
      if (observed->handle != expected->handle ||
          observed->pci != expected->pci ||
          observed->pci_id != expected->pci_id ||
          observed->class_code != expected->class_code ||
          observed->subsystem_id != expected->subsystem_id)
        return EFI_UNSUPPORTED;
      ++matches;
    }
    if (matches != 1) return EFI_UNSUPPORTED;
  }
  /* Reuse the source-level single-record identity and PCI read contract. */
  all = *prepared;
  all.selected_count = all.inventory.count;
  for (i = 0; i < all.selected_count; ++i) all.selected_indices[i] = i;
  memset(scanned, 0, sizeof(scanned));
  for (i = 0; i < all.selected_count; ++i) {
    status = GuRefreshPreparedGpu(table, &all, i, scanned[i].config,
                                  &scanned[i].pci);
    if (status != EFI_SUCCESS) return status;
  }
  /* The second read also guards against changes to a non-target GPU after
   * its first snapshot. These observations remain non-atomic. */
  for (i = 0; i < all.selected_count; ++i) {
    UINT32 confirm[64];
    EFI_PCI_IO_PROTOCOL *io = NULL;
    status = GuRefreshPreparedGpu(table, &all, i, confirm, &io);
    if (status != EFI_SUCCESS || io != scanned[i].pci ||
        memcmp(confirm, scanned[i].config, sizeof(confirm)) != 0)
      return EFI_UNSUPPORTED;
  }
  /* No fallible steps remain after both reads; publish only now, without
   * a redundant 4-KiB EFI stack buffer. */
  for (i = 0; i < prepared->selected_count; ++i)
    refreshed->targets[i] = scanned[prepared->selected_indices[i]];
  refreshed->count = prepared->selected_count;
  return EFI_SUCCESS;
}

/* Revalidate the complete saved NVIDIA PCI set. The selected card may have
 * expected configuration changes during its own transaction. Every other
 * NVIDIA card must retain its complete initial 256-byte PCI configuration.
 * This function has no GPU writes, driver detach, DMA, or reset actions. */
int GuIsPreparedPeerHandle(EFI_HANDLE handle) {
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(!ActiveInventoryValid||!handle)return 0;
  for(size_t i=0;i<ActiveInventory.inventory.count;i++)
    if(i!=ActiveTargetIndex&&ActiveInventory.inventory.nvidia[i].handle==handle)
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
  GU_GPU_PREPARED_SEQUENCE live, all;
  size_t i,j;
  EFI_STATUS s;
  if(!ActiveInventoryValid || !table || !table->BootServices)
    return EFI_UNSUPPORTED;
  s=GuPrepareGpuSequence(table,NULL,0,&live);
  if(s!=EFI_SUCCESS || live.inventory.count!=ActiveInventory.inventory.count)
    return EFI_UNSUPPORTED;
  for(i=0;i<ActiveInventory.inventory.count;i++) {
    const GU_GPU_RECORD *expected=&ActiveInventory.inventory.nvidia[i];
    size_t matches=0;
    for(j=0;j<live.inventory.count;j++) {
      const GU_GPU_RECORD *observed=&live.inventory.nvidia[j];
      if(observed->segment!=expected->segment ||
         observed->bus!=expected->bus ||
         observed->device!=expected->device ||
         observed->function!=expected->function)continue;
      if(observed->handle!=expected->handle ||
         observed->pci!=expected->pci ||
         observed->pci_id!=expected->pci_id ||
         observed->class_code!=expected->class_code ||
         observed->subsystem_id!=expected->subsystem_id)return EFI_UNSUPPORTED;
      matches++;
    }
    if(matches!=1)return EFI_UNSUPPORTED;
  }
  all=ActiveInventory;
  all.selected_count=all.inventory.count;
  for(i=0;i<all.selected_count;i++)all.selected_indices[i]=i;
  for(unsigned pass=0;pass<2;pass++) {
    for(i=0;i<all.selected_count;i++) {
      UINT32 now[64];
      EFI_PCI_IO_PROTOCOL *io=NULL;
      s=GuRefreshPreparedGpu(table,&all,i,now,&io);
      if(s!=EFI_SUCCESS || io!=all.inventory.nvidia[i].pci)return EFI_UNSUPPORTED;
      if(i!=ActiveTargetIndex &&
         memcmp(now,ActiveInitialConfig[i],sizeof(now))!=0) {
        GuLog("ACTIVE_PEER=DRIFT INDEX=%zu PASS=%u REFUSED=1\n",i,pass);
        return EFI_UNSUPPORTED;
      }
    }
  }
  return EFI_SUCCESS;
#endif
}

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Re-enumerate and read the COMPLETE NVIDIA inventory twice. A later
 * selected-card step must not trust a previous firmware/PCI snapshot. */
static EFI_STATUS ActiveStepObservations(EFI_SYSTEM_TABLE *table,
                                       GU_GPU_EXEC_OBSERVATION *observed) {
  GU_GPU_PREPARED_SEQUENCE all;
  GU_GPU_REFRESHED_SEQUENCE refreshed;
  if(!ActiveInventoryValid || !ActiveSequenceValid || !table ||
     !table->BootServices || !observed)return EFI_INVALID_PARAMETER;
  all=ActiveInventory;
  all.selected_count=all.inventory.count;
  for(size_t i=0;i<all.selected_count;i++)all.selected_indices[i]=i;
  EFI_STATUS s=GuRefreshPreparedSequence(table,&all,&refreshed);
  if(s!=EFI_SUCCESS || refreshed.count!=all.inventory.count)
    return EFI_UNSUPPORTED;
  for(size_t i=0;i<all.inventory.count;i++) {
    observed[i].identity=all.inventory.nvidia[i];
    memcpy(observed[i].config,refreshed.targets[i].config,
           sizeof(observed[i].config));
  }
  return EFI_SUCCESS;
}
void GuAbortSelectedGpuStep(void) {
  if(!ActiveSequenceValid)return;
  GuGpuExecAbort(&ActiveSequence);
  GuLog("SELECTED_GPU_STEP=HALTED NEXT=%zu MUTATION_AUTHORIZED=0\\n",
        ActiveSequence.next_step);
}
EFI_STATUS GuBeginSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                  const void *platform,
                                  const void *release_plan,
                                  const void *firmware_assets) {
  if(!ActiveSequenceValid || !ActiveInventoryValid ||
     !table || !table->BootServices ||
     Gu.bs!=table->BootServices || !Gu.handle || !Gu.pci ||
     !Gu.profile_verified || !Gu.prom_verified ||
     Gu.started || Gu.fault || Gu.retained || Gu.owned || Gu.armed ||
     ActiveTargetIndex>=ActiveInventory.inventory.count ||
     Gu.handle!=ActiveInventory.inventory.nvidia[ActiveTargetIndex].handle ||
     Gu.pci!=ActiveInventory.inventory.nvidia[ActiveTargetIndex].pci)
    return EFI_NOT_READY;
  GU_GPU_EXEC_OBSERVATION observed[GU_INVENTORY_MAX_NVIDIA];
  EFI_STATUS s=ActiveStepObservations(table,observed);
  if(s!=EFI_SUCCESS ||
     GuGpuExecBeginOwned(&ActiveSequence,0,observed,
                         ActiveInventory.inventory.count,
                         platform,release_plan,firmware_assets)
       !=GU_GPU_EXEC_OK) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  GuLog("SELECTED_GPU_STEP=ACTIVE ORDER=0 TARGET=%zu PEERS=%zu\\n",
        ActiveTargetIndex,ActiveInventory.inventory.count);
  return EFI_SUCCESS;
}
EFI_STATUS GuFinishSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                   int cleanup_and_handover_verified,
                                   const void *platform,
                                   const void *release_plan,
                                   const void *firmware_assets) {
  if(!ActiveSequenceValid || !ActiveInventoryValid ||
     !cleanup_and_handover_verified || !table ||
     !table->BootServices || Gu.bs!=table->BootServices ||
     ActiveTargetIndex>=ActiveInventory.inventory.count ||
     ActiveTargetIndex>=ActiveSequence.count ||
     Gu.handle!=ActiveInventory.inventory.nvidia[ActiveTargetIndex].handle ||
     Gu.pci!=ActiveInventory.inventory.nvidia[ActiveTargetIndex].pci ||
     !platform || !release_plan || !firmware_assets ||
     ActiveSequence.targets[ActiveTargetIndex].owners.platform!=platform ||
     ActiveSequence.targets[ActiveTargetIndex].owners.release_plan!=release_plan ||
     ActiveSequence.targets[ActiveTargetIndex].owners.firmware_assets!=firmware_assets) {
    /* A post-transaction check must not trust CPU owner tokens alone:
     * compare them to the actual live resource objects again. */
    GuAbortSelectedGpuStep();
    return EFI_NOT_READY;
  }
  GU_GPU_EXEC_OBSERVATION observed[GU_INVENTORY_MAX_NVIDIA];
  EFI_STATUS s=ActiveStepObservations(table,observed);
  if(s!=EFI_SUCCESS ||
     GuGpuExecFinish(&ActiveSequence,0,observed,
                     ActiveInventory.inventory.count,1)!=GU_GPU_EXEC_OK) {
    GuAbortSelectedGpuStep();
    return EFI_UNSUPPORTED;
  }
  GuLog("SELECTED_GPU_STEP=COMPLETE NEXT=1 PEERS_VERIFIED=%zu\\n",
        ActiveInventory.inventory.count);
  return EFI_SUCCESS;
}

#endif

EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  GU_GPU_PREPARED_SEQUENCE prepared;
  const GU_GPU_RECORD *selected;
  EFI_PCI_IO_PROTOCOL *io;
  UINT32 config[64];
  GU_CAPS caps;
  EFI_STATUS s;
  if (!image || !table) return EFI_INVALID_PARAMETER;
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
  s = GuPrepareGpuSequence(table, ActiveRequested, GU_GPU_SELECTION_COUNT,
                           &prepared);
#else
  s = GuPrepareGpuSequence(table, NULL, 0, &prepared);
#endif
  if (s != EFI_SUCCESS) return s;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  {
    GU_GPU_PREPARED_SEQUENCE all=prepared;
    GU_GPU_REFRESHED_SEQUENCE refreshed;
    GU_GPU_EXEC_OBSERVATION observed[GU_INVENTORY_MAX_NVIDIA];
    GU_GPU_EXEC_SEQUENCE sequence;
    /* Track every NVIDIA peer, not only the requested targets.
     * Preparation never calls Begin or authorizes hardware mutation. */
    all.selected_count=all.inventory.count;
    for(size_t i=0;i<all.selected_count;i++)all.selected_indices[i]=i;
    s=GuRefreshPreparedSequence(table,&all,&refreshed);
    if(s!=EFI_SUCCESS || refreshed.count!=all.selected_count) {
      GuLog("ACTIVE_REFRESH=REFUSED STATUS=%#llx BEFORE_GU_INIT=1\n",
            (unsigned long long)s);
      return EFI_UNSUPPORTED;
    }
    memset(observed,0,sizeof(observed));
    for(size_t i=0;i<all.inventory.count;i++) {
      observed[i].identity=all.inventory.nvidia[i];
      memcpy(observed[i].config,refreshed.targets[i].config,
             sizeof(observed[i].config));
    }
    if(GuGpuExecInit(&sequence,observed,all.inventory.count,
                     prepared.selected_indices,prepared.selected_count)!=GU_GPU_EXEC_OK ||
       GuGpuExecCheckNext(&sequence,observed,all.inventory.count)!=GU_GPU_EXEC_OK) {
      GuLog("ACTIVE_SEQUENCE=REFUSED REASON=STATE_OR_PEER_DRIFT BEFORE_GU_INIT=1\n");
      return EFI_UNSUPPORTED;
    }
    GuLog("ACTIVE_SEQUENCE=PREPARED_NOT_STARTED SELECTED=%zu TRACKED=%zu NEXT=0 MUTATION_AUTHORIZED=0\n",
          sequence.selected_count,sequence.count);
    GuLog("ACTIVE_REFRESH=PASSIVE_REVALIDATED TARGETS=%zu MUTATION_AUTHORIZED=0\n",
          refreshed.count);
  }
#endif
#ifndef GU_ACTIVE_SINGLE_TARGET
  if (prepared.inventory.count > 1) {
    GuLog("ACTIVE_EXECUTION=REFUSED REASON=MULTI_NVIDIA_GLOBAL_STATE NVIDIA_COUNT=%zu SELECTED=%zu BEFORE_GU_INIT=1\n",
          prepared.inventory.count, prepared.selected_count);
    return EFI_UNSUPPORTED; /* Legacy unselected execution must remain single-card. */
  }
  if (prepared.inventory.count != 1 || prepared.selected_count != 1 ||
      prepared.selected_indices[0] != 0)
    return EFI_UNSUPPORTED;
#else
  if(prepared.selected_count!=1 || prepared.inventory.count<1 ||
     prepared.selected_indices[0]>=prepared.inventory.count)
    return EFI_UNSUPPORTED;
#endif
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  GuLog("ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\n");
  return EFI_UNSUPPORTED;
#endif
  selected = &prepared.inventory.nvidia[prepared.selected_indices[0]];
  if (!GuIsSupportedPciId(selected->pci_id)) return EFI_UNSUPPORTED;
  s = GuRefreshPreparedGpu(table, &prepared, 0, config, &io);
  if (s != EFI_SUCCESS)return EFI_UNSUPPORTED;
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(!GuCheckConfigInitialMaster(config,selected->segment,selected->bus,
                                  selected->device,selected->function,&caps))
    return EFI_UNSUPPORTED;
#else
  if(!GuCheckConfig(config,selected->segment,selected->bus,
                    selected->device,selected->function,&caps))
    return EFI_UNSUPPORTED;
#endif

#ifdef GU_ACTIVE_SINGLE_TARGET
  /* The full inventory is re-enumerated, then read twice before publication.
   * Store original non-target configuration for later stage validation. */
  {
    GU_GPU_PREPARED_SEQUENCE all=prepared;
    GU_GPU_REFRESHED_SEQUENCE full;
    all.selected_count=all.inventory.count;
    for(size_t i=0;i<all.selected_count;i++)all.selected_indices[i]=i;
    s=GuRefreshPreparedSequence(table,&all,&full);
    if(s!=EFI_SUCCESS || full.count!=all.selected_count ||
       full.targets[prepared.selected_indices[0]].pci!=io ||
       memcmp(full.targets[prepared.selected_indices[0]].config,
              config,sizeof(config))!=0)return EFI_UNSUPPORTED;
    /* Build the checked one-target execution state BEFORE spending GuInit.
     * Failed binding must not overwrite prior target/peer state. */
    GU_GPU_EXEC_OBSERVATION observations[GU_INVENTORY_MAX_NVIDIA];
    GU_GPU_EXEC_SEQUENCE candidate;
    size_t chosen=prepared.selected_indices[0];
    memset(observations,0,sizeof(observations));
    for(size_t i=0;i<all.inventory.count;i++) {
      observations[i].identity=all.inventory.nvidia[i];
      memcpy(observations[i].config,full.targets[i].config,
             sizeof(observations[i].config));
    }
    if(GuGpuExecInit(&candidate,observations,all.inventory.count,
                     &chosen,1)!=GU_GPU_EXEC_OK ||
       GuGpuExecCheckNext(&candidate,observations,
                          all.inventory.count)!=GU_GPU_EXEC_OK)
      return EFI_UNSUPPORTED;
    s=GuInit(table->BootServices,io);
    if(s!=EFI_SUCCESS)return s;
    ActiveInventoryValid=0;
    ActiveSequenceValid=0;
    ActiveInventory=prepared;
    ActiveTargetIndex=chosen;
    for(size_t i=0;i<all.selected_count;i++)
      memcpy(ActiveInitialConfig[i],full.targets[i].config,sizeof(config));
    ActiveSequence=candidate;
    ActiveSequenceValid=1;
    ActiveInventoryValid=1;
  }
#else
  s=GuInit(table->BootServices,io);
  if(s!=EFI_SUCCESS)return s;
#endif
  Gu.image = image;
  Gu.handle = (EFI_HANDLE)selected->handle;
  Gu.device_id = (UINT16)(selected->pci_id >> 16);
  Gu.is_ampere = GuIsAmpereDeviceId(Gu.device_id);
  Gu.is_turing = GuIsTuringDeviceId(Gu.device_id);
  Gu.bar_base = (config[4] & ~0xfU);
  Gu.bar_size = 0x1000000;
  Gu.pm_cap = caps.pm;
  Gu.pcie_cap = caps.pcie;
  Gu.pcie_version = caps.pcie_version;
  Gu.msi_cap = caps.msi;
  Gu.msix_cap = caps.msix;
  return EFI_SUCCESS;
}
