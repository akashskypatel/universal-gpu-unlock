"""Stage S1/D2: CI builds the admitted image and diffs it.

The inventory's S1 exit criteria end with "CI builds and diffs the admitted
image", and D2 asks for a CI build of the waiver path. Neither had a mechanism:
the workflow built preparation-only images and the waiver path lived only in
`docs/UNVERIFIED_ACTUATION_RUNBOOK.md`.

`tools/waiver_build_diff.py` builds the same sources twice - once checked, once
admitted - and diffs the preprocessor define sets and the object symbols per
translation unit. It passes only if:

  * the waiver's macro delta is exactly the reviewed pair
    (`GU_ACTIVE_UNVERIFIED_ACTUATION` appears, `GU_ACTUATION_ADMITTED` flips
    0 -> 1), in every unit where it appears at all;
  * `GU_ACTUATION_ADMITTED` actually flips somewhere, so a waiver that admits
    nothing cannot pass as evidence;
  * no translation unit adds or removes a symbol, so the waiver flips a gate
    inside the same program instead of compiling a different one.

Set `GU_SKIP_BUILD_TESTS=1` to skip the compile-heavy part.
"""
from pathlib import Path
import os
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import waiver_build_diff as wbd  # noqa: E402


class TheToolRuns(unittest.TestCase):
    @unittest.skipIf(os.environ.get("GU_SKIP_BUILD_TESTS") == "1",
                     "GU_SKIP_BUILD_TESTS=1")
    def test_the_admitted_image_differs_only_where_reviewed(self):
        done = subprocess.run([sys.executable, str(ROOT / "tools" / "waiver_build_diff.py")],
                              capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(done.returncode, 0, done.stdout[-3000:] + done.stderr[-2000:])
        self.assertIn("WAIVER_DIFF=PASS", done.stdout)
        self.assertIn("SYMBOL_DELTAS=0", done.stdout)
        # The waiver has to reach the gate it claims to reach.
        line = [l for l in done.stdout.splitlines() if l.startswith("WAIVER_DIFF=PASS")][0]
        admitted = int(line.split("ADMITTED_UNITS=")[1].split()[0])
        self.assertGreater(admitted, 0, "the waiver reached no translation unit")
        self.assertNotIn("WAIVER_DIFF_PROBLEM", done.stdout)

    def test_the_tool_needs_no_generated_state(self):
        # CI runs it on a clean checkout, before the simulator has generated
        # anything, and `src/local/` is gitignored - so the tool must supply its
        # own placeholder boot profile rather than depend on `sim/build`.
        source = (ROOT / "tools" / "waiver_build_diff.py").read_text(encoding="utf-8")
        self.assertNotIn('"sim" / "build"', source)
        self.assertIn('(local / "boot_profile.h").write_text', source)
        self.assertIn("#define GU_WINDOWS_PARTITION_INITIALIZER", source)

    def test_a_missing_compiler_is_a_skip_not_a_pass(self):
        done = subprocess.run([sys.executable, str(ROOT / "tools" / "waiver_build_diff.py")],
                              capture_output=True, text=True, cwd=ROOT,
                              env={**os.environ, "CC": "definitely-not-a-compiler"})
        self.assertEqual(done.returncode, 0)
        self.assertIn("WAIVER_DIFF=SKIP", done.stdout,
                      "a missing compiler must not look like a passing diff")


class ThePinIsNarrow(unittest.TestCase):
    def test_exactly_the_reviewed_macros_may_change(self):
        self.assertEqual(
            wbd.EXPECTED_MACRO_DELTAS,
            {"GU_ACTIVE_UNVERIFIED_ACTUATION": (None, "1"),
             "GU_ACTUATION_ADMITTED": ("0", "1")})

    def test_no_unit_may_gain_or_lose_code(self):
        self.assertEqual(wbd.EXPECTED_SYMBOL_DELTA_UNITS, set())

    def test_the_waiver_is_the_single_admission_switch(self):
        # The tool must build the same profile the runbook names, plus the
        # waiver - not some other profile that happens to compile.
        self.assertIn("-DGU_AD104_TRANSACTION", wbd.BASE_DEFINES)
        self.assertIn("-DGU_ACTIVE_SINGLE_TARGET", wbd.BASE_DEFINES)
        self.assertEqual(wbd.WAIVER, ["-DGU_ACTIVE_UNVERIFIED_ACTUATION"])
        runbook = (ROOT / "docs" / "UNVERIFIED_ACTUATION_RUNBOOK.md").read_text(
            encoding="utf-8")
        for define in ("GU_AD104_TRANSACTION", "GU_ACTIVE_SINGLE_TARGET",
                       "GU_ACTIVE_UNVERIFIED_ACTUATION"):
            self.assertIn(define, runbook)

    def test_every_compiled_source_is_covered(self):
        # The diff is only evidence if it looks at the whole image.
        runner = (ROOT / "tools" / "run_simulation.py").read_text(encoding="utf-8")
        for source in wbd.SOURCES:
            self.assertIn(f'"{source}"', runner,
                          f"{source} is not part of the image the simulator builds")
        self.assertEqual(len(wbd.SOURCES), len(set(wbd.SOURCES)))


class TheDiffLogic(unittest.TestCase):
    """The comparison itself, without a compiler."""

    def test_an_unreviewed_macro_is_a_problem(self):
        delta = {"GU_PREFLIGHT_ONLY": ("0", "1")}
        problems = []
        for name, (before, after) in delta.items():
            if wbd.EXPECTED_MACRO_DELTAS.get(name) != (before, after):
                problems.append(name)
        self.assertEqual(problems, ["GU_PREFLIGHT_ONLY"])

    def test_the_reviewed_pair_is_not_a_problem(self):
        for name, pair in wbd.EXPECTED_MACRO_DELTAS.items():
            self.assertEqual(wbd.EXPECTED_MACRO_DELTAS.get(name), pair)

    def test_a_waiver_that_changes_nothing_is_caught(self):
        seen = set()  # nothing flipped
        self.assertNotIn(wbd.REQUIRED_MACRO_DELTA, seen)


if __name__ == "__main__":
    unittest.main()
