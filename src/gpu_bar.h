/* SPDX-License-Identifier: GPL-2.0-only */
/* BAR0 window helpers shared by the firmware port and the host fixtures.
 *
 * The register offsets this code uses are relative to BAR0 and every one of
 * them is inside the first 16 MiB. The window the firmware assigned is
 * authoritative: a 64-bit BAR may be placed above 4 GiB and a Resizable BAR
 * may be larger, so the base is carried as 64 bits and the size is read from
 * the firmware descriptor, never assumed.
 */
#pragma once

#define GU_BAR_MIN_BYTES 0x1000000ULL

static inline UINT64 GuBarBaseFromConfig(const UINT32 Config[64]) {
  UINT64 Base = (UINT64)(Config[4] & ~0xfU);
  /* BAR type bits [2:1]: 0b10 is a 64-bit memory BAR with its high dword at
   * offset 0x14. Anything else keeps the low dword. */
  if (((Config[4] >> 1) & 3U) == 2U) Base |= (UINT64)Config[5] << 32;
  return Base;
}
