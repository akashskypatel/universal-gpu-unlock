/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuMsiRelease(VOID);
BOOLEAN GuMsiReleased(VOID);
EFI_STATUS GuMsiFinish(VOID);
BOOLEAN GuMsiReadyForWindows(VOID);
BOOLEAN GuMsiMutated(VOID);
/* Validated identity of the selected platform's release state, or NULL.
 * An ownership mismatch permanently blocks the affected platform. */
const VOID *GuMsiPlanOwnerToken(VOID);
/* Reserve the release context for the current platform. Graph snapshots
 * allocate their own CPU-only memory without changing driver Stop guards. */
EFI_STATUS GuMsiReserveSelectedState(EFI_BOOT_SERVICES *bs);
