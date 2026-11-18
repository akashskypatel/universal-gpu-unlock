"""Contract tests for runtime Windows-ESP identification (no compiled GUID).

The pinned build identified the Windows ESP by a GPT GUID compiled in from a
private ``local/boot_profile.h``, so one image belonged to exactly one Windows
installation. ``-RuntimeEsp`` replaces the pin with a runtime rule:

  * the volume must be a located, non-USB, non-own filesystem whose device path
    carries exactly one GPT partition with the EFI-system type, and
  * it must actually contain ``\\EFI\\Microsoft\\Boot\\bootmgfw.efi``, and
  * exactly one such volume may exist, otherwise the boot is refused.

The pinned path is unchanged and still mandatory by default: this file proves
the relaxation exists only behind the named switch, and that the strict
single-ESP refusal survives it. Nothing here opens a real ESP.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
BOOT_PROFILE = (SRC / "boot_profile.h").read_text(encoding="utf-8")
BOOT_IO = (SRC / "boot_io.c").read_text(encoding="utf-8")
PATHS = (SRC / "gpu_windows_path.c").read_text(encoding="utf-8")
MSVC = (SRC / "Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
DXE = (SRC / "Build-Dxe-Driver.ps1").read_text(encoding="utf-8")
BUILDERS = (("Build-Experimental-MSVC.ps1", MSVC), ("Build-Dxe-Driver.ps1", DXE))


def compiler():
    return shutil.which("gcc") or shutil.which("clang")


def compile_profile(flags):
    cc = compiler()
    if not cc:
        raise unittest.SkipTest("A host C compiler is required")
    with tempfile.TemporaryDirectory() as tmp:
        probe = Path(tmp) / "probe.c"
        probe.write_text(
            '#include "boot_profile.h"\n'
            "#ifdef GU_WINDOWS_PARTITION_INITIALIZER\n"
            "#define GU_PROBE_HAS_PIN 1\n"
            "#else\n"
            "#define GU_PROBE_HAS_PIN 0\n"
            "#endif\n"
            "int gu_esp_probe = GU_PROBE_HAS_PIN;\n", encoding="utf-8")
        done = subprocess.run(
            [cc, "-std=c11", "-fsyntax-only", "-I", str(SRC), *flags, str(probe)],
            capture_output=True, text=True)
        return done.returncode, done.stdout + done.stderr


class BootProfileRules(unittest.TestCase):
    def test_pin_is_still_mandatory_by_default(self):
        status, output = compile_profile(())
        self.assertNotEqual(status, 0, "the default build compiled without a pin")
        # Either the private header is missing (the normal case for a clean
        # checkout) or it exists without the required initializer.
        self.assertTrue(
            "local/boot_profile.h" in output or
            "Configure the reviewed ESP GUID in local/boot_profile.h" in output, output)

    def test_runtime_esp_carries_no_pin(self):
        status, output = compile_profile(("-DGU_RUNTIME_ESP",))
        self.assertEqual(status, 0, output)

    def test_runtime_esp_rejects_the_synthetic_host_profile(self):
        status, output = compile_profile(("-DGU_RUNTIME_ESP", "-DGU_HOST_TEST_PROFILE"))
        self.assertNotEqual(status, 0)
        self.assertIn("Runtime ESP identification and the synthetic host profile are distinct builds",
                      output)

    def test_host_test_profile_still_provides_a_synthetic_pin(self):
        status, output = compile_profile(("-DGU_HOST_TEST_PROFILE",))
        self.assertEqual(status, 0, output)


class BootIoWiring(unittest.TestCase):
    def test_pinned_definition_is_gone_under_the_runtime_switch(self):
        squashed = BOOT_IO.replace(" ", "")
        self.assertIn("#ifndefGU_RUNTIME_ESP\nstaticconstEFI_GUIDWindowsPartition"
                      "=GU_WINDOWS_PARTITION_INITIALIZER;\n#endif", squashed)

    def test_both_esp_sites_select_on_the_runtime_rule(self):
        # The asset reader and the Windows loader each choose the runtime rule.
        self.assertEqual(BOOT_IO.count("GuWindowsEspPathCandidate((const uint8_t *)"), 2)
        self.assertEqual(BOOT_IO.count("GuWindowsEspPathMatches("), 2)
        self.assertIn("EspCarriesWindowsManager(b,handles[i])", BOOT_IO)

    def test_single_esp_refusal_survives(self):
        self.assertIn("if(matches!=1U)return matches?EFI_UNSUPPORTED:EFI_NOT_FOUND;", BOOT_IO)
        self.assertIn("if(matches!=1) {", BOOT_IO)
        self.assertIn("GuBootLoadWindows refused: %zu volumes carry a Windows boot manager",
                      BOOT_IO)

    def test_marker_probe_does_not_latch_file_status(self):
        probe = BOOT_IO.split("static BOOLEAN EspCarriesWindowsManager", 1)[1].split("\n}", 1)[0]
        self.assertNotIn("BootFileOpenResult", probe)
        self.assertNotIn("BootFileStatus", probe)
        self.assertIn("root->Close(root);", probe)

    def test_runtime_esp_log_never_claims_a_pinned_partition(self):
        runtime = BOOT_IO.split("policy=runtime_marker", 1)[0].split("#ifdef GU_RUNTIME_ESP", 1)[-1]
        self.assertIn("pinned_GPT=0", runtime)


class Predicate(unittest.TestCase):
    def test_candidate_test_is_structural_only(self):
        self.assertIn("int GuWindowsEspPathCandidate(const uint8_t *path, size_t bound) {", PATHS)
        body = PATHS.split("int GuWindowsEspPathCandidate", 1)[1]
        self.assertNotIn("memcmp", body)
        self.assertIn("partitions==1U", body)
        header = (SRC / "gpu_windows_path.h").read_text(encoding="utf-8")
        self.assertIn("int GuWindowsEspPathCandidate(const uint8_t *path, size_t bound);", header)


class BuilderSwitches(unittest.TestCase):
    def test_both_builders_expose_the_switch_once(self):
        for name, text in BUILDERS:
            self.assertEqual(text.count("/DGU_RUNTIME_ESP"), 1, name)
            self.assertIn("[switch]$RuntimeEsp", text, name)
            self.assertIn("RUNTIME_ESP=$([int][bool]$RuntimeEsp)", text, name)

    def test_builders_refuse_the_synthetic_combination(self):
        for name, text in BUILDERS:
            self.assertIn(
                "-RuntimeEsp cannot be combined with -SyntheticCompileProfile", text, name)


if __name__ == "__main__":
    unittest.main()
