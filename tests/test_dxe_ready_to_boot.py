"""Source contract for late DXE entry and one-shot GPU preparation.

This is an offline order check, not an EFI event or GPU execution test.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/dxe_main.c").read_text(encoding="utf-8")


def function_body(name):
    match = re.search(
        r"\b(?:static\s+)?(?:EFI_STATUS|VOID)\s+(?:EFIAPI\s+)?" +
        re.escape(name) + r"\s*\([^)]*\)\s*\{", SOURCE
    )
    if not match:
        raise AssertionError(f"required function missing: {name}")
    depth, start, offset = 1, match.end(), match.end()
    while depth and offset < len(SOURCE):
        depth += (SOURCE[offset] == "{") - (SOURCE[offset] == "}")
        offset += 1
    if depth:
        raise AssertionError(f"invalid function bounds: {name}")
    return SOURCE[start:offset-1]


class ReadyToBootOrderTests(unittest.TestCase):
    def test_entrypoint_registers_event_only(self):
        entry = function_body("AdaUnlockDriverEntryPoint")
        self.assertIn("CreateEventEx(", entry)
        self.assertIn("&ReadyToBootGroup", entry)
        self.assertIn("GuDxeOnReadyToBoot", entry)
        self.assertNotIn("GuDxeRun(", entry)
        self.assertNotIn("GuInit(", entry)
        self.assertNotIn("GuMsiRelease(", entry)
        self.assertNotIn("GuBootOpen(", entry)

    def test_callback_one_shot_and_closes_event_before_work(self):
        notify = function_body("GuDxeOnReadyToBoot")
        self.assertIn("if(DxeExecuted", notify)
        self.assertLess(notify.index("DxeExecuted=TRUE"), notify.index("GuDxeRun("))
        self.assertLess(notify.index("CloseEvent("), notify.index("GuDxeRun("))

    def test_callback_refuses_uncertain_close_and_never_retries(self):
        """Compile the actual event callback against simulated boot services."""
        actual="static VOID EFIAPI GuDxeOnReadyToBoot(EFI_EVENT event, VOID *context) {\n"+function_body("GuDxeOnReadyToBoot")+"\n}"
        host=r"""
#include <stddef.h>
#include <stdio.h>
#define VOID void
#define EFIAPI
#define TRUE 1
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED 1
#define EFI_DEVICE_ERROR 2
typedef int EFI_STATUS;
typedef void *EFI_EVENT;
typedef void *EFI_HANDLE;
typedef struct {EFI_STATUS (*CloseEvent)(EFI_EVENT);} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
static EFI_HANDLE DxeImage;
static EFI_SYSTEM_TABLE *DxeTable;
static EFI_EVENT DxeEvent;
static int DxeExecuted;
static EFI_STATUS DxeEventCloseStatus;
static int closes,runs,fail;
static EFI_STATUS Close(EFI_EVENT event){
  if(event!=DxeEvent)return EFI_DEVICE_ERROR;
  closes++;return fail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS GuDxeRun(EFI_HANDLE image,EFI_SYSTEM_TABLE *table){
  if(image!=DxeImage||table!=DxeTable||DxeEvent||!DxeExecuted)return EFI_DEVICE_ERROR;
  runs++;return EFI_SUCCESS;
}
""" +actual+r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
  int token=0,foreign=0;
  EFI_BOOT_SERVICES bs={Close};
  EFI_SYSTEM_TABLE table={&bs};
  for(int mode=0;mode<4;mode++){
    DxeEvent=&token;DxeImage=&token;DxeTable=&table;DxeExecuted=0;
    DxeEventCloseStatus=EFI_SUCCESS;closes=runs=0;fail=mode==1;
    table.BootServices=mode==3?NULL:&bs;bs.CloseEvent=mode==2?NULL:Close;
    GuDxeOnReadyToBoot(&foreign,NULL);CHECK(!DxeExecuted&&!closes&&!runs);
    GuDxeOnReadyToBoot(&token,NULL);
    CHECK(DxeExecuted&&closes==(mode<2)&&runs==(mode==0));
    CHECK(DxeEvent==(mode?&token:NULL));
    CHECK(DxeEventCloseStatus==(mode==0?EFI_SUCCESS:mode==1?EFI_DEVICE_ERROR:EFI_UNSUPPORTED));
    bs.CloseEvent=Close;table.BootServices=&bs;fail=0;
    GuDxeOnReadyToBoot(&token,NULL);
    CHECK(closes==(mode<2)&&runs==(mode==0));
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src=Path(directory)/"event.c"
            src.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                exe=Path(directory)/compiler
                built=subprocess.run([compiler,"-std=c11","-O2","-Wall","-Wextra","-Werror","-pedantic",str(src),"-o",str(exe)],capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        entry=function_body("AdaUnlockDriverEntryPoint")
        self.assertIn("if(DxeEvent)DxeExecuted=TRUE;",entry)

    def test_failed_success_path_close_reaches_containment(self):
        body = function_body("GuDxeRun")
        close = body.index("s=GuBootClose();")
        success = body.index("if(s==EFI_SUCCESS)return s;",close)
        failure = body.index("if (s != EFI_SUCCESS)",success)
        self.assertLess(close,success)
        self.assertLess(success,failure)
        self.assertLess(failure,body.index("GuExecutionNeedsContainment()",failure))
        self.assertIn('stage="dxe-close-USB-log";',body)

    def test_combined_dxe_final_graph_and_containment_contract(self):
        body = function_body("GuDxeRun")
        self.assertIn("GuExecutePreparedSequence(owner,t,image,GU_S3_OPTIONAL", body)
        self.assertLess(body.index("GuCompletePreparedSequence(s,&owner)"), body.index("GuExecutionReadyForWindows(t)"))
        self.assertLess(body.index("GuExecutionReadyForWindows(t)"), body.index('stage="dxe-close-USB-log"'))
        self.assertNotIn("GuBootWindows(", body)
        self.assertNotIn("GuBootLoadWindows(", body)
        containment = body[body.index("FIRMWARE_REATTACH_CONTAINMENT=HALT"):body.index("#else", body.index("FIRMWARE_REATTACH_CONTAINMENT=HALT"))]
        self.assertIn("GuBootHalt()", containment)
        self.assertNotIn("ResetSystem", containment)

    def test_original_active_guards_retained_in_actual_work(self):
        body = function_body("GuDxeRun")
        self.assertIn("GU_ACTIVE_SEQUENCE_PREPARE_ONLY", body)
        self.assertIn("ACTIVE_EXECUTION=REFUSED BEFORE_MUTATION=1", body)
        self.assertLess(body.index("GuDiscoverSingleGpu("), body.index("GuExecutePreparedSequence("))


if __name__ == "__main__":
    unittest.main()
