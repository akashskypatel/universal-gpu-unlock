"""One shared transaction contract for app and DXE source.

This rejects divergent sequences, missing pre-Windows checks, and early
firmware use. Hardware behavior is verified separately.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]/"src"
SHARED=(ROOT/"unlock_execution.c").read_text(encoding="utf-8")
APP=(ROOT/"app.c").read_text(encoding="utf-8")
DXE=(ROOT/"dxe_main.c").read_text(encoding="utf-8")

def body(source,name):
    m=re.search(r"\b(?:EFI_STATUS|VOID)\s+(?:EFIAPI\s+)?"+name+r"\s*\([^;]*?\)\s*\{",source,re.S)
    if not m:raise AssertionError("missing function "+name)
    start=m.end();pos=start;depth=1
    while depth and pos<len(source):
        depth+=(source[pos]=="{")-(source[pos]=="}");pos+=1
    if depth:raise AssertionError("unclosed "+name)
    return source[start:pos-1]

class SharedExecutionContract(unittest.TestCase):
    def test_complete_order(self):
        core=body(SHARED,"GuExecuteUnlockCore")
        ops=["GuMsiPreflight(","GuMsiRelease(","GuMsiReleased(","GuAd104ColdEntry(",
             "gpu_unlock_610_load_shared(","gpu_unlock_610_unlock(",
             "gpu_unlock_610_release_shared(","GuSafeToReturn(","GuMsiFinish(",
             "GuMsiReadyForWindows("]
        indices=[core.index(op) for op in ops]
        self.assertEqual(indices,sorted(indices))
        self.assertIn("*stage=",core)
        self.assertIn("result||Gu.fault",core)
    def test_selected_step_is_wrapped_around_real_handover(self):
        wrapper=body(SHARED,"GuExecuteUnlock")
        begin=wrapper.index("GuBeginSelectedGpuStep(")
        core=wrapper.index("GuExecuteUnlockCore(")
        finish=wrapper.index("GuFinishSelectedGpuStep(")
        self.assertLess(begin,core)
        self.assertLess(core,finish)
        self.assertIn("GuMsiReadyForWindows()",wrapper)
        self.assertIn("GuSafeToReturn()",wrapper)
        self.assertEqual(wrapper.count("GuAbortSelectedGpuStep()"),2)
        discovery=(ROOT/"gpu_discovery.c").read_text(encoding="utf-8")
        self.assertIn("GuGpuExecInit(&candidate",discovery)
        self.assertLess(discovery.index("GuGpuExecInit(&candidate"),
                        discovery.index("s=GuInit(table->BootServices,io);"))
        self.assertIn("GuGpuExecBeginOwned(&ActiveSequence",discovery)
        self.assertIn("GuMsiPlanOwnerToken()",wrapper)
        self.assertIn("GuFinishSelectedGpuStep(table,",wrapper)
        self.assertEqual(wrapper.count("GuMsiPlanOwnerToken()"),2)
        self.assertEqual(wrapper.count("GuAssets"),2)
        self.assertEqual(wrapper.count("&Gu"),2)
        finish_adapter=body(discovery,"GuFinishSelectedGpuStep")
        for check in ("!platform", "!release_plan", "!firmware_assets",
                      ".owners.platform!=platform",
                      ".owners.release_plan!=release_plan",
                      ".owners.firmware_assets!=firmware_assets"):
            self.assertIn(check,finish_adapter)
        self.assertIn("GuMsiReserveSelectedState(",wrapper)
        self.assertLess(wrapper.index("GuMsiReserveSelectedState("),begin)
        self.assertIn("GuAssets",wrapper)
        self.assertIn("GuGpuExecFinish(&ActiveSequence",discovery)
        self.assertIn("GuGpuExecAbort(&ActiveSequence)",discovery)

    def test_both_entrypoints_share_transaction(self):
        for src,name in [(APP,"UefiMain"),(DXE,"GuDxeRun")]:
            with self.subTest(name=name):
                code=body(src,name)
                self.assertEqual(code.count("GuExecuteUnlock("),1)
                for old in ("GuMsiRelease(","gpu_unlock_610_unlock(","GuMsiFinish("):
                    self.assertNotIn(old,code)
    def test_actual_s3_source_rejects_unverified_or_partial_script(self):
        """Compile the production S3 function with read-only fake EFI callbacks."""
        s3_source = "EFI_STATUS GuRegisterS3(EFI_SYSTEM_TABLE *table) {\n" + (
            body(SHARED, "GuRegisterS3")) + "\n}"
        mock = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
typedef void VOID;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef uint64_t EFI_STATUS;
typedef int BOOLEAN;
typedef struct { int dummy; } EFI_GUID;
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_NOT_READY 2
#define EFI_ALREADY_STARTED 3
#define EFI_UNSUPPORTED 4
#define EFI_DEVICE_ERROR 5
#define EFI_NOT_FOUND 6
#define EFI_S3_SAVE_STATE_PROTOCOL_GUID {0}
#define EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE 11
#define EfiBootScriptWidthUint32 3
typedef struct MOCK_SCRIPT EFI_S3_SAVE_STATE_PROTOCOL;
struct MOCK_SCRIPT {
  EFI_STATUS (*Write)(EFI_S3_SAVE_STATE_PROTOCOL *,int,int,UINT64,UINTN,const UINT32 *);
};
typedef struct MOCK_BOOT_SERVICES {
  EFI_STATUS (*LocateProtocol)(EFI_GUID *,VOID *,VOID **);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
static struct {UINT64 bar_base,bar_size;int fault,retained;} Gu;
static int ready,safe=1,available=1,fail_at,write_calls,locate_calls;
static UINT64 first_address,second_address;
static UINT32 first_value,second_value;
static EFI_STATUS Write(EFI_S3_SAVE_STATE_PROTOCOL *script,int opcode,int width,
                        UINT64 address,UINTN count,const UINT32 *value) {
  (void)script;
  if(opcode!=EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE || width!=EfiBootScriptWidthUint32 ||
     count!=1 || !value)return EFI_DEVICE_ERROR;
  write_calls++;
  if(write_calls==1){first_address=address;first_value=*value;}
  if(write_calls==2){second_address=address;second_value=*value;}
  return fail_at==write_calls?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_S3_SAVE_STATE_PROTOCOL script={Write};
static EFI_STATUS Locate(EFI_GUID *guid,VOID *registration,VOID **out) {
  (void)guid;(void)registration;
  locate_calls++;
  if(!available)return EFI_NOT_FOUND;
  *out=&script;return EFI_SUCCESS;
}
static BOOLEAN GuSafeToReturn(VOID) {return safe && !Gu.retained;}
static BOOLEAN GuMsiReadyForWindows(VOID) {return ready;}
static void GuLog(const char *fmt,...) {(void)fmt;}
""" + s3_source + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  int mode=argc>1?atoi(argv[1]):-1;
  static EFI_BOOT_SERVICES services={Locate};
  static EFI_SYSTEM_TABLE table={&services};
  Gu.bar_base=0x10000000ULL;Gu.bar_size=0x1000000ULL;
  ready=1;
  if(mode==0){
    ready=0;CHECK(GuRegisterS3(&table)==EFI_NOT_READY);
    CHECK(!write_calls && !locate_calls);
  } else if(mode==1){
    CHECK(GuRegisterS3(&table)==EFI_SUCCESS);
    CHECK(write_calls==2 && locate_calls==1 && !Gu.retained);
    CHECK(first_address==Gu.bar_base+0x82381cULL && first_value==0x88888888U);
    CHECK(second_address==Gu.bar_base+0x823820ULL && second_value==8U);
    CHECK(GuRegisterS3(&table)==EFI_ALREADY_STARTED && write_calls==2);
  } else if(mode==2){
    fail_at=1;CHECK(GuRegisterS3(&table)==EFI_DEVICE_ERROR);
    CHECK(write_calls==1 && Gu.retained);
    CHECK(GuRegisterS3(&table)==EFI_NOT_READY && write_calls==1);
  } else if(mode==3){
    fail_at=2;CHECK(GuRegisterS3(&table)==EFI_DEVICE_ERROR);
    CHECK(write_calls==2 && Gu.retained);
  } else if(mode==4){
    available=0;CHECK(GuRegisterS3(&table)==EFI_NOT_FOUND);
    CHECK(!write_calls && !Gu.retained);
    available=1;CHECK(GuRegisterS3(&table)==EFI_SUCCESS && write_calls==2);
  } else if(mode==5){
    Gu.bar_size=0;CHECK(GuRegisterS3(&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==6){
    Gu.bar_base=UINT64_MAX-0x82381dULL;
    CHECK(GuRegisterS3(&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==7){
    safe=0;CHECK(GuRegisterS3(&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==8){
    script.Write=NULL;CHECK(GuRegisterS3(&table)==EFI_UNSUPPORTED);
    CHECK(!Gu.retained && !write_calls);
  } else if(mode==9){
    services.LocateProtocol=NULL;
    CHECK(GuRegisterS3(&table)==EFI_UNSUPPORTED && !write_calls);
  } else if(mode==10){
    Gu.fault=1;CHECK(GuRegisterS3(&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==11){
    Gu.retained=1;CHECK(GuRegisterS3(&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==12){
    CHECK(GuRegisterS3(NULL)==EFI_NOT_READY && !locate_calls);
  } else return 2;
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "s3_source.c"
            source.write_text(mock, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                output = Path(directory) / ("s3_source_" + compiler)
                built = subprocess.run(
                    [compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                     "-Werror", "-pedantic", str(source), "-o", str(output)],
                    capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                for mode in range(13):
                    with self.subTest(compiler=compiler, mode=mode):
                        result = subprocess.run([str(output), str(mode)],
                                                capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0,
                                         result.stdout + result.stderr)

    def test_s3_is_optional_and_after_handover(self):
        for src,name in [(APP,"UefiMain"),(DXE,"GuDxeRun")]:
            code=body(src,name)
            self.assertLess(code.index("GuExecuteUnlock("),code.index("GuRegisterS3("))
        s3=body(SHARED,"GuRegisterS3")
        self.assertIn("GuSafeToReturn()",s3)
        self.assertIn("GuMsiReadyForWindows()",s3)
        self.assertIn("EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE",s3)
        self.assertIn("Gu.retained=TRUE",s3)
        self.assertLess(s3.index("attempted=TRUE"),s3.index("s3->Write("))
        self.assertLess(s3.index("if(a!=EFI_SUCCESS)"),s3.index("EFI_STATUS b=s3->Write("))
        for source in (APP,DXE):
            self.assertIn("if(Gu.retained)",source)
            self.assertIn("s3==EFI_SUCCESS?EFI_DEVICE_ERROR:s3",source)
        dxe=body(DXE,"GuDxeRun")
        after_s3=dxe[dxe.index("GuRegisterS3("):]
        self.assertIn("!GuMsiReadyForWindows() || !GuSafeToReturn()",after_s3)
        self.assertIn("s=EFI_NOT_READY;",after_s3)

if __name__=="__main__":unittest.main()
