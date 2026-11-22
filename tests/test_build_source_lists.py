"""The MSVC builder source lists are part of the program, and must link.

Phase 2 moved the per-card reference table into `src/prom_reference.c` and made
`profile.c` call it. The simulator's build list, the host build-diff tool and the
unit-test compiles were all updated; the two PowerShell builders that produce the
image were not, so every MSVC variant failed to link with an unresolved
`GuPromReferenceCount`. Nothing on the host noticed, because nothing on the host
looked at the PowerShell list.

Two checks close that hole:

  1. set equality - each builder's `$sources` array must be exactly the program the
     simulator compiles (`tools/run_simulation.py: APP_SOURCES`), so a file added
     to one list and not the other fails here rather than on a Windows runner;
  2. symbol coverage - for every listed unit, an undefined `Gu*` symbol must be
     defined by some other listed unit. This is a link check without the linker: it
     catches a *missing* file even when both lists are wrong in the same way.

The symbol check compiles with the host compiler that the build-diff tool already
uses and is skipped when none is available.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILDERS = ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1")

sys.path.insert(0, str(ROOT / "tools"))
import run_simulation  # noqa: E402
import waiver_build_diff  # noqa: E402

SOURCES_BLOCK = re.compile(r"\$sources\s*=\s*@\((.*?)\n\s*\)", re.S)
QUOTED = re.compile(r"'([^']+\.c)'")


def builder_sources(name: str) -> list[str]:
    text = (ROOT / "src" / name).read_text(encoding="utf-8-sig")
    block = SOURCES_BLOCK.search(text)
    assert block, f"{name}: no $sources array"
    return [entry.replace("\\", "/") for entry in QUOTED.findall(block.group(1))]


def app_sources() -> list[str]:
    return [entry.replace("\\", "/") for entry in run_simulation.APP_SOURCES]


# Reviewed differences: the DXE image has its own entrypoint, while the app has
# the app-only entrypoint and interactive install/remove flow. Both now compile
# driver_nvram.c: the DXE uses it for fail-closed self-disable recovery.
ALLOWED_DELTA = {
    "Build-Experimental-MSVC.ps1": (set(), set()),
    "Build-Dxe-Driver.ps1": ({"dxe_main.c"}, {"app.c"}),
}


class TheBuildersCompileTheSameProgramAsTheSimulator(unittest.TestCase):
    def test_each_builder_matches_the_simulator_source_list(self):
        expected = set(app_sources())
        for builder in BUILDERS:
            listed = set(builder_sources(builder))
            extra, absent = ALLOWED_DELTA[builder]
            self.assertEqual(listed - expected, extra,
                             f"{builder} compiles units the program does not: "
                             f"{sorted(listed - expected - extra)}")
            self.assertEqual(expected - listed, absent,
                             f"{builder} omits units the program needs: "
                             f"{sorted(expected - listed - absent)}")

    def test_the_reference_table_is_compiled_into_every_image(self):
        # The specific regression: profile.c calls into prom_reference.c.
        profile = (ROOT / "src" / "profile.c").read_text(encoding="utf-8-sig")
        self.assertIn("GuPromReferenceDecide", profile)
        for builder in BUILDERS:
            self.assertIn("prom_reference.c", builder_sources(builder),
                          f"{builder} would fail to link profile.c")

    def test_no_builder_list_silently_skips_a_unit(self):
        for builder in BUILDERS:
            listed = builder_sources(builder)
            self.assertEqual(len(listed), len(set(listed)), f"{builder} lists a file twice")
            for entry in listed:
                self.assertTrue((ROOT / "src" / entry).is_file(),
                                f"{builder} lists a file that does not exist: {entry}")


@unittest.skipUnless(shutil.which(waiver_build_diff.compiler()) and shutil.which("nm"),
                     "no host compiler for the link check")
class TheImagesWouldLink(unittest.TestCase):
    def test_every_listed_unit_finds_the_symbols_it_needs(self):
        units = app_sources()
        with tempfile.TemporaryDirectory() as temporary:
            workdir = Path(temporary)
            defined: set[str] = set()
            undefined: dict[str, set[str]] = {}
            for unit in units:
                objects = waiver_build_diff.symbols_for(unit, [], workdir)
                defined |= objects
                imported = set()
                obj = workdir / (Path(unit).stem + "_checked.o")
                listing = subprocess.run(["nm", "--undefined-only", str(obj)],
                                         capture_output=True, text=True, check=True)
                for line in listing.stdout.splitlines():
                    parts = line.split()
                    if not parts:
                        continue
                    name = parts[-1]
                    # Only this project's own functions: EFI services, libc and
                    # compiler builtins belong to the environment, not the image.
                    if re.fullmatch(r"Gu[A-Z]\w*", name):
                        imported.add(name)
                undefined[unit] = imported
            missing = {unit: sorted(names - defined) for unit, names in undefined.items()
                       if names - defined}
            self.assertEqual(missing, {},
                             "these builders would not link: " + repr(missing))


if __name__ == "__main__":
    unittest.main()
