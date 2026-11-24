/* SPDX-License-Identifier: GPL-2.0-only */
#include "unlock_execution.h"
#include "msi_release.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "compat/compat.h"
#ifndef GU_STOP_RELEASE_ONLY_BENCH
#include "vendor/unlock_610.h"
#include "s3_save.h"
#endif
#include "boot_io.h"

/* GU_PHASE and GU_TRACE_BEGIN come from boot_io.h. Every stage assignment is
 * also a trace line there, so the string a refusal prints at the end and the line
 * a stall prints at the moment it stalls cannot drift apart. */

#ifndef GU_STOP_RELEASE_ONLY_BENCH
/* The app and the DXE driver must not diverge on the release/firmware
 * order or skip the final selected-target Windows handover validation. */
static EFI_STATUS GuExecuteUnlockCore(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table,
                           EFI_HANDLE image,
                           const char **stage)
{
  EFI_STATUS s;
  int result;
  struct pci_dev *dev=NULL;
  struct gpu_unlock_610_shared shared={0};
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
  if(!table||!table->BootServices||!image||!stage||!owner->handle||!owner->pci)
    return EFI_INVALID_PARAMETER;
  GU_PHASE("preflight");
  s=GuMsiPreflight(table,image,owner->handle);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  GU_PHASE("compatibility-reserve");
  s=GuCompatReserveDevice(owner);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

#ifdef GU_FIRMWARE_REATTACH
  GU_PHASE("firmware-peer-warning");
  s=GuMsiStopReleaseWarnSurvivor();
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  if(!GuBootLogOpen()||!table->BootServices->Stall)return EFI_NOT_READY;
  s=table->BootServices->Stall(3000000U);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
#endif
  GU_PHASE("driver-release");
  if(!GuBootLogOpen())return EFI_DEVICE_ERROR;
  s=GuMsiRelease();
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  GuTraceDetail("driver-release","STATUS=%#llx RELEASED=%u",
                (unsigned long long)s,(unsigned)GuMsiReleased());
#ifdef GU_FIRMWARE_REATTACH
  EFI_STATUS peer_console=GuMsiStopReleaseTryPeerConsole();
  GuLog("FIRMWARE_PEER_CONSOLE_ATTEMPT STATUS=%#llx GLOBAL_CONOUT_CHANGED=0\n",
        (unsigned long long)peer_console);
  if(GuVerifyActivePeers(table)!=EFI_SUCCESS||!GuBootLogOpen())return EFI_DEVICE_ERROR;
#endif
  GU_PHASE("cold-entry");
  if(!GuMsiReleased()||owner!=GuPlatformActive||
     !GuAd104ColdEntryFor(owner)||owner!=GuPlatformActive)return EFI_NOT_READY;
  owner->display_check=GuMsiReleased;
  owner->display_ok=TRUE;
  owner->armed=TRUE;

  GU_PHASE("compatibility-open");
  s=GuCompatOpenDevice(owner,&dev);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault||
     GuCompatOwner(dev)!=owner)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  GU_PHASE("shared-firmware-assets");
  result=gpu_unlock_610_load_shared(dev,&shared);
  if(owner!=GuPlatformActive)return EFI_NOT_READY;
  if(!result) {
    GU_PHASE("firmware-transaction");
    result=gpu_unlock_610_unlock(dev,&shared);
  }
  GuTraceDetail("firmware-transaction","RESULT=%d MUTATED=%u DMA=%u FLRS=%u",
                result,(unsigned)GuMsiMutated(),(unsigned)owner->dma_count,
                (unsigned)owner->flrs);
  if(owner!=GuPlatformActive)return EFI_NOT_READY;
  int released=gpu_unlock_610_release_shared(dev,&shared);
  if(owner!=GuPlatformActive)return EFI_NOT_READY;
  if(!result&&released)result=released;
  s=GuCompatCloseDevice(dev);
  if(s!=EFI_SUCCESS)return s;
  if(owner!=GuPlatformActive)return EFI_NOT_READY;
  GuLog("Selected GPU result=%d fault=%u safe=%u retained=%u flrs=%u DMA=%u\n",
        result,(unsigned)owner->fault,(unsigned)GuSafeToReturnFor(owner),
        (unsigned)owner->retained,(unsigned)owner->flrs,(unsigned)owner->dma_count);
  if(result||owner!=GuPlatformActive||owner->fault||!GuSafeToReturnFor(owner))return EFI_DEVICE_ERROR;

  GU_PHASE("release-exclusive-claim");
  s=GuMsiFinish();
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

#ifdef GU_FIRMWARE_REATTACH
  GU_PHASE("reattach-firmware-drivers");
  if(!GuBootLogOpen())return EFI_DEVICE_ERROR;
  s=GuMsiReattachAfterStop();
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  GuLog("FIRMWARE_REATTACH=VERIFIED TRANSACTION_VERIFIED=1 FUNCTIONAL_PERSISTENCE=PENDING_WINDOWS\n");
#endif
  GU_PHASE("final-Windows-verification");
  if(!GuMsiReadyForWindows()||owner!=GuPlatformActive||owner->fault)return EFI_NOT_READY;
  GuLog("Selected preboot transaction verified; stock Windows driver acceptance and throughput remain unverified\n");
  return EFI_SUCCESS;
}

/* The ordered-state transition is tied to the actual shared transaction.
 * The existing MSI preflight/Stop and final handover remain authoritative:
 * this CPU-only record must never be used as their replacement. */
EFI_STATUS GuExecuteUnlock(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table,
                           EFI_HANDLE image,
                           const char **stage) {
  if(!table || !table->BootServices || !image || !stage)
    return EFI_INVALID_PARAMETER;
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
  GU_PHASE("selected-gpu-resource-reserve");
  EFI_STATUS s=GuMsiReserveSelectedState(table->BootServices);
  if(s!=EFI_SUCCESS)return s;
  if(owner!=GuPlatformActive)return EFI_NOT_READY;
#ifdef GU_ACTIVE_SINGLE_TARGET
  GU_PHASE("selected-gpu-state-begin");
  s=GuBeginSelectedGpuStep(table,owner,
                           GuMsiPlanOwnerToken(),owner->asset_entries);
  if(s!=EFI_SUCCESS)return s;
  s=GuExecuteUnlockCore(owner,table,image,stage);
  if(s!=EFI_SUCCESS || owner!=GuPlatformActive || owner->fault) {
    GuAbortSelectedGpuStep();
    return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  }
  GU_PHASE("selected-gpu-state-finish");
  /* Core already checked GuMsiFinish and the full Windows handover. */
  BOOLEAN ready=GuMsiReadyForWindows() && owner==GuPlatformActive &&
                !owner->fault && GuSafeToReturnFor(owner);
  if(owner!=GuPlatformActive)s=EFI_NOT_READY;
  else s=GuFinishSelectedGpuStep(table,ready,owner,
                                GuMsiPlanOwnerToken(),owner->asset_entries);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive) {
    GuAbortSelectedGpuStep();
    return s!=EFI_SUCCESS?s:EFI_NOT_READY;
  }
  return s;
#else
  return GuExecuteUnlockCore(owner,table,image,stage);
#endif
}
#endif /* !GU_STOP_RELEASE_ONLY_BENCH */

/* Shared preparation of the currently bound GPU. The application runs
 * this once before operator choice. Each successor runs it after binding. */
EFI_STATUS GuPrepareBoundGpu(GU_PLATFORM *owner,const char **stage) {
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
  if(!stage||!owner->handle||!owner->pci)return EFI_INVALID_PARAMETER;
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  if(owner->stop_release_ready||owner->started||owner->owned||owner->fault||
     owner->retained||owner->asset_entries_reserved||owner->asset_load_attempted)
    return EFI_NOT_READY;
  /* The release-only path needs a bound live PCI identity, not the firmware
   * payloads or PROM qualification used by the unlock transaction. */
  owner->stop_release_ready=TRUE;
  GU_PHASE("stop-release-ready");
  GuLog("STOP_RELEASE_PREPARED=1 FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0 "
        "DIAGNOSTIC_SCAN=0 FIRMWARE_TRANSACTION=0\n");
  return EFI_SUCCESS;
#else
  GU_TRACE_BEGIN();
  GU_PHASE("assets");
  EFI_STATUS s=GuBootLoadAssets();
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  GU_PHASE("read-only-diagnostic");
  (VOID)GuDiagnosticScan(owner);
  if(owner!=GuPlatformActive||owner->fault)return EFI_DEVICE_ERROR;
  GU_PHASE("read-only-cold-profile");
  s=GuAd104Prepare();
  return s!=EFI_SUCCESS?s:(owner!=GuPlatformActive||owner->fault)?EFI_DEVICE_ERROR:EFI_SUCCESS;
#endif
}

#ifdef GU_ACTIVE_SINGLE_TARGET
/* Advance only after the current step has completed and the caller has
 * checked that the ordered record has a successor. Keep the previous
 * owner until the checked binding operation publishes a different one. */
static EFI_STATUS GuPrepareNextSelectedGpu(GU_PLATFORM **owner,EFI_SYSTEM_TABLE *table,
                                          const char **stage) {
  if(!owner||!*owner||*owner!=GuPlatformActive||!stage)return EFI_NOT_READY;
  GU_PLATFORM *previous=*owner;
  if(previous->fault||previous->retained)return EFI_NOT_READY;
  if(!GuBootLogOpen())return EFI_DEVICE_ERROR;
  EFI_STATUS s=GuSelectNextGpu(table);
  if(s!=EFI_SUCCESS)return s;
  GU_PLATFORM *next=GuPlatformActive;
  if(!next||next==previous||previous->fault||previous->retained||next->fault)
    return EFI_NOT_READY;
  *owner=next;
#ifdef GU_FIRMWARE_REATTACH
  /* The previous step is complete and the successor is bound. Only this
   * read-only preparation window may reuse the verified direct peer. */
  GuBootTraceSuccessorPreparation(TRUE);
  GuTraceScreen("\r\n[gu] Preparing next selected GPU\r\n");
#endif
  s=GuPrepareBoundGpu(next,stage);
#ifdef GU_FIRMWARE_REATTACH
  GuBootTraceSuccessorPreparation(FALSE);
#endif
  return s!=EFI_SUCCESS?s:(next!=GuPlatformActive||next->fault)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
#endif

#ifndef GU_STOP_RELEASE_ONLY_BENCH
/* The first target is prepared before entry. Only the ordered execution
 * record can select a successor. No retry, fallback, or hardware admission
 * is granted by this loop. Preserve required versus optional S3 policy. */
EFI_STATUS GuExecutePreparedSequence(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table,
                                     EFI_HANDLE image,
                                     GU_S3_POLICY policy,const char **stage) {
  if(!table||!table->BootServices||!image||!stage||
     (policy!=GU_S3_DISABLED&&policy!=GU_S3_REQUIRED&&policy!=GU_S3_OPTIONAL))
    return EFI_INVALID_PARAMETER;
  if(!owner||owner!=GuPlatformActive)return EFI_NOT_READY;
#ifdef GU_FIRMWARE_REATTACH
  if(policy!=GU_S3_DISABLED)return EFI_UNSUPPORTED;
#endif
  EFI_STATUS s;
  for(;;) {
    GU_PHASE("selected-gpu-execution");
    if(!GuBootLogOpen()){s=EFI_DEVICE_ERROR;goto failed;}
    s=GuExecuteUnlock(owner,table,image,stage);
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
    if(!GuBootLogOpen()){s=EFI_DEVICE_ERROR;goto failed;}
    if(policy!=GU_S3_DISABLED) {
      GU_PHASE("s3-registration");
      EFI_STATUS s3=GuRegisterS3(owner,table);
      GuLog("S3 selected GPU registration status=%#llx\n",(unsigned long long)s3);
      if(owner!=GuPlatformActive||owner->retained||owner->fault||
         (policy==GU_S3_REQUIRED&&s3!=EFI_SUCCESS)) {
        s=s3==EFI_SUCCESS?EFI_DEVICE_ERROR:s3;
        goto failed;
      }
    }
#ifdef GU_ACTIVE_SINGLE_TARGET
    /* Recheck the current identity after optional resume-script work.
     * Do not advance on an invalid record or mistake failure for completion. */
    int more=0;
    GU_PHASE("selected-gpu-next");
    s=GuSelectedSequenceHasNext(table,&more);
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
    if(more) {
      s=GuPrepareNextSelectedGpu(&owner,table,stage);
      if(s!=EFI_SUCCESS)goto failed;
      continue;
    }
#endif
    break;
  }
  GU_PHASE("final-Windows-verification");
  if(owner!=GuPlatformActive||!GuExecutionReadyForWindows(table)||owner!=GuPlatformActive) {
    s=EFI_NOT_READY;
    goto failed;
  }
  return EFI_SUCCESS;
failed:
#ifdef GU_ACTIVE_SINGLE_TARGET
  GuAbortSelectedGpuStep();
#endif
  return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
}
/* Completion is shared by the application and DXE driver. Preserve the
 * first failure, never cancel before the transaction ends, and leave INTENT
 * and the caller's containment or handover policy untouched. */
EFI_STATUS GuCompletePreparedSequence(EFI_STATUS status,GU_PLATFORM **owner) {
  if(!owner)return EFI_INVALID_PARAMETER;
  *owner=GuPlatformActive;
  if(!*owner)return EFI_NOT_READY;
  if(status==EFI_SUCCESS&&!GuBootLogOpen())status=EFI_DEVICE_ERROR;
  EFI_STATUS cancelled=GuBootWatchdogCancel();
  if(status==EFI_SUCCESS&&cancelled!=EFI_SUCCESS)status=cancelled;
  if(status==EFI_SUCCESS&&(*owner!=GuPlatformActive||(*owner)->fault||
                           !GuBootLogOpen()))status=EFI_DEVICE_ERROR;
  return status;
}
#else
/* This is scoped to the attempted reattachment, not a refused preflight
 * or a release stage that never reached reconnect. */
static BOOLEAN StopReleaseReattachFailed;
BOOLEAN GuStopReleaseReattachFailed(VOID) {
  return StopReleaseReattachFailed;
}
/* The explicitly scoped bench image stops/releases each selected card,
 * reconnects its firmware drivers, verifies the reattached graph, then moves
 * to the next target. It has no call edge into the vendor unlock transaction. */
EFI_STATUS GuExecuteStopReleaseSequence(GU_PLATFORM *owner,
                           EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage) {
  if(!table||!table->BootServices||!image||!stage||
     !owner||owner!=GuPlatformActive)return EFI_INVALID_PARAMETER;
  EFI_STATUS s;
  StopReleaseReattachFailed=FALSE;
  if(GuSelectedGpuCount()>1U) {
    GuMsiStopReleaseRequireFileOnly();
    GuLog("STOP_RELEASE_OUTPUT=FILE_ONLY REASON=ORDERED_MULTI_TARGET CONSOLE_IO=0\n");
  }
  for(;;) {
    GU_PHASE("stop-release-state-reserve");
    s=GuMsiReserveSelectedState(table->BootServices);
    if(s!=EFI_SUCCESS)goto failed;
    s=GuBeginSelectedGpuStep(table,owner,GuMsiPlanOwnerToken(),&owner->stop_release_ready);
    if(s!=EFI_SUCCESS)goto failed;
    GU_PHASE("stop-release-preflight");
    s=GuMsiPreflight(table,image,owner->handle);
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
    /* Every selected GPU needs a new survivor warning and delay. */
    s=GuBootBenchCountdown();
    if(s!=EFI_SUCCESS)goto failed;
    GU_PHASE("stop-release");
    s=GuMsiRelease();
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
    /* GuMsiRelease does not return success until its released-state
     * PCI, protocol graph, survivor GOP and peer checks all pass. */
    GuLog("STOP_RELEASE_RELEASE_VERIFIED=1 FIRMWARE_TRANSACTION=0\n");
    /* Console-owning first-card experiment: after Stop, request the
     * independently captured peer GOP text interface. Its absence must
     * not prevent the already-stopped target from reattaching. */
    EFI_STATUS peer_console=GuMsiStopReleaseTryPeerConsole();
    GuLog("STOP_RELEASE_PEER_CONSOLE_ATTEMPT STATUS=%#llx "
          "REATTACH_CONTINUES=1 GLOBAL_CONOUT_CHANGED=0\n",
          (unsigned long long)peer_console);
    GU_PHASE("reattach-firmware-drivers");
    s=GuMsiReattachAfterStop();
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault) {
      StopReleaseReattachFailed=TRUE;
      GuLog("STOP_RELEASE_REATTACH=FAILED STATUS=%#llx NEXT_GPU=BLOCKED "
            "WARM_RESET=MANUAL_AFTER_CONTAINMENT AUTO_RETRY=0\n",
            (unsigned long long)(s==EFI_SUCCESS?EFI_DEVICE_ERROR:s));
      goto failed;
    }
    s=GuFinishSelectedGpuStep(table,1,owner,GuMsiPlanOwnerToken(),&owner->stop_release_ready);
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
    GuLog("STOP_RELEASE_STEP=COMPLETE BDF=%04x:%02x:%02x.%x "
          "FIRMWARE_TRANSACTION=0 REATTACH_VERIFIED=1\n",
          (unsigned)owner->discovery_identity.segment,
          (unsigned)owner->discovery_identity.bus,
          (unsigned)owner->discovery_identity.device,
          (unsigned)owner->discovery_identity.function);
    int more=0;
    s=GuSelectedSequenceHasNext(table,&more);
    if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
    if(!more)break;
    s=GuPrepareNextSelectedGpu(&owner,table,stage);
    if(s!=EFI_SUCCESS)goto failed;
  }
  GU_PHASE("stop-release-sequence-verification");
  if(GuVerifySelectedSequence(table)!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault) {
    s=EFI_NOT_READY;goto failed;
  }
#ifdef GU_RUNTIME_SELECTION
  GuLog("STOP_RELEASE_SEQUENCE=COMPLETE ORDER=LIVE_SUPPORTED_NVIDIA_ENUMERATION "
        "FIRMWARE_TRANSACTION=0 UNLOCK=0\n");
#else
  GuLog("STOP_RELEASE_SEQUENCE=COMPLETE ORDER=COMPILED_BDF_SEQUENCE "
        "FIRMWARE_TRANSACTION=0 UNLOCK=0\n");
#endif
  return EFI_SUCCESS;
failed:
  GuLog("STOP_RELEASE_SEQUENCE=FAILED STATUS=%#llx FIRMWARE_TRANSACTION=0\n",
        (unsigned long long)(s==EFI_SUCCESS?EFI_DEVICE_ERROR:s));
  GuAbortSelectedGpuStep();
  return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
}
#endif

/* Do not classify a later preflight failure as an untouched boot. */
BOOLEAN GuExecutionNeedsContainment(VOID) {
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(GuAnyPlatformNeedsContainment())return TRUE;
#endif
  return GuMsiMutated()||Gu.started||Gu.retained||Gu.owned||
    Gu.master_attributes_pending||Gu.allocations||Gu.dma_count||
    Gu.s3_script_attempted||Gu.flrs||Gu.mmio_writes||Gu.release_owner_invalid;
}

#ifndef GU_STOP_RELEASE_ONLY_BENCH
/* The selected path requires aggregate proof, not just the active GPU. */
BOOLEAN GuExecutionReadyForWindows(EFI_SYSTEM_TABLE *table) {
#ifdef GU_ACTIVE_SINGLE_TARGET
  return GuVerifySelectedSequence(table)==EFI_SUCCESS;
#else
  (VOID)table;
  return GuMsiReadyForWindows()&&GuSafeToReturn()&&!Gu.fault;
#endif
}

EFI_STATUS GuRegisterS3(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table)
{
  static EFI_GUID save_guid=EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  EFI_S3_SAVE_STATE_PROTOCOL *s3=NULL;
  /* A CPU idle state is not proof of a completed GPU handover. Do not
   * install any future resume writes before the MSI/peer checks pass. */
  if(!owner||owner!=GuPlatformActive||!table||!table->BootServices||
     owner->bs!=table->BootServices||!GuSafeToReturnFor(owner)||owner->fault||
     !GuMsiReadyForWindows()||owner!=GuPlatformActive||owner->fault||
     !owner->bar_base||owner->bar_size<GU_BAR_MIN_BYTES||
     owner->bar_base>~(UINT64)0-0x00823820ULL)return EFI_NOT_READY;
  if(owner->s3_script_attempted)return EFI_ALREADY_STARTED;
  if(!table->BootServices->LocateProtocol)return EFI_UNSUPPORTED;
  EFI_STATUS status=table->BootServices->LocateProtocol(&save_guid,NULL,(VOID **)&s3);
  if(status!=EFI_SUCCESS||!s3||!s3->Write)return status==EFI_SUCCESS?EFI_UNSUPPORTED:status;
  if(owner!=GuPlatformActive)return EFI_NOT_READY;
  UINT32 ss0=0x88888888U,ss1=8U;
  UINT64 addr0=(UINT64)owner->bar_base+0x0082381cULL;
  UINT64 addr1=(UINT64)owner->bar_base+0x00823820ULL;
  owner->s3_script_attempted=TRUE; /* One attempt per physical card. */
  EFI_STATUS a=s3->Write(s3,EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                  EfiBootScriptWidthUint32,addr0,(UINTN)1,&ss0);
  if(a!=EFI_SUCCESS) {
    /* A failed firmware call need not prove that no script entry was added. */
    owner->retained=TRUE;
    GuLog("S3 first write uncertain: status=%#llx; Windows handover blocked\n",
          (unsigned long long)a);
    return a;
  }
  if(owner!=GuPlatformActive) {
    owner->retained=TRUE;return EFI_NOT_READY;
  }
  EFI_STATUS b=s3->Write(s3,EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                  EfiBootScriptWidthUint32,addr1,(UINTN)1,&ss1);
  if(b!=EFI_SUCCESS||owner!=GuPlatformActive)owner->retained=TRUE;
  GuLog("S3 selected-card write result: SS0=%#llx SS1=%#llx PARTIAL=%u\n",
        (unsigned long long)a,(unsigned long long)b,(unsigned)owner->retained);
  /* Registering a resume write is not knowing that it worked. Nothing in this
   * image runs at resume: the boot script executes in firmware, the card's
   * post-resume state is whatever the Windows driver leaves, and the absolute
   * addresses below only stay correct if the BAR is not reassigned. Say so
   * instead of letting a registered script read as a verified resume. */
  GuLog("S3_RESUME=UNVERIFIED WHY=NO_RESUME_HOOK BAR_BASE_MAY_MOVE=1 "
        "ADDRESSES=2 WIDTH=UINT32\n");
  return b!=EFI_SUCCESS?b:owner==GuPlatformActive?EFI_SUCCESS:EFI_NOT_READY;
}
#endif /* !GU_STOP_RELEASE_ONLY_BENCH */
