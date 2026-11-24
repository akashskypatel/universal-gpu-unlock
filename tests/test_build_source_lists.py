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
    shared = (ROOT / "src" / "Gpu-Execution-Sources.ps1").read_text(encoding="utf-8-sig")
    assert ". (Join-Path $PSScriptRoot 'Gpu-Execution-Sources.ps1')" in text, name
    def units(variable):
        block = re.search(r"\$" + variable + r"\s*=\s*@\((.*?)\n\s*\)", shared, re.S)
        assert block, f"missing shared {variable}"
        return [entry.replace("\\", "/") for entry in QUOTED.findall(block.group(1))]
    core = units("executionCoreSources")
    support = units("executionSupportSources")
    if name == BUILDERS[0]:
        assert "$sources = @('app.c', 'driver_nvram.c') + $executionCoreSources + $executionSupportSources" in text
        return ["app.c", "driver_nvram.c"] + core + support
    assert name == BUILDERS[1], name
    assert "$sources = @('dxe_main.c') + $executionCoreSources + @('driver_nvram.c') + $executionSupportSources" in text
    return ["dxe_main.c"] + core + ["driver_nvram.c"] + support


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

    def test_powershell_source_inventory_matches_independent_simulator(self):
        import json
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            self.skipTest("PowerShell unavailable")
        for name in BUILDERS:
            text = (ROOT / "src" / name).read_text(encoding="utf-8-sig")
            assignment = next(line.strip() for line in text.splitlines()
                              if line.strip().startswith("$sources = @("))
            helper = str(ROOT / "src" / "Gpu-Execution-Sources.ps1").replace("'", "''")
            cmd = f". '{helper}'; {assignment}; ConvertTo-Json -InputObject $sources -Compress"
            result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", cmd],
                                    capture_output=True, text=True, check=True, timeout=30)
            actual = [unit.replace("\\", "/") for unit in json.loads(result.stdout)]
            expected = set(app_sources())
            extra, absent = ALLOWED_DELTA[name]
            self.assertEqual(set(actual), (expected - absent) | extra)
            self.assertEqual(len(actual), len(set(actual)))
            self.assertEqual(actual, builder_sources(name))

    def test_shared_compile_plan_preserves_commands_and_object_order(self):
        import json
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            self.skipTest("PowerShell unavailable")
        helper = str(ROOT / "src" / "Gpu-Execution-Sources.ps1").replace("'", "''")
        for name, directory in zip(BUILDERS, ("build", "build_dxe")):
            text = (ROOT / "src" / name).read_text(encoding="utf-8-sig")
            self.assertIn(f"New-GuSourceCompilePlan -Sources $sources -CompilerFlags $clFlags -ObjectDirectory '{directory}'", text)
            self.assertIn("$compileCmds = $compilePlan.Commands", text)
            self.assertIn("$objFiles = $compilePlan.Objects", text)
            assignment = next(line.strip() for line in text.splitlines()
                              if line.strip().startswith("$sources = @("))
            for stop_only in (False, True):
                with self.subTest(builder=name, stop_only=stop_only):
                    exclusion = r"; $sources = @($sources | Where-Object { $_ -ne 'vendor\unlock_610.c' })" if stop_only else ""
                    cmd = (f". '{helper}'; {assignment}{exclusion}; "
                           f"New-GuSourceCompilePlan -Sources $sources -CompilerFlags '/nologo /Oi /TC' "
                           rf"-ObjectDirectory '{directory}' -VcVars 'C:\VS Tools\vcvars64.bat' "
                           r"-SourceDirectory 'C:\GPU Source' | ConvertTo-Json -Compress")
                    result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", cmd],
                                            capture_output=True, text=True, check=True, timeout=30)
                    plan = json.loads(result.stdout)
                    commands = ['@echo off', r'call "C:\VS Tools\vcvars64.bat" >nul',
                                'if errorlevel 1 exit /b 1', r'cd /d "C:\GPU Source"']
                    objects = []
                    for source in builder_sources(name):
                        if stop_only and source == "vendor/unlock_610.c":
                            continue
                        base = Path(source).stem
                        if source.startswith("vendor/"):
                            base = "vendor_" + base
                        obj = f"{directory}\\{base}.obj"
                        objects.append(obj)
                        flags = "/nologo /Oi- /TC" if source == "runtime.c" else "/nologo /Oi /TC"
                        commands += [f"cl {flags} /Fo{obj} " + source.replace("/", "\\"),
                                     "if errorlevel 1 exit /b 1"]
                    self.assertEqual(plan["Objects"], objects)
                    self.assertEqual(plan["Commands"], commands)

    def test_all_builders_use_shared_toolchain_discovery(self):
        for name in ("Build-Experimental-MSVC.ps1", "Build-Dxe-Driver.ps1", "Build-Gpu-Topology-Diagnostic.ps1", "Build-Msi-Stop-Observation.ps1", "Build-Boot-State-Maintenance.ps1"):
            text = (ROOT / "src" / name).read_text(encoding="utf-8-sig")
            self.assertIn(". (Join-Path $PSScriptRoot 'Msvc-Environment.ps1')", text)
            suffix = " -SearchDefaultPaths" if name in BUILDERS else ""
            self.assertIn("$VcVars = Resolve-GuVcVars -VcVars $VcVars" + suffix + "\n", text)
            self.assertNotIn("vswhere.exe", text)

    def test_shared_toolchain_explicit_path_and_missing_path(self):
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            self.skipTest("PowerShell unavailable")
        helper = str(ROOT / "src" / "Msvc-Environment.ps1").replace("'", "''")
        with tempfile.TemporaryDirectory() as directory:
            explicit = Path(directory) / "vcvars64.bat"
            explicit.touch()
            prefix = f"$ErrorActionPreference='Stop'; . '{helper}'; Resolve-GuVcVars -VcVars "
            for path, success in ((explicit, True), (explicit.with_suffix(".missing"), False)):
                with self.subTest(path=path):
                    quoted = str(path).replace("'", "''")
                    result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command",
                                             prefix + f"'{quoted}' -SearchDefaultPaths"],
                                            capture_output=True, text=True, timeout=30)
                    if success:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.strip(), str(path))
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("Visual Studio x64 Build Tools not found", result.stderr)

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
    def _check_units(self, units: list[str]) -> None:
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
                    if re.fullmatch(r"Gu[A-Z]\w*", name):
                        imported.add(name)
                undefined[unit] = imported
            missing = {unit: sorted(names - defined) for unit, names in undefined.items()
                       if names - defined}
            self.assertEqual(missing, {},
                             "these builders would not link: " + repr(missing))

    def test_every_listed_unit_finds_the_symbols_it_needs(self):
        self._check_units(app_sources())

    def test_dxe_recovery_dependencies_would_link(self):
        self._check_units(builder_sources("Build-Dxe-Driver.ps1"))


if __name__ == "__main__":
    unittest.main()
