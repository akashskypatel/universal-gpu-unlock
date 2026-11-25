/* SPDX-License-Identifier: GPL-2.0-only */

#include "port.h"
#include "version.h"

#include "checks.h"
#include "gpu_discovery.h"

#include "profile.h"
#include "prom_reference.h"

#include "msi_release.h"
#include "unlock_execution.h"

#include "boot_io.h"

#if defined(GU_STOP_RELEASE_WINDOWS_HANDOFF) && (!defined(GU_STOP_RELEASE_ONLY_BENCH) || !defined(GU_RUNTIME_SELECTION) || !defined(GU_RUNTIME_ESP) || !defined(GU_DXE_FILE_ONLY_OUTPUT))
#error Stop-only Windows handover requires runtime selection, runtime ESP discovery and file-only Stop scope
#endif

#if defined(GU_SELECT_ALL_GPUS) && (!defined(GU_FIRMWARE_REATTACH) || !defined(GU_RUNTIME_SELECTION))
#error Automatic all-GPU selection requires combined runtime firmware reattachment
#endif

#if defined(GU_APP_OPERATION)
#if !defined(GU_ACTIVE_UNVERIFIED_ACTUATION) || defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_SYNTHETIC_COMPILE_ONLY) || defined(GU_ASSET_INSTALL_ONLY)
#error A fixed application operation requires the ordinary explicitly waived application
#endif
#if GU_APP_OPERATION != 'U' && GU_APP_OPERATION != 'P' && GU_APP_OPERATION != 'I' && GU_APP_OPERATION != 'R'
#error Invalid fixed application operation
#endif
#endif

#include "compat/compat.h"

#ifndef GU_STOP_RELEASE_ONLY_BENCH
#include "vendor/unlock_610.h"
#include "s3_save.h"
#endif

#include "driver_nvram.h"
#include "boot_state.h"

#ifdef GU_RUNTIME_SELECTION
#include "gpu_selection.h"
#endif

static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};

#ifdef GU_RUNTIME_SELECTION
#ifndef GU_STOP_RELEASE_ONLY_BENCH
/* The logged enumeration order can change on the next boot. Display the
 * exact live GPU identity before accepting a numeric choice. */
static EFI_STATUS ShowTargetChoice(EFI_SYSTEM_TABLE *t,size_t choice,
                                   const GU_GPU_RECORD *record) {
  if(!t||!t->ConOut||!t->ConOut->OutputString||!record||
     choice<1U||choice>9U||record->segment>0xffffU||record->bus>255U||
     record->device>31U||record->function>7U)return EFI_INVALID_PARAMETER;
  CHAR16 row[]=L"0: 0000:00:00.0 PCI=0000:0000\r\n";
  static const CHAR16 hex[]=L"0123456789abcdef";
  row[0]=(CHAR16)('0'+choice);
  for(UINTN i=0;i<4U;i++) {
    row[6U-i]=hex[(record->segment>>(4U*i))&15U];
    row[23U-i]=hex[(record->pci_id>>(4U*i))&15U];
    row[28U-i]=hex[(record->pci_id>>(16U+4U*i))&15U];
  }
  row[8]=hex[(record->bus>>4U)&15U];row[9]=hex[record->bus&15U];
  row[11]=hex[(record->device>>4U)&15U];row[12]=hex[record->device&15U];
  row[14]=hex[record->function];
  return t->ConOut->OutputString(t->ConOut,row);
}
#endif
/* The image carries no compiled target list. The Stop/release-only profile
 * sets an in-memory select-all policy before discovery; ordinary runtime
 * selection profiles continue to acquire operator policy through GuTargets. */
static EFI_STATUS ResolveTargetsOnConsole(EFI_SYSTEM_TABLE *t) {
  if(!t||!t->BootServices)return EFI_INVALID_PARAMETER;
#ifdef GU_SELECT_ALL_GPUS
  if(!GuBootLogOpen())return EFI_DEVICE_ERROR;
  EFI_STATUS automatic=GuRuntimeSelectionSelectAll();
  GuLog("ACTIVE_SELECTION=POLICY_ALL SOURCE=BUILD_OPTION STATUS=%#llx\n",
        (unsigned long long)automatic);
  return automatic!=EFI_SUCCESS?automatic:(GuBootLogOpen()?EFI_SUCCESS:EFI_DEVICE_ERROR);
#endif
#ifdef GU_STOP_RELEASE_ONLY_BENCH
  /* Select the supported NVIDIA devices found by the discovery pass that
   * follows. This is an in-memory policy; do not persist a stale BDF or ALL. */
  return GuRuntimeSelectionSelectAll();
#else
  GU_SELECTION selection;
  EFI_STATUS s;
#ifndef GU_FIRMWARE_REATTACH
  s=GuSelectionRead(t,&selection);
  if(s==EFI_SUCCESS) {
    GuLog("ACTIVE_SELECTION=PRESENT COUNT=%zu ALL=%u SOURCE=VARIABLE\n",
          selection.count,(unsigned)selection.all);
    return EFI_SUCCESS;
  }
  if(s!=EFI_NOT_FOUND) {
    GuLog("ACTIVE_SELECTION=UNREADABLE STATUS=%#llx\n",(unsigned long long)s);
    return s;
  }
#endif
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
#ifndef GU_FIRMWARE_REATTACH
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
#endif
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
    s=ShowTargetChoice(t,i+1U,record);
    if(s!=EFI_SUCCESS||!GuBootLogOpen()) {
      GuLog("ACTIVE_SELECTION=DISPLAY_REFUSED STATUS=%#llx\n",(unsigned long long)s);
      return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
    }
  }
  GuBootSay(L"\r\nSelect GPUs for this boot.\r\n"
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
#ifdef GU_FIRMWARE_REATTACH
        s=GuRuntimeSelectionSelectAll();
#else
        s=GuSelectionWrite(t,&selection);
#endif
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
#ifdef GU_FIRMWARE_REATTACH
        s=GuRuntimeSelectionChoose(&selection.list[0]);
#else
        s=GuSelectionWrite(t,&selection);
#endif
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
#endif /* !GU_STOP_RELEASE_ONLY_BENCH */
}
#endif

/* The outcomes an operator can be told apart: a missing resident driver file, a
 * full Driver table, a platform that will not take the variable, and everything
 * else. driver_nvram.c logs the file it opened beside this. */
static const char *DxeInstallReason(EFI_STATUS s) {
  if(s==EFI_NOT_FOUND)return "DRIVER_FILE_ABSENT";
  if(s==EFI_OUT_OF_RESOURCES)return "NO_FREE_DRIVER_SLOT";
  if(s==EFI_UNSUPPORTED)return "PLATFORM_REFUSED";
  if(s==EFI_ACCESS_DENIED)return "OWNED_SLOT_LOCKED";
  if(s==EFI_COMPROMISED_DATA)return "NVRAM_ENTRY_INVALID";
  return "UNEXPECTED";
}

#ifdef GU_APP_OPERATION
/* A fixed I/R image must not reboot into itself. Keep the existing operation
 * result, require a checked log close, then stop for manual boot selection. */
static EFI_STATUS FinishBuildSelectedDriverOperation(EFI_STATUS status) {
  GuLog("BUILD_SELECTED_DRIVER_OPERATION=RETURNED STATUS=%#llx AUTO_RESET=0\n",
        (unsigned long long)status);
  EFI_STATUS closed=GuBootClose();
  if(status==EFI_SUCCESS&&closed!=EFI_SUCCESS)status=closed;
  GuBootSay(L"\r\nAdministrative operation returned. Review the log. "
            L"Power off and select the intended next boot manually.\r\n");
  GuBootHalt();
  return status;
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

  /* Read again after GuBootOpen. The earlier log observation is not admission. */
  GU_BOOT_STATE boot_state;BOOLEAN boot_state_present=FALSE;
  s=GuBootReadPriorState(t,&boot_state,&boot_state_present);
  if(s!=EFI_SUCCESS)return s;

#if !defined(GU_STOP_RELEASE_ONLY_BENCH) && !defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
  if(GuBootStateRequiresRecovery(boot_state_present?&boot_state:NULL)) {
    /* Remove only the owned DXE option and preserve the record. */
    GuLog("USB_RECOVERY=FAILED_OR_INTERRUPTED ACTION=REMOVE_DXE_AND_SKIP_GPU\n");
    GuBootSay(L"\r\n[RECOVERY] An earlier GPU/DXE attempt failed or awaits owner acceptance.\r\n"
              L"Removing the AdaUnlock DXE boot entry; no GPU will be touched.\r\n");
    EFI_STATUS load=GuBootLoadWindows();
    EFI_STATUS remove=GuRemoveDriverOption(t);
    if(remove==EFI_NOT_FOUND)remove=EFI_SUCCESS;
    GuLog("USB_RECOVERY DRIVER_REMOVE=%#llx WINDOWS_LOAD=%#llx\n",
          (unsigned long long)remove,(unsigned long long)load);
    if(remove==EFI_SUCCESS) {
      GuLog("USB_RECOVERY BOOT_STATE_PRESERVED=1 AUTO_RETRY=0\n");
      GuBootSay(L"[RECOVERY] DXE entry removed. Failure record preserved. Continuing without GPU changes.\r\n");
    } else {
      GuBootSay(L"[ERROR] Could not remove the DXE boot entry. See USB log.\r\n");
    }
    EFI_STATUS closed=GuBootClose();
    if(remove!=EFI_SUCCESS||closed!=EFI_SUCCESS||load!=EFI_SUCCESS) {
      return remove!=EFI_SUCCESS?remove:(closed!=EFI_SUCCESS?closed:load);
    }
    return GuBootWindows();
  }
#endif

#ifdef GU_STOP_RELEASE_ONLY_BENCH
  GuLog("STOP_ONLY_SCOPE=DRIVER_STOP_REATTACH FIRMWARE_ASSET_FILES_READ=0 "
        "PROM_READ=0 PROM_SIGNATURE_CHECK=0 FIRMWARE_TRANSACTION=0\n");
#else
  /* Reference-table metadata only; normal preparation performs the actual
   * PROM and asset qualification below. */
  GuLog("PROM_REFERENCE=%s ROWS=%u AUTHENTICATION=0\n",
        GuPromReferenceMode(),(unsigned)GuPromReferenceCount());
#endif

  const char *stage="discover";
#ifndef GU_STOP_RELEASE_ONLY_BENCH
  CHAR16 choice=L'S';
#endif

#ifdef GU_RUNTIME_SELECTION
  /* A selection that cannot be resolved is not fatal here: discovery refuses
   * it and reports why, which is the behaviour the refusal tests pin. */
  s=ResolveTargetsOnConsole(t);
#ifdef GU_FIRMWARE_REATTACH
  if(s!=EFI_SUCCESS) {stage="selection";GuTracePhase(stage);goto failed;}
  if(s==EFI_SUCCESS)
    GuBootSay(L"\r\n[OK] Target selection accepted. Checking GPUs and loading firmware assets.\r\n"
              L"USB reads may take several minutes. Do not restart or remove the USB.\r\n"
              L"A separate warning will appear before each GPU driver release.\r\n");
#else
  if(s!=EFI_SUCCESS&&s!=EFI_NOT_FOUND) {stage="selection";GuTracePhase(stage);goto failed;}
#endif
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
#ifdef GU_INTERACTIVE_WINDOWS_ESP
  /* Resolve the boot destination before firmware asset fallback or any
   * actuation. Keep the loaded image and boot-local choice for handover. */
  stage="stage-windows-image";GuTracePhase(stage);
  s=GuBootLoadWindows();
  if(s!=EFI_SUCCESS)goto failed;
  if(owner!=GuPlatformActive||owner->fault||!GuBootLogOpen()){s=EFI_NOT_READY;goto failed;}
  GuBootSay(L"\r\nWindows boot destination accepted. Preparing GPU firmware assets.\r\n");
#endif
  if (owner->is_ampere) {
    GuLog("Ampere GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);
#ifndef GU_STOP_RELEASE_ONLY_BENCH
    GuLog("%s: GA102, volatile RAM-only attempt, no VBIOS flash\n",GU_FORK_VERSION);
#endif
  } else if (owner->is_turing) {
    GuLog("Turing GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);
#ifndef GU_STOP_RELEASE_ONLY_BENCH
    GuLog("%s: TU10X assets not authenticated for this owner\n",GU_FORK_VERSION);
#endif
  } else {
    GuLog("Ada Lovelace GPU detected: Device ID 0x%04x\n", (unsigned)owner->device_id);
#ifndef GU_STOP_RELEASE_ONLY_BENCH
    GuLog("%s: AD10X, volatile RAM-only attempt, no VBIOS flash\n",GU_FORK_VERSION);
#endif
  }

  /* Keep the watchdog state visible before the next stage. Stop/release-only
   * builds do not load firmware assets or read the PROM here. */
  {
    UINTN bound=0;
    if(GuBootWatchdogArmed(&bound))
      GuLog("WATCHDOG BOUND=%u SECONDS ACTIVE=1 PHASE=pre-prepare\n",(unsigned)bound);
    else
      GuLog("WATCHDOG BOUND=0 SECONDS ACTIVE=0 REASON=NO_BUILD_THRESHOLD PHASE=pre-prepare\n");
  }
  GuTracePhase("prepare");
  s=GuPrepareBoundGpu(owner,&stage);if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
  GuTraceDetail("prepare","STATUS=%#llx PROFILE_VERIFIED=%u PROM_VERIFIED=%u",
                (unsigned long long)s,(unsigned)owner->profile_verified,
                (unsigned)owner->prom_verified);

#ifdef GU_STOP_RELEASE_ONLY_BENCH
#ifdef GU_RUNTIME_SELECTION
  GuLog("STOP_RELEASE_ONLY_IMAGE=1 ORDER=LIVE_SUPPORTED_NVIDIA_ENUMERATION ");
#else
  GuLog("STOP_RELEASE_ONLY_IMAGE=1 ORDER=COMPILED_BDF_SEQUENCE ");
#endif
  GuLog("ASPM_ADMISSION=BYPASSED INDEPENDENT_SURVIVOR_DISPLAY_REQUIRED=CONDITIONAL "
        "FIRMWARE_TRANSACTION=0 UNLOCK=0 FIRMWARE_ASSET_FILES_READ=0 PROM_READ=0 "
#ifdef GU_STOP_RELEASE_WINDOWS_HANDOFF
        "OS_HANDOFF=WINDOWS_AFTER_SUCCESS HARDWARE_VERIFIED=0\n");
#else
        "OS_HANDOFF=NONE HARDWARE_VERIFIED=0\n");
#endif
  GuBootSilence();
  s=GuExecuteStopReleaseSequence(owner,t,image,&stage);
  if(s!=EFI_SUCCESS)goto failed;
  /* A successful sequence leaves the last GPU bound, not the first. */
  owner=GuPlatformActive;
  if(!owner||owner->fault){s=EFI_NOT_READY;goto failed;}
  s=GuBootWatchdogCancel();if(s!=EFI_SUCCESS)goto failed;
  GuLog("STOP_RELEASE_ONLY=COMPLETE FIRMWARE_TRANSACTION=0 UNLOCK=0 "
#ifdef GU_STOP_RELEASE_WINDOWS_HANDOFF
        "REATTACH_VERIFIED=1 OS_HANDOFF=WINDOWS_AFTER_SUCCESS\n");
#else
        "REATTACH_VERIFIED=1 OS_HANDOFF=NONE\n");
#endif
  GuBootUnsilence();
  s=GuBootBenchReportCompletion();
  if(s!=EFI_SUCCESS)
    GuLog("BENCH_COMPLETION_DISPLAY=FAILED STATUS=%#llx GPU_SEQUENCE=COMPLETE\n",
          (unsigned long long)s);
#ifdef GU_STOP_RELEASE_WINDOWS_HANDOFF
  stage="stage-stop-only-windows";GuTracePhase(stage);
  s=GuBootLoadWindows();
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
  /* Loading the image is not starting it. Verify the complete sequence
   * again after staging Windows and after the completion display. */
  stage="verify-stop-only-windows";GuTracePhase(stage);
  s=GuVerifySelectedSequence(t);
  if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
  GuLog("STOP_RELEASE_WINDOWS_HANDOFF=READY FINAL_GRAPH_VERIFIED=1 "
        "FIRMWARE_TRANSACTION=0 WINDOWS_START_ATTEMPTED=0 WINDOWS_BOOT_VERIFIED=0\n");
#endif
  stage="close-USB-log";GuTracePhase(stage);
  s=GuBootClose();if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
#ifdef GU_STOP_RELEASE_WINDOWS_HANDOFF
  s=GuBootWindows();
  /* A returned Windows image is not a successful handover. */
  GuBootHalt();return s==EFI_SUCCESS?EFI_ABORTED:s;
#else
  return EFI_SUCCESS;
#endif
#else
#ifndef GU_INTERACTIVE_WINDOWS_ESP
  stage="stage-windows-image";GuTracePhase(stage);s=GuBootLoadWindows();if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
#endif

#ifdef GU_APP_OPERATION
  stage="build-selected-operation";GuTracePhase(stage);
  choice=(CHAR16)GU_APP_OPERATION;
  if(owner!=GuPlatformActive||owner->fault){s=EFI_NOT_READY;goto failed;}
  GuLog("APP_OPERATION_SOURCE=BUILD KEY=%c MENU=0\n",(char)choice);
  if(choice==L'I'||choice==L'R') {
    /* These profiles halt instead of resetting. Do not leave a watchdog
     * armed or start an administrative operation without a healthy log. */
    s=GuBootWatchdogCancel();if(s!=EFI_SUCCESS)goto failed;
    if(!GuBootLogOpen()){s=EFI_DEVICE_ERROR;goto failed;}
  }
#else
  stage="manual-choice";GuTracePhase(stage);s=GuBootChoose(&choice);if(s!=EFI_SUCCESS||owner!=GuPlatformActive||owner->fault)goto failed;
#endif

  if (choice == L'I') {

    GuLog("Operator I: Requesting DXE Driver NVRAM installation\n");

    s = GuInstallDriverOption(image, t);

    if (s == EFI_SUCCESS) {

      GuLog("DXE Driver registered in an available Driver#### slot / DriverOrder\n");

      GuBootSay(L"\r\n\r\n[OK] AdaUnlock DXE Driver registered in UEFI NVRAM.\r\n"

                L"Driver starts on subsequent boots while this USB device stays connected.\r\n"
#ifndef GU_APP_OPERATION
                L"Rebooting system in 3 seconds...\r\n"
#endif
                );

    } else {

      /* Name the reason, not only the status: RUN0014 answered this line with
       * `0x800000000000000e` and nothing an operator could act on. */
      GuLog("DXE_INSTALL=FAILED STATUS=%#llx REASON=%s\n",
            (unsigned long long)s,DxeInstallReason(s));

      GuBootSay(L"\r\n\r\n[ERROR] Failed to install DXE Driver to NVRAM!\r\n"
#ifndef GU_APP_OPERATION
                L"Rebooting in 5 seconds...\r\n"
#endif
                );
#ifndef GU_APP_OPERATION
      if (t->BootServices->Stall) t->BootServices->Stall(2000000);
#endif

    }

#ifdef GU_APP_OPERATION
    return FinishBuildSelectedDriverOperation(s);
#else
    (VOID)GuBootClose();

    if (t->BootServices->Stall) t->BootServices->Stall(3000000);

    if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {

      t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    }

    return s;
#endif

  }

  if (choice == L'R') {

    GuLog("Operator R: Requesting DXE Driver NVRAM removal\n");

    s = GuRemoveDriverOption(t);

    GuLog("DXE Driver removal from owned Driver#### entries (status=%#llx)\n", (unsigned long long)s);

    GuBootSay(s == EFI_SUCCESS ? L"\r\n\r\n[OK] Owned AdaUnlock NVRAM entry removed.\r\n" :
              s == EFI_NOT_FOUND ? L"\r\n\r\n[INFO] No owned AdaUnlock NVRAM entry exists.\r\n" :
              L"\r\n\r\n[ERROR] NVRAM removal failed; review USB log.\r\n");

#ifdef GU_APP_OPERATION
    return FinishBuildSelectedDriverOperation(s);
#else
    GuBootSay(L"Rebooting system in 3 seconds...\r\n");

    (VOID)GuBootClose();

    if (t->BootServices->Stall) t->BootServices->Stall(3000000);

    if (t->RuntimeServices && t->RuntimeServices->ResetSystem) {

      t->RuntimeServices->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);

    }

    return s;
#endif

  }

  /* Which key was pressed is the difference between a transaction and a stock
   * boot, and until now it appeared in no log at all. */
  GuTraceDetail(stage,"KEY=%c TRANSACTION=%u S3_SCRIPT=%u",(char)choice,
                (unsigned)(choice == L'U' || choice == L'P'),
                (unsigned)(choice == L'P'));
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
  /* GuBootOpen already armed the bounded timer for this build, so the bound
   * covers preparation and everything below. Say so once, so a boot log records
   * whether this run had one. */
  {
    UINTN bound=0;
    if(GuBootWatchdogArmed(&bound))
      GuLog("WATCHDOG BOUND=%u SECONDS ACTIVE=1\n",(unsigned)bound);
    else
      GuLog("WATCHDOG BOUND=0 SECONDS ACTIVE=0 REASON=NO_BUILD_THRESHOLD\n");
  }
  /* Name the card before the first mutation so a power loss leaves evidence. */
  s=GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_INTENT,0,owner->device_id,
                          (UINT16)owner->discovery_identity.segment,
                          (UINT16)owner->discovery_identity.bus,
                          (UINT8)owner->discovery_identity.device,
                          (UINT8)owner->discovery_identity.function,0);
  if(s!=EFI_SUCCESS||!GuBootLogOpen()) {
    if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    goto failed;
  }
  s=GuExecutePreparedSequence(owner,t,image,enable_s3?GU_S3_REQUIRED:GU_S3_DISABLED,&stage);
  s=GuCompletePreparedSequence(s,&owner);
  if(!owner||s!=EFI_SUCCESS||Gu.fault)goto failed;

  GuLog("PREBOOT OK: final environment checks passed; Windows startup and CUDA throughput remain unverified\n");
#ifdef GU_FIRMWARE_REATTACH
  if(!GuExecutionReadyForWindows(t)||!GuBootLogOpen()){s=EFI_NOT_READY;goto failed;}
#else
  GuBootUnsilence();
  GuBootSay(
    L" [3/4] GPU Driver/Display Handover ......... [ OK ]\r\n"
    L" [4/4] Hardware SEC2 and MSR Unlock ........ [ OK ]\r\n"
    L" Launching Windows Boot Manager; post-boot benchmark required.\r\n\r\n"
  );
#endif

#ifdef GU_ACTIVE_UNVERIFIED_ACTUATION
  /* The experimental attempt is one-use. Keep the original verified INTENT
   * through handover; neither StartImage nor a reset proves OS acceptance. */
  GuLog("EXPERIMENT_RESTART_BARRIER=RETAINED PHASE=INTENT OWNER_ACCEPTANCE=PENDING AUTO_RETRY=0\n");
#endif
  stage="close-USB-log";GuTracePhase(stage);s=GuBootClose();if(s!=EFI_SUCCESS||Gu.fault)goto failed;

#ifndef GU_ACTIVE_UNVERIFIED_ACTUATION
  /* This legacy outcome records preboot completion, not a successful OS boot. */
  stage="record-preboot-outcome";
  EFI_STATUS recorded=GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_OUTCOME,(UINT64)s,owner->device_id,
                          (UINT16)owner->discovery_identity.segment,
                          (UINT16)owner->discovery_identity.bus,
                          (UINT8)owner->discovery_identity.device,
                          (UINT8)owner->discovery_identity.function,
                          (UINT8)(s==EFI_SUCCESS&&owner->armed?1U:0U));
  if(s==EFI_SUCCESS&&recorded!=EFI_SUCCESS)s=recorded;
  if(s!=EFI_SUCCESS)goto failed;
#endif

  stage="start-Windows";GuTracePhase(stage);s=GuBootWindows();

  /* A returned Windows image is not a successful handoff. Re-establish
   * the failed boot barrier; never request a reset or retry here. */
  if(s==EFI_SUCCESS)s=EFI_ABORTED;
#ifdef GU_ACTIVE_UNVERIFIED_ACTUATION
  /* No state rewrite is needed, including when StartImage returns. */
  GuBootHalt();return s;
#else
  recorded=GuBootStateRecord(t,boot_state_present?&boot_state:NULL,
                          GU_BOOT_STATE_OUTCOME,(UINT64)s,owner->device_id,
                          (UINT16)owner->discovery_identity.segment,
                          (UINT16)owner->discovery_identity.bus,
                          (UINT8)owner->discovery_identity.device,
                          (UINT8)owner->discovery_identity.function,0);
  GuBootHalt();return recorded==EFI_SUCCESS?s:recorded;
#endif
#endif /* !GU_STOP_RELEASE_ONLY_BENCH */

failed:

#ifdef GU_FIRMWARE_REATTACH
  /* GuBootHalt calls GuBootSay. Do not re-enable a potentially stopped
   * firmware console on any contained combined failure. */
  if(GuExecutionNeedsContainment())GuBootSilence();
  else GuBootUnsilence();
#else
  GuBootUnsilence();
#endif
  if(s==EFI_SUCCESS)s=EFI_DEVICE_ERROR;

  GuLog("STOP: stage=%s status=%#llx mutated=%u retained=%u fault=%u\n",stage,(unsigned long long)s,GuMsiMutated(),Gu.retained,Gu.fault);
  /* A stopped run has to be readable from the machine, not only from the USB
   * file that has to be carried to another computer afterwards. */
  GuTraceStop(stage,(UINT64)s);

  if(GuExecutionNeedsContainment()) {

    /* Fault-independent containment, no freeing of unverified live mappings. */

    if((Gu.started&&Gu.owned)||Gu.master_attributes_pending)(VOID)GuClearMaster(&Gu);
#ifdef GU_FIRMWARE_REATTACH
    EFI_STATUS cancel_status=GuBootWatchdogCancel();
    GuLog("FIRMWARE_REATTACH_CONTAINMENT=HALT NEXT_GPU=BLOCKED AUTO_WARM_RESET=0 WATCHDOG_CANCEL=%#llx\n",
          (unsigned long long)cancel_status);
#endif
#ifdef GU_STOP_RELEASE_ONLY_BENCH
    if(GuStopReleaseReattachFailed()) {
      /* The application has already stopped the sequence. Never use
       * ConOut: it can still belong to the failed GPU. A validated peer GOP
       * must show the reboot and USB boot-loop notice before any reset.
       * A missing notice, log, delay or reset service must fail closed. */
      EFI_STATUS notice=GuMsiStopReleaseShowReattachFailureNotice();
      GuLog("STOP_RELEASE_REATTACH_FAILED_REBOOT_NOTICE STATUS=%#llx "
            "NEXT_GPU=BLOCKED REBOOT_DELAY_SECONDS=30 "
            "USB_FIRST_BOOT_MAY_LOOP=1 ENTER_BIOS_SELECT_OTHER_DRIVE=1\n",
            (unsigned long long)notice);
      if(notice!=EFI_SUCCESS||!t||!t->BootServices||
         !t->BootServices->Stall||!t->RuntimeServices||
         !t->RuntimeServices->ResetSystem) {
        GuLog("STOP_RELEASE_REBOOT=REFUSED "
              "REASON=NOTICE_OR_REBOOT_SERVICE_UNAVAILABLE "
              "CONTAINMENT=HALT\n");
        (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;
      }
      GuLog("STOP_RELEASE_REBOOT=ARMED TYPE=WARM AFTER_SECONDS=30 "
            "USB_BOOT_LOOP_RISK=1 BIOS_SELECT_OTHER_BOOT_DRIVE=1 "
            "FIRMWARE_TRANSACTION=0\n");
      /* Flush and close the USB evidence before firmware resets control. */
      EFI_STATUS closed=GuBootClose();
      if(closed!=EFI_SUCCESS){GuBootHalt();return EFI_ABORTED;}
      EFI_STATUS delay=t->BootServices->Stall(30000000U);
      if(delay!=EFI_SUCCESS){GuBootHalt();return EFI_ABORTED;}
      t->RuntimeServices->ResetSystem(EfiResetWarm,EFI_ABORTED,0,NULL);
      /* ResetSystem is not guaranteed to complete on damaged firmware. */
      GuBootHalt();return EFI_ABORTED;
    }
    /* An incomplete release without an attempted reattachment is not proof
     * that a warm reboot can safely restore the original GPU driver. */
    GuLog("STOP_RELEASE_CONTAINMENT=HALT NEXT_GPU=BLOCKED "
          "RESET=OPERATOR_CONTROLLED AUTO_WARM_RESET=0 "
          "REASON=UNVERIFIED_GPU_OR_CONSOLE_STATE\n");
#endif

    (VOID)GuBootClose();GuBootHalt();return EFI_ABORTED;

  }

  GuBootSay(L"\r\nPreflight refused. No driver release or GPU transaction. Log saved to USB.\r\nReturning to Boot Menu in 3 seconds...\r\n");



  (VOID)GuBootClose();



  if(t&&t->BootServices&&t->BootServices->Stall)t->BootServices->Stall(3000000);



  return s;

}

