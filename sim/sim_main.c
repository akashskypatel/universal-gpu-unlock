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
  if (!esp || !windows) {
    fprintf(stderr, "usage: %s <prepare|transaction> <esp-dir> <windows-dir>\n", argv[0]);
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
    UINTN prom_before = SimGpuPromWrites(gpu);
    SimTraceReset();

    {
      EFI_BOOT_SERVICES *b = table->BootServices;
      SimReportLine("SIM_PROBE_BS dc=%d op=%d cp=%d rt=%d st=%d pph=%d opi=%d lhb=%d hp=%d ap=%d fp=%d",
                    b->DisconnectController != NULL, b->OpenProtocol != NULL,
                    b->CloseProtocol != NULL, b->RaiseTPL != NULL, b->RestoreTPL != NULL,
                    b->ProtocolsPerHandle != NULL, b->OpenProtocolInformation != NULL,
                    b->LocateHandleBuffer != NULL, b->HandleProtocol != NULL,
                    b->AllocatePool != NULL, b->FreePool != NULL);
      SimReportLine("SIM_PROBE_CON con=%d in=%d err=%d keys=%d rk=%d rs=%d wk=%d",
                    table->ConsoleOutHandle != NULL, table->ConsoleInHandle != NULL,
                    table->StandardErrorHandle != NULL, table->ConIn != NULL,
                    table->ConIn && table->ConIn->ReadKeyStroke != NULL,
                    table->ConIn && table->ConIn->Reset != NULL,
                    table->ConIn && table->ConIn->WaitForKey != NULL);
    }
    SimReportLine("SIM_PROBE peers=%llx cold_entry=%u started=%u owned=%u armed=%u",
                  (unsigned long long)GuVerifyActivePeers(table),
                  (unsigned)GuAd104ColdEntry(), (unsigned)owner->started,
                  (unsigned)owner->owned, (unsigned)owner->armed);
    SimReportLine("SIM_STAGE=GuExecuteUnlock");
    const char *tx_stage = "none";
    status = GuExecuteUnlock(owner, table, SimImageHandle(), &tx_stage);
    SimReportLine("SIM_RESULT=GuExecuteUnlock STATUS=%llx STAGE=%s",
                  (unsigned long long)status, tx_stage);

    long flr_first = SimTraceFirstOf(SIM_TRACE_FLR);
    long flr_last = SimTraceLastOf(SIM_TRACE_FLR);
    long prom_last = SimTraceLastOf(SIM_TRACE_PROM_WRITE);
    long sel0 = -1, sel1 = -1;
    for (size_t i = 0; i < SimTraceCount(); i++) {
      if (SimTraceKind((size_t)i) != SIM_TRACE_BAR_WRITE) continue;
      if (SimTraceOffset((size_t)i) == GU_FEAT_SS0_OFFSET) sel0 = (long)i;
      if (SimTraceOffset((size_t)i) == GU_FEAT_SS1_OFFSET) sel1 = (long)i;
    }
    SimReportLine("SIM_TX TRACE=%zu OVERFLOW=%d BAR_WRITES=%zu PROM_WRITES=%zu "
                  "CONFIG_WRITES=%zu FLR=%zu",
                  SimTraceCount(), SimTraceOverflowed(),
                  SimTraceCountKind(SIM_TRACE_BAR_WRITE),
                  SimTraceCountKind(SIM_TRACE_PROM_WRITE),
                  SimTraceCountKind(SIM_TRACE_CONFIG_WRITE),
                  SimTraceCountKind(SIM_TRACE_FLR));
    SimReportLine("SIM_TX ORDER FLR_FIRST=%ld FLR_LAST=%ld PROM_LAST=%ld "
                  "SS0_AT=%ld SS1_AT=%ld",
                  flr_first, flr_last, prom_last, sel0, sel1);
    SimReportLine("SIM_TX STATE ARMED=%u FAULT=%u RETAINED=%u FLRS=%u DMA=%u "
                  "STARTED=%u READY_FOR_WINDOWS=%u",
                  (unsigned)owner->armed, (unsigned)owner->fault,
                  (unsigned)owner->retained, (unsigned)owner->flrs,
                  (unsigned)owner->dma_count, (unsigned)owner->started,
                  (unsigned)GuMsiReadyForWindows());

    Check(status == EFI_SUCCESS, "transaction failed");
    Check(!SimTraceOverflowed(), "operation trace overflowed");
    Check(SimGpuFlrCount(gpu) == flrs_before + 1, "expected exactly one function-level reset");
    Check(SimGpuPromWrites(gpu) > prom_before, "transaction never wrote the PROM window");
    Check(flr_last >= 0, "no function-level reset was performed");
    Check(prom_last >= 0 && flr_last > prom_last,
          "PROM writes did not precede the final reset");
    Check(sel0 >= 0 && sel1 >= 0 && sel0 < sel1, "selectors were not written in order");
    Check(sel1 < flr_last || flr_last < 0,
          "selectors were not applied before the final reset");
    Check(owner->armed != 0, "owner was not armed after the transaction");
    Check(owner->fault == 0, "owner reported a fault");
    Check(owner->retained == 0, "owner retained firmware buffers");
    Check(GuMsiReadyForWindows() != 0, "handover was not ready after the transaction");

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
