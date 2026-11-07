/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef unsigned char UINT8;
typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;
typedef void VOID;
typedef struct {uint32_t Data1;uint16_t Data2,Data3;uint8_t Data4[8];} EFI_GUID;
typedef struct {UINT8 Type,SubType,Length[2];} EFI_DEVICE_PATH_PROTOCOL;
typedef struct {EFI_HANDLE DeviceHandle;} EFI_LOADED_IMAGE_PROTOCOL;
#define EFI_LOADED_IMAGE_PROTOCOL_GUID {1,2,3,{4,5,6,7,8,9,10,11}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID {12,13,14,{4,5,6,7,8,9,10,11}}
#define EFI_DEVICE_PATH_PROTOCOL_GUID {15,16,17,{4,5,6,7,8,9,10,11}}
#define EFIERR(n) ((uint64_t)0x8000000000000000ULL|(uint64_t)(n))
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER EFIERR(2)
#define EFI_UNSUPPORTED EFIERR(3)
#define EFI_BAD_BUFFER_SIZE EFIERR(4)
#define EFI_OUT_OF_RESOURCES EFIERR(9)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_COMPROMISED_DATA EFIERR(33)
#define EFI_ACCESS_DENIED EFIERR(15)
#define EFI_ERROR(s) (((s)&(uint64_t)0x8000000000000000ULL)!=0)
#define FALSE 0
#define ByProtocol 2
#define EfiBootServicesData 4
#define MEDIA_DEVICE_PATH 4
#define MEDIA_FILEPATH_DP 4
#define END_DEVICE_PATH_TYPE 0x7f
#define END_ENTIRE_DEVICE_PATH_SUBTYPE 0xff
#define EFIAPI

typedef struct EFI_BOOT_SERVICES_S {
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);
  EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID *,VOID *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*FreePool)(VOID *);
  EFI_STATUS (*AllocatePool)(int,UINTN,VOID **);
  EFI_STATUS (*LoadImage)(BOOLEAN,EFI_HANDLE,EFI_DEVICE_PATH_PROTOCOL *,VOID *,UINTN,EFI_HANDLE *);
  EFI_STATUS (*StartImage)(EFI_HANDLE,UINTN *,CHAR16 **);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
