/* SPDX-License-Identifier: GPL-2.0-only */
/* AdaUnlockDxe.c - Universal UEFI DXE Driver with S3 Boot Script Persistence */
#include "port.h"
#include "checks.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "msi_release.h"
#include "unlock_execution.h"
#include "boot_io.h"
#include "compat/compat.h"
#include "s3_save.h"
#include "boot_state.h"
#include "driver_nvram.h"
#ifndef GU_STOP_RELEASE_ONLY_BENCH
#include "vendor/unlock_610.h"
#endif

/* UEFI ReadyToBoot event group: 7ce88fb3-4bd7-4679-87a8-a8d8dee50d2b.
 * Run when console and PCI bindings are initialized, instead of assuming
 * that Driver#### entry loads after console ownership is stable.
 * Event registration does not grant active GPU mutation permission. */
static const EFI_GUID ReadyToBootGroup =
  {0x7ce88fb3,0x4bd7,0x4679,{0x87,0xa8,0xa8,0xd8,0xde,0xe5,0x0d,0x2b}};
static EFI_HANDLE DxeImage;
static EFI_SYSTEM_TABLE *DxeTable;
static EFI_EVENT DxeEvent;
static BOOLEAN DxeExecuted;
static EFI_STATUS DxeEventCloseStatus;



static EFI_STATUS GuDxeRun(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  if (!image || !t || !t->BootServices) return EFI_INVALID_PARAMETER;

  EFI_STATUS s = GuBootOpen(image, t);
  if (s != EFI_SUCCESS) return s;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
#ifdef GU_RUNTIME_SELECTION
  GuLog("STOP_RELEASE_ONLY_IMAGE=1 ORDER=LIVE_SUPPORTED_NVIDIA_ENUMERATION ");
#else
  GuLog("STOP_RELEASE_ONLY_IMAGE=1 ORDER=COMPILED_BDF_SEQUENCE ");
#endif
  GuLog("ASPM_ADMISSION=BYPASSED INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED=CONDITIONAL "
        "FIRMWARE_TRANSACTION=0 UNLOCK=0 FIRMWARE_ASSET_FILES_READ=0 "
        "PROM_READ=0 OS_HANDOFF=NONE SCREEN_OUTPUT=SUPPRESSED "
        "LOG_FILE=IMAGE_VOLUME HARDWARE_VERIFIED=0\n");
#endif

  /* Read again after GuBootOpen. The earlier log observation is not admission. */
  GU_BOOT_STATE boot_state;BOOLEAN boot_state_present=FALSE;
  s=GuBootReadPriorState(t,&boot_state,&boot_state_present);
  if(s!=EFI_SUCCESS) {
    GuLog("DXE_RECOVERY=UNREADABLE STATUS=%#llx ACTION=SKIP_GPU\n",(unsigned long long)s);
    EFI_STATUS closed=GuBootClose();
    return closed==EFI_SUCCESS?s:closed;
  }

#if !defined(GU_STOP_RELEASE_ONLY_BENCH) && !defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
  if(GuBootStateRequiresRecovery(boot_state_present?&boot_state:NULL)) {
    EFI_STATUS disabled = GuDisableDriverOption(t);
    GuLog("DXE_RECOVERY=FAILED_OR_INTERRUPTED DRIVER_ORDER_DISABLE=%#llx ACTION=SKIP_GPU\n",
          (unsigned long long)disabled);
    GuBootSay(L"\r\n[RECOVERY] An earlier DXE attempt failed or awaits owner acceptance.\r\n"
              L"The GPU transaction is disabled for this boot; continuing normally.\r\n");
    EFI_STATUS closed = GuBootClose();
    return closed == EFI_SUCCESS ? EFI_SUCCESS : closed;
  }
#endif

  const char *stage = "dxe-discover";
#if (defined(GU_STOP_RELEASE_ONLY_BENCH) && defined(GU_RUNTIME_SELECTION)) || defined(GU_SELECT_ALL_GPUS)
  s=GuRuntimeSelectionSelectAll();
  if(s!=EFI_SUCCESS)goto finish;
#endif
  s = GuDiscoverSingleGpu(image, t);
  if (s != EFI_SUCCESS) goto finish;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  GuLog("ACTIVE_DXE=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1\n");
  s = EFI_UNSUPPORTED; goto finish;
#endif

  /* Pin the discovered owner through preparation and transaction entry. */
  GU_PLATFORM *owner=GuPlatformActive;
  if(!owner){s=EFI_NOT_READY;goto finish;}
  if (owner->is_ampere) {
    GuLog("DXE-DRIVER: Ampere GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);
  } else if (owner->is_turing) {
    GuLog("DXE-DRIVER: Turing GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);
  } else {
    GuLog("DXE-DRIVER: Ada Lovelace GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);
  }

  s=GuPrepareBoundGpu(owner,&stage);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto finish;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  s=GuExecuteStopReleaseSequence(owner,t,image,&stage);
#else
  /* Name the card before the first mutation so a power loss leaves evidence. */
#ifdef GU_DXE_FIRMWARE_REATTACH
  GuLog("DXE_FIRMWARE_REATTACH=1 S3_POLICY=OPTIONAL OS_HANDOFF=FIRMWARE ASPM_ADMISSION_BYPASSED=0\n");
  GuBootSay(L"\r\n[OK] GPU preparation complete. A separate warning will appear before each GPU driver release.\r\n");
  GuBootSilence();
  GuTracePhase("record-boot-intent");
#endif
  s=GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_INTENT,0,owner->device_id,
                          (UINT16)owner->discovery_identity.segment,
                          (UINT16)owner->discovery_identity.bus,
                          (UINT8)owner->discovery_identity.device,
                          (UINT8)owner->discovery_identity.function,0);
  if(s!=EFI_SUCCESS||!GuBootLogOpen()) {
    if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    goto finish;
  }
  s=GuExecutePreparedSequence(owner,t,image,GU_S3_OPTIONAL,&stage);
  s=GuCompletePreparedSequence(s,&owner);
  if(!owner)goto finish;
#ifdef GU_DXE_FIRMWARE_REATTACH
  if(s==EFI_SUCCESS&&(!GuExecutionReadyForWindows(t)||!GuBootLogOpen()))s=EFI_NOT_READY;
#endif
#endif
  if(s!=EFI_SUCCESS||Gu.fault)goto finish;

finish:
  if (s == EFI_SUCCESS && Gu.fault) s = EFI_DEVICE_ERROR;
  if (s == EFI_SUCCESS) {
    stage="dxe-close-USB-log";
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    GuLog("DXE-DRIVER: Stop/release and reattach sequence verified; closing boot resources.\n");
    s=GuBootBenchReportCompletion();
    if(s!=EFI_SUCCESS)
      GuLog("BENCH_COMPLETION_DISPLAY=FAILED STATUS=%#llx GPU_SEQUENCE=COMPLETE\n",
            (unsigned long long)s);
    s=EFI_SUCCESS;
#else
    GuLog("DXE-DRIVER: final transaction checks passed; closing boot resources.\n");
#ifdef GU_DXE_FIRMWARE_REATTACH
    GuLog("PREBOOT OK: final environment checks passed; firmware controls OS startup; S3 resume and Windows CUDA remain unverified\n");
#endif
#ifdef GU_ACTIVE_UNVERIFIED_ACTUATION
    GuLog("EXPERIMENT_RESTART_BARRIER=RETAINED PHASE=INTENT OWNER_ACCEPTANCE=PENDING AUTO_RETRY=0\n");
#endif
#endif
    s=GuBootClose();
#ifdef GU_DXE_FIRMWARE_REATTACH
    if(s==EFI_SUCCESS&&Gu.fault)s=EFI_DEVICE_ERROR;
#endif
#if !defined(GU_STOP_RELEASE_ONLY_BENCH) && !defined(GU_ACTIVE_UNVERIFIED_ACTUATION)
    if(s==EFI_SUCCESS) {
      GU_PLATFORM *completed=GuPlatformActive;
      if(!completed||completed->fault)s=EFI_NOT_READY;
      else {
        EFI_STATUS recorded=GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_OUTCOME,EFI_SUCCESS,completed->device_id,
                          (UINT16)completed->discovery_identity.segment,
                          (UINT16)completed->discovery_identity.bus,
                          (UINT8)completed->discovery_identity.device,
                          (UINT8)completed->discovery_identity.function,
                          (UINT8)(completed->armed?1U:0U));
        if(s==EFI_SUCCESS&&recorded!=EFI_SUCCESS)s=recorded;
      }
    }
#endif
    if(s==EFI_SUCCESS)return s;
  }
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: STOP stage=%s status=%#llx mutated=%u retained=%u started=%u fault=%u\n",
          stage, (unsigned long long)s, GuMsiMutated(), Gu.retained, Gu.started, Gu.fault);
    if (GuExecutionNeedsContainment()) {
#ifdef GU_DXE_FIRMWARE_REATTACH
      GuBootSilence();
#endif
      if ((Gu.started && Gu.owned) || Gu.master_attributes_pending) (VOID)GuClearMaster(&Gu);
#ifdef GU_DXE_FIRMWARE_REATTACH
      EFI_STATUS cancel_status=GuBootWatchdogCancel();
      GuLog("FIRMWARE_REATTACH_CONTAINMENT=HALT NEXT_GPU=BLOCKED AUTO_WARM_RESET=0 WATCHDOG_CANCEL=%#llx\n",(unsigned long long)cancel_status);
      GuTraceStop(stage,(UINT64)s);
      (VOID)GuBootClose();
      GuBootHalt();
      return EFI_ABORTED;
#else
      /* A partially released or live-DMA GPU cannot be handed to the OS. */
      (VOID)GuBootClose();
      GuBootUnsilence();
      GuBootSay(L"\r\n[ERROR] GPU state may be unsafe; powering off.\r\n"
                L"Remove the unlock USB device before the next boot.\r\n");
      if (t->RuntimeServices && t->RuntimeServices->ResetSystem)
        t->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
      GuBootHalt();
      return EFI_ABORTED;
#endif
    }
#ifdef GU_DXE_FIRMWARE_REATTACH
    GuBootUnsilence();
    GuTraceStop(stage,(UINT64)s);
#endif
  }
  EFI_STATUS closed=GuBootClose();
  return closed==EFI_SUCCESS?s:closed;
}

/* Signal once, close the event, then execute the existing checked path. */
static VOID EFIAPI GuDxeOnReadyToBoot(EFI_EVENT event, VOID *context) {
  (VOID)context;
  if(DxeExecuted || !DxeImage || !DxeTable||!DxeEvent||event!=DxeEvent)return;
  DxeExecuted=TRUE;
  if(!DxeTable->BootServices||!DxeTable->BootServices->CloseEvent) {
    DxeEventCloseStatus=EFI_UNSUPPORTED;return;
  }
  DxeEventCloseStatus=DxeTable->BootServices->CloseEvent(DxeEvent);
  /* On uncertainty keep the address for diagnosis, never retry or run. */
  if(DxeEventCloseStatus!=EFI_SUCCESS)return;
  DxeEvent=NULL;
  (VOID)GuDxeRun(DxeImage,DxeTable);
}

EFI_STATUS EFIAPI AdaUnlockDriverEntryPoint(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
#ifdef GU_SYNTHETIC_COMPILE_ONLY
  (VOID)image; (VOID)t;
  return EFI_UNSUPPORTED; /* CI image: never register an event. */
#endif
  if(!image||!t||!t->BootServices||!t->RuntimeServices||
     !t->BootServices->CreateEventEx||!t->BootServices->CloseEvent)
    return EFI_UNSUPPORTED;
  if(DxeEvent||DxeExecuted)return EFI_ALREADY_STARTED;
  DxeImage=image;
  DxeTable=t;
  EFI_STATUS s=t->BootServices->CreateEventEx(
    EVT_NOTIFY_SIGNAL,TPL_CALLBACK,GuDxeOnReadyToBoot,
    NULL,&ReadyToBootGroup,&DxeEvent);
  if(s!=EFI_SUCCESS||!DxeEvent) {
    /* An error with a nonnull event output cannot prove no event exists.
     * Keep the reference and permanently refuse this image's callback. */
    if(DxeEvent)DxeExecuted=TRUE;
    DxeImage=NULL;DxeTable=NULL;
    return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  }
  return EFI_SUCCESS;
}
