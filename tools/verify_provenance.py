#!/usr/bin/env python3
"""Check that every firmware payload the image can load has a provenance record.

`tools/verify_reference_assets.py` answers "is the file on this disk the byte
sequence that was pinned". It cannot answer "where did those bytes come from",
and until now nothing asked. This tool asks, against the firmware's own asset
table in `src/profile.c`:

  * every row the image can load has a record in `docs/asset_provenance.json`,
  * the record's digest and length agree with the row the firmware enforces,
  * no record describes an asset the firmware does not have,
  * a record never claims a signature check this tree cannot perform, and
  * every payload present on disk matches its record.

What this is not: a signature check. `SIGNATURE_AUTHENTICATED=0` is printed on
every run, and the records say the same thing. Provenance here means "named and
machine-checked", not "proven genuine".

    python3 tools/verify_provenance.py [--root .] [--records docs/asset_provenance.json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROW = re.compile(
    r'\{(L"\\\\[^"]+"),\s*"([^"]+)",\s*"([0-9a-f]{64})",\s*(\d+)U\}')
ARCH_END = re.compile(r'\},\s*(L"\\\\[^"]+"|NULL)\};')
ARCH_NAME = re.compile(r"static const GU_ARCH_ASSETS (\w+)=\{\{")


class Failure(Exception):
    pass


def asset_table(profile: str):
    """Every row, each carrying its own table: the ESP path it will look for and
    the shared fallback that table names for payloads it does not carry."""
    rows = []
    for arch in ARCH_NAME.finditer(profile):
        start = arch.end()
        end = ARCH_END.search(profile, start)
        if not end:
            raise Failure("unterminated asset table: " + arch.group(1))
        block = profile[start:end.start()]
        fallback = None
        if end.group(1).startswith('L"'):
            fallback = (end.group(1).replace('L"\\\\', "").replace('"', "")
                        .replace("\\\\", "/"))
        for row in ROW.finditer(block):
            esp = row.group(1).replace('L"\\\\', "").replace('"', "").replace("\\\\", "/")
            rows.append({
                "table": arch.group(1),
                "esp_path": esp,
                "asset": row.group(2),
                "sha256": row.group(3),
                "bytes": int(row.group(4)),
                "fallback": fallback,
            })
    if not rows:
        raise Failure("no asset rows found in src/profile.c")
    return rows


def digest(path: Path) -> tuple[int, str]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            size += len(chunk)
            h.update(chunk)
    return size, h.hexdigest()


def resolve(root: Path, row):
    """The file this row uses: its own path, or the table's shared fallback."""
    for candidate in (row["esp_path"], row.get("fallback")):
        if not candidate:
            continue
        path = root / candidate
        if path.is_file():
            return path
    return None


def verify(root: Path, records_path: Path, table_path: Path):
    profile = table_path.read_text(encoding="utf-8")
    rows = asset_table(profile)
    data = json.loads(records_path.read_text(encoding="utf-8"))
    if data.get("schema") != "gu-asset-provenance/1":
        raise Failure("unknown provenance schema: %r" % data.get("schema"))
    auth = data.get("authentication") or {}
    if auth.get("signature_authenticated") is not False:
        raise Failure("provenance must not claim a signature check this tree cannot perform")
    records = {}
    for record in data.get("records", []):
        asset = record.get("asset")
        if not asset:
            raise Failure("record without an asset name")
        if asset in records:
            raise Failure("duplicate record: " + asset)
        records[asset] = record

    seen = set()
    present = absent = 0
    for row in rows:
        record = records.get(row["asset"])
        if record is None:
            raise Failure("no provenance record for %s (%s)" % (row["asset"], row["table"]))
        seen.add(row["asset"])
        if record.get("sha256") != row["sha256"]:
            raise Failure("provenance digest disagrees with the firmware pin: " + row["asset"])
        if int(record.get("bytes", -1)) != row["bytes"]:
            raise Failure("provenance length disagrees with the firmware pin: " + row["asset"])
        if not record.get("role"):
            raise Failure("provenance record has no role: " + row["asset"])
        path = resolve(root, row)
        if path is None:
            absent += 1
            continue
        present += 1
        size, sha = digest(path)
        if size != row["bytes"] or sha != row["sha256"]:
            raise Failure("payload does not match its record: " + str(path.relative_to(root)))
    orphans = sorted(set(records) - seen)
    if orphans:
        raise Failure("provenance describes assets the firmware does not load: " + ", ".join(orphans))
    return {"records": len(records), "present": present, "absent": absent,
            "tables": len({row["table"] for row in rows})}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=".")
    parser.add_argument("--records", default="docs/asset_provenance.json")
    parser.add_argument("--table", default="src/profile.c")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        result = verify(root, root / args.records, root / args.table)
    except Failure as error:
        print("PROVENANCE=FAILED REASON=%s SIGNATURE_AUTHENTICATED=0" % error)
        return 1
    print("PROVENANCE=RECORDED=%d TABLES=%d PRESENT=%d ABSENT=%d "
          "SIGNATURE_AUTHENTICATED=0 AUTHENTICITY_UNVERIFIED=1"
          % (result["records"], result["tables"], result["present"], result["absent"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
