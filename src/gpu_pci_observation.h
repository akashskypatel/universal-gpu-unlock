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
  const VOID *address; /* Diagnostic evidence only; never dereference or retry. */
  UINTN bytes;         /* Caller-declared extent; zero means unknown. */
  EFI_STATUS status;
  UINTN index;
  GU_PCI_OBS_STAGE stage;
} GU_PCI_OBS_POOL_FAILURE;

typedef struct {
  UINTN handle_count,pci_protocol_count,relevant_count,record_count,failures;
  UINTN first_failure_index;
  EFI_STATUS locate_status,first_failure_status,free_status;
  GU_PCI_OBS_STAGE first_failure_stage;
  UINT8 truncated;
  /* Exact EFI_SUCCESS is the only confirmed result. A UEFI warning is a
   * success code without confirmed completion: it is counted here and the
   * warned value is never published as observed evidence. */
  UINTN warnings,first_warning_index;
  EFI_STATUS first_warning_status;
  GU_PCI_OBS_STAGE first_warning_stage;
  /* At most one uncertain nested buffer and one outer buffer per capture.
   * A repeated or capture-overlapping reference is counted, never published. */
  UINTN uncertain_pool_count,uncertain_duplicate,uncertain_alias;
  GU_PCI_OBS_POOL_FAILURE uncertain_pool[2];
  GU_PCI_OBS_RECORD records[GU_PCI_OBS_MAX_RECORDS];
} GU_PCI_OBS_CAPTURE;

/* Return 1 only for a complete bounded supplementary observation. Even then
 * it is NOT a validated topology snapshot and cannot authorize GPU writes.
 * Return 0 on any partial/failure, preserving records and the first stage.
 * A UEFI warning result is not completion: it fails closed and is reported
 * separately from an error. A pool reference that repeats an already
 * uncertain address, or that overlaps this capture, is never published,
 * dereferenced, freed or retried.
 * Caller provides a zero-initialized capture with stable storage. */
int GuPciObserveReadOnly(EFI_BOOT_SERVICES *boot, GU_PCI_OBS_CAPTURE *out);
