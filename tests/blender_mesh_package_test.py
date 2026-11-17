"""Source inventory and combined-artifact separation checks."""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import blender_distribution as review
import blender_linux_distribution as linux_review
import blender_mesh_package as package
import blender_ubuntu_sources as ubuntu_sources

import deploy


class PackageTests(unittest.TestCase):
    def test_ubuntu_source_checksums_and_versions_cannot_be_substituted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            folder = root / "python-fixture"
            folder.mkdir()
            payload = folder / "python.orig.tar.xz"
            payload.write_bytes(b"exact source archive")
            checksum = review.sha256(payload)
            dsc = folder / "python.dsc"
            dsc.write_text(
                "-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n\nSource: python-fixture\nVersion: 2:3.12-1ubuntu1\nChecksums-Sha256:\n "
                + checksum
                + " 20 python.orig.tar.xz\n\n-----BEGIN PGP SIGNATURE-----\nfixture\n"
            )
            receipt = {
                "package": "python-fixture",
                "version": "2:3.12-1ubuntu1",
                "folder": folder.name,
                "files": [
                    {"file": dsc.name, "sha256": review.sha256(dsc)},
                    {"file": payload.name, "sha256": checksum},
                ],
            }
            ubuntu_sources.verify_receipts([receipt], root)
            payload.write_bytes(b"different source archive")
            receipt["files"][1]["sha256"] = review.sha256(payload)
            with self.assertRaisesRegex(RuntimeError, "disagrees with DSC"):
                ubuntu_sources.verify_receipts([receipt], root)
            receipt["version"] = "newer-unreviewed-version"
            with self.assertRaisesRegex(RuntimeError, "Wrong source version"):
                ubuntu_sources.verify_receipts([receipt], root)

    def test_linux_cannot_use_windows_source_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.tar.gz"
            Path(str(source) + ".json").write_text(
                json.dumps({"schema": 2, "platform": "windows-x64"})
            )
            with self.assertRaisesRegex(
                RuntimeError, "Windows receipts cannot approve Linux"
            ):
                linux_review.finalize(root, root, source)
            self.assertFalse(
                json.loads((root / "distribution-readiness.json").read_text())[
                    "ready_for_distribution"
                ]
            )

    def test_linux_rejects_unknown_or_excluded_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for component, path in (
                ("UNKNOWN", "lib/surprise.so"),
                ("OpenUSD", "plugin/usd/hdGatling.so"),
                ("OpenUSD", "lib/libtbb_debug.so.2"),
                ("usdcc", "lib/libunreviewed.so"),
            ):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    linux_review.validate_rows(
                        [{"component": component, "path": path}], [], root, root
                    )

    @unittest.skipUnless(sys.platform == "linux", "Linux ELF/RUNPATH verification")
    def test_elf_provenance_allows_runpath_relocation_but_detects_code_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "true"
            shutil.copy2("/bin/true", target)
            original = linux_review.elf_identity(target)
            subprocess.run(
                ["patchelf", "--set-rpath", "$ORIGIN/lib", str(target)], check=True
            )
            self.assertEqual(linux_review.elf_identity(target), original)
            data = bytearray(target.read_bytes())
            # Flip the first opcode at the program's ELF entry point, found through readelf.
            entry = int(
                subprocess.check_output(["readelf", "-h", str(target)], text=True)
                .split("Entry point address:")[1]
                .split()[0],
                16,
            )
            data[entry] ^= 1
            target.write_bytes(data)
            self.assertNotEqual(linux_review.elf_identity(target), original)

    @unittest.skipUnless(
        sys.platform == "linux", "Linux symlinks and executable permissions"
    )
    def test_linux_manifest_checks_link_targets_and_executable_permissions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = root / "lib.so.1.0"
            payload.write_bytes(b"library")
            alias = root / "lib.so.1"
            alias.symlink_to(payload.name)
            report = {
                "platform": "linux-x86_64",
                "ready_for_distribution": True,
                "files": linux_review.file_manifest(root),
            }
            (root / "distribution-readiness.json").write_text(json.dumps(report))
            linux_review.verify(root)
            payload.chmod(payload.stat().st_mode ^ 0o100)
            with self.assertRaisesRegex(
                RuntimeError, "permissions or symlinks changed"
            ):
                linux_review.verify(root)
            alias.unlink()
            alias.symlink_to("/bin/true")
            with self.assertRaisesRegex(RuntimeError, "Escaping"):
                linux_review.file_manifest(root)

    def test_usd_notices_use_actual_license_not_materialx(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            usd = root / "usd"
            usd.mkdir()
            (usd / "LICENSE").write_text("MaterialX Apache")
            with (
                mock.patch.object(deploy, "cache_value", return_value=""),
                self.assertRaises(SystemExit),
            ):
                deploy._usd_notices(usd, root / "licenses")
            source = usd / "licenses/OpenUSD"
            source.mkdir(parents=True)
            (source / "LICENSE.txt").write_text("TOST fixture")
            (source / "NOTICE.txt").write_text("Pixar fixture")
            deploy._usd_notices(usd, root / "licenses")
            self.assertEqual(
                (root / "licenses/OpenUSD/LICENSE.txt").read_text(), "TOST fixture"
            )

    def test_stale_usd_renderer_is_not_copied(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, target = root / "input", root / "output"
            for name in ("hdStorm", "hdGatling", "unreviewedRenderer"):
                folder = source / name
                folder.mkdir(parents=True)
                (folder / "plugInfo.json").write_text("{}")
                (source / (name + ".dll")).write_bytes(b"binary")
            deploy._openusd_plugins(source, target, deploy.copy_tree)
            self.assertEqual(
                sorted(p.name for p in target.iterdir()), ["hdStorm", "hdStorm.dll"]
            )

    def test_unknown_binary_and_proprietary_runtime_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for component, name in (
                ("UNKNOWN", "unknown.dll"),
                ("OpenUSD", "plugin/usd/hdGatling.dll"),
                ("OpenUSD", "bin/unreviewed.dll"),
            ):
                with self.subTest(name=name), self.assertRaises(RuntimeError):
                    review.check_binary_closure(
                        [{"path": name, "component": component, "imports": []}],
                        root,
                        root,
                    )

    def test_reviewed_manifest_rejects_added_changed_and_removed_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = root / "LICENSE.txt"
            payload.write_text("GPL")
            report = {
                "ready_for_distribution": True,
                "files": [{"path": "LICENSE.txt", "sha256": review.sha256(payload)}],
            }
            (root / "distribution-readiness.json").write_text(json.dumps(report))
            review.verify(root)
            payload.write_text("changed")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                review.verify(root)
            payload.write_text("GPL")
            (root / "unexpected.dll").write_bytes(b"binary")
            with self.assertRaises(RuntimeError):
                review.verify(root)
            (root / "unexpected.dll").unlink()
            nested = root / "nested/distribution-readiness.json"
            nested.parent.mkdir()
            nested.write_text("unexpected")
            with self.assertRaises(RuntimeError):
                review.verify(root)
            nested.unlink()
            payload.unlink()
            with self.assertRaises(RuntimeError):
                review.verify(root)

    def test_incomplete_and_mismatched_source_receipts_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "predates"):
            review.source_matches({"schema": 1}, [])
        info = {"schema": 2, "platform": "windows-x64", "resources_complete": False}
        with (
            mock.patch.object(review, "WINDOWS", True),
            self.assertRaisesRegex(RuntimeError, "Incomplete"),
        ):
            review.source_matches(info, [])
        info.update(
            resources_complete=True,
            resources=[],
            binary_files=[{"path": "app.exe", "sha256": "old"}],
        )
        with (
            mock.patch.object(review, "WINDOWS", True),
            self.assertRaisesRegex(RuntimeError, "Binaries differ"),
        ):
            review.source_matches(info, [{"path": "app.exe", "sha256": "new"}])

    def test_changed_dependency_pin_requires_renewed_review(self):
        info = {
            "schema": 2,
            "platform": "windows-x64",
            "resources_complete": True,
            "resources": [],
            "binary_files": [],
            "source_files": [],
            "license_policy_sha256": review.sha256(review.REVIEW),
            "supplemental_resources": [],
        }
        lock = {"resources": [], "reviewed_inputs": {"blender_pin_sha256": "changed"}}
        with (
            mock.patch.object(review, "WINDOWS", True),
            mock.patch.object(review.json, "loads", return_value=lock),
            self.assertRaisesRegex(RuntimeError, "renewed review"),
        ):
            review.source_matches(info, [])

    def test_source_snapshot_excludes_private_agent_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in [
                "src/local.cpp",
                "CMakeLists.txt",
                ".claude/private.md",
                "tools/new.py",
                "private.txt",
            ]:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("source")
            with (
                mock.patch.object(package, "REPO", root),
                mock.patch.object(
                    package,
                    "git",
                    side_effect=[
                        b"CMakeLists.txt\0src/local.cpp\0.claude/private.md\0",
                        b"tools/new.py\0private.txt\0",
                    ],
                ),
            ):
                self.assertEqual(
                    package.project_files(),
                    ["CMakeLists.txt", "src/local.cpp", "tools/new.py"],
                )

    def test_candidate_marks_review_pending_and_keeps_notices(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plugin = root / "build/plugins/blender_mesh_tools"
            license_folder = plugin / "licenses/blender/spdx"
            license_folder.mkdir(parents=True)
            (license_folder / "GPL-3.0-or-later.txt").write_text("GPL test fixture")
            (plugin / "usdcc-plugin.json").write_text("{}")
            (plugin / "build-info.json").write_text("{}")
            target = root / "candidate"
            target.mkdir()
            package.include_candidate(target, root / "build")
            report = json.loads((target / "distribution-readiness.json").read_text())
            self.assertEqual(report["legal_distribution_review"], "pending")
            self.assertIsNone(report["source_bundle"])
            self.assertTrue((target / "licenses/GPL-3.0-or-later.txt").is_file())
            self.assertFalse((target / "plugins/rtx_render").exists())
            bundle = root / "source.tar.gz"
            bundle.write_bytes(b"source")
            Path(str(bundle) + ".json").write_text(
                json.dumps({"archive_sha256": "wrong"})
            )
            with self.assertRaisesRegex(RuntimeError, "checksum"):
                package.include_candidate(target, root / "build", bundle)


if __name__ == "__main__":
    unittest.main()
