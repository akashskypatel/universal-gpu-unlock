#!/usr/bin/env python3
"""Render docs/DONOR_HARDWARE_SESSION.md from docs/hardware_session.json.

The manifest is the source of truth so the session plan cannot drift from the
checks that score it: every gate token is a format string this repository prints,
`tools/verify_run_log.py` evaluates the gates against a captured log, and
`tests/test_hardware_session.py` asserts this file is byte-identical to the
rendered output.

    python3 tools/render_hardware_session.py          # print
    python3 tools/render_hardware_session.py --write  # regenerate the document
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "hardware_session.json"
DOCUMENT = ROOT / "docs" / "DONOR_HARDWARE_SESSION.md"

HEADER = """# Donor-hardware session: the gates a run has to pass

The donor card, the programmer and the known-good image are on the bench. This
document is what turns that into a session with gates rather than an experiment
with hope. It is checked against the code and against a captured log, and it
**claims nothing**: no run described here has happened, `HARDWARE_VERIFIED=0` is
printed by every check, and the list at the end is everything a log can never
establish.

`docs/hardware_session.json` is the source of truth; this file is rendered from
it. `tools/verify_run_log.py` scores a captured log against the same manifest,
so the plan and the verdict cannot disagree.

## How a run is scored

```bash
# read-only contact: the diagnostic's own log
python3 tools/verify_run_log.py /media/stick/GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT --stage readonly

# preparation only: the app's numbered log
python3 tools/verify_run_log.py /media/stick/AD102-UNLOCK/RUN0001.TXT --stage prepare

# waived actuation: the run's log and the next boot's, in that order
python3 tools/verify_run_log.py /media/stick/AD102-UNLOCK/RUN0001.TXT \\
                                /media/stick/AD102-UNLOCK/RUN0002.TXT --stage actuate
```

Each stage is its own boot with its own log, so only the requested stage's gates
are evaluated: earlier stages are reported as `PRIOR_STAGE`, never as passes.
A log carrying the fixture marker is refused (exit 3) unless `--self-test` is
given, and a self-test verdict can never read `PASS`.

Exit codes: `0` every gate in the stage passed, `1` a gate failed, `2` a listed
log does not exist, `3` the log set is synthetic, `4` reserved for too few logs
(reported inline as a failing gate today).

The images each stage needs are in [`image_variants.json`](image_variants.json):
`src/Build-Sign-Deploy-UniversalGPU.ps1 -Only <id>` builds and signs them, and
[`BUILD_SIGN_DEPLOY.md`](BUILD_SIGN_DEPLOY.md) is the user document for that
script. The session stages map to `diag-topology-log`, `prep-selected` and
`actuate-one-target`.

Before the boot, `tools/session_preflight.py` checks the bench the same way:
that the medium carries no earlier `RUNnnnn.TXT` (a stale one is
indistinguishable from a live one), that the ESP profile holds a reviewed GUID
and not the committed placeholder, and that the image is a non-empty PE with its
SHA256 recorded:

```bash
python3 tools/session_preflight.py --stick /media/stick --image out/<the built>.EFI
```

It prints the same `MANUAL` gates and the same `HARDWARE_VERIFIED=0`. A clean
bench means nothing is obviously wrong before the run; it says nothing about a
card.

## The stages

"""

FOOTER = """
## What every one of these checks refuses to claim

The last section of the manifest is printed by the verifier as `NOT_PROVEN` on
every run, pass or fail. It is the honest half of the document: these are the
things a log cannot show, and each one is why the readiness tracker still counts
hardware evidence as 0 of 43.

The 617.42 Booter comparison is the one to keep in view. It is external evidence
that has never existed in this repository, no code here observes it, and a donor
run does not produce it. A green session is a session that behaved as the plan
says; it is not a statement that the card is safe, that Windows started it, or
that the firmware accepted anything.
"""


def render(manifest: dict) -> str:
    out = [HEADER]
    for stage in manifest["stages"]:
        out.append(f"### `{stage['id']}` — {stage['title']}\n\n")
        out.append(f"*Build:*\n\n```powershell\n{stage['build']}\n```\n\n")
        out.append(f"Note: {stage['build_note']}.\n\n")
        out.append(f"*Costs:* {stage['what_it_costs']}\n\n")
        out.append(f"*Exit criteria:* {stage['exit']}\n\n")
        gates = [g for g in manifest["gates"] if g["stage"] == stage["id"]]
        out.append("| Gate | What it checks | Requires | Must not contain |\n")
        out.append("| --- | --- | --- | --- |\n")
        for gate in gates:
            needs = []
            for group in gate.get("requires", []):
                needs.append(f"`{group['token']}`")
            for group in gate.get("requires_any", []):
                needs.append(f"`{group['token']}` *(one of)*")
            if gate.get("min_logs"):
                needs.append(f"at least {gate['min_logs']} logs")
            forbids = [f"`{group['token']}`" for group in gate.get("forbids", [])]
            out.append(f"| `{gate['id']}` | {gate['title']} | {'<br>'.join(needs)} | "
                       f"{'<br>'.join(forbids) if forbids else '—'} |\n")
        out.append("\n")
        for gate in gates:
            out.append(f"**`{gate['id']}`** — proves: {gate['proves']}. "
                       f"Does not prove: {gate['does_not_prove']}.\n\n")
            for group in gate.get("requires", []) + gate.get("requires_any", []):
                limit = group.get("max_per_log")
                limit_note = f" At most {limit} per log." if limit else ""
                out.append(f"* `{group['token']}` ({group['emitter']}) — {group['why']}."
                           f"{limit_note}\n")
            for group in gate.get("forbids", []):
                out.append(f"* must not contain `{group['token']}` ({group['emitter']}): "
                           f"{group['why']}\n")
            out.append("\n")

    out.append("## Manual gates\n\n")
    out.append("These cannot be checked from a log, so the verifier prints them as "
               "`MANUAL` on every run: they are remembered or they are skipped.\n\n")
    out.append("| Gate | What it means | Why it is not optional | What to record |\n")
    out.append("| --- | --- | --- | --- |\n")
    for item in manifest["manual_gates"]:
        out.append(f"| `{item['id']}` | {item['title']} | {item['why']} | {item['record']} |\n")
    out.append("\n")

    out.append("## Advisories\n\n")
    out.append("Printed as `NOTE` lines. An absent advisory is a finding to explain, "
               "not a failure: these lines exist in some entrypoints and not others.\n\n")
    for item in manifest["advisories"]:
        alternates = ", ".join(f"`{token}`" for token in item.get("alternatives", []))
        also = f" (or {alternates})" if alternates else ""
        out.append(f"* `{item['id']}` — `{item['token']}`{also} ({item['emitter']}): {item['why']}\n")
    out.append("\n")

    out.append("## Never established by a log\n\n")
    for item in manifest["never_established_by_a_log"]:
        out.append(f"* `{item['id']}` — {item['why']}\n")
    out.append(FOOTER)
    return "".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true", help="regenerate the document")
    args = parser.parse_args(argv)
    text = render(json.loads(MANIFEST.read_text(encoding="utf-8")))
    if args.write:
        DOCUMENT.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {DOCUMENT.relative_to(ROOT)}")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
