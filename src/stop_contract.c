/* SPDX-License-Identifier: GPL-2.0-only */
#include "stop_contract.h"

#include <string.h>

typedef struct {
  GU_SC_UINT32 Bit;
  const char *Name;
} GU_STOP_REQUIREMENT;

/* One row per requirement, in the plan's order. Adding a row here without a
 * field in the observation cannot compile a passing decision, because the
 * decision is computed from this table rather than written out by hand. */
static const GU_STOP_REQUIREMENT Requirements[] = {
  {GU_STOP_REQ_OWNER, "OWNER"},
  {GU_STOP_REQ_COLD, "COLD"},
  {GU_STOP_REQ_CONSOLE, "CONSOLE"},
  {GU_STOP_REQ_PEERS, "PEERS"},
  {GU_STOP_REQ_TARGET_GOP, "TARGET_GOP"},
  {GU_STOP_REQ_LOG, "LOG"},
  {GU_STOP_REQ_RECOVERY, "RECOVERY"},
  {GU_STOP_REQ_SIGNED_BOOTER, "SIGNED_BOOTER"},
  {GU_STOP_REQ_BOUND, "BOUND"},
  {GU_STOP_REQ_DMA, "DMA"},
};

#define GU_STOP_REQUIREMENT_COUNT \
  (sizeof(Requirements) / sizeof(Requirements[0]))

static GU_SC_BOOLEAN Held(const GU_STOP_OBSERVATION *o, GU_SC_UINT32 bit) {
  switch(bit) {
  case GU_STOP_REQ_OWNER: return o->Owner;
  case GU_STOP_REQ_COLD: return o->Cold;
  case GU_STOP_REQ_CONSOLE: return o->Console;
  case GU_STOP_REQ_PEERS: return o->Peers;
  case GU_STOP_REQ_TARGET_GOP: return o->TargetGopGone;
  case GU_STOP_REQ_LOG: return o->LogClosed;
  case GU_STOP_REQ_RECOVERY: return o->Recovery;
  case GU_STOP_REQ_SIGNED_BOOTER: return o->SignedBooter;
  case GU_STOP_REQ_BOUND: return o->Bound;
  case GU_STOP_REQ_DMA: return o->DmaIdle;
  default: return GU_SC_FALSE;
  }
}

GU_SC_UINT32 GuStopContractMissing(const GU_STOP_OBSERVATION *observation) {
  GU_SC_UINT32 missing = 0;
  if(!observation) return GU_STOP_REQ_ALL;
  for(GU_SC_UINTN i = 0; i < GU_STOP_REQUIREMENT_COUNT; i++)
    if(!Held(observation, Requirements[i].Bit)) missing |= Requirements[i].Bit;
  return missing;
}

GU_SC_BOOLEAN GuStopContractAllows(const GU_STOP_OBSERVATION *observation) {
  if(!observation) return GU_SC_FALSE;
  return GuStopContractMissing(observation) == 0 ? GU_SC_TRUE : GU_SC_FALSE;
}

const char *GuStopContractDecision(GU_SC_UINT32 missing) {
  return missing == 0 ? "ALLOW" : "REFUSE";
}

const char *GuStopContractRequirementName(GU_SC_UINT32 bit) {
  for(GU_SC_UINTN i = 0; i < GU_STOP_REQUIREMENT_COUNT; i++)
    if(Requirements[i].Bit == bit) return Requirements[i].Name;
  return "UNKNOWN";
}

GU_SC_UINTN GuStopContractDescribe(GU_SC_UINT32 missing, char *Out, GU_SC_UINTN Capacity) {
  GU_SC_UINTN written = 0;
  if(!Out || !Capacity) return 0;
  Out[0] = 0;
  if(missing == 0) {
    const char *none = "none";
    for(GU_SC_UINTN i = 0; none[i] && i + 1 < Capacity; i++) Out[written++] = none[i];
    Out[written] = 0;
    return written;
  }
  for(GU_SC_UINTN i = 0; i < GU_STOP_REQUIREMENT_COUNT; i++) {
    if(!(missing & Requirements[i].Bit)) continue;
    const char *name = Requirements[i].Name;
    if(written) {
      if(written + 1 >= Capacity) break;
      Out[written++] = ',';
    }
    for(GU_SC_UINTN c = 0; name[c]; c++) {
      if(written + 1 >= Capacity) break;
      Out[written++] = name[c];
    }
  }
  Out[written] = 0;
  return written;
}

const GU_SC_UINT32 *GuStopContractRequirements(GU_SC_UINTN *Count) {
  static GU_SC_UINT32 bits[GU_STOP_REQUIREMENT_COUNT + 1];
  static GU_SC_BOOLEAN prepared = GU_SC_FALSE;
  if(!prepared) {
    for(GU_SC_UINTN i = 0; i < GU_STOP_REQUIREMENT_COUNT; i++) bits[i] = Requirements[i].Bit;
    bits[GU_STOP_REQUIREMENT_COUNT] = 0;
    prepared = GU_SC_TRUE;
  }
  if(Count) *Count = GU_STOP_REQUIREMENT_COUNT;
  return bits;
}
