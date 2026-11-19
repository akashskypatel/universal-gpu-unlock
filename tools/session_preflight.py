#!/usr/bin/env python3
"""Check the bench before a donor run, so the plan is not broken by a detail.

`tools/verify_run_log.py` scores a run afterwards. This scores the bench
beforehand, on the machine the image will be built and booted from: the reviewed
ESP profile, the log medium, and the image itself. It checks only what a script
can check - everything else is printed from `docs/hardware_session.json` as a
`MANUAL` reminder, because those are the gates that get skipped.

    python3 tools/session_preflight.py --stick /media/stick --image out/MSI-....EFI
    python3 tools/session_preflight.py --stick E:\\ --profile local/boot_profile.h

Exit codes: 0 every check passed, 1 a check failed, 2 a named path does not exist.
`HARDWARE_VERIFIED=0` is printed either way: a clean bench says nothing about a
card.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "hardware_session.json"

# The placeholder committed in src/boot_profile.h. A waived build that compiles
# against this one is pinned to a partition that does not exist.
SYNTHETIC_GUID = "0x11111111"
LOG_PATTERNS = (
    re.compile(r"^RUN\d{4}\.TXT$", re.IGNORECASE),
    re.compile(r"^GPU-TOPOLOGY-DIAGNOSTIC-\d{4}\.TXT$", re.IGNORECASE),
)


def say(kind: str, text: str) -> None:
    print(f"{kind} {text}")


def check_profile(path: Path) -> tuple[bool, str]:
    if not path.is_file():
        return False, (f"PROFILE path={path} PRESENT=0 "
                       "ACTION=copy src/boot_profile.example.h and put the reviewed ESP GUID in it")
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"#define\s+GU_WINDOWS_PARTITION_INITIALIZER\s+\{([^}]*)\}", text)
    if not match:
        return False, f"PROFILE path={path} PRESENT=1 GUID=UNDEFINED"
    if SYNTHETIC_GUID in match.group(1):
        return False, (f"PROFILE path={path} PRESENT=1 GUID=SYNTHETIC "
                       "ACTION=a waived build cannot use the placeholder GUID")
    return True, f"PROFILE path={path} PRESENT=1 GUID=REVIEWED"


def check_stick(path: Path) -> tuple[bool, list[str]]:
    if not path.is_dir():
        return False, [f"STICK path={path} PRESENT=0"]
    stale: list[str] = []
    for entry in sorted(path.iterdir()):
        if any(pattern.match(entry.name) for pattern in LOG_PATTERNS):
            stale.append(entry.name)
    install_dir = path / "AD102-UNLOCK"
    for entry in sorted(install_dir.iterdir()) if install_dir.is_dir() else []:
        if any(pattern.match(entry.name) for pattern in LOG_PATTERNS):
            stale.append(f"AD102-UNLOCK/{entry.name}")
    notes = [f"STICK path={path} PRESENT=1 LOG_DIR={1 if install_dir.is_dir() else 0}"]
    if stale:
        notes.append(f"STICK STALE_LOGS={len(stale)} FILES={','.join(stale)} "
                     "ACTION=wipe the medium: a stale RUN0001 is indistinguishable "
                     "from a live one and the gates would score the wrong run")
        return False, notes
    notes.append("STICK FREE_OF_OLD_LOGS=1")
    return True, notes


def check_image(path: Path) -> tuple[bool, list[str]]:
    if not path.is_file():
        return False, [f"IMAGE path={path} PRESENT=0"]
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    notes = [f"IMAGE path={path} BYTES={len(data)} SHA256={digest}"]
    if not data:
        notes.append("IMAGE EMPTY=1 ACTION=build the image; a zero-byte file is not a run")
        return False, notes
    if data[:2] != b"MZ":
        notes.append("IMAGE PE_HEADER=0 ACTION=check you are pointing at the .EFI the builder wrote")
        return False, notes
    return True, notes


def manual_lines(manifest: dict) -> list[str]:
    return [f"MANUAL {item['id']} CHECKED_BY=OPERATOR RECORD_TO_RUN_LOG={item['record']!r}"
            for item in manifest.get("manual_gates", [])]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stick", type=Path, help="the prepared USB stick, as this machine sees it")
    parser.add_argument("--profile", type=Path, default=ROOT / "local" / "boot_profile.h",
                        help="the reviewed ESP profile (default: local/boot_profile.h)")
    parser.add_argument("--image", type=Path, help="the .EFI that will be booted")
    parser.add_argument("--skip-profile", action="store_true",
                        help="read-only stages do not need a reviewed profile")
    args = parser.parse_args(argv)

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    failures: list[str] = []
    missing: list[str] = []
    checked = 0

    if not args.stick and not args.image and args.skip_profile:
        parser.error("nothing to check: pass --stick, --image or drop --skip-profile")

    if args.stick is not None:
        ok, notes = check_stick(args.stick)
        checked += 1
        for note in notes:
            say("PREFLIGHT", note)
        if not ok:
            failures.append("STICK")
            if not args.stick.is_dir():
                missing.append(str(args.stick))
    if args.image is not None:
        ok, notes = check_image(args.image)
        checked += 1
        for note in notes:
            say("PREFLIGHT", note)
        if not ok:
            failures.append("IMAGE")
            if not args.image.is_file():
                missing.append(str(args.image))
    if not args.skip_profile:
        ok, note = check_profile(args.profile)
        checked += 1
        say("PREFLIGHT", note)
        if not ok:
            failures.append("PROFILE")

    for line in manual_lines(manifest):
        print(line)
    say("PREFLIGHT", f"NOT_PROVEN WINDOWS_61742_SIGNED_BOOTER SUPPLIED_BY=EXTERNAL_EVIDENCE_ONLY")

    verdict = "PASS" if not failures else "FAIL"
    failed = f" FAILED={','.join(failures)}" if failures else ""
    say("PREFLIGHT", f"PREFLIGHT_VERDICT={verdict}{failed} CHECKS={checked} HARDWARE_VERIFIED=0")
    if missing:
        print(f"PREFLIGHT_ERROR=NO_SUCH_PATH paths={','.join(missing)}", file=sys.stderr)
        return 2
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
