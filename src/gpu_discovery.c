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

EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  UINTN seg, bus, dev, fun;
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
  s = table->BootServices->HandleProtocol((EFI_HANDLE)selected->handle,
                                            &PciGuid, (VOID **)&io);
  if (EFI_ERROR(s) || io != selected->pci || !io ||
      !io->GetLocation || !io->Pci.Read) return EFI_UNSUPPORTED;
  s = io->GetLocation(io, &seg, &bus, &dev, &fun);
  if (EFI_ERROR(s) || seg != selected->segment || bus != selected->bus ||
      dev != selected->device || fun != selected->function) return EFI_UNSUPPORTED;
  s = io->Pci.Read(io, EfiPciIoWidthUint32, 0, 64, config);
  if (EFI_ERROR(s) || config[0] != selected->pci_id ||
      config[0x2c / 4] != selected->subsystem_id ||
      (config[2] >> 16) != selected->class_code ||
      !GuCheckConfig(config, seg, bus, dev, fun, &caps)) return EFI_UNSUPPORTED;

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
