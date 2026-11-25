"""The provenance gate: every payload the image can load has a record.

`tools/verify_reference_assets.py` proves the bytes on this disk are the pinned
bytes. It cannot say where they came from, and D3 named that gap: the 84 MB
payload has no provenance beyond a local hash.

`tools/verify_provenance.py` reads the firmware's own asset table out of
`src/profile.c` and requires a record for every row, matching digest and length,
with no orphan records and no claim of a signature check this tree cannot
perform. These tests run it on the repository and then break it deliberately:
a missing record, a record with a different digest, a record for an asset the
firmware does not load, a claimed signature, an unknown schema, and a payload
that no longer matches its record.

No payload byte is verified against NVIDIA: `SIGNATURE_AUTHENTICATED=0` is part
of the output this module pins.
"""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "verify_provenance.py"
RECORDS = ROOT / "docs" / "asset_provenance.json"


def run(root, records=None):
    command = ["python3", str(ROOT / "tools" / "verify_provenance.py"), "--root", str(root)]
    if records is not None:
        command += ["--records", str(records)]
    return subprocess.run(command, capture_output=True, text=True, timeout=180)


class Provenance(unittest.TestCase):
    def test_every_loadable_payload_has_a_record(self):
        result = run(ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PROVENANCE=RECORDED=14 TABLES=3", result.stdout)
        self.assertIn("SIGNATURE_AUTHENTICATED=0", result.stdout)
        self.assertIn("AUTHENTICITY_UNVERIFIED=1", result.stdout)
        # The fork carries no Turing payload, and the gate says so rather than
        # silently counting five records it never checked.
        self.assertIn("ABSENT=", result.stdout)

    def test_the_records_and_the_firmware_pins_agree(self):
        records = json.loads(RECORDS.read_text(encoding="utf-8"))
        profile = (ROOT / "src" / "profile.c").read_text(encoding="utf-8")
        self.assertEqual(records["schema"], "gu-asset-provenance/1")
        self.assertIs(records["authentication"]["signature_authenticated"], False)
        for record in records["records"]:
            with self.subTest(asset=record["asset"]):
                self.assertIn('"%s"' % record["asset"], profile)
                self.assertIn(record["sha256"], profile)
                self.assertIn("%dU" % record["bytes"], profile)
                self.assertTrue(record["role"])

    def _broken(self, mutate):
        """Copy the records, break them one way, and expect a named refusal."""
        with tempfile.TemporaryDirectory() as directory:
            data = json.loads(RECORDS.read_text(encoding="utf-8"))
            mutate(data)
            path = Path(directory) / "records.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            return run(ROOT, path)

    def test_a_missing_record_is_refused(self):
        def drop(data):
            data["records"] = [r for r in data["records"]
                               if r["asset"] != "gpu-unlock/610.43.03/gsp_ga10x.bin"]
        result = self._broken(drop)
        self.assertEqual(result.returncode, 1)
        self.assertIn("no provenance record for gpu-unlock/610.43.03/gsp_ga10x.bin", result.stdout)

    def test_a_changed_digest_is_refused(self):
        def flip(data):
            for record in data["records"]:
                if record["asset"].endswith("booter_load_ad102.bin"):
                    record["sha256"] = "0" * 64
        result = self._broken(flip)
        self.assertEqual(result.returncode, 1)
        self.assertIn("provenance digest disagrees with the firmware pin", result.stdout)

    def test_a_changed_length_is_refused(self):
        def grow(data):
            for record in data["records"]:
                if record["asset"].endswith("gsp_boot_desc_ad102.bin"):
                    record["bytes"] += 1
        result = self._broken(grow)
        self.assertEqual(result.returncode, 1)
        self.assertIn("provenance length disagrees", result.stdout)

    def test_an_orphan_record_is_refused(self):
        def add(data):
            data["records"].append({"asset": "gpu-unlock/610.43.03/not-a-file.bin",
                                    "role": "invented", "sha256": "0" * 64, "bytes": 1})
        result = self._broken(add)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not load", result.stdout)
        self.assertIn("not-a-file.bin", result.stdout)

    def test_a_signature_claim_is_refused(self):
        def claim(data):
            data["authentication"]["signature_authenticated"] = True
        result = self._broken(claim)
        self.assertEqual(result.returncode, 1)
        self.assertIn("signature check this tree cannot perform", result.stdout)

    def test_an_unknown_schema_is_refused(self):
        def relabel(data):
            data["schema"] = "gu-asset-provenance/2"
        result = self._broken(relabel)
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown provenance schema", result.stdout)

    def test_a_payload_that_no_longer_matches_is_refused(self):
        """A payload edited on disk must fail here, not only in the hash check."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "docs").mkdir()
            (root / "AD102-ASSETS").mkdir()
            shutil.copy(ROOT / "src" / "profile.c", root / "src" / "profile.c")
            shutil.copy(RECORDS, root / "docs" / "asset_provenance.json")
            for name in ("AD102-ASSETS/booter_load_ad102.bin",
                         "AD102-ASSETS/v67-ad102-610.43.03-four-plm.bin",
                         "AD102-ASSETS/gsp_ga10x.bin",
                         "AD102-ASSETS/gsp_boot_ad102.bin",
                         "AD102-ASSETS/gsp_boot_desc_ad102.bin",
                         "GA102-ASSETS/booter_load_ga102.bin",
                         "GA102-ASSETS/v67-ga102-610.43.03.bin",
                         "GA102-ASSETS/gsp_boot_ga102.bin",
                         "GA102-ASSETS/gsp_boot_desc_ga102.bin"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(ROOT / name, target)
            clean = run(root)
            self.assertEqual(clean.returncode, 0, clean.stdout)
            self.assertIn("PRESENT=10 ABSENT=5", clean.stdout)
            victim = root / "AD102-ASSETS" / "gsp_boot_desc_ad102.bin"
            victim.write_bytes(victim.read_bytes() + b"\0")
            broken = run(root)
            self.assertEqual(broken.returncode, 1)
            self.assertIn("payload does not match its record", broken.stdout)

    def test_the_workflow_runs_it(self):
        workflow = (ROOT / ".github" / "workflows" / "gpu-topology-diagnostic.yml").read_text(
            encoding="utf-8")
        self.assertIn("tools/verify_provenance.py", workflow)


if __name__ == "__main__":
    unittest.main()
