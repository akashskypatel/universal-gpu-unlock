/* SPDX-License-Identifier: GPL-2.0-only */

/* Per-card firmware acceptance: an explicit allowlist of captured references.
 *
 * `prom_identity.h` answers one question - is this 1 MiB readout a
 * structurally valid VBIOS that names this PCI device - and that answer is worth
 * little on its own: any card of the right family can present one. It is not
 * authentication, and the log has always said so
 * (`SIGNATURE_AUTHENTICATED=0`).
 *
 * This module is the authenticated half, as far as anything can be settled
 * offline. A reference is a tuple captured from a real card:
 *
 *     (device id, subsystem id, ROM SHA-256, loader version)
 *
 * A card is admitted only by an exact hit on all four. There is no prefix match,
 * no family match and no wildcard: a miss is a refusal, and the refusal says
 * which field missed.
 *
 * The **default build carries no reference** (`GuPromReferenceCount()` is 0).
 * That is the fail-closed default working as intended: with
 * `GU_PROM_REFERENCE_REQUIRED` such an image admits no card at all, which is the
 * correct answer until somebody has captured one. Capturing them is what the
 * read-only diagnostic is for - it prints the digest, the device and the
 * subsystem of the card in front of you, and stays usable precisely because the
 * requirement is checked at preparation, never in the diagnostic.
 *
 * Two cards have now been captured. `GU_PROM_REFERENCE_CAPTURED` selects a table
 * of those two rows, and nothing else adds a row: the table is compiled in, so a
 * reference can only arrive by rebuilding the image and editing this file, and
 * the rows are held to the recorded digests by
 * `tests/test_prom_reference_contract.py`. Defining it together with the
 * synthetic entry is a compile error, so one image cannot admit both a captured
 * card and the simulator's model.
 *
 * The synthetic entry exists for one reason: the simulator's model ROM is not a
 * captured card, so admitting it must be a deliberate, test-only build. It is
 * rejected with any firmware build.
 */
#ifndef GU_PROM_REFERENCE_H
#define GU_PROM_REFERENCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hosted stand-ins keep this module compilable outside EDK II, like
 * `stock_profile.h` and `prom_identity.h`. */
#if defined(GU_PROM_REFERENCE_HOST_TEST)
typedef unsigned int  GU_REF_UINT32;
typedef unsigned long GU_REF_UINTN;
#else
#include <Uefi.h>
typedef UINT32 GU_REF_UINT32;
typedef UINTN  GU_REF_UINTN;
#endif

#define GU_PROM_REFERENCE_DIGEST_CHARS 64U

typedef struct {
  GU_REF_UINT32 DeviceId;          /* PCI device id, e.g. 0x2684 */
  GU_REF_UINT32 SubsystemId;       /* config 0x2c: device | vendor << 16 */
  const char   *RomSha256;         /* 64 lowercase hex digits of the 1 MiB readout */
  const char   *LoaderVersion;     /* bundle this reference was captured with */
  const char   *Origin;            /* where the capture came from */
} GU_PROM_REFERENCE;

typedef enum {
  GU_PROM_REF_ACCEPTED = 0,
  GU_PROM_REF_UNKNOWN_CARD,      /* no reference names this device/subsystem */
  GU_PROM_REF_DIGEST_MISMATCH,   /* the card is known, its ROM is not */
  GU_PROM_REF_LOADER_MISMATCH,   /* captured with a different pinned bundle */
  GU_PROM_REF_MALFORMED,         /* the presented digest is not 64 hex digits */
} GU_PROM_REF_DECISION;

/* How many references this build carries. Zero is the expected value in the
 * shipped image and is not a defect. */
GU_REF_UINTN GuPromReferenceCount(void);

/* Exact lookup by identity, for reporting which card a build knows about. */
const GU_PROM_REFERENCE *GuPromReferenceFind(GU_REF_UINT32 DeviceId,
                                             GU_REF_UINT32 SubsystemId);

/* Which table this build compiled in, as one token for a log line: NONE,
 * CAPTURED or SYNTHETIC. Never NULL.
 *
 * It exists because a run log could not otherwise say which build produced it:
 * the prepare image refuses before it reads a PROM, so an image carrying the
 * captured table and one carrying none printed exactly the same lines, and two
 * boots of the two builds were byte-identical files. A log now names its own
 * table, and `GuPromReferenceCount()` beside it says how many rows that table
 * has, so a receipt and a boot can be matched to each other without trusting a
 * filename. */
const char *GuPromReferenceMode(void);

/* The decision for one observed card. `Digest` is 64 lowercase hex digits.
 * On return, `Found` (optional) names the reference that matched the identity,
 * so a caller can distinguish "unknown card" from "known card, wrong ROM". */
GU_PROM_REF_DECISION GuPromReferenceDecide(GU_REF_UINT32 DeviceId,
                                           GU_REF_UINT32 SubsystemId,
                                           const char *Digest,
                                           const char *LoaderVersion,
                                           const GU_PROM_REFERENCE **Found);

/* A stable token for logs and tests: ACCEPTED, UNKNOWN_CARD, DIGEST_MISMATCH,
 * LOADER_MISMATCH, MALFORMED. Never NULL. */
const char *GuPromReferenceReason(GU_PROM_REF_DECISION Decision);

#ifdef __cplusplus
}
#endif

#endif /* GU_PROM_REFERENCE_H */
