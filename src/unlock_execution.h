/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
/* Shared selected-target ownership, firmware, and pre-Windows handover.
 * The caller must run exact-card discovery, pinned asset load and cold profile. */
#ifndef GU_STOP_RELEASE_ONLY_BENCH
EFI_STATUS GuExecuteUnlock(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table,
                           EFI_HANDLE image,
                           const char **stage);
#else
/* Sequential driver Stop/release/reconnect only; no vendor unlock call. */
EFI_STATUS GuExecuteStopReleaseSequence(GU_PLATFORM *owner,
                           EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage);
/* True only after an actual reattachment attempt failed. */
BOOLEAN GuStopReleaseReattachFailed(VOID);
#endif
typedef enum { GU_S3_DISABLED, GU_S3_REQUIRED, GU_S3_OPTIONAL } GU_S3_POLICY;
/* Prepare the active context before its transaction. */
EFI_STATUS GuPrepareBoundGpu(GU_PLATFORM *owner,const char **stage);
#ifndef GU_STOP_RELEASE_ONLY_BENCH
/* The first context must already be prepared. Complete every ordered step
 * and its S3 policy, then check aggregate handover before returning. */
EFI_STATUS GuExecutePreparedSequence(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table,
                           EFI_HANDLE image,
                                     GU_S3_POLICY policy,const char **stage);
/* Resolve the final owner and cancel the watchdog before log closure. */
EFI_STATUS GuCompletePreparedSequence(EFI_STATUS status,GU_PLATFORM **owner);
#endif
/* Failure decision includes earlier published contexts. */
BOOLEAN GuExecutionNeedsContainment(VOID);
#ifndef GU_STOP_RELEASE_ONLY_BENCH
BOOLEAN GuExecutionReadyForWindows(EFI_SYSTEM_TABLE *table);
/* Optional persistence; never a substitute for verified active handover. */
EFI_STATUS GuRegisterS3(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table);
#endif
