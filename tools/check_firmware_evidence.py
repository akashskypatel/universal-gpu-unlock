#!/usr/bin/env python3
"""Read-only consistency check for existing Windows 617.42 / Linux 610-615 evidence.

Evaluates *reports*, not firmware bytes. Successful validation NEVER means
signed firmware authentication, runtime acceptance, safe driver Stop or unlock.
No networking, GPU access, EFI calls, writes or firmware substitution.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

HASH = re.compile(r"[0-9a-f]{64}\Z")
BOOTERS = ("booter_load_ad102.bin", "booter_load_ga102.bin")
BOOT = ("gsp_boot_ad102.bin", "gsp_boot_desc_ad102.bin",
        "gsp_boot_ga102.bin", "gsp_boot_desc_ga102.bin")
ASSETS = BOOTERS + BOOT


def require(ok: bool, reason: str) -> None:
    if not ok:
        raise ValueError(reason)


def digest(value: object, location: str) -> None:
    require(isinstance(value, str) and HASH.fullmatch(value) is not None,
            f"{location}: invalid SHA-256")


def record(value: dict, location: str) -> None:
    require(isinstance(value, dict) and type(value.get("bytes")) is int
            and value["bytes"] > 0, f"{location}: invalid byte size")
    digest(value.get("sha256"), location)


def assess(reference: dict, windows: dict) -> dict:
    """Return bounded findings, or refuse structurally contradictory reports."""
    require(reference.get("schema") == "nvidia-reference-boot-assets-comparison/1",
            "reference schema mismatch")
    require(reference.get("original_reference") == "610.43.03"
            and reference.get("comparison_reference") == "615.78.08"
            and reference.get("windows_driver_under_investigation") == "617.42"
            and reference.get("windows_gsp_version_observed") == "615.78.05",
            "reference/target release mismatch")
    require(reference.get("status") == "NEARBY_LINUX_REFERENCE_COMPARISON_COMPLETE"
            and reference.get("all_six_reference_assets_byte_identical") is False
            and reference.get("exact_windows_615_78_05_asset_identity_verified") is False,
            "reference conclusions contradict comparison scope")
    digest(reference.get("source_archive_sha256"), "615 source archive")
    require(set(reference.get("assets", {})) == set(ASSETS), "asset set differs")
    for filename in ASSETS:
        asset = reference["assets"][filename]
        old = asset["reference"]
        new = asset["candidate"]
        record(old, filename + " original")
        record(new, filename + " candidate")
        require(old.get("version") == "610.43.03"
                and new.get("version") == "615.78.08", filename + ": wrong source versions")
        equal = old["bytes"] == new["bytes"] and old["sha256"] == new["sha256"]
        require(asset.get("byte_identical") is equal, filename + ": equality claim contradicted")
        if filename in BOOT:
            require(equal and "components" not in asset,
                    filename + ": previously identical GSP boot component changed")
        else:
            require(not equal, filename + ": Booter unexpectedly equal")
            parts = asset.get("components", {})
            require(parts.get("byte_identical") ==
                    {"metadata": False, "image": False, "signatures": False},
                    filename + ": component equality mismatch")
            for version, outer in (("reference", old), ("candidate", new)):
                pieces = parts.get(version, {})
                require(set(pieces) == {"metadata", "image", "signatures"},
                        filename + ": missing Booter components")
                for name, length in (("metadata", 72), ("signatures", 768)):
                    require(pieces[name].get("bytes") == length,
                            filename + ": wrong " + name + " size")
                for name in ("metadata", "image", "signatures"):
                    record(pieces[name], filename + " " + version + " " + name)
                require(sum(pieces[name]["bytes"] for name in pieces) == outer["bytes"],
                        filename + ": Booter component lengths do not sum to bundle")
            for name in ("metadata", "image", "signatures"):
                require(parts["reference"][name]["sha256"] != parts["candidate"][name]["sha256"],
                        filename + ": changed component has identical hash")

    require(windows.get("schema") == "nvidia-windows-gsp-container-inventory/1",
            "Windows inventory schema mismatch")
    gsp, ucodes = windows["gsp"], windows["ucodes"]
    record(gsp, "Windows GSP")
    record(ucodes, "Windows NVUCODES")
    require(gsp.get("format") == "ELF64_LE_RISCV_ET_REL"
            and gsp.get("fwversion") == "615.78.05"
            and gsp.get("fwversion_raw_hex") == "3631352e37382e303500",
            "Windows GSP version/format contradiction")
    require(ucodes.get("format") == "NVUCODES"
            and ucodes.get("header_bytes") == 24
            and ucodes.get("format_version") == 1
            and ucodes.get("declared_bindata_entries") == 1293
            and ucodes.get("records_parsed") is False,
            "NVUCODES is not a verified parsed index")
    require(gsp.get("section_header_count") == len(gsp.get("sections", []))
            and gsp.get("section_name_table_index") < len(gsp["sections"]),
            "GSP section count/table inconsistency")
    sections = gsp["sections"]
    require(all(s.get("index") == i and type(s.get("offset")) is int
                and type(s.get("size")) is int and s["offset"] >= 0
                and s["size"] >= 0 and s["offset"] + s["size"] <= gsp["bytes"]
                for i, s in enumerate(sections)), "invalid ELF section bounds")
    names = [s["name"] for s in sections]
    require(len(names) == len(set(names)), "ambiguous duplicated ELF section name")
    require(names.count(".fwimage") == 1 and names.count(".fwversion") == 1,
            "missing GSP firmware image/version")
    require(next(s["size"] for s in sections if s["name"] == ".fwversion") == 10,
            "GSP version-size mismatch")
    signatures = gsp.get("signature_section_digests", {})
    expected = {".fwsignature_ad10x", ".fwsignature_ga10x"}
    require(expected.issubset(signatures), "missing target architecture signatures")
    require(set(signatures) == {n for n in names if n.startswith(".fwsignature_")},
            "signature section digest coverage mismatch")
    for name, sig in signatures.items():
        record(sig, name)
        section = next(s for s in sections if s["name"] == name)
        require(sig["bytes"] == section["size"], "signature section size mismatch")
    require(gsp.get("signature_sections_authenticated") is False
            and windows.get("firmware_signature_verified") is False,
            "signature authentication cannot be inferred from SHA-256")

    fp = ucodes.get("filename_index_fingerprint")
    require(isinstance(fp, str) and re.fullmatch(r"0x[0-9a-f]{16}", fp) is not None,
            "invalid NVUCODES fingerprint")
    match = windows.get("static_fingerprint_comparison", {})
    image = next(s for s in sections if s["name"] == ".fwimage")
    file_pos, rel_pos = match.get("first_file_offset"), match.get("first_fwimage_relative_offset")
    require(match.get("search_performed") is True and match.get("occurrences") == 1
            and match.get("unique_occurrence") is True
            and match.get("fingerprint_hex") == fp
            and type(file_pos) is int and type(rel_pos) is int
            and 0 <= rel_pos <= image["size"] - 8
            and file_pos == image["offset"] + rel_pos
            and match.get("runtime_pairing_verified") is False,
            "static fingerprint report does not prove runtime pairing")
    package = windows.get("package_comparison", {})
    require(all(package.get(key) == {"same_size": True, "same_sha256": True}
                for key in ("gsp", "ucodes")) and windows.get("package_files_identical") is True,
            "installer and DriverStore pair not established byte-identical")
    require(windows.get("gsp_ucodes_pair_accepted_by_driver") is False
            and windows.get("hardware_tested") is False
            and windows.get("can_unlock") is False
            and windows.get("can_flash") is False
            and all(reference.get(k) is False for k in
                    ("firmware_signature_authenticated", "hardware_tested",
                     "can_detach", "can_unlock", "can_flash")),
            "report attempts to promote unverified firmware/hardware state")

    return {
        "schema": "nvidia-firmware-evidence-admission/1",
        "status": "STRUCTURAL_EVIDENCE_CONSISTENT",
        "reference_versions": ["610.43.03", "615.78.08"],
        "linux_identical_boot_assets": list(BOOT),
        "linux_changed_booter_components": ["metadata", "image", "signatures"],
        "windows_gsp_fwversion": "615.78.05",
        "windows_package_gsp_ucodes_byte_identical": True,
        "windows_gsp_signature_sections_present": sorted(expected),
        "windows_nvucodes_index_fingerprint_unique_in_fwimage": True,
        "open": [
            "NVUCODES records unparsed",
            "GSP firmware signatures unauthenticated",
            "GSP/ucodes runtime pairing unverified",
            "Windows original signed Booter producer/loader ABI unverified",
            "NVIDIA vendor Stop and AMD post-Stop survival unverified",
            "Exact-card cold recovery and independent mutable state unverified",
        ],
        "windows_61742_secure_load_verified": False,
        "can_detach": False, "can_unlock": False, "can_flash": False,
        "recovery_verified": False
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--windows", type=Path, required=True)
    args = parser.parse_args()
    try:
        reference = json.loads(args.reference.read_text(encoding="utf-8"))
        windows = json.loads(args.windows.read_text(encoding="utf-8"))
        output = assess(reference, windows)
        print(json.dumps(output, indent=2, sort_keys=True))
        print("STATUS=COMPLETE CAN_DETACH=false CAN_UNLOCK=false CAN_FLASH=false "
              "RECOVERY_VERIFIED=false")
        return 0
    except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as error:
        print(f"STATUS=ERROR REASON={error} CAN_DETACH=false CAN_UNLOCK=false "
              "CAN_FLASH=false RECOVERY_VERIFIED=false")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
