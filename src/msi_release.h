/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuMsiRelease(VOID);
BOOLEAN GuMsiReleased(VOID);
EFI_STATUS GuMsiFinish(VOID);
BOOLEAN GuMsiReadyForWindows(VOID);
BOOLEAN GuMsiMutated(VOID);
/* Stable identity of this image's one-shot MSI release state. */
const VOID *GuMsiPlanOwnerToken(VOID);
/* Reserve distinct real release-plan/graph memory for the selected card.
 * Allocation is CPU-only and does not remove one-shot driver Stop guards. */
EFI_STATUS GuMsiReserveSelectedState(EFI_BOOT_SERVICES *bs);
