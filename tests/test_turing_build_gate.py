"""Build-time evidence for the Turing refusal (A5, second half).

A5's exit criterion asks for Turing to be "refused at *build* time (not only at
prepare)". The runtime refusal was already there; nothing stopped a build from
compiling a Turing actuation path in a tree that carries no TU102 payload.

`src/turing_assets.h` closes that, with two compile-time checks:

  1. defining `GU_TURING_ACTUATION` fails the build with an explanation;
  2. the pinned Turing manifest must keep the row count it was reviewed with, so
     trimming the table cannot silently leave Turing cards admitted.

These tests do not read the header's text and assert on words: they compile a
translation unit that includes it, and require the compiler to fail or not.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
HEADER = (SRC / "turing_assets.h").read_text(encoding="utf-8")
PROFILE = (SRC / "profile.c").read_text(encoding="utf-8")


class TuringBuildGate(unittest.TestCase):
    def compile(self, source: str, *flags: str) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "turing_gate.c"
            path.write_text(source, encoding="utf-8")
            return subprocess.run(
                [os.environ.get("CC", "gcc"), "-std=c11", "-c", "-fsyntax-only",
                 f"-I{SRC}", *flags, str(path)],
                capture_output=True, text=True)

    def test_a_turing_actuation_build_refuses_to_compile(self):
        built = self.compile('#include "turing_assets.h"\nint main(void){return 0;}\n',
                             "-DGU_TURING_ACTUATION")
        self.assertNotEqual(built.returncode, 0, "a Turing actuation build compiled")
        self.assertIn("TU102-ASSETS", built.stderr,
                      "the refusal must say what is missing, not just fail")
        self.assertIn("refused at prepare", built.stderr,
                      "the refusal must say what the tree does instead")

    def test_the_header_is_harmless_in_a_normal_build(self):
        built = self.compile('#include "turing_assets.h"\nint main(void){return 0;}\n')
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_a_manifest_with_the_wrong_row_count_fails_the_build(self):
        # Same shape as the real table: GU_TURING_MANIFEST_ROWS rows pass, one
        # fewer must not.
        good = '''
#include "turing_assets.h"
typedef struct { int entries[GU_TURING_MANIFEST_ROWS]; } TABLE;
static const TABLE TuringAssets = {{ {0} }};
GU_TURING_ROW_COUNT_IS(TuringAssets);
int main(void){return 0;}
'''
        built = self.compile(good)
        self.assertEqual(built.returncode, 0, built.stderr)

        short = good.replace("entries[GU_TURING_MANIFEST_ROWS]", "entries[GU_TURING_MANIFEST_ROWS-1]")
        built = self.compile(short)
        self.assertNotEqual(built.returncode, 0,
                            "a trimmed manifest compiled; the tripwire is inert")

    def test_the_image_actually_includes_the_gate(self):
        self.assertIn('#include "turing_assets.h"', PROFILE)
        self.assertIn("GU_TURING_ROW_COUNT_IS(TuringAssets);", PROFILE)
        # The tripwire has to apply to the real table, not a copy of it.
        table = PROFILE.index("static const GU_ARCH_ASSETS TuringAssets=")
        self.assertLess(table, PROFILE.index("GU_TURING_ROW_COUNT_IS(TuringAssets);"))

    def test_the_gate_does_not_claim_more_than_it_checks(self):
        # It stops an unusable path from shipping; it does not bless the assets.
        self.assertIn("Neither check claims the assets are correct", HEADER)
        self.assertIn("GU_TURING_MANIFEST_ROWS 5U", HEADER)


if __name__ == "__main__":
    unittest.main()
