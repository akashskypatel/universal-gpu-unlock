/* SPDX-License-Identifier: GPL-2.0-only */
/* Host evidence for the persisted boot record: validation, phase transitions,
 * and the rule that a corrupt record is refused rather than repaired. No
 * firmware, no GPU, no NVRAM. */
#include "../src/boot_state.h" /* compile with -DGU_BOOT_STATE_HOST_TEST */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x);return 1;}}while(0)

static unsigned char store[64];
static UINTN store_len;
static int store_present, writes, deletes, fail_write, drop_write;
static int order_present, order_unreadable, drop_delete, delete_read_error;

static EFI_STATUS FakeGetVariable(const void *Name, EFI_GUID *Vendor, UINT32 *Attributes,
                                  UINTN *Size, VOID *Data) {
  if (!Name || !Vendor || !Size) return EFI_INVALID_PARAMETER;
  if(Vendor->a==0x8be4df61U) {
    if(order_unreadable)return EFI_DEVICE_ERROR;
    if(order_present)return EFI_BUFFER_TOO_SMALL;
    return EFI_NOT_FOUND;
  }
  if(Vendor->a!=0x2a7d54c1U)return EFI_INVALID_PARAMETER;
  if(delete_read_error&&deletes)return EFI_DEVICE_ERROR;
  if (!store_present) return EFI_NOT_FOUND;
  if (*Size < store_len) {
    *Size = store_len;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (!Data) return EFI_INVALID_PARAMETER;
  memcpy(Data, store, store_len);
  *Size = store_len;
  if (Attributes) *Attributes = EFI_VARIABLE_NON_VOLATILE |
      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  return EFI_SUCCESS;
}

static EFI_STATUS FakeSetVariable(const void *Name, EFI_GUID *Vendor, UINT32 Attributes,
                                  UINTN Size, VOID *Data) {
  if (!Name || !Vendor || Vendor->a!=0x2a7d54c1U) return EFI_INVALID_PARAMETER;
  if (fail_write) return EFI_DEVICE_ERROR;
  if (!Data || !Size) {
    if (!store_present) return EFI_NOT_FOUND;
    if(drop_delete)return EFI_SUCCESS;
    store_present = 0;
    store_len = 0;
    deletes++;
    return EFI_SUCCESS;
  }
  if (Size > sizeof(store)) return EFI_OUT_OF_RESOURCES;
  if ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0) return EFI_INVALID_PARAMETER;
  if(drop_write)return EFI_SUCCESS;
  memcpy(store, Data, Size);
  store_len = Size;
  store_present = 1;
  writes++;
  return EFI_SUCCESS;
}

static EFI_STATUS FakeAllocatePool(int Type, UINTN Size, VOID **Out) {
  (void)Type;
  *Out = malloc(Size);
  return *Out ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS FakeFreePool(VOID *Buffer) { free(Buffer); return EFI_SUCCESS; }

static EFI_BOOT_SERVICES boot = {0};
static EFI_RUNTIME_SERVICES rt = {0};
static EFI_SYSTEM_TABLE table = {0};

static void Reset(void) {
  memset(store, 0, sizeof(store));
  store_len = 0;
  store_present = 0;
  writes = deletes = 0;
  fail_write = drop_write = 0;
  order_present=order_unreadable=drop_delete=delete_read_error=0;
}

static GU_BOOT_STATE Sample(void) {
  GU_BOOT_STATE s;
  memset(&s, 0, sizeof(s));
  s.Magic = GU_BOOT_STATE_MAGIC;
  s.BootCount = 7;
  s.Phase = GU_BOOT_STATE_OUTCOME;
  s.Status = 0;
  s.DeviceId = 0x2684;
  s.Segment = 0;
  s.Bus = 1;
  s.Device = 0;
  s.Function = 0;
  s.Armed = 1;
  return s;
}

static int CheckReadValidation(void) {
  GU_BOOT_STATE out;
  Reset();
  /* Absent. */
  CHECK(GuBootStateRead(&table, &out) == EFI_NOT_FOUND);
  CHECK(out.Magic == 0 && out.BootCount == 0);

  /* Round-trip. */
  GU_BOOT_STATE sample = Sample();
  CHECK(GuBootStateWrite(&table, &sample) == EFI_SUCCESS);
  CHECK(writes == 1 && store_len == sizeof(sample));
  CHECK(GuBootStateRead(&table, &out) == EFI_SUCCESS);
  CHECK(memcmp(&out, &sample, sizeof(out)) == 0);

  /* Short value: corruption, never a "compatible older record". */
  store_len = sizeof(sample) - 1;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);
  CHECK(out.Magic == 0);

  /* Long value. */
  store_len = sizeof(sample) + 1;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);

  /* Bad magic. */
  store_len = sizeof(sample);
  memcpy(store, &sample, sizeof(sample));
  store[0] ^= 0xff;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);

  /* Unknown phase. */
  memcpy(store, &sample, sizeof(sample));
  ((GU_BOOT_STATE *)store)->Phase = 9;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);

  /* Nonzero reserved byte. */
  memcpy(store, &sample, sizeof(sample));
  ((GU_BOOT_STATE *)store)->Reserved[1] = 1;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);

  /* Out-of-range PCI field. */
  memcpy(store, &sample, sizeof(sample));
  ((GU_BOOT_STATE *)store)->Device = 0x20;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);

  /* Armed is a flag, not a byte. */
  memcpy(store, &sample, sizeof(sample));
  ((GU_BOOT_STATE *)store)->Armed = 2;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);

  /* A refused record is never rewritten by the read path. */
  unsigned before = (unsigned)writes;
  CHECK(GuBootStateRead(&table, &out) == EFI_COMPROMISED_DATA);
  CHECK((unsigned)writes == before);

  /* Write-side validation matches read-side validation. */
  GU_BOOT_STATE bad = Sample();
  bad.Magic = 0;
  CHECK(GuBootStateWrite(&table, &bad) == EFI_INVALID_PARAMETER);
  bad = Sample();
  bad.Reserved[0] = 3;
  CHECK(GuBootStateWrite(&table, &bad) == EFI_INVALID_PARAMETER);
  CHECK(GuBootStateWrite(NULL, &sample) == EFI_INVALID_PARAMETER);

  /* A table without runtime services cannot be mistaken for "no record". */
  EFI_SYSTEM_TABLE bare = {0};
  CHECK(GuBootStateRead(&bare, &out) == EFI_UNSUPPORTED);
  CHECK(GuBootStateWrite(&bare, &sample) == EFI_UNSUPPORTED);
  CHECK(GuBootStateDeleteReviewed(&bare,&sample) == EFI_UNSUPPORTED);
  return 0;
}

static int CheckTransitions(void) {
  GU_BOOT_STATE out;
  Reset();

  /* First boot: intent from nothing. The count does not advance yet, so a
   * power loss between intent and outcome still reads as an interrupted boot. */
  CHECK(GuBootStateRecord(&table, NULL, GU_BOOT_STATE_INTENT, 0, 0x2684,
                          0, 1, 0, 0, 0) == EFI_SUCCESS);
  CHECK(GuBootStateRead(&table, &out) == EFI_SUCCESS);
  CHECK(out.Phase == GU_BOOT_STATE_INTENT && out.BootCount == 0 && !out.Armed);
  CHECK(out.DeviceId == 0x2684 && out.Bus == 1 && out.Function == 0);

  /* Outcome completes the boot. */
  GU_BOOT_STATE previous = out;
  CHECK(GuBootStateRecord(&table, &previous, GU_BOOT_STATE_OUTCOME, 0x8000000000000003ULL,
                          0x2684, 0, 1, 0, 0, 0) == EFI_SUCCESS);
  CHECK(GuBootStateRead(&table, &out) == EFI_SUCCESS);
  CHECK(out.Phase == GU_BOOT_STATE_OUTCOME && out.BootCount == 1);
  CHECK(out.Status == 0x8000000000000003ULL && !out.Armed);

  /* A successful outcome sets armed. */
  previous = out;
  CHECK(GuBootStateRecord(&table, &previous, GU_BOOT_STATE_OUTCOME, 0, 0x2684,
                          0, 1, 0, 0, 1) == EFI_SUCCESS);
  CHECK(GuBootStateRead(&table, &out) == EFI_SUCCESS);
  CHECK(out.BootCount == 2 && out.Armed == 1);

  /* Success without storing the new record is not a restart barrier. */
  previous = out;
  drop_write = 1;
  CHECK(GuBootStateRecord(&table, &previous, GU_BOOT_STATE_INTENT, 0, 0x2684,
                          0, 1, 0, 0, 0) == EFI_COMPROMISED_DATA);
  CHECK(GuBootStateRead(&table, &out) == EFI_SUCCESS);
  CHECK(out.Phase == GU_BOOT_STATE_OUTCOME);
  drop_write = 0;

  /* A write failure is reported and changes nothing. */
  previous = out;
  fail_write = 1;
  CHECK(GuBootStateRecord(&table, &previous, GU_BOOT_STATE_INTENT, 0, 0x2684,
                          0, 1, 0, 0, 0) == EFI_DEVICE_ERROR);
  CHECK(GuBootStateRead(&table, &out) == EFI_SUCCESS);
  CHECK(out.Phase == GU_BOOT_STATE_OUTCOME && out.BootCount == 2);

  fail_write=0;
  return 0;
}

static int CheckReviewedDeletion(void) {
  GU_BOOT_STATE reviewed,current,stale;
  Reset();
  CHECK(GuBootStateDeleteReviewed(&table,NULL)==EFI_INVALID_PARAMETER);
  reviewed=Sample();
  CHECK(GuBootStateWrite(&table,&reviewed)==EFI_SUCCESS);
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_NOT_READY);
  reviewed.Phase=GU_BOOT_STATE_INTENT;reviewed.Armed=0;
  CHECK(GuBootStateWrite(&table,&reviewed)==EFI_SUCCESS);
  stale=reviewed;stale.BootCount++;
  CHECK(GuBootStateDeleteReviewed(&table,&stale)==EFI_COMPROMISED_DATA);
  CHECK(store_present&&deletes==0);
  order_present=1;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_NOT_READY);
  order_present=0;order_unreadable=1;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_DEVICE_ERROR);
  order_unreadable=0;
  store[0]^=1;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_COMPROMISED_DATA);
  store[0]^=1;
  fail_write=1;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_DEVICE_ERROR);
  fail_write=0;drop_delete=1;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_COMPROMISED_DATA);
  CHECK(GuBootStateRead(&table,&current)==EFI_SUCCESS);
  CHECK(memcmp(&current,&reviewed,sizeof(current))==0&&deletes==0);
  drop_delete=0;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_SUCCESS);
  CHECK(!store_present&&deletes==1);
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_NOT_FOUND);
  CHECK(deletes==1);
  reviewed.Phase=GU_BOOT_STATE_OUTCOME;reviewed.Status=EFI_DEVICE_ERROR;
  CHECK(GuBootStateWrite(&table,&reviewed)==EFI_SUCCESS);
  delete_read_error=1;deletes=0;
  CHECK(GuBootStateDeleteReviewed(&table,&reviewed)==EFI_DEVICE_ERROR);
  CHECK(!store_present&&deletes==1); /* mutation occurred, confirmation failed */
  return 0;
}

int main(void) {
  boot.AllocatePool = FakeAllocatePool;
  boot.FreePool = FakeFreePool;
  rt.GetVariable = FakeGetVariable;
  rt.SetVariable = FakeSetVariable;
  table.BootServices = &boot;
  table.RuntimeServices = &rt;

  if (CheckReadValidation()) return 1;
  if (CheckTransitions()) return 1;
  if (CheckReviewedDeletion()) return 1;
  printf("test_boot_state: %u checks OK\n", checks);
  return 0;
}
