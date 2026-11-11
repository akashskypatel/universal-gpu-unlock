/* SPDX-License-Identifier: GPL-2.0-only */
#include "unlock_execution.h"
#include "msi_release.h"
#include "gpu_discovery.h"
#include "profile.h"
#include "compat/compat.h"
#include "vendor/unlock_610.h"
#include "s3_save.h"

/* The app and the DXE driver must not diverge on the release/firmware
 * order or skip the final selected-target Windows handover validation. */
static EFI_STATUS GuExecuteUnlockCore(EFI_SYSTEM_TABLE *table,EFI_HANDLE image,
                           const char **stage)
{
  EFI_STATUS s;
  int result;
  UINT64 mask=0xffffffffffffULL;
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

  struct pci_dev dev={.vendor=0x10de,.device=Gu.device_id,
    .dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},
    .current_state=PCI_D0,.pm_cap=Gu.pm_cap,.refs=1};

  *stage="shared-firmware-assets";
  result=gpu_unlock_610_load_shared(&dev,&shared);
  if(!result) {
    *stage="firmware-transaction";
    result=gpu_unlock_610_unlock(&dev,&shared);
  }
  gpu_unlock_610_release_shared(&shared);
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
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(!table || !table->BootServices || !image || !stage)
    return EFI_INVALID_PARAMETER;
  *stage="selected-gpu-state-begin";
  EFI_STATUS s=GuBeginSelectedGpuStep(table);
  if(s!=EFI_SUCCESS)return s;
  s=GuExecuteUnlockCore(table,image,stage);
  if(s!=EFI_SUCCESS || Gu.fault) {
    GuAbortSelectedGpuStep();
    return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  }
  *stage="selected-gpu-state-finish";
  /* Core already checked GuMsiFinish and the full Windows handover. */
  s=GuFinishSelectedGpuStep(table,GuMsiReadyForWindows() &&
                             !Gu.fault && GuSafeToReturn());
  if(s!=EFI_SUCCESS)GuAbortSelectedGpuStep();
  return s;
#else
  return GuExecuteUnlockCore(table,image,stage);
#endif
}

EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *table)
{
  static EFI_GUID save_guid=EFI_S3_SAVE_STATE_PROTOCOL_GUID;
  EFI_S3_SAVE_STATE_PROTOCOL *s3=NULL;
  if(!table||!table->BootServices||!GuSafeToReturn()||Gu.fault)
    return EFI_NOT_READY;
  EFI_STATUS status=table->BootServices->LocateProtocol(&save_guid,NULL,(VOID **)&s3);
  if(status!=EFI_SUCCESS||!s3||!s3->Write)return status==EFI_SUCCESS?EFI_UNSUPPORTED:status;
  UINT32 ss0=0x88888888U,ss1=8U;
  UINT64 addr0=(UINT64)Gu.bar_base+0x0082381cULL;
  UINT64 addr1=(UINT64)Gu.bar_base+0x00823820ULL;
  EFI_STATUS a=s3->Write(s3,EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                  EfiBootScriptWidthUint32,addr0,(UINTN)1,&ss0);
  EFI_STATUS b=s3->Write(s3,EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE,
                  EfiBootScriptWidthUint32,addr1,(UINTN)1,&ss1);
  GuLog("S3 selected-card write result: SS0=%#llx SS1=%#llx\n",
        (unsigned long long)a,(unsigned long long)b);
  return a!=EFI_SUCCESS?a:b;
}
