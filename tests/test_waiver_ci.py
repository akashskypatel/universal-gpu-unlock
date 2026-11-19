"""The admitted image is compiled in CI, and the family that compiles it is never published.

D2 has two halves. The first - "build and diff the admitted image" - is covered on
the host by `tools/waiver_build_diff.py`, which shows the waiver flips two macros
and no symbols. That is a statement about source shape under GCC. The second half
needs the toolchain the image actually ships from, so the Windows job compiles the
waiver image with MSVC.

These tests pin that wiring without needing a Windows runner:

  1. the four waiver builds are invoked (application and DXE, one target and two)
     in the Windows job, with the waiver flags;
  2. the file each step asserts exists is the name the builder computes from the
     same selection - derived here from `Gpu-Selection-Build.ps1`'s label rule, so
     a rename on one side cannot silently pass CI on greps;
  3. the waiver's own interlocks are still asserted to refuse before compiling;
  4. no artifact upload path can match a waiver binary: the admitted image exists
     to be reviewed, not deployed.
"""
from pathlib import Path
import fnmatch
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / ".github" / "workflows" / "gpu-topology-diagnostic.yml").read_text(encoding="utf-8")
APP = (ROOT / "src" / "Build-Experimental-MSVC.ps1").read_text(encoding="utf-8")
DXE = (ROOT / "src" / "Build-Dxe-Driver.ps1").read_text(encoding="utf-8")
SELECTION = (ROOT / "src" / "Gpu-Selection-Build.ps1").read_text(encoding="utf-8")

WAIVER_FLAGS = "-EnableSingleTargetExecution -AcceptUnverifiedActuation -RuntimeEsp"

# PowerShell spells paths with backslashes; compare in one spelling.
FLAT = WORKFLOW.replace("\\", "/")


def windows_job() -> str:
    start = FLAT.index("\n  efi:\n")
    return FLAT[start:]


def upload_paths() -> list[str]:
    """Every path line of every artifact upload step, whitespace-stripped."""
    paths = []
    for block in FLAT.split("- uses: actions/upload-artifact@v4\n")[1:]:
        lines = block.splitlines()
        collecting = False
        for line in lines:
            stripped = line.strip()
            if stripped.startswith("path:"):
                collecting = True
                value = stripped[len("path:"):].strip()
                if value and value != "|":
                    paths.append(value)
                continue
            if collecting:
                if stripped.startswith("- ") or stripped.startswith("if-"):
                    break
                if stripped:
                    paths.append(stripped)
                elif paths:
                    break
    return paths


def label(*targets: str) -> str:
    """Mirror Gpu-Selection-Build.ps1: canonical targets, ':' and '.' -> '-'."""
    return "--".join(target.replace(":", "-").replace(".", "-") for target in targets)


def expected_name(prefix: str, extension: str, *targets: str) -> str:
    return f"{prefix}{label(*targets)}{extension}"


ONE = ("0000:09:02.0",)
TWO = ("0000:09:02.0", "0001:2A:03.1")

# builder, source text, name prefix, extension, output directory, selection
WAIVER_BUILDS = (
    ("./src/Build-Experimental-MSVC.ps1", "app", "GPU-ONE-TARGET-UNVERIFIED-ACTUATION-",
     ".EFI", "src/build", ONE),
    ("./src/Build-Dxe-Driver.ps1", "dxe", "GPU-ONE-TARGET-UNVERIFIED-ACTUATION-DXE-",
     ".efi", "src/build_dxe", ONE),
    ("./src/Build-Experimental-MSVC.ps1", "app", "GPU-SEQUENCE-UNVERIFIED-ACTUATION-",
     ".EFI", "src/build", TWO),
    ("./src/Build-Dxe-Driver.ps1", "dxe", "GPU-SEQUENCE-UNVERIFIED-ACTUATION-DXE-",
     ".efi", "src/build_dxe", TWO),
)


class TheWaiverIsCompiledWithTheRealToolchain(unittest.TestCase):
    def builds(self):
        return WAIVER_BUILDS

    def test_each_waiver_variant_is_built_in_the_windows_job(self):
        job = windows_job()
        for builder, _text, prefix, extension, _directory, targets in self.builds():
            name = expected_name(prefix, extension, *targets)
            self.assertIn(name, job, f"the Windows job never builds {name}")
            invocation = f"& {builder} -"
            self.assertIn(invocation, job)

    def test_every_waiver_build_carries_the_waiver_flags(self):
        job = windows_job()
        for builder, _text, prefix, extension, _directory, targets in self.builds():
            name = expected_name(prefix, extension, *targets)
            block_start = job.index(name)
            # The build command is the nearest preceding invocation of the builder.
            command_start = job.rindex(f"& {builder} ", 0, block_start)
            command = job[command_start:job.index("\n", command_start)]
            self.assertIn(WAIVER_FLAGS, command, f"{name} is built without the waiver")
            self.assertIn("-SelectionBdf", command.replace("-SelectionBdfs", "-SelectionBdf"))

    def test_the_builder_still_names_the_waiver_family(self):
        # A rename in either builder must break this test, not CI at run time.
        self.assertIn("'GPU-ONE-TARGET-UNVERIFIED-ACTUATION-' + $selection.Label + '.EFI'", APP)
        self.assertIn("'GPU-SEQUENCE-UNVERIFIED-ACTUATION-' + $selection.Label + '.EFI'", APP)
        self.assertIn("'GPU-ONE-TARGET-UNVERIFIED-ACTUATION-DXE-' + $selection.Label + '.efi'", DXE)
        self.assertIn("'GPU-SEQUENCE-UNVERIFIED-ACTUATION-DXE-' + $selection.Label + '.efi'", DXE)

    def test_the_label_rule_is_still_the_one_the_names_are_derived_from(self):
        self.assertIn("$_ -replace '[:.]','-'", SELECTION)
        self.assertIn("-join '--'", SELECTION)

    def test_the_admitted_build_needs_no_private_material(self):
        # src/boot_profile.h demands the gitignored local/boot_profile.h unless
        # GU_RUNTIME_ESP is defined, so a pinned waiver build cannot run on a
        # hosted runner - and a waived build may not use the synthetic host-test
        # GUID. Runtime identification is the documented third path.
        profile = (ROOT / "src" / "boot_profile.h").read_text(encoding="utf-8-sig")
        self.assertIn('#include "local/boot_profile.h"', profile)
        self.assertIn("GU_RUNTIME_ESP", profile)
        job = windows_job()
        for _builder, _kind, prefix, extension, _directory, targets in WAIVER_BUILDS:
            name = expected_name(prefix, extension, *targets)
            command_start = job.rindex("& ./src/", 0, job.index(name))
            command = job[command_start:job.index("\n", command_start)]
            self.assertIn("-RuntimeEsp", command,
                          f"{name} would need local/boot_profile.h to build")
            self.assertNotIn("-SyntheticCompileProfile", command)


class TheWaiverStillRefusesItsUnsafeCombinations(unittest.TestCase):
    def test_each_interlock_is_asserted_in_ci(self):
        job = windows_job()
        for builder in ("./src/Build-Experimental-MSVC.ps1", "./src/Build-Dxe-Driver.ps1"):
            start = job.index(f"& {builder} -SelectionBdf '0000:09:02.0' -AcceptUnverifiedActuation ")
            block = job[start:start + 400]
            self.assertIn("-match 'requires -EnableSingleTargetExecution'", block,
                          f"{builder} interlock is not asserted in CI")
        synthetic = job.index("-AcceptUnverifiedActuation -SyntheticCompileProfile")
        block = job[synthetic - 200:synthetic + 400]
        self.assertIn("-match 'cannot be combined with -SyntheticCompileProfile'", block)

    def test_the_script_actually_throws_those_messages(self):
        for message in ("requires -EnableSingleTargetExecution",
                        "cannot be combined with -SyntheticCompileProfile"):
            self.assertIn(message, APP)
            self.assertIn(message, DXE)


class TheAdmittedImageIsNeverPublished(unittest.TestCase):
    def test_no_upload_path_can_match_a_waiver_binary(self):
        paths = upload_paths()
        self.assertGreaterEqual(len(paths), 6, f"upload parsing has drifted: {paths}")
        for path in paths:
            self.assertNotIn("UNVERIFIED-ACTUATION", path)
            for _builder, _kind, prefix, extension, directory, targets in WAIVER_BUILDS:
                built = f"{directory}/{expected_name(prefix, extension, *targets)}"
                # A glob in build/ is only safe while it cannot match these names.
                self.assertFalse(fnmatch.fnmatch(built, path),
                                 f"{path} would publish the admitted image {built}")

    def test_the_waiver_is_compiled_in_the_windows_job_only(self):
        host_job = FLAT[FLAT.index("\n  host:\n"):FLAT.index("\n  efi:\n")]
        self.assertNotIn("UNVERIFIED-ACTUATION", host_job)
        self.assertNotIn("AcceptUnverifiedActuation", host_job)

    def test_the_run_does_not_decide_eligibility(self):
        job = windows_job()
        for line in re.findall(r"'WAIVER_MSVC=[^']*'", job):
            self.assertIn("PUBLISHED=0", line)
            self.assertIn("HARDWARE_ACTION=NONE", line)
        self.assertIn("WAIVER_INTERLOCKS=REFUSED BEFORE_COMPILATION=1", job)


if __name__ == "__main__":
    unittest.main()
