/* SPDX-License-Identifier: GPL-2.0-only */

/* Where this image is allowed to run from.
 *
 * The application refuses to run unless it was started from removable media
 * (`GuBootOpen`: "any image not launched from USB is refused"). That rule exists
 * for good reasons - the bundle is re-read from the medium it booted from, the
 * ordered log is written beside it, and a USB stick can be pulled out of a
 * machine that will not boot - but it also means there is no deployable artifact:
 * every run needs the stick, and an ESP-resident copy cannot exist.
 *
 * This module decides the launch question explicitly instead of implicitly, so
 * that relaxing it is a reviewed choice rather than an accident:
 *
 *   USB launch                 always allowed; the install step requires it
 *   ESP-resident launch        only in a build that asks for it (GU_ESP_RESIDENT),
 *                              and only when every one of these holds:
 *                                - the volume is a disk partition, not removable
 *                                - it is the volume that carries the Windows loader
 *                                - the staged bundle record is CURRENT for this image
 *                                - a writable log directory could be opened on it
 *
 * The record is not re-hashed here: the loader verifies every byte against the
 * compiled manifest before anything is used, and this gate only answers "was this
 * image's bundle ever staged on this volume", which is what makes a resident copy
 * self-contained.
 *
 * The decision is a pure function of an explicit observation, so the refusal can
 * be tested without a machine, and a missing fact is named rather than assumed.
 * It answers one question only: *where may this image start from*. It cannot
 * admit a card, unlock anything, or move any of the actuation gates - an
 * ESP-resident image is exactly as restricted as a USB one, minus the stick.
 */
#pragma once

#if defined(GU_LAUNCH_SOURCE_HOST_TEST)
#include <stddef.h>
typedef unsigned long GU_LS_UINTN;
typedef unsigned long long GU_LS_UINT64;
typedef unsigned char GU_LS_BOOLEAN;
#define GU_LS_TRUE 1
#define GU_LS_FALSE 0
#else
#include <Uefi.h>
typedef UINTN GU_LS_UINTN;
typedef UINT64 GU_LS_UINT64;
typedef BOOLEAN GU_LS_BOOLEAN;
#define GU_LS_TRUE TRUE
#define GU_LS_FALSE FALSE
#endif

/* What was observed about the volume this image was started from. */
typedef struct {
  GU_LS_BOOLEAN Removable;        /* the device path names a USB node */
  GU_LS_BOOLEAN Partition;        /* the device path names a partition on a disk */
  GU_LS_BOOLEAN WindowsLoader;    /* \EFI\Microsoft\Boot\bootmgfw.efi is readable */
  GU_LS_BOOLEAN PinnedEsp;        /* the compiled partition GUID matched this volume */
  GU_LS_BOOLEAN BundleRecord;     /* a staged record names this bundle and file count */
  GU_LS_BOOLEAN LogWritable;      /* \AD102-UNLOCK could be opened for writing */
  GU_LS_BOOLEAN EspResidentBuild; /* the image was built with GU_ESP_RESIDENT */
} GU_LAUNCH_OBSERVATION;

#define GU_LAUNCH_ALLOW_REMOVABLE 0U /* started from the stick: unchanged path */
#define GU_LAUNCH_ALLOW_RESIDENT 1U  /* started from the Windows ESP, fully gated */
#define GU_LAUNCH_REFUSE 2U

/* One reason code per way a launch can be refused, so the log names the fact
 * instead of saying "not allowed". */
#define GU_LAUNCH_REASON_NONE 0U
#define GU_LAUNCH_REASON_NOT_REMOVABLE 1U    /* neither USB nor a disk partition */
#define GU_LAUNCH_REASON_BUILD_REFUSES 2U    /* disk launch in a build without -EspResident */
#define GU_LAUNCH_REASON_NOT_WINDOWS_ESP 3U  /* the volume does not carry the Windows loader */
#define GU_LAUNCH_REASON_PIN_MISMATCH 4U     /* a pinned build and a different partition */
#define GU_LAUNCH_REASON_BUNDLE_RECORD 5U /* no staged record, or a different bundle */
#define GU_LAUNCH_REASON_LOG_NOT_WRITABLE 6U

#define GU_LAUNCH_REFUSAL_COUNT 7U

/* Decide, and report the reason. The observation is never modified. */
GU_LS_UINTN GuLaunchSourceDecide(const GU_LAUNCH_OBSERVATION *Observation);

/* Stable name of a decision, and of a reason. "UNKNOWN" for anything else. */
const char *GuLaunchSourceDecisionName(GU_LS_UINTN Decision);
const char *GuLaunchSourceReasonName(GU_LS_UINTN Reason);

/* The reason that applies to a refusal, or GU_LAUNCH_REASON_NONE when the launch
 * is allowed. Both decisions are spelled out in one place so a caller cannot
 * report a reason the decision does not support. */
GU_LS_UINTN GuLaunchSourceReason(const GU_LAUNCH_OBSERVATION *Observation);
