/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuMsiRelease(VOID);
BOOLEAN GuMsiReleased(VOID);
EFI_STATUS GuMsiFinish(VOID);
BOOLEAN GuMsiReadyForWindows(VOID);
BOOLEAN GuMsiMutated(VOID);
/* Stable identity of the selected platform's release state, or NULL. */
const VOID *GuMsiPlanOwnerToken(VOID);
/* Reserve release-plan and graph memory for the current platform.
 * Allocation is CPU-only and does not remove one-shot driver Stop guards. */
EFI_STATUS GuMsiReserveSelectedState(EFI_BOOT_SERVICES *bs);
