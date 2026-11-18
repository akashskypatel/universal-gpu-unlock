/* SPDX-License-Identifier: GPL-2.0-only */
/* Runtime target selection: which PCI functions this image may act on.
 *
 * The compiled-in gpu-selection.h list is a build-time constant, so a card
 * added to a slot, a BDF that moves after a firmware update, or a second
 * machine all require a rebuild. This module carries the same list as data:
 * an operator-readable UEFI variable ("GuTargets" under a fork-owned GUID)
 * whose text form is "SSSS:BB:DD.F[,SSSS:BB:DD.F...]" or "ALL".
 *
 * The variable decides *which* cards are candidates. It never decides
 * whether a card may be mutated: that stays with the build admission and the
 * ordered execution coordinator.
 */
#pragma once

#if defined(GU_SELECTION_HOST_TEST) || defined(GU_DISCOVERY_HOST_TEST)
/* Host fixtures compile the same source the firmware does, so they supply the
 * EFI types instead of the real headers. */
#include "../tests/gpu_selection_mock.h"
#else
#include <Uefi.h>
#endif
#include "gpu_inventory.h"

#define GU_SELECTION_MAX 128U
#define GU_SELECTION_MAX_TEXT 4096U

/* Fork-owned variable. Not in the global GUID namespace on purpose: the
 * firmware vendor cannot own it, and it is not shared with another tool. */
#define GU_SELECTION_VARIABLE_NAME L"GuTargets"

typedef struct {
  GU_GPU_LOCATION list[GU_SELECTION_MAX];
  size_t count;
  /* "ALL" asks for every supported NVIDIA function present at boot. */
  BOOLEAN all;
  BOOLEAN from_variable;
} GU_SELECTION;

/* Parse the text form. Accepts surrounding whitespace, ASCII only, and
 * rejects duplicates, out-of-range fields, a leading comma, a trailing comma
 * and any other separator. An empty or whitespace-only text yields count == 0
 * (no request) rather than an error. */
EFI_STATUS GuSelectionParse(const CHAR8 *Text, UINTN Bytes, GU_SELECTION *Out);

/* Render the canonical text form ("ALL" or the comma-separated list). */
EFI_STATUS GuSelectionFormat(const GU_SELECTION *Selection, CHAR8 *Out,
                            UINTN Bytes, UINTN *Used);

/* Read the boot-time selection. EFI_NOT_FOUND means the variable is absent,
 * which is not an error: the caller applies its single-card policy. */
EFI_STATUS GuSelectionRead(EFI_SYSTEM_TABLE *Table, GU_SELECTION *Out);

/* Persist a selection. Used by the operator prompt and by an installer. */
EFI_STATUS GuSelectionWrite(EFI_SYSTEM_TABLE *Table, const GU_SELECTION *Selection);
EFI_STATUS GuSelectionDelete(EFI_SYSTEM_TABLE *Table);
