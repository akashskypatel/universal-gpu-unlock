/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only NVIDIA display inventory and optional identity selection. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define GU_INVENTORY_MAX_HANDLES 4096U
#define GU_INVENTORY_MAX_NVIDIA 16U

typedef struct {
  uint32_t pci_id;         /* device:vendor, PCI config offset 0x00 */
  uint32_t subsystem_id;   /* subsystem-device:subsystem-vendor, offset 0x2c */
  uint16_t class_code;     /* class:subclass, offset 0x08 >> 16 */
  uint32_t segment, bus, device, function;
  void *handle;
  void *pci;
} GU_GPU_RECORD;

typedef struct {
  uint32_t pci_id, subsystem_id;
  uint32_t segment, bus, device, function;
} GU_GPU_IDENTITY;

typedef struct {
  size_t count;
  GU_GPU_RECORD nvidia[GU_INVENTORY_MAX_NVIDIA];
} GU_GPU_INVENTORY;

typedef int (*GU_GPU_READER)(void *context, size_t handle_index, GU_GPU_RECORD *record);

enum {
  GU_INVENTORY_OK = 0,
  GU_INVENTORY_INVALID = 1,
  GU_INVENTORY_READ_FAILED = 2,
  GU_INVENTORY_TOO_MANY = 3,
  GU_INVENTORY_DUPLICATE = 4,
  GU_INVENTORY_NOT_FOUND = 5
};

typedef struct {
  uint32_t segment, bus, device, function;
} GU_GPU_LOCATION;

/* Reader must produce a complete CPU-owned record for every handle or fail. */
int GuCollectGpuInventory(size_t handle_count, GU_GPU_READER reader,
                          void *context, GU_GPU_INVENTORY *inventory);
/* Explicit selection is observational; it never arms or mutates a GPU. */
int GuFindExactGpu(const GU_GPU_INVENTORY *inventory,
                   const GU_GPU_IDENTITY *expected, size_t *index);
/* No model, subsystem, slot, root port or GPU count is assumed. A caller
 * must explicitly identify the BDF in the CURRENT snapshot. The selected
 * record still needs later, independent policy checks before any actuation. */
int GuFindGpuAtLocation(const GU_GPU_INVENTORY *inventory,
                        const GU_GPU_LOCATION *location, size_t *index);

/* Atomically resolve an ordered sequence of observed NVIDIA PCI locations.
 * On any error all output indices are zero. No driver or GPU state changes. */
int GuFindGpuSequence(const GU_GPU_INVENTORY *inventory,
                      const GU_GPU_LOCATION *locations, size_t count,
                      size_t *indices);
