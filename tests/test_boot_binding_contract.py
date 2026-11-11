"""Guard exact Windows GPT partition and exported boot/log state."""
from pathlib import Path
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
    def test_log_path_requires_open_log(self):
        self.assertIn("return Log?ActiveLogPath:NULL;",BOOT)
        self.assertIn("ActiveLogPath[14+k]=name[k]",BOOT)

if __name__=="__main__":unittest.main()
