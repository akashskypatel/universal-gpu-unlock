/* SPDX-License-Identifier: GPL-2.0-only */

#include "prom_reference.h"

#include <string.h>

/* A test-only reference must never reach a firmware image: it describes the
 * simulator's model ROM, not a card anybody has. Both markers are rejected in
 * unhosted builds, so this cannot be turned on by accident through a build flag
 * that also reaches silicon. */
#if defined(GU_PROM_REFERENCE_SYNTHETIC) && !defined(GU_PROM_REFERENCE_HOST_TEST) && \
    !defined(GU_SIMULATED_ACTUATION)
#error The synthetic PROM reference is test-only: it describes the simulator model, not a captured card
#endif
#if defined(GU_PROM_REFERENCE_SYNTHETIC) && __STDC_HOSTED__ != 1 && \
    !defined(GU_PROM_REFERENCE_HOST_TEST)
#error The synthetic PROM reference must not be part of a firmware image
#endif

#if defined(GU_PROM_REFERENCE_SYNTHETIC) && defined(GU_PROM_REFERENCE_CAPTURED)
#error One reference table per build: the synthetic model ROM and a captured card cannot both be admitted
#endif

#if defined(GU_PROM_REFERENCE_CAPTURED)
/* Two cards, both captured on this bench: the donor pair the session has been
 * reading since -0006. Every field is a recorded observation, not a guess -
 * device and subsystem from the PCI identity the image prints on each boot, the
 * digest from the 1 MiB readout at BAR0 + 0x300000 that three separate boots
 * produced (the same value `docs/DONOR_HARDWARE_SESSION.md` records), and the
 * loader version is the pinned bundle this capture belongs to.
 *
 * A row here admits the card only when all four fields match. It says nothing
 * about whether the firmware authenticates: `GuAd104PromMatches` still logs
 * SIGNATURE_AUTHENTICATED=0 on the VERIFIED path, and this table cannot change
 * that, because it holds digests of what was read rather than signatures of what
 * will run. It is a build switch on purpose: the shipped image carries no
 * reference, and an image that carries one says so by being built with it. */
static const GU_PROM_REFERENCE References[] = {
  /* RTX 4090, 10de:2684, subsystem 1043:88e6, ucode 9, 2 signatures. */
  { 0x2684U, 0x88e61043U,
    "422ea53f6b3393720f991fa9e189efefeefce98d1f54486acbfa0ecb96a570ad",
    "610.43.03",
    "captured: donor 01:00.0, PROM-0000-01-00-0-0000.BIN, -0008 digest = -0009 file = -0010 repeat" },
  /* RTX 3080, 10de:220a, subsystem 19da:b612, ucode 9, 3 signatures. */
  { 0x220aU, 0xb61219daU,
    "742549cf1a4b2ce6e1e854af35655e90b89a9a205603f03be2c096077dac9165",
    "610.43.03",
    "captured: donor 04:00.0, PROM-0000-04-00-0-0000.BIN, -0008 digest = -0009 file = -0010 repeat" },
};
#define GU_PROM_REFERENCE_TABLE_COUNT \
  (sizeof(References) / sizeof(References[0]))
#elif defined(GU_PROM_REFERENCE_SYNTHETIC)
/* The simulator's own model ROM: 10de:2684, subsystem a140:10de, the synthetic
 * 1 MiB image `sim/sim_gpu.c` builds. Pinning it here is also a drift check - if
 * the model changes, the row that admits it fails and the digest is re-derived
 * deliberately, rather than a different card quietly becoming admissible. */
static const GU_PROM_REFERENCE References[] = {
  { 0x2684U, 0xa14010deU,
    "fffd2b48a1b34a5a5ccdba880cf9195d618e31a9a33176ef5a80dc0b244aa8cb",
    "610.43.03",
    "SYNTHETIC: simulator model ROM, not a captured card" },
};
#define GU_PROM_REFERENCE_TABLE_COUNT \
  (sizeof(References) / sizeof(References[0]))
#else
/* No card has been captured. The table holds one placeholder and the count is
 * zero, so nothing can match by accident and `GuPromReferenceFind` never
 * returns a pointer into it. */
static const GU_PROM_REFERENCE References[] = {
  { 0U, 0U, NULL, NULL, NULL },
};
#define GU_PROM_REFERENCE_TABLE_COUNT 0U
#endif

GU_REF_UINTN GuPromReferenceCount(void) {
  return (GU_REF_UINTN)GU_PROM_REFERENCE_TABLE_COUNT;
}

const GU_PROM_REFERENCE *GuPromReferenceFind(GU_REF_UINT32 DeviceId,
                                             GU_REF_UINT32 SubsystemId) {
  for (GU_REF_UINTN i = 0; i < GuPromReferenceCount(); i++) {
    if (References[i].DeviceId == DeviceId &&
        References[i].SubsystemId == SubsystemId)
      return &References[i];
  }
  return NULL;
}

static int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1; /* uppercase is not accepted: the manifest prints lowercase */
}

static int DigestLooksValid(const char *Digest) {
  if (!Digest) return 0;
  for (GU_REF_UINTN i = 0; i < GU_PROM_REFERENCE_DIGEST_CHARS; i++)
    if (HexDigit(Digest[i]) < 0) return 0;
  return Digest[GU_PROM_REFERENCE_DIGEST_CHARS] == '\0';
}

/* Compared in full, without an early exit: the cost is 64 characters and the
 * habit is worth more than the cycles. */
static int DigestEquals(const char *a, const char *b) {
  unsigned diff = 0;
  for (GU_REF_UINTN i = 0; i < GU_PROM_REFERENCE_DIGEST_CHARS; i++)
    diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
  return diff == 0;
}

static int TextEquals(const char *a, const char *b) {
  if (!a || !b) return 0;
  return strcmp(a, b) == 0;
}

GU_PROM_REF_DECISION GuPromReferenceDecide(GU_REF_UINT32 DeviceId,
                                           GU_REF_UINT32 SubsystemId,
                                           const char *Digest,
                                           const char *LoaderVersion,
                                           const GU_PROM_REFERENCE **Found) {
  const GU_PROM_REFERENCE *reference;

  if (Found) *Found = NULL;
  if (!DigestLooksValid(Digest)) return GU_PROM_REF_MALFORMED;
  reference = GuPromReferenceFind(DeviceId, SubsystemId);
  if (Found) *Found = reference;
  if (!reference) return GU_PROM_REF_UNKNOWN_CARD;
  /* Identity first: a known card with an unexpected ROM is a different finding
   * from an unknown card, and the caller logs them differently. */
  if (!reference->RomSha256 || !DigestEquals(reference->RomSha256, Digest))
    return GU_PROM_REF_DIGEST_MISMATCH;
  if (!TextEquals(reference->LoaderVersion, LoaderVersion))
    return GU_PROM_REF_LOADER_MISMATCH;
  return GU_PROM_REF_ACCEPTED;
}

const char *GuPromReferenceReason(GU_PROM_REF_DECISION Decision) {
  switch (Decision) {
    case GU_PROM_REF_ACCEPTED:        return "ACCEPTED";
    case GU_PROM_REF_UNKNOWN_CARD:    return "UNKNOWN_CARD";
    case GU_PROM_REF_DIGEST_MISMATCH: return "DIGEST_MISMATCH";
    case GU_PROM_REF_LOADER_MISMATCH: return "LOADER_MISMATCH";
    case GU_PROM_REF_MALFORMED:       return "MALFORMED";
  }
  return "INVALID_DECISION";
}
