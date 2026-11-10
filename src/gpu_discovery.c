/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_discovery.h"
#include <string.h>
#ifndef GU_DISCOVERY_HOST_TEST
#include "port.h"
#include "checks.h"
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) && GU_PREFLIGHT_ONLY != 1
#error Preparation-only active EFI must also prohibit GPU writes, DMA and FLR
#endif
#endif
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
#include "gpu-selection.h" /* Generated operator-supplied PCI addresses. */
#if GU_GPU_SELECTION_COUNT < 1 || GU_GPU_SELECTION_COUNT > GU_INVENTORY_MAX_NVIDIA
#error Invalid active preparation target count
#endif
static const GU_GPU_LOCATION ActiveRequested[GU_GPU_SELECTION_COUNT] =
    GU_GPU_SELECTION_LOCATIONS;
#endif

static EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;

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

/* All-or-none publication of separately revalidated read-only targets.
 * Observations are sequential; callers must revalidate before later use.
 * In particular this does NOT prove cross-GPU transaction isolation. */
EFI_STATUS GuRefreshPreparedSequence(EFI_SYSTEM_TABLE *table,
                                     const GU_GPU_PREPARED_SEQUENCE *prepared,
                                     GU_GPU_REFRESHED_SEQUENCE *refreshed) {
  GU_GPU_REFRESHED_SEQUENCE current;
  size_t i, j;
  EFI_STATUS status;
  if (!refreshed) return EFI_INVALID_PARAMETER;
  memset(refreshed, 0, sizeof(*refreshed));
  if (!prepared || !prepared->selected_count ||
      prepared->selected_count > GU_INVENTORY_MAX_NVIDIA ||
      prepared->selected_count > prepared->inventory.count)
    return EFI_INVALID_PARAMETER;
  for (i = 0; i < prepared->selected_count; ++i) {
    if (prepared->selected_indices[i] >= prepared->inventory.count)
      return EFI_INVALID_PARAMETER;
    for (j = 0; j < i; ++j)
      if (prepared->selected_indices[j] == prepared->selected_indices[i])
        return EFI_INVALID_PARAMETER;
  }
  memset(&current, 0, sizeof(current));
  for (i = 0; i < prepared->selected_count; ++i) {
    status = GuRefreshPreparedGpu(table, prepared, i,
                                  current.targets[i].config,
                                  &current.targets[i].pci);
    if (status != EFI_SUCCESS) return status;
  }
  current.count = prepared->selected_count;
  *refreshed = current;
  return EFI_SUCCESS;
}

EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  GU_GPU_PREPARED_SEQUENCE prepared;
  const GU_GPU_RECORD *selected;
  EFI_PCI_IO_PROTOCOL *io;
  UINT32 config[64];
  GU_CAPS caps;
  EFI_STATUS s;
  if (!image || !table) return EFI_INVALID_PARAMETER;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  s = GuPrepareGpuSequence(table, ActiveRequested, GU_GPU_SELECTION_COUNT,
                           &prepared);
#else
  s = GuPrepareGpuSequence(table, NULL, 0, &prepared);
#endif
  if (s != EFI_SUCCESS) return s;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  {
    GU_GPU_REFRESHED_SEQUENCE refreshed;
    s = GuRefreshPreparedSequence(table, &prepared, &refreshed);
    if (s != EFI_SUCCESS) {
      GuLog("ACTIVE_REFRESH=REFUSED STATUS=%#llx BEFORE_GU_INIT=1\n",
            (unsigned long long)s);
      return s;
    }
    GuLog("ACTIVE_REFRESH=PASSIVE_REVALIDATED TARGETS=%zu MUTATION_AUTHORIZED=0\n",
          refreshed.count);
  }
#endif
  if (prepared.inventory.count > 1) {
    GuLog("ACTIVE_EXECUTION=REFUSED REASON=MULTI_NVIDIA_GLOBAL_STATE NVIDIA_COUNT=%zu SELECTED=%zu BEFORE_GU_INIT=1\n",
          prepared.inventory.count, prepared.selected_count);
    return EFI_UNSUPPORTED; /* Never reuse global Gu for multiple GPUs. */
  }
  if (prepared.inventory.count != 1 || prepared.selected_count != 1 ||
      prepared.selected_indices[0] != 0)
    return EFI_UNSUPPORTED;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  GuLog("ACTIVE_EXECUTION=REFUSED REASON=SELECTION_PREPARATION_ONLY BEFORE_GU_INIT=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\n");
  return EFI_UNSUPPORTED;
#endif
  selected = &prepared.inventory.nvidia[0];
  if (!GuIsSupportedPciId(selected->pci_id)) return EFI_UNSUPPORTED;
  s = GuRefreshPreparedGpu(table, &prepared, 0, config, &io);
  if (s != EFI_SUCCESS ||
      !GuCheckConfig(config, selected->segment, selected->bus,
                     selected->device, selected->function, &caps))
    return EFI_UNSUPPORTED;

  GuInit(table->BootServices, io);
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
