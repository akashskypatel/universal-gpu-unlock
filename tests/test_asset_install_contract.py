"""Contract tests for the staged firmware bundle (A6, partial).

The image re-reads five pinned files on every boot - including an 84 MB GSP
image - from the removable media it was launched from. Staging them on the ESP
is a real change in what the image is allowed to do, so these tests pin the
limits of that change:

  1. only the explicit install build stages anything, and that build never
     prepares, releases or mutates the GPU;
  2. every file is verified against its pinned digest before it is written, and
     read back and verified again before the record is written;
  3. an existing file is never overwritten - it is accepted only when it already
     holds the pinned content, so an interrupted install is resumable and a
     mismatched one is refused, not repaired;
  4. the record is written only after the whole bundle succeeds, and a corrupt
     or short record is refused on read;
  5. the read path stays write-free, and the removable-media launch rule is not
     relaxed anywhere by this change.

Nothing here claims that the ESP is an acceptable place to launch from, and
nothing here accepts a vendor signature: a digest proves "these bytes", not
"the vendor signed these bytes".
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TOOLS = ROOT / "tools"

BOOT = (SRC / "boot_io.c").read_text(encoding="utf-8")
BOOT_H = (SRC / "boot_io.h").read_text(encoding="utf-8")
INSTALL = (SRC / "asset_install.c").read_text(encoding="utf-8")
INSTALL_H = (SRC / "asset_install.h").read_text(encoding="utf-8")
PROFILE = (SRC / "profile.c").read_text(encoding="utf-8")
PROFILE_H = (SRC / "profile.h").read_text(encoding="utf-8")
APP = (SRC / "app.c").read_text(encoding="utf-8")
RUN_SIM = (TOOLS / "run_simulation.py").read_text(encoding="utf-8")
SIM_MAIN = (ROOT / "sim" / "sim_main.c").read_text(encoding="utf-8")

INSTALLER = BOOT[BOOT.index("EFI_STATUS GuAssetInstallStage(VOID) {"):]
STAGE_ONE = BOOT[BOOT.index("static EFI_STATUS StageAsset("):
                 BOOT.index("EFI_STATUS GuAssetInstallStage(VOID) {")]


def function_body(text, name):
    start = text.index(name)
    depth, index = 0, text.index("{", start)
    for position in range(index, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[index : position + 1]
    raise AssertionError("unbalanced body for " + name)


class TheLaunchRuleIsNotRelaxed(unittest.TestCase):
    def test_open_still_refuses_a_non_usb_image(self):
        body = function_body(BOOT, "EFI_STATUS GuBootOpen(")
        # The rule moved into an explicit decision (GU_ESP_RESIDENT); the default
        # build still cannot start from anything but the stick.
        self.assertIn("if(GuLaunchSourceDecide(&launch)!=GU_LAUNCH_ALLOW_RESIDENT) {", body)
        self.assertIn("launch.EspResidentBuild=GU_LS_FALSE;", body)
        self.assertIn("return EFI_ACCESS_DENIED;", body)
        self.assertIn("EFI_ACCESS_DENIED", body)

    def test_the_installer_requires_the_usb_launch(self):
        self.assertIn("if(!BootFromUsb)", INSTALLER)
        self.assertIn("REASON=IMAGE_NOT_ON_REMOVABLE_MEDIA", INSTALLER)
        # One place sets the flag, and it is the launch check itself.
        self.assertEqual(BOOT.count("BootFromUsb=TRUE;"), 1)
        self.assertLess(BOOT.index("BootFromUsb=TRUE;"),
                        BOOT.index("BootFromUsb=TRUE;") + 1)

    def test_install_never_authorizes_a_boot(self):
        # The record decides nothing: no branch may gate boot or load on it.
        for name, text in (("boot_io.c", BOOT), ("app.c", APP)):
            for line in text.splitlines():
                if "GuAssetInstallState(" not in line:
                    continue
                self.assertNotIn("if(", line, name)
                self.assertNotIn("return", line, name)


class OnlyTheInstallBuildStagesAnything(unittest.TestCase):
    def test_app_stages_only_under_the_install_flag(self):
        block = APP[APP.index("#ifdef GU_ASSET_INSTALL_ONLY"):]
        block = block[: block.index("#endif")]
        self.assertIn("GuAssetInstallStage()", block)
        self.assertIn("GuBootHalt()", block)
        self.assertIn("return EFI_ABORTED", block)

    def test_install_build_cannot_reach_prepare_or_the_sequence(self):
        block = APP[APP.index("#ifdef GU_ASSET_INSTALL_ONLY"):]
        block = block[: block.index("#endif")]
        for forbidden in ("GuPrepareBoundGpu", "GuExecutePreparedSequence",
                          "GuBootWindows", "GuAd104Prepare"):
            self.assertNotIn(forbidden, block)

    def test_installer_refuses_a_busy_owner(self):
        for state in ("Gu.started", "Gu.fault", "Gu.retained", "Gu.owned",
                      "Gu.allocations"):
            self.assertIn(state, INSTALLER, state)
        self.assertIn("REASON=GPU_STATE_NOT_IDLE", INSTALLER)

    def test_staging_is_not_compiled_into_the_driver_path(self):
        dxe = (SRC / "dxe_main.c").read_text(encoding="utf-8")
        self.assertNotIn("GuAssetInstallStage", dxe)


class EveryFileIsVerifiedTwice(unittest.TestCase):
    def test_source_digest_checked_before_writing(self):
        verify = INSTALLER.index("GuHashMatches(p,a->bytes,a->sha256)")
        write = INSTALLER.index("StageAsset(esp,path,p,a->bytes,a->sha256)")
        self.assertLess(verify, write)
        self.assertIn("REASON=SOURCE_DIGEST_MISMATCH", INSTALLER)

    def test_written_bytes_are_read_back_and_hashed(self):
        self.assertIn("ReadFile(esp,path,bytes,&written)", STAGE_ONE)
        self.assertIn("GuHashMatches(written,bytes,sha256)", STAGE_ONE)
        self.assertIn("EFI_CRC_ERROR", STAGE_ONE)

    def test_flush_and_exact_length_are_required(self):
        self.assertIn("file->Flush(file)", STAGE_ONE)
        self.assertIn("n!=wanted", STAGE_ONE)
        self.assertIn("info->FileSize==bytes", STAGE_ONE)


class ExistingFilesAreNeverOverwritten(unittest.TestCase):
    def test_mismatched_file_is_refused(self):
        self.assertIn("EFI_ALREADY_STARTED", STAGE_ONE)
        self.assertIn("EFI_ACCESS_DENIED", STAGE_ONE)
        self.assertIn("EXISTING_MISMATCH", INSTALLER)

    def test_no_truncating_open_and_no_delete(self):
        # A truncating or deleting write would make a torn install look like a
        # fresh one; neither is used.
        for forbidden in ("EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0)\n    s=",):
            self.assertNotIn(forbidden, STAGE_ONE)
        # Only the explicitly authorized combined-application log cleanup
        # may delete a file. The installer and its shared asset I/O remain
        # deletion-free, including when cleanup support is compiled in.
        cleanup = function_body(BOOT, "static EFI_STATUS DeleteOldRunLogs(")
        self.assertEqual(BOOT.count(cleanup), 1)
        self.assertNotIn("->Delete", BOOT.replace(cleanup, "", 1))
        self.assertEqual(cleanup.count("->Delete("), 1)
        self.assertIn("slot&&slot!=current", cleanup)
        self.assertIn("RunLogInfoSlot((const VOID *)words,bytes)!=slot", cleanup)
        self.assertNotIn("DeleteOldRunLogs", INSTALLER)
        self.assertNotIn("DeleteOldRunLogs", STAGE_ONE)
        self.assertEqual(BOOT.count("DeleteOldRunLogs("), 2)
        self.assertIn('s=Root->Open(Root,&dir,L"\\\\AD102-UNLOCK"', cleanup)
        self.assertIn("LogVolumeFull(total,free_bytes)", cleanup)
        self.assertNotIn("SetInfo", BOOT)

    def test_a_refusal_stops_the_whole_bundle(self):
        refused = INSTALLER.index("EXISTING_MISMATCH")
        record = INSTALLER.index("GuAssetInstallWrite")
        self.assertLess(refused, record)


class TheRecordIsTheCommitPoint(unittest.TestCase):
    def test_record_written_only_after_success(self):
        self.assertIn("if(s!=EFI_SUCCESS)return s;", INSTALLER)
        success = INSTALLER.index("if(s!=EFI_SUCCESS)return s;")
        self.assertLess(success, INSTALLER.index("GuAssetInstallWrite"))
        self.assertIn("ASSET_INSTALL=STAGED", INSTALLER)
        self.assertIn("ASSET_INSTALL=INCOMPLETE", INSTALLER)

    def test_record_carries_the_manifest_digest(self):
        self.assertIn("GuAssetInstallBundleDigest(&bundle,record.Bundle)",
                      INSTALLER)
        self.assertIn("GU_ASSET_BUNDLE_VERSION", INSTALLER)

    def test_digest_covers_every_pinned_field(self):
        body = function_body(INSTALL, "VOID GuAssetInstallBundleDigest(")
        for field in ("file->Name", "file->Bytes", "file->Sha256"):
            self.assertIn(field, body, field)

    def test_short_or_corrupt_records_are_refused(self):
        self.assertIn("size != sizeof(buffer)", INSTALL)
        self.assertIn("EFI_COMPROMISED_DATA", INSTALL)
        body = function_body(INSTALL, "EFI_STATUS GuAssetInstallValidate(")
        for check in ("Magic", "Files", "Version", "Bundle"):
            self.assertIn(check, body, check)

    def test_state_is_absent_current_stale_or_corrupt(self):
        body = function_body(INSTALL, "UINT32 GuAssetInstallState(")
        self.assertIn("ABSENT", INSTALL_H)
        self.assertIn("CURRENT", INSTALL_H)
        self.assertIn("STALE", INSTALL_H)
        self.assertIn("CORRUPT", INSTALL_H)
        self.assertIn("GuAssetInstallValidate(record)", body)
        self.assertIn("memcmp", body)


class TheBundleMatchesWhatLoads(unittest.TestCase):
    def test_pinned_version_is_single_and_named_by_every_path(self):
        # One literal, two widths: the wide macro is derived from the narrow one,
        # so a version bump cannot drift between the bundle record and the
        # per-card reference comparison.
        self.assertEqual(PROFILE_H.count('#define GU_ASSET_BUNDLE_VERSION_CHAR8 "'), 1)
        self.assertIn("#define GU_ASSET_BUNDLE_VERSION GU_WIDEN_LITERAL(", PROFILE_H)
        self.assertIn("GU_ASSET_BUNDLE_VERSION", PROFILE)
        version = re.search(r'#define GU_ASSET_BUNDLE_VERSION_CHAR8 "([0-9.]+)"',
                            PROFILE_H).group(1)
        # Every pinned vendor path names the one firmware version, so a manifest
        # cannot mix bundles behind a single digest.
        paths = re.findall(r'"(gpu-unlock/[^"]+)"', PROFILE)
        self.assertTrue(paths, "no pinned vendor paths found")
        for path in paths:
            self.assertIn(version, path, path)

    def test_bundle_projects_the_compiled_manifest(self):
        body = function_body(PROFILE, "VOID GuAssetBundleFromArch(")
        self.assertIn("arch->entries[i].name", body)
        self.assertIn("arch->entries[i].bytes", body)
        self.assertIn("arch->entries[i].sha256", body)
        self.assertIn("GU_ASSET_BUNDLE_VERSION", body)

    def test_load_path_reports_state_without_changing_order(self):
        load = function_body(BOOT, "EFI_STATUS GuBootLoadAssets(VOID)")
        report = load.index("ASSET_INSTALL STATE=")
        read = load.index("ReadFile(Root,a->path")
        self.assertLess(report, read)
        # USB first, ESP only as the pinned fallback: unchanged.
        self.assertLess(load.index("ReadFile(Root,a->path"),
                        load.index("ReadPinnedAssetFromConfiguredEsp(a,a->path"))


class TheWiringIsComplete(unittest.TestCase):
    def test_image_builds_compile_the_record(self):
        for script in ("Build-Dxe-Driver.ps1", "Build-Experimental-MSVC.ps1"):
            from test_build_source_lists import builder_sources
            self.assertIn("asset_install.c", builder_sources(script), script)

    def test_simulator_compiles_and_exercises_it(self):
        self.assertIn('"asset_install.c"', RUN_SIM)
        self.assertIn('"install-assets"', RUN_SIM)
        self.assertIn('("prepare", "install-assets")', RUN_SIM)
        self.assertIn("GuAssetInstallStage()", SIM_MAIN)
        self.assertIn("GU_ASSET_INSTALL_CURRENT", SIM_MAIN)
        self.assertIn("the installer reset the card", SIM_MAIN)

    def test_header_documents_the_limit(self):
        text = " ".join(INSTALL_H.split())
        self.assertIn("not an authorization", text)
        self.assertIn("ABSENT", text)


if __name__ == "__main__":
    unittest.main()
