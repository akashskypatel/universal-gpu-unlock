/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuBootOpen(EFI_HANDLE,EFI_SYSTEM_TABLE *);
EFI_STATUS GuBootLoadAssets(VOID);
EFI_STATUS GuBootLoadWindows(VOID);
EFI_STATUS GuBootChoose(CHAR16 *);
EFI_STATUS GuBootWindows(VOID);
EFI_STATUS GuBootClose(VOID);

/* One stage, named twice: `*stage` is what a refusal prints at the end and the
 * trace is what a stall prints at the moment it stalls. The macro expands inside
 * a function that owns a `const char **stage`, and it lives here so a harness
 * that splices one function out of its file still sees the same definition. */
#define GU_PHASE(NAME) do { *stage=(NAME); GuTracePhase(NAME); } while(0)
/* The trace counter starts at the first stage that can block, so `STEP=` is
 * comparable between logs instead of counting from process start. */
#define GU_TRACE_BEGIN() GuTraceReset()

/* The bounded firmware timer. It is armed once, in GuBootOpen, for a build with
 * GU_WATCHDOG_SECONDS!=0, and cancelled before the handover. Both are inert in
 * the default build, which disables the timer exactly as before. */
BOOLEAN GuBootWatchdogArmed(UINTN *seconds);
EFI_STATUS GuBootWatchdogCancel(VOID);
VOID GuBootSay(const CHAR16 *);
#ifdef GU_STOP_RELEASE_ONLY_BENCH
/* Emit the one pre-actuation warning directly to the firmware's configured
 * ConOut and require a successful three-second stall before any GPU access. */
EFI_STATUS GuBootBenchCountdown(VOID);
EFI_STATUS GuBootBenchReportCompletion(VOID);
#endif
VOID GuBootSilence(VOID);
VOID GuBootUnsilence(VOID);
VOID GuBootHalt(VOID);
/* v0.5.4: diagnostics log only while the USB log is open (GuLog faults otherwise). */
BOOLEAN GuBootLogOpen(VOID);
/* Read-only accessors. Target volume exists only after validated image staging. */
EFI_HANDLE GuBootGetTargetEspVolume(VOID);
const CHAR16 *GuBootGetLogPath(VOID);
/* Explicit asset-install build only (GU_ASSET_INSTALL_ONLY): copy the pinned
 * bundle for the selected card to the configured Windows ESP, verify every file
 * before and after writing, and record which bundle was staged. Refuses unless
 * the image was launched from removable media and no card is owned. This is the
 * only path in the image that writes to the ESP, and it does not change where
 * the image may be launched from. */
EFI_STATUS GuAssetInstallStage(VOID);
