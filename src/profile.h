/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "vendor/vbios_610.h"
#define GU_ASSET_COUNT 5U
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
EFI_STATUS GuDiagnosticScan(VOID);
BOOLEAN GuAd104AssetsValid(VOID);
BOOLEAN GuAd104PromMatches(const VOID *, UINTN);
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *);
/* Prepare is read-only on the GPU. It validates ROM, assets, exact PCI/card
 * state and creates the immutable selector latch. No driver release here. */
EFI_STATUS GuAd104Prepare(VOID);
BOOLEAN GuAd104ColdEntry(VOID);
