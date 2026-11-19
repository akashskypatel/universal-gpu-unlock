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
#include "boot_state.h"

#ifdef GU_RUNTIME_SELECTION
#include "gpu_selection.h"
#endif

static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};

#ifdef GU_RUNTIME_SELECTION
/* The image carries no compiled target list, so it has to acquire the
 * operator's policy on a machine it has never seen. One card is recorded
 * automatically; several are listed and chosen here; no answer leaves the
 * policy unset and discovery refuses instead of guessing. Writing the variable
 * is the only NVRAM side effect, and it happens before any device is bound. */
static EFI_STATUS ResolveTargetsOnConsole(EFI_SYSTEM_TABLE *t) {
  if(!t||!t->BootServices)return EFI_INVALID_PARAMETER;
  GU_SELECTION selection;
  EFI_STATUS s=GuSelectionRead(t,&selection);
  if(s==EFI_SUCCESS) {
    GuLog("ACTIVE_SELECTION=PRESENT COUNT=%zu ALL=%u SOURCE=VARIABLE\n",
          selection.count,(unsigned)selection.all);
    return EFI_SUCCESS;
  }
  if(s!=EFI_NOT_FOUND) {
    GuLog("ACTIVE_SELECTION=UNREADABLE STATUS=%#llx\n",(unsigned long long)s);
    return s;
  }
  GU_GPU_PREPARED_SEQUENCE prepared;
  s=GuPrepareGpuSequence(t,NULL,0,&prepared);
  if(s!=EFI_SUCCESS) {
    GuLog("ACTIVE_SELECTION=NO_INVENTORY STATUS=%#llx\n",(unsigned long long)s);
    return s;
  }
  if(!prepared.inventory.count) {
    GuLog("ACTIVE_SELECTION=NONE_PRESENT NVIDIA_COUNT=0\n");
    return EFI_NOT_FOUND;
  }
  memset(&selection,0,sizeof(selection));
  if(prepared.inventory.count==1) {
    const GU_GPU_RECORD *record=&prepared.inventory.nvidia[0];
    selection.list[0].segment=(UINT16)record->segment;
    selection.list[0].bus=(UINT8)record->bus;
    selection.list[0].device=(UINT8)record->device;
    selection.list[0].function=(UINT8)record->function;
    selection.count=1;
    s=GuSelectionWrite(t,&selection);
    GuLog("ACTIVE_SELECTION=AUTO_SINGLE BDF=%04x:%02x:%02x.%x STATUS=%#llx\n",
          (unsigned)record->segment,(unsigned)record->bus,
          (unsigned)record->device,(unsigned)record->function,
          (unsigned long long)s);
    GuBootSay(L"\r\nOne NVIDIA GPU found; recording it as the target.\r\n");
    return s;
  }
  /* Bounded display: the console offers at most nine entries. A larger
   * machine sets the variable directly, which this path only reads. */
  size_t shown=prepared.inventory.count<9?prepared.inventory.count:9;
  GuLog("ACTIVE_SELECTION=CHOICE_NEEDED NVIDIA_COUNT=%zu SHOWN=%zu\n",
        prepared.inventory.count,shown);
  for(size_t i=0;i<shown;i++) {
    const GU_GPU_RECORD *record=&prepared.inventory.nvidia[i];
    GuLog("ACTIVE_GPU[%zu] CHOICE=%zu BDF=%04x:%02x:%02x.%x ID=%04x:%04x\n",i,i+1,
          (unsigned)record->segment,(unsigned)record->bus,
          (unsigned)record->device,(unsigned)record->function,
          (unsigned)(record->pci_id&0xffffU),(unsigned)(record->pci_id>>16));
  }
  GuBootSay(L"\r\nMultiple NVIDIA GPUs found.\r\n"
            L"Press 1-9 to select a target, A for all supported cards, "
            L"or S to skip (no card is touched).\r\n");
  if(!t->ConIn||!t->ConIn->ReadKeyStroke)return EFI_NOT_FOUND;
  for(unsigned attempt=0;attempt<600;attempt++) {
    EFI_INPUT_KEY key;
    if(t->ConIn->ReadKeyStroke(t->ConIn,&key)==EFI_SUCCESS) {
      CHAR16 c=key.UnicodeChar;
      if(c=='s'||c=='S'||key.ScanCode==0x17) {   /* S or ESC */
        GuLog("ACTIVE_SELECTION=SKIPPED BY_OPERATOR=1\n");
        return EFI_NOT_FOUND;
      }
      if(c=='a'||c=='A') {
        selection.all=TRUE;
        s=GuSelectionWrite(t,&selection);
        GuLog("ACTIVE_SELECTION=POLICY_ALL BY_OPERATOR=1 STATUS=%#llx\n",
              (unsigned long long)s);
        return s;
      }
      if(c>='1'&&c<=(CHAR16)('0'+shown)) {
        const GU_GPU_RECORD *record=&prepared.inventory.nvidia[c-'1'];
        selection.list[0].segment=(UINT16)record->segment;
        selection.list[0].bus=(UINT8)record->bus;
        selection.list[0].device=(UINT8)record->device;
        selection.list[0].function=(UINT8)record->function;
        selection.count=1;
        s=GuSelectionWrite(t,&selection);
        GuLog("ACTIVE_SELECTION=CHOSEN BDF=%04x:%02x:%02x.%x STATUS=%#llx\n",
              (unsigned)record->segment,(unsigned)record->bus,
              (unsigned)record->device,(unsigned)record->function,
              (unsigned long long)s);
        return s;
      }
    }
    t->BootServices->Stall(10000);
  }
  GuLog("ACTIVE_SELECTION=NO_ANSWER TIMEOUT=1\n");
  return EFI_NOT_FOUND;
}
#endif

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

  /* boot_io reported the previous boot's record when it opened; this read only
   * carries the boot counter forward. It never changes the path. */
  GU_BOOT_STATE boot_state;BOOLEAN boot_state_present=FALSE;
  boot_state_present=(GuBootStateRead(t,&boot_state)==EFI_SUCCESS);

  const char *stage="discover";CHAR16 choice=L'S';

#ifdef GU_RUNTIME_SELECTION
  /* A selection that cannot be resolved is not fatal here: discovery refuses
   * it and reports why, which is the behaviour the refusal tests pin. */
  s=ResolveTargetsOnConsole(t);
  if(s!=EFI_SUCCESS&&s!=EFI_NOT_FOUND) {stage="selection";goto failed;}
#endif
  s=GuDiscoverSingleGpu(image,t);if(s!=EFI_SUCCESS)goto failed;
#ifdef GU_ASSET_INSTALL_ONLY
  /* Staging build: copy the pinned bundle for the selected card to the Windows
   * ESP and stop. It never prepares, releases or mutates the GPU, and the
   * discovery above is only there to resolve which manifest applies. */
  GuLog("ACTIVE_APP=ASSET_INSTALL_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1\n");
  s=GuAssetInstallStage();
  if(s!=EFI_SUCCESS)goto failed;
  stage="close-USB-log";s=GuBootClose();if(s!=EFI_SUCCESS)goto failed;
  GuLog("ASSET_INSTALL=DONE no driver release, no firmware, no reset\n");
  GuBootHalt();
  return EFI_ABORTED; /* never hand control to Windows from an installer */
#endif
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  /* Defense in depth: selected active preparation images never enter the
   * asset, driver-release, firmware, DMA or FLR stages even if discovery
   * behavior changes in the future. */
  GuLog("ACTIVE_APP=PREPARATION_ONLY ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1\n");
  s=EFI_UNSUPPORTED;goto failed;
#endif

  /* Pin the discovered owner through preparation and transaction entry. */
  GU_PLATFORM *owner=GuPlatformActive;
  if(!owner){s=EFI_NOT_READY;goto failed;}
  if (owner->is_ampere) {

    GuLog("Ampere GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);

    GuLog("%s: GA102, volatile RAM-only attempt, no VBIOS flash\n",GU_FORK_VERSION);

  } else if (owner->is_turing) {

    GuLog("Turing GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);

    GuLog("%s: TU10X assets not authenticated for this owner\n",GU_FORK_VERSION);

  } else {

    GuLog("Ada Lovelace GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);

    GuLog("%s: AD10X, volatile RAM-only attempt, no VBIOS flash\n",GU_FORK_VERSION);

  }

  s=GuPrepareBoundGpu(owner,&stage);if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;

  stage="stage-windows-image";s=GuBootLoadWindows();if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;

  stage="manual-choice";s=GuBootChoose(&choice);if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;

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

    if(owner!=GuPlatformActive||owner->fault||GuBootClose()!=EFI_SUCCESS||owner!=GuPlatformActive)return EFI_DEVICE_ERROR;

    return GuBootWindows();

  }

  GuBootSay(
    L"\r\nSelected GPU preparation passed. Driver release and firmware work will now begin.\r\n");
  GuLog("Operator U/P: ordered selected transactions; no automatic retry\n");
  GuBootSilence();
  /* Name the card before the first mutation so a power loss leaves evidence. */
  (VOID)GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_INTENT,0,owner->device_id,
                          (UINT16)owner->discovery_identity.segment,
                          (UINT16)owner->discovery_identity.bus,
                          (UINT8)owner->discovery_identity.device,
                          (UINT8)owner->discovery_identity.function,0);
  s=GuExecutePreparedSequence(owner,t,image,enable_s3?GU_S3_REQUIRED:GU_S3_DISABLED,&stage);
  (VOID)GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_OUTCOME,(UINT64)s,owner->device_id,
                          (UINT16)owner->discovery_identity.segment,
                          (UINT16)owner->discovery_identity.bus,
                          (UINT8)owner->discovery_identity.device,
                          (UINT8)owner->discovery_identity.function,
                          (UINT8)(s==EFI_SUCCESS&&owner->armed?1U:0U));
  if(s!=EFI_SUCCESS||Gu.fault)goto failed;

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

  if(GuExecutionNeedsContainment()) {

    /* Fault-independent containment, no freeing of unverified live mappings. */

    if((Gu.started&&Gu.owned)||Gu.master_attributes_pending)(VOID)GuClearMaster(&Gu);

    (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;

  }

  GuBootSay(L"\r\nPreflight refused. No driver release or GPU transaction. Log saved to USB.\r\nReturning to Boot Menu in 3 seconds...\r\n");



  (VOID)GuBootClose();



  if(t&&t->BootServices&&t->BootServices->Stall)t->BootServices->Stall(3000000);



  return s;

}

