/* SPDX-License-Identifier: GPL-2.0-only */

/* Turing is refused when the image is built, not only when it runs.
 *
 * The runtime refusal already exists: `GuAd104Prepare` refuses a Turing card and
 * the cold-state predicate is passive only (`TURING_COLD=PASSIVE …
 * ACTIVE_ALLOWED=0`). What was missing is the build half of A5's exit criterion.
 * This tree pins five TU102 hash expectations in `profile.c` while
 * `TU102-ASSETS/` contains only a README - the payload bytes are not here. So an
 * image that would *act* on a Turing card cannot be built at all: there is
 * nothing to stage, nothing to hash, and nothing that has passed a loader
 * contract.
 *
 * Two checks, both at compile time:
 *
 *   1. GU_TURING_ACTUATION fails the build with an explanation, rather than
 *      producing an image that refuses the card at prepare after the operator
 *      has already chosen it;
 *   2. the pinned Turing manifest must keep exactly the rows it was reviewed
 *      with. Emptying or trimming the table (say, to "remove the unusable
 *      assets") now breaks the build instead of silently leaving Turing cards
 *      admitted with a partial manifest - the admission list in
 *      `gpu_device_ids.h` has to change with it.
 *
 * Neither check claims the assets are correct. They only stop the tree from
 * shipping a Turing path that cannot work.
 */
#ifndef GU_TURING_ASSETS_H
#define GU_TURING_ASSETS_H

#ifdef GU_TURING_ACTUATION
#error "Turing actuation cannot be built: TU102-ASSETS/ carries no payload in this tree, so there is nothing to stage or hash. Implement the loader, sign it, and only then remove this gate - until then Turing stays refused at prepare."
#endif

/* The reviewed row count of the Turing manifest in profile.c. */
#define GU_TURING_MANIFEST_ROWS 5U

/* A tripwire, not a type: a negative array bound is a compile error, so a
 * manifest whose row count drifted from the reviewed one fails the build. */
#define GU_TURING_ROW_COUNT_IS(TABLE) \
  typedef char GuTuringManifestRowCount[ \
    (sizeof((TABLE).entries) / sizeof((TABLE).entries[0]) == GU_TURING_MANIFEST_ROWS) \
    ? 1 : -1]

#endif
