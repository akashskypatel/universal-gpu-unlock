/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuBootOpen(EFI_HANDLE,EFI_SYSTEM_TABLE *);
EFI_STATUS GuBootLoadAssets(VOID);
EFI_STATUS GuBootLoadWindows(VOID);
EFI_STATUS GuBootChoose(CHAR16 *);
EFI_STATUS GuBootWindows(VOID);
EFI_STATUS GuBootClose(VOID);
VOID GuBootSay(const CHAR16 *);
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
