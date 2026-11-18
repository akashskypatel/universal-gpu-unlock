/* SPDX-License-Identifier: GPL-2.0-only */
#define GU_DISCOVERY_HOST_TEST 1
#include "gpu_discovery_mock.h"
#include "../src/gpu_inventory.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GU_GPU_IDENTITY Primary={0x20b010deU,0xa12310deU,0,3,0,0};
static const GU_GPU_IDENTITY Secondary={0x250310deU,0xb45610deU,1,7,4,1};
static unsigned checks;
#ifdef GU_ACTIVE_SINGLE_TARGET
static int release_owner_token,other_owner_token;
#endif
static unsigned state_allocs,state_frees,fail_state_alloc,fail_state_free;
static void *last_failed_free;
static unsigned fail_state_alloc_with_output;
static void *uncertain_state;
static UINTN uncertain_state_bytes;
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
static unsigned context_allocs,context_frees,fail_context_free;
#endif
#ifdef GU_ACTIVE_SINGLE_TARGET
static int refuse_advance,refuse_completed,refuse_final_graph,drift_after_graph;
#endif
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

typedef struct {
  EFI_PCI_IO_PROTOCOL protocol;
  UINT32 dwords[64];
  UINTN segment, bus, device, function;
  int read_fail, location_fail, mutate_at_full_read;
  int drift_nonselected_on_full_read;
  unsigned full_reads;
} MOCK_DEVICE;
static MOCK_DEVICE gpu[24];
static EFI_HANDLE handles[24];
static size_t count;
static unsigned init_calls, free_calls, log_calls, resolve_calls, locate_calls;
static void *failed_handle_buffer;
static unsigned prep_markers, prep_refusals, global_refusals;
static int fail_pool_free, fail_locate, redirect_on_refresh, refuse_init;

static GU_PLATFORM MockPrimaryPlatform;
GU_PLATFORM *GuPlatformActive=&MockPrimaryPlatform;

EFI_STATUS GuInit(EFI_BOOT_SERVICES *bs, EFI_PCI_IO_PROTOCOL *pci) {
  init_calls++;
  if(refuse_init)return EFI_ALREADY_STARTED;
  memset(&Gu,0,sizeof(Gu));Gu.bs=bs;Gu.pci=pci;
  return EFI_SUCCESS;
}
void GuLog(const char *format, ...) {
  log_calls++;
  if(!format)return;
  if(strstr(format,"ACTIVE_SEQUENCE=PREPARED_NOT_STARTED"))prep_markers++;
  if(strstr(format,"REASON=SELECTION_PREPARATION_ONLY"))prep_refusals++;
  if(strstr(format,"REASON=MULTI_NVIDIA_GLOBAL_STATE"))global_refusals++;
}
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
  locate_calls++;
  *outCount = count;
  *outHandles = fail_locate==1?NULL:handles;
  return fail_locate?EFI_NOT_READY:EFI_SUCCESS;
}
static EFI_STATUS Resolve(EFI_HANDLE h, EFI_GUID *guid, VOID **io) {
  (void)guid; resolve_calls++;
  *io = (redirect_on_refresh && resolve_calls > count) ? handles[1] : h;
  return EFI_SUCCESS;
}
static EFI_STATUS Free(VOID *p) {
  if(p && p!=handles) {
    if(fail_state_free){fail_state_free=0;last_failed_free=p;return EFI_NOT_READY;}
    free(p);state_frees++;return EFI_SUCCESS;
  }
  free_calls++;
  if(p!=handles)return EFI_INVALID_PARAMETER;
  if(fail_pool_free){failed_handle_buffer=p;return EFI_UNSUPPORTED;}
  return EFI_SUCCESS;
}
static EFI_STATUS Allocate(int kind, UINTN bytes, VOID **out) {
  if(kind!=EfiLoaderData || !out || !bytes)return EFI_INVALID_PARAMETER;
  state_allocs++;
  if(fail_state_alloc==state_allocs)return EFI_OUT_OF_RESOURCES;
  if(fail_state_alloc_with_output==state_allocs) {
    *out=malloc(bytes);
    if(*out) {
      memset(*out,0xa5,bytes);
      uncertain_state=*out;uncertain_state_bytes=bytes;
    }
    return EFI_OUT_OF_RESOURCES;
  }
  *out=calloc(1,bytes);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_ACTIVE_SINGLE_TARGET)
EFI_STATUS GuAllocatePlatform(EFI_BOOT_SERVICES *bs,EFI_PCI_IO_PROTOCOL *pci,GU_PLATFORM **out) {
  if(!bs || !pci || !out || *out)return EFI_INVALID_PARAMETER;
  EFI_STATUS status=bs->AllocatePool(EfiLoaderData,sizeof(**out),(VOID **)out);
  if(status==EFI_SUCCESS) {
    (*out)->bs=bs;(*out)->pci=pci;context_allocs++;
  }
  return status;
}
EFI_STATUS GuDiscardUnboundPlatform(GU_PLATFORM **out) {
  if(!out || !*out)return EFI_INVALID_PARAMETER;
  if(fail_context_free && (*out)->pci==&gpu[1].protocol)return EFI_NOT_READY;
  EFI_STATUS status=(*out)->bs->FreePool(*out);
  if(status==EFI_SUCCESS){*out=NULL;context_frees++;}
  return status;
}
#endif
#ifdef GU_ACTIVE_SINGLE_TARGET
/* The registry transition itself is tested from the actual port.c source
 * in test_platform_bind_lifetime.py; this mock covers the adapter boundary. */
EFI_STATUS GuAdvanceAllocatedPlatform(EFI_BOOT_SERVICES *bs,GU_PLATFORM **owner,
    const GU_GPU_EXEC_SEQUENCE *sequence,const GU_GPU_EXEC_OBSERVATION *observed,size_t n) {
  if(refuse_advance||!owner||!*owner||(*owner)->bs!=bs||
     GuGpuExecCheckNext(sequence,observed,n)!=GU_GPU_EXEC_OK||
     (*owner)->pci!=sequence->targets[sequence->order[sequence->next_step]].identity.pci)
    return EFI_NOT_READY;
  GuPlatformActive=*owner;*owner=NULL;
  return EFI_SUCCESS;
}
EFI_STATUS GuVerifyCompletedPlatforms(EFI_BOOT_SERVICES *bs,
    const GU_GPU_EXEC_SEQUENCE *sequence,const GU_GPU_EXEC_OBSERVATION *observed,size_t n) {
  (void)bs;
  if(refuse_completed||GuGpuExecCheckComplete(sequence,observed,n)!=GU_GPU_EXEC_OK)
    return EFI_NOT_READY;
  return EFI_SUCCESS;
}
int GuMsiVerifyFinalGraph(void){
  if(drift_after_graph)gpu[1].dwords[4]^=0x4000U;
  return !refuse_final_graph;
}
#endif
static EFI_BOOT_SERVICES services = { Locate, Resolve, Free, Allocate };
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
#include "../src/gpu_discovery.c"

static void ResetPair(void) {
  GuPlatformActive=&MockPrimaryPlatform;
  memset(&Gu, 0, sizeof(Gu)); init_calls = free_calls = log_calls = 0;
  prep_markers = prep_refusals = global_refusals = 0;
  fail_pool_free = fail_locate = redirect_on_refresh = refuse_init = 0; resolve_calls = 0; count = 2;
  state_allocs=state_frees=fail_state_alloc=fail_state_free=0;
  fail_state_alloc_with_output=0;uncertain_state=NULL;uncertain_state_bytes=0;
  last_failed_free=NULL;
  /* Test-only boot reset. The production path never retries release. */
  UnreleasedPciHandles=NULL;
  failed_handle_buffer=NULL;locate_calls=0;
  Populate(0, Primary); Populate(1, Secondary);
}

static int CheckHandleBufferLifetime(void) {
  /* Invalid census size still transfers a successful Locate buffer, and
   * the release must be attempted exactly once, even on refusal. */
  for(int scenario=0;scenario<4;scenario++){
    ResetPair();
    if(scenario==0)count=0;
    if(scenario==1)count=GU_INVENTORY_MAX_HANDLES+1;
    if(scenario==2)fail_state_alloc=1;
    if(scenario==3)gpu[0].read_fail=1;
    fail_pool_free=1;
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED);
    CHECK(!init_calls&&locate_calls==1&&free_calls==1);
    CHECK(UnreleasedPciHandles==handles&&UnreleasedPciHandles==failed_handle_buffer);
    CHECK(state_allocs==(unsigned)(scenario>=2));
    CHECK(state_frees==(unsigned)(scenario==3));
    fail_pool_free=0;
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
    CHECK(locate_calls==1&&free_calls==1&&!init_calls);
    GU_GPU_PREPARED_SEQUENCE prepared;
    CHECK(GuPrepareGpuSequence(&system_table,NULL,0,&prepared)==EFI_ALREADY_STARTED);
    CHECK(!prepared.inventory.count&&!prepared.selected_count);
    CHECK(locate_calls==1&&free_calls==1);
  }
  /* Ordinary error without output must not poison the census. */
  ResetPair();fail_locate=1;
  EFI_HANDLE *out=NULL;UINTN n=0;
  CHECK(LocatePciHandles(&system_table,&out,&n)==EFI_UNSUPPORTED&&!out&&!n);
  CHECK(!UnreleasedPciHandles&&locate_calls==1&&!free_calls&&!resolve_calls);
  /* Error with a nonnull address is not an owned allocation. */
  ResetPair();fail_locate=2;
  CHECK(LocatePciHandles(&system_table,&out,&n)==EFI_DEVICE_ERROR&&!out&&!n);
  CHECK(UnreleasedPciHandles==handles&&locate_calls==1&&!free_calls&&!resolve_calls);
  CHECK(!state_allocs&&!init_calls);
  fail_locate=0;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
  GU_GPU_PREPARED_SEQUENCE prepared;
  CHECK(GuPrepareGpuSequence(&system_table,NULL,0,&prepared)==EFI_ALREADY_STARTED);
  CHECK(!prepared.inventory.count&&!prepared.selected_count);
  CHECK(locate_calls==1&&!free_calls&&!resolve_calls&&!state_allocs&&!init_calls);
  /* Error with nonnull discovery storage must not publish or initialize it.
   * The independently confirmed enumeration buffer is still released once. */
  ResetPair();fail_state_alloc_with_output=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_DEVICE_ERROR);
  CHECK(uncertain_state&&uncertain_state_bytes&&UnreleasedDiscoveryAllocation==uncertain_state);
  CHECK(state_allocs==1&&!state_frees&&locate_calls==1&&free_calls==1);
  CHECK(!resolve_calls&&!init_calls&&!UnreleasedPciHandles);
  for(UINTN i=0;i<uncertain_state_bytes;i++)
    CHECK(((const unsigned char *)uncertain_state)[i]==0xa5);
  fail_state_alloc_with_output=0;
  GU_GPU_DISCOVERY_STORAGE empty={0};
  CHECK(AllocateDiscoveryStorage(&services,2,&empty)==EFI_ALREADY_STARTED);
  CHECK(!empty.allocation&&!empty.capacity&&!empty.records&&!empty.live);
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
  CHECK(state_allocs==1&&!state_frees&&locate_calls==1&&free_calls==1&&!init_calls);
  free(uncertain_state); /* Host fixture teardown, not a production release. */
  UnreleasedDiscoveryAllocation=NULL;
  ResetPair();
  return 0;
}

static void ExpandInventory(size_t n) {
  count=n;
  for(size_t i=2;i<n;i++){Populate(i,Primary);gpu[i].bus=20+i;}
}

#if defined(GU_ACTIVE_SINGLE_TARGET)
/* The production single-target path supports the user's segment-0 PCI cards.
 * Earlier mock Segment=1 is intentionally not accepted by GuCheckConfig. */
static void ResetActivePair(void) {
  /* A test reset models a new boot, not a production unbind. */
  free(ActiveDiscovery.allocation);
  memset(&ActiveDiscovery,0,sizeof(ActiveDiscovery));
  memset(&ActiveInventory,0,sizeof(ActiveInventory));
  /* Test reset represents a new boot, not production disposal/retry. */
  free(UnreleasedDiscoveryAllocation);UnreleasedDiscoveryAllocation=NULL;
  memset(&ActiveSequence,0,sizeof(ActiveSequence));
  ActiveSequenceValid=ActiveInventoryValid=0;
  state_allocs=state_frees=fail_state_alloc=0;
  context_allocs=context_frees=fail_context_free=0;
  refuse_advance=refuse_completed=refuse_final_graph=drift_after_graph=0;
  ResetPair();gpu[1].segment=0;gpu[1].device=0;gpu[1].function=0;
}
static void PrepareMockOwners(void) {
  Gu.release_state=&release_owner_token;
  Gu.asset_entries_reserved=1;
}
int main(void) {
  CHECK(CheckHandleBufferLifetime()==0);
#if defined(GU_SYNTHETIC_COMPILE_ONLY) && GU_GPU_SELECTION_COUNT > 1
  /* Exercise actual discovery admission only in this host/compile profile.
   * The generated selections are [1,2,0,3,...] in a 20-device inventory. */
  enum { selected_count=GU_GPU_SELECTION_COUNT };
  for(int fail_late=0;fail_late<2;fail_late++){
    ResetActivePair();
    ExpandInventory(20);
    gpu[2].bus=9;
    int lease[selected_count]={0};
    GU_PLATFORM *owners[selected_count]={0};
    CHECK(selected_count<=18);
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
    CHECK(init_calls==1&&ActiveSequence.selected_count==selected_count&&ActiveSequence.count==20);
    CHECK(ActiveDiscovery.capacity==20&&GuIsPreparedPeerHandle(handles[19]));
    size_t completed=0;
    for(size_t i=0;i<selected_count;i++) {
      size_t target=i<3?(i+1)%3:i;
      CHECK(Gu.handle==handles[target]&&Gu.pci==&gpu[target].protocol);
      CHECK(Gu.discovery_identity.handle==Gu.handle&&Gu.discovery_identity.pci==Gu.pci);
      CHECK(Gu.discovery_identity.pci_id==ActiveSequence.targets[target].identity.pci_id);
      CHECK(Gu.discovery_identity.subsystem_id==ActiveSequence.targets[target].identity.subsystem_id);
      CHECK(Gu.discovery_identity.class_code==ActiveSequence.targets[target].identity.class_code);
      CHECK(Gu.discovery_identity.segment==ActiveSequence.targets[target].identity.segment&&
            Gu.discovery_identity.bus==ActiveSequence.targets[target].identity.bus&&
            Gu.discovery_identity.device==ActiveSequence.targets[target].identity.device&&
            Gu.discovery_identity.function==ActiveSequence.targets[target].identity.function);
      CHECK(ActiveSequence.order[i]==target&&ActiveSequence.next_step==i);
      CHECK(GuIsPreparedPeerHandle(handles[19]));
      CHECK(!Gu.profile_verified&&!Gu.prom_verified&&!Gu.release_state&&!Gu.asset_entries_reserved);
      owners[i]=GuPlatformActive;
      Gu.profile_verified=Gu.prom_verified=Gu.asset_entries_reserved=1;
      Gu.release_state=&lease[i];
      CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,&lease[i],Gu.asset_entries)==EFI_SUCCESS);
      gpu[target].dwords[4]^=(UINT32)(0x1000U*(i+1));
      CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,&lease[i],Gu.asset_entries)==EFI_SUCCESS);
      completed++;
      int more=0;
      CHECK(GuSelectedSequenceHasNext(&system_table,&more)==EFI_SUCCESS && more==(i+1<selected_count));
      if(more){
        if(fail_late&&i+2==selected_count){
          fail_state_alloc=state_allocs+1;
          CHECK(GuSelectNextGpu(&system_table)==EFI_OUT_OF_RESOURCES);
          CHECK(GuPlatformActive==owners[i]&&ActiveSequence.halted);
          CHECK(ActiveSequence.next_step==i+1);
          CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
          break;
        }
        CHECK(GuSelectNextGpu(&system_table)==EFI_SUCCESS);
      }
    }
    CHECK(completed==(size_t)(selected_count-fail_late));
    CHECK(context_allocs==completed-1 && !context_frees && init_calls==1);
    for(size_t i=0;i<completed;i++) {
      size_t target=i<3?(i+1)%3:i;
      CHECK(ActiveSequence.targets[target].owners.platform==owners[i]);
      CHECK(owners[i]->release_state==&lease[i]);
      CHECK(ActiveSequence.targets[target].expected_config[4]==gpu[target].dwords[4]);
      for(size_t j=0;j<i;j++)CHECK(owners[i]!=owners[j]);
    }
    if(fail_late){
      CHECK(ActiveSequence.targets[ActiveSequence.order[completed]].phase==GU_GPU_EXEC_PENDING);
      CHECK(GuVerifySelectedSequence(&system_table)==EFI_NOT_READY);
    }else{
      CHECK(GuVerifySelectedSequence(&system_table)==EFI_SUCCESS);
      gpu[19].dwords[4]^=0x1000U;
      CHECK(GuVerifySelectedSequence(&system_table)==EFI_NOT_READY && ActiveSequence.halted);
      CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
    }
    GuPlatformActive=owners[0];
    for(size_t i=1;i<completed;i++)free(owners[i]);
  }
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED);
  CHECK(!init_calls&&!ActiveSequenceValid);
  printf("PASS compile-profile actual ordered discovery: %u checks; selected=%u observed=20\n",checks,(unsigned)selected_count);
  return 0;
#endif
  EFI_STATUS status;
  ResetActivePair();
  status=GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table);
  CHECK(status==EFI_SUCCESS && init_calls==1);
  CHECK(state_allocs==1 && state_frees==0 && ActiveSequence.capacity==2);
  CHECK(Gu.handle==handles[1] && Gu.device_id==0x2503U);
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  /* A refused rebind must not replace the previous target or saved peer
   * record, even after the new inventory has passed both PCI read passes. */
  {
    EFI_HANDLE old=Gu.handle;
    UINT32 original_peer=ActiveSequence.targets[0].expected_config[4];
    GU_GPU_EXEC_TARGET *original_targets=ActiveSequence.targets;
    size_t *original_order=ActiveSequence.order;
    GU_GPU_EXEC_OBSERVATION *original_observations=ActiveDiscovery.observed;
    refuse_init=1;
    gpu[0].dwords[4]^=0x1000U;
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
    CHECK(Gu.handle==old && ActiveInventoryValid);
    CHECK(ActiveSequence.targets[0].expected_config[4]==original_peer);
    CHECK(ActiveSequence.targets==original_targets && ActiveSequence.order==original_order && ActiveDiscovery.observed==original_observations);
    CHECK(state_allocs==2 && state_frees==1 && init_calls==1);
    refuse_init=0;
  }
  /* One allocation contains every sequence backing region. No partial
   * owner may be published, even when firmware rejects a release. */
  ResetActivePair();fail_state_alloc=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_OUT_OF_RESOURCES);
  CHECK(!ActiveSequenceValid && !ActiveDiscovery.allocation && !ActiveDiscovery.targets &&
        init_calls==0 && state_allocs==1 && state_frees==0);
  ResetActivePair();refuse_init=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
  CHECK(!ActiveSequenceValid && !ActiveDiscovery.allocation && !ActiveDiscovery.order &&
        !ActiveDiscovery.observed && init_calls==1 && state_allocs==1 && state_frees==1);
  ResetActivePair();refuse_init=1;fail_state_free=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_NOT_READY);
  CHECK(!ActiveSequenceValid&&!ActiveDiscovery.allocation&&UnreleasedDiscoveryAllocation);
  CHECK(UnreleasedDiscoveryAllocation==last_failed_free&&state_allocs==1&&!state_frees);
  refuse_init=0;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
  CHECK(state_allocs==1&&init_calls==1&&!state_frees);
  /* A failed second candidate release must not replace the published owner. */
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  {
    void *saved=ActiveDiscovery.allocation;
    GU_GPU_EXEC_TARGET *targets=ActiveSequence.targets;
    size_t *order=ActiveSequence.order;
    fail_state_free=1;
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_NOT_READY);
    CHECK(UnreleasedDiscoveryAllocation==last_failed_free&&UnreleasedDiscoveryAllocation!=saved);
    CHECK(ActiveDiscovery.allocation==saved&&ActiveSequence.targets==targets&&ActiveSequence.order==order);
    CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
    CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
    CHECK(state_allocs==2&&init_calls==1&&!state_frees);
  }
  ResetActivePair();
  gpu[1].dwords[1]=4u; /* initial selected UEFI driver Bus Master enabled */
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  CHECK(ActiveDiscovery.observed && ActiveSequence.capacity==2 &&
        ActiveSequence.targets==ActiveDiscovery.targets &&
        ActiveSequence.order==ActiveDiscovery.order &&
        ActiveInventory.inventory.nvidia==ActiveDiscovery.records);
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

  /* Actual source integration of the selected RTX 4090/3080 step model.
   * These are mocked PCI reads; cleanup_verified below is a supplied
   * model input, NOT physical driver Stop or firmware authentication. */
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  CHECK(ActiveInventoryValid && ActiveSequenceValid &&
        ActiveSequence.selected_count==1 && ActiveSequence.next_step==0);
  CHECK(ActiveSequence.targets[ActiveSequence.order[0]].identity.pci==Gu.pci);
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  Gu.profile_verified=1;
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  Gu.prom_verified=1;
  CHECK(GuBeginSelectedGpuStep(&system_table,&other_owner_token,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &other_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,&other_owner_token)==EFI_NOT_READY);
  Gu.release_state=NULL;
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  PrepareMockOwners();
  Gu.asset_entries_reserved=0;
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  PrepareMockOwners();
  CHECK(ActiveSequence.next_step==0 && ActiveSequence.active_step==GU_GPU_EXEC_NO_STEP);
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(ActiveSequence.active_step==0 &&
        ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_ACTIVE);
  CHECK(ActiveSequence.targets[ActiveSequence.order[0]].owners.platform==&Gu &&
        ActiveSequence.targets[ActiveSequence.order[0]].owners.release_plan==&release_owner_token &&
        ActiveSequence.targets[ActiveSequence.order[0]].owners.firmware_assets==Gu.asset_entries);
  /* A failed cleanup verdict cannot publish a completed GPU. */
  CHECK(GuFinishSelectedGpuStep(&system_table,0,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  CHECK(ActiveSequence.halted &&
        ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_FAILED);
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_UNSUPPORTED);

  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  /* CPU model permits selected post-step PCI state, but not peer drift. */
  gpu[1].dwords[4]^=0x1000U;
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(ActiveSequence.next_step==1 &&
        ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_COMPLETE &&
        ActiveSequence.targets[ActiveSequence.order[0]].expected_config[4]==gpu[1].dwords[4]);
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  gpu[1].dwords[4]^=0x2000U;
  CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
  gpu[1].dwords[4]^=0x2000U;
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  /* The active context must still belong to the saved selected device. */
  Gu.handle=handles[0];
  CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
  Gu.handle=handles[1];
  CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_UNSUPPORTED);

  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  gpu[0].dwords[4]^=0x1000U;
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_UNSUPPORTED &&
        ActiveSequence.halted);
  CHECK(ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_FAILED);

  /* Reject a changed live owner even if PCI and handover are unchanged. */
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,
                                Gu.asset_entries,&release_owner_token)==EFI_NOT_READY);
  CHECK(ActiveSequence.halted &&
        ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_FAILED);

  /* A caller can retain the old token even after the live context changes. */
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  Gu.release_state=&other_owner_token;
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  CHECK(ActiveSequence.halted &&
        ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_FAILED);

  /* Reject a changed saved owner, not only a changed input. */
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  ActiveSequence.targets[ActiveSequence.order[0]].owners.firmware_assets=&release_owner_token;
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  CHECK(ActiveSequence.halted &&
        ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_FAILED);

  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(GuFinishSelectedGpuStep(&system_table,1,NULL,
                                &release_owner_token,Gu.asset_entries)==EFI_NOT_READY);
  CHECK(ActiveSequence.halted);

  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;
  PrepareMockOwners();
  gpu[0].dwords[4]^=0x1000U; /* drift before beginning selected work */
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,
                                &release_owner_token,Gu.asset_entries)==EFI_UNSUPPORTED &&
        ActiveSequence.halted);
  CHECK(ActiveSequence.next_step==0 && ActiveSequence.active_step==GU_GPU_EXEC_NO_STEP);

  /* Exercise actual adapter selection and the registry mock boundary.
   * Direct pointer changes below are negative fault injection only. */
  for(int scenario=0;scenario<8;scenario++) {
    ResetActivePair();
    GU_GPU_DISCOVERY_STORAGE storage={0};
    CHECK(AllocateDiscoveryStorage(&services,2,&storage)==EFI_SUCCESS);
    GU_GPU_PREPARED_VIEW prepared=DiscoveryView(&storage);
    GU_GPU_LOCATION requested[2]={{0,7,0,0},{0,3,0,0}};
    GU_PLATFORM *second=NULL;
    CHECK(ReadGpuRecords(&system_table,prepared.inventory.nvidia,storage.capacity,&prepared.inventory.count)==EFI_SUCCESS);
    CHECK(SelectPreparedRecords(&prepared,requested,2)==EFI_SUCCESS);
    CHECK(RefreshGpuRecords(&system_table,&prepared,storage.live,storage.scanned,storage.capacity)==EFI_SUCCESS);
    for(size_t i=0;i<2;i++){
      storage.observed[i].identity=prepared.inventory.nvidia[i];
      memcpy(storage.observed[i].config,storage.scanned[i].config,sizeof(storage.observed[i].config));
    }
    CHECK(BindPreparedSequence(&system_table,&gpu[1].protocol,&prepared,&storage)==EFI_SUCCESS);
    CHECK(!storage.allocation&&ActiveDiscovery.allocation);
    Gu.image=(EFI_HANDLE)1;Gu.handle=handles[1];
    Gu.profile_verified=Gu.prom_verified=1;PrepareMockOwners();
    CHECK(GuIsPreparedPeerHandle(handles[0]) && !GuIsPreparedPeerHandle(handles[1]));
    CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,&release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
    CHECK(ActiveSequence.active_step==0);
    CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
    gpu[1].dwords[4]^=0x1000U;
    CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,&release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
    CHECK(ActiveSequence.next_step==1 && GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
    int more=0;
    CHECK(GuSelectedSequenceHasNext(&system_table,&more)==EFI_SUCCESS && more==1);
    CHECK(GuSelectedSequenceHasNext(&system_table,NULL)==EFI_INVALID_PARAMETER);
    GU_PLATFORM *first=GuPlatformActive;
    if(scenario>=1&&scenario<=3) {
      EFI_STATUS expected=EFI_NOT_READY;
      if(scenario==1)refuse_advance=1;
      if(scenario==2){fail_state_alloc=state_allocs+1;expected=EFI_OUT_OF_RESOURCES;}
      if(scenario==3){gpu[0].dwords[4]^=0x4000U;expected=EFI_UNSUPPORTED;}
      CHECK(GuSelectNextGpu(&system_table)==expected);
      CHECK(ActiveSequence.halted && ActiveSequence.next_step==1);
      CHECK(GuPlatformActive==first && Gu.release_state==&release_owner_token);
      CHECK(ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_COMPLETE);
      CHECK(context_allocs==(unsigned)(scenario==1) && context_frees==context_allocs);
      CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
      continue;
    }
    CHECK(GuSelectNextGpu(&system_table)==EFI_SUCCESS);
    second=GuPlatformActive;
    CHECK(second!=first && context_allocs==1 && !context_frees);
    CHECK(second->bs==&services && second->pci==&gpu[0].protocol && second->handle==handles[0]);
    CHECK(second->image==first->image && second->device_id==(Primary.pci_id>>16));
    CHECK(!second->profile_verified && !second->release_state && !second->asset_entries_reserved);
    CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
    second->profile_verified=second->prom_verified=second->asset_entries_reserved=1;
    second->release_state=&other_owner_token;
    CHECK(GuIsPreparedPeerHandle(handles[1]) && !GuIsPreparedPeerHandle(handles[0]));
    CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
    CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,&other_owner_token,Gu.asset_entries)==EFI_SUCCESS);
    CHECK(ActiveSequence.active_step==1);
    GuPlatformActive=first;
    CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
    CHECK(GuIsPreparedPeerHandle(handles[0]) && GuIsPreparedPeerHandle(handles[1]));
    GuPlatformActive=second;
    gpu[0].dwords[4]^=0x2000U;
    CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,&other_owner_token,Gu.asset_entries)==EFI_SUCCESS);
    CHECK(ActiveSequence.next_step==2 && GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
    more=1;
    CHECK(GuSelectedSequenceHasNext(&system_table,&more)==EFI_SUCCESS && more==0);
    CHECK(ActiveSequence.targets[ActiveSequence.order[0]].owners.platform==first);
    CHECK(ActiveSequence.targets[ActiveSequence.order[1]].owners.platform==second);
    CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
    if(scenario>=4) {
      if(scenario==4)refuse_completed=1;
      if(scenario==5)refuse_final_graph=1;
      if(scenario==6)gpu[1].dwords[4]^=0x4000U;
      if(scenario==7)drift_after_graph=1;
      CHECK(GuVerifySelectedSequence(&system_table)==EFI_NOT_READY);
      CHECK(ActiveSequence.halted);
      refuse_completed=refuse_final_graph=drift_after_graph=0;
      if(scenario>=6)gpu[1].dwords[4]^=0x4000U;
      CHECK(GuVerifySelectedSequence(&system_table)==EFI_NOT_READY);
      GuPlatformActive=first;free(second);
      continue;
    }
    CHECK(GuVerifySelectedSequence(&system_table)==EFI_SUCCESS);
    gpu[1].dwords[4]^=0x4000U;
    CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
    gpu[1].dwords[4]^=0x4000U;
    CHECK(GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
    Gu.release_state=&release_owner_token;
    CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
    GuPlatformActive=first;
    CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
    free(second); /* simulated boot termination only */
  }
  /* A failed handle-buffer release after a completed step must prevent
   * all later census and selection without changing the saved record. */
  ResetActivePair();
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,&release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,&release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  {
    GU_PLATFORM *saved=GuPlatformActive;
    UINT32 prior=ActiveSequence.targets[ActiveSequence.order[0]].expected_config[4];
    fail_pool_free=1;
    CHECK(GuVerifyActivePeers(&system_table)==EFI_UNSUPPORTED);
    CHECK(UnreleasedPciHandles==handles&&UnreleasedPciHandles==failed_handle_buffer);
    unsigned located=locate_calls,released=free_calls;
    fail_pool_free=0;
    CHECK(GuIsPreparedPeerHandle(handles[0])&&GuIsPreparedPeerHandle(handles[1]));
    CHECK(GuVerifySelectedSequence(&system_table)==EFI_NOT_READY&&ActiveSequence.halted);
    CHECK(GuSelectNextGpu(&system_table)==EFI_NOT_READY);
    CHECK(GuPlatformActive==saved&&ActiveSequence.next_step==1);
    CHECK(ActiveSequence.targets[ActiveSequence.order[0]].phase==GU_GPU_EXEC_COMPLETE);
    CHECK(ActiveSequence.targets[ActiveSequence.order[0]].expected_config[4]==prior);
    CHECK(locate_calls==located&&free_calls==released);
  }
  /* The real selected adapter observes every NVIDIA GPU, not just sixteen. 
   * One target is selected; repeated peer model IDs are intentional. */
  ResetActivePair();ExpandInventory(20);
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_SUCCESS);
  CHECK(ActiveInventory.inventory.count==20&&ActiveDiscovery.capacity==20&&
        ActiveSequence.count==20&&ActiveSequence.selected_count==1);
  CHECK(ActiveDiscovery.records!=ActiveDiscovery.live&&ActiveSequence.targets==ActiveDiscovery.targets&&ActiveSequence.order==ActiveDiscovery.order);
  CHECK(GuIsPreparedPeerHandle(handles[19])&&GuVerifyActivePeers(&system_table)==EFI_SUCCESS);
  Gu.profile_verified=Gu.prom_verified=1;PrepareMockOwners();
  CHECK(GuBeginSelectedGpuStep(&system_table,&Gu,&release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(GuFinishSelectedGpuStep(&system_table,1,&Gu,&release_owner_token,Gu.asset_entries)==EFI_SUCCESS);
  CHECK(GuVerifySelectedSequence(&system_table)==EFI_SUCCESS);
  gpu[19].dwords[4]^=0x1000U;
  CHECK(GuVerifySelectedSequence(&system_table)==EFI_NOT_READY&&ActiveSequence.halted);
  CHECK(state_allocs==1&&state_frees==0); /* No new pool workspace during checks. */
  ResetActivePair();
  fail_state_free=1;count=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_NOT_READY);
  CHECK(!init_calls&&UnreleasedDiscoveryAllocation&&state_allocs==1&&!state_frees);
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
  CHECK(state_allocs==1);
  ResetActivePair();
  printf("PASS exact single-target active binding and CPU state handover: %u checks\n",checks);
  return 0;
}
#elif defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
int main(void) {
  CHECK(CheckHandleBufferLifetime()==0);
#if GU_GPU_SELECTION_COUNT == 18
  ResetPair();ExpandInventory(20);
  gpu[1].segment=0;gpu[1].device=0;gpu[1].function=0;gpu[2].bus=9;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED);
  CHECK(!init_calls&&prep_markers==1&&prep_refusals==1);
  CHECK(context_allocs==18&&context_frees==18);
  CHECK(state_allocs==20&&state_frees==20);
  CHECK(!PreparedContexts&&!PreparedContextCount);
  for(size_t i=0;i<count;i++)CHECK(gpu[i].full_reads==2);
  printf("PASS 18-target read-only preparation: %u checks; GuInit never called\n",checks);
  return 0;
#endif
  EFI_STATUS status;
  ResetPair();
  status=GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table);
  CHECK(status==EFI_UNSUPPORTED && init_calls==0 && free_calls==2);
  CHECK(prep_markers==1 && prep_refusals==1 && global_refusals==0);
  CHECK(context_allocs==GU_GPU_SELECTION_COUNT && context_frees==context_allocs);
  CHECK(state_allocs==GU_GPU_SELECTION_COUNT+2 && state_frees==state_allocs);
  CHECK(!PreparedContexts && !PreparedContextCount && !Gu.pci && !Gu.bs);
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
  CHECK(prep_markers==0 && prep_refusals==0 && global_refusals==0);
  ResetPair(); gpu[1].read_fail=1; /* Unselected peer fails a read. */
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED && init_calls==0);
  CHECK(prep_markers==0 && prep_refusals==0 && global_refusals==0);
  /* The registry must release earlier contexts after a later allocation fails. */
  ResetPair();state_allocs=state_frees=context_allocs=context_frees=0;
  fail_state_alloc=GU_GPU_SELECTION_COUNT+2;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_OUT_OF_RESOURCES);
  CHECK(init_calls==0 && !PreparedContexts && !PreparedContextCount);
  CHECK(context_allocs==GU_GPU_SELECTION_COUNT-1 && context_frees==context_allocs);
  CHECK(state_frees==state_allocs-1 && !prep_markers && !prep_refusals);
  fail_state_alloc=0;
#if GU_GPU_SELECTION_COUNT == 2
  /* A failed context release retains its reference, not a dangling address. */
  ResetPair();fail_context_free=1;
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_NOT_READY);
  CHECK(!init_calls && PreparedContexts && PreparedContextCount==2);
  size_t retained=0;
  for(size_t i=0;i<PreparedContextCount;i++)
    if(PreparedContexts[i]){
      CHECK(PreparedContexts[i]->pci==&gpu[1].protocol);
      retained++;
    }
  CHECK(retained==1);
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_ALREADY_STARTED);
  fail_context_free=0;
  /* Test-only disposal models boot termination, not production retry. */
  for(size_t i=0;i<PreparedContextCount;i++)free(PreparedContexts[i]);
  free(PreparedContexts);PreparedContexts=NULL;PreparedContextCount=0;
#endif
  ResetPair();ExpandInventory(20);
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED);
  CHECK(!init_calls&&prep_markers==1&&prep_refusals==1);
  CHECK(state_allocs==GU_GPU_SELECTION_COUNT+2&&state_frees==state_allocs);
  CHECK(gpu[19].full_reads==2);
  printf("PASS active selected-entrypoint refusal: %u checks; GuInit never called\n",checks);
  return 0;
}
#else
int main(void) {
  CHECK(CheckHandleBufferLifetime()==0);
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

  ResetPair();ExpandInventory(20);
  CHECK(GuDiscoverSingleGpu((EFI_HANDLE)1,&system_table)==EFI_UNSUPPORTED);
  CHECK(!init_calls&&global_refusals==1&&state_allocs==1&&state_frees==1);

  printf("PASS UEFI discovery adapter: %u checks; mocked protocols only\n", checks);
  return 0;
}
#endif
