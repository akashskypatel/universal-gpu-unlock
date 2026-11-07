/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#ifdef GU_WINDOWS_HANDOFF_HOST_TEST
#include "../tests/gpu_windows_handoff_mock.h"
#else
#include <Uefi.h>
#endif

/* Only called after passive capture has been logged and file handles closed.
 * Uses a build-time pinned Windows GPT ESP; never writes NVRAM or GPU state. */
EFI_STATUS GuWindowsBootManager(EFI_HANDLE image, EFI_SYSTEM_TABLE *table);
