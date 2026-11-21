#!/usr/bin/env python3
"""Score a captured hardware log against the donor-hardware session gates.

This is the gate between "a run happened" and "a run may be recorded as evidence".
It reads the numbered log files a boot wrote, evaluates the gates in
`docs/hardware_session.json`, and prints one line per gate. It never prints a
verdict stronger than the manifest allows: a log can prove that the image did
what it says it did, and nothing in a log can prove that a card is safe, that
Windows started it, or that the 617.42 Booter accepted anything.

The gate tokens are *format strings* from the source, converted to patterns, so a
gate cannot be satisfied by text that no build of this tree prints.

    python3 tools/verify_run_log.py RUN0001.TXT [RUN0002.TXT ...] --stage prepare
    python3 tools/verify_run_log.py --stage readonly /media/stick/*.TXT
    python3 tools/verify_run_log.py --list
    python3 tools/verify_run_log.py tests/fixtures/hardware_session/*.txt --self-test

Exit codes: 0 every gate in the requested stage passed, 1 at least one gate
failed, 2 usage or a missing file, 3 the log set is synthetic or unreadable as a
hardware record, 4 the log set cannot answer the requested stage (too few logs).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "hardware_session.json"

STAGE_ORDER = ("readonly", "prepare", "admission", "actuate")
SYNTHETIC_MARKER = "SYNTHETIC_FIXTURE_NOT_A_HARDWARE_RUN"

# printf conversion -> pattern fragment. Deliberately permissive on width and
# tightening only where the conversion says something (0x prefix, hex digits).
CONVERSION = {
    "d": r"[-+]?\d+", "i": r"[-+]?\d+", "u": r"\d+",
    "o": r"[0-7]+",
    "x": r"[0-9A-Fa-f]+", "X": r"[0-9A-Fa-f]+",
    "p": r"0x[0-9A-Fa-f]+",
    "s": r".*?", "c": r".",
    "f": r"[-+]?\d+\.\d+", "e": r"[-+]?\d+\.\d+e[-+]\d+",
    "g": r"[-+]?\d+(?:\.\d+)?",
}
SPEC = re.compile(r"%([#0 +\-]*)(\d*)(?:\.(\d+))?(?:hh|h|ll|l|z|j|t|L)?([diouxXeEfgGaAcsp%])")


def token_to_pattern(token: str) -> re.Pattern[str]:
    """Turn a printf format string into a pattern that matches the line it writes."""
    out: list[str] = []
    at = 0
    for match in SPEC.finditer(token):
        out.append(re.escape(token[at:match.start()]))
        flags, _width, _prec, conv = match.groups()
        if conv == "%":
            out.append("%")
        elif conv in "xX" and "#" in flags:
            out.append(r"0x[0-9A-Fa-f]+")
        elif conv in CONVERSION:
            out.append(CONVERSION[conv])
        else:  # unknown conversion: do not pretend to know its shape
            out.append(r".*?")
        at = match.end()
    out.append(re.escape(token[at:]))
    return re.compile("".join(out))


def literal_weight(token: str) -> int:
    """How much of the token is literal text, used to reject all-wildcard tokens."""
    stripped = SPEC.sub("", token)
    return len(stripped.strip())


def load_manifest(path: Path = MANIFEST) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_logs(paths: list[Path]) -> list[tuple[str, str]]:
    logs = []
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        logs.append((path.name, text.replace("\r\n", "\n").replace("\r", "\n")))
    return logs


def occurrences(text: str, token: str) -> int:
    pattern = token_to_pattern(token)
    return sum(1 for line in text.splitlines() if pattern.search(line))


def check_group(group: dict, logs: list[tuple[str, str]]) -> tuple[bool, str]:
    token = group["token"]
    counts = [(name, occurrences(text, token)) for name, text in logs]
    hits = sum(1 for _name, count in counts if count)
    if not hits:
        return False, f"MISSING token={token!r}"
    limit = group.get("max_per_log")
    if limit is not None:
        for name, count in counts:
            if count > limit:
                return False, f"TOO_OFTEN token={token!r} in {name}: {count} > {limit}"
    # A floor on the total across the set: one line proves the loop ran, a
    # floor of forty-two proves it ran over the whole table.
    floor = group.get("min_total")
    if floor is not None:
        total = sum(count for _name, count in counts)
        if total < floor:
            return False, f"TOO_FEW token={token!r}: {total} < {floor}"
    return True, "OK"


def evaluate(manifest: dict, gates: list[dict], logs: list[tuple[str, str]], stage: str) -> list[dict]:
    wanted = STAGE_ORDER.index(stage)
    results = []
    for gate in gates:
        gate_stage = STAGE_ORDER.index(gate["stage"])
        if gate_stage > wanted:
            results.append({"id": gate["id"], "stage": gate["stage"], "result": "OUT_OF_SCOPE",
                            "detail": f"later stage {gate['stage']}"})
            continue
        if gate_stage < wanted:
            # Each stage is its own boot and its own log set; a string of
            # earlier gates cannot be satisfied by a later run's log.
            results.append({"id": gate["id"], "stage": gate["stage"], "result": "PRIOR_STAGE",
                            "detail": f"verify with --stage {gate['stage']}"})
            continue
        # A gate may describe one optional image rather than the stage: it
        # applies only when that image printed its own marker line. Absence is
        # reported as NOT_APPLICABLE, never as PASS, so a log set that never ran
        # the image cannot look like it satisfied the gate.
        marker = gate.get("only_if")
        if marker and not any(occurrences(text, marker["token"]) for _name, text in logs):
            results.append({"id": gate["id"], "stage": gate["stage"],
                            "result": "NOT_APPLICABLE",
                            "detail": f"image not run in this log set: {marker['token']!r}"})
            continue
        problems: list[str] = []
        if gate.get("min_logs", 1) > len(logs):
            problems.append(f"TOO_FEW_LOGS need={gate['min_logs']} have={len(logs)}")
        for group in gate.get("requires", []):
            ok, detail = check_group(group, logs)
            if not ok:
                problems.append(detail)
        if "requires_any" in gate:
            tries = [check_group(group, logs) for group in gate["requires_any"]]
            if not any(ok for ok, _detail in tries):
                problems.append("MISSING any of " + " | ".join(detail for _ok, detail in tries))
        for group in gate.get("forbids", []):
            count = sum(occurrences(text, group["token"]) for _name, text in logs)
            if count:
                problems.append(f"FORBIDDEN token={group['token']!r} seen {count}x")
        results.append({
            "id": gate["id"], "stage": gate["stage"],
            "result": "FAIL" if problems else "PASS",
            "detail": "; ".join(problems) if problems else "OK",
        })
    return results


def advisories(manifest: dict, logs: list[tuple[str, str]]) -> list[dict]:
    notes = []
    for item in manifest.get("advisories", []):
        tokens = [item["token"]] + list(item.get("alternatives", []))
        seen = sum(occurrences(text, token) for token in tokens for _name, text in logs)
        notes.append({"id": item["id"], "seen": bool(seen), "why": item["why"]})
    return notes


def render(manifest: dict, logs: list[tuple[str, str]], stage: str, results: list[dict],
           notes: list[dict], synthetic: bool) -> list[str]:
    lines = [f"HARDWARE_SESSION stage={stage} logs={len(logs)} "
             f"files={','.join(name for name, _text in logs)}"]
    for result in results:
        detail = result["detail"] if result["result"] != "PASS" else ""
        suffix = f" {detail}" if detail else ""
        lines.append(f"GATE {result['id']} STAGE={result['stage']} "
                     f"RESULT={result['result']}{suffix}")
    for note in notes:
        state = "PRESENT" if note["seen"] else "ABSENT"
        if note["seen"]:
            lines.append(f"NOTE {note['id']} {state}")
        else:
            lines.append(f"NOTE {note['id']} {state} ({note['why']})")
    for item in manifest.get("manual_gates", []):
        lines.append(f"MANUAL {item['id']} CHECKED_BY=OPERATOR RECORD_TO_RUN_LOG={item['record']!r}")
    for item in manifest.get("never_established_by_a_log", []):
        lines.append(f"NOT_PROVEN {item['id']}")
    failed = [r["id"] for r in results if r["result"] == "FAIL"]
    if synthetic:
        verdict = "PASS_SELF_TEST" if not failed else "FAIL_SELF_TEST"
    else:
        verdict = "PASS" if not failed else "FAIL"
    first = f" FIRST_FAILED={failed[0]}" if failed else ""
    lines.append(f"RUN_LOG_VERDICT={verdict}{first} "
                 f"SYNTHETIC={1 if synthetic else 0} HARDWARE_VERIFIED=0 "
                 f"GATES={sum(1 for r in results if r['result'] in ('PASS', 'FAIL'))} "
                 f"GATES_FAILED={len(failed)}")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="*", type=Path, help="captured log files, in boot order")
    parser.add_argument("--stage", default="readonly", choices=STAGE_ORDER)
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--self-test", action="store_true",
                        help=f"accept logs carrying {SYNTHETIC_MARKER}; never claims hardware evidence")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--list", action="store_true", help="print the gates and exit")
    args = parser.parse_args(argv)

    manifest = load_manifest(args.manifest)

    if args.list:
        for stage in manifest["stages"]:
            print(f"STAGE {stage['id']}: {stage['title']}")
        for gate in manifest["gates"]:
            print(f"GATE {gate['id']} STAGE={gate['stage']} {gate['title']}")
        for item in manifest.get("manual_gates", []):
            print(f"MANUAL {item['id']} {item['title']}")
        return 0

    if not args.logs:
        parser.error("at least one captured log file is required")

    missing = [str(path) for path in args.logs if not path.is_file()]
    if missing:
        print(f"VERIFY_ERROR=MISSING_LOG files={','.join(missing)}", file=sys.stderr)
        return 2

    logs = read_logs(args.logs)
    synthetic = any(SYNTHETIC_MARKER in text for _name, text in logs)
    if synthetic and not args.self_test:
        print(f"RUN_LOG_VERDICT=REFUSED REASON=SYNTHETIC_FIXTURE HARDWARE_VERIFIED=0 "
              f"NOTE=pass --self-test to check the gates themselves")
        return 3

    results = evaluate(manifest, manifest["gates"], logs, args.stage)
    notes = advisories(manifest, logs)
    lines = render(manifest, logs, args.stage, results, notes, synthetic)
    if args.json:
        print(json.dumps({"lines": lines, "results": results, "notes": notes,
                          "stage": args.stage, "synthetic": synthetic,
                          "hardware_verified": False}, indent=2))
    else:
        print("\n".join(lines))

    if any(r["result"] == "FAIL" for r in results):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
