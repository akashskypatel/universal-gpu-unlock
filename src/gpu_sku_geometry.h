/* SPDX-License-Identifier: GPL-2.0-only */
/* Per-SKU register geometry for the pinned transaction.
 *
 * The transaction drives chip-specific registers: the permission/readout
 * offsets (0x1fa7cc, 0x1fa7c4, 0x9a0148, 0x823804), the selector pair
 * (0x82381c / 0x823820), the engine bases and the AD102 engine marker
 * 0x67b00001 all come from one captured AD102 dump. Nothing in the tree
 * shows that an AD103/AD104/AD106/AD107 part has the same layout, and a
 * wrong offset is a write to an unrelated register, not an error return.
 *
 * So this module answers exactly one question per PCI device id: has that
 * chip's register geometry actually been captured? The actuation paths
 * refuse everything else instead of extrapolating from AD102. Recovering a
 * new SKU means adding its captured table here, not loosening the gate.
 *
 * The read-only diagnostic path deliberately does NOT consult this module:
 * capturing the evidence needed to add a row is what the diagnostic is for.
 *
 * The API uses only plain C types so the firmware sources, the vendor core
 * (which is compiled in a kernel-style environment) and host unit tests all
 * include this header without fighting over typedefs.
 */
#pragma once

typedef struct {
  unsigned Device;        /* PCI device id */
  const char *Sku;        /* marketing name, for the log */
  const char *Chip;       /* ASIC family label */
  int GeometryCaptured;   /* 1 = offsets came from this chip's own dump */
} GU_SKU_GEOMETRY;

/* The row for a device id, or NULL when the id is not an Ada part. */
const GU_SKU_GEOMETRY *GuSkuGeometryFor(unsigned Device);

/* 1 when this Ada part has a captured register table. Always 0 for ids that
 * are not Ada: those take the generic Ampere/Turing branches, not these
 * offsets, and their admission is decided elsewhere. */
int GuSkuGeometryAdaCaptured(unsigned Device);

/* 1 when the id is Ada and its geometry is NOT captured, i.e. the actuation
 * paths must refuse it. This is the only predicate the gates use, so an id
 * that is added to GU_ADA_DEVICE_IDS without a captured table here
 * fails closed rather than silently inheriting AD102 offsets. */
int GuSkuGeometryRefuses(unsigned Device);

/* Chip label ("AD102", "AD104", ...), or NULL when the id is not Ada. */
const char *GuSkuGeometryChip(unsigned Device);
