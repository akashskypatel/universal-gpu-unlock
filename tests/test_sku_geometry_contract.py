"""Contract tests for the per-SKU register geometry gate (A5, partial).

The pinned transaction uses one captured chip's register offsets. These tests
pin the properties that make the gate fail closed:

  1. every Ada id the image admits is classified in exactly one place, so an id
     added to the device list cannot silently inherit AD102 offsets;
  2. only a chip whose own dump was captured is marked captured, and only the
     reference AD102 pair is;
  3. prepare refuses an unclassified Ada part before the first BAR access, and
     says which chip and why;
  4. the vendor transaction refuses too, before it consumes the one-shot
     attempt, so a caller that skipped prepare cannot reach the offsets;
  5. the read-only diagnostic deliberately does not consult the gate - it is
     how the missing capture is obtained;
  6. no caller branches on the table to *admit* a previously refused actuation.

Nothing here claims a captured table is a verified transaction: for every row
ACTUATION_VERIFIED=0 still holds.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
SIM = ROOT / "sim"
TOOLS = ROOT / "tools"

HEADER = (SRC / "gpu_sku_geometry.h").read_text(encoding="utf-8")
TABLE = (SRC / "gpu_sku_geometry.c").read_text(encoding="utf-8")
DEVICE_IDS = (SRC / "gpu_device_ids.h").read_text(encoding="utf-8")
PROFILE = (SRC / "profile.c").read_text(encoding="utf-8")
VENDOR = (SRC / "vendor" / "unlock_610.c").read_text(encoding="utf-8")
SIM_MAIN = (SIM / "sim_main.c").read_text(encoding="utf-8")
RUN_SIM = (TOOLS / "run_simulation.py").read_text(encoding="utf-8")


def ada_ids():
    body = re.search(r"#define GU_ADA_DEVICE_IDS\(X\)(.*?)\n\n", DEVICE_IDS, re.S)
    assert body, "GU_ADA_DEVICE_IDS not found"
    return {int(value, 16) for value in re.findall(r"X\((0x[0-9a-fA-F]+)U\)", body.group(1))}


def table_rows():
    rows = {}
    for device, sku, chip, captured in re.findall(
        r"\{(0x[0-9a-fA-F]+)U,\s*\"([^\"]*)\",\s*\"([^\"]*)\",\s*([01])\}", TABLE
    ):
        device = int(device, 16)
        assert device not in rows, "duplicate row for %#x" % device
        rows[device] = (sku, chip, int(captured))
    return rows


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
    raise AssertionError("unbalanced function body for " + name)


class EveryAdmittedIdIsClassified(unittest.TestCase):
    def test_table_covers_exactly_the_admitted_ada_ids(self):
        rows = set(table_rows())
        self.assertEqual(rows, ada_ids(),
                         "geometry table and GU_ADA_DEVICE_IDS disagree")

    def test_table_has_no_non_ada_rows(self):
        # Ampere and Turing take the generic branches; an Ada row for them
        # would let the gate answer for a part it was not written for.
        body = re.search(r"#define GU_AMPERE_DEVICE_IDS\(X\)(.*?)\n\n", DEVICE_IDS, re.S)
        turing = re.search(r"#define GU_TURING_DEVICE_IDS\(X\)(.*)", DEVICE_IDS, re.S)
        other = set()
        for block in (body, turing):
            if block:
                other |= {int(v, 16) for v in re.findall(r"X\((0x[0-9a-fA-F]+)U\)",
                                                         block.group(1))}
        self.assertFalse(set(table_rows()) & other)


class OnlyCapturedChipsAreCaptured(unittest.TestCase):
    def test_captured_set_is_the_reference_ad102_pair(self):
        captured = {device for device, (_, _, flag) in table_rows().items() if flag}
        self.assertEqual(captured, {0x2684, 0x2685})

    def test_captured_flag_matches_the_chip_label(self):
        for device, (_, chip, flag) in table_rows().items():
            self.assertEqual(flag, 1 if chip == "AD102" else 0,
                             "captured flag disagrees with chip label for %#x"
                             % device)

    def test_every_row_names_chip_and_sku(self):
        for device, (sku, chip, _) in table_rows().items():
            self.assertTrue(chip, "no chip label for %#x" % device)
            self.assertTrue(sku, "no sku label for %#x" % device)

    def test_lookup_is_a_single_table(self):
        # One table: the gate must not be re-implemented per call site.
        self.assertEqual(TABLE.count("const GU_SKU_GEOMETRY Ada[]"), 1)
        self.assertNotIn("0x2684", PROFILE)
        self.assertNotIn("0x2684", VENDOR)


class PrepareRefusesBeforeAnyBarAccess(unittest.TestCase):
    def test_prepare_consults_the_gate(self):
        body = function_body(PROFILE, "EFI_STATUS GuAd104Prepare(VOID)")
        self.assertIn("GuSkuGeometryRefuses(Gu.device_id)", body)
        self.assertIn("EFI_UNSUPPORTED", body)
        self.assertIn("REASON=NO_CAPTURED_REGISTER_TABLE", body)

    def test_refusal_precedes_every_hardware_read(self):
        body = function_body(PROFILE, "EFI_STATUS GuAd104Prepare(VOID)")
        gate = body.index("GuSkuGeometryRefuses")
        for later in ("ConfigFor(&Gu)", "RegistersFor(&Gu", "GuAlloc(",
                      "GuCopyFromIo(", "GuAd104AssetsValid"):
            self.assertGreater(body.index(later), gate,
                               "%s runs before the SKU gate" % later)

    def test_refusal_precedes_the_state_check(self):
        # A refusal must not depend on mutable state: the card is the wrong
        # chip whether or not a previous attempt ran.
        body = function_body(PROFILE, "EFI_STATUS GuAd104Prepare(VOID)")
        self.assertLess(body.index("GuSkuGeometryRefuses"),
                        body.index("state invalid"))

    def test_refusal_names_chip_sku_and_keeps_diagnostics(self):
        body = function_body(PROFILE, "EFI_STATUS GuAd104Prepare(VOID)")
        refusal = body[body.index("GuSkuGeometryRefuses"):]
        self.assertIn("DEVICE=%04x", refusal)
        self.assertIn("CHIP=%s", refusal)
        self.assertIn("SKU=%s", refusal)
        self.assertIn("DIAGNOSTIC_ALLOWED=1", refusal)


class VendorTransactionRefusesToo(unittest.TestCase):
    def test_transaction_checks_the_gate(self):
        self.assertIn("GuSkuGeometryRefuses(pdev->device)", VENDOR)

    def test_refusal_precedes_consuming_the_attempt(self):
        body = function_body(VENDOR, "int gpu_unlock_610_unlock(")
        gate = body.index("GuSkuGeometryRefuses(pdev->device)")
        consumed = body.index("owner->transaction_attempted=TRUE")
        self.assertLess(gate, consumed,
                        "an unsupported SKU would consume the one-shot attempt")

    def test_refusal_precedes_the_admission_block(self):
        body = function_body(VENDOR, "int gpu_unlock_610_unlock(")
        gate = body.index("GuSkuGeometryRefuses(pdev->device)")
        self.assertLess(gate, body.index("!gpu_is_supported_device(pdev->device)"))

    def test_ad102_branches_require_captured_geometry(self):
        body = function_body(VENDOR, "static bool is_ad102(")
        self.assertIn("GuSkuGeometryAdaCaptured", body)
        self.assertIn("gpu_is_ada_device", body)


class DiagnosticStaysAvailable(unittest.TestCase):
    def test_diagnostic_does_not_consult_the_gate(self):
        body = function_body(PROFILE, "EFI_STATUS GuDiagnosticScan(")
        self.assertNotIn("GuSkuGeometry", body)


class GateIsWiredEverywhere(unittest.TestCase):
    def test_image_builds_compile_the_table(self):
        for script in ("Build-Dxe-Driver.ps1", "Build-Experimental-MSVC.ps1"):
            text = (SRC / script).read_text(encoding="utf-8")
            self.assertIn("gpu_sku_geometry.c", text, script)

    def test_simulator_compiles_the_table(self):
        self.assertIn('"gpu_sku_geometry.c"', RUN_SIM)

    def test_simulator_has_a_refused_sku_scenario(self):
        self.assertIn('"ad104-sku"', RUN_SIM)
        self.assertIn('("prepare", "ad104-sku")', RUN_SIM)
        self.assertIn("SimGpuSetDeviceId(gpu, 0x2786)", SIM_MAIN)
        self.assertIn("SKU_GEOMETRY=REFUSED DEVICE=2786 CHIP=AD104", SIM_MAIN)
        self.assertIn("SimGpuWpr2Opens() == 0", SIM_MAIN)


class GateCannotWidenSilently(unittest.TestCase):
    def test_header_documents_that_refusal_is_fail_closed(self):
        self.assertIn("fails closed", " ".join(HEADER.split()))

    def test_refusal_predicate_is_the_only_admission_rule(self):
        # AdaCaptured must not be used to admit the actuation path: that would
        # turn a captured table into a verified transaction.
        for name, text in (("profile.c", PROFILE), ("unlock_610.c", VENDOR)):
            self.assertNotIn("GuSkuGeometryAdaCaptured(Gu.device_id)", text, name)

    def test_no_row_claims_verification(self):
        # "captured" is the strongest claim a row may carry.
        self.assertNotIn("Verified", TABLE)
        self.assertIn("ACTUATION_VERIFIED=0", TABLE)


if __name__ == "__main__":
    unittest.main()
