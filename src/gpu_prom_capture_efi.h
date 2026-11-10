/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#if !defined(GU_GPU_DIAGNOSTIC_ONLY) || defined(GU_AD104_TRANSACTION) || defined(GU_FULL_UNLOCK)
#error The PROM EFI adapter is diagnostic-only and must never link to an active unlock
#endif
#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include "gpu_discovery.h"
#include "gpu_prom_capture.h"

/* Diagnostic-only source adapter, NOT wired to any shipping EFI entrypoint.
 * Call only after independent target-specific proof that BAR0 PROM reads
 * have safe semantics. PCI admission does NOT establish those semantics. */
EFI_STATUS GuPromCapturePrepared(EFI_SYSTEM_TABLE *table,
    const GU_GPU_PREPARED_SEQUENCE *prepared, size_t selected_step,
    UINT8 *output, UINTN output_size, UINT8 digest[32]);

/* Persist the already complete capture to a target-specific name on a caller
 * provided, independently reviewed EFI volume. A pre-existence check is NOT
 * an atomic exclusive-create guarantee. No existing EFI entrypoint calls it. */
EFI_STATUS GuPromSaveNew(EFI_FILE_PROTOCOL *root, const GU_GPU_RECORD *target,
    const UINT8 *output, UINTN output_size);
