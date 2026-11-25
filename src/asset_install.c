/* SPDX-License-Identifier: GPL-2.0-only */
/* Staged-asset record. One NVRAM variable, fixed size, validated on read.
 * No file access, no GPU access, no control-flow authority. */
#include "asset_install.h"
#include "sha256.h"

#include <string.h>

/* Fork-owned GUID. 5c1f9b30-7d24-4e08-9a17-3f6c2d81b5a2 */
static EFI_GUID InstallGuid =
    {0x5c1f9b30, 0x7d24, 0x4e08, {0x9a, 0x17, 0x3f, 0x6c, 0x2d, 0x81, 0xb5, 0xa2}};

#define GU_ASSET_INSTALL_ATTRIBUTES                               \
  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
   EFI_VARIABLE_RUNTIME_ACCESS)

static int Usable(EFI_SYSTEM_TABLE *t) {
  return t && t->RuntimeServices && t->RuntimeServices->GetVariable &&
         t->RuntimeServices->SetVariable;
}

static BOOLEAN Hex64(const CHAR8 *text) {
  unsigned i;
  if (!text) return FALSE;
  for (i = 0; i < GU_ASSET_INSTALL_DIGEST_CHARS - 1; i++) {
    CHAR8 c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return FALSE;
  }
  return text[GU_ASSET_INSTALL_DIGEST_CHARS - 1] == 0;
}

static BOOLEAN TextEnds(const CHAR16 *text, UINTN limit) {
  UINTN i;
  if (!text) return FALSE;
  for (i = 0; i < limit; i++)
    if (!text[i]) return TRUE;
  return FALSE;
}

EFI_STATUS GuAssetInstallValidate(const GU_ASSET_INSTALL *record) {
  if (!record) return EFI_INVALID_PARAMETER;
  if (record->Magic != GU_ASSET_INSTALL_MAGIC) return EFI_COMPROMISED_DATA;
  if (record->Files == 0 || record->Files > GU_ASSET_INSTALL_MAX_FILES)
    return EFI_COMPROMISED_DATA;
  if (!TextEnds(record->Version, GU_ASSET_INSTALL_VERSION_CHARS))
    return EFI_COMPROMISED_DATA;
  if (!Hex64(record->Bundle)) return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}

/* One canonical byte stream: for every file in manifest order, the name, the
 * decimal size and the pinned hash, each NUL-terminated. Built in a fixed
 * buffer because the manifest is compiled in and bounded. */
VOID GuAssetInstallBundleDigest(const GU_ASSET_BUNDLE *bundle,
                                CHAR8 Out[GU_ASSET_INSTALL_DIGEST_CHARS]) {
  static const CHAR8 hex[] = "0123456789abcdef";
  CHAR8 canonical[4096];
  UINTN length = 0;
  UINT8 digest[32];
  unsigned i, j;
  if (!Out) return;
  Out[0] = 0;
  if (!bundle || !bundle->Files || bundle->Count == 0 ||
      bundle->Count > GU_ASSET_INSTALL_MAX_FILES)
    return;
  for (i = 0; i < bundle->Count; i++) {
    const GU_ASSET_REF *file = &bundle->Files[i];
    unsigned long long value = file->Bytes;
    CHAR8 digits[24];
    unsigned count = 0;
    if (!file->Name || !file->Sha256) { Out[0] = 0; return; }
    for (j = 0; file->Name[j]; j++) {
      if (length + 1 >= sizeof(canonical)) { Out[0] = 0; return; }
      canonical[length++] = file->Name[j];
    }
    canonical[length++] = 0;
    do {
      digits[count++] = (CHAR8)('0' + (value % 10));
      value /= 10;
    } while (value && count < sizeof(digits));
    while (count) {
      if (length + 1 >= sizeof(canonical)) { Out[0] = 0; return; }
      canonical[length++] = digits[--count];
    }
    canonical[length++] = 0;
    for (j = 0; file->Sha256[j]; j++) {
      if (length + 1 >= sizeof(canonical)) { Out[0] = 0; return; }
      canonical[length++] = file->Sha256[j];
    }
    canonical[length++] = 0;
  }
  GuSha256(canonical, length, digest);
  for (i = 0; i < 32; i++) {
    Out[i * 2] = hex[(digest[i] >> 4) & 0xf];
    Out[i * 2 + 1] = hex[digest[i] & 0xf];
  }
  Out[64] = 0;
}

UINT32 GuAssetInstallState(const GU_ASSET_INSTALL *record,
                           const GU_ASSET_BUNDLE *bundle) {
  CHAR8 expected[GU_ASSET_INSTALL_DIGEST_CHARS];
  if (!record) return GU_ASSET_INSTALL_ABSENT;
  if (GuAssetInstallValidate(record) != EFI_SUCCESS)
    return GU_ASSET_INSTALL_CORRUPT;
  GuAssetInstallBundleDigest(bundle, expected);
  /* An unusable manifest cannot make a record current; report it as stale so
   * the caller rebuilds it rather than trusting it. */
  if (!expected[0]) return GU_ASSET_INSTALL_STALE;
  if (record->Files != bundle->Count) return GU_ASSET_INSTALL_STALE;
  if (memcmp(record->Bundle, expected, sizeof(record->Bundle)) != 0)
    return GU_ASSET_INSTALL_STALE;
  return GU_ASSET_INSTALL_CURRENT;
}

const char *GuAssetInstallStateName(UINT32 State) {
  switch (State) {
    case GU_ASSET_INSTALL_CURRENT: return "CURRENT";
    case GU_ASSET_INSTALL_STALE: return "STALE";
    case GU_ASSET_INSTALL_CORRUPT: return "CORRUPT";
    default: return "ABSENT";
  }
}

EFI_STATUS GuAssetInstallRead(EFI_SYSTEM_TABLE *Table, GU_ASSET_INSTALL *Out) {
  if (!Out) return EFI_INVALID_PARAMETER;
  memset(Out, 0, sizeof(*Out));
  if (!Usable(Table)) return EFI_UNSUPPORTED;
  GU_ASSET_INSTALL buffer;
  UINTN size = sizeof(buffer);
  UINT32 attributes = 0;
  memset(&buffer, 0, sizeof(buffer));
  EFI_STATUS s = Table->RuntimeServices->GetVariable(
      (const void *)GU_ASSET_INSTALL_VARIABLE_NAME, &InstallGuid, &attributes,
      &size, &buffer);
  if (s != EFI_SUCCESS) return s;
  /* The variable must be exactly one record: a short or long value is refused
   * here, before any field is trusted. */
  if (size != sizeof(buffer)) return EFI_COMPROMISED_DATA;
  s = GuAssetInstallValidate(&buffer);
  if (s != EFI_SUCCESS) return s;
  *Out = buffer;
  return EFI_SUCCESS;
}

EFI_STATUS GuAssetInstallWrite(EFI_SYSTEM_TABLE *Table,
                               const GU_ASSET_INSTALL *Record) {
  if (!Record) return EFI_INVALID_PARAMETER;
  EFI_STATUS s = GuAssetInstallValidate(Record);
  if (s != EFI_SUCCESS) return s;
  if (!Usable(Table)) return EFI_UNSUPPORTED;
  GU_ASSET_INSTALL buffer = *Record;
  return Table->RuntimeServices->SetVariable(
      (const void *)GU_ASSET_INSTALL_VARIABLE_NAME, &InstallGuid,
      GU_ASSET_INSTALL_ATTRIBUTES, sizeof(buffer), &buffer);
}

EFI_STATUS GuAssetInstallDelete(EFI_SYSTEM_TABLE *Table) {
  if (!Usable(Table)) return EFI_UNSUPPORTED;
  return Table->RuntimeServices->SetVariable(
      (const void *)GU_ASSET_INSTALL_VARIABLE_NAME, &InstallGuid,
      GU_ASSET_INSTALL_ATTRIBUTES, 0, NULL);
}
