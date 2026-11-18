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
    m=re.search(r"\b(?:EFI_STATUS|VOID|BOOLEAN)\s+(?:EFIAPI\s+)?"+name+r"\s*\([^;]*?\)\s*\{",source,re.S)
    if not m:raise AssertionError("missing function "+name)
    start=m.end();pos=start;depth=1
    while depth and pos<len(source):
        depth+=(source[pos]=="{")-(source[pos]=="}");pos+=1
    if depth:raise AssertionError("unclosed "+name)
    return source[start:pos-1]

class SharedExecutionContract(unittest.TestCase):
    def test_complete_order(self):
        core=body(SHARED,"GuExecuteUnlockCore")
        ops=["GuMsiPreflight(","GuCompatReserveDevice(","GuMsiRelease(","GuMsiReleased(","GuAd104ColdEntryFor(",
             "GuCompatOpenDevice(","gpu_unlock_610_load_shared(","gpu_unlock_610_unlock(",
             "gpu_unlock_610_release_shared(","GuSafeToReturnFor(","GuMsiFinish(",
             "GuMsiReadyForWindows("]
        indices=[core.index(op) for op in ops]
        self.assertEqual(indices,sorted(indices))
        self.assertIn("*stage=",core)
        self.assertIn("result||owner!=GuPlatformActive||owner->fault",core)
        self.assertIn("GuCompatOwner(dev)!=owner",core)
        self.assertLess(core.index("owner!=GuPlatformActive"),core.index("GuMsiPreflight("))
    def test_selected_step_is_wrapped_around_real_handover(self):
        wrapper=body(SHARED,"GuExecuteUnlock")
        begin=wrapper.index("GuBeginSelectedGpuStep(")
        core=wrapper.index("GuExecuteUnlockCore(")
        finish=wrapper.index("GuFinishSelectedGpuStep(")
        self.assertLess(begin,core)
        self.assertLess(core,finish)
        self.assertIn("GuMsiReadyForWindows()",wrapper)
        self.assertIn("GuSafeToReturnFor(owner)",wrapper)
        self.assertEqual(wrapper.count("GuAbortSelectedGpuStep()"),2)
        discovery=(ROOT/"gpu_discovery.c").read_text(encoding="utf-8")
        self.assertIn("GuGpuExecInit(&candidate",discovery)
        self.assertLess(discovery.index("GuGpuExecInit(&candidate"),
                        discovery.index("s=GuInit(table->BootServices,io);"))
        self.assertIn("GuGpuExecBeginOwned(&ActiveSequence",discovery)
        self.assertIn("GuMsiPlanOwnerToken()",wrapper)
        self.assertIn("GuFinishSelectedGpuStep(table,",wrapper)
        self.assertEqual(wrapper.count("GuMsiPlanOwnerToken()"),2)
        self.assertEqual(wrapper.count("owner->asset_entries"),2)
        self.assertIn("GuBeginSelectedGpuStep(table,owner,",wrapper)
        self.assertIn("GuFinishSelectedGpuStep(table,ready,owner,",wrapper)
        self.assertLess(wrapper.index("owner!=GuPlatformActive"),wrapper.index("GuMsiReserveSelectedState("))
        finish_adapter=body(discovery,"GuFinishSelectedGpuStep")
        for check in ("!platform", "!release_plan", "!firmware_assets",
                      ".owners.platform!=platform",
                      ".owners.release_plan!=release_plan",
                      ".owners.firmware_assets!=firmware_assets"):
            self.assertIn(check,finish_adapter)
        self.assertIn("GuMsiReserveSelectedState(",wrapper)
        self.assertLess(wrapper.index("GuMsiReserveSelectedState("),begin)
        self.assertIn("owner->asset_entries",wrapper)
        self.assertIn("GuGpuExecFinish(&ActiveSequence",discovery)
        self.assertIn("GuGpuExecAbort(&ActiveSequence)",discovery)

    def test_both_entrypoints_share_transaction(self):
        for src,name in [(APP,"UefiMain"),(DXE,"GuDxeRun")]:
            with self.subTest(name=name):
                code=body(src,name)
                self.assertEqual(code.count("GuExecutePreparedSequence("),1)
                self.assertLess(code.index("GuPrepareBoundGpu("),code.index("GuExecutePreparedSequence("))
                self.assertNotIn("GuExecuteUnlock(",code)
                self.assertNotIn("GuRegisterS3(",code)
                for old in ("GuMsiRelease(","gpu_unlock_610_unlock(","GuMsiFinish("):
                    self.assertNotIn(old,code)
    def test_actual_s3_source_rejects_unverified_or_partial_script(self):
        """Compile the production S3 function with read-only fake EFI callbacks."""
        s3_source = "EFI_STATUS GuRegisterS3(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table) {\n" + (
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
#define GU_BAR_MIN_BYTES 0x1000000ULL
typedef struct MOCK_SCRIPT EFI_S3_SAVE_STATE_PROTOCOL;
struct MOCK_SCRIPT {
  EFI_STATUS (*Write)(EFI_S3_SAVE_STATE_PROTOCOL *,int,int,UINT64,UINTN,const UINT32 *);
};
typedef struct MOCK_BOOT_SERVICES {
  EFI_STATUS (*LocateProtocol)(EFI_GUID *,VOID *,VOID **);
} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
typedef struct {EFI_BOOT_SERVICES *bs;UINT64 bar_base,bar_size;int fault,retained,s3_script_attempted;} GU_PLATFORM;
static GU_PLATFORM Gu,peer,*GuPlatformActive=&Gu;
static int drift_at;
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
  if(drift_at==write_calls)GuPlatformActive=&peer;
  return fail_at==write_calls?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_S3_SAVE_STATE_PROTOCOL script={Write};
static EFI_STATUS Locate(EFI_GUID *guid,VOID *registration,VOID **out) {
  (void)guid;(void)registration;
  locate_calls++;
  if(!available)return EFI_NOT_FOUND;
  *out=&script;
  if(drift_at==3)GuPlatformActive=&peer;
  return EFI_SUCCESS;
}
static BOOLEAN GuSafeToReturnFor(const GU_PLATFORM *owner) {return safe && !owner->retained;}
static BOOLEAN GuMsiReadyForWindows(VOID) {return ready;}
static void GuLog(const char *fmt,...) {(void)fmt;}
""" + s3_source + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(int argc,char **argv) {
  int mode=argc>1?atoi(argv[1]):-1;
  static EFI_BOOT_SERVICES services={Locate};
  static EFI_SYSTEM_TABLE table={&services};
  Gu.bs=peer.bs=&services;
  Gu.bar_base=0x10000000ULL;Gu.bar_size=0x1000000ULL;
  peer.bar_base=0x20000000ULL;peer.bar_size=0x1000000ULL;
  ready=1;
  if(mode==0){
    ready=0;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY);
    CHECK(!write_calls && !locate_calls);
  } else if(mode==1){
    CHECK(GuRegisterS3(&Gu,&table)==EFI_SUCCESS);
    CHECK(write_calls==2 && locate_calls==1 && !Gu.retained);
    CHECK(first_address==Gu.bar_base+0x82381cULL && first_value==0x88888888U);
    CHECK(second_address==Gu.bar_base+0x823820ULL && second_value==8U);
    CHECK(GuRegisterS3(&Gu,&table)==EFI_ALREADY_STARTED && write_calls==2);
  } else if(mode==2){
    fail_at=1;CHECK(GuRegisterS3(&Gu,&table)==EFI_DEVICE_ERROR);
    CHECK(write_calls==1 && Gu.retained);
    CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY && write_calls==1);
  } else if(mode==3){
    fail_at=2;CHECK(GuRegisterS3(&Gu,&table)==EFI_DEVICE_ERROR);
    CHECK(write_calls==2 && Gu.retained);
  } else if(mode==4){
    available=0;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_FOUND);
    CHECK(!write_calls && !Gu.retained);
    available=1;CHECK(GuRegisterS3(&Gu,&table)==EFI_SUCCESS && write_calls==2);
  } else if(mode==5){
    Gu.bar_size=0;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==6){
    Gu.bar_base=UINT64_MAX-0x82381dULL;
    CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==7){
    safe=0;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==8){
    script.Write=NULL;CHECK(GuRegisterS3(&Gu,&table)==EFI_UNSUPPORTED);
    CHECK(!Gu.retained && !write_calls);
  } else if(mode==9){
    services.LocateProtocol=NULL;
    CHECK(GuRegisterS3(&Gu,&table)==EFI_UNSUPPORTED && !write_calls);
  } else if(mode==10){
    Gu.fault=1;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==11){
    Gu.retained=1;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY && !locate_calls);
  } else if(mode==12){
    CHECK(GuRegisterS3(&Gu,NULL)==EFI_NOT_READY && !locate_calls);
  } else if(mode==13){
    CHECK(GuRegisterS3(&peer,&table)==EFI_NOT_READY);
    CHECK(!locate_calls&&!write_calls&&!Gu.s3_script_attempted&&!peer.s3_script_attempted);
    GuPlatformActive=&peer;
    CHECK(GuRegisterS3(&peer,&table)==EFI_SUCCESS);
    CHECK(write_calls==2&&peer.s3_script_attempted&&!Gu.s3_script_attempted);
    CHECK(first_address==peer.bar_base+0x82381cULL&&second_address==peer.bar_base+0x823820ULL);
  } else if(mode==14){
    drift_at=3;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY);
    CHECK(locate_calls==1&&!write_calls&&!Gu.s3_script_attempted&&!peer.s3_script_attempted);
  } else if(mode==15){
    drift_at=1;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY);
    CHECK(write_calls==1&&Gu.retained&&Gu.s3_script_attempted&&!peer.retained&&!peer.s3_script_attempted);
  } else if(mode==16){
    drift_at=2;CHECK(GuRegisterS3(&Gu,&table)==EFI_NOT_READY);
    CHECK(write_calls==2&&Gu.retained&&Gu.s3_script_attempted&&!peer.retained&&!peer.s3_script_attempted);
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
                for mode in range(17):
                    with self.subTest(compiler=compiler, mode=mode):
                        result = subprocess.run([str(output), str(mode)],
                                                capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0,
                                         result.stdout + result.stderr)

    def test_prepared_sequence_preserves_s3_policy_and_stops_on_failure(self):
        """Compile the real preparation and coordinator with simulated services."""
        actual = "EFI_STATUS GuPrepareBoundGpu(GU_PLATFORM *owner,const char **stage) {\n" + body(SHARED,"GuPrepareBoundGpu") + "\n}\n"
        actual += "EFI_STATUS GuExecutePreparedSequence(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table, EFI_HANDLE image, GU_S3_POLICY policy,const char **stage) {\n" + body(SHARED,"GuExecutePreparedSequence") + "\n}\n"
        mock = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GU_ACTIVE_SINGLE_TARGET 1
#define VOID void
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 2
#define EFI_DEVICE_ERROR 3
#define EFI_NOT_READY 4
#define EFI_INVALID_PARAMETER 5
typedef int EFI_STATUS;
typedef void *EFI_HANDLE;
typedef struct {void *BootServices;} EFI_SYSTEM_TABLE;
typedef enum {GU_S3_DISABLED,GU_S3_REQUIRED,GU_S3_OPTIONAL} GU_S3_POLICY;
typedef struct {
  void *handle,*pci;
  unsigned fault,retained,assets,diagnostic,profile,completed,script;
} GU_PLATFORM;
static GU_PLATFORM gpus[4],*active;
#define GuPlatformActive active
#define Gu (*active)
static int current,count,scenario,failed_at,aborted,violations,final_calls,select_calls;
static int executions,loads,diagnostics,profiles,script_calls;
static void GuLog(const char *fmt,...) {(void)fmt;}
static void GuAbortSelectedGpuStep(void){aborted=1;}
static EFI_STATUS GuBootLoadAssets(void){
  loads++;
  if(aborted||Gu.assets||Gu.completed||!Gu.pci)violations++;
  if(current==failed_at&&scenario==1)return EFI_NOT_FOUND;
  Gu.assets=1;
  if(current==failed_at&&scenario==2)Gu.fault=1;
  return EFI_SUCCESS;
}
static EFI_STATUS GuDiagnosticScan(GU_PLATFORM *owner){
  diagnostics++;
  if(owner!=active||!Gu.assets||Gu.diagnostic||Gu.completed)violations++;
  Gu.diagnostic=1;
  if(current==failed_at&&scenario==3)Gu.fault=1;
  return EFI_SUCCESS;
}
static EFI_STATUS GuAd104Prepare(void){
  profiles++;
  if(!Gu.diagnostic||Gu.profile||Gu.completed)violations++;
  if(current==failed_at&&scenario==4)return EFI_NOT_READY;
  Gu.profile=1;
  if(current==failed_at&&scenario==5)Gu.fault=1;
  return EFI_SUCCESS;
}
static EFI_STATUS GuExecuteUnlock(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table,EFI_HANDLE image,const char **stage){
  (void)table;(void)image;*stage="transaction";executions++;
  if(owner!=active||aborted||!Gu.profile||Gu.completed||Gu.fault||Gu.retained)violations++;
  if(current==failed_at&&scenario==6)return EFI_DEVICE_ERROR;
  Gu.completed=1;
  if(current==failed_at&&scenario==7)Gu.fault=1;
  return EFI_SUCCESS;
}
static EFI_STATUS GuRegisterS3(GU_PLATFORM *owner,EFI_SYSTEM_TABLE *table){
  (void)table;script_calls++;
  if(owner!=active||!Gu.completed||Gu.script)violations++;
  if(current==failed_at&&scenario==8)return EFI_NOT_FOUND;
  if(current==failed_at&&scenario==9){Gu.retained=1;return EFI_DEVICE_ERROR;}
  if(current==failed_at&&scenario==10){Gu.retained=1;return EFI_SUCCESS;}
  if(current==failed_at&&scenario==11){Gu.fault=1;return EFI_SUCCESS;}
  Gu.script=1;
  return EFI_SUCCESS;
}
static EFI_STATUS GuSelectedSequenceHasNext(EFI_SYSTEM_TABLE *table,int *more){
  (void)table;
  if(!Gu.completed||Gu.fault||Gu.retained)violations++;
  if(current==failed_at&&scenario==12)return EFI_NOT_READY;
  *more=current+1<count;
  return EFI_SUCCESS;
}
static EFI_STATUS GuSelectNextGpu(EFI_SYSTEM_TABLE *table){
  (void)table;select_calls++;
  if(!Gu.completed||current+1>=count||Gu.retained||Gu.fault)violations++;
  if(current==failed_at&&scenario==13)return EFI_NOT_READY;
  if(current==failed_at&&scenario==15)return EFI_SUCCESS; /* No new owner published. */
  active=&gpus[++current];
  if(Gu.assets||Gu.completed||Gu.profile||Gu.script)violations++;
  return EFI_SUCCESS;
}
static int GuExecutionReadyForWindows(EFI_SYSTEM_TABLE *table){
  (void)table;final_calls++;
  if(current!=count-1||aborted)violations++;
  for(int i=0;i<count;i++)if(!gpus[i].completed||gpus[i].fault||gpus[i].retained)violations++;
  return scenario!=14;
}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d count=%d scenario=%d policy=%d: %s\n",__LINE__,count,scenario,policy,#x);return 1;}} while(0)
int main(void){
  EFI_SYSTEM_TABLE table={&table};
  for(count=1;count<=4;count++)for(int policy=0;policy<=2;policy++)for(scenario=0;scenario<=15;scenario++){
    memset(gpus,0,sizeof(gpus));
    current=0;active=&gpus[0];aborted=violations=final_calls=select_calls=0;
    executions=loads=diagnostics=profiles=script_calls=0;
    /* For a multi-card sequence exercise failures after the first card completed.
     * Selection failure occurs on the first card, before binding its successor. */
    failed_at=count>1&&scenario!=13&&scenario!=15?1:0;
    for(int i=0;i<count;i++){gpus[i].handle=&gpus[i];gpus[i].pci=&gpus[i].pci;}
    const char *stage="";
    CHECK(GuPrepareBoundGpu(&gpus[1],&stage)==EFI_NOT_READY);
    CHECK(GuExecutePreparedSequence(&gpus[1],&table,&table,(GU_S3_POLICY)policy,&stage)==EFI_NOT_READY);
    CHECK(!loads&&!diagnostics&&!profiles&&!executions&&!script_calls&&!select_calls&&!final_calls&&!aborted);
    EFI_STATUS init=GuPrepareBoundGpu(active,&stage);
    if(count==1&&scenario>=1&&scenario<=5){
      CHECK(init!=EFI_SUCCESS && executions==0 && final_calls==0);
      continue;
    }
    CHECK(init==EFI_SUCCESS);
    EFI_STATUS status=GuExecutePreparedSequence(active,&table,&table,(GU_S3_POLICY)policy,&stage);
    int success=scenario==0||(policy==GU_S3_DISABLED&&scenario>=8&&scenario<=11)||
       (policy==GU_S3_OPTIONAL&&scenario==8)||(count==1&&(scenario==13||scenario==15));
    CHECK((status==EFI_SUCCESS)==success);
    CHECK(!violations);
    CHECK(aborted==!success);
    if(success){
      CHECK(loads==count&&diagnostics==count&&profiles==count&&executions==count);
      CHECK(select_calls==count-1&&final_calls==1);
      CHECK(script_calls==(policy==GU_S3_DISABLED?0:count));
      for(int i=0;i<count;i++){
        CHECK(gpus[i].completed);
        CHECK(gpus[i].script==(policy!=GU_S3_DISABLED&&!(scenario==8&&i==failed_at)));
      }
    }else{
      CHECK(final_calls==(scenario==14?1:0));
      CHECK(current==failed_at || scenario==14);
      if(scenario!=14){
        for(int i=current+1;i<count;i++)CHECK(!gpus[i].assets&&!gpus[i].completed);
        if(current>0)CHECK(gpus[0].completed&&gpus[0].profile);
      }
      if(scenario>=1&&scenario<=5)CHECK(executions==failed_at);
      if(scenario>=8&&scenario<=11)CHECK(executions==failed_at+1);
    }
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src=Path(directory)/"sequence.c"
            src.write_text(mock,encoding="utf-8")
            for compiler in ("gcc","clang"):
                binary=Path(directory)/compiler
                compiled=subprocess.run(
                    [compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(src),"-o",str(binary)],
                    capture_output=True,text=True)
                self.assertEqual(compiled.returncode,0,compiled.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_failure_decision_retains_prior_transaction(self):
        actual = "int GuExecutionNeedsContainment(void) {\n" + body(SHARED,"GuExecutionNeedsContainment") + "\n}"
        mock = r"""
#include <stdio.h>
#include <string.h>
#define GU_ACTIVE_SINGLE_TARGET 1
#define TRUE 1
#define VOID void
static struct {
  int started,retained,owned,master_attributes_pending,allocations,dma_count;
  int s3_script_attempted,flrs,mmio_writes,release_owner_invalid;
} Gu;
static int previous,mutated;
static int GuAnyPlatformNeedsContainment(void){return previous;}
static int GuMsiMutated(void){return mutated;}
""" + actual + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void){
  CHECK(!GuExecutionNeedsContainment());
  previous=1;CHECK(GuExecutionNeedsContainment());previous=0;
  mutated=1;CHECK(GuExecutionNeedsContainment());mutated=0;
  Gu.started=1;CHECK(GuExecutionNeedsContainment());Gu.started=0;
  Gu.retained=1;CHECK(GuExecutionNeedsContainment());Gu.retained=0;
  Gu.owned=1;CHECK(GuExecutionNeedsContainment());Gu.owned=0;
  Gu.master_attributes_pending=1;CHECK(GuExecutionNeedsContainment());Gu.master_attributes_pending=0;
  Gu.allocations=1;CHECK(GuExecutionNeedsContainment());Gu.allocations=0;
  Gu.dma_count=1;CHECK(GuExecutionNeedsContainment());Gu.dma_count=0;
  Gu.s3_script_attempted=1;CHECK(GuExecutionNeedsContainment());Gu.s3_script_attempted=0;
  Gu.flrs=1;CHECK(GuExecutionNeedsContainment());Gu.flrs=0;
  Gu.mmio_writes=1;CHECK(GuExecutionNeedsContainment());Gu.mmio_writes=0;
  Gu.release_owner_invalid=1;CHECK(GuExecutionNeedsContainment());Gu.release_owner_invalid=0;
  CHECK(!GuExecutionNeedsContainment());
  return 0;
}
"""
        for src,name in [(APP,"UefiMain"),(DXE,"GuDxeRun")]:
            code=body(src,name)
            self.assertIn("GuExecutionNeedsContainment()",code)
            self.assertIn("Gu.started&&Gu.owned",code.replace(" ",""))
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/"failure.c"
            source.write_text(mock,encoding="utf-8")
            for compiler in ("gcc","clang"):
                binary=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(source),"-o",str(binary)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                ran=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)

    def test_s3_is_optional_and_after_handover(self):
        loop=body(SHARED,"GuExecutePreparedSequence")
        self.assertLess(loop.index("GuExecuteUnlock("),loop.index("GuRegisterS3("))
        self.assertLess(loop.index("GuRegisterS3("),loop.index("GuSelectNextGpu("))
        self.assertLess(loop.index("GuSelectNextGpu("),loop.index("GuPrepareBoundGpu("))
        self.assertLess(loop.index("GuPrepareBoundGpu("),loop.index("GuExecutionReadyForWindows("))
        self.assertIn("enable_s3?GU_S3_REQUIRED:GU_S3_DISABLED",APP)
        self.assertIn("GuExecutePreparedSequence(owner,t,image,GU_S3_OPTIONAL",DXE)
        s3=body(SHARED,"GuRegisterS3")
        self.assertIn("GuSafeToReturnFor(owner)",s3)
        self.assertLess(s3.index("owner!=GuPlatformActive"),s3.index("s3->Write("))
        self.assertIn("GuMsiReadyForWindows()",s3)
        self.assertIn("EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE",s3)
        self.assertIn("owner->retained=TRUE",s3)
        self.assertLess(s3.index("owner->s3_script_attempted=TRUE"),s3.index("s3->Write("))
        self.assertLess(s3.index("if(a!=EFI_SUCCESS)"),s3.index("EFI_STATUS b=s3->Write("))
        self.assertIn("owner!=GuPlatformActive||owner->retained||owner->fault||",loop)
        self.assertIn("(policy==GU_S3_REQUIRED&&s3!=EFI_SUCCESS)",loop)
        self.assertIn("s3==EFI_SUCCESS?EFI_DEVICE_ERROR:s3",loop)
        self.assertIn("!GuExecutionReadyForWindows(table)",loop)
        self.assertIn("s=EFI_NOT_READY;",loop)
        self.assertIn("GuAbortSelectedGpuStep()",loop)
        final=body(SHARED,"GuExecutionReadyForWindows")
        self.assertIn("GuMsiReadyForWindows()&&GuSafeToReturn()&&!Gu.fault",final)
        self.assertIn("GuVerifySelectedSequence(table)==EFI_SUCCESS",final)

if __name__=="__main__":unittest.main()
