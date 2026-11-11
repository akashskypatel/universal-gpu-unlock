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
#include "vendor/unlock_610.h"
#include "s3_save.h"

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



static EFI_STATUS GuDxeRun(EFI_HANDLE image, EFI_SYSTEM_TABLE *t) {
  if (!image || !t || !t->BootServices) return EFI_INVALID_PARAMETER;

  EFI_STATUS s = GuBootOpen(image, t);
  if (s != EFI_SUCCESS) return s;

  const char *stage = "dxe-discover";
  s = GuDiscoverSingleGpu(image, t);
  if (s != EFI_SUCCESS) goto finish;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  GuLog("ACTIVE_DXE=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1\n");
  s = EFI_UNSUPPORTED; goto finish;
#endif

  if (Gu.is_ampere) {
    GuLog("DXE-DRIVER: Ampere GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);
  } else if (Gu.is_turing) {
    GuLog("DXE-DRIVER: Turing GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);
  } else {
    GuLog("DXE-DRIVER: Ada Lovelace GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);
  }

  stage = "dxe-assets";
  s = GuBootLoadAssets();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage = "dxe-read-only-diagnostic";
  (VOID)GuDiagnosticScan();
  if(Gu.fault){s=EFI_DEVICE_ERROR;goto finish;}
  stage = "dxe-cold-profile";
  s = GuAd104Prepare();
  if (s != EFI_SUCCESS || Gu.fault) goto finish;

  stage="dxe-shared-execution";
  s=GuExecuteUnlock(t,image,&stage);
  if(s!=EFI_SUCCESS||Gu.fault)goto finish;
  GuLog("DXE selected GPU transaction checked; S3 registration optional\n");
  EFI_STATUS s3=GuRegisterS3(t);
  GuLog("DXE S3 registration status=%#llx\n",(unsigned long long)s3);
  if(Gu.retained) {
    stage="dxe-s3-partial-registration";
    s=s3==EFI_SUCCESS?EFI_DEVICE_ERROR:s3;
    goto finish;
  }
  /* S3 installation must not weaken the final AMD/peer/PCI handover
   * check. A changed boot-time ownership graph fails closed. */
  if(!GuMsiReadyForWindows() || !GuSafeToReturn()) {
    stage="dxe-post-s3-handover";
    s=EFI_NOT_READY;
    goto finish;
  }

finish:
  if (s == EFI_SUCCESS && Gu.fault) s = EFI_DEVICE_ERROR;
  if (s != EFI_SUCCESS) {
    GuLog("DXE-DRIVER: STOP stage=%s status=%#llx mutated=%u retained=%u started=%u fault=%u\n",
          stage, (unsigned long long)s, GuMsiMutated(), Gu.retained, Gu.started, Gu.fault);
    if (Gu.started || Gu.master_attributes_pending) (VOID)GuClearMaster();
    if (GuMsiMutated() || Gu.started || Gu.retained || Gu.master_attributes_pending) {
      /* A partially released or live-DMA GPU cannot be handed to the OS. */
      (VOID)GuBootClose();
      GuBootUnsilence();
      GuBootSay(L"\r\n[ERROR] GPU state may be unsafe; powering off.\r\n"
                L"Remove the unlock USB device before the next boot.\r\n");
      if (t->RuntimeServices && t->RuntimeServices->ResetSystem)
        t->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
      GuBootHalt();
      return EFI_ABORTED;
    }
  } else {
    GuLog("DXE-DRIVER: Pre-OS transaction completed; post-boot benchmark is still required.\n");
  }
  (VOID)GuBootClose();
  return s;
}

/* Signal once, close the event, then execute the existing checked path. */
static VOID EFIAPI GuDxeOnReadyToBoot(EFI_EVENT event, VOID *context) {
  (VOID)context;
  if(DxeExecuted || !DxeImage || !DxeTable)return;
  DxeExecuted=TRUE;
  if(DxeEvent && DxeTable->BootServices && DxeTable->BootServices->CloseEvent) {
    (VOID)DxeTable->BootServices->CloseEvent(DxeEvent);
    DxeEvent=NULL;
  }
  (VOID)event;
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
    DxeImage=NULL;DxeTable=NULL;DxeEvent=NULL;
    return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  }
  return EFI_SUCCESS;
}
