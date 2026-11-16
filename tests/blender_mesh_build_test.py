"""Checks source pin verification and extraction failures without network access."""

import hashlib
import importlib.util
import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("blender_build", REPO / "tools/build_blender_mesh.py")
DRIVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRIVER)


class BlenderSourceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.pin = {"commit": "a" * 40, "sha256": "0" * 64, "archive": "https://invalid.example/source"}

    def archive(self, member):
        output = self.work / "downloads" / f"blender-{self.pin['commit']}.tar.gz"
        output.parent.mkdir()
        with tarfile.open(output, "w:gz") as package:
            info = tarfile.TarInfo(member)
            info.size = 4
            package.addfile(info, io.BytesIO(b"test"))
        self.pin["sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
        return output

    def test_missing_archive_requires_explicit_fetch(self):
        with self.assertRaisesRegex(RuntimeError, "--fetch"):
            DRIVER.source_tree(self.pin, self.work, False)

    def test_checksum_is_checked_before_extraction(self):
        self.archive("unexpected/file")
        self.pin["sha256"] = "0" * 64
        with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
            DRIVER.source_tree(self.pin, self.work, False)
        self.assertFalse((self.work / "source").exists())

    def test_unexpected_archive_root_is_rejected(self):
        self.archive("unexpected/file")
        with self.assertRaisesRegex(RuntimeError, "Unexpected source archive path"):
            DRIVER.source_tree(self.pin, self.work, False)

    def test_archive_traversal_is_rejected(self):
        self.archive(f"blender-{self.pin['commit']}/../../escaped")
        with self.assertRaises(tarfile.FilterError):
            DRIVER.source_tree(self.pin, self.work, False)
        self.assertFalse((self.work / "escaped").exists())

    def test_verified_source_is_reusable_but_changed_pin_is_rejected(self):
        self.archive(f"blender-{self.pin['commit']}/file")
        source = DRIVER.source_tree(self.pin, self.work, False)
        self.assertEqual((source / "file").read_text(), "test")
        self.assertEqual(DRIVER.source_tree(self.pin, self.work, False), source)
        self.assertEqual(json.loads((source / ".usdcc-verified-source.json").read_text()), self.pin)
        self.pin["version"] = "changed"
        with self.assertRaisesRegex(RuntimeError, "stamp differs"):
            DRIVER.source_tree(self.pin, self.work, False)


if __name__ == "__main__":
    unittest.main()
