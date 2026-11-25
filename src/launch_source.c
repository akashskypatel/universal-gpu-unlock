/* SPDX-License-Identifier: GPL-2.0-only */
#include "launch_source.h"

/* A removable launch is the path everything else was written for; nothing about
 * it changed. A resident launch has to earn every gate. */
GU_LS_UINTN GuLaunchSourceReason(const GU_LAUNCH_OBSERVATION *observation) {
  if(!observation) return GU_LAUNCH_REASON_NOT_REMOVABLE;
  if(observation->Removable) return GU_LAUNCH_REASON_NONE;
  if(!observation->Partition) return GU_LAUNCH_REASON_NOT_REMOVABLE;
  if(!observation->EspResidentBuild) return GU_LAUNCH_REASON_BUILD_REFUSES;
  if(!observation->WindowsLoader) return GU_LAUNCH_REASON_NOT_WINDOWS_ESP;
  if(!observation->PinnedEsp) return GU_LAUNCH_REASON_PIN_MISMATCH;
  if(!observation->BundleRecord) return GU_LAUNCH_REASON_BUNDLE_RECORD;
  if(!observation->LogWritable) return GU_LAUNCH_REASON_LOG_NOT_WRITABLE;
  return GU_LAUNCH_REASON_NONE;
}

GU_LS_UINTN GuLaunchSourceDecide(const GU_LAUNCH_OBSERVATION *observation) {
  if(!observation) return GU_LAUNCH_REFUSE;
  if(observation->Removable) return GU_LAUNCH_ALLOW_REMOVABLE;
  return GuLaunchSourceReason(observation) == GU_LAUNCH_REASON_NONE
             ? GU_LAUNCH_ALLOW_RESIDENT : GU_LAUNCH_REFUSE;
}

const char *GuLaunchSourceDecisionName(GU_LS_UINTN Decision) {
  switch(Decision) {
  case GU_LAUNCH_ALLOW_REMOVABLE: return "ALLOW_REMOVABLE";
  case GU_LAUNCH_ALLOW_RESIDENT: return "ALLOW_RESIDENT";
  case GU_LAUNCH_REFUSE: return "REFUSE";
  default: return "UNKNOWN";
  }
}

const char *GuLaunchSourceReasonName(GU_LS_UINTN Reason) {
  switch(Reason) {
  case GU_LAUNCH_REASON_NONE: return "NONE";
  case GU_LAUNCH_REASON_NOT_REMOVABLE: return "NOT_A_LAUNCHABLE_VOLUME";
  case GU_LAUNCH_REASON_BUILD_REFUSES: return "BUILD_REFUSES_ESP_RESIDENT";
  case GU_LAUNCH_REASON_NOT_WINDOWS_ESP: return "NOT_THE_WINDOWS_ESP";
  case GU_LAUNCH_REASON_PIN_MISMATCH: return "PINNED_PARTITION_MISMATCH";
  case GU_LAUNCH_REASON_BUNDLE_RECORD: return "BUNDLE_RECORD_MISSING";
  case GU_LAUNCH_REASON_LOG_NOT_WRITABLE: return "LOG_NOT_WRITABLE";
  default: return "UNKNOWN";
  }
}
