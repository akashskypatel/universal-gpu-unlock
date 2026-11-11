/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>

EFI_STATUS GuInstallDriverOption(EFI_HANDLE image, EFI_SYSTEM_TABLE *t);
EFI_STATUS GuRemoveDriverOption(EFI_SYSTEM_TABLE *t);
/* Remove only verified owned slots from DriverOrder; retain Driver#### entries. */
EFI_STATUS GuDisableDriverOption(EFI_SYSTEM_TABLE *t);
