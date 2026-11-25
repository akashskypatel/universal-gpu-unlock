/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable capture core, ported from Windows-GPU-UNLOCK
 * internal-boot/boot/probe.c:ProbeBar/DumpProm. Does NOT access hardware
 * without a caller-provided reader callback. */
#include "gpu_prom_capture.h"
#include <string.h>

/* Half-open [base,base+bytes) comparison. An unknown extent (zero bytes)
 * still detects a point inside the other range; a range that could wrap the
 * address space is reported as overlapping. */
static int Overlaps(const void *a, size_t a_bytes, const void *b, size_t b_bytes) {
  size_t a0 = (size_t)(uintptr_t)a, b0 = (size_t)(uintptr_t)b;
  if (!a || !b) return 0;
  if (a0 > SIZE_MAX - a_bytes || b0 > SIZE_MAX - b_bytes) return 1;
  return a0 < b0 + b_bytes && b0 < a0 + a_bytes;
}

int GuPromCapture(const GU_PROM_READER *reader, uint8_t *output,
                  size_t output_size) {
  size_t i;
  if (!output || output_size != GU_PROM_CAPTURE_BYTES) return 0;
  if (!reader || !reader->read32) return 0;
  /* The capture owns the entire output window. A reader record or caller
   * context that lives inside it would be cleared or overwritten by this
   * function, so the pairing is refused before the first write rather than
   * repaired after the caller's storage has already been destroyed. */
  if (Overlaps(reader, sizeof(*reader), output, output_size)) return 0;
  if (Overlaps(reader->context, 0, output, output_size)) return 0;
  memset(output, 0, GU_PROM_CAPTURE_BYTES);
  for (i = 0; i < GU_PROM_CAPTURE_BYTES / 4U; ++i) {
    uint32_t word = 0;
    if (!reader->read32(reader->context,
                        GU_PROM_CAPTURE_BAR0_OFFSET + (uint32_t)(i * 4U),
                        &word)) {
      memset(output, 0, GU_PROM_CAPTURE_BYTES);
      return 0;
    }
    /* Encoding is explicit: produces x86 PCI little-endian byte order even
     * when the portable host tests execute on a different architecture. */
    output[i * 4U] = (uint8_t)word;
    output[i * 4U + 1U] = (uint8_t)(word >> 8);
    output[i * 4U + 2U] = (uint8_t)(word >> 16);
    output[i * 4U + 3U] = (uint8_t)(word >> 24);
  }
  return 1;
}
