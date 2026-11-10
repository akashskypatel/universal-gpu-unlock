"""Host-only negative tests for truthful static firmware-evidence admission."""
import copy
import importlib.util
import unittest
from pathlib import Path

file = Path(__file__).resolve().parents[1] / "tools" / "check_firmware_evidence.py"
spec = importlib.util.spec_from_file_location("check_firmware_evidence", file)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

A = "a" * 64
B = "b" * 64
C = "c" * 64


def fixture():
    reference = {
        "schema": "nvidia-reference-boot-assets-comparison/1",
        "status": "NEARBY_LINUX_REFERENCE_COMPARISON_COMPLETE",
        "original_reference": "610.43.03",
        "comparison_reference": "615.78.08",
        "windows_driver_under_investigation": "617.42",
        "windows_gsp_version_observed": "615.78.05",
        "exact_windows_615_78_05_asset_identity_verified": False,
        "source_archive_sha256": A,
        "all_six_reference_assets_byte_identical": False,
        "firmware_signature_authenticated": False, "hardware_tested": False,
        "can_detach": False, "can_unlock": False, "can_flash": False, "assets": {}
    }
    for name in m.ASSETS:
        equal = name in m.BOOT
        if equal:
            reference["assets"][name] = {
                "reference": {"version": "610.43.03", "bytes": 84, "sha256": A},
                "candidate": {"version": "615.78.08", "bytes": 84, "sha256": A},
                "byte_identical": True}
        else:
            parts = lambda sig: {
                "metadata": {"bytes": 72, "sha256": sig},
                "image": {"bytes": 4096, "sha256": sig},
                "signatures": {"bytes": 768, "sha256": sig}}
            reference["assets"][name] = {
                "reference": {"version": "610.43.03", "bytes": 4936, "sha256": A},
                "candidate": {"version": "615.78.08", "bytes": 4936, "sha256": B},
                "byte_identical": False,
                "components": {
                    "reference": parts(A), "candidate": parts(B),
                    "byte_identical": {p: False for p in ("metadata","image","signatures")}}}
    windows = {
        "schema": "nvidia-windows-gsp-container-inventory/1",
        "gsp": {
            "bytes": 30000, "sha256": A, "format": "ELF64_LE_RISCV_ET_REL",
            "fwversion": "615.78.05",
            "fwversion_raw_hex": "3631352e37382e303500",
            "section_header_count": 4, "section_name_table_index": 3,
            "sections": [
                {"index": 0, "name": "", "offset": 0, "size": 0},
                {"index": 1, "name": ".fwimage", "offset": 64, "size": 1000},
                {"index": 2, "name": ".fwversion", "offset": 2000, "size": 10},
                {"index": 3, "name": ".fwsignature_ad10x", "offset": 2048, "size": 4096},
                {"index": 4, "name": ".fwsignature_ga10x", "offset": 6144, "size": 4096},
            ],
            "signature_section_digests": {
                ".fwsignature_ad10x": {"bytes": 4096, "sha256": B},
                ".fwsignature_ga10x": {"bytes": 4096, "sha256": C}},
            "signature_sections_authenticated": False},
        "ucodes": {
            "bytes": 58000000, "sha256": B, "format": "NVUCODES",
            "header_bytes": 24, "format_version": 1, "declared_bindata_entries": 1293,
            "records_parsed": False, "filename_index_fingerprint": "0x2d38befecdb703a6"},
        "static_fingerprint_comparison": {
            "search_performed": True, "occurrences": 1, "unique_occurrence": True,
            "fingerprint_hex": "0x2d38befecdb703a6",
            "first_file_offset": 200, "first_fwimage_relative_offset": 136,
            "runtime_pairing_verified": False},
        "package_comparison": {"gsp": {"same_size": True, "same_sha256": True},
                               "ucodes": {"same_size": True, "same_sha256": True}},
        "package_files_identical": True, "firmware_signature_verified": False,
        "gsp_ucodes_pair_accepted_by_driver": False, "hardware_tested": False,
        "can_unlock": False, "can_flash": False
    }
    windows["gsp"]["section_header_count"] = len(windows["gsp"]["sections"])
    return reference, windows


class EvidenceTests(unittest.TestCase):
    def test_valid_metadata_is_not_hardware_permission(self):
        a, b = fixture()
        evidence = m.assess(a, b)
        self.assertEqual(evidence["status"], "STRUCTURAL_EVIDENCE_CONSISTENT")
        self.assertEqual(len(evidence["linux_identical_boot_assets"]), 4)
        self.assertFalse(evidence["windows_61742_secure_load_verified"])
        for flag in ("can_detach", "can_unlock", "can_flash", "recovery_verified"):
            self.assertFalse(evidence[flag])

    def test_booter_component_false_equal_rejected(self):
        a, b = fixture()
        a["assets"][m.BOOTERS[0]]["components"]["byte_identical"]["signatures"] = True
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_booter_component_size_mismatch_rejected(self):
        a, b = fixture()
        a["assets"][m.BOOTERS[1]]["components"]["candidate"]["image"]["bytes"] += 1
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_gsp_boot_signature_difference_rejected(self):
        a, b = fixture()
        a["assets"][m.BOOT[0]]["candidate"]["sha256"] = B
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_gsp_version_mismatch_rejected(self):
        a, b = fixture()
        b["gsp"]["fwversion"] = "615.78.08"
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_signature_presence_not_authentication(self):
        a, b = fixture()
        b["firmware_signature_verified"] = True
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_unparsed_nvucodes_cannot_be_called_parsed(self):
        a, b = fixture()
        b["ucodes"]["records_parsed"] = True
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_static_fingerprint_not_runtime_pairing(self):
        a, b = fixture()
        b["static_fingerprint_comparison"]["runtime_pairing_verified"] = True
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_fingerprint_offset_consistency(self):
        a, b = fixture()
        b["static_fingerprint_comparison"]["first_file_offset"] += 1
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_duplicate_signature_section_not_accepted(self):
        a, b = fixture()
        b["gsp"]["sections"][4]["name"] = ".fwsignature_ad10x"
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_unexpected_package_mismatch_rejected(self):
        a, b = fixture()
        b["package_comparison"]["ucodes"]["same_sha256"] = False
        with self.assertRaises(ValueError):
            m.assess(a, b)

    def test_report_claim_cannot_authorize_unlock(self):
        a, b = fixture()
        a["can_unlock"] = True
        with self.assertRaises(ValueError):
            m.assess(a, b)


if __name__ == "__main__":
    unittest.main()
