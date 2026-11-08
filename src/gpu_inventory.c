/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_inventory.h"
#include <string.h>

static int SameLocation(const GU_GPU_RECORD *a, const GU_GPU_RECORD *b) {
  return a->segment == b->segment && a->bus == b->bus &&
         a->device == b->device && a->function == b->function;
}

int GuCollectGpuInventory(size_t handle_count, GU_GPU_READER reader,
                          void *context, GU_GPU_INVENTORY *inventory) {
  size_t i, j;
  GU_GPU_RECORD record;
  if (!inventory) return GU_INVENTORY_INVALID;
  memset(inventory, 0, sizeof(*inventory));
  if (!reader || !handle_count || handle_count > GU_INVENTORY_MAX_HANDLES)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < handle_count; i++) {
    memset(&record, 0, sizeof(record));
    if (reader(context, i, &record)) goto read_failed;
    if (!record.handle || !record.pci) goto read_failed;
    if ((record.pci_id & 0xffffU) != 0x10deU ||
        (record.class_code != 0x0300U && record.class_code != 0x0302U))
      continue;
    if (record.segment > 0xffffU || record.bus > 255U ||
        record.device > 31U || record.function > 7U)
      goto read_failed;
    for (j = 0; j < inventory->count; j++) {
      const GU_GPU_RECORD *prior = &inventory->nvidia[j];
      if (SameLocation(prior, &record) || prior->handle == record.handle ||
          prior->pci == record.pci) {
        memset(inventory, 0, sizeof(*inventory));
        return GU_INVENTORY_DUPLICATE;
      }
    }
    if (inventory->count == GU_INVENTORY_MAX_NVIDIA) {
      memset(inventory, 0, sizeof(*inventory));
      return GU_INVENTORY_TOO_MANY;
    }
    inventory->nvidia[inventory->count++] = record;
  }
  return GU_INVENTORY_OK;
read_failed:
  memset(inventory, 0, sizeof(*inventory));
  return GU_INVENTORY_READ_FAILED;
}

int GuFindExactGpu(const GU_GPU_INVENTORY *inventory,
                   const GU_GPU_IDENTITY *expected, size_t *index) {
  size_t i;
  if (index) *index = 0;
  if (!inventory || !expected || !index ||
      inventory->count > GU_INVENTORY_MAX_NVIDIA)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < inventory->count; i++) {
    const GU_GPU_RECORD *p = &inventory->nvidia[i];
    if (p->pci_id == expected->pci_id &&
        p->subsystem_id == expected->subsystem_id &&
        p->segment == expected->segment && p->bus == expected->bus &&
        p->device == expected->device && p->function == expected->function) {
      *index = i;
      return GU_INVENTORY_OK;
    }
  }
  return GU_INVENTORY_NOT_FOUND;
}

/* Resolve an operator-supplied location against the current enumeration.
 * No GPU model or physical motherboard layout is part of discovery. */
int GuFindGpuAtLocation(const GU_GPU_INVENTORY *inventory,
                        const GU_GPU_LOCATION *location, size_t *index) {
  size_t i;
  if (index) *index = 0;
  if (!inventory || !location || !index ||
      inventory->count > GU_INVENTORY_MAX_NVIDIA ||
      location->segment > 0xffffU || location->bus > 255U ||
      location->device > 31U || location->function > 7U)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < inventory->count; i++) {
    const GU_GPU_RECORD *gpu = &inventory->nvidia[i];
    if (gpu->segment == location->segment && gpu->bus == location->bus &&
        gpu->device == location->device &&
        gpu->function == location->function) {
      *index = i;
      return GU_INVENTORY_OK;
    }
  }
  return GU_INVENTORY_NOT_FOUND;
}
