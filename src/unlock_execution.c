/* SPDX-License-Identifier: GPL-2.0-only */
#include "unlock_execution.h"
#include "msi_release.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "compat/compat.h"
#include "vendor/unlock_610.h"
#include "s3_save.h"
#include "boot_io.h"

/* The app and the DXE driver must not diverge on the release/firmware
 * order or skip the final selected-target Windows handover validation. */
static EFI_STATUS GuExecuteUnlockCore(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage)
{
  EFI_STATUS s;
  int result;
  struct pci_dev *dev=NULL;
  struct gpu_unlock_610_shared shared={0};
  if(!table||!table->BootServices||!image||!stage||!Gu.handle||!Gu.pci)
    return EFI_INVALID_PARAMETER;
  *stage="preflight";
  s=GuMsiPreflight(table,image,Gu.handle);
  if(s!=EFI_SUCCESS||Gu.fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  *stage="driver-release";
  s=GuMsiRelease();
  if(s!=EFI_SUCCESS||Gu.fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  *stage="cold-entry";
  if(!GuMsiReleased()||!GuAd104ColdEntry())return EFI_NOT_READY;
  Gu.display_check=GuMsiReleased;
  Gu.display_ok=TRUE;
  Gu.armed=TRUE;

  s=GuCompatOpenDevice(&dev);
  if(s!=EFI_SUCCESS||Gu.fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  *stage="shared-firmware-assets";
  result=gpu_unlock_610_load_shared(dev,&shared);
  if(!result) {
    *stage="firmware-transaction";
    result=gpu_unlock_610_unlock(dev,&shared);
  }
  gpu_unlock_610_release_shared(&shared);
  s=GuCompatCloseDevice(dev);
  if(s!=EFI_SUCCESS)return s;
  GuLog("Selected GPU result=%d fault=%u safe=%u retained=%u flrs=%u DMA=%u\n",
        result,(unsigned)Gu.fault,(unsigned)GuSafeToReturn(),
        (unsigned)Gu.retained,(unsigned)Gu.flrs,(unsigned)Gu.dma_count);
  if(result||Gu.fault||!GuSafeToReturn())return EFI_DEVICE_ERROR;

  *stage="release-exclusive-claim";
  s=GuMsiFinish();
  if(s!=EFI_SUCCESS||Gu.fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;

  *stage="final-Windows-verification";
  if(!GuMsiReadyForWindows()||Gu.fault)return EFI_NOT_READY;
  GuLog("Selected preboot transaction verified; stock Windows driver acceptance and throughput remain unverified\n");
  return EFI_SUCCESS;
}

/* The ordered-state transition is tied to the actual shared transaction.
 * The existing MSI preflight/Stop and final handover remain authoritative:
 * this CPU-only record must never be used as their replacement. */
EFI_STATUS GuExecuteUnlock(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage) {
  if(!table || !table->BootServices || !image || !stage)
    return EFI_INVALID_PARAMETER;
  *stage="selected-gpu-resource-reserve";
  EFI_STATUS s=GuMsiReserveSelectedState(table->BootServices);
  if(s!=EFI_SUCCESS)return s;
#ifdef GU_ACTIVE_SINGLE_TARGET
  *stage="selected-gpu-state-begin";
  s=GuBeginSelectedGpuStep(table,&Gu,
                           GuMsiPlanOwnerToken(),GuAssets);
  if(s!=EFI_SUCCESS)return s;
  s=GuExecuteUnlockCore(table,image,stage);
  if(s!=EFI_SUCCESS || Gu.fault) {
    GuAbortSelectedGpuStep();
    return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  }
  *stage="selected-gpu-state-finish";
  /* Core already checked GuMsiFinish and the full Windows handover. */
  s=GuFinishSelectedGpuStep(table,GuMsiReadyForWindows() &&
                             !Gu.fault && GuSafeToReturn(),&Gu,
                             GuMsiPlanOwnerToken(),GuAssets);
  if(s!=EFI_SUCCESS)GuAbortSelectedGpuStep();
  return s;
#else
  return GuExecuteUnlockCore(table,image,stage);
#endif
}

/* Shared preparation of the currently bound GPU. The application runs
 * this once before operator choice. Each successor runs it after binding. */
EFI_STATUS GuPrepareBoundGpu(const char **stage) {
  if(!stage||!Gu.handle||!Gu.pci)return EFI_INVALID_PARAMETER;
  *stage="assets";
  EFI_STATUS s=GuBootLoadAssets();
  if(s!=EFI_SUCCESS||Gu.fault)return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  *stage="read-only-diagnostic";
  (VOID)GuDiagnosticScan();
  if(Gu.fault)return EFI_DEVICE_ERROR;
  *stage="read-only-cold-profile";
  s=GuAd104Prepare();
  return s!=EFI_SUCCESS?s:Gu.fault?EFI_DEVICE_ERROR:EFI_SUCCESS;
}

/* The first target is prepared before entry. Only the ordered execution
 * record can select a successor. No retry, fallback, or hardware admission
 * is granted by this loop. Preserve required versus optional S3 policy. */
EFI_STATUS GuExecutePreparedSequence(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                                     GU_S3_POLICY policy,const char **stage) {
  if(!table||!table->BootServices||!image||!stage||
     (policy!=GU_S3_DISABLED&&policy!=GU_S3_REQUIRED&&policy!=GU_S3_OPTIONAL))
    return EFI_INVALID_PARAMETER;
  EFI_STATUS s;
  for(;;) {
    *stage="selected-gpu-execution";
    s=GuExecuteUnlock(table,image,stage);
    if(s!=EFI_SUCCESS||Gu.fault)goto failed;
    if(policy!=GU_S3_DISABLED) {
      *stage="s3-registration";
      EFI_STATUS s3=GuRegisterS3(table);
      GuLog("S3 selected GPU registration status=%#llx\n",(unsigned long long)s3);
      if(Gu.retained||Gu.fault||(policy==GU_S3_REQUIRED&&s3!=EFI_SUCCESS)) {
        s=s3==EFI_SUCCESS?EFI_DEVICE_ERROR:s3;
        goto failed;
      }
    }
#ifdef GU_ACTIVE_SINGLE_TARGET
    /* Recheck the current identity after optional resume-script work.
     * Do not advance on an invalid record or mistake failure for completion. */
    int more=0;
    *stage="selected-gpu-next";
    s=GuSelectedSequenceHasNext(table,&more);
    if(s!=EFI_SUCCESS||Gu.fault)goto failed;
    if(more) {
      s=GuSelectNextGpu(table);
      if(s!=EFI_SUCCESS||Gu.fault)goto failed;
      s=GuPrepareBoundGpu(stage);
      if(s!=EFI_SUCCESS||Gu.fault)goto failed;
      continue;
    }
#endif
    break;
  }
  *stage="final-Windows-verification";
  if(!GuExecutionReadyForWindows(table)) {
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

/* Do not classify a later preflight failure as an untouched boot. */
BOOLEAN GuExecutionNeedsContainment(VOID) {
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(GuAnyPlatformNeedsContainment())return TRUE;
#endif
  return GuMsiMutated()||Gu.started||Gu.retained||Gu.owned||
    Gu.master_attributes_pending||Gu.allocations||Gu.dma_count||
    Gu.s3_script_attempted||Gu.flrs||Gu.mmio_writes||Gu.release_owner_invalid;
}

/* The selected path requires aggregate proof, not just the active GPU. */
BOOLEAN GuExecutionReadyForWindows(EFI_SYSTEM_TABLE *table) {
#ifdef GU_ACTIVE_SINGLE_TARGET
  return GuVerifySelectedSequence(table)==EFI_SUCCESS;
#else
  (VOID)table;
  return GuMsiReadyForWindows()&&GuSafeToReturn()&&!Gu.fault;
#endif
}

EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *table)
{
  static EFI_GUID save_guid=EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  EFI_S3_SAVE_STATE_PROTOCOL *s3=NULL;
  /* A CPU idle state is not proof of a completed GPU handover. Do not
   * install any future resume writes before the MSI/peer checks pass. */
  if(!table||!table->BootServices||!GuSafeToReturn()||Gu.fault||
     !GuMsiReadyForWindows()||!Gu.bar_base||Gu.bar_size!=0x1000000ULL||
     Gu.bar_base>~(UINT64)0-0x00823820ULL)return EFI_NOT_READY;
  if(Gu.s3_script_attempted)return EFI_ALREADY_STARTED;
  if(!table->BootServices->LocateProtocol)return EFI_UNSUPPORTED;
  EFI_STATUS status=table->BootServices->LocateProtocol(&save_guid,NULL,(VOID **)&s3);
  if(status!=EFI_SUCCESS||!s3||!s3->Write)return status==EFI_SUCCESS?EFI_UNSUPPORTED:status;
  UINT32 ss0=0x88888888U,ss1=8U;
  UINT64 addr0=(UINT64)Gu.bar_base+0x0082381cULL;
  UINT64 addr1=(UINT64)Gu.bar_base+0x00823820ULL;
  Gu.s3_script_attempted=TRUE; /* One attempt per physical card. */
  EFI_STATUS a=s3->Write(s3,EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                  EfiBootScriptWidthUint32,addr0,(UINTN)1,&ss0);
  if(a!=EFI_SUCCESS) {
    /* A failed firmware call need not prove that no script entry was added. */
    Gu.retained=TRUE;
    GuLog("S3 first write uncertain: status=%#llx; Windows handover blocked\n",
          (unsigned long long)a);
    return a;
  }
  EFI_STATUS b=s3->Write(s3,EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                  EfiBootScriptWidthUint32,addr1,(UINTN)1,&ss1);
  if(b!=EFI_SUCCESS)Gu.retained=TRUE;
  GuLog("S3 selected-card write result: SS0=%#llx SS1=%#llx PARTIAL=%u\n",
        (unsigned long long)a,(unsigned long long)b,(unsigned)Gu.retained);
  return b;
}
