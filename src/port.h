/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <stddef.h>

/* Hardware PCI locations and BAR addresses are discovered at runtime. */

#include "gpu_device_ids.h"
#include "gpu_inventory.h"
/* Share the parent device registry across EFI and the vendor core. */
#define GU_PORT_MATCH(id) (dev == (UINT16)(id)) ||
static inline BOOLEAN GuIsAdaDeviceId(UINT16 dev) {
  return (GU_ADA_DEVICE_IDS(GU_PORT_MATCH) FALSE);
}
static inline BOOLEAN GuIsAmpereDeviceId(UINT16 dev) {
  return (GU_AMPERE_DEVICE_IDS(GU_PORT_MATCH) FALSE);
}
static inline BOOLEAN GuIsTuringDeviceId(UINT16 dev) {
  return (GU_TURING_DEVICE_IDS(GU_PORT_MATCH) FALSE);
}
#undef GU_PORT_MATCH
static inline BOOLEAN GuIsSupportedDeviceId(UINT16 dev) {
  return GuIsAdaDeviceId(dev) || GuIsAmpereDeviceId(dev) ||
         GuIsTuringDeviceId(dev);
}
static inline BOOLEAN GuIsAdaPciId(UINT32 id) {
  if ((id & 0xffffU) != 0x10deU) return FALSE;
  return GuIsAdaDeviceId((UINT16)(id >> 16));
}

static inline BOOLEAN GuIsAmperePciId(UINT32 id) {
  if ((id & 0xffffU) != 0x10deU) return FALSE;
  return GuIsAmpereDeviceId((UINT16)(id >> 16));
}

static inline BOOLEAN GuIsTuringPciId(UINT32 id) {
  if ((id & 0xffffU) != 0x10deU) return FALSE;
  return GuIsTuringDeviceId((UINT16)(id >> 16));
}

static inline BOOLEAN GuIsSupportedPciId(UINT32 id) {
  if ((id & 0xffffU) != 0x10deU) return FALSE;
  return GuIsSupportedDeviceId((UINT16)(id >> 16));
}
/* The default artifact is read-only. Legacy host containment tests are never
   a production enable switch; experimental firmware requires its own define. */
#if defined(GU_AD104_HOST_TEST) && __STDC_HOSTED__ != 1
#error GU_AD104_HOST_TEST is forbidden in firmware builds
#endif
#if defined(GU_STOCK_PROFILE_SS0) || defined(GU_STOCK_PROFILE_NAME)
#error Stock profile value and name must be derived from the explicit selector
#endif
#if (defined(GU_STOCK_PROFILE_ORIGINAL_03100062) + defined(GU_STOCK_PROFILE_OBSERVED_03000062) + defined(GU_ENTRY_LATCHED_SS1_PRESERVE) + defined(GU_ENTRY_LATCHED_SS1_FULL)) > 1
#error Select exactly one stock profile
#endif
#if defined(GU_AD104_TRANSACTION) && defined(GU_STOCK_PROFILE_ORIGINAL_03100062)
#define GU_STOCK_PROFILE_SS0 0x03100062U
#define GU_STOCK_PROFILE_NAME "original03100062"
#elif defined(GU_AD104_TRANSACTION) && defined(GU_STOCK_PROFILE_OBSERVED_03000062)
#define GU_STOCK_PROFILE_SS0 0x03000062U
#define GU_STOCK_PROFILE_NAME "observed03000062"
#elif defined(GU_AD104_TRANSACTION) && defined(GU_ENTRY_LATCHED_SS1_PRESERVE)
#define GU_STOCK_PROFILE_NAME "entryLatchedSs1Preserve"
#elif defined(GU_AD104_TRANSACTION) && defined(GU_ENTRY_LATCHED_SS1_FULL)
#define GU_STOCK_PROFILE_NAME "entryLatchedSs1Full"
#elif defined(GU_AD104_TRANSACTION)
#error Experimental firmware requires exactly one explicit stock profile
#elif defined(GU_STOCK_PROFILE_ORIGINAL_03100062) || defined(GU_STOCK_PROFILE_OBSERVED_03000062) || defined(GU_ENTRY_LATCHED_SS1_PRESERVE) || defined(GU_ENTRY_LATCHED_SS1_FULL)
#error Stock profile selection requires experimental firmware
#endif
/* GU_ACTIVE_UNVERIFIED_ACTUATION is the single, non-default acceptance that
 * an image may perform the unverified, irreversible GPU firmware transaction.
 * It is a deliberate operator waiver, not a verification result: vendor Stop
 * bounds, signed Windows 617.42 Booter acceptance, DMA quiescence and physical
 * recovery all remain unproven (see docs/MULTI_GPU_REFACTOR_REVIEW.md).
 *
 * The waiver is only meaningful in a deployable transaction build, so it is
 * rejected in combination with every preparation-only or synthetic profile
 * and requires both the experimental firmware transaction profile and the
 * ordered selected-target mode. GU_ACTIVE_SINGLE_TARGET selects that ordered
 * path: one card is active at a time, and the ordered list may name several
 * targets that are executed sequentially. */
#ifdef GU_ACTIVE_UNVERIFIED_ACTUATION
#if !defined(GU_AD104_TRANSACTION)
#error Unverified actuation requires the experimental firmware transaction profile
#endif
#if !defined(GU_ACTIVE_SINGLE_TARGET)
#error Unverified actuation requires the ordered selected-target execution mode
#endif
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_SYNTHETIC_COMPILE_ONLY)
#error Unverified actuation cannot be combined with preparation-only or synthetic builds
#endif
#if defined(GU_HOST_TEST_PROFILE)
#error Unverified actuation requires the reviewed local boot profile, not the synthetic ESP GUID
#endif
#endif
#if defined(GU_UNVERIFIED_BENCH_CONDITIONS)
#if !defined(GU_ACTIVE_UNVERIFIED_ACTUATION) || !defined(GU_ACTIVE_SINGLE_TARGET)
#error The unverified ASPM bench-condition override requires an explicit ordered actuation build
#endif
#if !defined(GU_DXE_FILE_ONLY_OUTPUT)
#error The unverified bench image must preserve its log while suppressing routine screen output
#endif
#endif
#if defined(GU_STOP_RELEASE_ONLY_BENCH)
#if !defined(GU_UNVERIFIED_BENCH_CONDITIONS) || !defined(GU_DXE_FILE_ONLY_OUTPUT)
#error Stop/release-only bench mode requires explicit ASPM admission waiver and file-only logging
#endif
#if !defined(GU_ACTIVE_UNVERIFIED_ACTUATION) || !defined(GU_ACTIVE_SINGLE_TARGET)
#error Stop/release-only mode requires explicit ordered stop/release consent
#endif
#endif

/* GU_RUNTIME_SELECTION moves the target list out of the build: the image reads
 * the operator's "GuTargets" variable at boot instead of a generated
 * gpu-selection.h. Because the count is then unknown at build time, the static
 * "exactly one address" gate cannot apply; the runtime admission gate does,
 * unchanged, so a multi-card request without a waiver still refuses to actuate.
 * A synthetic compile-only profile has no operator policy to read, so the
 * combination is rejected. */
#if defined(GU_RUNTIME_SELECTION)
#if !defined(GU_ACTIVE_SINGLE_TARGET)
#error Runtime target selection requires the ordered selected-target execution mode
#endif
#if defined(GU_SYNTHETIC_COMPILE_ONLY) || defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
#error Runtime target selection requires a real execution profile
#endif
#endif

/* GU_SIMULATED_ACTUATION is the hosted simulator profile. It exists so the
 * real active transaction sources can be executed against a modelled PCI
 * device on a workstation, for pre-execution and post-execution inspection.
 *
 * It is structurally impossible to ship: it is rejected in any build that is
 * not hosted, and rejected next to the GU_UEFI_PORT marker that both firmware
 * builders emit. A simulator build also never sets GU_HOST_TEST_PROFILE,
 * because the simulated card must satisfy the same config policy, PROM and
 * FWSEC checks as hardware.
 *
 * The simulator is evidence about code behaviour only. It is not evidence
 * about silicon, and its logs are marked SIMULATED for that reason. */
#ifdef GU_SIMULATED_ACTUATION
#if __STDC_HOSTED__ != 1
#error Simulated actuation is a hosted-only build and cannot be part of a firmware image
#endif
#if defined(GU_UEFI_PORT)
#error Simulated actuation must not use the UEFI port marker emitted by the firmware builders
#endif
#if !defined(GU_AD104_TRANSACTION)
#error Simulated actuation requires the experimental firmware transaction profile
#endif
#if !defined(GU_ACTIVE_SINGLE_TARGET)
#error Simulated actuation requires the ordered selected-target execution mode
#endif
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_SYNTHETIC_COMPILE_ONLY)
#error Simulated actuation cannot be combined with preparation-only or synthetic builds
#endif
#if defined(GU_ACTIVE_UNVERIFIED_ACTUATION)
#error Simulated actuation and the hardware waiver are distinct profiles
#endif
#endif

/* One admission switch, two provenances: the hardware waiver and the
 * simulator. Both relax the same admission checks; only the hardware waiver
 * can reach silicon, and only the simulator can be inspected afterwards. */
#if defined(GU_ACTIVE_UNVERIFIED_ACTUATION)
#define GU_ACTUATION_ADMITTED 1
#define GU_ACTUATION_SIMULATED 0
#elif defined(GU_SIMULATED_ACTUATION)
#define GU_ACTUATION_ADMITTED 1
#define GU_ACTUATION_SIMULATED 1
#else
#define GU_ACTUATION_ADMITTED 0
#define GU_ACTUATION_SIMULATED 0
#endif

/* Selected active artifacts are restricted to inventory/plan preparation.
 * The legacy engine is linked for compile coverage, but GPU I/O write,
 * DMA and FLR helpers also refuse in this configuration. */
#if GU_ACTUATION_ADMITTED
#define GU_PREFLIGHT_ONLY 0
#elif defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_SYNTHETIC_COMPILE_ONLY)
#define GU_PREFLIGHT_ONLY 1
#elif defined(GU_AD104_TRANSACTION) || (defined(GU_AD104_HOST_TEST) && __STDC_HOSTED__ == 1)
#define GU_PREFLIGHT_ONLY 0
#else
#define GU_PREFLIGHT_ONLY 1
#endif
#if defined(GU_ENTRY_LATCHED_SS1_PRESERVE) || defined(GU_ENTRY_LATCHED_SS1_FULL)
#define GU_ENTRY_LATCHED 1
#endif
#define GU_SELECTOR_PHASE_ENTRY 0U
#define GU_SELECTOR_PHASE_SS0_WRITTEN 1U
#define GU_SELECTOR_PHASE_SS1_WRITE 2U
#define GU_SELECTOR_PHASE_TARGET 3U
/* The manifest contains a fixed number of files per GPU. Each platform
 * owns its own table, including before asset reservation. */
/* BAR0 window rules live in gpu_bar.h so the host fixtures compile the same
 * definitions the firmware does. */
#include "gpu_bar.h"
#define GU_ASSET_COUNT 5U
struct firmware {size_t size;const UINT8 *data;};
typedef struct GU_ASSET {const char *name;struct firmware firmware;} GU_ASSET;
typedef struct GU_DMA GU_DMA;
struct GU_DMA {
  GU_DMA *next;
  VOID *cpu, *mapping;
  UINT64 dma;
  UINTN bytes, pages;
  BOOLEAN mapped;
};
typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  EFI_HANDLE image, handle;
  UINT64 bar_base, bar_size, ticks;
  UINT16 pm_cap, pcie_cap, msi_cap, msix_cap;
  UINT16 device_id;
  /* Retain the exact read-only discovery identity for this physical GPU. */
  GU_GPU_RECORD discovery_identity;
  UINT32 saved_config[64];
  UINT16 saved_devctl, saved_devctl2;
  UINT8 pcie_version;
  BOOLEAN is_ampere;
  BOOLEAN is_turing;
  BOOLEAN fault, retained, started, quiesced, armed, owned, display_ok, saved;
  BOOLEAN profile_verified, prom_verified;
  BOOLEAN transaction_attempted;
  BOOLEAN s3_script_attempted;
  BOOLEAN master_attributes_pending;
  UINT64 master_original_attributes;
  UINT16 master_original_command;
  EFI_PCI_IO_PROTOCOL *master_pci;
  /* Containment may run at TPL_NOTIFY. Capture diagnostics without file IO;
     preserve first errors even if a later containment retry succeeds. */
  EFI_STATUS master_attribute_status, master_hardware_status;
  UINT64 master_last_attributes;
  UINT16 master_last_command;
  UINT32 flrs, mmio_writes, dma_count;
  GU_DMA *allocations;
  /* CPU-only pool allocations are distinct from device-visible DMA buffers.
   * Keep their ownership and first uncertain release with this platform. */
  struct GU_CPU_POOL *cpu_pool;
  /* Retained compatibility identity; never recycle it for another GPU. */
  struct pci_dev *compat_device;
  BOOLEAN compat_device_open,compat_device_closed;
  VOID *cpu_pool_failure_address;
  EFI_STATUS cpu_pool_failure_status;
  /* These resources belong to this platform, not to the active selector. */
  struct GU_MSI_OWNED_STATE *release_state;
  BOOLEAN release_state_reserved, release_owner_invalid;
  GU_ASSET asset_entries[GU_ASSET_COUNT];
  BOOLEAN asset_entries_reserved, asset_load_attempted;
  BOOLEAN (*display_check)(VOID);
#ifdef GU_ENTRY_LATCHED
  /* Per-physical-GPU selector values, never shared with another adapter. */
  BOOLEAN entry_latched;
  UINT32 entry_ss0,entry_ss1;
#endif
#ifdef GU_ENTRY_LATCHED_SS1_FULL
  UINT32 selector_phase;
  BOOLEAN ss1_write_consumed;
#endif
} GU_PLATFORM;
/* All source modules resolve the selected card's concrete platform object.
 * The initial static object is only the unbound state. */
extern GU_PLATFORM *GuPlatformActive;
#define Gu (*GuPlatformActive)

/* One checked bind per image. The registry retains published platforms.
 * Rebinding requires a verified per-card teardown and handover protocol. */
EFI_STATUS GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci);
/* Allocate independent CPU-only state into an empty caller-owned output.
 * Reject a PCI protocol that already has an allocated platform.
 * Does not select a GPU, claim a protocol, or authorize hardware access. */
EFI_STATUS GuAllocatePlatform(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci,
                              GU_PLATFORM **Out);
/* Transfer a registered, pristine allocation to the first active slot.
 * Consumes the caller's exclusive reference only on success. Does not
 * permit a second bind or authorize hardware operations. */
EFI_STATUS GuBindAllocatedPlatform(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci,
                                   GU_PLATFORM **Owner);
/* Release only a registered, never-published, unchanged allocation.
 * The caller must own the reference exclusively. Consume it before the
 * firmware release. An uncertain release permanently blocks new binding.
 * A lost active pointer cannot make a published platform disposable. */
EFI_STATUS GuDiscardUnboundPlatform(GU_PLATFORM **Out);
#ifdef GU_ACTIVE_SINGLE_TARGET
#include "gpu_execution_state.h"
EFI_STATUS GuVerifyCompletedPlatforms(EFI_BOOT_SERVICES *bs,
                                      const GU_GPU_EXEC_SEQUENCE *sequence,
                                      const GU_GPU_EXEC_OBSERVATION *observations,
                                      size_t count);
BOOLEAN GuAnyPlatformNeedsContainment(VOID);
/* Retain the verified completed context and publish a distinct registered
 * next context. This changes only the CPU binding, not hardware ownership.
 * The existing one-target admission in the boot entrypoints is unchanged. */
EFI_STATUS GuAdvanceAllocatedPlatform(EFI_BOOT_SERVICES *bs,GU_PLATFORM **owner,
                                      const GU_GPU_EXEC_SEQUENCE *sequence,
                                      const GU_GPU_EXEC_OBSERVATION *observations,
                                      size_t count);
#endif
/* Only ColdCheck may latch, after all exact-card/cold/PROM checks. These
   values are private to port.c; the vendor core can only read/compare them.
   Every profile declares this API: the non-latched policies keep no private
   copy, so their latch is a no-op and their match has no expectation. Declaring
   it unconditionally is what lets those builds link at all - before this, three
   of the four policies failed to link because the call sites were unconditional
   and the definitions were not. */
BOOLEAN GuLatchEntrySelectorsFor(GU_PLATFORM *platform,UINT32 Ss0,UINT32 Ss1);
BOOLEAN GuGetEntrySelectorsFor(const GU_PLATFORM *platform,UINT32 *Ss0,UINT32 *Ss1);
BOOLEAN GuEntrySelectorsMatchFor(const GU_PLATFORM *platform,UINT32 Ss0,UINT32 Ss1);
BOOLEAN GuLatchEntrySelectors(UINT32 Ss0, UINT32 Ss1);
BOOLEAN GuGetEntrySelectors(UINT32 *Ss0, UINT32 *Ss1);
BOOLEAN GuEntrySelectorsMatch(UINT32 Ss0, UINT32 Ss1);
#ifdef GU_ENTRY_LATCHED_SS1_FULL
/* The full policy owns the only permitted SS1 write phase. */
BOOLEAN GuSelectorPhaseFor(GU_PLATFORM *platform,UINT32 Phase);
#endif
BOOLEAN GuTargetConfig(const UINT32 *Config, UINTN Segment, UINTN Bus, UINTN Device, UINTN Function);
BOOLEAN GuSafeToReturnFor(const GU_PLATFORM *platform);
BOOLEAN GuSafeToReturn(VOID);
EFI_STATUS GuDmaAllocate(GU_PLATFORM *owner,UINTN Bytes,VOID **Cpu,UINT64 *Dma);
EFI_STATUS GuDmaFree(GU_PLATFORM *owner,VOID *Cpu,UINT64 Dma);
UINT32 GuRead32For(GU_PLATFORM *platform,UINT32 Offset);
UINT32 GuRead32(UINT32 Offset);
VOID GuWrite32(GU_PLATFORM *owner,UINT32 Offset,UINT32 Value);
EFI_STATUS GuSavePci(GU_PLATFORM *owner);
EFI_STATUS GuRestorePci(GU_PLATFORM *owner);
EFI_STATUS GuFlr(GU_PLATFORM *owner);
EFI_STATUS GuClearMaster(GU_PLATFORM *owner);
EFI_STATUS GuEnableMaster(GU_PLATFORM *owner);
EFI_STATUS GuConfigReadFor(GU_PLATFORM *platform,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT32 Offset,VOID *Value);
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value);
EFI_STATUS GuConfigWrite(GU_PLATFORM *owner,EFI_PCI_IO_PROTOCOL_WIDTH Width,UINT32 Offset,VOID *Value);
VOID GuStartedFor(GU_PLATFORM *platform);
VOID GuQuiescedFor(GU_PLATFORM *platform);
VOID GuStarted(VOID);
VOID GuQuiesced(VOID);
VOID GuLog(const char *Format, ...);
VOID GuLogText(const char *Text);
/* Verbose actuation trace.
 *
 * A phase that can block must say so before it blocks, in the USB log *and* on
 * the console. RUN0011 is the reason: the waived boot wrote thirteen lines, went
 * black, and stayed black for over a minute inside a call that logged nothing
 * until it returned. From the outside that is indistinguishable from a fault, and
 * from the log alone it cannot even be told which call it was. Each line here is
 * flushed by GuLog and mirrored to the console even while the image is otherwise
 * silent, so a stall names its own last phase. */
VOID GuTraceReset(VOID);
VOID GuTracePhase(const char *Phase);
VOID GuTraceDetail(const char *Phase, const char *Format, ...);
VOID GuTraceProgress(const char *Phase, UINTN Done, UINTN Total);
VOID GuTraceDelay(UINT64 Ticks);
/* Console lines that must be visible even while the image is silent. */
VOID GuTraceScreen(const char *Text);
VOID GuTraceStop(const char *Stage, UINT64 Status);
BOOLEAN GuCopyFromIo(GU_PLATFORM *owner,VOID *Destination,const VOID *Source,size_t Bytes);
/* Read-only ownership proof for retained CPU objects, including inactive platforms. */
BOOLEAN GuCpuPoolOwns(const GU_PLATFORM *platform,const VOID *memory,size_t bytes);
/* Resolve a retained CPU object to one registered platform without selecting it. */
GU_PLATFORM *GuPlatformForCpuObject(const VOID *memory,size_t bytes);
/* Record an uncertain CPU pool result without dereferencing its address. */
VOID GuRecordCpuPoolFailure(EFI_STATUS status,VOID *address);
/* Temporary transaction memory uses the open compatibility record as its tag.
 * These CPU-only helpers do not select or claim a GPU. */
BOOLEAN GuCpuPoolCompatEmpty(const GU_PLATFORM *platform);
VOID *GuCpuPoolAllocScoped(const struct pci_dev *scope,size_t bytes);
BOOLEAN GuCpuPoolFreeScoped(const struct pci_dev *scope,VOID *memory);
BOOLEAN GuCompatCompletedPlatform(const GU_PLATFORM *platform);
/* General CPU memory for an explicitly identified active platform. */
VOID *GuAllocFor(GU_PLATFORM *owner,size_t Bytes);
BOOLEAN GuFreeFor(GU_PLATFORM *owner,VOID *Memory);
VOID *GuAlloc(size_t Bytes);
VOID GuFree(VOID *Memory);
VOID GuStall(GU_PLATFORM *owner,UINTN Microseconds);
