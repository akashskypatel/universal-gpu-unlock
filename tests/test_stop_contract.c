/* Host unit tests for the pre-Stop contract (no UEFI, no hardware). */
#include <stdio.h>
#include <string.h>

#include "stop_contract.h"

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

static GU_STOP_OBSERVATION Complete(void) {
  GU_STOP_OBSERVATION o;
  memset(&o, 0, sizeof(o));
  o.DeviceId = 0x2684U;
  o.Owner = 1;
  o.Cold = 1;
  o.Console = 1;
  o.Peers = 1;
  o.TargetGopGone = 1;
  o.LogClosed = 1;
  o.Recovery = 1;
  o.SignedBooter = 1;
  o.Bound = 1;
  o.DmaIdle = 1;
  return o;
}

int main(void) {
  GU_STOP_OBSERVATION zero;
  memset(&zero, 0, sizeof(zero));
  /* Fail closed: an untouched observation satisfies nothing. */
  CHECK(GuStopContractMissing(&zero) == GU_STOP_REQ_ALL);
  CHECK(!GuStopContractAllows(&zero));
  CHECK(strcmp(GuStopContractDecision(GuStopContractMissing(&zero)), "REFUSE") == 0);
  CHECK(!GuStopContractAllows(NULL));
  CHECK(GuStopContractMissing(NULL) == GU_STOP_REQ_ALL);

  /* Every requirement is independently necessary: drop it, and only it. */
  GU_SC_UINTN count = 0;
  const GU_SC_UINT32 *bits = GuStopContractRequirements(&count);
  CHECK(count == 10);
  CHECK(bits && bits[count] == 0);
  GU_SC_UINT32 union_bits = 0;
  for(GU_SC_UINTN i = 0; i < count; i++) {
    GU_SC_UINT32 bit = bits[i];
    union_bits |= bit;
    /* One bit per requirement: no aliases, no holes. */
    CHECK(bit && (bit & (bit - 1)) == 0);
    CHECK((GU_STOP_REQ_ALL & bit) == bit);
    CHECK(strcmp(GuStopContractRequirementName(bit), "UNKNOWN") != 0);
    GU_STOP_OBSERVATION o = Complete();
    /* Clear exactly this requirement through its own field. */
    switch(bit) {
    case GU_STOP_REQ_OWNER: o.Owner = 0; break;
    case GU_STOP_REQ_COLD: o.Cold = 0; break;
    case GU_STOP_REQ_CONSOLE: o.Console = 0; break;
    case GU_STOP_REQ_PEERS: o.Peers = 0; break;
    case GU_STOP_REQ_TARGET_GOP: o.TargetGopGone = 0; break;
    case GU_STOP_REQ_LOG: o.LogClosed = 0; break;
    case GU_STOP_REQ_RECOVERY: o.Recovery = 0; break;
    case GU_STOP_REQ_SIGNED_BOOTER: o.SignedBooter = 0; break;
    case GU_STOP_REQ_BOUND: o.Bound = 0; break;
    case GU_STOP_REQ_DMA: o.DmaIdle = 0; break;
    }
    CHECK(GuStopContractMissing(&o) == bit);
    CHECK(!GuStopContractAllows(&o));
  }
  CHECK(union_bits == GU_STOP_REQ_ALL);
  CHECK(strcmp(GuStopContractRequirementName(0x4000U), "UNKNOWN") == 0);

  /* A complete capture is the only way through. */
  GU_STOP_OBSERVATION full = Complete();
  CHECK(GuStopContractMissing(&full) == 0);
  CHECK(GuStopContractAllows(&full));
  CHECK(strcmp(GuStopContractDecision(0), "ALLOW") == 0);
  CHECK(full.DeviceId == 0x2684U); /* the id is recorded, not consulted */

  /* The description is stable, ordered, and bounded. */
  char text[128];
  GuStopContractDescribe(0, text, sizeof(text));
  CHECK(strcmp(text, "none") == 0);
  GuStopContractDescribe(GU_STOP_REQ_RECOVERY | GU_STOP_REQ_OWNER, text, sizeof(text));
  CHECK(strcmp(text, "OWNER,RECOVERY") == 0);
  GuStopContractDescribe(GU_STOP_REQ_ALL, text, sizeof(text));
  CHECK(strcmp(text, "OWNER,COLD,CONSOLE,PEERS,TARGET_GOP,LOG,RECOVERY,SIGNED_BOOTER,BOUND,DMA") == 0);
  /* Truncation terminates; it never overruns. */
  char small[10];
  memset(small, 'x', sizeof(small));
  GU_SC_UINTN written = GuStopContractDescribe(GU_STOP_REQ_ALL, small, sizeof(small));
  CHECK(written < sizeof(small));
  CHECK(small[written] == 0);
  CHECK(strlen(small) == written);
  CHECK(GuStopContractDescribe(GU_STOP_REQ_ALL, NULL, 8) == 0);
  CHECK(GuStopContractDescribe(GU_STOP_REQ_ALL, text, 0) == 0);

  /* The shipped state of the world: four facts are observable only if the code
   * sets them, and nothing sets the six external ones, so a real capture is
   * incomplete by construction. */
  GU_STOP_OBSERVATION shipped;
  memset(&shipped, 0, sizeof(shipped));
  shipped.Owner = 1;
  shipped.Cold = 1;
  shipped.Console = 1;
  shipped.Peers = 1;
  CHECK(GuStopContractMissing(&shipped) ==
        (GU_STOP_REQ_TARGET_GOP | GU_STOP_REQ_LOG | GU_STOP_REQ_RECOVERY |
         GU_STOP_REQ_SIGNED_BOOTER | GU_STOP_REQ_BOUND | GU_STOP_REQ_DMA));

  printf("PASS pre-Stop contract: %d checks; pure decision over an explicit capture\n",
         checks);
  return 0;
}
