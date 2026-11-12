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
        for s in (APP,DXE):
            self.assertIn("'gpu_windows_path.c'",s)
    def test_only_report_selected_volume_after_success(self):
        self.assertIn("if(s==EFI_SUCCESS)TargetEspVolume=volume;",BOOT)
        self.assertIn("EFI_HANDLE GuBootGetTargetEspVolume(VOID)",BOOT)
        self.assertIn("const CHAR16 *GuBootGetLogPath(VOID)",BOOT)
        self.assertIn("EFI_HANDLE GuBootGetTargetEspVolume(VOID);",HEADER)
    def test_asset_fallback_is_only_pinned_read_only_esp(self):
        c=BOOT.split("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp(",1)[1].split("EFI_STATUS GuBootLoadAssets(",1)[0]
        self.assertIn("GuWindowsEspPathMatches(",c)
        self.assertIn("(const uint8_t *)&WindowsPartition",c)
        self.assertIn("matches!=1U",c)
        self.assertIn("GuHashMatches(",c)
        self.assertNotIn("SetVariable(",c)
        self.assertNotIn("EFI_FILE_MODE_WRITE",c)
        self.assertNotIn("EFI_FILE_MODE_CREATE",c)
        loader=BOOT.split("EFI_STATUS GuBootLoadAssets(",1)[1].split("static EFI_STATUS ReadWindowsBinary(",1)[0]
        self.assertIn("ReadPinnedAssetFromConfiguredEsp(",loader)
        self.assertIn("GuFree((VOID *)GuAssets[i].firmware.data)",loader)

    def test_preparation_only_usb_log_preserves_uefi_watchdog(self):
        """Verify the original boot-open path keeps active watchdog behavior isolated."""
        begin = BOOT.index("EFI_STATUS GuBootOpen(")
        end = BOOT.index("static EFI_STATUS ReadFile(", begin)
        opened = BOOT[begin:end]
        guard = opened.index("#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY")
        finish_log = opened.index("if(closed!=EFI_SUCCESS)return closed;")
        watchdog = opened.index("if(!b->SetWatchdogTimer)return EFI_UNSUPPORTED;")
        self.assertLess(finish_log, guard)
        self.assertLess(guard, watchdog)
        self.assertEqual(
            opened[guard:watchdog].strip(),
            "#ifdef GU_ACTIVE_SEQUENCE_PREPARE_ONLY\n"
            "  /* Preparation reads PCI identity and writes only its removable USB log.\n"
            "   * Do not change the firmware watchdog or any active boot state. */\n"
            "  return EFI_SUCCESS;\n"
            "#endif")
        self.assertIn("return b->SetWatchdogTimer(0,0,0,NULL);", opened[watchdog:])
        for source in (APP, DXE):
            self.assertIn("/DGU_ACTIVE_SEQUENCE_PREPARE_ONLY", source)

    def test_windows_binary_reader_rejects_invalid_info_lengths(self):
        """Run the actual boot-file reader with independent fake EFI file services."""
        source = BOOT.split("static EFI_STATUS ReadWindowsBinary(", 1)[1].split(
            "\nEFI_STATUS GuBootLoadWindows(", 1)[0]
        production = "static EFI_STATUS ReadWindowsBinary(" + source
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
#define EFI_FILE_MODE_READ 1
#define EFI_FILE_DIRECTORY 0x10
static int InfoGuid,mode,open_calls,info_calls,read_calls,close_calls,alloc_calls;
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *,EFI_FILE_PROTOCOL **,CHAR16 *,UINT64,UINT64);
  EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *,void *,UINTN *,void *);
  EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *,UINTN *,void *);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
};
static EFI_FILE_PROTOCOL file;
static void *GuAlloc(size_t n) {alloc_calls++;return malloc(n);}
static void GuFree(void *p) {free(p);}
static EFI_STATUS Open(EFI_FILE_PROTOCOL *r,EFI_FILE_PROTOCOL **f,CHAR16 *name,
                       UINT64 open_mode,UINT64 attr) {
  (void)r;(void)name;(void)attr;
  open_calls++;
  if(open_mode!=EFI_FILE_MODE_READ)return EFI_DEVICE_ERROR;
  *f=&file;return EFI_SUCCESS;
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
  if(mode<0||mode>10)return 3;
  UINTN size=99;
  void *bytes=(void *)(uintptr_t)1;
  EFI_STATUS s=ReadWindowsBinary(&root,name,&size,&bytes);
  EFI_STATUS expected=(mode==0?EFI_SUCCESS:
                       mode==3||mode==7||mode==10?EFI_DEVICE_ERROR:
                       mode==8?EFI_DEVICE_ERROR:EFI_COMPROMISED_DATA);
  CHECK(s==expected);
  CHECK(open_calls==1 && info_calls==1 && close_calls==1);
  if(mode==0) {
    CHECK(size==65536 && bytes!=NULL && read_calls==1 && alloc_calls==1);
    GuFree(bytes);
  } else {
    CHECK(size==0 && bytes==NULL);
    if(mode>=1&&mode<=6)CHECK(read_calls==0 && alloc_calls==0);
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
                for mode in range(11):
                    with self.subTest(compiler=compiler,mode=mode):
                        result=subprocess.run(
                            [str(binary),str(mode)],capture_output=True,text=True)
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_log_path_requires_open_log(self):
        self.assertIn("return Log?ActiveLogPath:NULL;",BOOT)
        self.assertIn("ActiveLogPath[14+k]=name[k]",BOOT)

if __name__=="__main__":unittest.main()
