/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#ifdef GU_WINDOWS_HANDOFF_HOST_TEST
#include "../tests/gpu_windows_handoff_mock.h"
#else
#include <Uefi.h>
#endif

/* The Windows ESP is pinned at build time. Prepare loads the verified
 * boot image without running it; the caller may then perform an isolated
 * firmware operation and check the result before starting the image.
 * No BootOrder, BootNext, reset, or GPU configuration changes here. */
EFI_STATUS GuWindowsBootManagerPrepare(EFI_HANDLE image,
                                        EFI_SYSTEM_TABLE *table,
                                        EFI_HANDLE *prepared);
EFI_STATUS GuWindowsBootManagerStartPrepared(EFI_HANDLE prepared,
                                              EFI_SYSTEM_TABLE *table);

/* Existing passive-diagnostic convenience entrypoint. */
EFI_STATUS GuWindowsBootManager(EFI_HANDLE image, EFI_SYSTEM_TABLE *table);
