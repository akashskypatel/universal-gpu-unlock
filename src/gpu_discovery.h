/* SPDX-License-Identifier: GPL-2.0-only */
/* Active-entrypoint target preparation: PCI reads only; no GuInit or writes. */
#pragma once
#ifdef GU_DISCOVERY_HOST_TEST
#include "../tests/gpu_discovery_mock.h"
#else
#include <Uefi.h>
#include <Protocol/PciIo.h>
#endif
#include "gpu_inventory.h"

/* Identity/handle references are valid only within the current boot and are
 * revalidated immediately before any later use. No mutation is authorized. */
typedef struct {
  GU_GPU_INVENTORY inventory;
  size_t selected_count;
  size_t selected_indices[GU_INVENTORY_MAX_NVIDIA];
} GU_GPU_PREPARED_SEQUENCE;

EFI_STATUS GuPrepareGpuSequence(EFI_SYSTEM_TABLE *table,
                                const GU_GPU_LOCATION *requested,
                                size_t requested_count,
                                GU_GPU_PREPARED_SEQUENCE *prepared);

/* Revalidate one selected boot-local PCI identity against the same handle,
 * protocol instance, BDF, vendor/device, class, and subsystem. A 256-byte
 * config snapshot and protocol pointer are output only on full success.
 * This does not refresh GOP/console ownership or permit GPU mutation. */
EFI_STATUS GuRefreshPreparedGpu(EFI_SYSTEM_TABLE *table,
                                const GU_GPU_PREPARED_SEQUENCE *prepared,
                                size_t step, UINT32 config[64],
                                EFI_PCI_IO_PROTOCOL **pci);

/* Legacy active transaction supports only one NVIDIA display and never
 * treats a read-only selection as permission for multi-GPU actuation. */
EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table);
