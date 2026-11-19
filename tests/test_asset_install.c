/* SPDX-License-Identifier: GPL-2.0-only */
/* Host evidence for the staged-asset record: the bundle digest, the
 * CURRENT/STALE decision, and the rule that a corrupt record is refused rather
 * than repaired. No firmware, no GPU, no filesystem, no NVRAM. */
#include "../src/asset_install.h" /* -DGU_ASSET_INSTALL_HOST_TEST */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x);return 1;}}while(0)

#define HASH_A "6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49"
#define HASH_B "44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb"
#define HASH_C "572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"

static const GU_ASSET_REF Files[3] = {
    {"booter_load_ad102.bin", 57672ull, HASH_A},
    {"v67-ad102-four-plm.bin", 64000ull, HASH_B},
    {"gsp_ga10x.bin", 84277400ull, HASH_C},
};
static const CHAR16 Version[] = {'6', '1', '0', '.', '4', '3', '.', '0', '3', 0};
static const GU_ASSET_BUNDLE Bundle = {Version, 3, Files};

static void Digest(const GU_ASSET_BUNDLE *bundle, CHAR8 out[65]) {
  GuAssetInstallBundleDigest(bundle, out);
}

static GU_ASSET_INSTALL Good(void) {
  GU_ASSET_INSTALL record;
  CHAR8 digest[65];
  memset(&record, 0, sizeof(record));
  record.Magic = GU_ASSET_INSTALL_MAGIC;
  record.Files = 3;
  record.BootCount = 7;
  memcpy(record.Version, Version, sizeof(Version));
  Digest(&Bundle, digest);
  memcpy(record.Bundle, digest, sizeof(record.Bundle));
  return record;
}

/* ---- in-memory variable store ---------------------------------------- */

static unsigned char store[512];
static UINTN store_len;
static int store_present, writes, deletes, fail_write;

static EFI_STATUS FakeGetVariable(const void *Name, EFI_GUID *Vendor, UINT32 *Attributes,
                                  UINTN *Size, VOID *Data) {
  if (!Name || !Vendor || !Size) return EFI_INVALID_PARAMETER;
  if (!store_present) return EFI_NOT_FOUND;
  if (*Size < store_len) {
    *Size = store_len;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (!Data) return EFI_INVALID_PARAMETER;
  memcpy(Data, store, store_len);
  *Size = store_len;
  if (Attributes) *Attributes = EFI_VARIABLE_NON_VOLATILE;
  return EFI_SUCCESS;
}

static EFI_STATUS FakeSetVariable(const void *Name, EFI_GUID *Vendor, UINT32 Attributes,
                                  UINTN Size, VOID *Data) {
  if (!Name || !Vendor) return EFI_INVALID_PARAMETER;
  if (fail_write) return EFI_DEVICE_ERROR;
  if (!Data || !Size) {
    if (!store_present) return EFI_NOT_FOUND;
    store_present = 0;
    store_len = 0;
    deletes++;
    return EFI_SUCCESS;
  }
  if (Size > sizeof(store)) return EFI_OUT_OF_RESOURCES;
  if ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0) return EFI_INVALID_PARAMETER;
  memcpy(store, Data, Size);
  store_len = Size;
  store_present = 1;
  writes++;
  return EFI_SUCCESS;
}

static struct MOCK_RUNTIME_SERVICES runtime = {FakeGetVariable, FakeSetVariable};
static EFI_SYSTEM_TABLE table;

static int DigestIsStable(void) {
  CHAR8 first[65], second[65], other[65];
  GU_ASSET_REF changed[3];
  GU_ASSET_BUNDLE bundle;

  Digest(&Bundle, first);
  Digest(&Bundle, second);
  CHECK(strlen(first) == 64);
  CHECK(strcmp(first, second) == 0);

  /* A lower-case hex digest: the record validator accepts nothing else. */
  for (unsigned i = 0; first[i]; i++)
    CHECK(!(first[i] >= 'A' && first[i] <= 'F'));

  memcpy(changed, Files, sizeof(changed));
  changed[0].Bytes++;
  bundle = (GU_ASSET_BUNDLE){Version, 3, changed};
  Digest(&bundle, other);
  CHECK(strcmp(first, other) != 0);

  memcpy(changed, Files, sizeof(changed));
  changed[1].Sha256 = HASH_A;
  bundle = (GU_ASSET_BUNDLE){Version, 3, changed};
  Digest(&bundle, other);
  CHECK(strcmp(first, other) != 0);

  memcpy(changed, Files, sizeof(changed));
  changed[2].Name = "gsp_ga10x_other.bin";
  bundle = (GU_ASSET_BUNDLE){Version, 3, changed};
  Digest(&bundle, other);
  CHECK(strcmp(first, other) != 0);

  /* Order is part of the manifest: the same files reordered is a different
   * bundle, so a stale record cannot pass by coincidence. */
  changed[0] = Files[1];
  changed[1] = Files[0];
  changed[2] = Files[2];
  bundle = (GU_ASSET_BUNDLE){Version, 3, changed};
  Digest(&bundle, other);
  CHECK(strcmp(first, other) != 0);

  /* Truncation is a different bundle too. */
  bundle = (GU_ASSET_BUNDLE){Version, 2, Files};
  Digest(&bundle, other);
  CHECK(strcmp(first, other) != 0);

  /* No usable manifest means no digest, never a digest of nothing. */
  Digest(NULL, other);
  CHECK(other[0] == 0);
  bundle = (GU_ASSET_BUNDLE){Version, 0, Files};
  Digest(&bundle, other);
  CHECK(other[0] == 0);
  bundle = (GU_ASSET_BUNDLE){Version, 3, NULL};
  Digest(&bundle, other);
  CHECK(other[0] == 0);
  return 0;
}

static int ValidationRefusesWhatItCannotProve(void) {
  GU_ASSET_INSTALL record = Good();
  CHECK(GuAssetInstallValidate(&record) == EFI_SUCCESS);
  CHECK(GuAssetInstallValidate(NULL) == EFI_INVALID_PARAMETER);

  record = Good(); record.Magic = 0;
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good(); record.Files = 0;
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good(); record.Files = GU_ASSET_INSTALL_MAX_FILES + 1;
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good();
  memset(record.Version, 'x', sizeof(record.Version)); /* not terminated */
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good(); record.Bundle[63] = 0;             /* short */
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good(); record.Bundle[64] = 'a';           /* not terminated */
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good(); record.Bundle[0] = 'A';            /* upper case */
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  record = Good(); record.Bundle[5] = 'z';            /* not hex */
  CHECK(GuAssetInstallValidate(&record) == EFI_COMPROMISED_DATA);
  return 0;
}

static int StateIsDecidedByTheManifest(void) {
  GU_ASSET_INSTALL record = Good();
  GU_ASSET_REF changed[3];
  GU_ASSET_BUNDLE bundle;

  CHECK(GuAssetInstallState(NULL, &Bundle) == GU_ASSET_INSTALL_ABSENT);
  CHECK(GuAssetInstallState(&record, &Bundle) == GU_ASSET_INSTALL_CURRENT);

  record.Magic = 0;
  CHECK(GuAssetInstallState(&record, &Bundle) == GU_ASSET_INSTALL_CORRUPT);
  record = Good();
  record.Bundle[10] = record.Bundle[10] == 'a' ? 'b' : 'a';
  CHECK(GuAssetInstallState(&record, &Bundle) == GU_ASSET_INSTALL_STALE);
  record = Good();
  record.Files = 2;
  CHECK(GuAssetInstallState(&record, &Bundle) == GU_ASSET_INSTALL_STALE);

  /* A manifest that cannot be digested never makes a record current. */
  CHECK(GuAssetInstallState(&record, NULL) == GU_ASSET_INSTALL_STALE);
  memcpy(changed, Files, sizeof(changed));
  bundle = (GU_ASSET_BUNDLE){Version, 3, NULL};
  CHECK(GuAssetInstallState(&record, &bundle) == GU_ASSET_INSTALL_STALE);
  return 0;
}

static int RecordRoundTripsAndRefusesCorruption(void) {
  GU_ASSET_INSTALL record = Good(), readback;
  memset(&table, 0, sizeof(table));
  table.RuntimeServices = &runtime;

  CHECK(GuAssetInstallRead(&table, &readback) == EFI_NOT_FOUND);
  CHECK(GuAssetInstallWrite(&table, &record) == EFI_SUCCESS);
  CHECK(writes == 1);
  memset(&readback, 0, sizeof(readback));
  CHECK(GuAssetInstallRead(&table, &readback) == EFI_SUCCESS);
  CHECK(memcmp(&readback, &record, sizeof(record)) == 0);
  CHECK(GuAssetInstallState(&readback, &Bundle) == GU_ASSET_INSTALL_CURRENT);

  /* A short value in the variable is refused, not padded or guessed. */
  store_len = sizeof(GU_ASSET_INSTALL) - 1;
  CHECK(GuAssetInstallRead(&table, &readback) == EFI_COMPROMISED_DATA);
  CHECK(GuAssetInstallState(&readback, &Bundle) == GU_ASSET_INSTALL_CORRUPT);
  store_len = sizeof(GU_ASSET_INSTALL);

  /* An invalid record is refused at the write, so it can never be stored. */
  record = Good();
  record.Magic = 0;
  CHECK(GuAssetInstallWrite(&table, &record) == EFI_COMPROMISED_DATA);
  CHECK(writes == 1);
  CHECK(GuAssetInstallWrite(&table, NULL) == EFI_INVALID_PARAMETER);

  /* A failed write leaves the previous record untouched and is reported. */
  record = Good();
  fail_write = 1;
  CHECK(GuAssetInstallWrite(&table, &record) == EFI_DEVICE_ERROR);
  fail_write = 0;
  CHECK(GuAssetInstallRead(&table, &readback) == EFI_SUCCESS);
  CHECK(GuAssetInstallState(&readback, &Bundle) == GU_ASSET_INSTALL_CURRENT);

  CHECK(GuAssetInstallDelete(&table) == EFI_SUCCESS);
  CHECK(deletes == 1);
  CHECK(GuAssetInstallRead(&table, &readback) == EFI_NOT_FOUND);
  CHECK(GuAssetInstallState(&readback, &Bundle) == GU_ASSET_INSTALL_CORRUPT);

  /* No runtime services: refused rather than silently skipped. */
  table.RuntimeServices = NULL;
  CHECK(GuAssetInstallRead(&table, &readback) == EFI_UNSUPPORTED);
  CHECK(GuAssetInstallWrite(&table, &record) == EFI_UNSUPPORTED);
  CHECK(GuAssetInstallDelete(&table) == EFI_UNSUPPORTED);
  return 0;
}

int main(void) {
  if (DigestIsStable()) return 1;
  if (ValidationRefusesWhatItCannotProve()) return 1;
  if (StateIsDecidedByTheManifest()) return 1;
  if (RecordRoundTripsAndRefusesCorruption()) return 1;
  printf("test_asset_install: %u checks OK\n", checks);
  return 0;
}
