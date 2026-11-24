"""Guard exact Windows GPT partition and exported boot/log state."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]/"src"
BOOT=(ROOT/"boot_io.c").read_text(encoding="utf-8")
HEADER=(ROOT/"boot_io.h").read_text(encoding="utf-8")
APP=(ROOT/"Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
DXE=(ROOT/"Build-Dxe-Driver.ps1").read_text(encoding="utf-8")

class BootBinding(unittest.TestCase):
    def test_only_exact_gpt_esp_is_a_candidate(self):
        self.assertIn("GuWindowsEspPathMatches((const uint8_t *)p,bytes,",BOOT)
        self.assertIn("(const uint8_t *)&WindowsPartition",BOOT)
        self.assertIn("matches!=1",BOOT)
        self.assertIn("return EFI_UNSUPPORTED;",BOOT)
        self.assertNotIn('L"\\\\EFI\\\\Boot\\\\bootx64.efi"',BOOT)
        self.assertNotIn('L"EFI\\\\Boot\\\\bootx64.efi"',BOOT)
    def test_reuse_existing_bounded_parser_in_both_builds(self):
        from test_build_source_lists import builder_sources
        for name in ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1"):
            self.assertIn("gpu_windows_path.c", builder_sources(name))
    def test_only_report_selected_volume_after_success(self):
        self.assertIn("if(s==EFI_SUCCESS)TargetEspVolume=volume;",BOOT)
        self.assertIn("EFI_HANDLE GuBootGetTargetEspVolume(VOID)",BOOT)
        self.assertIn("const CHAR16 *GuBootGetLogPath(VOID)",BOOT)
        self.assertIn("EFI_HANDLE GuBootGetTargetEspVolume(VOID);",HEADER)
    def test_asset_fallback_is_only_pinned_read_only_esp(self):
        # One locator decides which volume the ESP is; the reader itself may not
        # write to it, and the installer (a separate, explicit build) may.
        locator=BOOT.split("static EFI_STATUS ConfiguredEspVolume(",1)[1].split("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(",1)[0]
        self.assertIn("GuWindowsEspPathMatches(",locator)
        self.assertIn("(const uint8_t *)&WindowsPartition",locator)
        self.assertIn("matches!=1U",locator)
        c=BOOT.split("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(",1)[1].split("EFI_STATUS GuBootLoadAssets(",1)[0]
        self.assertIn("ConfiguredEspVolume(",c)
        self.assertIn("GuHashMatches(",c)
        self.assertNotIn("SetVariable(",c)
        self.assertNotIn("EFI_FILE_MODE_WRITE",c)
        self.assertNotIn("EFI_FILE_MODE_CREATE",c)
        self.assertNotIn("GuAssetInstallStage(",c)
        loader=BOOT.split("EFI_STATUS GuBootLoadAssets(VOID) {",1)[1].split("static EFI_STATUS ReadWindowsBinary(",1)[0]
        self.assertNotIn("EFI_FILE_MODE_WRITE",loader)
        self.assertNotIn("GuAssetInstallStage(",loader)
        loader=BOOT.split("EFI_STATUS GuBootLoadAssets(",1)[1].split("static EFI_STATUS ReadWindowsBinary(",1)[0]
        self.assertIn("ReadPinnedAssetFromConfiguredEsp(",loader)
        self.assertIn("GuFree((VOID *)GuAssets[i].firmware.data)",loader)

    def test_shared_filesystem_census_refuses_uncertain_release(self):
        """Compile the real enumeration/release helpers with simulated services."""
        production = BOOT.split("static EFI_STATUS ReleaseBootFilesystemHandles(",1)[1].split(
            "/* Only the explicitly configured Windows GPT ESP",1)[0]
        production = "static EFI_STATUS ReleaseBootFilesystemHandles(" + production
        host = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define VOID void
typedef void *EFI_HANDLE;
typedef size_t UINTN;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 1
#define EFI_NOT_READY 2
#define EFI_UNSUPPORTED 3
#define EFI_INVALID_PARAMETER 4
#define EFI_NOT_FOUND 5
#define ByProtocol 2
typedef struct {
  EFI_STATUS (*LocateHandleBuffer)(int,void *,void *,UINTN *,EFI_HANDLE **);
  EFI_STATUS (*FreePool)(void *);
} EFI_BOOT_SERVICES;
static EFI_HANDLE fixture[129];
static EFI_HANDLE *UnreleasedFilesystemHandles;
static EFI_STATUS BootFileStatus;
static int FsGuid,locates,frees,fail_free,fail_locate,null_buffer;
static UINTN supplied=2;
static void *released;
static void GuLog(const char *fmt,...){(void)fmt;}
static EFI_STATUS Locate(int kind,void *guid,void *key,UINTN *count,EFI_HANDLE **out){
  (void)key;
  if(kind!=ByProtocol||guid!=&FsGuid)return EFI_INVALID_PARAMETER;
  locates++;
  if(fail_locate==1)return EFI_NOT_FOUND;
  *count=supplied;*out=null_buffer?NULL:fixture;
  return fail_locate?EFI_NOT_FOUND:EFI_SUCCESS;
}
static EFI_STATUS Free(void *p){
  frees++;released=p;
  /* Even an unexpected NOT_FOUND must not enable the asset fallback. */
  return fail_free?EFI_NOT_FOUND:EFI_SUCCESS;
}
""" + production + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
  EFI_BOOT_SERVICES bs={Locate,Free};
  EFI_HANDLE *h=NULL;UINTN n=0;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_SUCCESS && h==fixture && n==2);
  CHECK(ReleaseBootFilesystemHandles(&bs,&h)==EFI_SUCCESS && !h && frees==1);
  CHECK(ReleaseBootFilesystemHandles(&bs,&h)==EFI_SUCCESS && frees==1);
  fail_locate=1;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_NOT_FOUND && !h && !n && frees==1);
  fail_locate=0;null_buffer=1;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_UNSUPPORTED && !h && !n && frees==1);
  null_buffer=0;supplied=0;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_UNSUPPORTED && !h && !n && frees==2);
  supplied=129;fail_free=1;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_DEVICE_ERROR && !h && !n && frees==3);
  CHECK(UnreleasedFilesystemHandles==fixture && released==fixture);
  fail_free=0;supplied=2;
  int saved_locates=locates;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_NOT_READY && !h && !n);
  CHECK(frees==3&&locates==saved_locates);
  /* New boot fixture: a valid census fails release after use. */
  UnreleasedFilesystemHandles=NULL;fail_free=1;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_SUCCESS);
  CHECK(ReleaseBootFilesystemHandles(&bs,&h)==EFI_DEVICE_ERROR&&!h);
  CHECK(UnreleasedFilesystemHandles==fixture&&released==fixture&&frees==4);
  CHECK(ReleaseBootFilesystemHandles(&bs,&h)==EFI_SUCCESS&&frees==4);
  saved_locates=locates;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_NOT_READY&&locates==saved_locates);
  /* New boot fixture: error with nonnull enumeration output. */
  UnreleasedFilesystemHandles=NULL;fail_free=0;fail_locate=2;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_DEVICE_ERROR&&!h&&!n);
  CHECK(UnreleasedFilesystemHandles==fixture&&frees==4);
  fail_locate=0;saved_locates=locates;
  CHECK(LocateBootFilesystems(&bs,&h,&n)==EFI_NOT_READY&&!h&&!n);
  CHECK(locates==saved_locates&&frees==4);
  CHECK(ReleaseBootFilesystemHandles(&bs,&h)==EFI_SUCCESS&&frees==4);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"fs_census.c"
            path.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                binary=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(path),"-o",str(binary)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

        locator=BOOT.split("static EFI_STATUS ConfiguredEspVolume(",1)[1].split("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(",1)[0]
        self.assertIn("LocateBootFilesystems(b,&handles,&count)",locator)
        # The census must finish releasing before anything reads a pinned asset.
        # Compared over the whole source: `locator` stops at the reader's
        # signature, so it can never contain it.
        self.assertLess(BOOT.index("if(freed!=EFI_SUCCESS)return freed;"),
                        BOOT.index("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp("))
        assets=BOOT.split("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(",1)[1].split("EFI_STATUS GuBootLoadAssets(",1)[0]
        self.assertIn("ConfiguredEspVolume(&matched)",assets)
        self.assertLess(assets.index("ConfiguredEspVolume(&matched)"),assets.index("fs->OpenVolume("))
        windows=BOOT.split("EFI_STATUS GuBootLoadWindows(",1)[1].split("EFI_STATUS GuBootChoose(",1)[0]
        self.assertIn("UnreleasedFilesystemHandles",windows)
        self.assertIn("LocateBootFilesystems(b,&h,&count)",windows)
        release=windows.index("released=ReleaseBootFilesystemHandles(b,&h);")
        self.assertLess(release,windows.index("if(released!=EFI_SUCCESS)s=released;"))
        refusal=windows.index("return s==EFI_SUCCESS?EFI_NOT_FOUND:s;",release)
        self.assertLess(refusal,windows.index("ReadWindowsBinary("))
        self.assertLess(refusal,windows.index("b->LoadImage("))

    def test_preparation_only_usb_log_preserves_uefi_watchdog(self):
        """Verify the original boot-open path keeps active watchdog behavior isolated."""
        begin = BOOT.index("EFI_STATUS GuBootOpen(")
        end = BOOT.index("static EFI_STATUS ReadFile(", begin)
        opened = BOOT[begin:end]
        guard = opened.index("#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY")
        finish_log = opened.index("if(closed!=EFI_SUCCESS){s=closed;goto failed;}")
        watchdog = opened.index("s=WatchdogProgram((UINTN)GU_WATCHDOG_SECONDS);")
        self.assertLess(finish_log, guard)
        self.assertLess(guard, watchdog)
        guarded=opened[guard:watchdog].strip()
        self.assertTrue(guarded.startswith("#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY"))
        self.assertIn(
            "  /* Preparation reads PCI identity and writes only its removable USB log.\n"
            "   * Do not change the firmware watchdog or any active boot state. */\n"
            "  return EFI_SUCCESS;\n"
            "#endif", guarded)
        # The guard returns before the timer is touched, whatever the bound is.
        self.assertNotIn("SetWatchdogTimer", guarded)
        # The old unconditional disable lives in WatchdogProgram now, reached only
        # with a zero bound, so the prepare-only return still wins.
        program=BOOT.split("static EFI_STATUS WatchdogProgram(UINTN Bound) {",1)[1].split("\n}\n",1)[0]
        self.assertIn("SetWatchdogTimer(Bound * 10000000U, 0, 0, NULL)", program)
        self.assertIn("if (s == EFI_SUCCESS && Bound)", program)
        self.assertIn("GU_ACTIVE_SEQUENCE_PREPARE_ONLY", opened)
        self.assertIn("EFI_STATUS released=GuBootClose();",opened[watchdog:])
        for source in (APP, DXE):
            self.assertIn("/DGU_ACTIVE_SEQUENCE_PREPARE_ONLY", source)

    def test_shared_file_close_failure_is_sticky(self):
        """Exercise actual close helpers, including failure after log flush."""
        close = BOOT.split("static EFI_STATUS RecordBootFileFailure(",1)[1].split("\nEFI_STATUS GuBootOpen(",1)[0]
        boot_close=BOOT.split("EFI_STATUS GuBootClose(VOID) {",1)[1].split("\nEFI_STATUS GuBootWindows(",1)[0]
        actual="static EFI_STATUS RecordBootFileFailure("+close+"\nEFI_STATUS GuBootClose(VOID) {"+boot_close
        host=r"""
#include <stddef.h>
#include <stdio.h>
#define VOID void
typedef size_t UINTN;
typedef int EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_DEVICE_ERROR 1
#define EFI_UNSUPPORTED 2
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
  EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
  int failed,flush_failed,closes,flushes;
};
static EFI_FILE_PROTOCOL *Root,*Log;
static EFI_FILE_PROTOCOL *UnreleasedBootFiles[5];
static UINTN UnreleasedBootFileCount;
static EFI_STATUS BootFileStatus;
static EFI_STATUS Close(EFI_FILE_PROTOCOL *f){f->closes++;return f->failed?EFI_UNSUPPORTED:EFI_SUCCESS;}
static EFI_STATUS Flush(EFI_FILE_PROTOCOL *f){f->flushes++;return f->flush_failed?EFI_UNSUPPORTED:EFI_SUCCESS;}
""" +actual+r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
  for(unsigned flags=0;flags<8;flags++){
    EFI_FILE_PROTOCOL root={Close,Flush,(int)(flags&1),0,0,0};
    EFI_FILE_PROTOCOL log={Close,Flush,(int)(flags&2),(int)(flags&4),0,0};
    Root=&root;Log=&log;BootFileStatus=EFI_SUCCESS;UnreleasedBootFileCount=0;
    EFI_STATUS expected=flags?EFI_DEVICE_ERROR:EFI_SUCCESS;
    CHECK(GuBootClose()==expected);
    CHECK(!Root&&!Log&&root.closes==1&&log.closes==1&&log.flushes==1);
    CHECK(UnreleasedBootFileCount==(UINTN)(!!(flags&1)+!!(flags&2)));
    if(flags&2)CHECK(UnreleasedBootFiles[0]==&log);
    if(flags&1)CHECK(UnreleasedBootFiles[UnreleasedBootFileCount-1]==&root);
    CHECK(GuBootClose()==expected);
    CHECK(root.closes==1&&log.closes==1&&log.flushes==1);
  }
  /* Separate boot: an unconfirmed result cannot be closed. Confirmed
   * root and log references still get their single cleanup attempt. */
  EFI_FILE_PROTOCOL root={Close,Flush,0,0,0,0};
  EFI_FILE_PROTOCOL log={Close,Flush,0,0,0,0};
  EFI_FILE_PROTOCOL uncertain={Close,Flush,0,0,0,0};
  Root=&root;Log=&log;BootFileStatus=EFI_SUCCESS;UnreleasedBootFileCount=0;
  EFI_FILE_PROTOCOL *out=NULL;
  CHECK(BootFileOpenResult(EFI_UNSUPPORTED,&out)==EFI_UNSUPPORTED&&!out);
  CHECK(BootFileStatus==EFI_SUCCESS&&!UnreleasedBootFileCount);
  out=&uncertain;
  CHECK(BootFileOpenResult(EFI_UNSUPPORTED,&out)==EFI_DEVICE_ERROR&&!out);
  CHECK(UnreleasedBootFileCount==1&&UnreleasedBootFiles[0]==&uncertain);
  CHECK(CloseBootFile(&out)==EFI_SUCCESS&&!uncertain.closes);
  CHECK(GuBootClose()==EFI_DEVICE_ERROR&&!Root&&!Log);
  CHECK(root.closes==1&&log.closes==1&&!uncertain.closes);
  CHECK(GuBootClose()==EFI_DEVICE_ERROR&&root.closes==1&&log.closes==1);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"file_close.c"
            path.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                binary=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(path),"-o",str(binary)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        windows=BOOT.split("EFI_STATUS GuBootLoadWindows(",1)[1].split("EFI_STATUS GuBootChoose(",1)[0]
        self.assertIn("s=CloseBootFile(&tf);\n            if(s!=EFI_SUCCESS)break;",windows)
        self.assertIn("if(closed!=EFI_SUCCESS)s=closed;",windows)
        self.assertIn("if(released!=EFI_SUCCESS)s=released;",windows)
        self.assertLess(windows.index("if(s!=EFI_SUCCESS||matches<1"),windows.index("b->LoadImage("))
        self.assertIn("BootFileStatus!=EFI_SUCCESS",BOOT.split("EFI_STATUS GuBootWindows(",1)[1])
        import re
        # Every open result must be handled, not necessarily wrapped in
        # BootFileOpenResult: the inline sites test the status immediately.
        # Counting is what let a stale pin hide (9 became 15 when the staged
        # bundle added readers), so the property is checked instead.
        lines=BOOT.splitlines()
        opens=[index for index,line in enumerate(lines) if re.search(r"->(?:Open|OpenVolume)\(",line)]
        self.assertGreaterEqual(len(opens),9)
        for index in opens:
            handled="\n".join(lines[index:index+3])
            self.assertRegex(handled, r"BootFileOpenResult\(|!=EFI_SUCCESS|==EFI_SUCCESS",
                             f"open at src/boot_io.c:{index+1} is not checked")
        # Boot-wide staging must not allocate from either GPU's CPU pool.
        reader=BOOT.split("static EFI_STATUS ReadWindowsBinary(",1)[1].split("EFI_STATUS GuBootLoadWindows(",1)[0]
        for source in (reader,windows):
            self.assertNotIn("GuAlloc(",source)
            self.assertNotIn("GuFree(",source)
        self.assertIn("AllocateBootBuffer(bs,sz,&p)",reader)
        self.assertIn("AllocateBootBuffer(b,total,&path)",windows)
        self.assertLess(windows.index("b->LoadImage("),windows.index("ReleaseBootBuffer(b,&bytes)"))
        self.assertLess(windows.index("ReleaseBootBuffer(b,&bytes)"),windows.index("ReleaseBootBuffer(b,&path)"))
        self.assertLess(windows.index("if(bytes_released!=EFI_SUCCESS||path_released!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;"),windows.index("TargetEspVolume=volume"))

    def test_windows_binary_reader_rejects_invalid_info_lengths(self):
        """Run the actual boot-file reader with independent fake EFI file services."""
        source = BOOT.split("static EFI_STATUS ReadWindowsBinary(", 1)[1].split(
            "\nEFI_STATUS GuBootLoadWindows(", 1)[0]
        close = BOOT.split("static EFI_STATUS RecordBootFileFailure(",1)[1].split("\nEFI_STATUS GuBootOpen(",1)[0]
        buffers=BOOT.split("static EFI_STATUS BootBufferFailure(",1)[1].split("\nstatic EFI_STATUS ReadFile(",1)[0]
        production = "static EFI_STATUS RecordBootFileFailure(" + close + "\nstatic EFI_STATUS BootBufferFailure(" + buffers + "\nstatic EFI_STATUS ReadWindowsBinary(" + source
        host = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char UINT8;
typedef uint16_t CHAR16;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef int EFI_STATUS;
typedef struct {UINT64 Size,FileSize,PhysicalSize;
  UINT8 CreateTime[16],LastAccessTime[16],ModificationTime[16];
  UINT64 Attribute;CHAR16 FileName[1];} EFI_FILE_INFO;
#define VOID void
#define EFI_SUCCESS 0
#define EFI_COMPROMISED_DATA 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_DEVICE_ERROR 4
#define EFI_NOT_FOUND 5
#define EFI_UNSUPPORTED 6
#define EFI_INVALID_PARAMETER 7
#define EfiLoaderData 1
#define EFI_FILE_MODE_READ 1
#define EFI_FILE_DIRECTORY 0x10
static int InfoGuid,mode,open_calls,info_calls,read_calls,close_calls,alloc_calls,free_calls;
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *,EFI_FILE_PROTOCOL **,CHAR16 *,UINT64,UINT64);
  EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *,void *,UINTN *,void *);
  EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *,UINTN *,void *);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
};
static EFI_FILE_PROTOCOL file;
static EFI_FILE_PROTOCOL *UnreleasedBootFiles[5];
static UINTN UnreleasedBootFileCount;
static EFI_STATUS BootFileStatus;
static void *UnreleasedBootBuffers[2];
static UINTN UnreleasedBootBufferCount;
typedef struct {
  EFI_STATUS (*AllocatePool)(int,UINTN,void **);
  EFI_STATUS (*FreePool)(void *);
} EFI_BOOT_SERVICES;
static EFI_STATUS Allocate(int type,UINTN n,void **out) {
  if(type!=EfiLoaderData)return EFI_INVALID_PARAMETER;
  alloc_calls++;
  if(mode==11)return EFI_OUT_OF_RESOURCES;
  if(mode==12)return EFI_SUCCESS;
  *out=malloc(n);
  if(mode==13&&*out)return EFI_OUT_OF_RESOURCES;
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(void *p) {
  free_calls++;
  if(mode==14||mode==15)return EFI_DEVICE_ERROR;
  free(p);return EFI_SUCCESS;
}
static EFI_STATUS Open(EFI_FILE_PROTOCOL *r,EFI_FILE_PROTOCOL **f,CHAR16 *name,
                       UINT64 open_mode,UINT64 attr) {
  (void)r;(void)name;(void)attr;
  open_calls++;
  if(open_mode!=EFI_FILE_MODE_READ)return EFI_DEVICE_ERROR;
  if(mode==17)return EFI_NOT_FOUND;
  *f=&file;return mode==16?EFI_NOT_FOUND:EFI_SUCCESS;
}
static EFI_STATUS GetInfo(EFI_FILE_PROTOCOL *f,void *guid,UINTN *size,void *buffer) {
  (void)f;(void)guid;info_calls++;
  if(mode==3)return EFI_DEVICE_ERROR;
  EFI_FILE_INFO *info=(EFI_FILE_INFO *)buffer;
  memset(info,0,sizeof(*info));
  info->FileSize=65536;
  if(mode==4)info->FileSize=65535;
  if(mode==5)info->FileSize=33554433;
  if(mode==6)info->Attribute=EFI_FILE_DIRECTORY;
  if(mode==1)*size=1025; /* Larger than reader's 1024-byte local buffer. */
  else if(mode==2)*size=sizeof(EFI_FILE_INFO)-1;
  else *size=sizeof(EFI_FILE_INFO);
  return EFI_SUCCESS;
}
static EFI_STATUS Read(EFI_FILE_PROTOCOL *f,UINTN *n,void *out) {
  (void)f;read_calls++;
  if(mode==7)return EFI_DEVICE_ERROR;
  if(mode==8){*n=0;return EFI_SUCCESS;}
  memset(out,0,*n);
  if(mode!=9){((UINT8 *)out)[0]='M';((UINT8 *)out)[1]='Z';}
  return EFI_SUCCESS;
}
static EFI_STATUS Close(EFI_FILE_PROTOCOL *f) {
  (void)f;close_calls++;
  return mode==10?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
""" + production + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d mode %d: %s\n",__LINE__,mode,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  static CHAR16 name[]={'b','o','o','t',0};
  static EFI_FILE_PROTOCOL root={Open,NULL,NULL,NULL};
  file=(EFI_FILE_PROTOCOL){NULL,GetInfo,Read,Close};
  mode=argc>1?atoi(argv[1]):-1;
  if(mode<0||mode>17)return 3;
  EFI_BOOT_SERVICES bs={Allocate,Free};
  if(mode==15) {
    void *a=NULL,*b=NULL,*c=NULL;
    CHECK(AllocateBootBuffer(&bs,8,&a)==EFI_SUCCESS);
    CHECK(AllocateBootBuffer(&bs,16,&b)==EFI_SUCCESS);
    void *saved_a=a,*saved_b=b;
    CHECK(ReleaseBootBuffer(&bs,&a)==EFI_DEVICE_ERROR&&!a);
    CHECK(ReleaseBootBuffer(&bs,&b)==EFI_DEVICE_ERROR&&!b);
    CHECK(free_calls==2&&UnreleasedBootBufferCount==2);
    CHECK(UnreleasedBootBuffers[0]==saved_a&&UnreleasedBootBuffers[1]==saved_b);
    CHECK(ReleaseBootBuffer(&bs,&a)==EFI_SUCCESS&&free_calls==2);
    CHECK(AllocateBootBuffer(&bs,8,&c)==EFI_DEVICE_ERROR&&!c&&alloc_calls==2);
    free(saved_a);free(saved_b); /* Host fixture teardown. */
    return 0;
  }
  UINTN size=99;
  void *bytes=(void *)(uintptr_t)1;
  EFI_STATUS s=ReadWindowsBinary(&bs,&root,name,&size,&bytes);
  if(mode==16||mode==17) {
    CHECK(s==(mode==16?EFI_DEVICE_ERROR:EFI_NOT_FOUND)&&!size&&!bytes);
    CHECK(open_calls==1&&!info_calls&&!read_calls&&!close_calls&&!alloc_calls&&!free_calls);
    if(mode==16) {
      CHECK(BootFileStatus==EFI_DEVICE_ERROR&&UnreleasedBootFileCount==1&&UnreleasedBootFiles[0]==&file);
      CHECK(ReadWindowsBinary(&bs,&root,name,&size,&bytes)==EFI_DEVICE_ERROR);
      CHECK(open_calls==1&&!close_calls);
    }else CHECK(BootFileStatus==EFI_SUCCESS&&!UnreleasedBootFileCount);
    return 0;
  }
  EFI_STATUS expected=(mode==0||mode==14?EFI_SUCCESS:
                       mode==3||mode==7||mode==8||mode==10||mode==13?EFI_DEVICE_ERROR:
                       mode==11||mode==12?EFI_OUT_OF_RESOURCES:EFI_COMPROMISED_DATA);
  CHECK(s==expected);
  CHECK(open_calls==1 && info_calls==1 && close_calls==1);
  if(mode==10) {
    CHECK(BootFileStatus==EFI_DEVICE_ERROR&&UnreleasedBootFileCount==1&&UnreleasedBootFiles[0]==&file);
    CHECK(ReadWindowsBinary(&bs,&root,name,&size,&bytes)==EFI_DEVICE_ERROR);
    CHECK(open_calls==1&&close_calls==1);
  }
  if(mode==0||mode==14) {
    CHECK(size==65536 && bytes!=NULL && read_calls==1 && alloc_calls==1);
    void *saved=bytes;
    CHECK(ReleaseBootBuffer(&bs,&bytes)==(mode==14?EFI_DEVICE_ERROR:EFI_SUCCESS));
    CHECK(!bytes&&free_calls==1);
    if(mode==14) {
      CHECK(BootFileStatus==EFI_DEVICE_ERROR&&UnreleasedBootBufferCount==1&&UnreleasedBootBuffers[0]==saved);
      CHECK(ReleaseBootBuffer(&bs,&bytes)==EFI_SUCCESS&&free_calls==1);
      CHECK(AllocateBootBuffer(&bs,16,&bytes)==EFI_DEVICE_ERROR&&!bytes&&alloc_calls==1);
      free(saved); /* Host fixture teardown. */
    }
  } else {
    CHECK(size==0 && bytes==NULL);
    if(mode>=1&&mode<=6)CHECK(read_calls==0 && alloc_calls==0);
    if(mode>=11)CHECK(alloc_calls==1&&read_calls==0&&!free_calls);
    if(mode==13) {
      CHECK(BootFileStatus==EFI_DEVICE_ERROR&&UnreleasedBootBufferCount==1&&UnreleasedBootBuffers[0]);
      CHECK(ReadWindowsBinary(&bs,&root,name,&size,&bytes)==EFI_DEVICE_ERROR);
      CHECK(open_calls==1&&alloc_calls==1&&!free_calls);
      free(UnreleasedBootBuffers[0]); /* Host fixture teardown. */
    }
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "boot_file_read.c"
            path.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                binary=Path(directory)/("boot_file_read_"+compiler)
                built=subprocess.run(
                    [os.environ.get("CC",compiler) if compiler=="gcc" else compiler,
                     "-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",
                     str(path),"-o",str(binary)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                for mode in range(18):
                    with self.subTest(compiler=compiler,mode=mode):
                        result=subprocess.run(
                            [str(binary),str(mode)],capture_output=True,text=True)
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_passive_msi_and_topology_handoff_require_durable_log_and_pinned_esp(self):
        builder=(ROOT/"Build-Gpu-Topology-Diagnostic.ps1").read_text(encoding="utf-8")
        source=(ROOT/"gpu_topology_efi.c").read_text(encoding="utf-8")
        handover=(ROOT/"gpu_windows_handoff.c").read_text(encoding="utf-8")
        self.assertIn("'-WindowsEspGuid requires -SaveLog",builder)
        # Every opt-in source diagnostic must reject PCI selection, including
        # the read-only block probe.
        self.assertIn(
            "($MsiB850RomVariables -or $MsiB850FuseVersion -or $MsiB850Sec2Ucode1 -or $VramBlockProbe) -and $selection.Count",
            builder)
        for option in ("$MsiB850RomVariables.IsPresent", "$MsiB850FuseVersion.IsPresent",
                       "$MsiB850Sec2Ucode1.IsPresent", "$VramBlockProbe.IsPresent"):
            self.assertIn(option,builder)
        self.assertIn(".Where({ $_ }).Count -gt 1",builder)
        self.assertIn("MSI B850 diagnostics must be built separately.",builder)
        self.assertNotIn("($selection.Count -or $WindowsEspGuid)",builder)
        self.assertIn("if ($selection.Count -and $WindowsEspGuid)",builder)
        self.assertIn("if ($MsiB850Sec2Ucode1 -and -not $SaveLog)",builder)
        self.assertIn("if (@($MsiB850RomVariables.IsPresent, $MsiB850FuseVersion.IsPresent, $MsiB850Sec2Ucode1.IsPresent, $VramBlockProbe.IsPresent).Where({ $_ }).Count -gt 1)",builder)
        self.assertIn("$sources += 'gpu_windows_handoff.c'",builder)
        self.assertIn("if ($WindowsEspGuid) { $flags += ' /DGU_TOPO_WINDOWS_HANDOFF' }",builder)
        for name in ("GPU-TOPOLOGY-WINDOWS-HANDOFF-DIAGNOSTIC.EFI",
                     "MSI-B850-ROM-VARIABLES-WINDOWS-HANDOFF-DIAGNOSTIC.EFI",
                     "MSI-B850-FUSE-VERSION-WINDOWS-HANDOFF-DIAGNOSTIC.EFI",
                     "MSI-B850-SEC2-UCODE1-WINDOWS-HANDOFF-DIAGNOSTIC.EFI"):
            with self.subTest(name=name):
                self.assertIn(name,builder)
        msi=source.split('#include "msi_b850_rom_diag.h"',1)[1].split("#endif",1)[0]
        self.assertIn("defined(GU_TOPO_SELECTION_ENABLED)",msi)
        self.assertNotIn("defined(GU_TOPO_WINDOWS_HANDOFF)",msi)
        entry=source.split("EFI_STATUS EFIAPI UefiMain(",1)[1]
        saved=entry.index("EFI_STATUS written=SaveLog(image,table);")
        fail=entry.index("if(EFI_ERROR(written))return written;",saved)
        start=entry.index("GuWindowsBootManager(image,table)",fail)
        self.assertLess(saved,fail)
        self.assertLess(fail,start)
        self.assertIn("#ifdef GU_TOPO_WINDOWS_HANDOFF",entry[fail:start])
        self.assertNotIn("result==GU_TOPO_COMPLETE",entry[fail:start])
        self.assertIn("if(matches!=1)",handover)
        self.assertIn("GuWindowsEspPathMatches(",handover)
        self.assertIn("StartImage(prepared,NULL,NULL)",handover)
        self.assertNotIn("DisconnectController(",entry)
        self.assertNotIn("SetVariable(",entry)

    def test_log_path_requires_open_log(self):
        self.assertIn("return Log?ActiveLogPath:NULL;",BOOT)
        self.assertIn("ActiveLogPath[14+k]=name[k]",BOOT)

if __name__=="__main__":unittest.main()
