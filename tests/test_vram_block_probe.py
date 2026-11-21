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
* the same derivation over the writer's reads, minus the write set, must equal
  the probe's read-context list, so an abort point (poll_us() or an inactive
  read_bar()) cannot be missing from the evidence;
* the probe function itself must stay read-only: no register write, no detach,
  no unlock, no Stop, no DMA, no reset, no FLR, no read-modify-write readback.

Nothing here runs on hardware. These tests read source text only.
SPDX-License-Identifier: GPL-2.0-only
"""

from __future__ import annotations

from pathlib import Path
import json
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


def structural_reads() -> set[int]:
    """Absolute offsets of every statically resolvable read in the writer."""
    text = _read(WRITER)
    engines = engine_offsets()
    gfw = _read(GFW)
    resolved: set[int] = set()
    for expr in set(re.findall(r"(?:read_bar|poll_us)\(work,\s*([^,]+),", text)):
        expr = expr.strip()
        if expr in {"offset", "addr2", "ad102_permissions[i].offset"} or expr.startswith("addr2"):
            continue  # runtime base or per-chip table: not a static address
        delta_match = re.search(r"\+\s*(0x[0-9a-fA-F]+)u?", expr)
        delta = int(delta_match.group(1), 16) if delta_match else 0
        if re.fullmatch(r"0x[0-9a-fA-F]+", expr):
            resolved.add(int(expr, 16))
            continue
        if expr.isdigit():
            resolved.add(0)
            continue
        if expr.startswith("SEC2_BASE"):
            resolved.add(_define(text, "SEC2_BASE") + delta)
            continue
        if expr.startswith("eng->"):
            field = re.match(r"eng->([a-z0-9]+)", expr).group(1)
            for engine in engines.values():
                resolved.add(engine[field] + delta)
            continue
        if expr.startswith("base"):
            for engine in engines.values():
                resolved.add(engine["base"] + delta)
            continue
        if re.fullmatch(r"[A-Z0-9_]+", expr):
            try:
                resolved.add(_define(gfw, expr))
            except AssertionError:
                try:
                    resolved.add(_define(text, expr))
                except AssertionError:
                    continue
            continue
        # `0x00001400 + 0x0e * 4` and fuse tables: a computed address, skip.
        if "+" in expr and "*" in expr:
            continue
        raise AssertionError(f"unresolved read target: {expr!r}")
    return resolved


def function_body(path: Path, signature: str) -> str:
    """The text of one function, brace-matched, so a test can compile it."""
    text = _read(path)
    start = text.index(signature)
    depth, i = 0, start
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1


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

    def test_context_words_are_exactly_the_writers_read_only_reads(self):
        # The probe's read context must be the writer's whole read set minus the
        # words it also writes: every other read is a place poll_us() or an
        # inactive read_bar() aborts the transaction, so a word missing here is
        # an abort point the operator cannot see before a boot.
        _, context = probe_blocks()
        reads = structural_reads()
        writes = structural_writes()
        self.assertEqual(
            sorted(hex(v) for v in (reads - writes) - context),
            [],
            "the writer reads words the probe never reports",
        )
        self.assertEqual(
            sorted(hex(v) for v in context - (reads - writes)),
            [],
            "the probe reports a context word the writer does not read, or writes",
        )
        self.assertEqual(context & writes, set())
        # The words that decide whether an actuating run gets started at all.
        for anchor in (0x008400F4, 0x001100F4, 0x008403C0, 0x00841388,
                       0x00111388, 0x00823804, 0x00000000):
            self.assertIn(anchor, reads | writes, f"0x{anchor:08x} is not read")
        self.assertIn(0x008400F4, context, "falcon_reset() must poll HwCfg")
        self.assertIn(0x00841388, context, "the RISC-V halt word must be read")

    def test_the_rom_predicate_is_the_transactions_predicate(self):
        # src/prom_identity.h decides whether a board is a candidate at all, and
        # the probe copies that decision. Both functions are compiled here and
        # driven over the same buffers: agreement is the point, not a reread of
        # the same prose in two places.
        header = ROOT / "src" / "prom_identity.h"
        probe = _read(PROBE)
        defines = "\n".join(re.findall(
            r"^#define\s+GU_VRAM_ROM_(?:OFFSET|BYTES|CHUNK)\s+\S+$", probe, re.MULTILINE))
        self.assertTrue(defines, "the probe does not define its ROM window constants")
        harness = (
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "#include <string.h>\n"
            "typedef unsigned char UINT8;\n"
            "typedef unsigned long UINTN;\n"
            "typedef unsigned int UINT32;\n"
            f'#include "{header.as_posix()}"\n'
            + defines + "\n"
            + function_body(PROBE, "static UINT32 GuVramRomWord(const UINT8 *rom,UINTN at)")
            + "\n"
            + function_body(PROBE, "static UINT32 GuVramRomMatches(const UINT8 *rom,UINTN bytes,UINT32 device)")
            + "\n"
            "static unsigned char *rom;\n"
            "static size_t bytes = GU_VRAM_ROM_BYTES;\n"
            "static unsigned dev = 0x2684u;\n"
            "static int bad;\n"
            "static void check(const char *label) {\n"
            "  int a = GuPromImageMatchesDevice(rom, bytes, dev);\n"
            "  UINT32 b = GuVramRomMatches(rom, (UINTN)bytes, (UINT32)dev);\n"
            "  if ((a ? 1 : 0) != (int)b) { printf(\"DISAGREE %s a=%d b=%u\\n\", label, a, b); bad = 1; }\n"
            "}\n"
            "static void pcir(const char *magic, unsigned vendor, unsigned device) {\n"
            "  memcpy(rom + 0xe0, magic, 4); rom[0xe4] = vendor & 0xff; rom[0xe5] = vendor >> 8;\n"
            "  rom[0xe6] = device & 0xff; rom[0xe7] = device >> 8;\n"
            "}\n"
            "static void fresh(void) {\n"
            "  memset(rom, 0, bytes); rom[0] = 0x55; rom[1] = 0xaa; rom[0x18] = 0xe0; rom[0x19] = 0x00;\n"
            "  pcir(\"PCIR\", 0x10de, 0x2684);\n"
            "}\n"
            "int main(void) {\n"
            "  rom = malloc(bytes);\n"
            "  if (!rom) return 2;\n"
            "  fresh(); check(\"valid\");\n"
            "  fresh(); dev = 0x220a; check(\"wrong-live-device\"); dev = 0x2684;\n"
            "  fresh(); rom[0] = 0x56; rom[1] = 0x4e; check(\"signature-4e56\");\n"
            "  fresh(); rom[0] = 0x77; rom[1] = 0xbb; check(\"signature-bb77\");\n"
            "  fresh(); rom[0] = 0x00; rom[1] = 0x00; check(\"signature-invalid\");\n"
            "  fresh(); rom[0x18] = 0xff; rom[0x19] = 0xff; check(\"pcir-out-of-range\");\n"
            "  fresh(); pcir(\"PCIx\", 0x10de, 0x2684); check(\"pcir-magic\");\n"
            "  fresh(); pcir(\"PCIR\", 0x10df, 0x2684); check(\"vendor\");\n"
            "  fresh(); pcir(\"PCIR\", 0x10de, 0x220a); check(\"device-field\");\n"
            "  fresh(); dev = 0x10000u; check(\"device-too-wide\"); dev = 0x2684;\n"
            "  fresh(); bytes = GU_VRAM_ROM_BYTES - 1; check(\"short-buffer\"); bytes = GU_VRAM_ROM_BYTES;\n"
            "  { unsigned char *saved = rom; rom = NULL; check(\"null-buffer\"); rom = saved; }\n"
            "  if (bad) return 1;\n"
            "  printf(\"AGREE %d\\n\", 11);\n"
            "  free(rom);\n"
            "  return 0;\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "rom.c"
            source.write_text(harness, encoding="utf-8")
            binary = Path(directory) / "rom"
            build = subprocess.run(
                ["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("AGREE 11", run.stdout)

    def test_the_rom_check_is_handed_the_device_half_of_the_identity(self):
        # -0008 shipped with the vendor half in this argument and reported
        # LIVE_DEVICE=0x10de / MATCH=0 for a ROM that names the card exactly. The
        # encoding is pinned here against the same split the topology printer
        # uses for the identical field, so a swap fails a test instead of a boot.
        function = strip_comments(probe_function())
        self.assertIn("ReportVramRom(", function)
        call = re.search(r"ReportVramRom\([^;]*?\);", function, re.DOTALL)
        self.assertIsNotNone(call)
        self.assertIn("id->pci_id>>16", call.group(0),
                      "the ROM check must be handed the device half")
        self.assertNotIn("id->pci_id & 0xffff", call.group(0),
                         "the vendor half is not a device id")
        # The printer that renders the same field as %04x:%04x is the anchor, and
        # it lives outside the probe block, so read the whole translation unit.
        whole = strip_comments(_read(PROBE))
        printer = re.search(
            r"PCI\[%zu\][\s\S]{0,400}?pci_id&65535U\)[\s\S]{0,200}?pci_id>>16\)", whole)
        self.assertIsNotNone(
            printer, "the PCI line no longer splits pci_id into vendor then device")

    def test_the_rom_window_is_the_window_the_transaction_reads(self):
        probe = _read(PROBE)
        offset = re.search(r"#define\s+GU_VRAM_ROM_OFFSET\s+(0x[0-9a-fA-F]+)[uU]?", probe)
        size = re.search(r"#define\s+GU_VRAM_ROM_BYTES\s+(\d+)[uU]?", probe)
        self.assertIsNotNone(offset, "the probe does not name its ROM window offset")
        self.assertIsNotNone(size, "the probe does not name its ROM window size")
        offset_value = int(offset.group(1), 16)
        size_value = int(size.group(1))
        # profile.c copies exactly this window with GuCopyFromIo, and fwsec reads
        # the same one again, so the three literals have to agree.
        profile = _read(ROOT / "src" / "profile.c")
        writer = _read(WRITER)
        literal = f"0x{offset_value:x}"
        self.assertIn(f"Gu.bar_base+{literal}", profile,
                      "preparation copies a different window than the probe reads")
        self.assertIn(literal, writer,
                      "the transaction reads a different window than the probe reads")
        self.assertEqual(size_value, 1048576)
        self.assertIn(str(size_value), profile)
        self.assertIn("1024 * 1024", writer)

    def test_the_probe_classifies_with_the_writers_own_predicate(self):
        # Both functions are compiled here and driven over the same matrix: the
        # probe must call a word inaccessible exactly when the transaction does.
        harness = (
            "#include <stdio.h>\n"
            "#include <stdint.h>\n"
            "typedef int bool;\n"
            "typedef uint32_t u32;\n"
            "typedef uint32_t UINT32;\n"
            "#define true 1\n"
            "#define false 0\n"
            + function_body(WRITER, "static bool inaccessible(u32 value)")
            + "\n"
            + function_body(PROBE, "static UINT32 GuVramBlockAccessible(UINT32 value)")
            + "\n"
            "int main(void) {\n"
            "  unsigned i;\n"
            "  static const unsigned v[] = {0x0u,0x1u,0x10u,0x110u,0x3c0u,0x5a4u,\n"
            "    0x00000400u,0x71306153u,0xffffff8fu,0x00410080u,\n"
            "    0xbadf0000u,0xbadf5620u,0xbadfffffu,0xffffffffu,\n"
            "    0xffff0000u,0xbadf0001u,0x12345678u};\n"
            "  for (i = 0; i < sizeof(v)/sizeof(v[0]); i++) {\n"
            "    if (GuVramBlockAccessible(v[i]) == (unsigned)!!inaccessible(v[i])) {\n"
            "      printf(\"DISAGREE %#x\\n\", v[i]);\n"
            "      return 1;\n"
            "    }\n"
            "  }\n"
            "  printf(\"AGREE %zu\\n\", sizeof(v)/sizeof(v[0]));\n"
            "  return 0;\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "classify.c"
            source.write_text(harness, encoding="utf-8")
            binary = Path(directory) / "classify"
            build = subprocess.run(
                ["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("AGREE 17", run.stdout)

    def test_the_probe_calls_the_poison_value_not_accessable(self):
        # The first physical run returned 0xbadf5620 on 45 of 84 words; the
        # probe must name that class instead of reporting it as entry state.
        function = strip_comments(probe_function())
        self.assertIn("GuVramBlockAccessible(", function)
        self.assertIn('"NOT_ACCESSIBLE"', function)
        self.assertIn('"STATE"', function)
        self.assertIn("NOT_ACCESSIBLE=%u", function)

    def test_every_probe_row_is_reached_by_name(self):
        text = _read(BLOCKS)
        names = set(
            re.findall(
                r"^#define\s+(GU_VRAM_BLOCK_[A-Z0-9_]+)\s+0x[0-9a-fA-F]+u?\s*$",
                text,
                re.MULTILINE,
            )
        )
        self.assertEqual(len(names), 66)
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
        # Three call sites: the write-set loop, the read-context loop, and the ROM
        # window's header dword plus its chunked copy.
        self.assertEqual(len(reads), 4, "expected the word loops and the ROM window")
        self.assertIn("EfiPciIoWidthUint32", function)
        # One 32-bit read per row, no width or bar-index variation there.
        self.assertEqual(function.count("EfiPciIoWidthUint32"), 3)
        self.assertIn("EfiPciIoWidthUint8", function, "the ROM copy is byte-wide")

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
            "ACCESS=%s",
            '"STATE"',
            '"NOT_ACCESSIBLE"',
            "OBSERVED=%u NOT_ACCESSIBLE=%u",
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
        # A captured run is the exception, and it is checked against the log it
        # quotes instead: a log records what an older build printed, so evolving
        # the source must not rewrite history.
        manifest = json.loads((ROOT / "docs" / "hardware_session.json").read_text(encoding="utf-8"))
        source = _read(PROBE)
        for gate in manifest["gates"]:
            for token in re.findall(r"(VRAM_BLOCK_PROBE[^`\"]*)", gate.get("token", "")):
                self.assertIn(token.strip(), source, f"gate token is not in the source: {token!r}")
        for key in ("evidence", "advisories"):
            for entry in manifest.get(key, []):
                for token in re.findall(r"(VRAM_BLOCK_PROBE[^`\"]*)", json.dumps(entry)):
                    self.assertIn(token.strip(), source, f"quoted line is not in the source: {token!r}")

    def test_every_captured_run_quote_is_in_the_log_it_quotes(self):
        manifest = json.loads((ROOT / "docs" / "hardware_session.json").read_text(encoding="utf-8"))
        checked = 0
        for run in manifest["captured_runs"]["runs"]:
            log = (ROOT / run["file"]).read_text(encoding="utf-8", errors="replace")
            for observation in run.get("observations", []):
                token = observation.get("token", "")
                if not token.startswith("VRAM_BLOCK_PROBE"):
                    continue
                self.assertIn(token, log, f"{run['id']} does not contain {token!r}")
                checked += 1
        self.assertGreater(checked, 0, "no captured-run probe line is quoted anywhere")

    def test_no_signature_or_acceptance_inference(self):
        function = strip_comments(probe_function())
        # The probe reports raw words; it must not name a signature slot or
        # claim an authentication result.
        for token in ("SIGNATURE_SLOT=", "SECURE_ACCEPTANCE=PASS", "ACCEPTANCE=VERIFIED"):
            self.assertNotIn(token, function)


class TheRomReadoutNamesTheCardInTheLog(unittest.TestCase):
    """A stored ROM line must agree with the log's own GPU lines.

    -0008 reported LIVE_DEVICE=0x10de - the vendor - for a ROM naming 10de:2684,
    because the reader was handed the wrong half of pci_id. That is a defect a
    reader can see in one line but no gate could: the gate asks only that the
    readout exist. This test asks the arithmetic question instead, for every
    stored run, using the identity the same log prints.
    """

    def test_every_rom_readout_agrees_with_the_identity_in_its_own_log(self):
        manifest = json.loads((ROOT / "docs" / "hardware_session.json").read_text(encoding="utf-8"))
        checked = 0
        for run in manifest["captured_runs"]["runs"]:
            path = ROOT / run["file"]
            if not path.exists():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            live_devices = {
                match.group(1): int(match.group(3), 16)
                for match in re.finditer(
                    r"GPU\[\d+\] BDF=(\d{4}:[0-9a-f]{2}:[0-9a-f]{2}\.\d) "
                    r"ID=([0-9a-f]{4}):([0-9a-f]{4})", text)
            }
            for match in re.finditer(
                    r"VRAM_BLOCK_PROBE_ROM GPU=(\S+) BAR0_OFFSET=0x300000 BYTES=\d+ "
                    r"SIGNATURE=0x([0-9a-f]{4}) PCIR_OFFSET=0x([0-9a-f]{4}) "
                    r"PCIR_VENDOR=0x([0-9a-f]{4}) PCIR_DEVICE=0x([0-9a-f]{4}) "
                    r"LIVE_DEVICE=0x([0-9a-f]{4}) MATCH=(\d)", text):
                gpu, signature, _pcir, vendor, device, live, match_flag = match.groups()
                vendor = int(vendor, 16); device = int(device, 16)
                live = int(live, 16); match_flag = int(match_flag)
                checked += 1
                self.assertIn(device, live_devices.values(),
                              f"{run['id']}: the ROM names device {device:#06x} and "
                              f"no GPU line in the log does")
                if "probe_defect" not in run:
                    self.assertEqual(live, device,
                                     f"{run['id']}: LIVE_DEVICE must be the device the "
                                     f"ROM names, not {live:#06x}")
                    self.assertEqual(match_flag, 1 if vendor == 0x10de else 0,
                                     f"{run['id']}: MATCH must follow the comparison")
                self.assertIn(int(signature, 16), (0xAA55, 0x4E56, 0xBB77), run["id"])
        self.assertGreater(checked, 0, "no stored run carries a ROM readout")


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

    def test_the_rom_gate_scores_a_structural_readout(self):
        self.ROM_GATE = "GATE R5_ROM_WINDOW_STRUCTURE STAGE=readonly"
        out = self.verify([FIXTURES / "readonly_vram_blocks.txt"])
        self.assertIn(f"{self.ROM_GATE} RESULT=PASS", out)

    def test_a_missing_rom_readout_fails_the_gate(self):
        self.ROM_GATE = "GATE R5_ROM_WINDOW_STRUCTURE STAGE=readonly"
        text = (FIXTURES / "readonly_vram_blocks.txt").read_text(encoding="utf-8")
        kept = [line for line in text.splitlines()
                if not line.startswith("VRAM_BLOCK_PROBE_ROM ")]
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write("\n".join(kept) + "\n")
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        out = self.verify([Path(handle.name)])
        self.assertIn(f"{self.ROM_GATE} RESULT=FAIL", out)
        self.assertIn("MISSING", out)

    def test_an_unreadable_rom_window_is_a_note_and_a_failed_gate(self):
        self.ROM_GATE = "GATE R5_ROM_WINDOW_STRUCTURE STAGE=readonly"
        text = (FIXTURES / "readonly_vram_blocks.txt").read_text(encoding="utf-8")
        lines = text.splitlines()
        out_lines = []
        for line in lines:
            if line.startswith("VRAM_BLOCK_PROBE_ROM GPU=4090 "):
                out_lines.append("ROM_WINDOW_UNREADABLE GPU=4090 BAR0_OFFSET=0x300000 "
                                 "SIGNATURE=0x0000 REASON=NOT_A_ROM_HEADER NO_COPY=1 READ_ONLY=1")
            else:
                out_lines.append(line)
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write("\n".join(out_lines) + "\n")
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        out = self.verify([Path(handle.name)])
        self.assertIn("NOTE ROM_WINDOW_UNREADABLE PRESENT", out)
        self.assertIn(f"{self.ROM_GATE} RESULT=FAIL", out)
        self.assertIn("FORBIDDEN", out)

    def test_a_log_without_the_rom_counter_is_not_applicable(self):
        self.ROM_GATE = "GATE R5_ROM_WINDOW_STRUCTURE STAGE=readonly"
        text = (FIXTURES / "readonly_vram_blocks.txt").read_text(encoding="utf-8")
        text = text.replace(" OBSERVED=72 NOT_ACCESSIBLE=60 ROMS=2",
                            " OBSERVED=72 NOT_ACCESSIBLE=60")
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        out = self.verify([Path(handle.name)])
        self.assertIn(f"{self.ROM_GATE} RESULT=NOT_APPLICABLE", out)

    def test_a_boot_without_the_probe_is_not_applicable_not_a_pass(self):
        out = self.verify([FIXTURES / "readonly_two.txt"])
        self.assertIn(f"{self.GATE} RESULT=NOT_APPLICABLE", out)
        self.assertNotIn(f"{self.GATE} RESULT=PASS", out)
        self.assertIn("RUN_LOG_VERDICT=PASS_SELF_TEST", out)



class TheCapturedWindowIsGated(unittest.TestCase):
    """R6 asks for the file, not the digest. A build that only hashes the window
    cannot pass it by omission, and a run that read both cards must have written
    both files or it fails.
    """

    GATE = "GATE R6_ROM_WINDOW_CAPTURED STAGE=readonly"

    def verify(self, paths: list[Path]) -> str:
        result = subprocess.run(
            [sys.executable, str(VERIFIER), *[str(p) for p in paths], "--self-test"],
            capture_output=True, text=True, cwd=ROOT)
        return result.stdout

    def temporary(self, text: str) -> Path:
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        return Path(handle.name)

    def fixture(self) -> str:
        return (FIXTURES / "readonly_vram_blocks.txt").read_text(encoding="utf-8")

    def test_a_run_that_captured_both_windows_passes(self):
        out = self.verify([FIXTURES / "readonly_vram_blocks.txt"])
        self.assertIn(f"{self.GATE} RESULT=PASS", out)
        self.assertIn("HARDWARE_VERIFIED=0", out)

    def test_one_captured_window_is_not_both(self):
        text = self.fixture()
        lines = [line for line in text.splitlines()
                 if not line.startswith("VRAM_BLOCK_PROBE_ROM_FILE GPU=3080")]
        self.assertEqual(len(lines), len(text.splitlines()) - 1)
        out = self.verify([self.temporary("\n".join(lines) + "\n")])
        self.assertIn(f"{self.GATE} RESULT=FAIL", out)

    def test_a_window_read_but_not_written_fails(self):
        text = self.fixture().replace(
            "VRAM_BLOCK_PROBE_ROM_FILE GPU=3080 BYTES=1048576 FILE=",
            "VRAM_BLOCK_PROBE_ROM_FILE=UNAVAILABLE GPU=3080 BYTES=1048576 REASON=FILE_NOT_WRITTEN NO_COPY=1 READ_ONLY=1 IGNORED=",
            1)
        self.assertIn("REASON=FILE_NOT_WRITTEN", text)
        out = self.verify([self.temporary(text)])
        self.assertIn(f"{self.GATE} RESULT=FAIL", out)
        self.assertIn("FORBIDDEN", out)

    def test_a_build_that_only_hashed_the_window_is_not_applicable(self):
        text = "\n".join(line for line in self.fixture().splitlines()
                         if not line.startswith("VRAM_BLOCK_PROBE_ROM_FILE"))
        out = self.verify([self.temporary(text + "\n")])
        self.assertIn(f"{self.GATE} RESULT=NOT_APPLICABLE", out)
        self.assertNotIn(f"{self.GATE} RESULT=PASS", out)

    def test_the_capture_line_is_the_one_the_image_prints(self):
        """The gate reads the image's own format string, so a reworded line
        fails here rather than on the bench."""
        source = _read(ROOT / "src" / "gpu_topology_efi.c")
        manifest = json.loads((ROOT / "docs" / "hardware_session.json")
                              .read_text(encoding="utf-8"))
        gate = next(g for g in manifest["gates"] if g["id"] == "R6_ROM_WINDOW_CAPTURED")
        self.assertIn(gate["requires"][0]["token"], source)
        self.assertIn(gate["forbids"][0]["token"], source)
        self.assertIn(gate["only_if"]["token"], source)


PROLOGUE = r"""
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "PROM_FILENAME_HEADER"
typedef void VOID;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef size_t UINTN;
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef wchar_t CHAR16;
typedef int EFI_GUID;
#define EFI_SUCCESS 0ULL
#define EFI_ERR(n) ((1ULL<<63)|(n))
#define EFI_ERROR(x) (((x)>>63)!=0)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_DEVICE_ERROR EFI_ERR(7)
#define EFI_VOLUME_FULL EFI_ERR(11)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_COMPROMISED_DATA EFI_ERR(33)
#define EFI_FILE_MODE_READ 1ULL
#define EFI_FILE_MODE_WRITE 2ULL
#define EFI_FILE_MODE_CREATE 4ULL
typedef struct {UINT32 segment,bus,device,function;} GU_GPU_RECORD;
typedef struct file EFI_FILE_PROTOCOL;
struct file {
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *,EFI_FILE_PROTOCOL **,CHAR16 *,uint64_t,uint64_t);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *);
  EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *,UINTN *,VOID *);
  EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *);
  EFI_STATUS (*Delete)(EFI_FILE_PROTOCOL *);
};
typedef struct fs EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct fs {EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,EFI_FILE_PROTOCOL **);};
typedef struct {EFI_HANDLE DeviceHandle;} EFI_LOADED_IMAGE_PROTOCOL;
typedef struct {EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);} EFI_BOOT_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;} EFI_SYSTEM_TABLE;
static EFI_GUID LoadedGuid,FsGuid;
static void Say(const char *format,...) {(void)format;}
static EFI_FILE_PROTOCOL root,file,prior,foreign;
static unsigned mode,opens,creates,writes,flushes,deletes,root_closes,file_closes;
static unsigned prior_closes,foreign_closes,protocols,volumes;
static UINTN write_bytes;
static char first_name[GU_PROM_FILE_CHARS];
static void Remember(CHAR16 *name) {
  size_t i;
  for(i=0;i+1U<GU_PROM_FILE_CHARS && name[i];i++) first_name[i]=(char)name[i];
  first_name[i]=0;
}
static EFI_STATUS Close(EFI_FILE_PROTOCOL *f) {
  if(f==&root){root_closes++;return mode==7?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  if(f==&file){file_closes++;return mode==6?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  if(f==&prior){prior_closes++;return mode==8?EFI_DEVICE_ERROR:EFI_SUCCESS;}
  foreign_closes++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS Open(EFI_FILE_PROTOCOL *f,EFI_FILE_PROTOCOL **out,CHAR16 *name,uint64_t flags,uint64_t attrs) {
  (void)f;(void)attrs;
  if(!first_name[0]) Remember(name);
  opens++;
  if(flags&EFI_FILE_MODE_CREATE) {
    creates++;
    if(mode==3){*out=&foreign;return EFI_DEVICE_ERROR;}
    if(mode==10){*out=&root;return EFI_SUCCESS;}
    *out=&file;return EFI_SUCCESS;
  }
  /* mode 1: the first name is taken and the next one is free.
   * mode 8: the same prior name is found but cannot be closed. */
  if((mode==1||mode==8) && opens==1){*out=&prior;return EFI_SUCCESS;}
  if(mode==9){*out=&root;return EFI_SUCCESS;}
  return EFI_NOT_FOUND;
}
static EFI_STATUS Write(EFI_FILE_PROTOCOL *f,UINTN *n,VOID *data) {
  (void)f;(void)data;
  writes++;write_bytes=*n;
  if(mode==4||mode==5)return EFI_DEVICE_ERROR;
  if(mode==11){if(*n)*n=*n-1U;return EFI_SUCCESS;}
  return EFI_SUCCESS;
}
static EFI_STATUS Flush(EFI_FILE_PROTOCOL *f) {(void)f;flushes++;return EFI_SUCCESS;}
static EFI_STATUS Delete(EFI_FILE_PROTOCOL *f) {
  (void)f;deletes++;return mode==5?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **out) {
  (void)fs;volumes++;
  if(mode==2){*out=&foreign;return EFI_DEVICE_ERROR;}
  *out=&root;return EFI_SUCCESS;
}
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs={Volume};
static EFI_LOADED_IMAGE_PROTOCOL li={(EFI_HANDLE)1};
static EFI_STATUS Protocol(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
  (void)h;protocols++;
  if(mode==12)return EFI_DEVICE_ERROR;
  if(guid==&LoadedGuid)*out=&li;else *out=&fs;
  return EFI_SUCCESS;
}
"""


CAPTURE_MAIN = r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d mode %u: %s\n",__LINE__,mode,#x);return 1;}}while(0)
int main(void) {
  EFI_BOOT_SERVICES bs={Protocol};
  EFI_SYSTEM_TABLE table={&bs};
  GU_GPU_RECORD target={0,1,0,0};
  UINT8 rom[16];
  char text[GU_PROM_FILE_CHARS];
  unsigned run;
  memset(rom,0x5a,sizeof(rom));
  root=(EFI_FILE_PROTOCOL){Open,Close,NULL,NULL,Delete};
  file=(EFI_FILE_PROTOCOL){NULL,Close,Write,Flush,Delete};
  prior=(EFI_FILE_PROTOCOL){NULL,Close,NULL,NULL,NULL};
  foreign=prior;
  for(mode=0;mode<=12;mode++) {
    opens=creates=writes=flushes=deletes=root_closes=file_closes=0;
    prior_closes=foreign_closes=protocols=volumes=0;
    write_bytes=0;first_name[0]=0;PromFileFault=0;
    memset(text,0,sizeof(text));text[0]=(char)-1;
    int ok=SavePromWindow((EFI_HANDLE)1,&table,&target,rom,sizeof(rom),text,sizeof(text));
    CHECK(!foreign_closes);
    switch(mode) {
    case 0:
      CHECK(ok==1);
      CHECK(strcmp(text,"\\PROM-0000-01-00-0-0000.BIN")==0);
      CHECK(strcmp(first_name,text)==0);
      CHECK(writes==1 && write_bytes==sizeof(rom) && flushes==1);
      CHECK(file_closes==1 && root_closes==1 && prior_closes==0 && deletes==0);
      break;
    case 1:
      /* The first name is taken: the capture moves to the next run index
       * instead of refusing to write at all. */
      CHECK(ok==1);
      CHECK(strcmp(text,"\\PROM-0000-01-00-0-0001.BIN")==0);
      CHECK(strcmp(first_name,"\\PROM-0000-01-00-0-0000.BIN")==0);
      CHECK(prior_closes==1 && creates==1 && writes==1);
      break;
    case 2:
      /* The volume could not be opened: no file operations, the capture's own
       * latch is set, and the caller is told by a zero return. */
      CHECK(ok==0 && text[0]==0);
      CHECK(volumes==1 && opens==0 && creates==0 && PromFileFault==1);
      break;
    case 3:
      CHECK(ok==0 && text[0]==0);
      CHECK(PromFileFault==1 && deletes==0 && file_closes==0);
      break;
    case 4:
      /* A write that failed leaves no partial file behind. */
      CHECK(ok==0 && text[0]==0);
      CHECK(PromFileFault==0 && deletes==1 && root_closes==1);
      break;
    case 5:
      CHECK(ok==0 && text[0]==0);
      CHECK(PromFileFault==1 && deletes==1);
      break;
    case 6:
      CHECK(ok==0 && text[0]==0);
      CHECK(file_closes==1 && root_closes==1);
      break;
    case 7:
      CHECK(ok==0 && text[0]==0);
      CHECK(root_closes==1 && PromFileFault==1);
      break;
    case 8:
      CHECK(ok==0 && text[0]==0);
      CHECK(prior_closes==1 && PromFileFault==1 && writes==0);
      break;
    case 9:
      CHECK(ok==0 && text[0]==0);
      CHECK(PromFileFault==1 && prior_closes==0 && creates==0);
      break;
    case 10:
      CHECK(ok==0 && text[0]==0);
      CHECK(PromFileFault==1 && file_closes==0);
      break;
    case 11:
      /* A short write is not a complete capture. */
      CHECK(ok==0 && text[0]==0);
      CHECK(writes==1 && deletes==1);
      break;
    case 12:
      CHECK(ok==0 && text[0]==0 && volumes==0 && PromFileFault==0);
      break;
    }
    if(PromFileFault) {
      unsigned ops=protocols+volumes+opens+creates+writes+flushes+deletes+
                   root_closes+file_closes+prior_closes;
      CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,rom,sizeof(rom),text,sizeof(text))==0);
      CHECK(ops==protocols+volumes+opens+creates+writes+flushes+deletes+
               root_closes+file_closes+prior_closes);
      CHECK(text[0]==0);
    }
  }
  /* An address that cannot be encoded produces no file at all, and is refused
   * before anything is created. */
  mode=0;target.segment=0x10000U;
  opens=creates=writes=volumes=0;PromFileFault=0;text[0]=(char)-1;
  CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,rom,sizeof(rom),text,sizeof(text))==0);
  CHECK(text[0]==0 && creates==0 && writes==0);
  target.segment=0;
  /* Arguments are refused before any service is called. */
  protocols=0;
  CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,NULL,sizeof(rom),text,sizeof(text))==0);
  CHECK(SavePromWindow((EFI_HANDLE)1,&table,NULL,rom,sizeof(rom),text,sizeof(text))==0);
  CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,rom,0,text,sizeof(text))==0);
  CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,rom,sizeof(rom),NULL,sizeof(text))==0);
  CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,rom,sizeof(rom),text,GU_PROM_FILE_CHARS-1U)==0);
  CHECK(protocols==0);
  /* The name the log prints is the name the file system was asked for, on both
   * cards of the bench and for four runs of each. */
  mode=0;
  for(run=0;run<4;run++) {
    target.bus=run?4U:1U;
    first_name[0]=0;
    memset(text,0,sizeof(text));text[0]=(char)-1;
    CHECK(SavePromWindow((EFI_HANDLE)1,&table,&target,rom,sizeof(rom),text,sizeof(text))==1);
    CHECK(strcmp(first_name,text)==0);
  }
  return 0;
}
"""


class TheCapturedWindowLeavesTheBench(unittest.TestCase):
    """The probe already copies and hashes a megabyte per card. What it lacked
    was a way for those bytes to leave the machine, without which the FWSEC
    question the transaction asks cannot be answered anywhere.

    These tests run the real save path - the real name builder, the real
    open/close helpers and the real capture function - against file mocks, so
    the failure a bench would see is one of these first.
    """

    def program(self) -> str:
        source = _read(ROOT / "src" / "gpu_topology_efi.c")
        helpers = "static EFI_STATUS LogOpenResult(" + source.split(
            "static EFI_STATUS LogOpenResult(", 1)[1].split(
            "\nstatic EFI_STATUS SaveLog(", 1)[0]
        capture = "static int PromFileFault;" + source.split(
            "static int PromFileFault;", 1)[1].split("\n#endif\n#endif", 1)[0]
        header = str(ROOT / "src" / "prom_filename.h")
        body = PROLOGUE.replace("PROM_FILENAME_HEADER", header)
        return body + helpers + "\n" + capture + CAPTURE_MAIN

    def build(self, program: str) -> None:
        handle = tempfile.TemporaryDirectory()
        self.addCleanup(handle.cleanup)
        source = Path(handle.name) / "prom_save.c"
        source.write_text(program, encoding="utf-8")
        for compiler in ("gcc", "clang"):
            with self.subTest(compiler=compiler):
                binary = Path(handle.name) / compiler
                # -fshort-wchar is the EDK II ABI: CHAR16 is 16 bits, as it is
                # in MSVC. Without it a name built in 16-bit units is packed
                # two characters per wchar_t and the file system is handed
                # garbage, which is why this harness would rather fail here.
                build = subprocess.run(
                    [compiler, "-std=c11", "-fshort-wchar",
                     "-O2", "-Wall", "-Wextra", "-Werror",
                     "-pedantic", str(source), "-o", str(binary)],
                    capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                run = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_the_capture_path_writes_and_names_the_window(self):
        self.build(self.program())

    def test_the_capture_keeps_a_latch_the_log_cannot_set(self):
        """A commit that failed on the megabyte must not cost the report of what
        was read, so the capture answers to its own latch."""
        source = _read(ROOT / "src" / "gpu_topology_efi.c")
        save_log = strip_comments(source.split("static EFI_STATUS SaveLog(", 1)[1]
                                  .split("\n#ifdef GU_VRAM_BLOCK_PROBE", 1)[0])
        capture = strip_comments(source.split("static int SavePromWindow(", 1)[1]
                                 .split("\n#endif\n#endif", 1)[0])
        self.assertIn("LogFileFault", save_log)
        self.assertNotIn("PromFileFault", save_log)
        self.assertIn("PromFileFault", capture)
        self.assertNotIn("LogFileFault", capture)
        # ... and the log is still written after a capture fault, because it is
        # the last thing the image does.
        self.assertLess(source.index("static int SavePromWindow("),
                        source.index("EFI_STATUS written=SaveLog(image,table);"))

    def test_the_capture_line_names_the_file_the_writer_created(self):
        source = strip_comments(_read(ROOT / "src" / "gpu_topology_efi.c"))
        self.assertIn(
            '"VRAM_BLOCK_PROBE_ROM_FILE GPU=%s BYTES=%u FILE=%s SHA256=%s '
            'ROLE=PARSE_OFFLINE READ_ONLY=1 ACCEPTANCE=UNVERIFIED', source)
        self.assertIn(
            '"VRAM_BLOCK_PROBE_ROM_FILE=UNAVAILABLE GPU=%s BYTES=%u '
            'REASON=FILE_NOT_WRITTEN NO_COPY=1 READ_ONLY=1', source)
        # One save per card, in the function that already read the window, while
        # the buffer the digest describes is still live.
        body = strip_comments(probe_function())
        self.assertEqual(body.count("SavePromWindow("), 1)
        self.assertLess(body.index("GuSha256("), body.index("SavePromWindow("))
        self.assertLess(body.index("SavePromWindow("), body.rindex("FreePool(rom)"))
        # And the window is still read exactly once per card.
        self.assertEqual(body.count("EfiPciIoWidthUint8"), 1)


def synthetic_rom_window(descriptor_version: int = 3) -> bytes:
    """A 1 MiB window shaped like a capture: an expansion ROM naming the 4090,
    one BIT token pointing at a one-entry ucode table, and one V3 descriptor.

    It is a fixture for the reader, not a VBIOS: nothing here is signed and no
    byte of it came from a card.
    """
    import struct
    rom = bytearray(1024 * 1024)

    def w16(offset: int, value: int) -> None:
        rom[offset:offset + 2] = struct.pack("<H", value)

    def w32(offset: int, value: int) -> None:
        rom[offset:offset + 4] = struct.pack("<I", value)

    w16(0, 0xAA55)
    rom[2] = 0x08
    pcir = 0xE0
    w16(0x18, pcir)
    rom[pcir:pcir + 4] = b"PCIR"
    w16(pcir + 4, 0x10DE)
    w16(pcir + 6, 0x2684)
    w16(pcir + 0x0A, 0)
    w16(pcir + 0x10, 0x08)
    rom[pcir + 0x14] = 0
    rom[pcir + 0x15] = 0x80
    bit, pointer, table, descriptor = 0x800, 0x900, 0xA00, 0x1000
    rom[bit] = 0xFF
    rom[bit + 1] = 0xB8
    rom[bit + 2:bit + 6] = b"BIT\0"
    rom[bit + 8] = 12
    rom[bit + 9] = 8
    rom[bit + 10] = 1
    rom[bit + 11] = 0
    rom[bit + 11] = (-(sum(rom[bit:bit + 12]) & 0xFF)) & 0xFF
    token = bit + 12
    rom[token] = 0x70
    rom[token + 1] = 0x02
    w16(token + 2, 8)
    w32(token + 4, pointer)
    w32(pointer, table)
    rom[table] = 1
    rom[table + 1] = 6
    rom[table + 2] = 6
    rom[table + 3] = 1
    rom[table + 6] = 0x85
    w32(table + 8, descriptor)
    size = 44 + 384
    w32(descriptor, (size << 16) | (descriptor_version << 8) | 1)
    w32(descriptor + 4, 1024)
    w32(descriptor + 8, 0)
    w32(descriptor + 12, 8)
    w32(descriptor + 16, 0)
    w32(descriptor + 20, 384)
    w32(descriptor + 24, 0)
    w32(descriptor + 28, 0)
    w32(descriptor + 32, 512)
    w16(descriptor + 36, 0x400)
    rom[descriptor + 38] = 1
    rom[descriptor + 39] = 1
    w16(descriptor + 40, 1)
    return bytes(rom)


class TheCapturedWindowCanBeParsed(unittest.TestCase):
    """The file the probe writes is only useful if the parser the transaction
    uses can read it. This runs that parser - tests/test_fwsec_v2.c includes
    src/vendor/vbios_610.c itself - over a window shaped like a capture, so the
    bench run's output can be read the moment it lands.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.binary = Path(self._tmp.name) / "fwsec"
        build = subprocess.run(
            ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Isrc", "tests/test_fwsec_v2.c", "-o", str(self.binary)],
            capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(build.returncode, 0, build.stderr)

    def window(self, data: bytes) -> Path:
        path = Path(self._tmp.name) / "PROM-0000-01-00-0-0000.BIN"
        path.write_bytes(data)
        return path

    def parse(self, path: Path) -> subprocess.CompletedProcess:
        return subprocess.run([str(self.binary), str(path)],
                              capture_output=True, text=True, cwd=ROOT)

    def test_a_window_with_a_usable_descriptor_is_accepted(self):
        run = self.parse(self.window(synthetic_rom_window()))
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PROM_FILE FWSEC_FOUND=1", run.stdout)
        self.assertIn("DESCRIPTOR_VERSION=3", run.stdout)
        self.assertIn("ENGINE_MASK=0x400", run.stdout)
        self.assertIn("ACCEPTED=1", run.stdout)
        self.assertIn("SIGNATURE_AUTHENTICATED=0", run.stdout)

    def test_a_window_without_a_descriptor_is_reported_not_guessed(self):
        run = self.parse(self.window(bytes(1024 * 1024)))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("PROM_FILE FWSEC_FOUND=0", run.stdout)
        self.assertNotIn("ACCEPTED=1", run.stdout)

    def test_a_v2_descriptor_is_not_an_admission(self):
        """V2 carries no ucode id or signature count. The transaction refuses
        it, so the report must not call it accepted."""
        run = self.parse(self.window(synthetic_rom_window(descriptor_version=2)))
        self.assertIn("FWSEC_FOUND=0", run.stdout + run.stderr)
        self.assertNotIn("ACCEPTED=1", run.stdout)

    def test_the_admission_bits_are_the_transactions_predicate(self):
        """The four printed bits have to be the four conditions of
        GuAd104FwsecMatches(), or the report answers a different question than
        the firmware does."""
        profile = _read(ROOT / "src" / "profile.c")
        predicate = profile.split("BOOLEAN GuAd104FwsecMatches(", 1)[1].split("\n}", 1)[0]
        self.assertIn("descriptor_version == 3", predicate)
        self.assertIn("ucode_id != 0", predicate)
        self.assertIn("sig_count >= 1", predicate)
        self.assertIn("sig_bytes == (UINT32)f->sig_count * 384", predicate)
        harness = _read(ROOT / "tests" / "test_fwsec_v2.c")
        self.assertIn("v3=fw.descriptor_version==3;", harness)
        self.assertIn("ucode=fw.ucode_id!=0;", harness)
        self.assertIn("count=fw.sig_count>=1;", harness)
        self.assertIn("sigbytes=fw.sig_bytes==(u32)fw.sig_count*384;", harness)
        self.assertIn("accepted=v3&&ucode&&count&&sigbytes;", harness)

    def test_the_capture_the_probe_writes_is_the_one_this_reads(self):
        """The file is 1 MiB and the parser refuses more, so the size the probe
        writes and the size this reads cannot drift."""
        probe = strip_comments(_read(ROOT / "src" / "gpu_topology_efi.c"))
        self.assertIn("#define GU_VRAM_ROM_BYTES  1048576U", probe)
        harness = _read(ROOT / "tests" / "test_fwsec_v2.c")
        self.assertIn("size>1024*1024", harness)
        self.assertEqual(len(synthetic_rom_window()), 1048576)

if __name__ == "__main__":
    unittest.main()
