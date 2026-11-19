/* SPDX-License-Identifier: GPL-2.0-only */
/* Staged-asset record: what the removable media installed, and whether it is
 * still the bundle this image expects.
 *
 * The image re-reads five pinned firmware files on every boot, including an
 * 84 MB GSP image, and today the only place those files can come from is the
 * USB stick the image was launched from. An install step copies them to the
 * configured Windows ESP once, verifying each file against its pinned SHA-256,
 * and records here which bundle it staged.
 *
 * The record is a *claim about bytes*, not an authorization: it cannot admit a
 * card, unlock anything, or make the ESP an accepted place to launch from. It
 * exists so the next boot can say whether the staged copy is still the one this
 * image would load (CURRENT), belongs to a different firmware bundle (STALE), or
 * is missing/unreadable (ABSENT/CORRUPT).
 *
 * Contents are one fixed-size, validated structure: magic, the pinned firmware
 * version, the file count, the boot that staged it, and one digest over the
 * whole compiled manifest. A short, long or corrupt record is refused and never
 * repaired in place.
 */
#pragma once

#if defined(GU_ASSET_INSTALL_HOST_TEST)
#include "../tests/asset_install_mock.h"
#else
#include <Uefi.h>
#endif

#define GU_ASSET_INSTALL_VARIABLE_NAME L"GuAssetInstall"
#define GU_ASSET_INSTALL_MAGIC 0x31495547U /* "GUI1" little-endian */
#define GU_ASSET_INSTALL_VERSION_CHARS 32
#define GU_ASSET_INSTALL_DIGEST_CHARS 65
#define GU_ASSET_INSTALL_MAX_FILES 8

/* State of the staged copy, decided by comparing the record with the manifest
 * this image would actually load. */
#define GU_ASSET_INSTALL_ABSENT 0U  /* no record: nothing was ever staged */
#define GU_ASSET_INSTALL_CURRENT 1U /* staged bundle == expected manifest */
#define GU_ASSET_INSTALL_STALE 2U   /* staged a different firmware bundle */
#define GU_ASSET_INSTALL_CORRUPT 3U /* record present but not valid */

typedef struct {
  UINT32 Magic;
  UINT32 Files;
  UINT64 BootCount; /* boot that completed the staging */
  CHAR16 Version[GU_ASSET_INSTALL_VERSION_CHARS];
  CHAR8 Bundle[GU_ASSET_INSTALL_DIGEST_CHARS]; /* hex digest of the manifest */
} GU_ASSET_INSTALL;

/* A manifest view with no UEFI types in it, so the digest and the comparison
 * can be exercised on the host. */
typedef struct {
  const char *Name;
  unsigned long long Bytes;
  const char *Sha256; /* 64 lowercase hex characters */
} GU_ASSET_REF;

typedef struct {
  const CHAR16 *Version;
  unsigned Count;
  const GU_ASSET_REF *Files;
} GU_ASSET_BUNDLE;

/* Canonical digest over the compiled manifest: name, size and pinned hash of
 * every file, in order. Two manifests that differ anywhere produce different
 * digests, and the same manifest always produces the same one. */
VOID GuAssetInstallBundleDigest(const GU_ASSET_BUNDLE *Bundle,
                                CHAR8 Out[GU_ASSET_INSTALL_DIGEST_CHARS]);

/* Shape, magic, reserved-byte and digest-syntax checks. */
EFI_STATUS GuAssetInstallValidate(const GU_ASSET_INSTALL *Record);

/* ABSENT when no record is supplied; CORRUPT when the record fails validation;
 * CURRENT/STALE by comparing the recorded digest with the bundle. */
UINT32 GuAssetInstallState(const GU_ASSET_INSTALL *Record,
                           const GU_ASSET_BUNDLE *Bundle);

/* "ABSENT" / "CURRENT" / "STALE" / "CORRUPT", for a single-line report. */
const char *GuAssetInstallStateName(UINT32 State);

/* NVRAM access, same attributes and validation discipline as the boot record. */
EFI_STATUS GuAssetInstallRead(EFI_SYSTEM_TABLE *Table, GU_ASSET_INSTALL *Out);
EFI_STATUS GuAssetInstallWrite(EFI_SYSTEM_TABLE *Table,
                               const GU_ASSET_INSTALL *Record);
EFI_STATUS GuAssetInstallDelete(EFI_SYSTEM_TABLE *Table);
