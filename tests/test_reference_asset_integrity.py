"""Negative checks for the offline, read-only asset hash verifier."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

source = Path(__file__).resolve().parents[1] / "tools" / "verify_reference_assets.py"
spec = importlib.util.spec_from_file_location("verify_reference_assets", source)
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class AssetChecks(unittest.TestCase):
    def test_hash_size_and_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "asset.bin"
            sha = hashlib.sha256(b"1234").hexdigest()
            self.assertEqual(verifier.check_file(path, 4, sha), "MISSING_OR_UNREADABLE")
            path.write_bytes(b"1234")
            self.assertEqual(verifier.check_file(path, 4, sha), "MATCHED")
            self.assertEqual(verifier.check_file(path, 3, sha), "REFUSED_SIZE_OR_TYPE")
            path.write_bytes(b"5678")
            self.assertEqual(verifier.check_file(path, 4, sha), "HASH_MISMATCH")

    def test_full_set_and_extras(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for d in ("AD102-ASSETS", "GA102-ASSETS"):
                (root / d).mkdir()
            name = "GA102-ASSETS/ref.bin"
            sha = hashlib.sha256(b"good").hexdigest()
            pins = {name: (4, sha)}
            self.assertNotEqual(verifier.verify(root, pins)[name], "MATCHED")
            (root / name).write_bytes(b"good")
            self.assertEqual(verifier.verify(root, pins), {name: "MATCHED"})
            (root / "GA102-ASSETS" / "extra.bin").write_bytes(b"unexpected")
            self.assertIn("UNEXPECTED_BINARY", verifier.verify(root, pins).values())

    def test_no_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / "actual.bin"
            link = root / "link.bin"
            target.write_bytes(b"good")
            try:
                link.symlink_to(target)
            except (OSError, NotImplementedError):
                self.skipTest("symlink support unavailable")
            self.assertEqual(verifier.check_file(link, 4, hashlib.sha256(b"good").hexdigest()),
                             "REFUSED_SYMLINK")


if __name__ == "__main__":
    unittest.main()
