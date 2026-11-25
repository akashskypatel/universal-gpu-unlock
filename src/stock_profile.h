/* SPDX-License-Identifier: GPL-2.0-only */
/* The compiled stock profile as a precondition, not a comment.
 *
 * Which stock selector policy an image implements is chosen at build time
 * (`GU_STOCK_PROFILE_ORIGINAL_03100062`, `GU_STOCK_PROFILE_OBSERVED_03000062`,
 * `GU_ENTRY_LATCHED_SS1_PRESERVE`, `GU_ENTRY_LATCHED_SS1_FULL`). Three of the
 * four also imply an expectation about the card's cold selector SS0.
 *
 * Until this module existed, that expectation was never checked: the constant
 * was defined in `port.h` and referenced nowhere, so an image built for one
 * stock value would run its destructive path on a card presenting another. The
 * two entry-latched policies deliberately have no fixed SS0 - they latch
 * whatever the card presents - which is why no probe can tell them apart
 * before the write, and why choosing between them stays an operator decision.
 *
 * This predicate is the enforceable half: an image refuses a card whose cold
 * SS0 contradicts the profile it was built for. It never decides which policy
 * is correct, and it never widens one: a build without an expectation accepts
 * exactly what it accepted before.
 */
#pragma once

/* 1 when the observed value satisfies the compiled profile, 0 when it
 * contradicts it. A profile without an expectation always accepts. */
static inline int GuStockProfileAccepts(unsigned Observed, int HasExpected,
                                        unsigned Expected) {
  if (!HasExpected) return 1;
  return Observed == Expected;
}
