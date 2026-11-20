"""Host contract for the read-only VRAM block probe.

The waived actuation writes a fixed set of BAR0 words. Before the first
actuation the operator runs a read-only image that addresses *the same words*
so a bad address is discovered by a read rather than by a write. That claim is
only as good as the coupling between the two lists, so this module derives the
write set from ``src/vendor/unlock_610.c`` mechanically and holds
``src/vram_blocks.h`` (and therefore the probe table) to it:

* every ``write_bar(work, <expression>, ...)`` site resolves to an absolute
  BAR0 offset for both engines (``gu_sec2``, ``gu_gsp``);
* every ``write_selector(work, "SSx", GU_FEAT_SSx_OFFSET, ...)`` call resolves
  to the selector word;
* the derived set must equal the probe's block set exactly, so a new write site
  cannot be added without a matching probe row, and the probe cannot invent an
  address the writer never touches;
* the probe function itself must stay read-only: no register write, no detach,
  no unlock, no Stop, no DMA, no reset, no FLR, no read-modify-write readback.

Nothing here runs on hardware. These tests read source text only.
SPDX-License-Identifier: GPL-2.0-only
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WRITER = ROOT / "src" / "vendor" / "unlock_610.c"
GFW = ROOT / "src" / "vendor" / "gfw_eval.h"
BLOCKS = ROOT / "src" / "vram_blocks.h"
PROBE = ROOT / "src" / "gpu_topology_efi.c"
BUILD = ROOT / "src" / "Build-Gpu-Topology-Diagnostic.ps1"
VERIFIER = ROOT / "tools" / "verify_run_log.py"
FIXTURES = ROOT / "tests" / "fixtures" / "hardware_session"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _define(text: str, name: str) -> int:
    found = re.findall(rf"^#define\s+{name}\s+(0x[0-9a-fA-F]+)u?\b", text, re.MULTILINE)
    assert found, f"missing #define {name}"
    assert len(set(found)) == 1, f"conflicting definitions of {name}"
    return int(found[0], 16)


def engine_offsets() -> dict[str, dict[str, int]]:
    """Resolve gu_sec2 and gu_gsp to absolute BAR0 bases."""
    text = _read(WRITER)
    macros = {
        "SEC2_BASE": _define(text, "SEC2_BASE"),
        "SEC2_FBIF": _define(text, "SEC2_FBIF"),
        "SEC2_FALCON2": _define(text, "SEC2_FALCON2"),
    }
    engines: dict[str, dict[str, int]] = {}
    for name in ("gu_sec2", "gu_gsp"):
        match = re.search(rf"struct falcon_eng\s+{name}\s*=\s*\{{(.*?)\}};", text, re.DOTALL)
        assert match, f"missing {name} initializer"
        body = match.group(1)
        fields: dict[str, int] = {}
        for field in ("base", "fbif", "falcon2"):
            item = re.search(rf"\.{field}\s*=\s*(0x[0-9a-fA-F]+)u?|\.{field}\s*=\s*([A-Z0-9_]+)", body)
            assert item, f"{name} has no .{field}"
            literal, macro = item.groups()
            fields[field] = int(literal, 16) if literal else macros[macro]
        engines[name] = fields
    return engines


def structural_writes() -> set[int]:
    """Absolute offsets of every concrete write_bar() and write_selector()."""
    text = _read(WRITER)
    engines = engine_offsets()
    gfw = _read(GFW)
    base_macro = _define(text, "SEC2_BASE")
    offsets: set[int] = set()
    for call in re.findall(r"write_bar\(work,\s*([^,]+),\s*", text):
        expr = call.strip()
        # The generic form inside write_selector() carries the caller's offset.
        if expr == "offset":
            continue
        delta_match = re.search(r"\+\s*(0x[0-9a-fA-F]+)u?", expr)
        delta = int(delta_match.group(1), 16) if delta_match else 0
        if expr.startswith("SEC2_BASE"):
            offsets.add(base_macro + delta)
            continue
        if expr in {"eng->base", "eng->fbif", "eng->falcon2"} or expr.startswith(
            ("eng->base", "eng->falcon2", "eng->fbif")
        ):
            field = re.match(r"eng->([a-z0-9]+)", expr).group(1)
            for engine in engines.values():
                offsets.add(engine[field] + delta)
            continue
        if expr.startswith("base"):
            # falcon_dma(work, base, ...) is called with eng->base for both
            # engines, so a "base + delta" write lands at both engine bases.
            for engine in engines.values():
                offsets.add(engine["base"] + delta)
            continue
        raise AssertionError(f"unresolved write_bar() target: {expr!r}")
    for name, macro in re.findall(
        r'write_selector\(work,\s*"(SS[01])",\s*([A-Z0-9_]+)\s*,', text
    ):
        want = f"GU_FEAT_{name}_OFFSET"
        assert macro == want, f"write_selector({name}) passes {macro}, expected {want}"
        offsets.add(_define(gfw, want))
    return offsets


def probe_blocks() -> tuple[set[int], set[int]]:
    """Probe table offsets: (write set, read-only context)."""
    text = _read(BLOCKS)
    writes, context = set(), set()
    for name, value in re.findall(
        r"^#define\s+(GU_VRAM_BLOCK_[A-Z0-9_]+)\s+(0x[0-9a-fA-F]+)u?\s*$", text, re.MULTILINE
    ):
        if name.startswith("GU_VRAM_BLOCK_CONTEXT_"):
            context.add(int(value, 16))
        else:
            writes.add(int(value, 16))
    return writes, context


def strip_comments(source: str) -> str:
    """Remove C comments so prose cannot satisfy or trip a code check."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", source)


def probe_function() -> str:
    """The probe's guarded block: its table, its context table, its function."""
    source = _read(PROBE)
    start = source.index("static void ReportVramBlocks(int pci_ok) {")
    guard = source.rindex("#ifdef GU_VRAM_BLOCK_PROBE", 0, start)
    assert source.count("#ifdef GU_VRAM_BLOCK_PROBE") >= 2
    end = source.index("\n}\n", start) + len("\n}\n")
    body = source[start:end]
    # Brace balance proves the slice covers the whole function and nothing more.
    assert body.count("{") == body.count("}")
    return source[guard:end]


class WriteSetTest(unittest.TestCase):
    def test_writer_resolves_to_expected_shape(self):
        # Twenty engine deltas at two engines plus the two selectors. A change
        # here means the actuating sequence gained or lost a register, which is
        # exactly the event this suite must not absorb silently.
        structural = structural_writes()
        self.assertEqual(len(structural), 42, sorted(hex(v) for v in structural))
        sel = {0x0082381C, 0x00823820}
        self.assertTrue(sel <= structural)
        engine_writes = structural - sel
        self.assertEqual(len(engine_writes), 40)
        sec2 = {v - 0x00840000 for v in engine_writes if 0x00840000 <= v < 0x00850000}
        gsp = {v - 0x00110000 for v in engine_writes if 0x00110000 <= v < 0x00120000}
        self.assertEqual(len(sec2), 20, sorted(hex(v) for v in sec2))
        self.assertEqual(
            sec2,
            gsp,
            "the two engines must be written at the same register deltas",
        )
        # Named anchors: engine id/halt, CPUCTL, mailbox0/1, and the falcon2
        # window expressed as an offset from the engine base.
        for delta in (0x3C0, 0x100, 0x130, 0x104, 0x40, 0x44, 0x84,
                      0x10C, 0x110, 0x114, 0x118, 0x11C, 0x128,
                      0x600, 0x624, 0x1180, 0x1198, 0x119C, 0x1210, 0x1668):
            self.assertIn(delta, sec2, f"engine delta 0x{delta:x} is not written")

    def test_probe_covers_exactly_the_write_set(self):
        derived = structural_writes()
        probed, _ = probe_blocks()
        self.assertEqual(
            sorted(hex(v) for v in derived - probed),
            [],
            "the actuating sequence writes offsets the probe never reads",
        )
        self.assertEqual(
            sorted(hex(v) for v in probed - derived),
            [],
            "the probe reads offsets the actuating sequence never writes",
        )

    def test_context_words_are_not_part_of_the_write_set(self):
        _, context = probe_blocks()
        self.assertEqual(context, {0x00823804, 0x00823818})
        self.assertEqual(context & structural_writes(), set())

    def test_every_probe_row_is_reached_by_name(self):
        text = _read(BLOCKS)
        names = set(
            re.findall(
                r"^#define\s+(GU_VRAM_BLOCK_[A-Z0-9_]+)\s+0x[0-9a-fA-F]+u?\s*$",
                text,
                re.MULTILINE,
            )
        )
        self.assertEqual(len(names), 44)
        function = probe_function()
        missing = sorted(n for n in names if n not in function)
        self.assertEqual(missing, [], "probe table does not reference these blocks")
        # Duplicate table rows would silently double the read count.
        used = re.findall(r"\{\"[A-Z0-9_.]+\",(GU_VRAM_BLOCK_[A-Z0-9_]+)\}", function)
        self.assertEqual(len(used), len(set(used)))
        self.assertEqual(len(used), len(names))


class ReadOnlyTest(unittest.TestCase):
    FORBIDDEN = (
        "GuWrite32",
        "GuCompatWrite32",
        "write_bar",
        "Mem.Write",
        "GuCompatDmaBarrier(",
        "NvStop",
        "nv_stop",
        "Flr",
        "FLR(",
        "ExitBootServices",
        "SetVariable",
        "detach",
    )

    def test_probe_issues_no_writes_and_no_lifecycle_calls(self):
        function = strip_comments(probe_function())
        for token in self.FORBIDDEN:
            self.assertNotIn(token, function, f"probe body contains {token}")

    def test_probe_reads_through_pci_io_only(self):
        function = strip_comments(probe_function())
        reads = re.findall(r"pci->Mem\.Read\(", function)
        self.assertEqual(len(reads), 2, "expected one read pair: table and context")
        self.assertIn("EfiPciIoWidthUint32", function)
        # One 32-bit read per row, no width or bar-index variation.
        self.assertEqual(function.count("EfiPciIoWidthUint32"), 2)

    def test_probe_is_fingerprint_gated_and_refuses(self):
        function = strip_comments(probe_function())
        self.assertIn("GuMsiB850KnownRomDiagnosticHardware(&Snapshot)", function)
        self.assertIn("REASON=PLATFORM_FINGERPRINT_NO_MATCH", function)
        self.assertIn("REASON=PCI_OWNER_OR_BDF_MISMATCH", function)
        self.assertIn("NO_GPU_READS=1", function)

    def test_dispatch_is_guarded(self):
        source = _read(PROBE)
        call = source.index("ReportVramBlocks(pci_ok);")
        guard = source.rindex("#ifdef GU_VRAM_BLOCK_PROBE", 0, call)
        between = source[guard:call]
        self.assertNotIn("#endif", between, "ReportVramBlocks call is outside its guard")
        self.assertIn("#ifdef GU_VRAM_BLOCK_PROBE", source)

    def test_probe_cannot_share_an_image_with_a_write_capable_probe(self):
        source = _read(PROBE)
        self.assertIn(
            "#error The VRAM block probe must be built as its own image", source
        )
        build = _read(BUILD)
        self.assertIn("[switch]$VramBlockProbe", build)
        self.assertIn("/DGU_VRAM_BLOCK_PROBE", build)
        self.assertIn("VRAM-BLOCK-PROBE-DIAGNOSTIC.EFI", build)
        self.assertIn("'-VramBlockProbe requires -SaveLog", build)
        self.assertIn("$VramBlockProbe.IsPresent).Where({ $_ }).Count -gt 1", build)


class LogContractTest(unittest.TestCase):
    def test_bracket_and_summary_tokens(self):
        source = _read(PROBE)
        for token in (
            "VRAM_BLOCK_PROBE_BEGIN ",
            "VRAM_BLOCK_PROBE_CARD ",
            "VRAM_BLOCK_PROBE_END ",
            "VRAM_BLOCK_PROBE GPU=",
            "ROLE=WRITE",
            "ROLE=READ_CONTEXT",
            "STATUS=OBSERVED",
            "STATUS=UNAVAILABLE",
            "NO_GPU_WRITES=1",
            "NO_FALLBACK=1",
            "SECURITY_ACCEPTANCE=UNVERIFIED",
            "ACCEPTANCE=UNVERIFIED",
            "INCOMPLETE_READ_SET=1",
            "DIGEST_CV=CHANGE_DETECTOR_ONLY",
            "READS_SUCCEEDED=%u READS_FAILED=%u",
        ):
            self.assertIn(token, source, f"log token missing: {token}")

    def test_begin_line_states_the_read_only_boundary(self):
        source = _read(PROBE)
        start = source.index('"VRAM_BLOCK_PROBE_BEGIN SOURCE=UNLOCK_610_WRITE_SET')
        end = source.index('\\r\\n"', start)
        # The Say() argument is a run of adjacent literals; the emitted line is
        # their concatenation with all source whitespace removed.
        begin = " ".join(source[start:end].replace('"', " ").split())
        for token in (
            "READ_ONLY=1",
            "NO_STOP=1",
            "NO_DMA=1",
            "NO_RESET=1",
            "NO_UNLOCK=1",
            "NO_FLR=1",
            "CAN_DETACH=0",
            "CAN_UNLOCK=0",
            "CAN_FLASH=0",
            "NO_GPU_WRITES=1",
            "BLOCKS=%u",
            "CONTEXT=%u",
        ):
            self.assertIn(token, begin, f"begin line missing {token}")

    def test_the_documented_evidence_lines_are_whole_emitted_lines(self):
        # Every quoted token in the session guide must be a complete line this
        # file prints, not a fragment: a verifier scores the line it is given.
        manifest = _read(ROOT / "docs" / "hardware_session.json")
        for token in re.findall(r'"token": "(VRAM_BLOCK_PROBE[^"]*)"', manifest):
            self.assertIn(token, _read(PROBE), f"quoted line is not in the source: {token!r}")

    def test_no_signature_or_acceptance_inference(self):
        function = strip_comments(probe_function())
        # The probe reports raw words; it must not name a signature slot or
        # claim an authentication result.
        for token in ("SIGNATURE_SLOT=", "SECURE_ACCEPTANCE=PASS", "ACCEPTANCE=VERIFIED"):
            self.assertNotIn(token, function)


class VerifierScoresTheProbe(unittest.TestCase):
    """The gate is only worth having if it fails when the probe is incomplete."""

    GATE = "GATE R4_WRITE_SET_READ_BEFORE_ACTUATION STAGE=readonly"

    def verify(self, paths: list[Path]) -> str:
        result = subprocess.run(
            [sys.executable, str(VERIFIER), *[str(p) for p in paths], "--self-test"],
            capture_output=True, text=True, cwd=ROOT)
        return result.stdout

    def mutate(self, old: str, new: str) -> Path:
        text = (FIXTURES / "readonly_vram_blocks.txt").read_text(encoding="utf-8")
        self.assertTrue(old in text, f"fixture line not found: {old!r}")
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write(text.replace(old, new, 1))
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        return Path(handle.name)

    def test_a_complete_probe_log_passes_the_gate(self):
        out = self.verify([FIXTURES / "readonly_vram_blocks.txt"])
        self.assertIn(f"{self.GATE} RESULT=PASS", out)
        self.assertIn("RUN_LOG_VERDICT=PASS_SELF_TEST", out)
        self.assertIn("HARDWARE_VERIFIED=0", out)

    def test_one_unreadable_block_fails_the_gate(self):
        out = self.verify([self.mutate(
            "BLOCK=SEC2.SIG_DMEM BAR0_OFFSET=0x841210 ROLE=WRITE STATUS=OBSERVED",
            "BLOCK=SEC2.SIG_DMEM BAR0_OFFSET=0x841210 ROLE=WRITE STATUS=UNAVAILABLE EFI_STATUS=0x800000000000000e")])
        self.assertIn(f"{self.GATE} RESULT=FAIL", out)
        self.assertIn("FORBIDDEN", out)
        self.assertIn("RUN_LOG_VERDICT=FAIL_SELF_TEST", out)

    def test_a_short_block_list_fails_the_gate(self):
        text = (FIXTURES / "readonly_vram_blocks.txt").read_text(encoding="utf-8")
        lines = [line for line in text.splitlines()
                 if not line.startswith("VRAM_BLOCK_PROBE GPU=3080")]
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write("\n".join(lines) + "\n")
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        out = self.verify([Path(handle.name)])
        self.assertIn(f"{self.GATE} RESULT=PASS", out, "42 lines from one card still clears the floor")
        # Now drop one card's worth of lines entirely and keep only a fragment.
        lines = [line for line in text.splitlines()
                 if line.startswith("VRAM_BLOCK_PROBE GPU=")][:41]
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write("\n".join(lines + [line for line in text.splitlines()
                                        if not line.startswith("VRAM_BLOCK_PROBE GPU=")]) + "\n")
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        out = self.verify([Path(handle.name)])
        self.assertIn(f"{self.GATE} RESULT=FAIL", out)
        self.assertIn("TOO_FEW", out)

    def test_a_boot_without_the_probe_is_not_applicable_not_a_pass(self):
        out = self.verify([FIXTURES / "readonly_two.txt"])
        self.assertIn(f"{self.GATE} RESULT=NOT_APPLICABLE", out)
        self.assertNotIn(f"{self.GATE} RESULT=PASS", out)
        self.assertIn("RUN_LOG_VERDICT=PASS_SELF_TEST", out)


if __name__ == "__main__":
    unittest.main()
