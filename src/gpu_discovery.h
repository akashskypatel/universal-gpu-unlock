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

/* Bounded compatibility API for existing callers and host fixtures. The
 * actual boot discovery path uses runtime-sized pool-backed views instead.
 * Identity references are valid only within the current boot. */
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

/* Publish selected targets only after an exact current NVIDIA inventory
 * re-enumeration plus two bounded PCI snapshots of every saved device,
 * including non-targets. Reject added/missing/rebound cards, saved aliases
 * and non-target drift. No atomic state or mutation permission is implied. */
typedef struct {
  EFI_PCI_IO_PROTOCOL *pci;
  UINT32 config[64];
} GU_GPU_REFRESHED_TARGET;
typedef struct {
  size_t count;
  GU_GPU_REFRESHED_TARGET targets[GU_INVENTORY_MAX_NVIDIA];
} GU_GPU_REFRESHED_SEQUENCE;
EFI_STATUS GuRefreshPreparedSequence(EFI_SYSTEM_TABLE *table,
                                     const GU_GPU_PREPARED_SEQUENCE *prepared,
                                     GU_GPU_REFRESHED_SEQUENCE *refreshed);

/* Legacy active transaction supports only one NVIDIA display and never
 * treats a read-only selection as permission for multi-GPU actuation. */
EFI_STATUS GuDiscoverSingleGpu(EFI_HANDLE image, EFI_SYSTEM_TABLE *table);

/* Exact selected-card mode: verify PCI identities and compare peer config
 * twice against the ordered execution record. Include a completed target.
 * In default firmware modes this is a no-op. */
EFI_STATUS GuVerifyActivePeers(EFI_SYSTEM_TABLE *table);
int GuIsPreparedPeerHandle(EFI_HANDLE handle);
/* True only for a verified, currently unselected record in the active inventory. */
int GuIsUnselectedGpuHandle(EFI_HANDLE handle);

/* One selected active card. Binds a genuine ordered-state record around
 * the existing shared GuExecuteUnlock sequence, without permitting a
 * second active card or authorizing firmware work. */
#ifdef GU_RUNTIME_SELECTION
/* Stop/release bench policy: select every currently detected supported NVIDIA
 * function in memory, without persisting GuTargets to NVRAM. */
EFI_STATUS GuRuntimeSelectionSelectAll(VOID);
#endif

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Runtime identity list for the warning shown before an ordered actuation. */
size_t GuSelectedGpuCount(VOID);
#ifdef GU_FIRMWARE_REATTACH
size_t GuPreparedGpuCount(VOID);
EFI_STATUS GuPreparedGpuRecordAt(size_t index,GU_GPU_RECORD *out);
#endif
EFI_STATUS GuSelectedGpuRecordAt(size_t index,GU_GPU_RECORD *out);
EFI_STATUS GuBeginSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                  const void *platform,
                                  const void *release_plan,
                                  const void *firmware_assets);
EFI_STATUS GuFinishSelectedGpuStep(EFI_SYSTEM_TABLE *table,
                                   int cleanup_and_handover_verified,
                                   const void *platform,
                                   const void *release_plan,
                                   const void *firmware_assets);
/* CPU context selection after a verified previous transaction.
 * Admission remains independent of this ordered execution mechanism. */
EFI_STATUS GuSelectedSequenceHasNext(EFI_SYSTEM_TABLE *table,int *more);
EFI_STATUS GuSelectNextGpu(EFI_SYSTEM_TABLE *table);
EFI_STATUS GuVerifySelectedSequence(EFI_SYSTEM_TABLE *table);
void GuAbortSelectedGpuStep(void);
#endif
