/* SPDX-License-Identifier: GPL-2.0-only */

#include "port.h"
#include "version.h"

#include "checks.h"
#include "gpu_discovery.h"

#include "profile.h"

#include "msi_release.h"
#include "unlock_execution.h"

#include "boot_io.h"

#include "compat/compat.h"

#include "vendor/unlock_610.h"

#include "s3_save.h"

#include "driver_nvram.h"

static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {

  if(!image||!t||!t->BootServices||!t->RuntimeServices||!t->RuntimeServices->GetVariable)return EFI_INVALID_PARAMETER;
#ifdef GU_SYNTHETIC_COMPILE_ONLY
  /* A CI image must not access or mutate user hardware. */
  return EFI_UNSUPPORTED;
#endif

  UINT8 secure=1;UINTN n=1;UINT32 attrs=0;

  EFI_STATUS s=t->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,&attrs,&n,&secure);

  /* Authenticode signature verified by UEFI firmware against custom DB key. Allow SecureBoot. */

  (void)secure;

  s=GuBootOpen(image,t);if(s!=EFI_SUCCESS)return s;

  const char *stage="discover";CHAR16 choice=L'S';

  s=GuDiscoverSingleGpu(image,t);if(s!=EFI_SUCCESS)goto failed;
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  /* Defense in depth: selected active preparation images never enter the
   * asset, driver-release, firmware, DMA or FLR stages even if discovery
   * behavior changes in the future. */
  GuLog("ACTIVE_APP=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1\n");
  s=EFI_UNSUPPORTED;goto failed;
#endif

  if (Gu.is_ampere) {

    GuLog("Ampere GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);

    GuLog("%s: GA102, volatile RAM-only attempt, no VBIOS flash\n",GU_FORK_VERSION);

  } else if (Gu.is_turing) {

    GuLog("Turing GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);

    GuLog("%s: TU10X assets not authenticated for this owner\n",GU_FORK_VERSION);

  } else {

    GuLog("Ada Lovelace GPU detected: Device ID 0x%04x\n", (unsigned)Gu.device_id);

    GuLog("%s: AD10X, volatile RAM-only attempt, no VBIOS flash\n",GU_FORK_VERSION);

  }

  stage="assets";s=GuBootLoadAssets();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="read-only-diagnostic";(VOID)GuDiagnosticScan();
  if(Gu.fault){s=EFI_DEVICE_ERROR;goto failed;}
  stage="read-only-cold-profile";s=GuAd104Prepare();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="stage-windows-image";s=GuBootLoadWindows();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="manual-choice";s=GuBootChoose(&choice);if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  if (choice == L'I') {

    GuLog("Operator I: Requesting DXE Driver NVRAM installation\n");

    s = GuInstallDriverOption(image, t);

    if (s == EFI_SUCCESS) {

      GuLog("DXE Driver registered in an available Driver#### slot / DriverOrder\n");

      GuBootSay(L"\r\n\r\n[OK] AdaUnlock DXE Driver registered in UEFI NVRAM.\r\n"

                L"Driver starts on subsequent boots while this USB device stays connected.\r\n"

                L"Rebooting system in 3 seconds...\r\n");

    } else {

      GuLog("ERROR: Failed to install DXE Driver: %#llx\n", (unsigned long long)s);

      GuBootSay(L"\r\n\r\n[ERROR] Failed to install DXE Driver to NVRAM!\r\n"

                L"Rebooting in 5 seconds...\r\n");

      if (t->BootServices->Stall) t->BootServices->Stall(2000000);

    }

    (VOID)GuBootClose();

    if (t->BootServices->Stall) t->BootServices->Stall(3000000);

    if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {

      t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    }

    return s;

  }

  if (choice == L'R') {

    GuLog("Operator R: Requesting DXE Driver NVRAM removal\n");

    s = GuRemoveDriverOption(t);

    GuLog("DXE Driver removal from owned Driver#### entries (status=%#llx)\n", (unsigned long long)s);

    GuBootSay(s == EFI_SUCCESS ? L"\r\n\r\n[OK] Owned AdaUnlock NVRAM entry removed.\r\n" :
              s == EFI_NOT_FOUND ? L"\r\n\r\n[INFO] No owned AdaUnlock NVRAM entry exists.\r\n" :
              L"\r\n\r\n[ERROR] NVRAM removal failed; review USB log.\r\n");

    GuBootSay(L"Rebooting system in 3 seconds...\r\n");

    (VOID)GuBootClose();

    if (t->BootServices->Stall) t->BootServices->Stall(3000000);

    if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {

      t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    }

    return s;

  }

  BOOLEAN unlock = (choice == L'U' || choice == L'P');

  BOOLEAN enable_s3 = (choice == L'P');

  if(!unlock) {

    GuLog("Operator skipped: no GPU transaction, no driver release; stock Windows\n");

    if(Gu.fault||GuBootClose()!=EFI_SUCCESS)return EFI_DEVICE_ERROR;

    return GuBootWindows();

  }

  GuBootSay(
    L"\r\nSelected GPU preparation passed. Driver release and firmware work will now begin.\r\n");
  GuLog("Operator U/P: one selected physical attempt; no automatic retry\n");
  GuBootSilence();
  stage="selected-gpu-execution";
  s=GuExecuteUnlock(t,image,&stage);
  if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  /* The final pre-Windows consistency check has not completed yet. Do not announce success. */

  /* Probe and register S3 Boot Script for standby persistence if operator chose [P] */

  if(enable_s3) {
    EFI_STATUS s3=GuRegisterS3(t);
    GuLog("S3 persistence requested: status=%#llx\n",(unsigned long long)s3);
  } else {
    GuLog("S3 persistence skipped by operator\n");
  }

  stage="final-Windows-verification";if(!GuMsiReadyForWindows()){s=EFI_NOT_READY;goto failed;}

  GuLog("PREBOOT OK: final environment checks passed; Windows startup and CUDA throughput remain unverified\n");
  GuBootUnsilence();
  GuBootSay(
    L" [3/4] GPU Driver/Display Handover ......... [ OK ]\r\n"
    L" [4/4] Hardware SEC2 and MSR Unlock ........ [ OK ]\r\n"
    L" Launching Windows Boot Manager; post-boot benchmark required.\r\n\r\n"
  );

  stage="close-USB-log";s=GuBootClose();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

  stage="start-Windows";s=GuBootWindows();

  /* A returned Windows image is not a successful handoff. */

  GuBootHalt();return s==EFI_SUCCESS?EFI_ABORTED:s;

failed:

  GuBootUnsilence();
  if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;

  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",stage,(unsigned long long)s,GuMsiMutated(),Gu.retained,Gu.fault);

  if(GuMsiMutated()||Gu.started||Gu.retained) {

    /* Fault-independent containment, no freeing of unverified live mappings. */

    if(Gu.started||Gu.master_attributes_pending)(VOID)GuClearMaster();

    (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;

  }

  GuBootSay(L"\r\nPreflight refused. No driver release or GPU transaction. Log saved to USB.\r\nReturning to Boot Menu in 3 seconds...\r\n");



  (VOID)GuBootClose();



  if(t&&t->BootServices&&t->BootServices->Stall)t->BootServices->Stall(3000000);



  return s;

}

