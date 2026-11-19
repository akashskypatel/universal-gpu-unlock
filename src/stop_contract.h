/* SPDX-License-Identifier: GPL-2.0-only */

/* Pre-Stop state capture and the per-target contract it feeds.
 *
 * The only detach path in this tree calls the stock NVIDIA UEFI driver's Stop
 * callback for the exact research cards (AD102 `0x2684`, GA102 `0x220a`). That
 * callback can write BAR0 and poll without a proven local bound, so the release
 * is refused wholesale for those two device ids. A wholesale refusal is safe but
 * blind: it cannot say what would have to be true for the answer to change, and
 * nothing on the path records what was actually observed before the callback.
 *
 * This module is that missing piece. It is a pure contract over an explicit
 * snapshot:
 *
 *   observation  = what the image can see at the moment before the first Stop
 *   requirement  = one of the ten facts the plan requires for admission
 *   decision     = ALLOW only when every required fact holds
 *
 * The decision fails closed by construction: a zero-initialised observation is
 * refused, and a missing requirement is named. The four facts that cannot be
 * observed from source - an independent console, the target's firmware GOP being
 * gone, a flushed-and-closed intent log, the comparison of the stock 617.42
 * Booter provenance, an executed recovery procedure and a measured Stop bound -
 * are carried as fields rather than assumed, so the log says which ones are
 * missing instead of implying the whole set passed.
 *
 * Nothing here authenticates, admits a card, or makes the Stop safe. It converts
 * "refused because the device id is on a list" into "refused because these
 * specific facts are absent, here they are", which is what a hardware run has to
 * fill in.
 */
#pragma once

#if defined(GU_STOP_CONTRACT_HOST_TEST)
#include <stddef.h>
typedef unsigned long GU_SC_UINTN;
typedef unsigned int GU_SC_UINT32;
typedef unsigned char GU_SC_BOOLEAN;
#define GU_SC_TRUE 1
#define GU_SC_FALSE 0
#else
#include <Uefi.h>
typedef UINTN GU_SC_UINTN;
typedef UINT32 GU_SC_UINT32;
typedef BOOLEAN GU_SC_BOOLEAN;
#define GU_SC_TRUE TRUE
#define GU_SC_FALSE FALSE
#endif

/* One bit per requirement, in the order the plan lists them. */
#define GU_STOP_REQ_OWNER 0x0001U          /* exactly one verified owner, no fault */
#define GU_STOP_REQ_COLD 0x0002U           /* the stock values are still latched */
#define GU_STOP_REQ_CONSOLE 0x0004U        /* an AMD display function owns the console */
#define GU_STOP_REQ_PEERS 0x0008U          /* every non-target card is unchanged */
#define GU_STOP_REQ_TARGET_GOP 0x0010U     /* no firmware GOP remains for the target */
#define GU_STOP_REQ_LOG 0x0020U            /* the intent log is flushed and closed */
#define GU_STOP_REQ_RECOVERY 0x0040U       /* the recovery procedure has been executed */
#define GU_STOP_REQ_SIGNED_BOOTER 0x0080U  /* 617.42 Booter provenance compared */
#define GU_STOP_REQ_BOUND 0x0100U          /* the Stop callback has a measured bound */
#define GU_STOP_REQ_DMA 0x0200U            /* DMA is provably idle after the Stop */

#define GU_STOP_REQ_ALL 0x03FFU

typedef struct {
  GU_SC_UINT32 DeviceId;
  /* Observed by the image, from state it already validates. */
  GU_SC_BOOLEAN Owner;        /* ReleaseOwnerMatches() held and the plan was untouched */
  GU_SC_BOOLEAN Cold;         /* GuAd104ColdEntry(): the stock selectors are latched */
  GU_SC_BOOLEAN Console;      /* an AMD display function owns the console */
  GU_SC_BOOLEAN Peers;        /* GuVerifyActivePeers() passed for every non-target card */
  /* Not observable in this image. Kept as fields so the record cannot claim
   * them by omission; nothing in the tree sets them. */
  GU_SC_BOOLEAN TargetGopGone;
  GU_SC_BOOLEAN LogClosed;
  GU_SC_BOOLEAN Recovery;
  GU_SC_BOOLEAN SignedBooter;
  GU_SC_BOOLEAN Bound;
  GU_SC_BOOLEAN DmaIdle;
} GU_STOP_OBSERVATION;

/* The fact-to-bit answer set: every requirement that does not hold. */
GU_SC_UINT32 GuStopContractMissing(const GU_STOP_OBSERVATION *observation);

/* ALLOW only when nothing is missing. A null observation is refused. */
GU_SC_BOOLEAN GuStopContractAllows(const GU_STOP_OBSERVATION *observation);

/* "ALLOW" or "REFUSE", for a log line that does not need a second vocabulary. */
const char *GuStopContractDecision(GU_SC_UINT32 missing);

/* Stable short name of one requirement bit, or "UNKNOWN" for a bit that is not
 * one of the ten. Used to spell the missing set out in one line. */
const char *GuStopContractRequirementName(GU_SC_UINT32 bit);

/* Write the missing set as comma-separated names into Out, always terminated
 * when Capacity is non-zero. Returns the number of characters written. A
 * complete observation writes "none". */
GU_SC_UINTN GuStopContractDescribe(GU_SC_UINT32 missing, char *Out, GU_SC_UINTN Capacity);

/* The requirements, in order, for a caller that wants to iterate rather than
 * switch. Index 0 is GU_STOP_REQ_OWNER; index Count is NULL. */
const GU_SC_UINT32 *GuStopContractRequirements(GU_SC_UINTN *Count);
