/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded, read-only device-path check for an explicitly pinned GPT ESP. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Returns the byte length through END_ENTIRE, or zero for an invalid path. */
size_t GuWindowsPathLength(const uint8_t *path, size_t bound);

/* The GUID argument uses EFI_GUID's in-memory, little-endian byte layout.
 * Exactly one GPT hard-drive node must carry the expected unique GUID. */
int GuWindowsEspPathMatches(const uint8_t *path, size_t bound,
                            const uint8_t guid[16]);
