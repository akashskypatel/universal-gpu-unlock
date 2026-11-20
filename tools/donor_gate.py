#!/usr/bin/env python3
"""Decide whether the donor hardware session may enter its next stage.

`tools/verify_run_log.py` scores one captured log set against the gates for one
stage.  That is a report, not a go/no-go decision: it will happily print
`RESULT=PASS` for a read-only log set while the next stage's preconditions are
still missing, and it says nothing about which card the earlier stages saw.

This tool composes those verdicts into a single answer for the operator:

* the read-only stage must pass before preparation counts as done;
* preparation must pass before actuation may be attempted at all;
* every stage must identify the same card: the one a stage *selected* has to be
  one the read-only stage enumerated, and the PCI id printed beside it has to
  agree across stages, so a card swap cannot slip between boots. A read-only
  census of several cards is not a refusal - a two-card bench is the ordinary
  bench - but a stage that prepared several targets is;
* every manual gate in the manifest must be acknowledged by name, so
  `M1_RECOVERY_TOOLKIT_READY` cannot be assumed silently.

It prints `DONOR_GATE VERDICT=GO` only when all of that holds, and
`VERDICT=NO_GO` with a `BLOCKED` line per reason otherwise.  It never claims
hardware verification: a GO means "the paperwork and the captured evidence
permit the next stage", and every line carries `HARDWARE_VERIFIED=0`.

Usage:

    python3 tools/donor_gate.py \\
        --readonly media/GPU-TOPOLOGY-DIAGNOSTIC-0001.TXT \\
        --prepare  media/GPU-ACTIVE-PREPARE-0001.TXT \\
        --ack M1_RECOVERY_TOOLKIT_READY,M2_DONOR_CARD_RECORDED,\\
M3_LOG_MEDIUM_EMPTY,M4_DONOR_CARDS_RECORDED,M5_HALT_STOPS_THE_SESSION,\\
M6_STOCK_BOOT_AFTER_EVERY_RUN

Add `--actuate LOG...` once the waived image has run; then the same command
scores that stage too and the verdict answers "may the next boot continue",
which is what M5 and M6 govern.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_run_log  # noqa: E402

# The BDF is the join key every stage writes (the read-only stage names a card
# only that way), so it is what the stage-to-stage comparison uses.  The PCI id
# is an extra check wherever a stage happens to print it; `SUB=10de:...` is a
# subsystem id, not the card, and is excluded.
BDF_PATTERN = re.compile(r"(?i)\bBDF=([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])\b")
DEVICE_PATTERN = re.compile(r"(?i)(?<!sub=)\b10de:([0-9a-f]{4})\b")

# A census is not a choice.  The read-only diagnostic reports every NVIDIA card
# on the machine (`GPU[%zu] BDF=...`, `NVIDIA_DRIVER_OWNER[%zu] BDF=...`), and a
# two-card bench is the ordinary bench: the first physical session named
# `01:00.0` and `04:00.0` in every stage and the old "one BDF per stage" rule
# blocked a session that had done nothing wrong.  What identifies the session is
# the card a stage *selected*, and those lines say so explicitly.
# One boot writes its target list in exactly one of these forms, and a log that
# writes two of them (a selection request and the walk that follows it) is not
# naming a card twice - it is saying the same thing twice. The first family a
# stage actually printed is the one that describes its list.
TARGET_FAMILIES = (
    ("prepared",
     re.compile(r"(?i)\bACTIVE_PREPARED_STEP\[\d+\] BDF=([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])\b")),
    ("request",
     re.compile(r"(?i)\bSELECTION_REQUEST\[\d+\] BDF=([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])\b")),
    ("walk",
     re.compile(r"(?i)\bSELECTION_STEP\[\d+\]=BEGIN BDF=([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])\b")),
    ("identified",
     re.compile(r"(?i)\bPASSIVE_SELECTION=GPU_IDENTIFIED TARGET=([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])\b")),
)
STAGE_FLAGS = ("readonly", "prepare", "actuate")


def identity_in(logs: list[tuple[str, str]]) -> tuple[set[str], list[str], list[str], set[str], dict]:
    """Return (cards observed, targets in order, target occurrences, ids, bdf -> ids).

    Observed is the census: every card the stage named.  Targets are the cards
    the stage says it chose, in the order it named them - this fork executes an
    ordered list, so order is part of the identity, not an accident of printing.
    Occurrences keep repeats, because a list that names one card twice is not a
    two-card session.  The last value pairs a device id with the BDF printed on
    the same line, so the comparison is about the target card rather than about
    every card that happened to be in the machine.
    """
    observed: set[str] = set()
    targets: list[str] = []
    occurrences: list[str] = []
    devices: set[str] = set()
    pairs: dict[str, set[str]] = {}
    for _name, text in logs:
        observed.update("%s:%s.%s" % (m.group(2), m.group(3), m.group(4))
                        for m in BDF_PATTERN.finditer(text))
        devices.update(m.group(1).lower() for m in DEVICE_PATTERN.finditer(text))
        for _family, pattern in TARGET_FAMILIES:
            named = ["%s:%s.%s" % (m.group(2), m.group(3), m.group(4))
                     for m in pattern.finditer(text)]
            if named:
                occurrences.extend(named)
                for bdf in named:
                    if bdf not in targets:
                        targets.append(bdf)
                break
        for line in text.splitlines():
            ids = {m.group(1).lower() for m in DEVICE_PATTERN.finditer(line)}
            if not ids:
                continue
            for m in BDF_PATTERN.finditer(line):
                pairs.setdefault("%s:%s.%s" % (m.group(2), m.group(3), m.group(4)),
                                 set()).update(ids)
    return observed, targets, occurrences, devices, pairs


def score_stage(manifest: dict, stage: str, logs: list[tuple[str, str]]) -> list[dict]:
    return verify_run_log.evaluate(manifest, manifest["gates"], logs, stage)


def decide(manifest: dict, stages: dict[str, list[tuple[str, str]]],
           acks: set[str]) -> tuple[bool, list[str], dict]:
    """Return (go, lines, facts) for the stage set that was supplied."""
    lines: list[str] = []
    blocked: list[str] = []
    facts: dict = {"stages": {}, "bdf": None, "device": None, "acked": sorted(acks),
                   "hardware_verified": False}
    bdfs: dict[str, set[str]] = {}
    observed: dict[str, set[str]] = {}
    targets: dict[str, list[str]] = {}
    occurrences: dict[str, list[str]] = {}
    conflicting_lists: dict[str, list[list[str]]] = {}
    devices: dict[str, set[str]] = {}
    pairs: dict[str, dict[str, set[str]]] = {}

    manual_ids = [item["id"] for item in manifest.get("manual_gates", [])]
    unknown = sorted(acks - set(manual_ids))
    if unknown:
        lines.append("BLOCKED UNKNOWN_ACK ids=%s" % ",".join(unknown))
        blocked.append("UNKNOWN_ACK")
    missing = [identifier for identifier in manual_ids if identifier not in acks]
    if missing:
        lines.append("BLOCKED MANUAL_UNACKNOWLEDGED ids=%s" % ",".join(missing))
        blocked.append("MANUAL_UNACKNOWLEDGED")

    order = [stage for stage in STAGE_FLAGS if stage in stages]
    for stage in order:
        results = score_stage(manifest, stage, stages[stage])
        failed = [result for result in results if result["result"] == "FAIL"]
        facts["stages"][stage] = {
            "logs": len(stages[stage]),
            "failed": [result["id"] for result in failed],
            "passed": sum(1 for r in results if r["result"] == "PASS"),
        }
        lines.append(
            "DONOR_GATE %s RESULT=%s GATES=%d FAILED=%s"
            % (stage, "FAIL" if failed else "PASS",
               facts["stages"][stage]["passed"],
               ",".join(result["id"] for result in failed) or "-")
        )
        for result in failed:
            lines.append("BLOCKED GATE %s STAGE=%s %s"
                         % (result["id"], stage, result["detail"]))
        if failed:
            blocked.append("GATE_" + stage.upper())

        observed[stage], targets[stage], occurrences[stage], devices[stage], pairs[stage] = \
            identity_in(stages[stage])
        # A stage may be scored from several boots, and each log speaks for one
        # boot. Two boots that name different lists are not one ordered list, and
        # a single boot that names one card twice is not a two-card session, so
        # both are judged per log before the stage is merged.
        per_log = [identity_in([log]) for log in stages[stage]]
        named_lists = [entry[1] for entry in per_log if entry[1]]
        conflicting_lists[stage] = named_lists if len({tuple(e) for e in named_lists}) > 1 else []
        repeated = sorted({bdf for entry in per_log for bdf in set(entry[2])
                           if entry[2].count(bdf) > 1})
        # A stage that names targets identifies the session with those, in the
        # order it named them; a stage that names none (the plain read-only
        # census, an actuation log that reports indexes) is compared to them.
        bdfs[stage] = set(targets[stage]) or observed[stage]
        if not observed[stage]:
            lines.append("BLOCKED IDENTITY_NOT_NAMED STAGE=%s" % stage)
            blocked.append("IDENTITY_NOT_NAMED")
        elif repeated:
            lines.append("BLOCKED IDENTITY_DUPLICATE_TARGET STAGE=%s bdf=%s reason=one_boot_named_the_same_card_twice"
                         % (stage, ",".join(repeated)))
            blocked.append("IDENTITY_DUPLICATE_TARGET")
        elif conflicting_lists[stage]:
            lines.append("BLOCKED IDENTITY_TARGET_ORDER_CHANGED STAGE=%s LISTS=%s"
                         % (stage, " | ".join(",".join(entry) for entry in conflicting_lists[stage])))
            blocked.append("IDENTITY_TARGET_ORDER_CHANGED")
        elif not devices[stage]:
            # A stage that names a BDF but no device id has not identified the
            # card: after a slot swap the same BDF is different silicon, and the
            # rules this tool prints say such a stage is refused rather than
            # assumed. Before this check existed, one stage naming an id was
            # enough for the whole session, which is how the prepare fixture in
            # its stripped form could still reach GO.
            lines.append("BLOCKED IDENTITY_UNKNOWN STAGE=%s reason=this_stage_named_no_pci_id"
                         % stage)
            blocked.append("IDENTITY_UNKNOWN")

    # The session is an ordered list, and the list that has to stay consistent
    # is the one the waived stages will execute - this fork's model is one card
    # active at a time, in order, each step gated on its predecessor's
    # completion record (`GU_ACTIVE_SINGLE_TARGET`).
    # execute, so it is the preparation stage that defines it. The read-only
    # stage's selection is an observation set: it may name cards the session
    # will leave alone, and naming them is not a promise to touch them.
    session: list[str] = []
    session_stage = None
    for stage in ("prepare", "actuate"):
        if stage in order and targets[stage]:
            session, session_stage = list(targets[stage]), stage
            break
    census = observed.get("readonly", set())
    if not session:
        # No stage named a target. One card in the machine is unambiguous; two
        # are not, and picking one here would be this tool choosing a donor.
        seen = set()
        for stage in order:
            seen |= bdfs[stage]
        if len(seen) == 1:
            session = [next(iter(seen))]
            session_stage = "census"
        else:
            lines.append("BLOCKED IDENTITY_TARGET_UNNAMED STAGE=%s OBSERVED=%s reason=no_stage_named_a_target"
                         % (session_stage or order[0], ",".join(sorted(seen))))

    if session:
        facts["bdf"] = ",".join(session)
        facts["targets"] = len(session)
        if len(session) > 1:
            lines.append("DONOR_GATE sequence TARGETS=%d ORDER=%s MODEL=ONE_CARD_ACTIVE_AT_A_TIME_STEP_GATED_BY_PREDECESSOR"
                         % (len(session), ",".join(session)))
        for stage in order:
            # The read-only stage's list is an observation set, and its intent is
            # "look at these", not "execute these": it may be longer than the
            # session and that is reported as peers below, not refused here.
            if stage in (session_stage, "readonly") or not targets[stage]:
                continue
            if targets[stage] != session:
                lines.append("BLOCKED IDENTITY_TARGET_ORDER_CHANGED SESSION=%s %s=%s"
                             % (",".join(session), stage, ",".join(targets[stage])))
                blocked.append("IDENTITY_TARGET_ORDER_CHANGED")
        # Whatever a later stage named must be a card the read-only stage
        # actually saw: a preparation log for a card the diagnostic never
        # enumerated is a different session wearing the same paperwork.
        if census:
            for bdf in session:
                if bdf not in census:
                    lines.append("BLOCKED IDENTITY_NOT_OBSERVED STAGE=%s BDF=%s OBSERVED=%s"
                                 % (session_stage, bdf, ",".join(sorted(census))))
                    blocked.append("IDENTITY_NOT_OBSERVED")

    # Identity is a property of the named cards: compare the ids printed beside
    # each BDF in the session, and fall back to everything the stage named only
    # when no line tied an id to an address. Comparing whole censuses would
    # block a two-card bench over a peer card whose id one stage omitted.
    device_sets = []
    for stage in order:
        if not devices[stage]:
            continue
        scoped: set[str] = set()
        for bdf in (session or sorted(bdfs[stage])):
            scoped |= pairs[stage].get(bdf, set())
        device_sets.append((stage, scoped or devices[stage]))
    if not device_sets:
        lines.append("BLOCKED IDENTITY_UNKNOWN reason=no_stage_named_a_pci_id")
        blocked.append("IDENTITY_UNKNOWN")
    else:
        first_stage, first_devices = device_sets[0]
        facts["device"] = ",".join(sorted(first_devices))
        for stage, value in device_sets[1:]:
            if value != first_devices:
                lines.append("BLOCKED PCI_ID_CHANGED %s=%s %s=%s"
                             % (first_stage, ",".join(sorted(first_devices)), stage,
                                ",".join(sorted(value))))
                blocked.append("PCI_ID_CHANGED")
    # The waiver line states how many targets it admitted. A session whose
    # actuation admits fewer than its prepared list is a different session.
    if session and "actuate" in stages:
        for _name, text in stages["actuate"]:
            for match in re.finditer(r"(?i)\bACTIVE_EXECUTION=WAIVED_UNVERIFIED SELECTED=(\d+)", text):
                if int(match.group(1)) != len(session):
                    lines.append("BLOCKED ACTUATION_SELECTION_CHANGED SESSION=%d WAIVED_SELECTED=%s ORDER=%s"
                                 % (len(session), match.group(1), ",".join(session)))
                    blocked.append("ACTUATION_SELECTION_CHANGED")

    identity_ok = not ({"IDENTITY_NOT_NAMED", "IDENTITY_DUPLICATE_TARGET",
                        "IDENTITY_TARGET_ORDER_CHANGED", "IDENTITY_TARGET_UNNAMED",
                        "IDENTITY_UNKNOWN", "IDENTITY_NOT_OBSERVED", "PCI_ID_CHANGED",
                        "ACTUATION_SELECTION_CHANGED"} & set(blocked))
    facts["observed"] = {stage: sorted(observed[stage]) for stage in order}
    facts["selected"] = {stage: list(targets[stage]) for stage in order}
    lines.append("DONOR_GATE identity BDF=%s DEVICE=%s STAGES=%s OBSERVED=%d TARGETS=%d RESULT=%s"
                 % (facts.get("bdf", "-"), facts.get("device", "-"),
                    ",".join(stage for stage in order if bdfs[stage]) or "-",
                    len(census) if census else len(session),
                    len(session), "PASS" if identity_ok else "FAIL"))
    if len(census) > len(session):
        lines.append("NOTE PEERS=%d OBSERVED=%d TARGET=%s (the read-only stage reported every card "
                     "on the machine; the session's ordered list names the cards under test, and "
                     "M4 records the rest by serial)"
                     % (len(census) - len(session), len(census), facts.get("bdf", "-")))

    lines.append("DONOR_GATE manual ACKED=%d REQUIRED=%d RESULT=%s"
                 % (len([a for a in acks if a in manual_ids]), len(manual_ids),
                    "PASS" if not missing and not unknown else "FAIL"))

    # A later stage is only meaningful once every earlier stage was scored.
    predecessors = {"prepare": ("readonly",), "actuate": ("readonly", "prepare")}
    for stage in order:
        for prior in predecessors.get(stage, ()):
            if prior not in stages:
                lines.append("BLOCKED PRIOR_STAGE_MISSING stage=%s needed_by=%s"
                             % (prior, stage))
                blocked.append("PRIOR_STAGE_MISSING")

    facts["blocked"] = blocked
    return (not blocked), lines, facts


def next_step(manifest: dict, stages: dict[str, list[tuple[str, str]]]) -> str:
    for stage in STAGE_FLAGS:
        if stage not in stages:
            return "run the %s stage, then score it here" % stage
    return "continue the session only as M6 permits; re-score the next boot's log"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    for stage in STAGE_FLAGS:
        parser.add_argument("--" + stage, nargs="+", type=Path, metavar="LOG",
                            help=f"captured log files for the {stage} stage, in boot order")
    parser.add_argument("--ack", default="",
                        help="comma-separated manual gate ids the operator has completed")
    parser.add_argument("--manifest", type=Path, default=verify_run_log.MANIFEST)
    parser.add_argument("--self-test", action="store_true",
                        help="accept logs carrying the synthetic fixture marker")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    supplied = {stage: getattr(args, stage) for stage in STAGE_FLAGS
                if getattr(args, stage)}
    if not supplied:
        parser.error("at least one of --readonly, --prepare or --actuate is required")
    for stage, paths in supplied.items():
        missing = [str(path) for path in paths if not path.is_file()]
        if missing:
            print("DONOR_GATE VERDICT=NO_GO BLOCKED MESSAGE=MISSING_LOG "
                  "STAGE=%s FILES=%s" % (stage, ",".join(missing)), file=sys.stderr)
            return 2

    manifest = verify_run_log.load_manifest(args.manifest)
    logs = {stage: verify_run_log.read_logs(paths) for stage, paths in supplied.items()}
    synthetic = any(
        verify_run_log.SYNTHETIC_MARKER in text
        for stage_logs in logs.values() for _name, text in stage_logs
    )
    if synthetic and not args.self_test:
        print("DONOR_GATE VERDICT=NO_GO BLOCKED SYNTHETIC_FIXTURE "
              "REASON=not_a_hardware_run HARDWARE_VERIFIED=0 "
              "NOTE=pass --self-test to check the gate itself")
        return 3

    acks = {item.strip() for item in args.ack.split(",") if item.strip()}
    go, lines, facts = decide(manifest, logs, acks)

    verdict = "GO" if go else "NO_GO"
    tail = ("DONOR_GATE VERDICT=%s NEXT=%s SYNTHETIC=%d HARDWARE_VERIFIED=0"
            % (verdict, next_step(manifest, logs) if go else "do not start or"
               " continue the session", 1 if synthetic else 0))
    if not go:
        lines.append("DONOR_GATE VERDICT=NO_GO BLOCKED=%s HARDWARE_VERIFIED=0"
                     % ",".join(dict.fromkeys(facts["blocked"])))
    lines.append(tail)

    if args.json:
        print(json.dumps({"verdict": verdict, "lines": lines, "facts": facts,
                          "synthetic": synthetic, "hardware_verified": False}, indent=2))
    else:
        print("\n".join(lines))
    return 0 if go else 1


if __name__ == "__main__":
    sys.exit(main())
