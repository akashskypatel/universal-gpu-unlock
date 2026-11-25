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

/* The same structural test without a pin: exactly one GPT hard-drive node with
 * a nonempty partition GUID and the EFI-system type. Used by images that
 * identify the Windows ESP from the live device tree and rely on the caller's
 * "exactly one volume carries the Windows boot manager" rule instead of a
 * compiled GUID. */
int GuWindowsEspPathCandidate(const uint8_t *path, size_t bound);
/* Copy the nonzero unique GPT partition identity from a validated path. */
int GuWindowsEspPathGuid(const uint8_t *path, size_t bound, uint8_t guid[16]);

/* A2's missing half: does the boot option's file-path list name this volume?
 *
 * ``load_option`` is an EFI_LOAD_OPTION as a Boot#### variable returns it:
 * Attributes (4), FilePathListLength (2), a NUL-terminated UTF-16 description,
 * then the device path. The comparison is over the device-node prefix - the
 * nodes before the first FilePath (media/0x04) or end (0x7f) node - so an
 * option that
 * appends \EFI\Microsoft\Boot\bootmgfw.efi still names the same partition,
 * while a different partition, controller or segment does not.
 *
 * Returns 1 for a well-formed option whose device prefix matches, 0 otherwise.
 * Both arguments are untrusted byte ranges with explicit bounds. */
int GuBootOptionNamesDevicePath(const uint8_t *load_option, size_t option_bound,
                                const uint8_t *path, size_t path_bound);

/* A2's cross-check, testable without firmware: walk BootOrder, read every
 * Boot#### it names, and ask whether any of them names this volume.
 *
 * ``read`` receives a NUL-terminated UTF-16LE variable name and its byte
 * length, and must return 1 on success, 0 when the variable does not exist and
 * -1 for any other failure. BootOrder is what the firmware will try, so:
 *
 *   GU_ORDER_NAMES      one named option points at this volume
 *   GU_ORDER_NONE       BootOrder was readable and nothing points here
 *   GU_ORDER_UNREADABLE BootOrder or an option could not be read in full
 *
 * A caller may refuse on GU_ORDER_NONE, but must never report GU_ORDER_NAMES
 * for GU_ORDER_UNREADABLE. */
#define GU_ORDER_NAMES 1
#define GU_ORDER_NONE 0
#define GU_ORDER_UNREADABLE (-1)
typedef int (*GU_VARIABLE_READ)(const uint8_t *name, size_t name_bytes,
                                uint8_t *out, size_t *size);
int GuBootOrderNamesVolume(GU_VARIABLE_READ read, const uint8_t *path,
                           size_t path_bound);
