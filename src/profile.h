/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "vendor/vbios_610.h"
#include "asset_install.h"
/* The one pinned firmware version every manifest below refers to. The staged
 * bundle records it, and the contract test checks that every pinned path still
 * names it. */
#define GU_ASSET_BUNDLE_VERSION L"610.43.03"

typedef struct {
  const CHAR16 *path;
  const char *name;
  const char *sha256;
  UINTN bytes;
} GU_ARCH_ASSET;
typedef struct {
  GU_ARCH_ASSET entries[GU_ASSET_COUNT];
  const CHAR16 *alternate_gsp_path;
} GU_ARCH_ASSETS;
/* Resolve a manifest from the full vendor/device PCI identity, without
 * consulting or changing the currently bound GPU. This selects reference
 * assets only; it does not establish Windows 617.42 signature acceptance. */
const GU_ARCH_ASSETS *GuGetArchAssetsForPciId(UINT32 pci_id);
const GU_ARCH_ASSETS *GuGetArchAssets(VOID);
/* Project the compiled manifest into the record module's view (names, sizes,
 * pinned digests, version). CPU-only; reads no file and touches no card. */
VOID GuAssetBundleFromArch(const GU_ARCH_ASSETS *,GU_ASSET_REF *,
                           GU_ASSET_BUNDLE *);
/* Read-only report; reject a null, inactive or unbound owner. */
EFI_STATUS GuDiagnosticScan(GU_PLATFORM *owner);
BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform);
/* CPU-only ownership proof; no content validation or hardware dispatch. */
BOOLEAN GuPlatformAssetsOwned(const GU_PLATFORM *platform);
BOOLEAN GuAd104PromMatches(const GU_PLATFORM *platform,const VOID *, UINTN);
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *);
/* Prepare is read-only on the GPU. It validates ROM, assets, exact PCI/card
 * state and creates the immutable selector latch. No driver release here. */
EFI_STATUS GuAd104Prepare(VOID);
BOOLEAN GuAd104ColdEntryFor(GU_PLATFORM *platform);
BOOLEAN GuAd104ColdEntry(VOID);
