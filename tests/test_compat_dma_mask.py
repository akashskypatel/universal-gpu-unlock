"""Compile the production DMA mask macro with real host C compilers.

A 64-bit shift must not run for a 64-bit DMA address mask.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HEADER = Path(__file__).resolve().parents[1] / "src/compat/compat.h"


class DmaMaskTests(unittest.TestCase):
    def test_production_macro_at_64_bit_boundary(self):
        source = HEADER.read_text(encoding="utf-8")
        matches = re.findall(r"^#define\s+DMA_BIT_MASK\(n\)\s+.*$", source, re.MULTILINE)
        self.assertEqual(len(matches), 1, "Expected one production DMA_BIT_MASK macro")
        program = r"""
#include <stdint.h>
""" + matches[0] + r"""
int main(void) {
    const unsigned widths[] = {0, 1, 16, 32, 63, 64};
    const uint64_t expected[] = {
        0ull, 1ull, 65535ull, 0xffffffffull,
        0x7fffffffffffffffull, 0xffffffffffffffffull
    };
    for (unsigned i = 0; i < sizeof(widths)/sizeof(widths[0]); ++i) {
        volatile unsigned n = widths[i];
        if ((uint64_t)DMA_BIT_MASK(n) != expected[i])
            return (int)i + 1;
    }
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "dma_mask.c"
            src.write_text(program, encoding="utf-8")
            for compiler in ("gcc", "clang"):
                with self.subTest(compiler=compiler):
                    binary = Path(directory) / ("dma_mask_" + compiler)
                    built = subprocess.run(
                        [compiler, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                         "-pedantic", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                         str(src), "-o", str(binary)],
                        capture_output=True, text=True, check=False)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    checked = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


if __name__ == "__main__":
    unittest.main()
