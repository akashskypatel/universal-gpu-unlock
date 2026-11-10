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
} MOCK_DEVICE;
static MOCK_DEVICE gpu[4];
static EFI_HANDLE handles[4];
static size_t count;
static unsigned init_calls, free_calls, log_calls, resolve_calls;
static int fail_pool_free, redirect_on_refresh;
GU_PLATFORM Gu;

void GuInit(EFI_BOOT_SERVICES *bs, EFI_PCI_IO_PROTOCOL *pci) {
  init_calls++; memset(&Gu, 0, sizeof(Gu)); Gu.bs = bs; Gu.pci = pci;
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
  memset(caps, 0, sizeof(*caps)); caps->pm = 0x40; caps->pcie = 0x60;
  return 1;
}
static EFI_STATUS Read(EFI_PCI_IO_PROTOCOL *io, int width, UINT32 offset, UINTN n, VOID *out) {
  MOCK_DEVICE *m = (MOCK_DEVICE *)io->context;
  if (m->read_fail || width != EfiPciIoWidthUint32 || !out ||
      (offset & 3) || offset / 4 + n > 64) return 3;
  memcpy(out, m->dwords + offset / 4, n * sizeof(UINT32));
  if (m->mutate_at_full_read && n == 64)
    ((UINT32 *)out)[0x2c / 4] ^= 1U;
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
  fail_pool_free = redirect_on_refresh = 0; resolve_calls = 0; count = 2;
  Populate(0, Primary); Populate(1, Secondary);
}
#include "../src/gpu_discovery.c"

#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
int main(void) {
  EFI_STATUS status;
  ResetPair();
  status=GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table);
  CHECK(status==EFI_UNSUPPORTED && init_calls==0 && free_calls==1);
  ResetPair(); count=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED && init_calls==0);
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
