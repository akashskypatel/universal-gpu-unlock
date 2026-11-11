/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
/* Shared selected-target ownership, firmware, and pre-Windows handover.
 * The caller must run exact-card discovery, pinned asset load and cold profile. */
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage);
/* Optional persistence; never a substitute for verified active handover. */
EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *table);
