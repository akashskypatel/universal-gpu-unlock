/* SPDX-License-Identifier: GPL-2.0-only */
/* Separate owner maintenance image. No GPU access or boot registration. */
#include "boot_state.h"
#include "format.h"
#include <stdarg.h>
#include <stdint.h>

static EFI_STATUS Say(EFI_SYSTEM_TABLE *t,const char *fmt,...) {
  char ascii[512];
  CHAR16 wide[512];
  va_list args;
  va_start(args,fmt);
  size_t n=GuFormatV(ascii,sizeof(ascii),fmt,args);
  va_end(args);
  if(n>=sizeof(ascii))return EFI_BAD_BUFFER_SIZE;
  for(size_t i=0;i<=n;i++)wide[i]=(CHAR16)(unsigned char)ascii[i];
  return t->ConOut->OutputString(t->ConOut,wide);
}

static EFI_STATUS Confirm(EFI_SYSTEM_TABLE *t) {
  static const CHAR16 expected[]=L"REARM\r";
  EFI_STATUS s=t->ConIn->Reset(t->ConIn,FALSE);
  if(s!=EFI_SUCCESS)return s;
  s=Say(t,"Preserve the log and photograph this record before continuing.\r\n"
          "Confirm separately authorized next experiment, stock recovery,\r\n"
          "and removal of all active experiment boot media.\r\n"
          "Type REARM and Enter to remove only the reviewed record.\r\n"
          "Any other input cancels. This does not accept a GPU result.\r\n");
  if(s!=EFI_SUCCESS)return s;
  EFI_EVENT event=t->ConIn->WaitForKey;
  for(size_t i=0;expected[i];i++) {
    UINTN index=0;
    s=t->BootServices->WaitForEvent(1,&event,&index);
    if(s!=EFI_SUCCESS)return s;
    if(index!=0)return EFI_DEVICE_ERROR;
    EFI_INPUT_KEY key={0};
    s=t->ConIn->ReadKeyStroke(t->ConIn,&key);
    if(s!=EFI_SUCCESS)return s;
    if(key.ScanCode!=0||key.UnicodeChar!=expected[i])return EFI_ABORTED;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  (void)image;
  if(!t||!t->ConOut||!t->ConOut->OutputString||!t->ConIn||
     !t->ConIn->Reset||!t->ConIn->ReadKeyStroke||!t->ConIn->WaitForKey||
     !t->BootServices||!t->BootServices->WaitForEvent||
     !t->BootServices->SetWatchdogTimer)return EFI_UNSUPPORTED;
  EFI_STATUS s=Say(t,"\r\nBoot state maintenance only. No GPU transaction.\r\n");
  if(s!=EFI_SUCCESS)return s;
  GU_BOOT_STATE reviewed;
  s=GuBootStateRead(t,&reviewed);
  if(s!=EFI_SUCCESS) {
    (VOID)Say(t,"REACTIVATION=REFUSED READ_STATUS=%#llx\r\n",(unsigned long long)s);
    return s;
  }
  if(!GuBootStateRequiresRecovery(&reviewed)) {
    (VOID)Say(t,"REACTIVATION=REFUSED REASON=NOT_LATCHED\r\n");
    return EFI_NOT_READY;
  }
  char raw[sizeof(reviewed)*2+1];
  const unsigned char *bytes=(const unsigned char *)&reviewed;
  static const char hex[]="0123456789abcdef";
  for(size_t i=0;i<sizeof(reviewed);i++) {
    raw[2*i]=hex[bytes[i]>>4];raw[2*i+1]=hex[bytes[i]&15];
  }
  raw[sizeof(raw)-1]=0;
  s=Say(t,"Variable: GuBootState (2a7d54c1-5b3e-4a19-8f60-9d21c4b7e803)\r\n"
          "Phase=%u BootCount=%u Status=%#llx Armed=%u\r\n"
          "Device=%04x Address=%04x:%02x:%02x.%x\r\n"
          "Reviewed record bytes: %s\r\n"
          "DriverOrder must be absent. No boot entries will be changed.\r\n",
        (unsigned)reviewed.Phase,(unsigned)reviewed.BootCount,
        (unsigned long long)reviewed.Status,(unsigned)reviewed.Armed,
        (unsigned)reviewed.DeviceId,(unsigned)reviewed.Segment,
        (unsigned)reviewed.Bus,(unsigned)reviewed.Device,
        (unsigned)reviewed.Function,raw);
  if(s!=EFI_SUCCESS)return s;
  s=Confirm(t);
  if(s!=EFI_SUCCESS) {
    (VOID)Say(t,"REACTIVATION=CANCELLED RECORD_UNCHANGED=1\r\n");
    return s;
  }
  /* Do not let the application watchdog cause an automatic reboot once
   * deletion is attempted. If cancellation fails, do not change NVRAM. */
  s=t->BootServices->SetWatchdogTimer(0,0,0,NULL);
  if(s!=EFI_SUCCESS)return s;
  s=GuBootStateDeleteReviewed(t,&reviewed);
  if(s==EFI_SUCCESS)
    (VOID)Say(t,"REACTIVATION=RECORD_REMOVAL_VERIFIED ACCEPTANCE=NOT_EVALUATED\r\n");
  else
    (VOID)Say(t,"REACTIVATION=REFUSED_OR_UNCONFIRMED STATUS=%#llx\r\n",
              (unsigned long long)s);
  (VOID)Say(t,"STOPPED. Power off. Remove maintenance media.\r\n"
              "No GPU transaction, boot entry change or reset was requested.\r\n");
  /* Never continue to an automatically selected image after a deletion
   * attempt, including when readback is uncertain. */
  for(;;) {}
}
