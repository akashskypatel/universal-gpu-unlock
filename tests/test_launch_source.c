/* Host unit tests for the launch-source decision (no UEFI, no hardware). */
#include <stdio.h>
#include <string.h>

#include "launch_source.h"

static int checks;

static int Check(int ok, const char *what, int line) {
  checks++;
  if(!ok) {
    fprintf(stderr, "FAIL line %d: %s\n", line, what);
    return 0;
  }
  return 1;
}
#define CHECK(x) do { if(!Check((x) ? 1 : 0, #x, __LINE__)) return 1; } while(0)

/* An ESP-resident launch in a build that asks for one, with every gate met. */
static GU_LAUNCH_OBSERVATION Resident(void) {
  GU_LAUNCH_OBSERVATION o;
  memset(&o, 0, sizeof(o));
  o.Partition = 1;
  o.WindowsLoader = 1;
  o.PinnedEsp = 1;
  o.BundleRecord = 1;
  o.LogWritable = 1;
  o.EspResidentBuild = 1;
  return o;
}

int main(void) {
  /* Default build, USB stick: allowed, and no reason is reported. */
  GU_LAUNCH_OBSERVATION usb;
  memset(&usb, 0, sizeof(usb));
  usb.Removable = 1;
  CHECK(GuLaunchSourceDecide(&usb) == GU_LAUNCH_ALLOW_REMOVABLE);
  CHECK(GuLaunchSourceReason(&usb) == GU_LAUNCH_REASON_NONE);
  CHECK(strcmp(GuLaunchSourceDecisionName(GuLaunchSourceDecide(&usb)),
               "ALLOW_REMOVABLE") == 0);
  /* A USB launch is allowed even when nothing else holds: the stick is the path
   * the rest of the code was written for. */
  CHECK(GuLaunchSourceReason(&usb) == GU_LAUNCH_REASON_NONE);

  /* Default build, ESP: refused, and the refusal says why. */
  GU_LAUNCH_OBSERVATION resident = Resident();
  resident.EspResidentBuild = 0;
  CHECK(GuLaunchSourceDecide(&resident) == GU_LAUNCH_REFUSE);
  CHECK(GuLaunchSourceReason(&resident) == GU_LAUNCH_REASON_BUILD_REFUSES);
  CHECK(strcmp(GuLaunchSourceReasonName(GU_LAUNCH_REASON_BUILD_REFUSES),
               "BUILD_REFUSES_ESP_RESIDENT") == 0);

  /* Nothing observed at all refuses. */
  GU_LAUNCH_OBSERVATION zero;
  memset(&zero, 0, sizeof(zero));
  CHECK(GuLaunchSourceDecide(&zero) == GU_LAUNCH_REFUSE);
  CHECK(GuLaunchSourceReason(&zero) == GU_LAUNCH_REASON_NOT_REMOVABLE);
  CHECK(GuLaunchSourceDecide(NULL) == GU_LAUNCH_REFUSE);

  /* Every gate is independently necessary: drop one, get its reason. */
  struct { int which; unsigned reason; const char *name; } gates[] = {
    {0, GU_LAUNCH_REASON_NOT_WINDOWS_ESP, "NOT_THE_WINDOWS_ESP"},
    {1, GU_LAUNCH_REASON_PIN_MISMATCH, "PINNED_PARTITION_MISMATCH"},
    {2, GU_LAUNCH_REASON_BUNDLE_RECORD, "BUNDLE_RECORD_MISSING"},
    {3, GU_LAUNCH_REASON_LOG_NOT_WRITABLE, "LOG_NOT_WRITABLE"},
  };
  for(unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
    GU_LAUNCH_OBSERVATION o = Resident();
    switch(gates[i].which) {
    case 0: o.WindowsLoader = 0; break;
    case 1: o.PinnedEsp = 0; break;
    case 2: o.BundleRecord = 0; break;
    case 3: o.LogWritable = 0; break;
    }
    CHECK(GuLaunchSourceDecide(&o) == GU_LAUNCH_REFUSE);
    CHECK(GuLaunchSourceReason(&o) == gates[i].reason);
    CHECK(strcmp(GuLaunchSourceReasonName(gates[i].reason), gates[i].name) == 0);
  }

  /* A volume that is neither removable nor a partition is refused before any of
   * the ESP gates are consulted. */
  GU_LAUNCH_OBSERVATION odd = Resident();
  odd.Partition = 0;
  CHECK(GuLaunchSourceDecide(&odd) == GU_LAUNCH_REFUSE);
  CHECK(GuLaunchSourceReason(&odd) == GU_LAUNCH_REASON_NOT_REMOVABLE);

  /* The fully gated case is the only resident admission. */
  GU_LAUNCH_OBSERVATION allowed = Resident();
  CHECK(GuLaunchSourceDecide(&allowed) == GU_LAUNCH_ALLOW_RESIDENT);
  CHECK(GuLaunchSourceReason(&allowed) == GU_LAUNCH_REASON_NONE);
  CHECK(strcmp(GuLaunchSourceDecisionName(GU_LAUNCH_ALLOW_RESIDENT),
               "ALLOW_RESIDENT") == 0);

  /* Unknown values are named, not guessed. */
  CHECK(strcmp(GuLaunchSourceDecisionName(99), "UNKNOWN") == 0);
  CHECK(strcmp(GuLaunchSourceReasonName(99), "UNKNOWN") == 0);

  /* The decision and the reason never disagree. */
  for(unsigned mask = 0; mask < 64; mask++) {
    GU_LAUNCH_OBSERVATION o;
    memset(&o, 0, sizeof(o));
    o.Removable = (mask & 1) ? 1 : 0;
    o.Partition = (mask & 2) ? 1 : 0;
    o.EspResidentBuild = (mask & 4) ? 1 : 0;
    o.WindowsLoader = (mask & 8) ? 1 : 0;
    o.PinnedEsp = (mask & 16) ? 1 : 0;
    o.BundleRecord = (mask & 32) ? 1 : 0;
    o.LogWritable = 1;
    unsigned decision = GuLaunchSourceDecide(&o);
    unsigned reason = GuLaunchSourceReason(&o);
    CHECK((decision == GU_LAUNCH_REFUSE) == (reason != GU_LAUNCH_REASON_NONE));
    if(decision == GU_LAUNCH_ALLOW_RESIDENT) CHECK(!o.Removable && o.Partition);
    if(decision == GU_LAUNCH_ALLOW_REMOVABLE) CHECK(o.Removable);
  }

  printf("PASS launch source: %d checks; a stick always, the ESP only when every gate holds\n",
         checks);
  return 0;
}
