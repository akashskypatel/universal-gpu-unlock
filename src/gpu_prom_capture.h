/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Exact legacy Windows-GPU-UNLOCK probe.c PROM window, not a complete NVGI
 * IFR/SPI backup. No register writes, firmware execution, DMA or FLR. */
#define GU_PROM_CAPTURE_BYTES 1048576U
#define GU_PROM_CAPTURE_BAR0_OFFSET 0x00300000U
typedef int (*GU_PROM_READ32)(void *context, uint32_t offset, uint32_t *value);
typedef struct {
  void *context;
  GU_PROM_READ32 read32;
} GU_PROM_READER;

/* Copies precisely 1 MiB from BAR0 via 262144 individually checked reads.
 * A failed read clears the entire output; no partial success is published.
 * The reader callback is a capability: callers must independently establish
 * selected device identity, resource bounds and permitted MMIO semantics. */
int GuPromCapture(const GU_PROM_READER *reader, uint8_t *output,
                  size_t output_size);
