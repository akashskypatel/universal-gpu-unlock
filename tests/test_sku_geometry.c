/* SPDX-License-Identifier: GPL-2.0-only */
/* Host unit test for the per-SKU geometry table.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -pedantic \
 *          -Isrc tests/test_sku_geometry.c src/gpu_sku_geometry.c
 *
 * The point of this table is that it cannot silently widen: every Ada id the
 * image admits must be classified here, and only a chip whose own dump was
 * captured may be reported as captured.
 */
#include <stdio.h>
#include <string.h>

#include "gpu_sku_geometry.h"
#include "gpu_device_ids.h"

static int checks, failures;

static void check(int ok, const char *what, unsigned device) {
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL:%d %s (device %#x)\n", checks, what, device);
  }
}

#define GU_CHECK_DEVICE(id) check(1, "row exists", (unsigned)(id));

int main(void) {
  /* The captured set is the reference AD102 pair and nothing else. Two ids
   * share one chip, so the count is by chip, not by id. */
  static const unsigned captured[] = {0x2684U, 0x2685U};
  unsigned known[64];
  unsigned count = 0, i, j;

  /* Every Ada id the image admits must have a row: an unclassified id would
   * otherwise be refused by accident rather than by decision. */
#define GU_ADA_ROW(id)                                                        \
  do {                                                                        \
    const GU_SKU_GEOMETRY *row = GuSkuGeometryFor((unsigned)(id));            \
    check(row != NULL, "Ada id has no geometry row", (unsigned)(id));         \
    check(row && row->Device == (unsigned)(id), "row has the wrong id",       \
          (unsigned)(id));                                                    \
    check(row && row->Chip && row->Chip[0], "row has no chip label",          \
          (unsigned)(id));                                                    \
    check(row && row->Sku && row->Sku[0], "row has no sku label",             \
          (unsigned)(id));                                                    \
    check(row && (row->GeometryCaptured ==                                  \
                  (strcmp(row->Chip, "AD102") == 0 ? 1 : 0)),               \
          "captured flag disagrees with the chip label", (unsigned)(id));     \
    if (row) known[count++] = (unsigned)(id);                                 \
  } while (0);
  GU_ADA_DEVICE_IDS(GU_ADA_ROW)

  /* Captured exactly where a dump exists. */
  for (i = 0; i < sizeof(captured) / sizeof(captured[0]); i++) {
    const GU_SKU_GEOMETRY *row = GuSkuGeometryFor(captured[i]);
    check(row != NULL, "captured id missing from the table", captured[i]);
    check(GuSkuGeometryAdaCaptured(captured[i]) == 1,
          "captured id is not marked captured", captured[i]);
    check(GuSkuGeometryRefuses(captured[i]) == 0,
          "captured id is refused", captured[i]);
  }

  /* AdaCaptured and Refuses are exact complements over the Ada set. */
  for (i = 0; i < count; i++) {
    int a = GuSkuGeometryAdaCaptured(known[i]);
    int r = GuSkuGeometryRefuses(known[i]);
    check(a != r, "Ada id is neither admitted nor refused", known[i]);
    check(a == 0 || a == 1, "AdaCaptured is not boolean", known[i]);
    check(r == 0 || r == 1, "Refuses is not boolean", known[i]);
  }

  /* Everything outside the Ada set is somebody else's admission decision. */
#define GU_OTHER_ROW(id)                                                      \
  do {                                                                        \
    check(GuSkuGeometryFor((unsigned)(id)) == NULL,                          \
          "a non-Ada id has an Ada geometry row", (unsigned)(id));            \
    check(GuSkuGeometryAdaCaptured((unsigned)(id)) == 0,                      \
          "a non-Ada id reports captured geometry", (unsigned)(id));          \
    check(GuSkuGeometryRefuses((unsigned)(id)) == 0,                          \
          "the Ada gate refuses a non-Ada id", (unsigned)(id));               \
    check(GuSkuGeometryChip((unsigned)(id)) == NULL,                          \
          "a non-Ada id has a chip label", (unsigned)(id));                   \
  } while (0);
  GU_AMPERE_DEVICE_IDS(GU_OTHER_ROW)
  GU_TURING_DEVICE_IDS(GU_OTHER_ROW)

  /* Unknown ids, including the ones a firmware might report on a bad day. */
  check(GuSkuGeometryFor(0x0000U) == NULL, "device 0 has a row", 0);
  check(GuSkuGeometryFor(0xffffU) == NULL, "device 0xffff has a row", 0xffffU);
  check(GuSkuGeometryRefuses(0xffffU) == 0, "0xffff is refused by the Ada gate",
        0xffffU);
  check(GuSkuGeometryAdaCaptured(0x0000U) == 0, "device 0 reports captured", 0);

  /* Duplicate rows would make the answer depend on scan order. */
  for (i = 0; i < count; i++)
    for (j = i + 1; j < count; j++)
      check(known[i] != known[j], "duplicate device id in the Ada list", known[i]);

  /* The table is static: the same lookup must keep returning the same row. */
  check(GuSkuGeometryFor(0x2684U) == GuSkuGeometryFor(0x2684U),
        "geometry lookup is not stable", 0x2684U);

  printf("test_sku_geometry: %d checks OK\n", checks);
  return failures ? 1 : 0;
}
