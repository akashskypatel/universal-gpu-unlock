/* SPDX-License-Identifier: GPL-2.0-only */
/* Host definitions for compiling the runtime target-selection unit only.
 * It reuses the discovery mock's basic EFI types and adds the two services the
 * selection module touches: variable storage and pool allocation. */
#pragma once

#include "gpu_discovery_mock.h"

#include <string.h>

typedef char CHAR8;
typedef unsigned char BOOLEAN;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* Distinct values are all the module needs: it only compares and forwards. */
#define EFI_BAD_BUFFER_SIZE 10
#define EFI_BUFFER_TOO_SMALL 11
#define EFI_NOT_FOUND 14

#define EFI_VARIABLE_NON_VOLATILE 0x00000001U
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002U
#define EFI_VARIABLE_RUNTIME_ACCESS 0x00000004U

struct MOCK_RUNTIME_SERVICES {
  EFI_STATUS (*GetVariable)(const void *Name, EFI_GUID *Vendor, UINT32 *Attributes,
                            UINTN *Size, VOID *Data);
  EFI_STATUS (*SetVariable)(const void *Name, EFI_GUID *Vendor, UINT32 Attributes,
                            UINTN Size, VOID *Data);
};
