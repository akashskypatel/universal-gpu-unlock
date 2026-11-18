/* SPDX-License-Identifier: GPL-2.0-only */
/* Runtime target selection. No GPU access, no allocation beyond one pooled
 * text buffer, and no write to hardware or NVRAM except GuSelectionWrite. */
#include "gpu_selection.h"

#include <string.h>

/* Fork-owned variable GUID. 5c1f2b40-8e21-4ef7-9d2b-7a113c5e8402 */
static EFI_GUID TargetGuid =
    {0x5c1f2b40, 0x8e21, 0x4ef7, {0x9d, 0x2b, 0x7a, 0x11, 0x3c, 0x5e, 0x84, 0x02}};

#define GU_SELECTION_ATTRIBUTES \
  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

static int HexDigit(CHAR8 c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int Upper(CHAR8 c) {
  return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

static BOOLEAN IsSpace(CHAR8 c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Exactly SSSS:BB:DD.F, the same shape the build-time parser accepts. */
static EFI_STATUS ParseLocation(const CHAR8 *Text, GU_GPU_LOCATION *Out) {
  UINT32 segment = 0, bus = 0, device = 0, function = 0;
  if (!Text || !Out) return EFI_INVALID_PARAMETER;
  for (UINTN i = 0; i < 4; i++) {
    int digit = HexDigit(Text[i]);
    if (digit < 0) return EFI_INVALID_PARAMETER;
    segment = (segment << 4) | (UINT32)digit;
  }
  if (Text[4] != ':') return EFI_INVALID_PARAMETER;
  for (UINTN i = 5; i < 7; i++) {
    int digit = HexDigit(Text[i]);
    if (digit < 0) return EFI_INVALID_PARAMETER;
    bus = (bus << 4) | (UINT32)digit;
  }
  if (Text[7] != ':') return EFI_INVALID_PARAMETER;
  for (UINTN i = 8; i < 10; i++) {
    int digit = HexDigit(Text[i]);
    if (digit < 0) return EFI_INVALID_PARAMETER;
    device = (device << 4) | (UINT32)digit;
  }
  if (Text[10] != '.') return EFI_INVALID_PARAMETER;
  int fn = HexDigit(Text[11]);
  if (fn < 0 || fn > 7) return EFI_INVALID_PARAMETER;
  function = (UINT32)fn;
  if (segment > 0xffffU || bus > 255U || device > 31U) return EFI_INVALID_PARAMETER;
  Out->segment = segment;
  Out->bus = bus;
  Out->device = device;
  Out->function = function;
  return EFI_SUCCESS;
}

static BOOLEAN SameLocation(const GU_GPU_LOCATION *a, const GU_GPU_LOCATION *b) {
  return a->segment == b->segment && a->bus == b->bus &&
         a->device == b->device && a->function == b->function;
}

EFI_STATUS GuSelectionParse(const CHAR8 *Text, UINTN Bytes, GU_SELECTION *Out) {
  if (!Out) return EFI_INVALID_PARAMETER;
  memset(Out, 0, sizeof(*Out));
  if (!Text || !Bytes) return EFI_SUCCESS;         /* no request */
  UINTN at = 0, end = Bytes;
  while (at < end && IsSpace(Text[at])) at++;
  while (end > at && IsSpace(Text[end - 1])) end--;
  if (at == end) return EFI_SUCCESS;               /* whitespace only */
  /* Embedded NULs are malformed, not a terminator. */
  for (UINTN i = at; i < end; i++)
    if (Text[i] == 0) return EFI_INVALID_PARAMETER;
  if (end - at == 3 && Upper(Text[at]) == 'A' && Upper(Text[at + 1]) == 'L' &&
      Upper(Text[at + 2]) == 'L') {
    Out->all = TRUE;
    return EFI_SUCCESS;
  }
  while (at < end) {
    if (Out->count == GU_SELECTION_MAX) goto bad;
    GU_GPU_LOCATION location;
    if (ParseLocation(&Text[at], &location) != EFI_SUCCESS) goto bad;
    for (size_t i = 0; i < Out->count; i++)
      if (SameLocation(&Out->list[i], &location)) goto bad;
    Out->list[Out->count++] = location;
    at += 12;
    while (at < end && IsSpace(Text[at])) at++;
    if (at == end) break;
    if (Text[at] != ',') goto bad;
    at++;
    while (at < end && IsSpace(Text[at])) at++;
    if (at == end) goto bad;                       /* trailing comma */
  }
  return EFI_SUCCESS;
bad:
  memset(Out, 0, sizeof(*Out));                    /* never a partial selection */
  return EFI_INVALID_PARAMETER;
}

static UINTN Append(CHAR8 *Out, UINTN Bytes, UINTN Used, const CHAR8 *Text) {
  while (*Text && Used + 1 < Bytes) Out[Used++] = *Text++;
  return Used;
}

EFI_STATUS GuSelectionFormat(const GU_SELECTION *Selection, CHAR8 *Out,
                            UINTN Bytes, UINTN *Used) {
  static const CHAR8 hex[] = "0123456789abcdef";
  if (!Selection || !Out || !Bytes) return EFI_INVALID_PARAMETER;
  UINTN used = 0;
  if (Selection->all) used = Append(Out, Bytes, used, "ALL");
  else if (Selection->count == 0) used = Append(Out, Bytes, used, "");
  else
    for (size_t i = 0; i < Selection->count; i++) {
      const GU_GPU_LOCATION *l = &Selection->list[i];
      if (i) used = Append(Out, Bytes, used, ",");
      CHAR8 field[13];
      field[0] = hex[(l->segment >> 12) & 0xf];
      field[1] = hex[(l->segment >> 8) & 0xf];
      field[2] = hex[(l->segment >> 4) & 0xf];
      field[3] = hex[l->segment & 0xf];
      field[4] = ':';
      field[5] = hex[(l->bus >> 4) & 0xf];
      field[6] = hex[l->bus & 0xf];
      field[7] = ':';
      field[8] = hex[(l->device >> 4) & 0xf];
      field[9] = hex[l->device & 0xf];
      field[10] = '.';
      field[11] = (CHAR8)('0' + (l->function & 7U));
      field[12] = 0;
      used = Append(Out, Bytes, used, field);
    }
  Out[used] = 0;
  if (Used) *Used = used;
  return EFI_SUCCESS;
}

EFI_STATUS GuSelectionRead(EFI_SYSTEM_TABLE *Table, GU_SELECTION *Out) {
  if (!Out) return EFI_INVALID_PARAMETER;
  memset(Out, 0, sizeof(*Out));
  if (!Table || !Table->RuntimeServices || !Table->RuntimeServices->GetVariable ||
      !Table->BootServices || !Table->BootServices->AllocatePool ||
      !Table->BootServices->FreePool)
    return EFI_UNSUPPORTED;
  CHAR8 *text = NULL;
  if (EFI_ERROR(Table->BootServices->AllocatePool(EfiLoaderData,
                                                  GU_SELECTION_MAX_TEXT,
                                                  (VOID **)&text)) ||
      !text)
    return EFI_OUT_OF_RESOURCES;
  UINTN size = GU_SELECTION_MAX_TEXT;
  UINT32 attributes = 0;
  EFI_STATUS s = Table->RuntimeServices->GetVariable(
      GU_SELECTION_VARIABLE_NAME, &TargetGuid, &attributes, &size, text);
  if (s == EFI_BUFFER_TOO_SMALL) s = EFI_BAD_BUFFER_SIZE;
  if (s == EFI_SUCCESS) s = GuSelectionParse(text, size, Out);
  (VOID)Table->BootServices->FreePool(text);
  if (s == EFI_SUCCESS) Out->from_variable = TRUE;
  return s;
}

EFI_STATUS GuSelectionWrite(EFI_SYSTEM_TABLE *Table, const GU_SELECTION *Selection) {
  if (!Table || !Selection || !Table->RuntimeServices ||
      !Table->RuntimeServices->SetVariable || !Table->BootServices ||
      !Table->BootServices->AllocatePool || !Table->BootServices->FreePool)
    return EFI_UNSUPPORTED;
  if (!Selection->count && !Selection->all) return EFI_INVALID_PARAMETER;
  CHAR8 *text = NULL;
  if (EFI_ERROR(Table->BootServices->AllocatePool(EfiLoaderData,
                                                  GU_SELECTION_MAX_TEXT,
                                                  (VOID **)&text)) ||
      !text)
    return EFI_OUT_OF_RESOURCES;
  UINTN used = 0;
  EFI_STATUS s = GuSelectionFormat(Selection, text, GU_SELECTION_MAX_TEXT, &used);
  if (s == EFI_SUCCESS)
    s = Table->RuntimeServices->SetVariable(GU_SELECTION_VARIABLE_NAME, &TargetGuid,
                                            GU_SELECTION_ATTRIBUTES, used, text);
  (VOID)Table->BootServices->FreePool(text);
  return s;
}

EFI_STATUS GuSelectionDelete(EFI_SYSTEM_TABLE *Table) {
  if (!Table || !Table->RuntimeServices || !Table->RuntimeServices->SetVariable)
    return EFI_UNSUPPORTED;
  EFI_STATUS s = Table->RuntimeServices->SetVariable(
      GU_SELECTION_VARIABLE_NAME, &TargetGuid, 0, 0, NULL);
  return s == EFI_NOT_FOUND ? EFI_SUCCESS : s;
}
