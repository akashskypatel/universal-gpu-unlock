/* SPDX-License-Identifier: GPL-2.0-only */
/* Persisted boot record: the only piece of state that survives a failed boot.
 *
 * This module keeps one fixed-size record in NVRAM:
 *
 *   phase = INTENT   written before the first GPU mutation, naming the card
 *   phase = OUTCOME  legacy preboot result after durable completion
 *
 * An explicitly waived active experiment retains INTENT even on successful
 * preboot completion. It is a one-use barrier until explicit owner recovery;
 * neither returning firmware control nor starting Windows clears it.
 *
 * An unmatched INTENT or a non-success OUTCOME is a recovery latch: the DXE
 * entrypoint skips all GPU work and removes its owned Driver#### entry from
 * DriverOrder. This makes the next boot fail closed instead of repeating a
 * failed/interrupted transaction. The USB application recognizes the same latch before GPU discovery and removes
 * the owned NVRAM entry entirely, but preserves the failure record. Neither
 * path tries to repair GPU state or repeats a failed transaction.
 *
 * Explicit recovery may delete the record. An invalid or unreadable record
 * blocks active entry; it is never repaired in place.
 */
#pragma once

#if defined(GU_BOOT_STATE_HOST_TEST)
#include "../tests/boot_state_mock.h"
#else
#include <Uefi.h>
#endif

#define GU_BOOT_STATE_VARIABLE_NAME L"GuBootState"
#define GU_BOOT_STATE_MAGIC 0x31425547U /* "GUB1" little-endian */

#define GU_BOOT_STATE_IDLE 0U
#define GU_BOOT_STATE_INTENT 1U
#define GU_BOOT_STATE_OUTCOME 2U

typedef struct {
  UINT32 Magic;
  UINT32 BootCount;      /* increments once per boot that reaches this module */
  UINT32 Phase;          /* IDLE / INTENT / OUTCOME */
  UINT64 Status;         /* EFI_STATUS of the completed (or attempted) sequence */
  UINT16 DeviceId;       /* PCI device id of the target, 0 when unknown */
  UINT16 Segment;
  UINT16 Bus;
  UINT8 Device;
  UINT8 Function;
  UINT8 Armed;           /* the sequence reported a completed transaction */
  UINT8 Reserved[3];     /* must be zero; a nonzero value means corruption */
} GU_BOOT_STATE;

/* Read the record. EFI_NOT_FOUND when absent; EFI_COMPROMISED_DATA when the
 * stored value fails validation. Active entry refuses an invalid record. */
EFI_STATUS GuBootStateRead(EFI_SYSTEM_TABLE *Table, GU_BOOT_STATE *Out);

/* Replace the record and verify the stored bytes and attributes. A successful
 * service return alone is not sufficient evidence of a restart barrier. */
EFI_STATUS GuBootStateWrite(EFI_SYSTEM_TABLE *Table, const GU_BOOT_STATE *State);
EFI_STATUS GuBootStateDelete(EFI_SYSTEM_TABLE *Table);

/* Build and verify one phase transition. Active entry requires confirmed
 * INTENT before mutation and a confirmed OUTCOME before normal handover. */
EFI_STATUS GuBootStateRecord(EFI_SYSTEM_TABLE *Table, const GU_BOOT_STATE *Previous,
                             UINT32 Phase, UINT64 Status, UINT16 DeviceId,
                             UINT16 Segment, UINT16 Bus, UINT8 Device, UINT8 Function,
                             UINT8 Armed);
