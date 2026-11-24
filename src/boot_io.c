/* SPDX-License-Identifier: GPL-2.0-only */

/* USB log/assets, and the one configured Windows ESP as a pinned read-only
 * fallback. The exception is the explicit asset-install build
 * (GU_ASSET_INSTALL_ONLY), whose GuAssetInstallStage writes the verified bundle
 * to that ESP and records it as an NVRAM variable; nothing else writes there and
 * nothing here changes where the image may be launched from. */

#include "boot_io.h"
#include "launch_source.h"
#include "version.h"
#include "gpu_windows_path.h"
#ifdef GU_STOP_RELEASE_ONLY_BENCH
#include "gpu_discovery.h"
#include "gpu_selection.h"
#include "msi_release.h"
#endif

#include "boot_profile.h"
#include "boot_state.h"
#include "asset_install.h"

#include "compat/compat.h"

#include "format.h"

#include "profile.h"

#include "sha256.h"

#include <Protocol/LoadedImage.h>

#include <Protocol/SimpleFileSystem.h>
#if defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_FIRMWARE_REATTACH)
#include <Protocol/SimpleTextOut.h>
#endif

#include <Guid/FileInfo.h>



static EFI_SYSTEM_TABLE *Table;

/* Defined at the end of this file so the boot-file harnesses can splice the
 * close and read paths without dragging the timer in. */
#ifndef GU_WATCHDOG_SECONDS
#define GU_WATCHDOG_SECONDS 0
#endif
/* Anything beyond this is a typo, not a budget: firmware timers that long are
 * indistinguishable from "never", which is the failure this code exists to
 * avoid. */
#define GU_WATCHDOG_MAX_SECONDS 3600U

static EFI_STATUS WatchdogProgram(UINTN Bound);

static EFI_HANDLE Self,Windows,OwnVolume,TargetEspVolume;

/* Set once at open: the image was launched from removable media. The installer
 * refuses to stage firmware anywhere unless that holds. */
static BOOLEAN BootFromUsb;
/* The boot counter the record carried, so a staged bundle names its boot. */
static UINT32 OpenBootCount;

static EFI_FILE_PROTOCOL *Root,*Log;
static EFI_HANDLE *UnreleasedFilesystemHandles;
/* At most five live file references: USB root/log, retained match root,
 * current candidate root and candidate file. Stop opening on failure. */
static EFI_FILE_PROTOCOL *UnreleasedBootFiles[5];
static UINTN UnreleasedBootFileCount;
static EFI_STATUS BootFileStatus;
/* Windows staging owns at most two temporary buffers, independent of GPU selection.
 * Retain uncertain addresses for diagnosis only. Never dereference or retry them. */
static VOID *UnreleasedBootBuffers[2];
static UINTN UnreleasedBootBufferCount;
static CHAR16 ActiveLogPath[]=L"\\AD102-UNLOCK\\RUN0000.TXT";

static BOOLEAN Silent;
/* The platform owns the asset table and its one-attempt latch. */

static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,InfoGuid=EFI_FILE_INFO_ID;

#ifndef GU_RUNTIME_ESP
static const EFI_GUID WindowsPartition=GU_WINDOWS_PARTITION_INITIALIZER;
#endif

static const CHAR16 WindowsName[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

static const CHAR16 * const WindowsCandidates[] = {
  L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
  L"EFI\\Microsoft\\Boot\\bootmgfw.efi"
};

static UINTN GuStrLen(const CHAR16 *s) {
  UINTN len = 0;
  while (s && s[len]) len++;
  return len;
}



static BOOLEAN Inspect(const EFI_DEVICE_PATH_PROTOCOL *p,UINTN *size,BOOLEAN *usb,BOOLEAN *windows) {
  UINTN at=0;*usb=FALSE;*windows=FALSE;
  if(!p)return FALSE;
  for(UINTN nodes=0;nodes<64&&at+4<=4096;nodes++) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>4096-at)return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE) {
      if(n!=4||p->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE)return FALSE;
      *size=at+4;return TRUE;
    }
    if(p->Type==MESSAGING_DEVICE_PATH&&p->SubType==MSG_USB_DP&&n>=sizeof(USB_DEVICE_PATH))*usb=TRUE;
    if(p->Type==MEDIA_DEVICE_PATH) {
      *windows=TRUE;
    }
    at+=n;p=(const VOID *)((const UINT8 *)p+n);
  }
  return FALSE;
}

VOID GuBootSay(const CHAR16 *text) {
#ifdef GU_DXE_FILE_ONLY_OUTPUT
  /* The installed experimental DXE has no menu. Its status remains in GuLog;
   * screen output is intentionally suppressed for this named build only. */
  (void)text;
#else
  if(!Silent&&Table&&Table->ConOut&&Table->ConOut->OutputString)
    Table->ConOut->OutputString(Table->ConOut,(CHAR16 *)text);
#endif
}

#ifdef GU_STOP_RELEASE_ONLY_BENCH
#define GU_STOP_BENCH_TARGET_TEXT_CHARS (GU_SELECTION_MAX*80U+1U)
static BOOLEAN BenchAppendChar(CHAR16 *out,UINTN capacity,UINTN *used,CHAR16 ch) {
  if(!out||!used||*used+1U>=capacity)return FALSE;
  out[(*used)++]=ch;out[*used]=0;
  return TRUE;
}
static BOOLEAN BenchAppendText(CHAR16 *out,UINTN capacity,UINTN *used,
                               const CHAR16 *text) {
  if(!text)return FALSE;
  while(*text)if(!BenchAppendChar(out,capacity,used,*text++))return FALSE;
  return TRUE;
}
static BOOLEAN BenchAppendHex(CHAR16 *out,UINTN capacity,UINTN *used,
                              UINT32 value,UINTN digits) {
  static const CHAR16 hex[]=L"0123456789abcdef";
  if(digits>8U)return FALSE;
  for(UINTN i=digits;i>0;i--)
    if(!BenchAppendChar(out,capacity,used,hex[(value>>((i-1U)*4U))&0xfU]))
      return FALSE;
  return TRUE;
}
static EFI_STATUS BenchBuildTargetText(CHAR16 *out,UINTN capacity) {
  if(!out||capacity<2U)return EFI_INVALID_PARAMETER;
  out[0]=0;UINTN used=0;size_t count=GuSelectedGpuCount();
  if(!count||count>GU_SELECTION_MAX)return EFI_NOT_READY;
  for(size_t i=0;i<count;i++) {
    GU_GPU_RECORD record;
    EFI_STATUS s=GuSelectedGpuRecordAt(i,&record);
    if(s!=EFI_SUCCESS)return s;
    if(i&&!BenchAppendText(out,capacity,&used,L"\r\n"))return EFI_BUFFER_TOO_SMALL;
    if(!BenchAppendHex(out,capacity,&used,record.segment,4U)||
       !BenchAppendChar(out,capacity,&used,L':')||
       !BenchAppendHex(out,capacity,&used,record.bus,2U)||
       !BenchAppendChar(out,capacity,&used,L':')||
       !BenchAppendHex(out,capacity,&used,record.device,2U)||
       !BenchAppendChar(out,capacity,&used,L'.')||
       !BenchAppendHex(out,capacity,&used,record.function,1U)||
       !BenchAppendText(out,capacity,&used,L" NVIDIA PCI=")||
       !BenchAppendHex(out,capacity,&used,record.pci_id&0xffffU,4U)||
       !BenchAppendChar(out,capacity,&used,L':')||
       !BenchAppendHex(out,capacity,&used,record.pci_id>>16,4U)||
       !BenchAppendText(out,capacity,&used,L" SUB=")||
       !BenchAppendHex(out,capacity,&used,record.subsystem_id&0xffffU,4U)||
       !BenchAppendChar(out,capacity,&used,L':')||
       !BenchAppendHex(out,capacity,&used,record.subsystem_id>>16,4U))
      return EFI_BUFFER_TOO_SMALL;
  }
  return EFI_SUCCESS;
}
/* A display warning is optional. Only a confirmed live firmware text
 * interface may be called; the file log is the primary evidence. Do not
 * call stale console function pointers after a GPU's driver was stopped. */
static EFI_STATUS BenchWriteConsole(const CHAR16 *prefix,
                                    const CHAR16 *targets,
                                    const CHAR16 *suffix) {
  EFI_SYSTEM_TABLE *t=Table;
  static EFI_GUID text_guid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
  if(!t||!t->BootServices||!t->BootServices->HandleProtocol)
    return EFI_UNSUPPORTED;
  EFI_HANDLE handles[2]={t->ConsoleOutHandle,t->StandardErrorHandle};
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *expected[2]={t->ConOut,t->StdErr};
  for(UINTN i=0;i<2;i++) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *live=NULL;
    if(!handles[i]||!expected[i])continue;
    if(i==1 && handles[i]==handles[0])continue;
    EFI_STATUS s=t->BootServices->HandleProtocol(handles[i],&text_guid,(VOID **)&live);
    if(s!=EFI_SUCCESS||live!=expected[i]||!live->OutputString)continue;
    s=live->OutputString(live,(CHAR16 *)prefix);
    if(s==EFI_SUCCESS)s=live->OutputString(live,(CHAR16 *)targets);
    if(s==EFI_SUCCESS)s=live->OutputString(live,(CHAR16 *)suffix);
    if(s==EFI_SUCCESS)return EFI_SUCCESS;
  }
  return EFI_NOT_READY;
}
EFI_STATUS GuBootBenchCountdown(VOID) {
  static CHAR16 prefix[]=L"\r\n*** WARNING: NVIDIA driver Stop/release and reattach test starts in 3 seconds. ***\r\n"
    L"The following detected NVIDIA GPUs will be processed in order:\r\n";
  static CHAR16 suffix[]=L"\r\n\r\nA display may go black, disconnect, or flicker during the test.\r\n"
    L"Do not rearrange display cables. Reattachment may fail or the firmware may hang.\r\n"
    L"Recovery has not been verified. Full power-off may be required.\r\n\r\n";
  static CHAR16 targets[GU_STOP_BENCH_TARGET_TEXT_CHARS];
  size_t count=GuSelectedGpuCount();
  if(!GuBootLogOpen()||!Table||!Table->BootServices||!Table->BootServices->Stall) {
    GuLog("BENCH_WARNING=REFUSED REASON=LOG_OR_STALL_UNAVAILABLE GPU_ACTION=0\n");
    return EFI_UNSUPPORTED;
  }
  /* The selected GPU inventory can exceed the fixed text-menu buffer.
   * Multi-GPU warning pixels use only the current target identity. */
  EFI_STATUS text_status=count>1U?EFI_SUCCESS:
    BenchBuildTargetText(targets,GU_STOP_BENCH_TARGET_TEXT_CHARS);
  if(text_status!=EFI_SUCCESS)return text_status;
  /* Multi-target warnings use a non-target NVIDIA GOP directly.
   * System text output may be bound to the GPU that will be stopped. */
  EFI_STATUS display=count>1U?
    GuMsiStopReleaseWarnSurvivor():
    BenchWriteConsole(prefix,targets,suffix);
  GuLog("BENCH_WARNING=%s CONSOLE=%s TARGET_COUNT=%zu "
        "DISPLAY_STATUS=%#llx FILE_LOG=AUTHORITATIVE DELAY_US=3000000 "
        "GPU_ACTION=%s\n",
        display==EFI_SUCCESS?"DISPLAYED":"CONSOLE_NOT_SHOWN",
        count>1U?"NON_TARGET_NVIDIA_GOP":"BEST_EFFORT",count,
        (unsigned long long)display,
        count>1U&&display!=EFI_SUCCESS?"REFUSED":"GATED");
  if(!GuBootLogOpen())return EFI_DEVICE_ERROR;
  if(count>1U&&display!=EFI_SUCCESS)return display;
  /* Even when no firmware console is available, the delay is mandatory. */
  EFI_STATUS delay=Table->BootServices->Stall(3000000U);
  if(delay!=EFI_SUCCESS)
    GuLog("BENCH_WARNING=DELAY_FAILED STATUS=%#llx GPU_ACTION=REFUSED\n",
          (unsigned long long)delay);
  return delay;
}
EFI_STATUS GuBootBenchReportCompletion(VOID) {
  static CHAR16 prefix[]=L"\r\nNVIDIA driver Stop/release and verified reattachment completed for:\r\n";
  static CHAR16 suffix[]=L"\r\nNo firmware unlock transaction ran. See the file log for verified results.\r\n";
  static CHAR16 targets[GU_STOP_BENCH_TARGET_TEXT_CHARS];
  if(!GuBootLogOpen())return EFI_NOT_READY;
  EFI_STATUS text_status=BenchBuildTargetText(targets,GU_STOP_BENCH_TARGET_TEXT_CHARS);
  EFI_STATUS display=text_status==EFI_SUCCESS?
    BenchWriteConsole(prefix,targets,suffix):text_status;
  GuLog("BENCH_COMPLETION=%s CONSOLE=BEST_EFFORT STATUS=%#llx "
        "GPU_SEQUENCE=COMPLETE FILE_LOG=AUTHORITATIVE\n",
        display==EFI_SUCCESS?"CONSOLE_DISPLAYED":"CONSOLE_NOT_SHOWN",
        (unsigned long long)display);
  /* Console output is never used as evidence of reattachment or a gate. */
  return EFI_SUCCESS;
}
#endif

VOID GuBootSilence(VOID){Silent=TRUE;}

VOID GuBootUnsilence(VOID){Silent=FALSE;}

VOID GuLog(const char *format,...) {

  if(!Log)return;

  CHAR8 buffer[2048];va_list args;va_start(args,format);

  size_t n=GuFormatV(buffer,sizeof(buffer),format,args);va_end(args);

  /* Never treat an incomplete or unwritten event as transaction evidence.
   * Keep the first durable I/O failure sticky in every image. */
  if(n==(size_t)-1||n>=sizeof(buffer)) {
    BootFileStatus=EFI_DEVICE_ERROR;
    return;
  }
  if(BootFileStatus!=EFI_SUCCESS)return;
  UINTN bytes=n;
  EFI_STATUS s=Log->Write?Log->Write(Log,&bytes,buffer):EFI_UNSUPPORTED;
  if(s!=EFI_SUCCESS||bytes!=n) {
    BootFileStatus=EFI_DEVICE_ERROR;
    return;
  }
  s=Log->Flush?Log->Flush(Log):EFI_UNSUPPORTED;
  if(s!=EFI_SUCCESS)BootFileStatus=EFI_DEVICE_ERROR;


}

VOID GuLogText(const char *text){GuLog("%s",text);}

/* ---- Verbose actuation trace ----------------------------------------------
 *
 * See port.h. Two properties matter more than the format:
 *
 * 1. Legacy console traces bypass Silent. The combined firmware/reattach
 *    application keeps global output silent after GuBootSilence.
 *    Successor preparation can mirror traces through the verified direct
 *    peer text interface. The file log remains authoritative.
 *    File-only DXE images never mirror traces to the console.
 * 2. Nothing here can block or allocate. GetTime is used when the firmware offers
 *    it and the elapsed field reads NA when it does not, so a model world without
 *    a clock still traces.
 */
static size_t TraceFormat(char *out,size_t capacity,const char *format,...) {
  va_list args;
  size_t n;
  va_start(args,format);
  n=GuFormatV(out,capacity,format,args);
  va_end(args);
  return n;
}

static UINTN TraceSteps;
static UINTN TraceStartSeconds;
static BOOLEAN TraceClockStarted;
static UINT64 TraceDelayShown;
static UINT8 TraceSpinnerDots;
static BOOLEAN TraceSpinnerActive;
static BOOLEAN TraceAssetReadActive;
/* Learned only from exact successful reads on the retained launch root. */
static UINTN AssetReadSize=1048576U;
static UINTN AssetReadBestBytes,AssetReadBestSeconds;
static BOOLEAN AssetReadSizeSettled;
static UINTN TraceBootStartSeconds;
static BOOLEAN TraceBootClockStarted;
#ifdef GU_ACTIVE_SINGLE_TARGET
/* A boot-local snapshot of the exact successful launch-root GSP file. No
 * shared allocation or borrowed firmware pointer is published to a successor. */
typedef struct {
  const CHAR16 *path;
  EFI_TIME created,modified;
  UINT64 physical_size,attributes;
} GU_GSP_FILE_SNAPSHOT;
static GU_GSP_FILE_SNAPSHOT GspFileCandidate,GspFileCached;
static GU_PLATFORM *GspCacheOwner;
static const GU_ARCH_ASSET *GspCacheManifest;
static BOOLEAN GspFileReadActive;
static BOOLEAN GspTimeEqual(const EFI_TIME *a,const EFI_TIME *b) {
  return a->Year==b->Year&&a->Month==b->Month&&a->Day==b->Day&&
    a->Hour==b->Hour&&a->Minute==b->Minute&&a->Second==b->Second&&
    a->Nanosecond==b->Nanosecond&&a->TimeZone==b->TimeZone&&a->Daylight==b->Daylight;
}
#endif

static BOOLEAN TraceClock(UINTN *seconds) {
  EFI_RUNTIME_SERVICES *rt=Table?Table->RuntimeServices:NULL;
  EFI_TIME now;
  if(!seconds||!rt||!rt->GetTime)return FALSE;
  if(EFI_ERROR(rt->GetTime(&now,NULL)))return FALSE;
  *seconds=((UINTN)now.Hour*3600U)+((UINTN)now.Minute*60U)+(UINTN)now.Second;
  return TRUE;
}

static const char *TraceElapsedText(char *buffer,UINTN size) {
  UINTN now=0;
  if(!TraceClockStarted) {
    TraceClockStarted=TraceClock(&TraceStartSeconds);
  }
  if(!TraceClockStarted||!TraceClock(&now))return "NA";
  /* Seconds of day, so a boot that crosses midnight still reports a small
   * number: one subtraction, wrapped into a day, is precise enough for a
   * heartbeat and cannot be wrong by more than that. */
  UINTN delta=(now+86400U-TraceStartSeconds)%86400U;
  TraceFormat(buffer,size,"%us",(unsigned)delta);
  return buffer;
}

#ifdef GU_FIRMWARE_REATTACH
/* The previous transaction verified this direct peer text interface. Never
 * substitute the system console or retain a route across another Stop. */
static EFI_HANDLE TracePeerHandle;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *TracePeerText;
static UINTN TracePeerOutput;
static BOOLEAN TraceSuccessorPreparation;

VOID GuBootTraceRememberPeer(EFI_HANDLE handle,EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text) {
  TracePeerHandle=handle;
  TracePeerText=text;
  TracePeerOutput=text?(UINTN)text->OutputString:0;
}

VOID GuBootTraceSuccessorPreparation(BOOLEAN enable) {
  TraceSuccessorPreparation=enable;
  if(!enable)GuBootTraceRememberPeer(NULL,NULL);
}

static VOID TracePeerSay(CHAR16 *wide) {
  static EFI_GUID text_guid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *live=NULL;
  if(!TraceSuccessorPreparation||!TracePeerHandle||!TracePeerText||
     !Table||!Table->BootServices||!Table->BootServices->HandleProtocol)return;
  EFI_STATUS s=Table->BootServices->HandleProtocol(TracePeerHandle,&text_guid,(VOID **)&live);
  if(s!=EFI_SUCCESS||live!=TracePeerText||!live||
     !live->OutputString||(UINTN)live->OutputString!=TracePeerOutput) {
    GuBootTraceRememberPeer(NULL,NULL);
    GuLog("SUCCESSOR_TRACE_CONSOLE=UNAVAILABLE FILE_LOG=AUTHORITATIVE\n");
    return;
  }
  if(live->OutputString(live,wide)!=EFI_SUCCESS) {
    GuBootTraceRememberPeer(NULL,NULL);
    GuLog("SUCCESSOR_TRACE_CONSOLE=OUTPUT_FAILED FILE_LOG=AUTHORITATIVE\n");
  }
}
#endif

static VOID TraceSay(const char *text) {
#ifdef GU_DXE_FILE_ONLY_OUTPUT
  /* GuTracePhase/Detail/Progress also call GuLog, which writes the file. */
  (void)text;
#else
  CHAR16 wide[96];
  UINTN i=0;
  if(!text)return;
  for(;i+1U<sizeof(wide)/sizeof(wide[0])&&text[i];i++)
    wide[i]=(CHAR16)(unsigned char)text[i];
  wide[i]=0;
#ifdef GU_FIRMWARE_REATTACH
  /* The global console remains silent after the first release. A successor
   * may mirror read-only preparation through the verified direct peer. */
  if(Silent){TracePeerSay(wide);return;}
#endif
  if(!Table||!Table->ConOut||!Table->ConOut->OutputString)return;
  Table->ConOut->OutputString(Table->ConOut,wide);
#endif
}

VOID GuTraceScreen(const char *text){TraceSay(text);}

VOID GuTraceStop(const char *stage,UINT64 status) {
  char line[128];
  if(TraceSpinnerActive)TraceSay("\r\n");
  TraceSpinnerActive=FALSE;
  TraceFormat(line,sizeof(line),"[gu] STOP stage=%s status=%#llx\r\n",
              stage?stage:"?",(unsigned long long)status);
  TraceSay(line);
}

VOID GuTraceReset(VOID) {
  TraceSteps=0;
  TraceStartSeconds=0;
  TraceClockStarted=FALSE;
  TraceDelayShown=0;
  TraceSpinnerDots=0;
  TraceSpinnerActive=FALSE;
  TraceAssetReadActive=FALSE;
}

VOID GuTraceDetail(const char *phase,const char *format,...) {
  char detail[160];
  va_list args;
  if(!Log)return;
  if(!format)format="";
  va_start(args,format);
  {size_t n=GuFormatV(detail,sizeof(detail),format,args);(void)n;}
  va_end(args);
  char elapsed[16];
  GuLog("TRACE STEP=%u PHASE=%s %s ELAPSED=%s\n",(unsigned)TraceSteps,phase?phase:"?",detail,
        TraceElapsedText(elapsed,sizeof(elapsed)));
}

VOID GuTracePhase(const char *phase) {
  char elapsed[16];char line[128];
  if(!Log)return;
  if(TraceSpinnerActive)TraceSay("\r\n");
  TraceSpinnerActive=FALSE;
  TraceSpinnerDots=0;
  ++TraceSteps;
  if(!TraceBootClockStarted)TraceBootClockStarted=TraceClock(&TraceBootStartSeconds);
  GuLog("TRACE STEP=%u ELAPSED=%s PHASE=%s\n",(unsigned)TraceSteps,
        TraceElapsedText(elapsed,sizeof(elapsed)),phase?phase:"?");
  TraceFormat(line,sizeof(line),"[gu] %u %s (%s)\r\n",(unsigned)TraceSteps,
           phase?phase:"?",TraceElapsedText(elapsed,sizeof(elapsed)));
  TraceSay(line);
  if(TraceBootClockStarted&&phase) {
    const char *a=phase,*b="close-USB-log";
    while(*a&&*a==*b){a++;b++;}
    UINTN now=0;
    if(!*a&&!*b&&TraceClock(&now))GuLog("TRACE_BOOT TOTAL_SECONDS=%u\n",
        (unsigned)((now+86400U-TraceBootStartSeconds)%86400U));
  }
}

VOID GuTraceProgress(const char *phase,UINTN done,UINTN total) {
  char elapsed[16];char line[128];
  if(!Log)return;
  GuLog("TRACE STEP=%u ELAPSED=%s PHASE=%s DONE=%u OF=%u\n",(unsigned)TraceSteps,
        TraceElapsedText(elapsed,sizeof(elapsed)),phase?phase:"?",
        (unsigned)done,(unsigned)total);
  if(TraceAssetReadActive) {
    TraceFormat(line,sizeof(line),"[gu] %s %u of %u\r\n",phase,
                (unsigned)done,(unsigned)total);
    TraceSay(line);
  }
}

/* The vendor transaction waits through one delay hook. A firmware step that
 * takes a minute spends that minute here, so report it as it goes - on a
 * threshold of accumulated delay, not per call, because the loop is tight and an
 * unthrottled trace would be the slow thing. */
VOID GuTraceDelay(UINT64 ticks) {
  char elapsed[16];
  if(!Log)return;
  if(ticks<TraceDelayShown+250000ULL)return;
  TraceDelayShown=ticks;
  GuLog("TRACE STEP=%u ELAPSED=%s PHASE=vendor-wait ACCUMULATED_US=%llu\n",
        (unsigned)TraceSteps,TraceElapsedText(elapsed,sizeof(elapsed)),
        (unsigned long long)ticks);
  TraceSpinnerDots=(UINT8)(TraceSpinnerDots%3U+1U);
  {
    static const char *dots[]={".","..","..."};
    char line[96];
    TraceFormat(line,sizeof(line),"\r[gu] %u vendor-wait %s   ",
                (unsigned)TraceSteps,dots[TraceSpinnerDots-1U]);
    TraceSay(line);
    TraceSpinnerActive=TRUE;
  }
}

BOOLEAN GuBootLogOpen(VOID){
  return Log!=NULL&&BootFileStatus==EFI_SUCCESS;
}
EFI_HANDLE GuBootGetTargetEspVolume(VOID){return TargetEspVolume;}
const CHAR16 *GuBootGetLogPath(VOID){return Log?ActiveLogPath:NULL;}

/* Retain uncertain file references at boot scope, not on a GPU. */
static EFI_STATUS RecordBootFileFailure(EFI_FILE_PROTOCOL *file) {
  if(UnreleasedBootFileCount<sizeof(UnreleasedBootFiles)/sizeof(UnreleasedBootFiles[0]))
    UnreleasedBootFiles[UnreleasedBootFileCount++]=file;
  BootFileStatus=EFI_DEVICE_ERROR;
  return BootFileStatus;
}
/* An error with a nonnull output does not establish an open file.
 * Do not use that reference or pass it to CloseBootFile. */
static EFI_STATUS BootFileOpenResult(EFI_STATUS status,EFI_FILE_PROTOCOL **file) {
  if(status!=EFI_SUCCESS&&*file) {
    EFI_FILE_PROTOCOL *uncertain=*file;
    *file=NULL;
    return RecordBootFileFailure(uncertain);
  }
  return status;
}
/* Consume each confirmed local reference once. Retain failed reference
 * addresses for diagnosis only; never dereference or retry them. */
static EFI_STATUS CloseBootFile(EFI_FILE_PROTOCOL **file) {
  if(!file||!*file)return EFI_SUCCESS;
  EFI_FILE_PROTOCOL *f=*file;
  *file=NULL;
  EFI_STATUS s=f->Close?f->Close(f):EFI_UNSUPPORTED;
  return s==EFI_SUCCESS?s:RecordBootFileFailure(f);
}

#ifdef GU_ESP_RESIDENT
/* A resident image must identify its ESP at runtime. A compiled partition GUID
 * would tie the installed copy to one machine, which is the coupling A2 removed,
 * so the two switches are not allowed to disagree. */
#ifndef GU_RUNTIME_ESP
#error GU_ESP_RESIDENT requires GU_RUNTIME_ESP
#endif
static BOOLEAN EspCarriesWindowsManager(EFI_BOOT_SERVICES *b,EFI_HANDLE handle);
static BOOLEAN StagedBundleRecordUsable(VOID);
#endif

EFI_STATUS GuBootOpen(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {

  if(Table)return EFI_ALREADY_STARTED;

  if(!image||!t||!t->BootServices)return EFI_INVALID_PARAMETER;

  Table=t;Self=image;
  TraceBootClockStarted=TraceClock(&TraceBootStartSeconds);

  EFI_BOOT_SERVICES *b=t->BootServices;EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;

  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;

  EFI_STATUS s=b->HandleProtocol(image,&LoadedGuid,(VOID **)&loaded);

  if(s!=EFI_SUCCESS||!loaded||!loaded->DeviceHandle)return EFI_UNSUPPORTED;

  OwnVolume=loaded->DeviceHandle;s=b->HandleProtocol(OwnVolume,&PathGuid,(VOID **)&path);

  UINTN length=0;BOOLEAN usb=FALSE,windows=FALSE;

  if(s!=EFI_SUCCESS||!Inspect(path,&length,&usb,&windows))return EFI_ACCESS_DENIED;
  /* Where may this image start from. A stick is the path everything else was
   * written for and stays unconditional; a resident copy has to earn the gates
   * below, and the refusal names the one that failed. */
  GU_LAUNCH_OBSERVATION launch={0};
  launch.Removable=usb?GU_LS_TRUE:GU_LS_FALSE;
  launch.Partition=windows?GU_LS_TRUE:GU_LS_FALSE;
#ifdef GU_ESP_RESIDENT
  launch.EspResidentBuild=GU_LS_TRUE;
#else
  launch.EspResidentBuild=GU_LS_FALSE;
#endif
  if(usb) {
    BootFromUsb=TRUE;
  } else {
#ifdef GU_ESP_RESIDENT
    launch.PinnedEsp=GU_LS_TRUE; /* nothing is pinned in a runtime-ESP build */
    launch.WindowsLoader=EspCarriesWindowsManager(b,OwnVolume)?GU_LS_TRUE:GU_LS_FALSE;
    launch.BundleRecord=StagedBundleRecordUsable()?GU_LS_TRUE:GU_LS_FALSE;
#endif
    if(GuLaunchSourceDecide(&launch)!=GU_LAUNCH_ALLOW_RESIDENT) {
      GU_LS_UINTN reason=GuLaunchSourceReason(&launch);
      GuLog("LAUNCH_SOURCE DECISION=%s REASON=%s REMOVABLE=%u PARTITION=%u "
            "WINDOWS_LOADER=%u PINNED=%u BUNDLE=%u RESIDENT_BUILD=%u\n",
            GuLaunchSourceDecisionName(GU_LAUNCH_REFUSE),
            GuLaunchSourceReasonName(reason),(unsigned)launch.Removable,
            (unsigned)launch.Partition,(unsigned)launch.WindowsLoader,
            (unsigned)launch.PinnedEsp,(unsigned)launch.BundleRecord,
            (unsigned)launch.EspResidentBuild);
      return EFI_ACCESS_DENIED;
    }
  }

  s=b->HandleProtocol(OwnVolume,&FsGuid,(VOID **)&fs);if(s!=EFI_SUCCESS||!fs)return EFI_UNSUPPORTED;

  s=fs->OpenVolume(fs,&Root);
  s=BootFileOpenResult(s,&Root);
  if(s!=EFI_SUCCESS||!Root){s=EFI_DEVICE_ERROR;goto failed;}

  EFI_FILE_PROTOCOL *dir=NULL;

  s=Root->Open(Root,&dir,L"\\AD102-UNLOCK",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  s=BootFileOpenResult(s,&dir);

  if(s!=EFI_SUCCESS||!dir){s=EFI_DEVICE_ERROR;goto failed;}

  for(UINTN slot=1;slot<=9999;slot++) {

    CHAR16 name[]=L"RUN0000.TXT";UINTN number=slot;

    for(UINTN at=7;at>3;){at--;name[at]=(CHAR16)('0'+number%10);number/=10;}

    EFI_FILE_PROTOCOL *existing=NULL;s=dir->Open(dir,&existing,name,EFI_FILE_MODE_READ,0);
    s=BootFileOpenResult(s,&existing);

    if(s==EFI_SUCCESS&&existing){s=CloseBootFile(&existing);if(s!=EFI_SUCCESS)break;continue;}

    if(s!=EFI_NOT_FOUND)break;

    s=dir->Open(dir,&Log,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    s=BootFileOpenResult(s,&Log);
    if(s==EFI_SUCCESS&&Log) {
      for(UINTN k=0;k<sizeof(name)/sizeof(CHAR16);k++) ActiveLogPath[14+k]=name[k];
    }
    break;

  }

  EFI_STATUS closed=CloseBootFile(&dir);
  if(closed!=EFI_SUCCESS){s=closed;goto failed;}
  if(s!=EFI_SUCCESS||!Log){if(s==EFI_SUCCESS)s=EFI_OUT_OF_RESOURCES;goto failed;}

  if(!BootFromUsb) {
    /* The last gate: a resident image needs its own writable log directory, which
     * is what the lines above just proved by creating the file. */
    launch.LogWritable=GU_LS_TRUE;
    GU_LS_UINTN decision=GuLaunchSourceDecide(&launch);
    GU_LS_UINTN reason=GuLaunchSourceReason(&launch);
    GuLog("LAUNCH_SOURCE DECISION=%s REASON=%s ESP_RESIDENT=1 LOG_WRITABLE=1\n",
          GuLaunchSourceDecisionName(decision),GuLaunchSourceReasonName(reason));
    if(decision!=GU_LAUNCH_ALLOW_RESIDENT){s=EFI_ACCESS_DENIED;goto failed;}
    /* Loud, once: this copy runs from the ESP and not from the stick it was
     * installed from. Every actuation gate is unchanged. */
    GuLog("LAUNCH_SOURCE=ESP_RESIDENT_WINDOWS_ESP ALLOWED_BY=GU_ESP_RESIDENT "
          "INSTALL_STILL_REQUIRES_USB=1 ACTUATION_GATES_UNCHANGED=1\n");
  }

  /* What removable media last staged on the ESP, if anything. Reported before
   * discovery because it is a property of the medium, not of the card; the
   * CURRENT/STALE decision needs a manifest and is reported at load. */
  {
    GU_ASSET_INSTALL staged;
    EFI_STATUS read=GuAssetInstallRead(Table,&staged);
    if(read==EFI_SUCCESS)
      GuLog("ASSET_INSTALL PRESENT=1 FILES=%u BOOT_COUNT=%u BUNDLE=%s\n",
            (unsigned)staged.Files,(unsigned)staged.BootCount,staged.Bundle);
    else if(read==EFI_NOT_FOUND)
      GuLog("ASSET_INSTALL PRESENT=0\n");
    else
      GuLog("ASSET_INSTALL PRESENT=0 READ_STATUS=%#llx IGNORED=1\n",
            (unsigned long long)read);
  }

  /* The persisted record is the only state that survives a failed boot, so it
   * is reported here, once, for every entrypoint. An unmatched INTENT is also
   * consumed by the entrypoint-specific fail-closed recovery path. */
  {
    GU_BOOT_STATE previous;
    EFI_STATUS read=GuBootStateRead(Table,&previous);
    if(read==EFI_SUCCESS) {
      GuLog("BOOT_STATE PRESENT=1 PHASE=%u BOOT_COUNT=%u STATUS=%#llx DEVICE=%04x "
            "BDF=%04x:%02x:%02x.%x ARMED=%u\n",
            (unsigned)previous.Phase,(unsigned)previous.BootCount,
            (unsigned long long)previous.Status,(unsigned)previous.DeviceId,
            (unsigned)previous.Segment,(unsigned)previous.Bus,
            (unsigned)previous.Device,(unsigned)previous.Function,
            (unsigned)previous.Armed);
      if(previous.Phase==GU_BOOT_STATE_INTENT)
        GuLog("BOOT_STATE WARNING=previous attempt interrupted or awaiting owner acceptance\n");
      OpenBootCount=previous.BootCount;
    } else if(read==EFI_NOT_FOUND) {
      GuLog("BOOT_STATE PRESENT=0\n");
    } else {
      GuLog("BOOT_STATE PRESENT=0 READ_STATUS=%#llx VALID=0\n",
            (unsigned long long)read);
    }
  }

#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
  /* Preparation reads PCI identity and writes only its removable USB log.
   * Do not change the firmware watchdog or any active boot state. */
  return EFI_SUCCESS;
#endif
  /* The only place the firmware timer is programmed. A build without a
   * threshold disables it exactly as before; a build with one arms it here, so
   * the bound covers preparation, the transaction and the driver work, and
   * GuBootWatchdogCancel() removes it before the handover. */
  s=WatchdogProgram((UINTN)GU_WATCHDOG_SECONDS);
  if(s==EFI_SUCCESS)return s;
failed:
  {
    EFI_STATUS released=GuBootClose();
    return released==EFI_SUCCESS?s:released;
  }
}

/* Boot-wide CPU memory. The caller retains each confirmed reference until
 * one checked release. An error with nonnull output is uncertain ownership. */
static EFI_STATUS BootBufferFailure(VOID *address) {
  if(UnreleasedBootBufferCount<sizeof(UnreleasedBootBuffers)/sizeof(UnreleasedBootBuffers[0]))
    UnreleasedBootBuffers[UnreleasedBootBufferCount++]=address;
  BootFileStatus=EFI_DEVICE_ERROR;
  return BootFileStatus;
}
static EFI_STATUS AllocateBootBuffer(EFI_BOOT_SERVICES *bs,UINTN size,VOID **out) {
  if(!out||*out||!size)return EFI_INVALID_PARAMETER;
  if(BootFileStatus!=EFI_SUCCESS)return BootFileStatus;
  if(!bs||!bs->AllocatePool||!bs->FreePool)return EFI_UNSUPPORTED;
  VOID *p=NULL;
  EFI_STATUS s=bs->AllocatePool(EfiLoaderData,size,&p);
  if(s!=EFI_SUCCESS)return p?BootBufferFailure(p):s;
  if(!p)return EFI_OUT_OF_RESOURCES;
  *out=p;
  return EFI_SUCCESS;
}
static EFI_STATUS ReleaseBootBuffer(EFI_BOOT_SERVICES *bs,VOID **out) {
  if(!out||!*out)return EFI_SUCCESS;
  VOID *p=*out;
  *out=NULL;
  if(!bs||!bs->FreePool||bs->FreePool(p)!=EFI_SUCCESS)
    return BootBufferFailure(p);
  return EFI_SUCCESS;
}

static EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *root,const CHAR16 *name,UINTN expected,VOID **data) {

  *data=NULL;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  if(BootFileStatus!=EFI_SUCCESS)return BootFileStatus;

  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);
  s=BootFileOpenResult(s,&f);

  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;

  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);

  EFI_FILE_INFO *info=(VOID *)info_words;

  if(s==EFI_SUCCESS&&(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||

     info->FileSize!=expected||(info->Attribute&EFI_FILE_DIRECTORY)))s=EFI_COMPROMISED_DATA;

  VOID *p=NULL;

  if(s==EFI_SUCCESS){p=GuAlloc(expected);if(!p)s=EFI_OUT_OF_RESOURCES;}

  /* Only the read-only firmware load uses adaptive requests. Keep installer
   * readback and other file users at their original request size. The launch
   * root stays open for the boot; temporary fallback roots learn locally. */
  const BOOLEAN adaptive=TraceAssetReadActive;
  const BOOLEAN learn=adaptive&&root==Root;
  UINTN chunk=learn?AssetReadSize:1048576U;
  UINTN best_bytes=learn?AssetReadBestBytes:0,best_seconds=learn?AssetReadBestSeconds:0;
  BOOLEAN settled=learn&&AssetReadSizeSettled;
  UINTN reads=0,read_seconds=0,progress_seconds=0,reported=0,last_report=0;
  UINTN proven=learn?AssetReadSize:1048576U;
  BOOLEAN timing=adaptive&&TraceClock(&last_report);
  UINTN at=0;
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(s==EFI_SUCCESS&&adaptive&&GspFileReadActive&&root==Root&&
     GspCacheOwner&&GspCacheOwner!=GuPlatformActive&&GspCacheManifest&&
     GspFileCached.path&&expected==GspCacheManifest->bytes&&
     f->SetPosition&&f->GetPosition&&info->ModificationTime.Year) {
    const CHAR16 *a=name,*b=GspFileCached.path;
    while(*a&&*a==*b){a++;b++;}
    const GU_ARCH_ASSETS *arch=GuGetArchAssets();
    /* Never bypass an existing distinct preferred file. Reuse only the exact
     * prior selected path and unchanged metadata on the retained launch root.
     * The pinned RAM snapshot, not a new full disk read, supplies the bytes. */
    if(!*a&&!*b&&arch&&arch->entries[2].bytes==expected&&
       strcmp(arch->entries[2].sha256,GspCacheManifest->sha256)==0&&
       info->PhysicalSize==GspFileCached.physical_size&&
       info->Attribute==GspFileCached.attributes&&
       GspTimeEqual(&info->CreateTime,&GspFileCached.created)&&
       GspTimeEqual(&info->ModificationTime,&GspFileCached.modified)) {
      const struct firmware *source=&GspCacheOwner->asset_entries[2].firmware;
      if(GspCacheOwner->fault||GspCacheOwner->retained||
         !GuPlatformAssetsOwned(GspCacheOwner)||
         source->size!=expected||!GuCpuPoolOwns(GspCacheOwner,source->data,expected)||
         !GuCpuPoolOwns(&Gu,p,expected)||source->data==p) {
        s=EFI_DEVICE_ERROR;
      } else {
        UINT64 pos=0;
        s=f->SetPosition(f,(UINT64)expected);
        if(s==EFI_SUCCESS)s=f->GetPosition(f,&pos);
        if(s==EFI_SUCCESS&&pos!=(UINT64)expected)s=EFI_DEVICE_ERROR;
        if(s==EFI_SUCCESS) {
          UINTN start=0,end=0;
          BOOLEAN timed=TraceClock(&start);
          memcpy(p,source->data,expected);
          at=expected;
          timed=timed&&TraceClock(&end);
          GuTraceDetail("asset-cache","HIT=1 BYTES=%u COPY_SECONDS=%u CLOCK=%u INDEPENDENT_BUFFER=1",
                        (unsigned)expected,(unsigned)(timed?(end+86400U-start)%86400U:0),
                        (unsigned)timed);
          GuTraceProgress("asset-read-bytes",at,expected);
          if(BootFileStatus!=EFI_SUCCESS)s=BootFileStatus;
        }
      }
    }
  }
#endif
  for(;s==EFI_SUCCESS&&at<expected;) {
    UINTN n=expected-at;if(n>chunk)n=chunk;UINTN wanted=n;
    UINTN before=0,after=0;
    BOOLEAN measured=adaptive&&TraceClock(&before);
    s=f->Read(f,&n,(UINT8 *)p+at);reads++;
    measured=measured&&TraceClock(&after);
    if(adaptive&&!measured)timing=FALSE;
    UINTN elapsed=measured?(after+86400U-before)%86400U:0;
    read_seconds+=elapsed;
    /* A generic device error or short read is not a size rejection. Only
     * BAD_BUFFER_SIZE permits a bounded retry, at a proved original offset. */
    if(adaptive&&s==EFI_BAD_BUFFER_SIZE&&wanted>1048576U&&
       f->SetPosition&&f->GetPosition&&BootFileStatus==EFI_SUCCESS) {
      UINT64 pos=0;
      s=f->SetPosition(f,(UINT64)at);
      if(s==EFI_SUCCESS)s=f->GetPosition(f,&pos);
      if(s!=EFI_SUCCESS||pos!=(UINT64)at){s=EFI_DEVICE_ERROR;break;}
      do {chunk/=2U;} while(chunk>=wanted&&chunk>1048576U);
      settled=TRUE;best_bytes=0;best_seconds=0;proven=chunk;
      GuTraceDetail("asset-read-size","REJECTED=%u RETRY=%u",(unsigned)wanted,(unsigned)chunk);
      if(BootFileStatus!=EFI_SUCCESS){s=BootFileStatus;break;}
      continue;
    }
    if(s!=EFI_SUCCESS)break;
    if(n!=wanted){s=EFI_DEVICE_ERROR;break;}
    at+=n;
    if(adaptive&&wanted==chunk)proven=chunk;
    if(adaptive&&!settled&&wanted==chunk) {
      /* RTC seconds are coarse. Require two sufficiently long measurements
       * and a material regression before reducing; otherwise grow on exact
       * success. The finite cap also works without a clock. */
      if(measured&&elapsed>=2U) {
        if(best_seconds&&((UINT64)n*best_seconds*4U <
                          (UINT64)best_bytes*elapsed*3U)) {
          if(chunk>1048576U)chunk/=2U;
          settled=TRUE;proven=chunk;
        } else if(!best_seconds||(UINT64)n*best_seconds>(UINT64)best_bytes*elapsed) {
          best_bytes=n;best_seconds=elapsed;
        }
      }
      if(!settled&&chunk<16777216U)chunk*=2U;
    }
    if(adaptive) {
      UINTN now=0,finished=0;
      BOOLEAN clock=timing&&TraceClock(&now);
      /* Preserve first, final and time/byte heartbeats; do not change GuLog
       * durability or suppress phase/intent/result records. */
      if(!reported||at==expected||at-reported>=4194304U||
         (clock&&(now+86400U-last_report)%86400U>=2U)) {
        GuTraceProgress("asset-read-bytes",at,expected);
        reported=at;
        if(clock&&TraceClock(&finished)) {
          progress_seconds+=(finished+86400U-now)%86400U;
          last_report=finished;
        } else timing=FALSE;
        if(BootFileStatus!=EFI_SUCCESS){s=BootFileStatus;break;}
      }
    }
  }
  if(adaptive)GuTraceDetail("asset-read-timing",
    "READ_CALLS=%u READ_SECONDS=%u PROGRESS_SECONDS=%u NEXT_BYTES=%u CLOCK=%u STATUS=%#llx",
    (unsigned)reads,(unsigned)read_seconds,(unsigned)progress_seconds,(unsigned)chunk,
    (unsigned)timing,(unsigned long long)s);
  if(s==EFI_SUCCESS&&BootFileStatus!=EFI_SUCCESS)s=BootFileStatus;

  if(s==EFI_SUCCESS){UINT8 tail;UINTN n=1;s=f->Read(f,&n,&tail);if(s==EFI_SUCCESS&&n)s=EFI_COMPROMISED_DATA;}

  EFI_STATUS closed=CloseBootFile(&f);if(closed!=EFI_SUCCESS)s=closed;

  if(s!=EFI_SUCCESS){GuFree(p);return s;}
  if(learn&&expected>=1048576U&&reads){AssetReadSize=proven;AssetReadBestBytes=best_bytes;
    AssetReadBestSeconds=best_seconds;AssetReadSizeSettled=settled;}
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(adaptive&&GspFileReadActive&&root==Root&&info->ModificationTime.Year) {
    GspFileCandidate.path=name;
    GspFileCandidate.created=info->CreateTime;
    GspFileCandidate.modified=info->ModificationTime;
    GspFileCandidate.physical_size=info->PhysicalSize;
    GspFileCandidate.attributes=info->Attribute;
  }
#endif
  *data=p;return EFI_SUCCESS;

}

/* Both asset fallback and Windows staging use the same boot-local census.
 * A failed release is not a missing file and must never trigger a fallback. */
static EFI_STATUS ReleaseBootFilesystemHandles(EFI_BOOT_SERVICES *bs,EFI_HANDLE **handles) {
  if(!handles||!*handles)return EFI_SUCCESS;
  EFI_HANDLE *buffer=*handles;
  *handles=NULL;
  EFI_STATUS s=bs->FreePool(buffer);
  if(s!=EFI_SUCCESS) {
    UnreleasedFilesystemHandles=buffer;
    GuLog("FILESYSTEM_HANDLE_BUFFER_FREE_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS LocateBootFilesystems(EFI_BOOT_SERVICES *bs,EFI_HANDLE **handles,UINTN *count) {
  if(!handles||!count)return EFI_INVALID_PARAMETER;
  *handles=NULL;*count=0;
  if(UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS)return EFI_NOT_READY;
  if(!bs||!bs->LocateHandleBuffer||!bs->FreePool)return EFI_UNSUPPORTED;
  EFI_STATUS s=bs->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,count,handles);
  if(s!=EFI_SUCCESS){
    if(*handles) {
      /* A failed enumeration does not transfer a verified pool buffer. */
      UnreleasedFilesystemHandles=*handles;
      GuLog("FILESYSTEM_HANDLE_BUFFER_LOCATE_STATUS=%#llx RETAINED=1\n",(unsigned long long)s);
      s=EFI_DEVICE_ERROR;
    }
    *handles=NULL;*count=0;return s;
  }
  if(!*handles||!*count||*count>128U) {
    s=ReleaseBootFilesystemHandles(bs,handles);
    *count=0;
    return s==EFI_SUCCESS?EFI_UNSUPPORTED:s;
  }
  return EFI_SUCCESS;
}

/* Only the explicitly configured Windows GPT ESP may supply a missing
 * firmware file. Never search arbitrary partitions or write to this ESP. */
#ifdef GU_RUNTIME_ESP
#ifdef GU_RUNTIME_ESP_PIN
#include "windows-esp-pin.h"
static const uint8_t RuntimeEspPin[16]=GU_WINDOWS_ESP_PIN_BYTES;
#endif
/* The same exact optional pin constrains firmware fallback and Windows staging. */
#if defined(GU_INTERACTIVE_WINDOWS_ESP) && !defined(GU_RUNTIME_ESP_PIN)
static EFI_HANDLE SelectedEspHandle;
static uint8_t SelectedEspGuid[16];
#endif
static BOOLEAN RuntimeEspPathCandidate(const EFI_DEVICE_PATH_PROTOCOL *path,UINTN bytes) {
#ifdef GU_RUNTIME_ESP_PIN
  return GuWindowsEspPathMatches((const uint8_t *)path,bytes,RuntimeEspPin);
#else
#if defined(GU_INTERACTIVE_WINDOWS_ESP)
  if(SelectedEspHandle)
    return GuWindowsEspPathMatches((const uint8_t *)path,bytes,SelectedEspGuid);
#endif
  return GuWindowsEspPathCandidate((const uint8_t *)path,bytes);
#endif
}
/* Probe only: does this volume carry the Windows boot manager? A failure means
 * "not the Windows ESP", so it must not latch BootFileStatus or disturb the
 * image's own open-file accounting. */
static BOOLEAN EspCarriesWindowsManager(EFI_BOOT_SERVICES *b,EFI_HANDLE handle) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_PROTOCOL *root=NULL,*file=NULL;
  if(!b||!handle)return FALSE;
  if(b->HandleProtocol(handle,&FsGuid,(VOID **)&fs)!=EFI_SUCCESS||!fs)return FALSE;
  if(fs->OpenVolume(fs,&root)!=EFI_SUCCESS||!root)return FALSE;
  EFI_STATUS s=root->Open(root,&file,(CHAR16 *)WindowsCandidates[0],EFI_FILE_MODE_READ,0);
  BOOLEAN found=s==EFI_SUCCESS&&file!=NULL;
  if(file)file->Close(file);
  root->Close(root);
  return found;
}
#endif

/* The one configured Windows ESP, or EFI_NOT_FOUND when no volume qualifies and
 * EFI_UNSUPPORTED when more than one does. Reads use it as a fallback source;
 * the installer uses it as the staging destination, so both must agree on
 * which volume that is. */
#ifdef GU_ESP_RESIDENT
/* Was this image's bundle ever staged on this volume? Presence and identity of
 * the record, not a re-hash: the loader verifies every byte it uses. */
static BOOLEAN StagedBundleRecordUsable(VOID) {
  GU_ASSET_INSTALL staged;
  if(GuAssetInstallRead(Table,&staged)!=EFI_SUCCESS)return FALSE;
  if(staged.Files!=GU_ASSET_COUNT)return FALSE;
  for(UINTN i=0;i<GU_ASSET_INSTALL_VERSION_CHARS;i++) {
    CHAR16 want=GU_ASSET_BUNDLE_VERSION[i];
    if(staged.Version[i]!=want)return FALSE;
    if(!want)return TRUE;
  }
  return FALSE;
}
#endif

#ifdef GU_RUNTIME_ESP
/* The EFI global-variable GUID, spelled out the way driver_nvram.c does so
 * this compiles against the simulation headers as well as EDK II's. */
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};

/* A2's remaining half: the selected volume must be the one BootOrder boots
 * from.
 *
 * BootOrder lists the load options the firmware will try. Every option it names
 * is read, and its device-path prefix compared with the selected volume. A
 * readable BootOrder that names none of them is a contradiction, so it refuses;
 * a BootOrder this image cannot read degrades to the previous rule and says so,
 * because a check that did not happen must never be logged as one that passed. */
/* The one firmware call the shared walk makes. Any failure that is not
 * "variable absent" is reported as unreadable, so an option this image cannot
 * read in full can never be counted as a match. */
static int ReadGlobalVariable(const uint8_t *name,size_t name_bytes,
                              uint8_t *out,size_t *size) {
  EFI_RUNTIME_SERVICES *rs=Table?Table->RuntimeServices:NULL;
  if(!rs||!rs->GetVariable||!name||!out||!size)return -1;
  UINTN n=*size;
  UINT32 attrs=0;
  EFI_STATUS s=rs->GetVariable((CHAR16 *)name,&GlobalGuid,&attrs,&n,out);
  *size=n;
  if(s==EFI_SUCCESS)return 1;
  return s==EFI_NOT_FOUND?0:-1;
}

static EFI_STATUS BootOrderNamesVolume(const EFI_DEVICE_PATH_PROTOCOL *dp,UINTN bytes) {
  int verdict=GuBootOrderNamesVolume(ReadGlobalVariable,(const uint8_t *)dp,bytes);
  if(verdict==GU_ORDER_NAMES)return EFI_SUCCESS;
  return verdict==GU_ORDER_NONE?EFI_UNSUPPORTED:EFI_NOT_READY;
}
#endif

#if defined(GU_INTERACTIVE_WINDOWS_ESP) && !defined(GU_RUNTIME_ESP_PIN)
#define GU_ESP_MENU_MAX 9U
typedef struct {
  EFI_HANDLE handle;
  uint8_t guid[16];
} GU_ESP_CHOICE;

/* Probe using the same file names and confirmed close accounting as staging. */
static EFI_STATUS ProbeWindowsManager(EFI_BOOT_SERVICES *b,EFI_HANDLE handle,BOOLEAN *found) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  EFI_FILE_PROTOCOL *root=NULL;
  *found=FALSE;
  EFI_STATUS s=b->HandleProtocol(handle,&FsGuid,(VOID **)&fs);
  if(s!=EFI_SUCCESS||!fs)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  if(!fs->OpenVolume)return EFI_UNSUPPORTED;
  s=fs->OpenVolume(fs,&root);
  s=BootFileOpenResult(s,&root);
  if(s!=EFI_SUCCESS||!root)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  if(!root->Open)s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<sizeof(WindowsCandidates)/sizeof(WindowsCandidates[0]);i++) {
    EFI_FILE_PROTOCOL *file=NULL;
    EFI_STATUS opened=root->Open(root,&file,(CHAR16 *)WindowsCandidates[i],EFI_FILE_MODE_READ,0);
    opened=BootFileOpenResult(opened,&file);
    if(opened==EFI_NOT_FOUND)continue;
    if(opened!=EFI_SUCCESS||!file){s=opened==EFI_SUCCESS?EFI_DEVICE_ERROR:opened;break;}
    s=CloseBootFile(&file);
    if(s==EFI_SUCCESS)*found=TRUE;
    break;
  }
  EFI_STATUS closed=CloseBootFile(&root);
  return closed==EFI_SUCCESS?s:closed;
}

static EFI_STATUS ShowWindowsEspChoice(EFI_SYSTEM_TABLE *t,UINTN number,
                                      const uint8_t guid[16],BOOLEAN show) {
  static const CHAR16 hex[]=L"0123456789abcdef";
  static const UINT8 order[]={3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
  CHAR16 row[]=L"0: Windows Boot Manager 00000000-0000-0000-0000-000000000000\r\n";
  UINTN at=sizeof(L"0: Windows Boot Manager ")/sizeof(CHAR16)-1U;
  if(!guid||number<1U||number>GU_ESP_MENU_MAX)return EFI_INVALID_PARAMETER;
  row[0]=(CHAR16)('0'+number);
  for(UINTN i=0;i<16U;i++) {
    if(i==4U||i==6U||i==8U||i==10U)at++;
    row[at++]=hex[guid[order[i]]>>4U];
    row[at++]=hex[guid[order[i]]&15U];
  }
  GuLog("WINDOWS_ESP_CANDIDATE %ls",row);
  if(!show)return EFI_SUCCESS;
  if(!t||!t->ConOut||!t->ConOut->OutputString)return EFI_UNSUPPORTED;
  return t->ConOut->OutputString(t->ConOut,row);
}

/* A single candidate needs no question. Multiple candidates require fresh
 * input; no timeout or cancellation can publish a default choice. */
static EFI_STATUS ChooseWindowsEsp(EFI_SYSTEM_TABLE *t,const GU_ESP_CHOICE *choices,
                                   UINTN count,UINTN *selected) {
  if(!t||!choices||!selected||!count||count>GU_ESP_MENU_MAX)return EFI_INVALID_PARAMETER;
  *selected=count;
  for(UINTN i=0;i<count;i++) {
    EFI_STATUS s=ShowWindowsEspChoice(t,i+1U,choices[i].guid,count>1U);
    if(s!=EFI_SUCCESS||!GuBootLogOpen())return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  }
  if(count==1U){*selected=0;return EFI_SUCCESS;}
  if(!t->ConIn||!t->ConIn->ReadKeyStroke||!t->ConOut||!t->ConOut->OutputString||
     !t->BootServices||!t->BootServices->Stall)return EFI_UNSUPPORTED;
  EFI_STATUS s=t->ConOut->OutputString(t->ConOut,
    L"\r\nSelect Windows boot partition (1-9), or ESC to cancel.\r\n"
    L"No GPU transaction has started. Selection expires in 30 seconds.\r\n");
  if(s!=EFI_SUCCESS)return s;
  for(UINTN i=0;i<3000U;i++) {
    EFI_INPUT_KEY key;
    s=t->ConIn->ReadKeyStroke(t->ConIn,&key);
    if(s==EFI_SUCCESS) {
      if(key.ScanCode==0x17||key.UnicodeChar=='s'||key.UnicodeChar=='S')return EFI_ABORTED;
      if(key.UnicodeChar>='1'&&key.UnicodeChar<=(CHAR16)('0'+count)) {
        *selected=(UINTN)(key.UnicodeChar-'1');
        return EFI_SUCCESS;
      }
    } else if(s!=EFI_NOT_READY)return s;
    s=t->BootServices->Stall(10000U);
    if(s!=EFI_SUCCESS)return s;
  }
  return EFI_TIMEOUT;
}

/* Do not hold open files or a filesystem handle buffer across user input.
 * The loader repeats its exact unique-match census before LoadImage. */
static EFI_STATUS SelectRuntimeWindowsEsp(VOID) {
  if(SelectedEspHandle||!Table||!Table->BootServices||!GuBootLogOpen())return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Table->BootServices;
  EFI_HANDLE *handles=NULL;
  UINTN count=0,matches=0,selected=0;
  GU_ESP_CHOICE choices[GU_ESP_MENU_MAX];
  EFI_STATUS s=LocateBootFilesystems(b,&handles,&count);
  if(s!=EFI_SUCCESS)return s;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(handles[i]==OwnVolume)continue;
    EFI_DEVICE_PATH_PROTOCOL *dp=NULL;
    UINTN bytes=0;BOOLEAN usb=FALSE,media=FALSE,found=FALSE;
    uint8_t guid[16];
    if(b->HandleProtocol(handles[i],&PathGuid,(VOID **)&dp)!=EFI_SUCCESS||
       !Inspect(dp,&bytes,&usb,&media)||usb||
       !GuWindowsEspPathGuid((const uint8_t *)dp,bytes,guid))continue;
    s=ProbeWindowsManager(b,handles[i],&found);
    if(s!=EFI_SUCCESS||!found)continue;
    if(matches==GU_ESP_MENU_MAX){s=EFI_UNSUPPORTED;break;}
    for(UINTN k=0;k<matches;k++)
      if(!memcmp(choices[k].guid,guid,sizeof(guid))||choices[k].handle==handles[i]) {
        s=EFI_COMPROMISED_DATA;break;
      }
    if(s!=EFI_SUCCESS)break;
    choices[matches].handle=handles[i];
    memcpy(choices[matches++].guid,guid,sizeof(guid));
  }
  EFI_STATUS freed=ReleaseBootFilesystemHandles(b,&handles);
  if(freed!=EFI_SUCCESS)return freed;
  if(s!=EFI_SUCCESS)return s;
  if(!matches)return EFI_NOT_FOUND;
  s=ChooseWindowsEsp(Table,choices,matches,&selected);
  if(s!=EFI_SUCCESS||!GuBootLogOpen()||selected>=matches) {
    GuLog("WINDOWS_ESP_SELECTION=REFUSED STATUS=%#llx CANDIDATES=%zu\n",
          (unsigned long long)s,(size_t)matches);
    return s!=EFI_SUCCESS?s:EFI_DEVICE_ERROR;
  }
  memcpy(SelectedEspGuid,choices[selected].guid,sizeof(SelectedEspGuid));
  SelectedEspHandle=choices[selected].handle;
  GuLog("WINDOWS_ESP_SELECTION=ACCEPTED CHOICE=%zu CANDIDATES=%zu SOURCE=%s NVRAM=UNCHANGED\n",
        (size_t)selected+1U,(size_t)matches,matches==1U?"UNIQUE":"OPERATOR");
  return GuBootLogOpen()?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
#endif

static EFI_STATUS ConfiguredEspVolume(EFI_HANDLE *out) {
  if(!out||!Table||!Table->BootServices)return EFI_INVALID_PARAMETER;
  *out=NULL;
  EFI_BOOT_SERVICES *b=Table->BootServices;
  EFI_HANDLE *handles=NULL,matched=NULL;
  UINTN count=0,matches=0;
  EFI_STATUS s=LocateBootFilesystems(b,&handles,&count);
  if(s!=EFI_SUCCESS)return s;
  for(UINTN i=0;i<count;i++) {
    if(handles[i]==OwnVolume)continue;
    EFI_DEVICE_PATH_PROTOCOL *dp=NULL;
    UINTN bytes=0;BOOLEAN usb=FALSE,media=FALSE;
    if(b->HandleProtocol(handles[i],&PathGuid,(VOID **)&dp)==EFI_SUCCESS&&
       Inspect(dp,&bytes,&usb,&media)&&!usb&&
#ifdef GU_RUNTIME_ESP
       /* The marker is what makes a volume the Windows ESP, so a second ESP
        * without one can no longer make the machine ambiguous. */
       RuntimeEspPathCandidate(dp,bytes)&&
       EspCarriesWindowsManager(b,handles[i])
#else
       GuWindowsEspPathMatches((const uint8_t *)dp,bytes,
                              (const uint8_t *)&WindowsPartition)
#endif
       ) {
      matches++;
      matched=handles[i];
    }
  }
  EFI_STATUS freed=ReleaseBootFilesystemHandles(b,&handles);
  if(freed!=EFI_SUCCESS)return freed;
  if(matches!=1U)return matches?EFI_UNSUPPORTED:EFI_NOT_FOUND;
#if defined(GU_INTERACTIVE_WINDOWS_ESP) && !defined(GU_RUNTIME_ESP_PIN)
  if(!SelectedEspHandle||matched!=SelectedEspHandle)return EFI_NOT_READY;
#endif
#ifdef GU_RUNTIME_ESP
  {
    /* The volume that carries the boot manager is not necessarily the one the
     * firmware will hand the console to. Ask BootOrder. */
    EFI_DEVICE_PATH_PROTOCOL *mdp=NULL;
    UINTN mbytes=0;BOOLEAN musb=FALSE,mmedia=FALSE;
    if(b->HandleProtocol(matched,&PathGuid,(VOID **)&mdp)!=EFI_SUCCESS||!mdp||
       !Inspect(mdp,&mbytes,&musb,&mmedia))return EFI_UNSUPPORTED;
    EFI_STATUS order=BootOrderNamesVolume(mdp,mbytes);
    if(order==EFI_UNSUPPORTED) {
      GuLog("BOOTORDER=NONE_NAMES_SELECTED_ESP REFUSED=1\n");
      return EFI_UNSUPPORTED;
    }
    if(order!=EFI_SUCCESS)
      GuLog("BOOTORDER=UNCHECKED STATUS=%#llx PROCEEDING=1\n",(unsigned long long)order);
    else
      GuLog("BOOTORDER=NAMES_SELECTED_ESP=1\n");
  }
#endif
  *out=matched;
  return EFI_SUCCESS;
}

static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(const GU_ARCH_ASSET *asset,
                                                    const CHAR16 *path,VOID **out) {
  if(!asset||!path||!out||!Table||!Table->BootServices)return EFI_INVALID_PARAMETER;
  *out=NULL;
  EFI_BOOT_SERVICES *b=Table->BootServices;
  EFI_HANDLE matched=NULL;
  EFI_STATUS s=ConfiguredEspVolume(&matched);
  if(s!=EFI_SUCCESS)return s;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  s=b->HandleProtocol(matched,&FsGuid,(VOID **)&fs);
  if(s!=EFI_SUCCESS||!fs)return EFI_UNSUPPORTED;
  EFI_FILE_PROTOCOL *root=NULL;
  s=fs->OpenVolume(fs,&root);
  s=BootFileOpenResult(s,&root);
  if(s!=EFI_SUCCESS||!root)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  s=ReadFile(root,path,asset->bytes,out);
  EFI_STATUS closed=CloseBootFile(&root);
  if(closed!=EFI_SUCCESS)s=closed;
  if(s==EFI_SUCCESS&&!GuHashMatches(*out,asset->bytes,asset->sha256))
    s=EFI_CRC_ERROR;
  if(s!=EFI_SUCCESS&&*out){GuFree(*out);*out=NULL;}
  return s;
}

EFI_STATUS GuBootLoadAssets(VOID) {
  if(!Root||UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS||Gu.started||Gu.fault||Gu.retained||Gu.asset_load_attempted)
    return EFI_NOT_READY;
  /* Do not overwrite or silently free data published by another owner. */
  for(UINTN i=0;i<GU_ASSET_COUNT;i++)
    if(GuAssets[i].name||GuAssets[i].firmware.data||
       GuAssets[i].firmware.size)return EFI_ALREADY_STARTED;
  const GU_ARCH_ASSETS *arch=GuGetArchAssets();
  if(!arch)return EFI_UNSUPPORTED;
  /* Whether the staged copy is still the bundle this image would load. Purely
   * a report: the load order below is unchanged, and the digest check on every
   * file is what actually decides what may be used. */
  {
    GU_ASSET_INSTALL staged;
    GU_ASSET_REF refs[GU_ASSET_COUNT];
    GU_ASSET_BUNDLE bundle;
    GuAssetBundleFromArch(arch,refs,&bundle);
    EFI_STATUS read=GuAssetInstallRead(Table,&staged);
    UINT32 state=read==EFI_SUCCESS?GuAssetInstallState(&staged,&bundle)
                :(read==EFI_NOT_FOUND?GU_ASSET_INSTALL_ABSENT:GU_ASSET_INSTALL_CORRUPT);
    GuLog("ASSET_INSTALL STATE=%s FILES=%u\n",GuAssetInstallStateName(state),
          (unsigned)GU_ASSET_COUNT);
  }
#ifdef GU_ACTIVE_SINGLE_TARGET
  /* Build an independently owned immutable firmware table first. */
  EFI_STATUS reserved=GuAssetReserveEntries(&Gu,Gu.bs);
  if(reserved!=EFI_SUCCESS)return reserved;
#endif
  Gu.asset_load_attempted=TRUE; /* Latch before the first file operation. */
  EFI_STATUS status=EFI_SUCCESS;
  /* Five firmware files are read and hashed from removable media before any
   * register is touched, and until this trace existed the only line that band
   * could produce was a failure. RUN0011 stopped with `ASSET_INSTALL ...` as its
   * last line and no way to tell this loop from the diagnostic that follows it. */
  GuTracePhase("asset-load");
  TraceAssetReadActive=TRUE;
#ifdef GU_ACTIVE_SINGLE_TARGET
  GspFileCandidate.path=NULL;
#endif
  for(UINTN i=0;i<GU_ASSET_COUNT;i++) {
    const GU_ARCH_ASSET *a=&arch->entries[i];
    VOID *p=NULL;
    GuTraceDetail("asset-read","INDEX=%u NAME=%s BYTES=%u",(unsigned)i,
                  a->name?a->name:"?",(unsigned)a->bytes);
#ifdef GU_ACTIVE_SINGLE_TARGET
    GspFileReadActive=(i==2);
#endif
    status=ReadFile(Root,a->path,a->bytes,&p);
    if(status==EFI_NOT_FOUND&&i==2&&arch->alternate_gsp_path)
      status=ReadFile(Root,arch->alternate_gsp_path,a->bytes,&p);
#ifdef GU_ACTIVE_SINGLE_TARGET
    GspFileReadActive=FALSE;
#endif
    if(status==EFI_NOT_FOUND)
      status=ReadPinnedAssetFromConfiguredEsp(a,a->path,&p);
    if(status==EFI_NOT_FOUND&&i==2&&arch->alternate_gsp_path)
      status=ReadPinnedAssetFromConfiguredEsp(a,arch->alternate_gsp_path,&p);
    if(status!=EFI_SUCCESS) {
      GuLog("Pinned asset unavailable from USB/configured ESP: %s status=%#llx\n",
            a->name,(unsigned long long)status);
      break;
    }
    /* Verify every fallback immediately; do not accept matching filenames
     * from the internal partition without the original signed-data digest. */
#ifdef GU_ACTIVE_SINGLE_TARGET
    UINTN hash_start=0,hash_end=0;
    BOOLEAN hash_timed=i==2&&TraceClock(&hash_start);
#endif
    BOOLEAN hash_ok=GuHashMatches(p,a->bytes,a->sha256);
#ifdef GU_ACTIVE_SINGLE_TARGET
    if(i==2) {
      hash_timed=hash_timed&&TraceClock(&hash_end);
      GuTraceDetail("asset-hash-timing","BYTES=%u SECONDS=%u CLOCK=%u MATCH=%u",
        (unsigned)a->bytes,(unsigned)(hash_timed?(hash_end+86400U-hash_start)%86400U:0),
        (unsigned)hash_timed,(unsigned)hash_ok);
    }
#endif
    if(!hash_ok) {
      GuFree(p);
      status=EFI_CRC_ERROR;
      break;
    }
    GuAssets[i].name=a->name;
    GuAssets[i].firmware=(struct firmware){a->bytes,p};
    GuTraceProgress("asset-read",(UINTN)i+1U,(UINTN)GU_ASSET_COUNT);
  }
  TraceAssetReadActive=FALSE;
  if(status==EFI_SUCCESS) {
    GuTracePhase("asset-verify-bundle");
    if(!GuAd104AssetsValid(&Gu))status=EFI_CRC_ERROR;
  }
  GuTraceDetail("asset-load","STATUS=%#llx",(unsigned long long)status);
  if(status==EFI_SUCCESS&&BootFileStatus!=EFI_SUCCESS)status=BootFileStatus;
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(status==EFI_SUCCESS&&GspFileCandidate.path) {
    GspFileCached=GspFileCandidate;
    GspCacheOwner=&Gu;
    GspCacheManifest=&arch->entries[2];
  }
#endif
  if(status!=EFI_SUCCESS) {
    for(UINTN i=0;i<GU_ASSET_COUNT;i++) {
      if(GuAssets[i].firmware.data)GuFree((VOID *)GuAssets[i].firmware.data);
      GuAssets[i].name=NULL;
      GuAssets[i].firmware=(struct firmware){0,NULL};
    }
  }
  return status;
}

static EFI_STATUS ReadWindowsBinary(EFI_BOOT_SERVICES *bs,EFI_FILE_PROTOCOL *root,const CHAR16 *name,UINTN *file_size,VOID **data) {

  *data=NULL;*file_size=0;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  if(BootFileStatus!=EFI_SUCCESS)return BootFileStatus;

  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);
  s=BootFileOpenResult(s,&f);

  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;

  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);

  EFI_FILE_INFO *info=(VOID *)info_words;

  UINTN sz=0;
  if(s==EFI_SUCCESS) {
    if(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||
       info->FileSize<65536||info->FileSize>33554432||
       (info->Attribute&EFI_FILE_DIRECTORY))
      s=EFI_COMPROMISED_DATA;
    else
      sz=(UINTN)info->FileSize;
  }
  VOID *p=NULL;

  if(s==EFI_SUCCESS)s=AllocateBootBuffer(bs,sz,&p);

  for(UINTN at=0;s==EFI_SUCCESS&&at<sz;) {

    UINTN n=sz-at;if(n>1048576)n=1048576;UINTN wanted=n;

    s=f->Read(f,&n,(UINT8 *)p+at);if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;at+=n;

  }

  EFI_STATUS closed=CloseBootFile(&f);if(closed!=EFI_SUCCESS)s=closed;

  if(s==EFI_SUCCESS&&(((UINT8 *)p)[0]!='M'||((UINT8 *)p)[1]!='Z'))s=EFI_COMPROMISED_DATA;
  if(s!=EFI_SUCCESS) {
    EFI_STATUS released=ReleaseBootBuffer(bs,&p);
    return released==EFI_SUCCESS?s:released;
  }
  *data=p;*file_size=sz;return EFI_SUCCESS;

}

EFI_STATUS GuBootLoadWindows(VOID) {
  if(Windows||!Table||UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS||Gu.started||TargetEspVolume)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Table->BootServices;EFI_HANDLE *h=NULL,volume=NULL;UINTN count=0,matches=0,path_bytes=0;
  EFI_DEVICE_PATH_PROTOCOL *base=NULL;
  const CHAR16 *selected_name=NULL;
#ifdef GU_RUNTIME_ESP_PIN
  GuLog("WINDOWS_ESP_GUID=%s POLICY=EXPLICIT_GUID\n",GU_WINDOWS_ESP_PIN_TEXT);
#elif defined(GU_INTERACTIVE_WINDOWS_ESP)
  EFI_STATUS selection=SelectRuntimeWindowsEsp();
  if(selection!=EFI_SUCCESS)return selection;
#endif
  EFI_STATUS s=LocateBootFilesystems(b,&h,&count);
  if(s!=EFI_SUCCESS)return s;
  EFI_FILE_PROTOCOL *matched_root=NULL;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(h[i]==OwnVolume) continue;
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;UINTN bytes=0;BOOLEAN usb=FALSE,windows=FALSE;
    if(b->HandleProtocol(h[i],&PathGuid,(VOID **)&p)==EFI_SUCCESS&&
       Inspect(p,&bytes,&usb,&windows)&&!usb&&
#ifdef GU_RUNTIME_ESP
       /* The candidate-file probe below is the Windows-ESP test in this build;
        * the volume list itself still has to yield exactly one match. */
       RuntimeEspPathCandidate(p,bytes)
#else
       GuWindowsEspPathMatches((const uint8_t *)p,bytes,(const uint8_t *)&WindowsPartition)
#endif
       ) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *tfs=NULL;EFI_FILE_PROTOCOL *troot=NULL;
      if(b->HandleProtocol(h[i],&FsGuid,(VOID **)&tfs)==EFI_SUCCESS&&tfs) {
        EFI_STATUS opened=tfs->OpenVolume(tfs,&troot);
        opened=BootFileOpenResult(opened,&troot);
        if(BootFileStatus!=EFI_SUCCESS){s=BootFileStatus;break;}
        if(opened!=EFI_SUCCESS||!troot)continue;
        const CHAR16 *found_name=NULL;
        for(UINTN c_idx=0;c_idx<sizeof(WindowsCandidates)/sizeof(WindowsCandidates[0]);c_idx++) {
          EFI_FILE_PROTOCOL *tf=NULL;
          opened=troot->Open(troot,&tf,(CHAR16 *)WindowsCandidates[c_idx],EFI_FILE_MODE_READ,0);
          opened=BootFileOpenResult(opened,&tf);
          if(BootFileStatus!=EFI_SUCCESS){s=BootFileStatus;break;}
          if(opened==EFI_SUCCESS&&tf) {
            s=CloseBootFile(&tf);
            if(s!=EFI_SUCCESS)break;
            found_name=WindowsCandidates[c_idx];
            break;
          }
        }
        if(found_name) {
          if(!matches) {
            volume=h[i]; base=p; path_bytes=bytes; matched_root=troot; troot=NULL;
            selected_name=found_name;
          }
          matches++;
          GuLog("GuBootLoadWindows: candidate %zu volume=%p found '%ls' (match #%zu)\n", i, (void *)h[i], found_name, matches);
        }
        EFI_STATUS closed=CloseBootFile(&troot);
        if(closed!=EFI_SUCCESS)s=closed;
      }
    }
  }
  EFI_STATUS released=ReleaseBootFilesystemHandles(b,&h);
  if(released!=EFI_SUCCESS)s=released;
  if(s!=EFI_SUCCESS||matches<1||!volume||!matched_root||!selected_name) {
    EFI_STATUS closed=CloseBootFile(&matched_root);
    if(closed!=EFI_SUCCESS)s=closed;
    GuLog("GuBootLoadWindows failed: matches=%zu volume=%p s=%#llx\n", (size_t)matches, (void *)volume, (unsigned long long)s);
    return s==EFI_SUCCESS?EFI_NOT_FOUND:s;
  }
  if(matches!=1) {
#ifdef GU_RUNTIME_ESP
    GuLog("GuBootLoadWindows refused: %zu volumes carry a Windows boot manager\n",(size_t)matches);
#else
    GuLog("GuBootLoadWindows refused: pinned partition maps to %zu distinct filesystem handles\n",(size_t)matches);
#endif
    EFI_STATUS closed=CloseBootFile(&matched_root);
    return closed==EFI_SUCCESS?EFI_UNSUPPORTED:closed;
  }
#if defined(GU_INTERACTIVE_WINDOWS_ESP) && !defined(GU_RUNTIME_ESP_PIN)
  if(volume!=SelectedEspHandle) {
    EFI_STATUS closed=CloseBootFile(&matched_root);
    return closed==EFI_SUCCESS?EFI_NOT_READY:closed;
  }
#endif
  EFI_FILE_PROTOCOL *root=matched_root;
  VOID *bytes=NULL;UINTN actual_size=0;
  s=ReadWindowsBinary(b,root,selected_name,&actual_size,&bytes);
  EFI_STATUS closed=CloseBootFile(&root);if(closed!=EFI_SUCCESS)s=closed;
  UINTN name_bytes=(GuStrLen(selected_name)+1)*sizeof(CHAR16);
  UINTN node_bytes=4+name_bytes,total=path_bytes-4+node_bytes+4;
  VOID *path=NULL;
  if(s==EFI_SUCCESS)s=AllocateBootBuffer(b,total,&path);
  if(s==EFI_SUCCESS) {
    memcpy(path,base,path_bytes-4);UINT8 *node=(UINT8 *)path+path_bytes-4;
    node[0]=MEDIA_DEVICE_PATH;node[1]=MEDIA_FILEPATH_DP;node[2]=(UINT8)node_bytes;node[3]=(UINT8)(node_bytes>>8);
    memcpy(node+4,selected_name,name_bytes);node+=node_bytes;
    node[0]=END_DEVICE_PATH_TYPE;node[1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;node[2]=4;node[3]=0;
    s=b->LoadImage(FALSE,Self,(VOID *)path,bytes,actual_size,&Windows);
  }
  /* The image buffer and device path have independent confirmed references.
   * Consume both once even when an earlier release fails. */
  EFI_STATUS bytes_released=ReleaseBootBuffer(b,&bytes);
  EFI_STATUS path_released=ReleaseBootBuffer(b,&path);
  if(bytes_released!=EFI_SUCCESS||path_released!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS) {
    EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
    if(b->HandleProtocol(Windows,&LoadedGuid,(VOID **)&im)==EFI_SUCCESS&&im&&im->DeviceHandle!=volume) {
      im->DeviceHandle=volume;
    }
  }
  if(s==EFI_SUCCESS)TargetEspVolume=volume;
#ifdef GU_RUNTIME_ESP_PIN
  GuLog("Windows image staged: status=%#llx matches=%zu pinned_GPT=1 policy=explicit_runtime_guid GUID=%s path='%ls'\n",(unsigned long long)s,(size_t)matches,GU_WINDOWS_ESP_PIN_TEXT,selected_name);
#elif defined(GU_INTERACTIVE_WINDOWS_ESP)
  GuLog("Windows image staged: status=%#llx matches=%zu pinned_GPT=0 policy=boot_local_choice path='%ls'\n",(unsigned long long)s,(size_t)matches,selected_name);
#elif defined(GU_RUNTIME_ESP)
  GuLog("Windows image staged: status=%#llx matches=%zu pinned_GPT=0 policy=runtime_marker path='%ls'\n",(unsigned long long)s,(size_t)matches,selected_name);
#else
  GuLog("Windows image staged: status=%#llx matches=%zu pinned_GPT=1 path='%ls'\n",(unsigned long long)s,(size_t)matches,selected_name);
#endif
  return s;
}

EFI_STATUS GuBootChoose(CHAR16 *choice) {

  if(!choice||!Table||!Table->ConIn||!Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;

  *choice=L'S';

  GuBootSay(

    L"\r\n=======================================================\r\n"

    L" " GU_FORK_VERSION_WIDE L" (Multi-Gen Edition)\r\n"

    L"=======================================================\r\n"

    L"  [U] Experimental selected GPU transaction (RAM-only)\r\n"

    L"  [P] Experimental GPU transaction with S3 resume script\r\n"

    L"  [I] Install DXE Driver to UEFI NVRAM (Auto-boot / S3)\r\n"

    L"  [R] Remove DXE Driver from UEFI NVRAM\r\n"

    L"  [S] Stock Windows (Skip unlock, normal boot)\r\n\r\n"

    L"Select mode [U/P/I/R/S] (default: Stock Windows in 30s): "

  );

  for(UINTN i=0;i<300;i++) {
    if(i%10==0) {
      UINTN seconds=(300-i+9)/10;
      CHAR16 digits[]=L"00s (S=stock Windows)  ";
      digits[0]=(CHAR16)(L'0'+seconds/10);
      digits[1]=(CHAR16)(L'0'+seconds%10);
      GuBootSay(L"\rStock Windows in ");
      GuBootSay(digits);
    }
    EFI_INPUT_KEY key;EFI_STATUS s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);

    if(s==EFI_SUCCESS) {

      if(key.UnicodeChar=='u'||key.UnicodeChar=='U'){*choice=L'U';return EFI_SUCCESS;}

      if(key.UnicodeChar=='p'||key.UnicodeChar=='P'){*choice=L'P';return EFI_SUCCESS;}

      if(key.UnicodeChar=='i'||key.UnicodeChar=='I'){*choice=L'I';return EFI_SUCCESS;}

      if(key.UnicodeChar=='r'||key.UnicodeChar=='R'){*choice=L'R';return EFI_SUCCESS;}

      if(key.ScanCode==0x17||key.UnicodeChar==27||key.UnicodeChar=='s'||key.UnicodeChar=='S'){*choice=L'S';return EFI_SUCCESS;}

    } else if(s!=EFI_NOT_READY)return s;

    s=Table->BootServices->Stall(100000);if(s!=EFI_SUCCESS)return s;

  }

  return EFI_SUCCESS;

}

EFI_STATUS GuBootClose(VOID) {

  if(Log){
    EFI_STATUS flushed=Log->Flush?Log->Flush(Log):EFI_UNSUPPORTED;
    if(flushed!=EFI_SUCCESS)BootFileStatus=EFI_DEVICE_ERROR;
    (VOID)CloseBootFile(&Log);
  }
  (VOID)CloseBootFile(&Root);
  return BootFileStatus;

}

EFI_STATUS GuBootWindows(VOID) {

  if(!Windows||!Table||Root||Log||UnreleasedFilesystemHandles||BootFileStatus!=EFI_SUCCESS)return EFI_NOT_READY;

  return Table->BootServices->StartImage(Windows,NULL,NULL);

}

VOID GuBootHalt(VOID) {

  GuBootSay(L"\r\nSTOPPED. Power OFF fully; next boot use Windows Boot Manager.\r\n");

  #if defined(_MSC_VER)

  for(;;);

#else

  for(;;)__asm__ volatile("pause");

#endif

}

/* ---- staged firmware bundle (installer) -------------------------------
 *
 * The load path above can only read pinned assets from the media the image was
 * launched from, or - as a pinned fallback - from the one configured Windows
 * ESP. This section is the only code in the image that *writes* to the ESP, and
 * it runs only in the explicit asset-install build (GU_ASSET_INSTALL_ONLY) from
 * a USB-launched image, without an owning GPU. Every file is verified against
 * its pinned digest before it is written, and verified again by reading it back
 * before the record that says "staged" is written. Nothing here changes where
 * the image may be launched from: the removable-media launch rule stands.
 */

/* The directory part of a root-relative asset path ("\AD102-ASSETS\x" ->
 * "\AD102-ASSETS"), so the ESP gets the same layout the loader looks for. */
static BOOLEAN AssetDirectory(const CHAR16 *path,CHAR16 *out,UINTN capacity) {
  UINTN last=0,i=0;
  if(!path||!out||capacity<2)return FALSE;
  for(;path[i];i++)if(path[i]==L'\\'||path[i]==L'/')last=i;
  if(last==0||last+1>capacity)return FALSE;
  for(i=0;i<last;i++)out[i]=path[i];
  out[last]=0;
  return TRUE;
}

static EFI_STATUS EnsureEspDirectory(EFI_FILE_PROTOCOL *root,const CHAR16 *path) {
  EFI_FILE_PROTOCOL *dir=NULL;
  EFI_STATUS s=root->Open(root,&dir,(CHAR16 *)path,
     EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  if(s!=EFI_SUCCESS)return s;
  return dir->Close(dir);
}

/* Stage one verified file. An existing file is accepted only when it already
 * holds exactly the pinned content; a mismatch is refused, never overwritten,
 * because this image has no way to prove that overwriting is safe. */
static EFI_STATUS StageAsset(EFI_FILE_PROTOCOL *esp,const CHAR16 *path,
                             const VOID *data,UINTN bytes,const char *sha256) {
  EFI_FILE_PROTOCOL *file=NULL;
  EFI_STATUS s=esp->Open(esp,&file,(CHAR16 *)path,EFI_FILE_MODE_READ,0);
  if(s==EFI_SUCCESS&&file) {
    UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
    EFI_FILE_INFO *info=(VOID *)info_words;
    EFI_STATUS sized=file->GetInfo(file,&InfoGuid,&info_bytes,info_words);
    BOOLEAN exact=sized==EFI_SUCCESS&&info_bytes>=sizeof(EFI_FILE_INFO)&&
                  info->FileSize==bytes;
    EFI_STATUS closed=file->Close(file);
    if(closed!=EFI_SUCCESS)return closed;
    if(!exact)return EFI_ACCESS_DENIED;
    VOID *existing=NULL;
    s=ReadFile(esp,path,bytes,&existing);
    if(s!=EFI_SUCCESS)return s;
    BOOLEAN match=GuHashMatches(existing,bytes,sha256);
    GuFree(existing);
    return match?EFI_ALREADY_STARTED:EFI_ACCESS_DENIED;
  }
  if(s!=EFI_NOT_FOUND)return s;
  s=esp->Open(esp,&file,(CHAR16 *)path,
              EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(s!=EFI_SUCCESS||!file)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  const UINT8 *source=(const UINT8 *)data;
  for(UINTN done=0;s==EFI_SUCCESS&&done<bytes;) {
    UINTN n=bytes-done;if(n>1048576)n=1048576;UINTN wanted=n;
    s=file->Write(file,&n,(VOID *)(source+done));
    if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;
    done+=n;
  }
  if(s==EFI_SUCCESS&&file->Flush)s=file->Flush(file);
  EFI_STATUS closed=file->Close(file);
  if(s==EFI_SUCCESS)s=closed;
  if(s!=EFI_SUCCESS)return s;
  /* Read back what the firmware actually stored: the record below is a claim
   * about bytes, so it is only written after the bytes are proven. */
  VOID *written=NULL;
  s=ReadFile(esp,path,bytes,&written);
  if(s!=EFI_SUCCESS)return s;
  BOOLEAN match=GuHashMatches(written,bytes,sha256);
  GuFree(written);
  return match?EFI_SUCCESS:EFI_CRC_ERROR;
}

/* Copy the pinned bundle for the selected architecture to the configured ESP and
 * record which bundle it was. Returns the first refusal, and never writes the
 * record when a file could not be staged. */
EFI_STATUS GuAssetInstallStage(VOID) {
  if(!Table||!Root)return EFI_NOT_READY;
  if(!BootFromUsb) {
    GuLog("ASSET_INSTALL=REFUSED REASON=IMAGE_NOT_ON_REMOVABLE_MEDIA\n");
    return EFI_ACCESS_DENIED;
  }
  /* Installing firmware while a card is owned or a transaction is in flight
   * would put a file write in the middle of the destructive path. */
  if(Gu.started||Gu.fault||Gu.retained||Gu.owned||Gu.allocations) {
    GuLog("ASSET_INSTALL=REFUSED REASON=GPU_STATE_NOT_IDLE\n");
    return EFI_NOT_READY;
  }
  const GU_ARCH_ASSETS *arch=GuGetArchAssets();
  if(!arch) {
    GuLog("ASSET_INSTALL=REFUSED REASON=NO_MANIFEST_FOR_TARGET\n");
    return EFI_UNSUPPORTED;
  }
  EFI_STATUS s=ConfiguredEspVolume(&TargetEspVolume);
  if(s!=EFI_SUCCESS) {
    GuLog("ASSET_INSTALL=REFUSED REASON=ESP_NOT_IDENTIFIED STATUS=%#llx\n",
          (unsigned long long)s);
    return s;
  }
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  s=Table->BootServices->HandleProtocol(TargetEspVolume,&FsGuid,(VOID **)&fs);
  if(s!=EFI_SUCCESS||!fs)return EFI_UNSUPPORTED;
  EFI_FILE_PROTOCOL *esp=NULL;
  s=fs->OpenVolume(fs,&esp);
  s=BootFileOpenResult(s,&esp);
  if(s!=EFI_SUCCESS||!esp)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;

  UINTN staged=0,already=0;
  for(UINTN i=0;s==EFI_SUCCESS&&i<GU_ASSET_COUNT;i++) {
    const GU_ARCH_ASSET *a=&arch->entries[i];
    const CHAR16 *path=a->path;
    VOID *p=NULL;
    s=ReadFile(Root,path,a->bytes,&p);
    if(s==EFI_NOT_FOUND&&i==2&&arch->alternate_gsp_path) {
      path=arch->alternate_gsp_path;
      s=ReadFile(Root,path,a->bytes,&p);
    }
    if(s!=EFI_SUCCESS) {
      GuLog("ASSET_INSTALL=REFUSED REASON=SOURCE_UNAVAILABLE FILE=%s STATUS=%#llx\n",
            a->name,(unsigned long long)s);
      break;
    }
    if(!GuHashMatches(p,a->bytes,a->sha256)) {
      GuFree(p);
      GuLog("ASSET_INSTALL=REFUSED REASON=SOURCE_DIGEST_MISMATCH FILE=%s\n",a->name);
      s=EFI_CRC_ERROR;
      break;
    }
    CHAR16 directory[64];
    if(!AssetDirectory(path,directory,sizeof(directory)/sizeof(directory[0]))) {
      GuFree(p);
      s=EFI_INVALID_PARAMETER;
      break;
    }
    EFI_STATUS made=EnsureEspDirectory(esp,directory);
    EFI_STATUS wrote=made==EFI_SUCCESS?StageAsset(esp,path,p,a->bytes,a->sha256):made;
    GuFree(p);
    if(wrote==EFI_ALREADY_STARTED) {already++;continue;}
    if(wrote!=EFI_SUCCESS) {
      GuLog("ASSET_INSTALL=REFUSED FILE=%s STATUS=%#llx EXISTING_MISMATCH=%u\n",
            a->name,(unsigned long long)wrote,(unsigned)(wrote==EFI_ACCESS_DENIED));
      s=wrote;
      break;
    }
    staged++;
  }
  EFI_STATUS closed=CloseBootFile(&esp);
  if(s==EFI_SUCCESS)s=closed;
  if(s!=EFI_SUCCESS)return s;

  GU_ASSET_INSTALL record={0};
  GU_ASSET_REF files[GU_ASSET_COUNT];
  GU_ASSET_BUNDLE bundle;
  GuAssetBundleFromArch(arch,files,&bundle);
  record.Magic=GU_ASSET_INSTALL_MAGIC;
  record.Files=(UINT32)GU_ASSET_COUNT;
  record.BootCount=(UINT64)OpenBootCount+1;
  for(UINTN i=0;i<GU_ASSET_INSTALL_VERSION_CHARS-1&&GU_ASSET_BUNDLE_VERSION[i];i++)
    record.Version[i]=GU_ASSET_BUNDLE_VERSION[i];
  GuAssetInstallBundleDigest(&bundle,record.Bundle);
  if(!record.Bundle[0]) {
    GuLog("ASSET_INSTALL=REFUSED REASON=MANIFEST_DIGEST_UNAVAILABLE\n");
    return EFI_UNSUPPORTED;
  }
  s=GuAssetInstallWrite(Table,&record);
  if(s!=EFI_SUCCESS) {
    GuLog("ASSET_INSTALL=INCOMPLETE FILES=%u STAGED=%u RECORD_STATUS=%#llx\n",
          (unsigned)GU_ASSET_COUNT,(unsigned)staged,(unsigned long long)s);
    return s;
  }
  GuLog("ASSET_INSTALL=STAGED FILES=%u WRITTEN=%u ALREADY=%u BUNDLE=%s BOOT_COUNT=%u\n",
        (unsigned)GU_ASSET_COUNT,(unsigned)staged,(unsigned)already,
        record.Bundle,(unsigned)record.BootCount);
  return EFI_SUCCESS;
}

/* Bounded watchdog.
 *
 * The firmware timer was only ever disabled, because nobody knew how long the
 * sequence takes. This gives it a bound: a build that sets GU_WATCHDOG_SECONDS
 * arms the timer in GuBootOpen, so it covers preparation, the transaction and
 * the driver work, and the handover path cancels it as soon as the outcome is
 * recorded. A hang then reboots the machine instead of leaving a half-written
 * card behind a frozen screen, and nothing is left armed across the handover to
 * Windows.
 *
 * The threshold is a build constant, not a measurement. No timing budget has
 * been captured on hardware yet, so the default is 0, which arms nothing and
 * reproduces today's behaviour exactly - the timer is disabled, as before. The
 * value in force is reported by GuBootWatchdogArmed() and by the two log lines
 * below, so a boot record shows whether a bound existed
 * (docs/PRODUCTION_READINESS_INVENTORY.md 11.12).
 */
static BOOLEAN WatchdogArmed;
static UINTN WatchdogSeconds;

/* Program the firmware timer. A zero bound is the default build: the timer is
 * disabled and nothing is armed, which is the old behaviour exactly. */
static EFI_STATUS WatchdogProgram(UINTN Bound) {
  EFI_BOOT_SERVICES *b;
  EFI_STATUS s;
  if (!Table || !Table->BootServices || !Table->BootServices->SetWatchdogTimer)
    return EFI_UNSUPPORTED;
  b = Table->BootServices;
  if (Bound > (UINTN)GU_WATCHDOG_MAX_SECONDS) {
    GuLog("WATCHDOG=REFUSED SECONDS=%u REASON=IMPLAUSIBLE_THRESHOLD\n",
          (unsigned)Bound);
    return EFI_INVALID_PARAMETER;
  }
  /* 100 ns units; reset code 0 because the transaction owns no runtime data. */
  s = b->SetWatchdogTimer(Bound * 10000000U, 0, 0, NULL);
  if (s == EFI_SUCCESS && Bound) {
    WatchdogArmed = TRUE;
    WatchdogSeconds = Bound;
    GuLog("WATCHDOG=ARMED SECONDS=%u\n", (unsigned)Bound);
  }
  return s;
}

/* Whether this build armed the timer, and for how long. Called by the handover
 * path and by the diagnostic report; it never programs the timer. */
BOOLEAN GuBootWatchdogArmed(UINTN *seconds) {
  if (seconds) *seconds = WatchdogArmed ? WatchdogSeconds : 0;
  return WatchdogArmed;
}

/* Cancel before the handover. A failure here is reported but never masked: the
 * caller is already past the point where the card is consistent. */
EFI_STATUS GuBootWatchdogCancel(VOID) {
  EFI_STATUS s;
  if (!WatchdogArmed) return EFI_SUCCESS;
  if (!Table || !Table->BootServices || !Table->BootServices->SetWatchdogTimer)
    return EFI_UNSUPPORTED;
  s = Table->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
  if (EFI_ERROR(s)) {
    GuLog("WATCHDOG=CANCEL_FAILED SECONDS=%u STATUS=%#llx\n",
          (unsigned)WatchdogSeconds, (unsigned long long)s);
    return s;
  }
  GuLog("WATCHDOG=CANCELLED SECONDS=%u\n", (unsigned)WatchdogSeconds);
  WatchdogArmed = FALSE;
  return EFI_SUCCESS;
}

/* This is the admission read, not the earlier log observation in GuBootOpen.
 * Preparation and Stop-only builds refuse latched records without modifying
 * boot registration. Full transactions leave recovery to their entrypoint. */
EFI_STATUS GuBootReadPriorState(EFI_SYSTEM_TABLE *table,GU_BOOT_STATE *state,
                                BOOLEAN *present) {
  EFI_STATUS status=GuBootStateRead(table,state);
  *present=(status==EFI_SUCCESS);
  if(!*present&&status!=EFI_NOT_FOUND) {
    GuLog("BOOT_STATE=REFUSED READ_STATUS=%#llx GPU_ACTION=0\n",(unsigned long long)status);
    EFI_STATUS closed=GuBootClose();
    return closed==EFI_SUCCESS?status:closed;
  }
#if defined(GU_STOP_RELEASE_ONLY_BENCH) || defined(GU_ACTIVE_SEQUENCE_PREPARE_ONLY)
  if(GuBootStateRequiresRecovery(*present?state:NULL)) {
#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY
    GuLog("PREPARATION_ONLY=REFUSED REASON=PRIOR_FAILED_GPU_STATE NVRAM_MUTATION=0 GPU_ACTION=0\n");
#else
    GuLog("STOP_RELEASE_ONLY=REFUSED REASON=PRIOR_FAILED_GPU_STATE NVRAM_MUTATION=0 GPU_ACTION=0\n");
#endif
    GuBootSay(L"\r\n[REFUSED] Prior GPU state is incomplete or awaits acceptance; no GPU action was taken.\r\n");
    EFI_STATUS closed=GuBootClose();
    return closed==EFI_SUCCESS?EFI_ABORTED:closed;
  }
#endif
  return EFI_SUCCESS;
}
