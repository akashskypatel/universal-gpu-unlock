"""Run the entrypoint's own two-card contract through the shipped C mock.

``tests/test_gpu_discovery.c`` is the oldest and broadest host test of the
discovery/entrypoint path: a mocked UEFI system table, a mocked PCI world, and
3660-3764 checks against the real ``gpu_discovery.c``. It is compiled directly by
CI in several ``gpu-selection.h`` configurations, which meant the two-card
expectations were pinned only inside the workflow: a change to
``SelectPreparedRecords`` could break them without any ``python3 -m unittest``
module failing.

This module runs the same three configurations from the host suite, so the
contract is reachable the same way every other host test is:

* preparation-only with **two** selected locations -- the whole inventory is
  prepared, the unselected peer is revalidated, and nothing is mutated;
* ordered single-target compilation with three declared locations -- the shape
  the waiver admits, compiled but never run;
* single-target with three declared locations must **fail to compile**, because
  that build may only ever name one card.

No PCI/GPU hardware is touched: every device here is the mock's.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "src"
TEST = ROOT / "tests" / "test_gpu_discovery.c"
SOURCES = [str(TEST), str(INCLUDE / "gpu_inventory.c"), str(INCLUDE / "gpu_execution_state.c")]


def selection_header(locations: list[tuple[int, int, int, int]]) -> str:
    body = ", ".join("{ %dU,%dU,%dU,%dU }" % item for item in locations)
    return ("#define GU_GPU_SELECTION_COUNT %dU\n"
            "#define GU_GPU_SELECTION_LOCATIONS { %s }\n" % (len(locations), body))


class TwoCardContract(unittest.TestCase):
    def build(self, definitions: list[str], locations, expect_success: bool,
              extra: list[str] | None = None):
        compiler = os.environ.get("CC", "gcc")
        with tempfile.TemporaryDirectory() as directory:
            include = Path(directory)
            (include / "gpu-selection.h").write_text(selection_header(locations),
                                                     encoding="utf-8")
            binary = include / "test"
            command = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", *definitions, "-I", str(include), "-I", str(INCLUDE),
                       *SOURCES, "-o", str(binary)]
            if extra:
                command = [compiler, "-std=c11", *definitions, "-I", str(include),
                           "-I", str(INCLUDE), "-fsyntax-only", str(TEST)]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=120, check=False)
            if not expect_success:
                return built, None
            self.assertEqual(built.returncode, 0, built.stderr[-2000:])
            executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                      timeout=120, check=False)
            return built, executed

    def test_preparation_only_build_prepares_two_cards_without_mutating(self):
        _built, executed = self.build(
            ["-DGU_ACTIVE_SEQUENCE_PREPARE_ONLY"],
            [(1, 7, 4, 1), (0, 3, 0, 0)], True)
        self.assertEqual(executed.returncode, 0, executed.stderr[-2000:])
        self.assertIn("PASS active selected-entrypoint refusal", executed.stdout)
        self.assertIn("GuInit never called", executed.stdout)

    def test_ordered_single_target_build_compiles_with_three_locations(self):
        _built, executed = self.build(
            ["-DGU_ACTIVE_SINGLE_TARGET", "-DGU_SYNTHETIC_COMPILE_ONLY"],
            [(0, 7, 0, 0), (0, 9, 0, 0), (0, 3, 0, 0)], True)
        self.assertEqual(executed.returncode, 0, executed.stderr[-2000:])
        self.assertIn("PASS", executed.stdout)

    def test_single_target_build_refuses_three_locations_at_compile_time(self):
        built, _executed = self.build(
            ["-DGU_ACTIVE_SINGLE_TARGET"],
            [(0, 7, 0, 0), (0, 9, 0, 0), (0, 3, 0, 0)], False,
            extra=["-fsyntax-only"])
        self.assertNotEqual(built.returncode, 0,
                            "a three-card single-target build must not compile")
        self.assertIn("#error", built.stderr)

    def test_the_two_card_expectation_is_still_in_the_mock(self):
        """The contract this module runs must still be the one the C test asserts."""
        text = TEST.read_text(encoding="utf-8")
        self.assertIn(
            "CHECK(GuPrepareGpuSequence(&system_table,NULL,0,&plan)==EFI_SUCCESS);", text)
        self.assertIn("plan.inventory.count==2 && plan.selected_count==0", text)


if __name__ == "__main__":
    unittest.main()
