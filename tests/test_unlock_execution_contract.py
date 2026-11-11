"""One shared transaction contract for app and DXE source.

This rejects divergent sequences, missing pre-Windows checks, and early
firmware use. Hardware behavior is verified separately.
"""
from pathlib import Path
import re
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
    def test_s3_is_optional_and_after_handover(self):
        for src,name in [(APP,"UefiMain"),(DXE,"GuDxeRun")]:
            code=body(src,name)
            self.assertLess(code.index("GuExecuteUnlock("),code.index("GuRegisterS3("))
        s3=body(SHARED,"GuRegisterS3")
        self.assertIn("GuSafeToReturn()",s3)
        self.assertIn("EFI_BOOT_SCRIPT_MEM_WRITE_OPCODE",s3)

if __name__=="__main__":unittest.main()
