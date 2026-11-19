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
        # Split on the definition, not on the name: the module now also has a
        # forward declaration earlier in the file.
        probe = BOOT_IO.split(
            "static BOOLEAN EspCarriesWindowsManager(EFI_BOOT_SERVICES *b,EFI_HANDLE handle) {",
            1)[1].split("\n}", 1)[0]
        self.assertNotIn("BootFileOpenResult", probe)
        self.assertNotIn("BootFileStatus", probe)
        self.assertIn("root->Close(root);", probe)

    def test_runtime_esp_log_never_claims_a_pinned_partition(self):
        runtime = BOOT_IO.split("policy=runtime_marker", 1)[0].split("#ifdef GU_RUNTIME_ESP", 1)[-1]
        self.assertIn("pinned_GPT=0", runtime)


def function_body(text, signature):
    """The brace-matched body of one function, so a neighbouring function's
    contents cannot be mistaken for this one's."""
    start = text.index(signature) + len(signature)
    depth = 1
    pos = start
    while depth and pos < len(text):
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    if depth:
        raise AssertionError("unterminated function: " + signature)
    return text[start:pos]


class Predicate(unittest.TestCase):
    def test_candidate_test_is_structural_only(self):
        signature = "int GuWindowsEspPathCandidate(const uint8_t *path, size_t bound) {"
        self.assertIn(signature, PATHS)
        body = function_body(PATHS, signature)
        self.assertNotIn("memcmp", body)
        self.assertIn("partitions==1U", body)
        header = (SRC / "gpu_windows_path.h").read_text(encoding="utf-8")
        self.assertIn("int GuWindowsEspPathCandidate(const uint8_t *path, size_t bound);", header)

    def test_the_pinned_test_still_compares_the_guid(self):
        """The runtime rule dropped the pin; the pinned rule must not lose it."""
        signature = ("int GuWindowsEspPathMatches(const uint8_t *path, size_t bound,\n"
                     "                            const uint8_t guid[16]) {")
        body = function_body(PATHS, signature)
        self.assertIn("memcmp(path+at+24,guid,16)", body)

    def test_a_boot_option_prefix_is_compared_by_value(self):
        """A2: BootOrder names a volume by its device-path prefix, so the
        comparison is over bytes - but only the prefix before a file name."""
        signature = ("int GuBootOptionNamesDevicePath(const uint8_t *load_option, size_t option_bound,\n"
                     "                                const uint8_t *path, size_t path_bound) {")
        body = function_body(PATHS, signature)
        self.assertIn("memcmp(load_option+at,path,prefix)", body)
        self.assertIn("wanted=(size_t)load_option[4]", body)


class BootOrderCrossCheck(unittest.TestCase):
    """A2: the ESP the image picks must be the ESP BootOrder boots from."""

    def test_the_walk_is_shared_and_bounded(self):
        header = (SRC / "gpu_windows_path.h").read_text(encoding="utf-8")
        self.assertIn("int GuBootOrderNamesVolume(GU_VARIABLE_READ read", header)
        self.assertIn("#define GU_ORDER_UNREADABLE (-1)", header)
        source = PATHS
        self.assertIn("#define GU_ORDER_MAX_ENTRIES 64U", source)
        self.assertIn("#define GU_ORDER_OPTION_BYTES 2048U", source)
        # The entry count is captured before the loop: the per-option read uses
        # its own size variable, so a second iteration cannot change the bound.
        self.assertIn("entries=order_size/2U;", source)

    def test_a_contradiction_refuses_and_an_unreadable_order_does_not_claim_a_pass(self):
        signature = "static EFI_STATUS ConfiguredEspVolume(EFI_HANDLE *out) {"
        body = function_body(BOOT_IO, signature)
        self.assertIn("BootOrderNamesVolume(mdp,mbytes)", body)
        self.assertIn("BOOTORDER=NONE_NAMES_SELECTED_ESP REFUSED=1", body)
        self.assertIn("BOOTORDER=UNCHECKED STATUS=%#llx PROCEEDING=1", body)
        self.assertIn("BOOTORDER=NAMES_SELECTED_ESP=1", body)
        # A readable order that names nothing must return before the volume is
        # published, and the unchecked path must be a different line.
        self.assertLess(body.index("BOOTORDER=NONE_NAMES_SELECTED_ESP"),
                        body.index("*out=matched;"))

    def test_only_the_runtime_build_consults_boot_order(self):
        # The pinned build still matches one compiled GUID and never reads a
        # variable: the cross-check lives inside the runtime branch only.
        start = BOOT_IO.index("static EFI_STATUS ConfiguredEspVolume(EFI_HANDLE *out) {")
        body = BOOT_IO[start:BOOT_IO.index("static EFI_STATUS ReadPinnedAssetFromConfiguredEsp", start)]
        guarded = body.split("#ifdef GU_RUNTIME_ESP", 1)[1]
        self.assertIn("BootOrderNamesVolume", guarded)
        self.assertIn("#else", body)
        before = body.split("#else", 1)[0]
        self.assertNotIn("BootOrderNamesVolume", before)

    def test_the_adapter_never_reads_without_a_variable_service(self):
        signature = "static int ReadGlobalVariable(const uint8_t *name,size_t name_bytes,"
        self.assertIn(signature, BOOT_IO)
        body = function_body(BOOT_IO, signature + "\n                              uint8_t *out,size_t *size) {")
        self.assertIn("if(!rs||!rs->GetVariable", body)
        self.assertIn("return s==EFI_NOT_FOUND?0:-1;", body)

    def test_the_simulator_models_a_linked_card(self):
        """C8 made an untrained link a refusal, so the modelled card must have
        one: a simulator card without a link would prove the gate fires, not
        that a real card passes it."""
        sim = (ROOT / "sim" / "sim_gpu.c").read_text(encoding="utf-8")
        self.assertIn("Gen4 x16, trained, up", sim)
        self.assertIn("max: Gen4 x16", sim)
        self.assertIn("ASPM: disabled", sim)


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
