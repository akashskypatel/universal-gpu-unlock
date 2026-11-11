/* SPDX-License-Identifier: GPL-2.0-only */
#define GU_DISCOVERY_HOST_TEST 1
#include "gpu_discovery_mock.h"
#include "../src/gpu_inventory.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const GU_GPU_IDENTITY Primary={0x20b010deU,0xa12310deU,0,3,0,0};
static const GU_GPU_IDENTITY Secondary={0x250310deU,0xb45610deU,1,7,4,1};
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

typedef struct {
  EFI_PCI_IO_PROTOCOL protocol;
  UINT32 dwords[64];
  UINTN segment, bus, device, function;
  int read_fail, location_fail, mutate_at_full_read;
  int drift_nonselected_on_full_read;
  unsigned full_reads;
} MOCK_DEVICE;
static MOCK_DEVICE gpu[4];
static EFI_HANDLE handles[4];
static size_t count;
static unsigned init_calls, free_calls, log_calls, resolve_calls;
static int fail_pool_free, redirect_on_refresh, refuse_init;
GU_PLATFORM Gu;

EFI_STATUS GuInit(EFI_BOOT_SERVICES *bs, EFI_PCI_IO_PROTOCOL *pci) {
  init_calls++;
  if(refuse_init)return EFI_ALREADY_STARTED;
  memset(&Gu,0,sizeof(Gu));Gu.bs=bs;Gu.pci=pci;
  return EFI_SUCCESS;
}
void GuLog(const char *format, ...) { (void)format; log_calls++; }
int GuIsSupportedPciId(UINT32 pci) {
  return pci == Primary.pci_id || pci == Secondary.pci_id;
}
int GuIsAmpereDeviceId(UINT16 dev) { return dev == 0x2503U; }
int GuIsTuringDeviceId(UINT16 dev) { (void)dev; return 0; }
int GuCheckConfig(const UINT32 c[64], UINTN seg, UINTN bus, UINTN dev, UINTN fun, GU_CAPS *caps) {
  if (!c || !caps || seg != 0 || bus > 255 || dev > 31 || fun > 7 ||
      !GuIsSupportedPciId(c[0]) || (c[2] >> 16) != 0x0300) return 0;
  if(c[1]&4u)return 0;
  memset(caps, 0, sizeof(*caps)); caps->pm = 0x40; caps->pcie = 0x60;
  return 1;
}
int GuCheckConfigInitialMaster(const UINT32 c[64], UINTN seg, UINTN bus,
                                UINTN dev, UINTN fun, GU_CAPS *caps) {
  UINT32 without_master[64];
  if(!c)return 0;
  memcpy(without_master,c,sizeof(without_master));
  without_master[1]&=~4u;
  return GuCheckConfig(without_master,seg,bus,dev,fun,caps);
}
static EFI_STATUS Read(EFI_PCI_IO_PROTOCOL *io, int width, UINT32 offset, UINTN n, VOID *out) {
  MOCK_DEVICE *m = (MOCK_DEVICE *)io->context;
  if (m->read_fail || width != EfiPciIoWidthUint32 || !out ||
      (offset & 3) || offset / 4 + n > 64) return 3;
  memcpy(out, m->dwords + offset / 4, n * sizeof(UINT32));
  if (n == 64) {
    m->full_reads++;
    if (m->mutate_at_full_read)
      ((UINT32 *)out)[0x2c / 4] ^= 1U;
    if (m->drift_nonselected_on_full_read) {
      /* Selected GPU[1] changes non-target GPU[0] after its first read. */
      gpu[0].dwords[4] ^= 0x1000U;
      m->drift_nonselected_on_full_read = 0;
    }
  }
  return EFI_SUCCESS;
}
static EFI_STATUS GetLocation(EFI_PCI_IO_PROTOCOL *io, UINTN *seg, UINTN *bus, UINTN *dev, UINTN *fun) {
  MOCK_DEVICE *m = (MOCK_DEVICE *)io->context;
  if (m->location_fail) return 3;
  *seg = m->segment; *bus = m->bus; *dev = m->device; *fun = m->function;
  return EFI_SUCCESS;
}
static EFI_STATUS Locate(int type, EFI_GUID *guid, VOID *key, UINTN *outCount, EFI_HANDLE **outHandles) {
  (void)type; (void)guid; (void)key;
  *outCount = count; *outHandles = handles; return EFI_SUCCESS;
}
static EFI_STATUS Resolve(EFI_HANDLE h, EFI_GUID *guid, VOID **io) {
  (void)guid; resolve_calls++;
  *io = (redirect_on_refresh && resolve_calls > count) ? handles[1] : h;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {
  free_calls++; if (p != handles || fail_pool_free) return 3;
  return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES services = { Locate, Resolve, Free };
static EFI_SYSTEM_TABLE system_table = { &services };
static void Populate(size_t i, GU_GPU_IDENTITY id) {
  MOCK_DEVICE *m = &gpu[i]; memset(m, 0, sizeof(*m));
  m->protocol.GetLocation = GetLocation; m->protocol.Pci.Read = Read;
  m->protocol.context = m;
  m->dwords[0] = id.pci_id;
  m->dwords[2] = 0x03000000U;
  m->dwords[0x2c / 4] = id.subsystem_id;
  m->dwords[4] = 0xfb000000U + (UINT32)(i * 0x1000000U);
  m->segment = id.segment; m->bus = id.bus;
  m->device = id.device; m->function = id.function;
  handles[i] = &m->protocol;
}
static void ResetPair(void) {
  memset(&Gu, 0, sizeof(Gu)); init_calls = free_calls = log_calls = 0;
  fail_pool_free = redirect_on_refresh = refuse_init = 0; resolve_calls = 0; count = 2;
  Populate(0, Primary); Populate(1, Secondary);
}
#include "../src/gpu_discovery.c"

#if defined(GU_ACTIVE_SINGLE_TARGET)
/* The production single-target path supports the user's segment-0 PCI cards.
 * Earlier mock Segment=1 is intentionally not accepted by GuCheckConfig. */
static void ResetActivePair(void) {
  ResetPair();gpu[1].segment=0;gpu[1].device=0;gpu[1].function=0;
}
int main(void) {
  EFI_STATUS status;
  ResetActivePair();
  status=GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table);
  CHECK(status==EFI_SUCCESS && init_calls==1);
  CHECK(Gu.handle==handles[1] && Gu.device_id==0x2503U);
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  /* A refused rebind must not replace the previous target or saved peer
   * record, even after the new inventory has passed both PCI read passes. */
  {
    EFI_HANDLE old=Gu.handle;
    UINT32 original_peer=ActiveInitialConfig[0][4];
    refuse_init=1;
    gpu[0].dwords[4]^=0x1000U;
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
    CHECK(Gu.handle==old && ActiveInventoryValid);
    CHECK(ActiveInitialConfig[0][4]==original_peer);
    refuse_init=0;
  }
  ResetActivePair();
  gpu[1].dwords[1]=4u; /* initial selected UEFI driver Bus Master enabled */
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  /* The selected GPU may change its own BAR after the active transaction. */
  gpu[1].dwords[4]^=0x1000U;
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  /* Any change to the other card must fail without being repaired. */
  gpu[0].dwords[4]^=0x1000U;
  CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
  ResetActivePair();
  count=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED &&
        init_calls==0);
  ResetActivePair();
  gpu[0].bus=gpu[1].bus;gpu[0].device=gpu[1].device;
  gpu[0].function=gpu[1].function;gpu[0].segment=gpu[1].segment;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED &&
        init_calls==0);
  ResetActivePair();
  gpu[0].drift_nonselected_on_full_read=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED &&
        init_calls==0);
  ResetPair(); /* wrong segment/device/function for explicitly selected PCI target */
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED &&
        init_calls==0);
  printf("PASS exact single-target active binding with protected peer: %u checks\n",checks);
  return 0;
}
#elif defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
int main(void) {
  EFI_STATUS status;
  ResetPair();
  status=GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table);
  CHECK(status==EFI_UNSUPPORTED && init_calls==0 && free_calls==2);
  /* Full-set re-enumeration precedes two PCI reads of all NVIDIA devices. */
  CHECK(gpu[0].full_reads == 2 && gpu[1].full_reads == 2);
  ResetPair(); count=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED && init_calls==0);
#if GU_GPU_SELECTION_COUNT == 2
  CHECK(gpu[0].full_reads == 0 && free_calls==1);
#else
  CHECK(gpu[0].full_reads == 2 && free_calls==2);
#endif
  ResetPair(); count=3;
  Populate(2,Primary);gpu[2].bus=6;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED && init_calls==0);
  ResetPair(); fail_pool_free=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED && init_calls==0);
  printf("PASS active selected-entrypoint refusal: %u checks; GuInit never called\n",checks);
  return 0;
}
#else
int main(void) {
  EFI_STATUS status;
  /* Prepare requested targets without any active GuInit; preserve operator
   * order even when UEFI enumerates devices in the opposite order. */
  {
    GU_GPU_PREPARED_SEQUENCE plan;
    const GU_GPU_LOCATION requested[2] = {{1,7,4,1},{0,3,0,0}};
    const GU_GPU_LOCATION missing[2] = {{1,7,4,1},{0,9,0,0}};
    const GU_GPU_LOCATION repeated[2] = {{0,3,0,0},{0,3,0,0}};
    ResetPair();
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,&plan)==EFI_SUCCESS);
    CHECK(init_calls==0 && plan.inventory.count==2 && plan.selected_count==2);
    CHECK(plan.selected_indices[0]==1 && plan.selected_indices[1]==0);
    CHECK(plan.inventory.nvidia[plan.selected_indices[0]].pci_id==Secondary.pci_id);
    /* Aggregate observations must never publish partial outputs or accept
     * aliased device contexts; second-pass checks catch cross-target drift. */
    {
      GU_GPU_REFRESHED_SEQUENCE refreshed;
      size_t saved_index;
      EFI_HANDLE saved_handle;
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_SUCCESS);
      CHECK(refreshed.count==2 &&
            refreshed.targets[0].pci==&gpu[1].protocol &&
            refreshed.targets[1].pci==&gpu[0].protocol);
      CHECK(refreshed.targets[0].config[0]==Secondary.pci_id &&
            refreshed.targets[1].config[0]==Primary.pci_id);
      CHECK(gpu[0].full_reads==2 && gpu[1].full_reads==2 && !init_calls);
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,NULL)==EFI_INVALID_PARAMETER);

      memset(&refreshed,0xa5,sizeof(refreshed));
      CHECK(GuRefreshPreparedSequence(NULL,&plan,&refreshed)==EFI_INVALID_PARAMETER);
      CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
      saved_index=plan.selected_indices[1];
      plan.selected_indices[1]=plan.selected_indices[0];
      memset(&refreshed,0xa5,sizeof(refreshed));
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_INVALID_PARAMETER);
      CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
      plan.selected_indices[1]=saved_index;

      saved_handle=(EFI_HANDLE)plan.inventory.nvidia[0].handle;
      plan.inventory.nvidia[0].handle=plan.inventory.nvidia[1].handle;
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_INVALID_PARAMETER);
      CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
      plan.inventory.nvidia[0].handle=saved_handle;
      {
        GU_GPU_RECORD saved=plan.inventory.nvidia[0];
        plan.inventory.nvidia[0].pci=plan.inventory.nvidia[1].pci;
        CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_INVALID_PARAMETER);
        CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
        plan.inventory.nvidia[0]=saved;
        plan.inventory.nvidia[0].segment=plan.inventory.nvidia[1].segment;
        plan.inventory.nvidia[0].bus=plan.inventory.nvidia[1].bus;
        plan.inventory.nvidia[0].device=plan.inventory.nvidia[1].device;
        plan.inventory.nvidia[0].function=plan.inventory.nvidia[1].function;
        CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_INVALID_PARAMETER);
        CHECK(refreshed.count==0 && refreshed.targets[1].pci==NULL);
        plan.inventory.nvidia[0]=saved;
      }
      saved_index=plan.selected_indices[1];
      plan.selected_indices[1]=plan.inventory.count;
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_INVALID_PARAMETER);
      CHECK(refreshed.count==0);
      plan.selected_indices[1]=saved_index;

      plan.selected_count=0;
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_INVALID_PARAMETER);
      CHECK(refreshed.count==0);
      plan.selected_count=2;

      gpu[0].read_fail=1; /* Second selected target fails after first succeeded. */
      memset(&refreshed,0xa5,sizeof(refreshed));
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_UNSUPPORTED);
      CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL &&
            refreshed.targets[0].config[0]==0 && refreshed.targets[1].pci==NULL);
      gpu[0].read_fail=0;

      /* GPU[1] changes already-sampled GPU[0] during the first pass.
       * This preserves the mid-snapshot drift failure invariant after the
       * aggregate refresh switches from selection to inventory order. */
      gpu[1].drift_nonselected_on_full_read=1;
      memset(&refreshed,0xa5,sizeof(refreshed));
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_UNSUPPORTED);
      CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL &&
            refreshed.targets[1].pci==NULL && !init_calls);
      gpu[0].dwords[4]^=0x1000U; /* Restore first-sampled BAR. */
      CHECK(GuRefreshPreparedSequence(&system_table,&plan,&refreshed)==EFI_SUCCESS);
      CHECK(refreshed.count==2 && refreshed.targets[0].pci==&gpu[1].protocol);
    }
    /* A saved target must still be the exact same boot-local PCI_IO owner,
     * with unchanged BDF, subsystem, class and device ID. No GuInit occurs. */
    {
      UINT32 config[64];
      EFI_PCI_IO_PROTOCOL *io;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_SUCCESS);
      CHECK(io==&gpu[1].protocol && config[0]==Secondary.pci_id && !init_calls);
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,1,config,&io)==EFI_SUCCESS);
      CHECK(io==&gpu[0].protocol && config[0]==Primary.pci_id && !init_calls);
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,2,config,&io)==EFI_INVALID_PARAMETER);
      CHECK(io==NULL && config[0]==0);
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,NULL,&io)==EFI_INVALID_PARAMETER);
      CHECK(io==NULL);
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,NULL)==EFI_INVALID_PARAMETER);
      CHECK(GuRefreshPreparedGpu(NULL,&plan,0,config,&io)==EFI_INVALID_PARAMETER);
      CHECK(io==NULL && config[0]==0);
      gpu[1].dwords[0x2c/4]^=1U;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_UNSUPPORTED);
      CHECK(io==NULL && config[0]==0);
      gpu[1].dwords[0x2c/4]^=1U;
      gpu[1].dwords[2]^=0x10000U;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_UNSUPPORTED);
      gpu[1].dwords[2]^=0x10000U;
      gpu[1].bus++;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_UNSUPPORTED);
      gpu[1].bus--;
      gpu[1].mutate_at_full_read=1;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_UNSUPPORTED);
      gpu[1].mutate_at_full_read=0;
      gpu[1].read_fail=1;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_UNSUPPORTED);
      gpu[1].read_fail=0;
      redirect_on_refresh=1;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,1,config,&io)==EFI_UNSUPPORTED);
      redirect_on_refresh=0;
      plan.selected_indices[0]=plan.inventory.count;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_INVALID_PARAMETER);
      CHECK(io==NULL && config[0]==0 && !init_calls);
      plan.selected_indices[0]=1;
      CHECK(GuRefreshPreparedGpu(&system_table,&plan,0,config,&io)==EFI_SUCCESS);
      CHECK(!init_calls);
    }
    CHECK(GuPrepareGpuSequence(&system_table,NULL,0,&plan)==EFI_SUCCESS);
    CHECK(plan.inventory.count==2 && plan.selected_count==0 && init_calls==0);
    CHECK(GuPrepareGpuSequence(&system_table,missing,2,&plan)==EFI_UNSUPPORTED);
    CHECK(plan.selected_count==0 && plan.inventory.count==0 && !init_calls);
    CHECK(GuPrepareGpuSequence(&system_table,repeated,2,&plan)==EFI_UNSUPPORTED);
    CHECK(plan.selected_count==0 && plan.inventory.count==0 && !init_calls);
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,NULL)==EFI_INVALID_PARAMETER);
    CHECK(GuPrepareGpuSequence(&system_table,NULL,2,&plan)==EFI_INVALID_PARAMETER);
    CHECK(GuPrepareGpuSequence(&system_table,requested,0,&plan)==EFI_INVALID_PARAMETER);
    CHECK(GuPrepareGpuSequence(&system_table,requested,GU_INVENTORY_MAX_NVIDIA+1,&plan)==EFI_INVALID_PARAMETER);
    CHECK(GuPrepareGpuSequence(NULL,requested,2,&plan)==EFI_INVALID_PARAMETER);
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,&plan)==EFI_SUCCESS);
    ResetPair();Populate(0,Secondary);Populate(1,Primary);
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,&plan)==EFI_SUCCESS);
    CHECK(plan.selected_indices[0]==0 && plan.selected_indices[1]==1 && !init_calls);
    ResetPair();count=3;Populate(2,Primary);gpu[2].bus=6;
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,&plan)==EFI_SUCCESS);
    CHECK(plan.inventory.count==3 && plan.selected_count==2 && !init_calls);
    ResetPair();fail_pool_free=1;
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,&plan)==EFI_UNSUPPORTED);
    CHECK(plan.inventory.count==0 && plan.selected_count==0 && !init_calls);
    ResetPair();gpu[1].read_fail=1;
    CHECK(GuPrepareGpuSequence(&system_table,requested,2,&plan)==EFI_UNSUPPORTED);
    CHECK(plan.inventory.count==0 && plan.selected_count==0 && !init_calls);
  }
  /* A one-card request MUST revalidate its unselected NVIDIA peer as well.
   * Every refusal leaves the caller's output blank and never calls GuInit. */
  {
    GU_GPU_PREPARED_SEQUENCE single;
    GU_GPU_REFRESHED_SEQUENCE refreshed;
    const GU_GPU_LOCATION target = {1,7,4,1};
    GU_GPU_RECORD saved;
    ResetPair();
    CHECK(GuPrepareGpuSequence(&system_table,&target,1,&single)==EFI_SUCCESS);
    CHECK(single.inventory.count==2 && single.selected_count==1 &&
          single.selected_indices[0]==1 && !init_calls);
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_SUCCESS);
    CHECK(refreshed.count==1 && refreshed.targets[0].pci==&gpu[1].protocol &&
          refreshed.targets[0].config[0]==Secondary.pci_id);
    CHECK(gpu[0].full_reads==2 && gpu[1].full_reads==2);
    /* The source inventory must be current: adding or removing an
     * NVIDIA handle between preparation and refresh is a refusal, even
     * though the saved handles could still be read successfully. */
    count=1;
    memset(&refreshed,0xa5,sizeof(refreshed));
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_UNSUPPORTED);
    CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
    count=3;
    Populate(2,Primary);
    gpu[2].bus=6;
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_UNSUPPORTED);
    CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
    count=2;
    gpu[0].read_fail=1; /* Unselected target is unreadable. */
    memset(&refreshed,0xa5,sizeof(refreshed));
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_UNSUPPORTED);
    CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL &&
          refreshed.targets[0].config[0]==0);
    gpu[0].read_fail=0;
    gpu[0].bus++; /* Unselected BDF drift after preparation. */
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_UNSUPPORTED);
    CHECK(refreshed.count==0);
    gpu[0].bus--;
    saved=single.inventory.nvidia[0];
    single.inventory.nvidia[0].pci=single.inventory.nvidia[1].pci;
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_INVALID_PARAMETER);
    CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
    single.inventory.nvidia[0]=saved;
    single.inventory.nvidia[0].segment=single.inventory.nvidia[1].segment;
    single.inventory.nvidia[0].bus=single.inventory.nvidia[1].bus;
    single.inventory.nvidia[0].device=single.inventory.nvidia[1].device;
    single.inventory.nvidia[0].function=single.inventory.nvidia[1].function;
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_INVALID_PARAMETER);
    CHECK(refreshed.count==0);
    single.inventory.nvidia[0]=saved;
    /* GPU[1] changes GPU[0]'s BAR after GPU[0] was sampled. */
    gpu[1].drift_nonselected_on_full_read=1;
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_UNSUPPORTED);
    CHECK(refreshed.count==0 && refreshed.targets[0].pci==NULL);
    gpu[0].dwords[4]^=0x1000U; /* Restore non-target BAR. */
    CHECK(GuRefreshPreparedSequence(&system_table,&single,&refreshed)==EFI_SUCCESS);
    CHECK(refreshed.count==1 && refreshed.targets[0].pci==&gpu[1].protocol);
    CHECK(!init_calls);
  }
  ResetPair();
  status = GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table);
  CHECK(status == EFI_UNSUPPORTED && init_calls == 0 && free_calls == 1 && log_calls >= 3);

  ResetPair();
  Populate(0, Secondary); Populate(1, Primary);
  status = GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table);
  CHECK(status == EFI_UNSUPPORTED && init_calls == 0 && free_calls == 1);

  ResetPair(); gpu[1].dwords[0x2c / 4] ^= 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); gpu[1].bus = gpu[0].bus;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); gpu[0].read_fail = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); gpu[0].location_fail = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); fail_pool_free = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 3;
  Populate(2, Primary); gpu[2].bus = 6;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_SUCCESS && init_calls == 1);
  CHECK(Gu.handle == handles[0] && Gu.device_id == 0x20b0 && !Gu.is_ampere);

  ResetPair(); count = 1; gpu[0].mutate_at_full_read = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);
  ResetPair(); count = 1; redirect_on_refresh = 1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 1; gpu[0].dwords[0] = 0x123410deU;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_UNSUPPORTED && !init_calls);

  ResetPair(); count = 2; gpu[1].dwords[0] = 0x164e1002U;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1, &system_table) == EFI_SUCCESS && init_calls == 1);
  CHECK(Gu.handle == handles[0]);

  printf("PASS UEFI discovery adapter: %u checks; mocked protocols only\n", checks);
  return 0;
}
#endif
