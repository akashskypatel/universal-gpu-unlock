"""Verify that one exact asset table drives both firmware loading and verification."""
from pathlib import Path
import re
import unittest
R=Path(__file__).resolve().parents[1]/"src"
P=(R/"profile.c").read_text(encoding="utf-8")
B=(R/"boot_io.c").read_text(encoding="utf-8")
A=(R/"app.c").read_text(encoding="utf-8")
D=(R/"dxe_main.c").read_text(encoding="utf-8")
class AssetContract(unittest.TestCase):
    def test_five_exact_assets_and_hashes_for_each_gpu(self):
        for n in ("AdaAssets","AmpereAssets"):
            part=P.split("static const GU_ARCH_ASSETS "+n+"=",1)[1].split("};",1)[0]
            self.assertEqual(len(re.findall(r'\{L"[^"]+","[^"]+","[0-9a-f]{64}",\d+U\}',part)),5)
    def test_load_and_validate_same_manifest(self):
        load=B.split("EFI_STATUS GuBootLoadAssets(VOID) {",1)[1].split("static EFI_STATUS ReadWindowsBinary(",1)[0]
        verify=P.split("BOOLEAN GuAd104AssetsValid(VOID) {",1)[1].split("BOOLEAN GuAd104PromMatches(",1)[0]
        self.assertIn("GuGetArchAssets()",load)
        self.assertIn("arch->entries[i]",load)
        self.assertIn("GuAd104AssetsValid()",load)
        self.assertIn("GuGetArchAssets()",verify)
        self.assertIn("GuHashMatches(",verify)
        self.assertNotIn("ada_sizes",load)
    def test_diagnostics_are_read_only(self):
        x=P.split("EFI_STATUS GuDiagnosticScan(VOID) {",1)[1].split("EFI_STATUS GuAd104Prepare(VOID) {",1)[0]
        for write in ("GuWrite32(","GuEnableMaster(","GuFlr(","GuMsiRelease(",
                      "GuLatchEntrySelectors(","Gu.profile_verified=TRUE"):
            self.assertNotIn(write,x)
        self.assertIn("GuAd104FwsecMatches(&fw)",x)
        self.assertIn("SIGNATURE_AUTHENTICATED=0",x)
    def test_diagnostics_run_before_preparation(self):
        for src in (A,D):
            self.assertLess(src.index("GuDiagnosticScan()"),src.index("GuAd104Prepare()"))
if __name__=="__main__":unittest.main()
