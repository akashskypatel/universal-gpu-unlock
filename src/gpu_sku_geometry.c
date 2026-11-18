/* SPDX-License-Identifier: GPL-2.0-only */
#include "gpu_sku_geometry.h"

/* Every Ada id admitted by GU_ADA_DEVICE_IDS appears here exactly once; the
 * contract test enforces that, so adding an id to the device list without
 * classifying its geometry cannot compile into a permissive image.
 *
 * "Captured" means the offsets the transaction uses were derived from a dump
 * of that chip. It does not mean the transaction has been verified on it:
 * ACTUATION_VERIFIED=0 still holds for every row.
 */
static const GU_SKU_GEOMETRY Ada[] = {
  {0x2684U, "RTX 4090",           "AD102", 1},
  {0x2685U, "RTX 4090D",          "AD102", 1},
  {0x2702U, "0x2702",             "AD103", 0},
  {0x2703U, "RTX 4080 SUPER",     "AD103", 0},
  {0x2704U, "RTX 4080",           "AD103", 0},
  {0x2705U, "RTX 4070 Ti SUPER",  "AD103", 0},
  {0x2782U, "RTX 4070 Ti",        "AD104", 0},
  {0x2783U, "RTX 4070 SUPER",     "AD104", 0},
  {0x2786U, "RTX 4070",           "AD104", 0},
  {0x2860U, "RTX 4060 Ti",        "AD106", 0},
  {0x2882U, "RTX 4060",           "AD107", 0},
};

const GU_SKU_GEOMETRY *GuSkuGeometryFor(unsigned Device) {
  unsigned i;
  for (i = 0; i < sizeof(Ada) / sizeof(Ada[0]); i++)
    if (Ada[i].Device == Device) return &Ada[i];
  return 0;
}

int GuSkuGeometryAdaCaptured(unsigned Device) {
  const GU_SKU_GEOMETRY *row = GuSkuGeometryFor(Device);
  return row && row->GeometryCaptured;
}

int GuSkuGeometryRefuses(unsigned Device) {
  const GU_SKU_GEOMETRY *row = GuSkuGeometryFor(Device);
  return row && !row->GeometryCaptured;
}

const char *GuSkuGeometryChip(unsigned Device) {
  const GU_SKU_GEOMETRY *row = GuSkuGeometryFor(Device);
  return row ? row->Chip : 0;
}
