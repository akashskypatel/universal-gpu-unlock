/* SPDX-License-Identifier: GPL-2.0-only */
/* Persisted boot record. One NVRAM variable, fixed size, validated on read.
 * No GPU access, no control-flow authority. */
#include "boot_state.h"

#include <string.h>

/* Fork-owned GUID. 2a7d54c1-5b3e-4a19-8f60-9d21c4b7e803 */
static EFI_GUID StateGuid =
    {0x2a7d54c1, 0x5b3e, 0x4a19, {0x8f, 0x60, 0x9d, 0x21, 0xc4, 0xb7, 0xe8, 0x03}};

#define GU_BOOT_STATE_ATTRIBUTES                                  \
  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
   EFI_VARIABLE_RUNTIME_ACCESS)

static int Usable(EFI_SYSTEM_TABLE *t) {
  return t && t->RuntimeServices && t->RuntimeServices->GetVariable &&
         t->RuntimeServices->SetVariable;
}

static int Valid(const GU_BOOT_STATE *s) {
  if (!s) return 0;
  if (s->Magic != GU_BOOT_STATE_MAGIC) return 0;
  if (s->Phase != GU_BOOT_STATE_IDLE && s->Phase != GU_BOOT_STATE_INTENT &&
      s->Phase != GU_BOOT_STATE_OUTCOME)
    return 0;
  if (s->Armed > 1) return 0;
  if (s->Bus > 0xffU || s->Device > 0x1fU || s->Function > 7U)
    return 0;
  for (size_t i = 0; i < sizeof(s->Reserved); i++)
    if (s->Reserved[i]) return 0;
  return 1;
}

EFI_STATUS GuBootStateRead(EFI_SYSTEM_TABLE *Table, GU_BOOT_STATE *Out) {
  if (!Out) return EFI_INVALID_PARAMETER;
  memset(Out, 0, sizeof(*Out));
  if (!Usable(Table)) return EFI_UNSUPPORTED;
  GU_BOOT_STATE buffer;
  memset(&buffer, 0, sizeof(buffer));
  UINTN size = sizeof(buffer);
  UINT32 attributes = 0;
  EFI_STATUS s = Table->RuntimeServices->GetVariable(
      GU_BOOT_STATE_VARIABLE_NAME, &StateGuid, &attributes, &size, &buffer);
  if (s == EFI_BUFFER_TOO_SMALL) return EFI_COMPROMISED_DATA;
  if (s != EFI_SUCCESS) return s;
  /* A short or long value is corruption, not a compatible older record: the
   * record has no version field, so guessing at a layout would be a guess. */
  if (size != sizeof(buffer)) return EFI_COMPROMISED_DATA;
  if (!Valid(&buffer)) return EFI_COMPROMISED_DATA;
  if (attributes != GU_BOOT_STATE_ATTRIBUTES) return EFI_COMPROMISED_DATA;
  memcpy(Out,&buffer,sizeof(buffer));
  return EFI_SUCCESS;
}

int GuBootStateRequiresRecovery(const GU_BOOT_STATE *State) {
  return State && (State->Phase == GU_BOOT_STATE_INTENT ||
                   (State->Phase == GU_BOOT_STATE_OUTCOME && State->Status != EFI_SUCCESS));
}

EFI_STATUS GuBootStateWrite(EFI_SYSTEM_TABLE *Table, const GU_BOOT_STATE *State) {
  if (!Table || !State) return EFI_INVALID_PARAMETER;
  if (!Usable(Table)) return EFI_UNSUPPORTED;
  if (!Valid(State)) return EFI_INVALID_PARAMETER;
  EFI_STATUS s = Table->RuntimeServices->SetVariable(GU_BOOT_STATE_VARIABLE_NAME, &StateGuid,
                                             GU_BOOT_STATE_ATTRIBUTES, sizeof(*State),
                                             (VOID *)State);
  if(s!=EFI_SUCCESS)return s;
  GU_BOOT_STATE confirmed;
  s=GuBootStateRead(Table,&confirmed);
  if(s!=EFI_SUCCESS)return s;
  return memcmp(&confirmed,State,sizeof(confirmed))==0?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}

/* Maintenance only. Refuse any ordered driver registration, including an
 * unrelated one: do not guess whether a boot entry can invoke an old image.
 * This routine is not a proof of physical recovery or owner acceptance. */
EFI_STATUS GuBootStateDeleteReviewed(EFI_SYSTEM_TABLE *Table,
                                    const GU_BOOT_STATE *Reviewed) {
  if(!Valid(Reviewed))return EFI_INVALID_PARAMETER;
  if(!Usable(Table))return EFI_UNSUPPORTED;
  if(!GuBootStateRequiresRecovery(Reviewed))
    return EFI_NOT_READY;
  EFI_GUID global = {0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}};
  UINT16 order;
  UINTN size=sizeof(order);
  UINT32 attributes=0;
  EFI_STATUS s=Table->RuntimeServices->GetVariable(L"DriverOrder",&global,
                                                    &attributes,&size,&order);
  if(s==EFI_SUCCESS||s==EFI_BUFFER_TOO_SMALL)return EFI_NOT_READY;
  if(s!=EFI_NOT_FOUND)return s;
  GU_BOOT_STATE current;
  s=GuBootStateRead(Table,&current);
  if(s!=EFI_SUCCESS)return s;
  if(memcmp(&current,Reviewed,sizeof(current)))return EFI_COMPROMISED_DATA;
  s=Table->RuntimeServices->SetVariable(GU_BOOT_STATE_VARIABLE_NAME,&StateGuid,0,0,NULL);
  if(s!=EFI_SUCCESS)return s;
  s=GuBootStateRead(Table,&current);
  if(s==EFI_NOT_FOUND)return EFI_SUCCESS;
  return s==EFI_SUCCESS?EFI_COMPROMISED_DATA:s;
}

EFI_STATUS GuBootStateRecord(EFI_SYSTEM_TABLE *Table, const GU_BOOT_STATE *Previous,
                             UINT32 Phase, UINT64 Status, UINT16 DeviceId,
                             UINT16 Segment, UINT16 Bus, UINT8 Device, UINT8 Function,
                             UINT8 Armed) {
  GU_BOOT_STATE next;
  memset(&next, 0, sizeof(next));
  next.Magic = GU_BOOT_STATE_MAGIC;
  /* Only an outcome completes a boot; an intent keeps the previous count so a
   * power loss in between leaves the count pointing at the interrupted boot. */
  next.BootCount = Previous ? Previous->BootCount : 0;
  if (Phase == GU_BOOT_STATE_OUTCOME) next.BootCount++;
  next.Phase = Phase;
  next.Status = Status;
  next.DeviceId = DeviceId;
  next.Segment = Segment;
  next.Bus = Bus;
  next.Device = Device;
  next.Function = Function;
  next.Armed = Armed ? 1U : 0U;
  return GuBootStateWrite(Table, &next);
}
