/* SPDX-License-Identifier: GPL-2.0-only */
/* Portable capture core, ported from Windows-GPU-UNLOCK
 * internal-boot/boot/probe.c:ProbeBar/DumpProm. Does NOT access hardware
 * without a caller-provided reader callback. */
#include "gpu_prom_capture.h"
#include <string.h>

int GuPromCapture(const GU_PROM_READER *reader, uint8_t *output,
                  size_t output_size) {
  size_t i;
  if (!output || output_size != GU_PROM_CAPTURE_BYTES) return 0;
  memset(output, 0, GU_PROM_CAPTURE_BYTES);
  if (!reader || !reader->read32) return 0;
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
