/* SPDX-License-Identifier: GPL-2.0-only */
/* The one name for a captured 1-MiB ROM window, so the file a boot writes and
 * the file a later build reads cannot drift apart.
 *
 * Two sources write this name: src/gpu_topology_efi.c writes the window the
 * VRAM-block probe just hashed, beside its own log, and
 * src/gpu_prom_capture_efi.c carries the same format for the prepared capture
 * that is not linked into any image. A host test compiles this header and
 * exercises both expectations, exactly as tests/test_prom_identity.c does for
 * the structural predicate.
 *
 * The four-digit run index is what keeps a second boot from overwriting a first
 * capture: it is chosen by the writer's own prior-existence loop, so the name is
 * unique without needing an atomic exclusive create, which UEFI file protocols
 * do not provide. Nothing here authenticates anything; the name only says which
 * card a megabyte came from and which capture of it this is.
 *
 * No UEFI service, no hardware, no allocation. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* "\PROM-FFFF-FF-1F-7-9999.BIN" is 27 characters; this is the buffer a caller
 * must provide, including the terminating 0. */
#define GU_PROM_FILE_CHARS 28U

/* Element type of the name. In the firmware this is CHAR16 (UINT16 = unsigned
 * short), which is why the builder writes 16-bit units; the host tests use the
 * same width so a name is never narrowed by the width of wchar_t. */
typedef uint16_t GU_PROM_CHAR;

/* Builds the name of capture `run` of the window read from this card's
 * function into `out`. Returns the number of characters written (27), or 0 when
 * an argument does not fit: segment 16 bits, bus 8, device 5, function 3 and run
 * 4 decimal digits. On 0 nothing is written and nothing is read past `out`. */
static inline size_t GuPromFileName(GU_PROM_CHAR *out, size_t capacity,
                                    unsigned segment, unsigned bus,
                                    unsigned device, unsigned function,
                                    unsigned run)
{
  static const GU_PROM_CHAR hex[] = {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
  };
  GU_PROM_CHAR buffer[GU_PROM_FILE_CHARS];
  size_t i;

  if (!out || capacity < GU_PROM_FILE_CHARS) return 0;
  if (segment > 0xffffU || bus > 0xffU || device > 0x1fU ||
      function > 0x7U || run > 9999U) return 0;

  buffer[0] = '\\';
  buffer[1] = 'P';
  buffer[2] = 'R';
  buffer[3] = 'O';
  buffer[4] = 'M';
  buffer[5] = '-';
  for (i = 0; i < 4U; i++)
    buffer[6U + i] = hex[(GU_PROM_CHAR)((segment >> (12U - 4U * (unsigned)i)) & 15U)];
  buffer[10] = '-';
  for (i = 0; i < 2U; i++)
    buffer[11U + i] = hex[(GU_PROM_CHAR)((bus >> (4U - 4U * (unsigned)i)) & 15U)];
  buffer[13] = '-';
  for (i = 0; i < 2U; i++)
    buffer[14U + i] = hex[(GU_PROM_CHAR)((device >> (4U - 4U * (unsigned)i)) & 15U)];
  buffer[16] = '-';
  buffer[17] = (GU_PROM_CHAR)('0' + function);
  buffer[18] = '-';
  buffer[19] = (GU_PROM_CHAR)('0' + run / 1000U);
  buffer[20] = (GU_PROM_CHAR)('0' + (run / 100U) % 10U);
  buffer[21] = (GU_PROM_CHAR)('0' + (run / 10U) % 10U);
  buffer[22] = (GU_PROM_CHAR)('0' + run % 10U);
  buffer[23] = '.';
  buffer[24] = 'B';
  buffer[25] = 'I';
  buffer[26] = 'N';
  buffer[27] = 0;
  for (i = 0; i < GU_PROM_FILE_CHARS; i++) out[i] = buffer[i];
  return 27U;
}

/* The same name in the narrow text a diagnostic line is printed in. The
 * firmware log is `char`, not CHAR16, and the capture line has to name the file
 * it wrote or an operator cannot match a log to a file. Every character the
 * builder produces is ASCII, and the narrowing is still checked. Returns the
 * number of characters written, or 0 on the same failures as GuPromFileName,
 * including a capacity below GU_PROM_FILE_CHARS. */
static inline size_t GuPromFileText(char *out, size_t capacity,
                                    unsigned segment, unsigned bus,
                                    unsigned device, unsigned function,
                                    unsigned run)
{
  GU_PROM_CHAR wide[GU_PROM_FILE_CHARS];
  size_t i, written;
  if (!out || capacity < GU_PROM_FILE_CHARS) return 0;
  written = GuPromFileName(wide, GU_PROM_FILE_CHARS, segment, bus, device,
                           function, run);
  if (!written) return 0;
  for (i = 0; i <= written; i++) {
    if (wide[i] > 0x7fU) return 0;
    out[i] = (char)wide[i];
  }
  return written;
}
