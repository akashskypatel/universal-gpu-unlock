/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_inventory.h"
#include <string.h>

/* Half-open range comparison for caller-owned arrays. A range that could wrap
 * the address space is reported as overlapping, never as independent. */
static size_t SpanBytes(size_t count, size_t size) {
  if (!count || !size) return 0;
  if (count > SIZE_MAX / size) return SIZE_MAX;
  return count * size;
}

static int RangesOverlap(const void *a, size_t a_bytes,
                         const void *b, size_t b_bytes) {
  size_t a0 = (size_t)(uintptr_t)a, b0 = (size_t)(uintptr_t)b;
  if (!a || !b) return 0;
  if (a0 > SIZE_MAX - a_bytes || b0 > SIZE_MAX - b_bytes) return 1;
  return a0 < b0 + b_bytes && b0 < a0 + a_bytes;
}

static int SameLocation(const GU_GPU_RECORD *a, const GU_GPU_RECORD *b) {
  return a->segment == b->segment && a->bus == b->bus &&
         a->device == b->device && a->function == b->function;
}

int GuCollectGpuRecords(size_t handle_count, GU_GPU_READER reader,
                        void *context, GU_GPU_RECORD *records,
                        size_t capacity, size_t *count) {
  size_t i, j;
  GU_GPU_RECORD record;
  if (!count) return GU_INVENTORY_INVALID;
  if (!records || !capacity || capacity > SIZE_MAX / sizeof(*records)) {
    *count = 0;
    return GU_INVENTORY_INVALID;
  }
  /* Publishing a count into the record storage itself would corrupt the
   * records this function is about to produce, so refuse without writing. */
  if (RangesOverlap(records, capacity * sizeof(*records), count, sizeof(*count)))
    return GU_INVENTORY_INVALID;
  *count = 0;
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
  size_t record_bytes, location_bytes, index_bytes;
  if (!indices || !locations || !records || !count ||
      count > GU_INVENTORY_MAX_HANDLES ||
      record_count > GU_INVENTORY_MAX_HANDLES)
    return GU_INVENTORY_INVALID;
  /* The documented non-overlap precondition is established here, before any
   * output byte is written, regardless of what the caller assumed. */
  record_bytes = SpanBytes(record_count, sizeof(*records));
  location_bytes = SpanBytes(count, sizeof(*locations));
  index_bytes = SpanBytes(count, sizeof(*indices));
  if (RangesOverlap(records, record_bytes, locations, location_bytes) ||
      RangesOverlap(records, record_bytes, indices, index_bytes) ||
      RangesOverlap(locations, location_bytes, indices, index_bytes))
    return GU_INVENTORY_OVERLAP;
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
