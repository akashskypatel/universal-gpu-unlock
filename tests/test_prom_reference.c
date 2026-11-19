/* Host test for the per-card firmware acceptance allowlist (multi-GPU step 2).
 *
 * Two builds of the same module are compiled and linked into one binary:
 *
 *   - with the synthetic reference, so the accepted path and every refusal
 *     reason can be exercised against it;
 *   - without it, to prove the shipped table (zero references) admits nothing
 *     and that no card can match the placeholder row by accident.
 *
 * Build (see .github/workflows/gpu-topology-diagnostic.yml):
 *   cc -std=c11 -O1 -fsanitize=undefined -Wall -Wextra -Werror -pedantic \
 *      -DGU_PROM_REFERENCE_HOST_TEST -DGU_PROM_REFERENCE_SYNTHETIC -Isrc \
 *      tests/test_prom_reference.c src/prom_reference.c -o test_prom_reference
 *   cc ... without the synthetic define, with -DGU_PROM_REFERENCE_EMPTY_TEST
 */
#include <stdio.h>
#include <string.h>

#include "prom_reference.h"

static int failures;

#define CHECK(cond) do { \
  if (!(cond)) { printf("CHECK FAILED %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* The digest of the simulator's model ROM, which the synthetic table pins. */
#define MODEL_DIGEST "fffd2b48a1b34a5a5ccdba880cf9195d618e31a9a33176ef5a80dc0b244aa8cb"
#define MODEL_DEVICE  0x2684U
#define MODEL_SUBSYS  0xa14010deU
#define MODEL_LOADER  "610.43.03"

static const char *const REASONS[] = {
  "ACCEPTED", "UNKNOWN_CARD", "DIGEST_MISMATCH", "LOADER_MISMATCH", "MALFORMED",
};

int main(void) {
  /* Reason tokens are stable, one per decision, and never NULL. */
  for (unsigned i = 0; i < sizeof(REASONS) / sizeof(REASONS[0]); i++) {
    CHECK(GuPromReferenceReason((GU_PROM_REF_DECISION)i) != NULL);
    CHECK(strcmp(GuPromReferenceReason((GU_PROM_REF_DECISION)i), REASONS[i]) == 0);
  }
  CHECK(strcmp(GuPromReferenceReason((GU_PROM_REF_DECISION)99), "INVALID_DECISION") == 0);

#ifdef GU_PROM_REFERENCE_SYNTHETIC
  const GU_PROM_REFERENCE *found = (const GU_PROM_REFERENCE *)1;
  CHECK(GuPromReferenceCount() == 1);

  /* Exact identity lookup. No prefix, family or wildcard match. */
  CHECK(GuPromReferenceFind(MODEL_DEVICE, MODEL_SUBSYS) != NULL);
  CHECK(GuPromReferenceFind(MODEL_DEVICE + 1, MODEL_SUBSYS) == NULL);
  CHECK(GuPromReferenceFind(MODEL_DEVICE, MODEL_SUBSYS + 1) == NULL);
  CHECK(GuPromReferenceFind(MODEL_DEVICE >> 4, MODEL_SUBSYS) == NULL);
  CHECK(GuPromReferenceFind(0, 0) == NULL); /* the placeholder row is not reachable */
  CHECK(GuPromReferenceFind(0xffffffffU, 0xffffffffU) == NULL);

  /* The reference says what it is. */
  found = GuPromReferenceFind(MODEL_DEVICE, MODEL_SUBSYS);
  CHECK(found && found->Origin && strstr(found->Origin, "SYNTHETIC"));
  CHECK(found && found->LoaderVersion && strcmp(found->LoaderVersion, MODEL_LOADER) == 0);

  /* Accepted: exact device, subsystem, digest and loader. */
  found = NULL;
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, MODEL_LOADER,
                              &found) == GU_PROM_REF_ACCEPTED);
  CHECK(found != NULL);
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, MODEL_LOADER,
                              NULL) == GU_PROM_REF_ACCEPTED);

  /* Unknown card: identity misses, and the caller is told nothing was found. */
  found = NULL;
  CHECK(GuPromReferenceDecide(0x2685U, MODEL_SUBSYS, MODEL_DIGEST, MODEL_LOADER,
                              &found) == GU_PROM_REF_UNKNOWN_CARD);
  CHECK(found == NULL);
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, 0x0, MODEL_DIGEST, MODEL_LOADER, NULL) ==
        GU_PROM_REF_UNKNOWN_CARD);

  /* Known card, wrong ROM: a different finding from an unknown card, with the
   * reference reported so the log can say which card it expected. */
  found = NULL;
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS,
                              "0000000000000000000000000000000000000000000000000000000000000000",
                              MODEL_LOADER, &found) == GU_PROM_REF_DIGEST_MISMATCH);
  CHECK(found != NULL);
  /* One hex digit different is a mismatch, not a near miss. */
  {
    char edited[65];
    memcpy(edited, MODEL_DIGEST, sizeof(edited));
    edited[63] = (edited[63] == 'b') ? 'c' : 'b';
    CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, edited, MODEL_LOADER,
                                NULL) == GU_PROM_REF_DIGEST_MISMATCH);
  }

  /* Right ROM, different pinned bundle: the capture no longer describes what
   * this image stages. */
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, "610.43.04",
                              NULL) == GU_PROM_REF_LOADER_MISMATCH);
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, "",
                              NULL) == GU_PROM_REF_LOADER_MISMATCH);
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, NULL,
                              NULL) == GU_PROM_REF_LOADER_MISMATCH);
  /* Loader compared case-sensitively and in full. */
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, "610.43.030",
                              NULL) == GU_PROM_REF_LOADER_MISMATCH);

  /* Malformed digests never reach the comparison. */
  {
    char shortd[64], longd[66], upper[65];
    memcpy(shortd, MODEL_DIGEST, 63);
    shortd[63] = '\0';
    memcpy(longd, MODEL_DIGEST, 65);
    longd[64] = '0';
    longd[65] = '\0';
    memcpy(upper, MODEL_DIGEST, 65);
    for (int i = 0; i < 64; i++)
      if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] = (char)(upper[i] - 32);
    CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, shortd, MODEL_LOADER,
                                NULL) == GU_PROM_REF_MALFORMED);
    CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, longd, MODEL_LOADER,
                                NULL) == GU_PROM_REF_MALFORMED);
    CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, upper, MODEL_LOADER,
                                NULL) == GU_PROM_REF_MALFORMED);
    CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, NULL, MODEL_LOADER,
                                NULL) == GU_PROM_REF_MALFORMED);
    CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
                                MODEL_LOADER, NULL) == GU_PROM_REF_MALFORMED);
  }
  /* Malformed is reported before identity: a broken readout is not a card
   * answer, and must not be logged as "unknown card". */
  CHECK(GuPromReferenceDecide(0x2685U, 0x0, NULL, NULL, NULL) == GU_PROM_REF_MALFORMED);
#else
  /* The shipped table: no captured reference, so nothing is admitted - not even
   * the card the synthetic build would have accepted. */
  CHECK(GuPromReferenceCount() == 0);
  CHECK(GuPromReferenceFind(MODEL_DEVICE, MODEL_SUBSYS) == NULL);
  CHECK(GuPromReferenceFind(0, 0) == NULL);
  CHECK(GuPromReferenceDecide(MODEL_DEVICE, MODEL_SUBSYS, MODEL_DIGEST, MODEL_LOADER,
                              NULL) == GU_PROM_REF_UNKNOWN_CARD);
  CHECK(GuPromReferenceDecide(0, 0, MODEL_DIGEST, MODEL_LOADER, NULL) ==
        GU_PROM_REF_UNKNOWN_CARD);
#endif

  if (failures) {
    printf("test_prom_reference: %d checks failed\n", failures);
    return 1;
  }
  printf("test_prom_reference: OK (%s)\n",
#ifdef GU_PROM_REFERENCE_SYNTHETIC
         "synthetic reference"
#else
         "no captured reference"
#endif
  );
  return 0;
}
