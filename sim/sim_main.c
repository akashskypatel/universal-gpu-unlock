/* SPDX-License-Identifier: GPL-2.0-only */
/* Host simulator driver.
 *
 * Stages:
 *   prepare      - real GuBootOpen, GuDiscoverSingleGpu and GuPrepareBoundGpu
 *                  against the modelled card; asserts pre-execution state.
 *   transaction  - additionally enters the real vendor transaction engine.
 *
 * The simulator never claims a hardware result. Every report line is tagged
 * SIM_ so a captured log cannot be mistaken for a hardware run.
 */
#include "sim_world.h"

#include "boot_io.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "msi_release.h"
#include "unlock_execution.h"
#include "profile.h"
#include "vendor/gfw_eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Check(int condition, const char *what) {
  if (condition) return;
  SimFail("%s", what);
  SimReportLine("SIM_CHECK_FAILED=%s", what);
}

static UINT32 *SnapshotNew(void) {
  size_t dwords = SimGpuSnapshotDwords();
  UINT32 *snap = calloc(dwords, sizeof(UINT32));
  return snap;
}

static void ReportIdentity(SIM_GPU *gpu) {
  SimReportLine("SIM_DEVICE BDF=0000:01:00.0 ID=%04x:%04x SUBSYS=%08x CLASS=%06x",
                SimGpuConfigRead(gpu, 0) & 0xffffu,
                SimGpuConfigRead(gpu, 0) >> 16,
                SimGpuConfigRead(gpu, 0x2c), SimGpuConfigRead(gpu, 8) >> 16);
  SimReportLine("SIM_MODEL PROM_BYTES=%u BAR_BYTES=%u STOCK_DRIVER_BOUND=%d",
                (unsigned)SIM_PROM_BYTES, (unsigned)SIM_BAR_BYTES,
                SimGpuStockDriverBound(gpu));
}

int main(int argc, char **argv) {
  const char *stage = argc > 1 ? argv[1] : "prepare";
  const char *esp = argc > 2 ? argv[2] : NULL;
  const char *windows = argc > 3 ? argv[3] : NULL;
  const char *variant = argc > 4 ? argv[4] : "nominal";
  int msi_enabled = strcmp(variant, "msi-enabled") == 0;
  int flr_refused = strcmp(variant, "flr-refused") == 0;

  SimReportLine("SIM_VARIANT=%s", variant);
  if (!esp || !windows) {
    fprintf(stderr, "usage: %s <prepare|transaction> <esp-dir> <windows-dir> "
            "[nominal|msi-enabled|flr-refused]\n", argv[0]);
    return 2;
  }

  SimReportLine("SIM_ENVIRONMENT=HOST_SIMULATOR PROFILE=GU_SIMULATED_ACTUATION "
                "ACTUATION_VERIFIED=0 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0 "
                "RECOVERY_VERIFIED=0");
  if (SimWorldInit(esp, esp, windows) != EFI_SUCCESS) {
    SimReportLine("SIM_INIT=FAILED");
    return 2;
  }
  SIM_GPU *gpu = SimPrimaryGpu();
  if (msi_enabled) SimGpuEnableMsi(gpu, 1);
  if (flr_refused) SimGpuSetFlrSupported(gpu, 0);
  EFI_SYSTEM_TABLE *table = SimSystemTable();
  ReportIdentity(gpu);

  UINT32 *before = SnapshotNew();
  if (!before) { SimReportLine("SIM_SNAPSHOT=FAILED"); return 2; }
  SimGpuSnapshot(gpu, before, SimGpuSnapshotDwords());

  /* Self-check the world before handing it to the image. */
  {
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    VOID *loaded = NULL, *path = NULL, *fs = NULL;
    EFI_STATUS l = table->BootServices->HandleProtocol(SimImageHandle(), &loaded_guid, &loaded);
    EFI_STATUS p = l == EFI_SUCCESS
                       ? table->BootServices->HandleProtocol(
                             ((EFI_LOADED_IMAGE_PROTOCOL *)loaded)->DeviceHandle, &path_guid, &path)
                       : l;
    EFI_STATUS f = l == EFI_SUCCESS
                       ? table->BootServices->HandleProtocol(
                             ((EFI_LOADED_IMAGE_PROTOCOL *)loaded)->DeviceHandle, &fs_guid, &fs)
                       : l;
    SimReportLine("SIM_WORLD loaded=%llx path=%llx fs=%llx", (unsigned long long)l,
                  (unsigned long long)p, (unsigned long long)f);
  }

  SimReportLine("SIM_STAGE=GuBootOpen");
  EFI_STATUS status = GuBootOpen(SimImageHandle(), table);
  SimReportLine("SIM_RESULT=GuBootOpen STATUS=%llx", (unsigned long long)status);
  Check(status == EFI_SUCCESS, "GuBootOpen returned an error");

  SimReportLine("SIM_STAGE=GuDiscoverSingleGpu");
  status = GuDiscoverSingleGpu(SimImageHandle(), table);
  SimReportLine("SIM_RESULT=GuDiscoverSingleGpu STATUS=%llx", (unsigned long long)status);
  if (msi_enabled) {
    /* An enabled MSI vector violates the cold PCI policy: the image must refuse
     * the card without touching it. */
    Check(status != EFI_SUCCESS, "an MSI-enabled card was accepted");
    Check(SimGpuBarWrites(gpu) == 0, "a refused card still saw BAR0 writes");
    Check(SimGpuFlrCount(gpu) == 0, "a refused card was reset");
    SimReportLine("SIM_ABORT=pre-execution REFUSED=1 BAR_WRITES=%zu FLR=%zu STATUS=%llx",
                  SimGpuBarWrites(gpu), SimGpuFlrCount(gpu),
                  (unsigned long long)status);
    SimReportLine("SIM_RESULT=%s CHECKS_FAILED=%d",
                  SimFailures() ? "FAIL" : "PASS", SimFailures());
    int refused = SimFailures();
    SimWorldShutdown();
    free(before);
    if (refused) printf("%s", SimLogText());
    return refused ? 1 : 0;
  }
  Check(status == EFI_SUCCESS, "selected discovery failed");

  GU_PLATFORM *owner = GuPlatformActive;
  if (!owner) {
    SimReportLine("SIM_OWNER=NONE");
    return 1;
  }
  SimReportLine("SIM_OWNER DEVICE_ID=%04x BAR_BASE=%llx BAR_SIZE=%llx "
                "PROFILE_VERIFIED=%u PROM_VERIFIED=%u",
                owner->device_id, (unsigned long long)owner->bar_base,
                (unsigned long long)owner->bar_size,
                (unsigned)owner->profile_verified, (unsigned)owner->prom_verified);

  SimReportLine("SIM_STAGE=GuPrepareBoundGpu");
  const char *prepare_stage = "none";
  status = GuPrepareBoundGpu(owner, &prepare_stage);
  SimReportLine("SIM_RESULT=GuPrepareBoundGpu STATUS=%llx STAGE=%s",
                (unsigned long long)status, prepare_stage);
  Check(status == EFI_SUCCESS, "pre-execution preparation failed");
  Check(owner->profile_verified != 0, "profile was not verified");
  Check(owner->prom_verified != 0, "PROM readout was not accepted");
  Check(SimLogContains("PROM readout SHA256:"), "PROM readout was not logged");
  Check(SimLogContains("SIGNATURE_AUTHENTICATED=0"),
        "PROM readout did not record SIGNATURE_AUTHENTICATED=0");
  Check(SimGpuStockDriverBound(gpu), "stock driver model was released too early");

  UINT32 *after_prepare = SnapshotNew();
  SimGpuSnapshot(gpu, after_prepare, SimGpuSnapshotDwords());
  UINT32 offsets[64], values[64];
  size_t changed = SimGpuDiff(before, after_prepare, offsets, values, 64);
  SimReportLine("SIM_PREEXEC_BAR0_DELTA=%zu BAR_WRITES=%zu PROM_WRITES=%zu",
                changed, SimGpuBarWrites(gpu), SimGpuPromWrites(gpu));
  Check(changed == 0, "preparation wrote to BAR0");
  Check(SimGpuPromWrites(gpu) == 0, "preparation wrote into the PROM window");

  if (strcmp(stage, "transaction") == 0) {
    UINT32 *pre_tx = SnapshotNew();
    if (!pre_tx) { SimReportLine("SIM_SNAPSHOT=FAILED"); return 2; }
    SimGpuSnapshot(gpu, pre_tx, SimGpuSnapshotDwords());
    UINTN flrs_before = SimGpuFlrCount(gpu);
    SimTraceReset();
    int ss0_slot = SimTraceWatch(GU_FEAT_SS0_OFFSET);
    int ss1_slot = SimTraceWatch(GU_FEAT_SS1_OFFSET);

    SimReportLine("SIM_STAGE=GuExecuteUnlock");
    const char *tx_stage = "none";
    status = GuExecuteUnlock(owner, table, SimImageHandle(), &tx_stage);
    SimReportLine("SIM_RESULT=GuExecuteUnlock STATUS=%llx STAGE=%s",
                  (unsigned long long)status, tx_stage);

    long flr_last = SimTraceLastOf(SIM_TRACE_FLR);
    long ss0_at = SimTraceWatchLast(ss0_slot);
    long ss1_at = SimTraceWatchLast(ss1_slot);

    SimReportLine("SIM_TX TRACE=%zu BAR_WRITES=%zu PROM_WRITES=%zu "
                  "CONFIG_WRITES=%zu FLR=%zu FW_STARTS=%zu WPR2_OPENS=%zu",
                  SimTraceCount(),
                  SimTraceCountOf(SIM_TRACE_BAR_WRITE),
                  SimTraceCountOf(SIM_TRACE_PROM_WRITE),
                  SimTraceCountOf(SIM_TRACE_CONFIG_WRITE),
                  SimTraceCountOf(SIM_TRACE_FLR),
                  SimTraceCountOf(SIM_TRACE_FW_START),
                  SimTraceCountOf(SIM_TRACE_WPR2));
    SimReportLine("SIM_TX ORDER FLR=%ld SS0=%ld SS1=%ld SEL_WRITES=%zu:%zu",
                  flr_last, ss0_at, ss1_at,
                  SimTraceWatchWrites(ss0_slot), SimTraceWatchWrites(ss1_slot));
    SimReportLine("SIM_TX STATE ARMED=%u FAULT=%u RETAINED=%u FLRS=%u DMA=%u "
                  "STARTED=%u READY_FOR_WINDOWS=%u WPR2=%08x:%08x",
                  (unsigned)owner->armed, (unsigned)owner->fault,
                  (unsigned)owner->retained, (unsigned)owner->flrs,
                  (unsigned)owner->dma_count, (unsigned)owner->started,
                  (unsigned)GuMsiReadyForWindows(),
                  SimGpuWpr2Low(gpu), SimGpuWpr2High(gpu));

    if (flr_refused) {
      /* A card that drops the reset leaves the permission window open. The
       * image must fail, and the model must show why: no reset happened. */
      Check(status != EFI_SUCCESS, "a card that refuses FLR still reported success");
      Check(SimGpuFlrCount(gpu) == 0, "a refused reset was counted as performed");
      Check(SimTraceCountOf(SIM_TRACE_FLR_REFUSED) == 1,
            "the refused reset was not observed");
      Check(SimGpuWpr2High(gpu) != 0, "the open WPR2 window was silently closed");
      /* The engine may still be reading its buffers, so the image must keep
       * them and the device reference instead of freeing memory. */
      Check(owner->retained != 0, "buffers were freed while firmware may still DMA");
      Check(owner->dma_count > 0, "DMA accounting was cleared after the abort");
      Check(!SimLogContains("Selected preboot transaction verified"),
            "a refused reset was still reported as a verified transaction");
      SimReportLine("SIM_ABORT=function-reset REFUSED=1 WPR2=%08x:%08x "
                    "STAGE=%s STATUS=%llx RETAINED=%u DMA=%u FAULT=%u",
                    SimGpuWpr2Low(gpu), SimGpuWpr2High(gpu), tx_stage,
                    (unsigned long long)status, (unsigned)owner->retained,
                    (unsigned)owner->dma_count, (unsigned)owner->fault);
    } else {
      Check(status == EFI_SUCCESS, "transaction failed");
      Check(SimGpuFlrCount(gpu) == flrs_before + 1,
            "expected exactly one function-level reset");
      Check(flr_last >= 0, "no function-level reset was performed");
      /* The PROM window is read-only in this design: the unlock patches a RAM
       * copy of the ROM and DMAs it to the engines. */
      Check(SimGpuPromWrites(gpu) == 0, "the PROM window was written");
      Check(SimGpuWpr2Opens() == 1, "FWSEC did not open exactly one WPR2 window");
      Check(SimGpuWpr2High(gpu) == 0, "WPR2 remained enabled after the reset");
      Check(ss0_at >= 0 && ss1_at >= 0 && ss0_at < ss1_at,
            "selectors were not written in order");
      Check(ss1_at >= 0 && flr_last >= 0 && ss1_at < flr_last,
            "selectors were not applied before the final reset");
      Check(owner->armed != 0, "owner was not armed after the transaction");
      Check(owner->fault == 0, "owner reported a fault");
      Check(owner->retained == 0, "owner retained firmware buffers");
      Check(owner->dma_count == 0, "DMA buffers were not released");
      Check(owner->flrs == 1, "owner recorded an unexpected reset count");
      Check(GuMsiReadyForWindows() != 0, "handover was not ready after the transaction");
      Check(SimGpuStockDriverBound(gpu) == 0, "stock driver model was never released");
      /* The image's own self-report has to agree with the model's view. */
      Check(SimLogContains("Selected GPU result=0 fault=0 safe=1"),
            "the image did not report a clean selected-GPU result");
      Check(SimLogContains("SELECTED_GPU_STEP=COMPLETE"),
            "the image did not report the selected step as complete");
      Check(SimLogContains("Selected preboot transaction verified"),
            "the image did not verify the preboot transaction");
      Check(SimLogContains("MSI pre-close silicon verification: valid=1"),
            "MSI state was not re-read from the card before handover");
      Check(SimLogContains("restore PCI command: actual=0x2"),
            "the PCI command word was not restored");
    }

    UINT32 *post_tx = SnapshotNew();
    if (post_tx) {
      SimGpuSnapshot(gpu, post_tx, SimGpuSnapshotDwords());
      UINT32 offsets[256], values[256];
      size_t changed = SimGpuDiff(pre_tx, post_tx, offsets, values, 256);
      SimReportLine("SIM_TX BAR0_DELTA=%zu", changed);
      for (size_t i = 0; i < changed && i < 32; i++)
        SimReportLine("SIM_TX BAR0_CHANGED[%zu]=%06x:%08x", i, offsets[i], values[i]);
      free(post_tx);
    }
    free(pre_tx);
  }

  SimReportLine("SIM_CAPTURED_LOG_BYTES=%zu", SimLogLength());
  SimReportLine("SIM_RESULT=%s CHECKS_FAILED=%d",
                SimFailures() ? "FAIL" : "PASS", SimFailures());
  int failures = SimFailures();
  SimWorldShutdown();
  free(before);
  free(after_prepare);
  if (failures) printf("%s", SimLogText());
  return failures ? 1 : 0;
}
