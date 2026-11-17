/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_inventory.h"
#include <string.h>

static int SameLocation(const GU_GPU_RECORD *a, const GU_GPU_RECORD *b) {
  return a->segment == b->segment && a->bus == b->bus &&
         a->device == b->device && a->function == b->function;
}

int GuCollectGpuRecords(size_t handle_count, GU_GPU_READER reader,
                        void *context, GU_GPU_RECORD *records,
                        size_t capacity, size_t *count) {
  size_t i, j;
  GU_GPU_RECORD record;
  if (count) *count = 0;
  if (!count || !records || !capacity ||
      capacity > SIZE_MAX / sizeof(*records))
    return GU_INVENTORY_INVALID;
  memset(records, 0, capacity * sizeof(*records));
  if (!reader || !handle_count || handle_count > GU_INVENTORY_MAX_HANDLES)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < handle_count; i++) {
    memset(&record, 0, sizeof(record));
    if (reader(context, i, &record)) goto read_failed;
    if (!record.handle || !record.pci) goto read_failed;
    if ((record.pci_id & 0xffffU) != 0x10deU ||
        ((record.class_code & 0xff00U) != 0x0300U))
      continue;
    if (record.segment > 0xffffU || record.bus > 255U ||
        record.device > 31U || record.function > 7U)
      goto read_failed;
    for (j = 0; j < *count; j++) {
      const GU_GPU_RECORD *prior = &records[j];
      if (SameLocation(prior, &record) || prior->handle == record.handle ||
          prior->pci == record.pci) {
        memset(records, 0, capacity * sizeof(*records));
        *count = 0;
        return GU_INVENTORY_DUPLICATE;
      }
    }
    if (*count == capacity) {
      memset(records, 0, capacity * sizeof(*records));
      *count = 0;
      return GU_INVENTORY_TOO_MANY;
    }
    records[(*count)++] = record;
  }
  return GU_INVENTORY_OK;
read_failed:
  memset(records, 0, capacity * sizeof(*records));
  *count = 0;
  return GU_INVENTORY_READ_FAILED;
}

int GuCollectGpuInventory(size_t handle_count, GU_GPU_READER reader,
                          void *context, GU_GPU_INVENTORY *inventory) {
  if (!inventory) return GU_INVENTORY_INVALID;
  return GuCollectGpuRecords(handle_count, reader, context, inventory->nvidia,
                             GU_INVENTORY_MAX_NVIDIA, &inventory->count);
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
static int FindRecordAtLocation(const GU_GPU_RECORD *records, size_t record_count,
                                const GU_GPU_LOCATION *location, size_t *index) {
  size_t i;
  if (index) *index = 0;
  if (!records || !location || !index ||
      record_count > GU_INVENTORY_MAX_HANDLES ||
      location->segment > 0xffffU || location->bus > 255U ||
      location->device > 31U || location->function > 7U)
    return GU_INVENTORY_INVALID;
  for (i = 0; i < record_count; i++) {
    const GU_GPU_RECORD *gpu = &records[i];
    if (gpu->segment == location->segment && gpu->bus == location->bus &&
        gpu->device == location->device &&
        gpu->function == location->function) {
      *index = i;
      return GU_INVENTORY_OK;
    }
  }
  return GU_INVENTORY_NOT_FOUND;
}

int GuFindGpuAtLocation(const GU_GPU_INVENTORY *inventory,
                        const GU_GPU_LOCATION *location, size_t *index) {
  if (index) *index = 0;
  if (!inventory || inventory->count > GU_INVENTORY_MAX_NVIDIA)
    return GU_INVENTORY_INVALID;
  return FindRecordAtLocation(inventory->nvidia, inventory->count, location, index);
}

int GuFindGpuRecordSequence(const GU_GPU_RECORD *records, size_t record_count,
                            const GU_GPU_LOCATION *locations, size_t count,
                            size_t *indices) {
  size_t i, j;
  int result;
  if (!indices || !locations || !records || !count ||
      count > GU_INVENTORY_MAX_HANDLES ||
      record_count > GU_INVENTORY_MAX_HANDLES)
    return GU_INVENTORY_INVALID;
  memset(indices, 0, count * sizeof(*indices));
  for (i = 0; i < count; i++) {
    for (j = 0; j < i; j++)
      if (locations[i].segment == locations[j].segment &&
          locations[i].bus == locations[j].bus &&
          locations[i].device == locations[j].device &&
          locations[i].function == locations[j].function) {
        result = GU_INVENTORY_DUPLICATE;
        goto failed;
      }
    result = FindRecordAtLocation(records, record_count, &locations[i], &indices[i]);
    if (result != GU_INVENTORY_OK) goto failed;
    for (j = 0; j < i; j++)
      if (indices[i] == indices[j]) {
        result = GU_INVENTORY_DUPLICATE;
        goto failed;
      }
  }
  return GU_INVENTORY_OK;
failed:
  memset(indices, 0, count * sizeof(*indices));
  return result;
}

int GuFindGpuSequence(const GU_GPU_INVENTORY *inventory,
                      const GU_GPU_LOCATION *locations, size_t count,
                      size_t *indices) {
  if (!inventory || count > GU_INVENTORY_MAX_NVIDIA ||
      inventory->count > GU_INVENTORY_MAX_NVIDIA)
    return GU_INVENTORY_INVALID;
  return GuFindGpuRecordSequence(inventory->nvidia, inventory->count,
                                 locations, count, indices);
}
