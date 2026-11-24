#!/usr/bin/env python3
"""Render docs/RECOVERY_MATRIX.md from docs/recovery_matrix.json.

The manifest is the source of truth so the prose cannot drift from what the
source actually logs: every symptom is a format string this repository prints,
every record field is a field of GU_BOOT_STATE, and every simulator row named in
the manifest is one the runner executes. `tests/test_recovery_matrix.py` asserts
the rendered file is byte-identical to this function's output.

    python3 tools/render_recovery_matrix.py          # print
    python3 tools/render_recovery_matrix.py --write  # regenerate the document
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "recovery_matrix.json"
DOCUMENT = ROOT / "docs" / "RECOVERY_MATRIX.md"

HEADER = """# Recovery matrix

What to do when a boot did not end the way it should, and what proves recovery.
**Manual cold-power recovery: HARDWARE AND FIRMWARE VERIFIED (owner report).**
The owner confirms recovery from a failed firmware transaction by a
60-second cold boot with USB removed (60 seconds powered off before boot).
Manual recovery is verified on hardware by owner report. Do not repeat
the test. This verifies the observed manual recovery, not every automated
recovery branch or failure class. The source/host contracts for those branches
remain separate. Production eligibility gates still read
`CAN_DETACH=false`, `CAN_UNLOCK=false`, `CAN_FLASH=false`, `RECOVERY_VERIFIED=false`.

Every class below is grounded in something this repository prints or stores:

* the **symptom** is a log line the source actually emits (the token is the
  format string, so it is verifiable by grep);
* the **record** is what survives in NVRAM (`GuBootState`, the A7 record) - the
  only state that outlives a failed boot;
* the **simulator row** is the scenario that exercises the software half, when one
  exists. It shows the image behaves as described; it says nothing about silicon.

`docs/recovery_matrix.json` is the source of truth and this file is rendered from
it by `tools/render_recovery_matrix.py`; `tests/test_recovery_matrix.py` fails if
they differ, if a symptom line no longer exists, or if any class starts claiming a
verified recovery.

Recovery is per card and per class. There is no rollback or re-flash path. A
failed/interrupted boot record now blocks another DXE transaction and selects a
no-GPU recovery path; the code does not attempt to repair GPU firmware state.

---

"""


def render(manifest: dict) -> str:
    out = [HEADER]
    for entry in manifest["classes"]:
        out.append(f"### `{entry['id']}`\n\n")
        out.append(f"**{entry['title']}**\n\n")
        out.append("| | |\n| --- | --- |\n")
        symptoms = "<br>".join(
            f"`{s['token']}` ({s['file']})" for s in entry["symptoms"])
        out.append(f"| Symptom | {symptoms} |\n")
        out.append(f"| Record | {', '.join('`' + f + '`' for f in entry['record'])} |\n")
        out.append(f"| Record state | {entry['record_state']} |\n")
        out.append(f"| Card state | {entry['card_state']} |\n")
        out.append(f"| Machine state | {entry['machine_state']} |\n")
        out.append(f"| Simulator | {('`' + entry['sim_scenario'] + '`') if entry['sim_scenario'] else 'none'} |\n\n")
        out.append("Steps:\n\n")
        for index, step in enumerate(entry["steps"], 1):
            out.append(f"{index}. {step}\n")
        out.append("\nRecovered when:\n\n")
        for proof in entry["proof"]:
            out.append(f"* {proof}\n")
        out.append(f"\nNot verified: {entry['why_unverified']}\n\n---\n\n")
    return "".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    text = render(json.loads(MANIFEST.read_text(encoding="utf-8")))
    if args.write:
        DOCUMENT.write_text(text, encoding="utf-8")
        print(f"wrote {DOCUMENT.relative_to(ROOT)}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
