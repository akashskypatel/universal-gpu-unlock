/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <stddef.h>

/* Hardware PCI locations and BAR addresses are discovered at runtime. */

#include "gpu_device_ids.h"
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
/* Selected active artifacts are restricted to inventory/plan preparation.
 * The legacy engine is linked for compile coverage, but GPU I/O write,
 * DMA and FLR helpers also refuse in this configuration. */
#if defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY) || defined(GU_SYNTHETIC_COMPILE_ONLY)
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
  BOOLEAN compat_device_open;
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
 * The caller must own the reference exclusively. Clear it only on success.
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
#ifdef GU_ENTRY_LATCHED
/* Only ColdCheck may latch, after all exact-card/cold/PROM checks. These
   values are private to port.c; the vendor core can only read/compare them. */
BOOLEAN GuLatchEntrySelectors(UINT32 Ss0, UINT32 Ss1);
BOOLEAN GuGetEntrySelectors(UINT32 *Ss0, UINT32 *Ss1);
BOOLEAN GuEntrySelectorsMatch(UINT32 Ss0, UINT32 Ss1);
#endif
#ifdef GU_ENTRY_LATCHED_SS1_FULL
/* The full policy owns the only permitted SS1 write phase. */
BOOLEAN GuSelectorPhase(UINT32 Phase);
#endif
BOOLEAN GuTargetConfig(const UINT32 *Config, UINTN Segment, UINTN Bus, UINTN Device, UINTN Function);
BOOLEAN GuSafeToReturn(VOID);
EFI_STATUS GuDmaAllocate(UINTN Bytes, VOID **Cpu, UINT64 *Dma);
EFI_STATUS GuDmaFree(VOID *Cpu, UINT64 Dma);
UINT32 GuRead32(UINT32 Offset);
VOID GuWrite32(UINT32 Offset, UINT32 Value);
EFI_STATUS GuSavePci(VOID);
EFI_STATUS GuRestorePci(VOID);
EFI_STATUS GuFlr(VOID);
EFI_STATUS GuClearMaster(VOID);
EFI_STATUS GuEnableMaster(VOID);
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value);
EFI_STATUS GuConfigWrite(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value);
VOID GuStarted(VOID);
VOID GuQuiesced(VOID);
VOID GuLog(const char *Format, ...);
VOID GuLogText(const char *Text);
VOID GuCopyFromIo(VOID *Destination, const VOID *Source, size_t Bytes);
VOID *GuAlloc(size_t Bytes);
VOID GuFree(VOID *Memory);
VOID GuStall(UINTN Microseconds);
