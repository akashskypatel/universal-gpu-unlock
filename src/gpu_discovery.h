/* SPDX-License-Identifier: GPL-2.0-only */
/* Active-entrypoint target preparation: PCI reads only; no GuInit or writes. */
#pragma once
#ifdef GU_DISCOVERY_HOST_TEST
#include "../tests/gpu_discovery_mock.h"
#else
#include <Uefi.h>
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

/* Legacy active transaction supports only one NVIDIA display and never
 * treats a read-only selection as permission for multi-GPU actuation. */
EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table);
