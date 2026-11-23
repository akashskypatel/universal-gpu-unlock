"""Contracts for the explicitly opted-in, unverified DXE bench variant."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


class UnsafeBenchVariantContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = (SRC / "port.h").read_text(encoding="utf-8")
        cls.checks = (SRC / "checks.c").read_text(encoding="utf-8")
        cls.release = (SRC / "msi_release.c").read_text(encoding="utf-8")
        cls.boot = (SRC / "boot_io.c").read_text(encoding="utf-8")
        cls.dxe = (SRC / "dxe_main.c").read_text(encoding="utf-8")
        cls.builder = (SRC / "Build-Dxe-Driver.ps1").read_text(encoding="utf-8")
        cls.catalog = json.loads((ROOT / "docs/image_variants.json").read_text(encoding="utf-8"))

    def test_override_requires_explicit_ordered_unverified_actuation_and_file_only(self):
        block = self.port.split("#if defined(GU_UNVERIFIED_BENCH_CONDITIONS)", 1)[1].split("#endif", 1)[0]
        self.assertIn("GU_ACTIVE_UNVERIFIED_ACTUATION", block)
        self.assertIn("GU_ACTIVE_SINGLE_TARGET", block)
        self.assertIn("GU_DXE_FILE_ONLY_OUTPUT", self.port)

    def test_only_aspm_admission_is_waived_in_the_named_experiment(self):
        accepted = self.checks.split("BOOLEAN GuLinkAccepted(", 1)[1].split("const char *GuLinkReason", 1)[0]
        self.assertIn("#ifdef GU_UNVERIFIED_BENCH_CONDITIONS", accepted)
        self.assertIn("Verdict == GU_LINK_OK || Verdict == GU_LINK_ASPM_ENABLED", accepted)
        self.assertIn("return Verdict == GU_LINK_OK;", accepted)
        self.assertIn("if ((Link.errors & 0x06U) != 0) return GU_LINK_ERROR_LOGGED;", self.checks)
        self.assertIn("if (!Link.trained) return GU_LINK_NOT_TRAINED;", self.checks)

    def test_console_ownership_waiver_is_compile_time_scoped_and_logged(self):
        for fn, marker in (("ReadOnlyAmdConsoleOwnership", "INDEPENDENT_AMD_CONSOLE"),
                           ("ReadOnlyAmdConsoleGraph", "LIVE_AMD_TEXTOUT_GRAPH")):
            body = self.release.split(f"static EFI_STATUS {fn}", 1)[1]
            self.assertIn("#ifdef GU_UNVERIFIED_BENCH_CONDITIONS", body)
            self.assertIn(f"GATE={marker} RESULT=BYPASSED", body)
        stop = self.release.split("static void StopContractLog", 1)[1].split("EFI_STATUS GuMsiRelease", 1)[0]
        self.assertIn("Console=FALSE", stop)

    def test_file_only_mode_suppresses_image_screen_messages_but_keeps_file_log(self):
        for fn in ("VOID GuBootSay", "static VOID TraceSay"):
            body = self.boot.split(fn, 1)[1].split("\n}", 1)[0]
            self.assertIn("GU_DXE_FILE_ONLY_OUTPUT", body)
        self.assertIn("Log->Write(Log,&bytes,buffer)", self.boot)
        self.assertIn("Log->Flush(Log)", self.boot)
        self.assertIn("SCREEN_OUTPUT=SUPPRESSED", self.dxe)

    def test_builder_restricts_override_to_4090_then_3080_and_marks_image(self):
        self.assertIn("[switch]$UnsafeBenchExperiment", self.builder)
        self.assertIn("-UnsafeBenchExperiment requires -AcceptUnverifiedActuation", self.builder)
        self.assertIn("0000:01:00.0,0000:04:00.0", self.builder)
        self.assertIn("/DGU_UNVERIFIED_BENCH_CONDITIONS /DGU_DXE_FILE_ONLY_OUTPUT", self.builder)
        self.assertIn("GPU-SEQUENCE-UNVERIFIED-BENCH-DXE-", self.builder)
        for field in ("ASPM_ADMISSION_BYPASSED", "AMD_CONSOLE_OWNERSHIP_BYPASSED", "FILE_ONLY_OUTPUT"):
            self.assertIn(field + "=", self.builder)

    def test_catalogue_keeps_override_as_a_separate_actuating_variant(self):
        matches = [v for v in self.catalog["variants"] if v["id"] == "dxe-actuate-sequence-bench-override"]
        self.assertEqual(len(matches), 1)
        v = matches[0]
        self.assertEqual(v["safety"], "actuates")
        self.assertEqual(v["binary"], "GPU-SEQUENCE-UNVERIFIED-BENCH-DXE-<label>.efi")
        self.assertTrue(v["switches"]["UnsafeBenchExperiment"])
        self.assertEqual(v["requires_selection"], "sequence")
        for token in ("ASPM_ADMISSION_BYPASSED=1", "AMD_CONSOLE_OWNERSHIP_BYPASSED=1",
                      "FILE_ONLY_OUTPUT=1", "RECOVERY_VERIFIED=0"):
            self.assertIn(token, v["attestations"])
        original = [v for v in self.catalog["variants"] if v["id"] == "dxe-actuate-sequence"]
        self.assertEqual(len(original), 1)
        self.assertNotIn("UnsafeBenchExperiment", original[0]["switches"])


if __name__ == "__main__":
    unittest.main()
