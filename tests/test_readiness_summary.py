"""The readiness percentage is computed from the item list, not typed into prose.

`docs/readiness_status.json` records one row per inventory item and
`tools/render_readiness.py` renders the "Readiness at a glance" block from it.
These tests keep both honest:

  1. the manifest covers exactly the items the document tracks, with the same
     per-class counts - a new item cannot be added to one without the other;
  2. every met or partial row points at a fix-log section that exists;
  3. no row claims hardware verification, because nothing here has run on a GPU -
     the summary reports that as its own number rather than folding it away;
  4. the block in the inventory is byte-identical to the rendered manifest, sits
     above the item classes, and its percentages match the counts.
"""
from pathlib import Path
import json
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / "docs" / "readiness_status.json").read_text(encoding="utf-8"))
DOCUMENT = (ROOT / "docs" / "PRODUCTION_READINESS_INVENTORY.md").read_text(encoding="utf-8")

sys.path.insert(0, str(ROOT / "tools"))
import render_readiness  # noqa: E402

ITEMS = MANIFEST["items"]
EXPECTED_PER_CLASS = {"A": 8, "B": 10, "C": 10, "D": 5, "E": 10}


def summary_block() -> str:
    begin = DOCUMENT.index(render_readiness.BEGIN)
    end = DOCUMENT.index(render_readiness.END) + len(render_readiness.END)
    return DOCUMENT[begin:end]


class TheManifestMatchesTheDocument(unittest.TestCase):
    def test_every_item_is_tracked_exactly_once(self):
        ids = [item["id"] for item in ITEMS]
        self.assertEqual(len(ids), len(set(ids)), "duplicate item id")
        self.assertEqual(len(ids), sum(EXPECTED_PER_CLASS.values()))

    def test_each_class_has_the_items_the_document_lists(self):
        for letter, expected in EXPECTED_PER_CLASS.items():
            rows = [item for item in ITEMS if item["class"] == letter]
            self.assertEqual(len(rows), expected, f"class {letter}")
            self.assertTrue(all(item["id"].startswith(letter) for item in rows))

    def test_the_document_still_lists_every_tracked_item(self):
        for item in ITEMS:
            identifier = item["id"]
            # Class A items are headings, B/C/D are rows, E is a numbered list,
            # and a closed B/C/D item is struck through in its row.
            forms = (f"### {identifier}.", f"| {identifier} |", f"| ~~{identifier}~~ |",
                     f"({identifier},")
            self.assertTrue(any(form in DOCUMENT for form in forms),
                            f"{identifier} is in the manifest but not the document")

    def test_the_document_tracks_no_item_the_manifest_lost(self):
        found = set(re.findall(r"^### ([A-E]\d+)\.", DOCUMENT, re.M))
        found |= set(re.findall(r"^\| (?:~~)?([B-E]\d+)(?:~~)? \|", DOCUMENT, re.M))
        found |= set(re.findall(r"\(([E]\d+), P\d\)", DOCUMENT))
        self.assertEqual(found, {item["id"] for item in ITEMS},
                         "the document and the manifest disagree about which items exist")


class TheStatesAreGrounded(unittest.TestCase):
    def test_states_and_priorities_come_from_one_vocabulary(self):
        for item in ITEMS:
            self.assertIn(item["source"], ("done", "partial", "none"), item["id"])
            self.assertIn(item["priority"], ("P0", "P1", "P2"), item["id"])

    def test_every_claim_points_at_a_fix_log_section_that_exists(self):
        headings = set(re.findall(r"^### (11\.\d+) ", DOCUMENT, re.M))
        for item in ITEMS:
            if item["source"] == "none":
                self.assertIsNone(item["section"], f"{item['id']} claims nothing")
                continue
            self.assertIsNotNone(item["section"], f"{item['id']} claims work with no pointer")
            self.assertIn(item["section"], headings,
                          f"{item['id']} points at a missing §{item['section']}")

    def test_nothing_claims_to_be_verified_on_hardware(self):
        for item in ITEMS:
            self.assertIs(item["hardware_verified"], False,
                          f"{item['id']} claims hardware evidence")
        self.assertIn("Verified on hardware: 0 of", summary_block())
        self.assertIn("It is 0% because no run has ever driven a card.", summary_block())

    def test_every_item_says_what_it_is(self):
        for item in ITEMS:
            self.assertTrue(item["note"].strip(), f"{item['id']} has no note")


class TheSummaryIsGenerated(unittest.TestCase):
    def test_the_block_is_byte_identical_to_the_render(self):
        self.assertEqual(summary_block(), render_readiness.render(MANIFEST),
                         "docs/PRODUCTION_READINESS_INVENTORY.md is stale: run "
                         "python3 tools/render_readiness.py --write")

    def test_the_block_comes_before_the_items_it_summarises(self):
        start = DOCUMENT.index(render_readiness.BEGIN)
        self.assertLess(start, DOCUMENT.index("## 0. What the source can do today"))
        self.assertLess(start, DOCUMENT.index("## 2. Class A"))

    def test_the_percentages_are_the_counts(self):
        counts = {state: sum(1 for i in ITEMS if i["source"] == state)
                  for state in ("done", "partial", "none")}
        total = len(ITEMS)
        block = summary_block()
        for state, label in (("done", "Met"), ("partial", "Partial"), ("none", "Not started")):
            share = render_readiness.percent(counts[state], total)
            self.assertIn(f"| {counts[state]} | {share}% |", block, label)
        self.assertIn(f"**Source progress: {render_readiness.percent(counts['done'] + 0.5 * counts['partial'], total)}%**", block)

    def test_the_class_table_adds_up(self):
        block = summary_block()
        for letter, expected in EXPECTED_PER_CLASS.items():
            self.assertRegex(block, rf"\| {letter} - [^|]+ \| \d+ \| \d+ \| \d+ \| {expected} \|")
        rows = re.findall(r"^\| ([A-E]) - [^|]+ \| (\d+) \| (\d+) \| (\d+) \| (\d+) \|",
                          block, re.M)
        self.assertEqual(len(rows), len(EXPECTED_PER_CLASS))
        for letter, met, partial, missing, total in rows:
            self.assertEqual(int(met) + int(partial) + int(missing), int(total), letter)


if __name__ == "__main__":
    unittest.main()
