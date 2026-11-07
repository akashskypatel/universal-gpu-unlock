/* SPDX-License-Identifier: GPL-2.0-only */
/* Supplementary, untrusted read-only PCI evidence, never an unlock predicate.
 * Adapted from Windows-GPU-UNLOCK internal-boot/boot/diagnostic.c:
 * AllHandles -> HandleProtocol -> GetLocation -> Pci.Read -> owner records.
 * No PCI/ROM/MMIO write, driver connection/disconnection, or NVRAM service.
 */
#pragma once
#ifdef GU_PCI_OBSERVATION_HOST_TEST
#include "../tests/gpu_pci_observation_mock.h"
#else
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#endif

#define GU_PCI_OBS_MAX_HANDLES 4096U
#define GU_PCI_OBS_MAX_RECORDS 96U
#define GU_PCI_OBS_PATH_BYTES 192U
#define GU_PCI_OBS_MAX_OPENERS 512U
typedef struct {
  UINTN index,segment,bus,device,function;
  UINT32 config[16];             /* 64 bytes; no GPU register reads. */
  UINT8 path[GU_PCI_OBS_PATH_BYTES];
  UINTN path_bytes;
  UINTN by_driver;
  EFI_HANDLE first_agent;
  UINT8 location_ok,path_ok,owners_known;
} GU_PCI_OBS_RECORD;

typedef enum {
  GuPciObsNone=0,GuPciObsLocate=1,GuPciObsCount=2,
  GuPciObsInterface=3,GuPciObsRead=4,GuPciObsLocation=5,
  GuPciObsPath=6,GuPciObsOwners=7,GuPciObsFree=8
} GU_PCI_OBS_STAGE;

typedef struct {
  UINTN handle_count,pci_protocol_count,relevant_count,record_count,failures;
  UINTN first_failure_index;
  EFI_STATUS locate_status,first_failure_status,free_status;
  GU_PCI_OBS_STAGE first_failure_stage;
  UINT8 truncated;
  GU_PCI_OBS_RECORD records[GU_PCI_OBS_MAX_RECORDS];
} GU_PCI_OBS_CAPTURE;

/* Return 1 only for a complete bounded supplementary observation. Even then
 * it is NOT a validated topology snapshot and cannot authorize GPU writes.
 * Return 0 on any partial/failure, preserving records and the first stage.
 * Caller provides a zero-initialized capture with stable storage. */
int GuPciObserveReadOnly(EFI_BOOT_SERVICES *boot, GU_PCI_OBS_CAPTURE *out);
