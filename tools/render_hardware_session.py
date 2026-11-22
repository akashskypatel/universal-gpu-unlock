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
document turns the session into gates rather than an experiment with hope. It is
checked against the code and captured logs. The operator-reported RUN0031 record
below documents a successful physical RTX 4090 mutation separately from the
formal gate score; it does not erase the remaining contract and provenance gaps.
`HARDWARE_VERIFIED=0` remains the verifier's status, not a denial of the user's
hardware result.

`docs/hardware_session.json` is the source of truth; this file is rendered from
it. `tools/verify_run_log.py` scores a captured log against the same manifest,
so the plan and the verdict cannot disagree.

## How a run is scored

```bash
# read-only contact: the diagnostic's own log
python3 tools/verify_run_log.py /media/stick/GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT --stage readonly

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

"""

FOOTER = """
## What every one of these checks refuses to claim

The last section of the manifest is printed by the verifier as `NOT_PROVEN` on
every run, pass or fail. These are the items the automated session verifier
still does not establish. Operator-supplied evidence is documented above but
does not silently set contract fields or turn a failed formal gate into a pass.

The Windows 617.42 Booter comparison remains external evidence: no code here
observes it, and a donor run does not produce it. The successful 4090 mutation
and attached CUDA comparison are real hardware evidence; they do not establish
the exact Booter provenance, cryptographic firmware authentication, or the
unverified Stop-contract requirements.
"""


def render_readonly_guide(spec: dict) -> str:
    """The build-sign-stage -> verify-run-log loop, one candidate per row.

    Every command here is one this repository ships and every quoted line is a
    format string the named file prints; `tests/test_hardware_session.py` grounds
    both, so the guide cannot describe a command or a log line that does not
    exist.
    """
    by_id = {candidate["id"]: candidate for candidate in spec["candidates"]}
    out = [f"## {spec['title']}\n\n"]
    for paragraph in spec["intro"]:
        out.append(f"{paragraph}\n\n")
    out.append(f"**The name.** {spec['naming']}\n\n")

    out.append("### Before the first boot\n\n")
    for item in spec["preconditions"]:
        out.append(f"* `{item['id']}` — {item['why']}\n")
    out.append("\n")

    out.append("### The steps\n\n")
    for step in spec["steps"]:
        out.append(f"**Step {step['n']} — {step['title']}**\n\n")
        if step.get("command"):
            fence = "powershell" if step["shell"] == "powershell" else "bash"
            out.append(f"```{fence}\n{step['command']}\n```\n\n")
        out.append(f"*Expect:* {step['expect']}\n\n")
        if step.get("expect_tokens"):
            out.append("*Look for:*\n\n")
            for group in step["expect_tokens"]:
                out.append(f"* `{group['token']}` — printed by `{group['emitter']}`\n")
            out.append("\n")
        out.append(f"*If it does not:* {step['if_not']}\n\n")
    out.append(f"{spec['after_the_steps']}\n\n")

    out.append("### The read-only candidates\n\n")
    out.append("Every image in the catalogue whose safety class writes nothing to the GPU. One candidate per stick, "
               "one boot per candidate, one scored log per boot.\n\n")
    out.append("| Candidate | Safety | Stage | Log | Facts it puts on paper |\n")
    out.append("| --- | --- | --- | --- | --- |\n")
    for candidate in spec["candidates"]:
        goals = " ".join(f"`{goal}`" for goal in dict.fromkeys(
            entry["goal"] for entry in candidate["evidence"]))
        out.append(f"| `{candidate['id']}` | {candidate['safety']} | `{candidate['stage']}` | "
                   f"{candidate['log']} | {goals} |\n")
    out.append("\n")

    for candidate in spec["candidates"]:
        out.append(f"#### `{candidate['id']}` — {candidate['title']}\n\n")
        out.append(f"{candidate['summary']}\n\n")
        if candidate.get("commands"):
            # A stage may be reached by more than one build, and the difference
            # is the session (one card, or an ordered list): render each rather
            # than choosing one and hiding the other.
            for variant in candidate["commands"]:
                out.append(f"*{variant['label']}* — `{variant['binary']}`\n\n")
                out.append(f"```powershell\n{variant['command']}\n```\n\n")
        else:
            out.append(f"```powershell\n{candidate['command']}\n```\n\n")
        out.append(f"*Log:* {candidate['log']}\n\n")
        out.append(f"*Score:* `python3 tools/verify_run_log.py <log> --stage {candidate['stage']}`\n\n")
        if candidate.get("also_collects"):
            out.append(f"*Also collects* everything `{candidate['also_collects']}` collects, plus:\n\n")
        for entry in candidate["evidence"]:
            out.append(f"* `{entry['goal']}` — `{entry['token']}` (`{entry['emitter']}`): {entry['why']}\n")
        out.append(f"\n*Does not prove:* {candidate['does_not_prove']}\n\n")
        del by_id[candidate["id"]]  # keeps the mapping honest if a candidate is duplicated

    out.append("### Candidates that produce no scoreable log, and why\n\n")
    for item in spec["unscoreable"]:
        out.append(f"* `{item['id']}` — {item['why']}\n")
    out.append("\n")

    out.append("### What the read-only stage cannot advance\n\n")
    out.append("The ceiling of the loop above, item by item. This is the table to read before promising that a clean "
               "session means anything beyond what it says.\n\n")
    out.append("| Item | After every candidate above has been scored | Why it does not go further |\n")
    out.append("| --- | --- | --- |\n")
    for item in spec["ceiling"]:
        out.append(f"| `{item['goal']}` | {item['after']} | {item['why_not']} |\n")
    out.append("\n")
    out.append(f"{spec['closing']}\n\n")
    return "".join(out)


def render_captured_runs(spec: dict) -> str:
    """Physical runs, as files, with the verdict the verifier prints for them.

    The suite re-scores every file listed here, so this section cannot describe a
    run the tools disagree with, and a gate that no healthy machine can satisfy
    fails the test suite instead of the operator.
    """
    out = [f"## {spec['title']}\n\n", f"{spec['note']}\n\n"]
    out.append("| Run | Image | Stage | Lines | SHA-256 of the stored log | Verdict |\n")
    out.append("| --- | --- | --- | --- | --- | --- |\n")
    for run in spec["runs"]:
        out.append(f"| `{run['id']}` | `{run['image']}` | `{run['stage']}` | {run['lines']} | "
                   f"`{run['sha256']}` | `{run['verdict']}` |\n")
    out.append("\n")
    for run in spec["runs"]:
        out.append(f"### `{run['id']}`\n\n")
        out.append(f"*File:* `{run['file']}` — {run['bytes']} bytes stored, {run['captured_bytes']} as captured, "
                   f"{run['lines']} lines. {run['line_endings']}.\n\n")
        out.append(f"*Captured:* {run['captured']}. *Bench:* {run['bench']}.\n\n")
        out.append(f"*Score:*\n\n```bash\npython3 tools/verify_run_log.py {run['file']} --stage {run['stage']}\n```\n\n")
        out.append(f"It prints `{run['verdict']}`.\n\n")
        out.append("*What this log puts on paper:*\n\n")
        for item in run["observations"]:
            out.append(f"* `{item['token']}` — {item['why']}\n")
        out.append("\n")
        out.append(f"*History:* {run['history']}\n\n")
        out.append(f"*What it does not show:* {run['does_not_show']}\n\n")
    return "".join(out)


def render_operator_reported_results(spec: dict) -> str:
    """Operator-attested physical outcomes, kept outside automated gate scores."""
    out = [f"## {spec['title']}\n\n", f"{spec['note']}\n\n"]
    for result in spec["results"]:
        out.append(f"### {result['id']} — {result['title']}\n\n")
        out.append(f"{result['summary']}\n\n")
        out.append("**Evidence files (stored UTF-8/LF normalized):**\n\n")
        out.append("| File | Captured format | Captured bytes | Stored bytes | Lines | SHA-256 |\n")
        out.append("| --- | --- | ---: | ---: | ---: | --- |\n")
        for record in result["files"]:
            out.append(f"| `{record['file']}` | {record['capture_format']} | {record['captured_bytes']} | {record['bytes']} | "
                       f"{record['lines']} | `{record['sha256']}` |\n")
        out.append("\n**What RUN0031 records:**\n\n")
        for item in result["observations"]:
            out.append(f"* `{item['token']}` — {item['why']}\n")
        out.append("\n**Bench comparison:**\n\n")
        for item in result["benchmarks"]:
            out.append(f"* {item}\n")
        out.append("\n**Keep these distinctions:**\n\n")
        for item in result["limitations"]:
            out.append(f"* {item}\n")
        out.append("\n")
    return "".join(out)


def render_go_no_go(spec: dict) -> str:
    out = ["## Advancing between stages: `donor_gate.py`\n\n", f"{spec['summary']}\n\n"]
    out.append("```powershell\n" + spec["command"] + "\n```\n\n")
    out.append("It refuses to advance unless:\n\n")
    for rule in spec["rules"]:
        out.append(f"* {rule}\n")
    out.append(f"\nIt prints {spec['verdict']}.\n\n")
    out.append("Exit codes: " + ", ".join(f"`{code}`" for code in spec["exit_codes"]) + ".\n\n")
    out.append(f"What a GO does not prove: {spec['does_not_prove']}.\n\n")
    return "".join(out)


def render(manifest: dict) -> str:
    out = [HEADER]
    if "readonly_guide" in manifest:
        out.append(render_readonly_guide(manifest["readonly_guide"]))
    if "captured_runs" in manifest:
        out.append(render_captured_runs(manifest["captured_runs"]))
    if "stopped_runs" in manifest:
        out.append(render_captured_runs(manifest["stopped_runs"]))
    if "operator_reported_results" in manifest:
        out.append(render_operator_reported_results(manifest["operator_reported_results"]))
    if "go_no_go" in manifest:
        out.append(render_go_no_go(manifest["go_no_go"]))
    out.append("## The stages\n\n")
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

    out.append("## Not established by the formal session verifier\n\n")
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
