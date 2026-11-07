/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t UINT8,BOOLEAN;
typedef uint32_t UINT32;
typedef uint64_t UINTN,EFI_STATUS;
typedef void VOID;
typedef void *EFI_HANDLE;
typedef struct {UINT32 Data1;uint16_t Data2,Data3;UINT8 Data4[8];} EFI_GUID;
typedef struct {UINT8 Type,SubType,Length[2];} EFI_DEVICE_PATH_PROTOCOL;
typedef struct EFI_PCI_IO_PROTOCOL_S EFI_PCI_IO_PROTOCOL;
typedef EFI_STATUS (*GU_PCI_READ)(EFI_PCI_IO_PROTOCOL *,int,UINT32,UINTN,VOID *);
typedef EFI_STATUS (*GU_PCI_LOCATION)(EFI_PCI_IO_PROTOCOL *,UINTN *,UINTN *,UINTN *,UINTN *);
struct EFI_PCI_IO_PROTOCOL_S {
  struct {GU_PCI_READ Read;} Pci;
  GU_PCI_LOCATION GetLocation;
  unsigned index;
};
typedef struct {
  EFI_HANDLE AgentHandle,ControllerHandle;
  UINT32 Attributes,OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID *,VOID *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
  EFI_STATUS (*OpenProtocolInformation)(EFI_HANDLE,EFI_GUID *,
                                        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **,UINTN *);
} EFI_BOOT_SERVICES;
#define EFI_PCI_IO_PROTOCOL_GUID {1,0,0,{0}}
#define EFI_DEVICE_PATH_PROTOCOL_GUID {2,0,0,{0}}
#define EfiPciIoWidthUint32 2
#define AllHandles 0
#define EFI_OPEN_PROTOCOL_BY_DRIVER 0x10U
#define EFIERR(n) (0x8000000000000000ULL | (uint64_t)(n))
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED EFIERR(3)
#define EFI_BAD_BUFFER_SIZE EFIERR(4)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_COMPROMISED_DATA EFIERR(33)
#define EFI_DEVICE_ERROR EFIERR(7)
#define EFI_ERROR(s) (!!((uint64_t)(s) & 0x8000000000000000ULL))
