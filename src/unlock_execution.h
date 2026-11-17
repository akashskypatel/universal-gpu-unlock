/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
/* Shared selected-target ownership, firmware, and pre-Windows handover.
 * The caller must run exact-card discovery, pinned asset load and cold profile. */
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage);
typedef enum { GU_S3_DISABLED, GU_S3_REQUIRED, GU_S3_OPTIONAL } GU_S3_POLICY;
/* Prepare the active context before its transaction. */
EFI_STATUS GuPrepareBoundGpu(const char **stage);
/* The first context must already be prepared. Complete every ordered step
 * and its S3 policy, then check aggregate handover before returning. */
EFI_STATUS GuExecutePreparedSequence(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                                     GU_S3_POLICY policy,const char **stage);
/* Failure decision includes earlier published contexts. */
BOOLEAN GuExecutionNeedsContainment(VOID);
BOOLEAN GuExecutionReadyForWindows(EFI_SYSTEM_TABLE *table);
/* Optional persistence; never a substitute for verified active handover. */
EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *table);
