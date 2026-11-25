/* SPDX-License-Identifier: GPL-2.0-only */
/* Host evidence for runtime target selection: text parsing, canonical
 * formatting, and the variable round-trip. No firmware, no GPU, no PCI. */
#include "../src/gpu_selection.h"   /* compile with -DGU_SELECTION_HOST_TEST */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x);return 1;}}while(0)

/* ---- fake variable store and pool ---- */
static char store[GU_SELECTION_MAX_TEXT + 64];
static UINTN store_len;
static int store_present, store_writes, store_deletes, get_calls;

static EFI_STATUS FakeGetVariable(const void *Name, EFI_GUID *Vendor, UINT32 *Attributes,
                                  UINTN *Size, VOID *Data) {
  get_calls++;
  if (!Name || !Vendor || !Size) return EFI_INVALID_PARAMETER;
  if (!store_present) return EFI_NOT_FOUND;
  if (*Size < store_len) {
    *Size = store_len;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (!Data) return EFI_INVALID_PARAMETER;
  memcpy(Data, store, store_len);
  *Size = store_len;
  if (Attributes) *Attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS;
  return EFI_SUCCESS;
}

static EFI_STATUS FakeSetVariable(const void *Name, EFI_GUID *Vendor, UINT32 Attributes,
                                  UINTN Size, VOID *Data) {
  if (!Name || !Vendor) return EFI_INVALID_PARAMETER;
  if (!Data || !Size) {
    if (!store_present) return EFI_NOT_FOUND;
    store_present = 0;
    store_len = 0;
    store_deletes++;
    return EFI_SUCCESS;
  }
  if (Size > sizeof(store)) return EFI_OUT_OF_RESOURCES;
  memcpy(store, Data, Size);
  store_len = Size;
  store_present = 1;
  store_writes++;
  if ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0 ||
      (Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0)
    return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}

static EFI_STATUS FakeAllocatePool(int Type, UINTN Size, VOID **Out) {
  (void)Type;
  if (!Out) return EFI_INVALID_PARAMETER;
  *Out = malloc(Size);
  return *Out ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS FakeFreePool(VOID *Buffer) {
  free(Buffer);
  return EFI_SUCCESS;
}

static EFI_BOOT_SERVICES boot = {0};
static EFI_RUNTIME_SERVICES rt = {0};
static EFI_SYSTEM_TABLE table = {0};

static void Reset(const char *text) {
  memset(store, 0, sizeof(store));
  store_len = text ? strlen(text) : 0;
  store_present = text != NULL;
  store_writes = store_deletes = get_calls = 0;
}

static int Same(const GU_GPU_LOCATION *l, unsigned s, unsigned b, unsigned d, unsigned f) {
  return l->segment == s && l->bus == b && l->device == d && l->function == f;
}

static int ParseEquals(const char *text, EFI_STATUS want) {
  GU_SELECTION sel;
  EFI_STATUS s = GuSelectionParse((const CHAR8 *)text, strlen(text), &sel);
  if (s != want) {
    fprintf(stderr, "parse(\"%s\") status=%llx want=%llx\n", text,
            (unsigned long long)s, (unsigned long long)want);
    return 0;
  }
  return 1;
}

static int CheckParsing(void) {
  GU_SELECTION sel;
  /* Single location, canonical. */
  CHECK(GuSelectionParse((const CHAR8 *)"0000:01:00.0", 12, &sel) == EFI_SUCCESS);
  CHECK(sel.count == 1 && !sel.all && Same(&sel.list[0], 0, 1, 0, 0));
  /* Mixed case, spaces around tokens and commas, newline padding. */
  const char *spaced = " 000A:0b:1F.7 ,\t0000:01:00.0\r\n";
  CHECK(GuSelectionParse((const CHAR8 *)spaced, strlen(spaced), &sel) == EFI_SUCCESS);
  CHECK(sel.count == 2 && Same(&sel.list[0], 0xa, 0xb, 0x1f, 7));
  CHECK(Same(&sel.list[1], 0, 1, 0, 0));
  /* Explicit ALL policy, case-insensitive. */
  CHECK(GuSelectionParse((const CHAR8 *)"ALL", 3, &sel) == EFI_SUCCESS);
  CHECK(sel.all && sel.count == 0);
  CHECK(GuSelectionParse((const CHAR8 *)" all ", 5, &sel) == EFI_SUCCESS);
  CHECK(sel.all && sel.count == 0);
  /* Empty input is "no request", not an error. */
  CHECK(GuSelectionParse((const CHAR8 *)"", 0, &sel) == EFI_SUCCESS);
  CHECK(sel.count == 0 && !sel.all);
  CHECK(GuSelectionParse((const CHAR8 *)"   \n", 4, &sel) == EFI_SUCCESS);
  CHECK(sel.count == 0 && !sel.all);
  CHECK(GuSelectionParse(NULL, 0, &sel) == EFI_SUCCESS && sel.count == 0);
  /* Malformed forms are refused, and never leave a partial list behind. */
  CHECK(ParseEquals("0000:01:00.8", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00.", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("000:01:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00.0,", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals(",0000:01:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00.0,,0000:02:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00.0;0000:02:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00.0,0000:01:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:00:20.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:00:1f.0", EFI_SUCCESS));
  CHECK(ParseEquals("0000:g1:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("AL", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("ALL,0000:01:00.0", EFI_INVALID_PARAMETER));
  CHECK(ParseEquals("0000:01:00.0x", EFI_INVALID_PARAMETER));
  /* An embedded NUL is malformed, not a terminator. */
  {
    const char with_nul[12] = {'0', '0', '0', '0', ':', '0', 0, '1',
                               ':', '0', '0', '.'};
    CHECK(GuSelectionParse(with_nul, 12, &sel) == EFI_INVALID_PARAMETER);
  }
  /* Exactly GU_SELECTION_MAX entries are accepted; one more is refused. */
  {
    char text[GU_SELECTION_MAX_TEXT];
    UINTN used = 0, last_comma = 0;
    for (unsigned i = 0; i <= GU_SELECTION_MAX; i++) {
      if (i) last_comma = used;
      int n = snprintf(text + used, sizeof(text) - used, "%s%04x:00:00.0",
                       i ? "," : "", i);
      if (n < 0) return 1;
      used += (UINTN)n;
    }
    /* 129 entries: over the bound. */
    CHECK(GuSelectionParse((const CHAR8 *)text, used, &sel) == EFI_INVALID_PARAMETER);
    CHECK(sel.count == 0);
    /* 128 entries: exactly at the bound, accepted. */
    CHECK(GuSelectionParse((const CHAR8 *)text, last_comma, &sel) == EFI_SUCCESS);
    CHECK(sel.count == GU_SELECTION_MAX);
  }
  return 0;
}

static int CheckFormattingRoundTrip(void) {
  static const char *texts[] = {"0000:01:00.0", "0001:0a:1f.7,0000:03:00.1", "ALL"};
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    GU_SELECTION first, second;
    char out[GU_SELECTION_MAX_TEXT];
    UINTN used = 0;
    CHECK(GuSelectionParse((const CHAR8 *)texts[i], strlen(texts[i]), &first) == EFI_SUCCESS);
    CHECK(GuSelectionFormat(&first, out, sizeof(out), &used) == EFI_SUCCESS);
    CHECK(used == strlen(out));
    CHECK(GuSelectionParse((const CHAR8 *)out, used, &second) == EFI_SUCCESS);
    CHECK(second.count == first.count && second.all == first.all);
    for (size_t k = 0; k < first.count; k++)
      CHECK(Same(&second.list[k], first.list[k].segment, first.list[k].bus,
                 first.list[k].device, first.list[k].function));
  }
  /* A canonical render of the spaced form drops the padding. */
  GU_SELECTION spaced;
  char out[64];
  UINTN used = 0;
  const char *raw = " 000A:0b:1F.7 , 0000:01:00.0 ";
  CHECK(GuSelectionParse((const CHAR8 *)raw, strlen(raw), &spaced) == EFI_SUCCESS);
  CHECK(GuSelectionFormat(&spaced, out, sizeof(out), &used) == EFI_SUCCESS);
  CHECK(strcmp(out, "000a:0b:1f.7,0000:01:00.0") == 0);
  /* A truncated output buffer must stay NUL-terminated and never overflow. */
  char tiny[8];
  CHECK(GuSelectionFormat(&spaced, tiny, sizeof(tiny), &used) == EFI_SUCCESS);
  CHECK(used + 1 == sizeof(tiny));
  CHECK(tiny[sizeof(tiny) - 1] == 0);
  return 0;
}

static int CheckVariableRoundTrip(void) {
  GU_SELECTION sel, back;
  CHECK(GuSelectionParse((const CHAR8 *)"0000:01:00.0,0002:0c:02.3", 25, &sel) == EFI_SUCCESS);

  /* Absent variable is EFI_NOT_FOUND, and reads never invent a selection. */
  Reset(NULL);
  memset(&back, 0xaa, sizeof(back));
  CHECK(GuSelectionRead(&table, &back) == EFI_NOT_FOUND);
  CHECK(back.count == 0 && !back.all && !back.from_variable);

  /* Write persists the canonical text with NVRAM attributes. */
  CHECK(GuSelectionWrite(&table, &sel) == EFI_SUCCESS);
  CHECK(store_writes == 1);
  CHECK(store_len == 25 && memcmp(store, "0000:01:00.0,0002:0c:02.3", 25) == 0);

  /* Read returns it, flagged as a variable-sourced selection. */
  memset(&back, 0, sizeof(back));
  CHECK(GuSelectionRead(&table, &back) == EFI_SUCCESS);
  CHECK(back.from_variable && back.count == 2);
  CHECK(Same(&back.list[1], 2, 0xc, 2, 3));

  /* ALL survives the round-trip as a policy, not as an address list. */
  GU_SELECTION all;
  CHECK(GuSelectionParse((const CHAR8 *)"ALL", 3, &all) == EFI_SUCCESS);
  CHECK(GuSelectionWrite(&table, &all) == EFI_SUCCESS);
  memset(&back, 0, sizeof(back));
  CHECK(GuSelectionRead(&table, &back) == EFI_SUCCESS);
  CHECK(back.all && back.count == 0);

  /* An empty policy is never written; deleting an absent one is a no-op. */
  GU_SELECTION empty;
  memset(&empty, 0, sizeof(empty));
  CHECK(GuSelectionWrite(&table, &empty) == EFI_INVALID_PARAMETER);
  store_present = 0;
  CHECK(GuSelectionDelete(&table) == EFI_SUCCESS);
  CHECK(store_deletes == 0);

  /* A stored text longer than the reader's bound is refused, not truncated. */
  Reset(NULL);
  memset(store, 'x', GU_SELECTION_MAX_TEXT + 32);
  store_len = GU_SELECTION_MAX_TEXT + 32;
  store_present = 1;
  memset(&back, 0, sizeof(back));
  CHECK(GuSelectionRead(&table, &back) == EFI_BAD_BUFFER_SIZE);

  /* A malformed stored value is refused and reported as read-only garbage. */
  Reset("0000:01:00.0,not-a-location");
  CHECK(GuSelectionRead(&table, &back) == EFI_INVALID_PARAMETER);
  CHECK(!back.from_variable && back.count == 0);

  /* A table without runtime services is unsupported, not a silent default. */
  EFI_SYSTEM_TABLE bare = {0};
  CHECK(GuSelectionRead(&bare, &back) == EFI_UNSUPPORTED);
  CHECK(GuSelectionWrite(&bare, &sel) == EFI_UNSUPPORTED);
  CHECK(GuSelectionDelete(&bare) == EFI_UNSUPPORTED);
  return 0;
}

int main(void) {
  boot.AllocatePool = FakeAllocatePool;
  boot.FreePool = FakeFreePool;
  rt.GetVariable = FakeGetVariable;
  rt.SetVariable = FakeSetVariable;
  table.BootServices = &boot;
  table.RuntimeServices = &rt;

  if (CheckParsing()) return 1;
  if (CheckFormattingRoundTrip()) return 1;
  if (CheckVariableRoundTrip()) return 1;
  printf("test_gpu_selection: %u checks OK\n", checks);
  return 0;
}
