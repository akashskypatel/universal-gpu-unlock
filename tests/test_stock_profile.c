/* SPDX-License-Identifier: GPL-2.0-only */
/* Host unit test for the compiled stock-profile precondition.
 *
 * Build: gcc -std=c11 -Wall -Wextra -Werror -pedantic \
 *          -Isrc tests/test_stock_profile.c
 *
 * The predicate decides one thing: does the card's cold SS0 contradict the
 * profile this image was built for? A profile with no expectation accepts
 * whatever it always accepted, so this can never widen a build.
 */
#include <stdio.h>

#include "stock_profile.h"

static unsigned checks, failures;

static void check(int ok, const char *what) {
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL:%u %s\n", checks, what);
  }
}

int main(void) {
  /* The two reference stock values the fork has captured. */
  const unsigned original = 0x03100062U;
  const unsigned observed = 0x03000062U;

  /* Expectation met. */
  check(GuStockProfileAccepts(original, 1, original) == 1, "exact match accepted");
  check(GuStockProfileAccepts(observed, 1, observed) == 1, "second match accepted");

  /* One bit apart is still a different card. */
  check(GuStockProfileAccepts(observed, 1, original) == 0,
        "observed card refused by an original build");
  check(GuStockProfileAccepts(original, 1, observed) == 0,
        "original card refused by an observed build");
  check(GuStockProfileAccepts(original ^ 1U, 1, original) == 0,
        "a one-bit difference is refused");

  /* Values a card might present when it is not what was captured. */
  check(GuStockProfileAccepts(0U, 1, original) == 0, "zero refused");
  check(GuStockProfileAccepts(0xffffffffU, 1, original) == 0, "all ones refused");
  check(GuStockProfileAccepts(0x88888888U, 1, original) == 0,
        "the unusable selector refused");

  /* Entry-latched builds carry no expectation: they accept exactly what they
   * accepted before this check existed. */
  check(GuStockProfileAccepts(original, 0, 0U) == 1, "no expectation accepts");
  check(GuStockProfileAccepts(observed, 0, 0U) == 1, "no expectation accepts again");
  check(GuStockProfileAccepts(0xffffffffU, 0, 0U) == 1,
        "no expectation still accepts the sanity-checked value");

  /* The predicate is total: the same input always gives the same answer. */
  check(GuStockProfileAccepts(original, 1, original) ==
            GuStockProfileAccepts(original, 1, original),
        "predicate is stable");

  printf("test_stock_profile: %u checks OK\n", checks);
  return failures ? 1 : 0;
}
