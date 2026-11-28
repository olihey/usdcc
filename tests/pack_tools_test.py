"""The release side of the optional packs (docs/USD_PLUGINS.md section 10): a pack as the
folder usdcc installs (tools/deploy.py), its archive and the index usdcc reads
(tools/ci/pack_index.py), and the test pack (tools/make_test_pack.py)."""

import contextlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "ci"))
import check_pack_install
import deploy
import fetch_usd
import make_test_pack
import pack_index
import usd_plugin_asset


def write(file, text="x"):
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(text, encoding="utf-8", newline="\n")


def a_pack(parent, name="alpha", data="one"):
    pack = Path(parent) / name
    write(pack / "usdcc-plugin.json", json.dumps({"id": f"org.usdcc.pack.{name}", "name": "Alpha", "version": "1.2", "api": 1}))
    write(pack / "plugin/usd/plugInfo.json", "{}")
    write(pack / "plugin/usd/thing/resources/plugInfo.json", "{}")
    write(pack / "bin/data.bin", data)
    return pack


def quiet(function, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return function(*args, **kwargs)


class ContentTests(unittest.TestCase):
    def test_the_same_files_have_the_same_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            first = a_pack(Path(temporary) / "a")
            second = a_pack(Path(temporary) / "b")
            self.assertEqual(pack_index.content_hash(first), pack_index.content_hash(second))
            self.assertEqual(len(pack_index.content_hash(first)), 64)

    def test_any_change_to_the_files_changes_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            pack = a_pack(temporary)
            before = pack_index.content_hash(pack)
            write(pack / "bin/data.bin", "two")
            changed = pack_index.content_hash(pack)
            self.assertNotEqual(before, changed)
            (pack / "bin/data.bin").rename(pack / "bin/other.bin")
            renamed = pack_index.content_hash(pack)
            self.assertNotEqual(changed, renamed)
            write(pack / "plugin/usd/more.txt")
            self.assertNotEqual(renamed, pack_index.content_hash(pack))

    def test_what_usdcc_writes_into_an_installed_pack_is_no_part_of_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            pack = a_pack(temporary)
            before = pack_index.content_hash(pack)
            size = pack_index.installed_size(pack)
            write(pack / "pack.json", "{}")
            write(pack / ".in-use", "")
            write(pack / ".removed", "")
            write(pack / "lib/libapp.so", "the app's, through the link")
            self.assertEqual(before, pack_index.content_hash(pack))
            self.assertEqual(size, pack_index.installed_size(pack))
            # Only at the top: a folder of that name further in is the pack's own.
            write(pack / "plugin/usd/thing/lib/libthing.so", "its own")
            self.assertNotEqual(before, pack_index.content_hash(pack))


class ArchiveTests(unittest.TestCase):
    def test_a_zip_holds_the_pack_under_its_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            pack = a_pack(temporary)
            write(pack / "pack.json", "{}")
            archive = pack_index.make_archive(pack, Path(temporary) / "out" / pack_index.archive_name("26.12.1", "windows-x64", "alpha"))
            self.assertEqual(archive.name, "usdcc-26.12.1-windows-x64-pack-alpha.zip")
            with zipfile.ZipFile(archive) as packed:
                self.assertEqual(sorted(packed.namelist()),
                                 ["alpha/bin/data.bin", "alpha/plugin/usd/plugInfo.json",
                                  "alpha/plugin/usd/thing/resources/plugInfo.json", "alpha/usdcc-plugin.json"])

    def test_a_tar_xz_elsewhere(self):
        with tempfile.TemporaryDirectory() as temporary:
            pack = a_pack(temporary)
            name = pack_index.archive_name("26.12.1", "linux-x64", "alpha")
            self.assertEqual(name, "usdcc-26.12.1-linux-x64-pack-alpha.tar.xz")
            archive = pack_index.make_archive(pack, Path(temporary) / "out" / name)
            with tarfile.open(archive) as packed:
                self.assertIn("alpha/usdcc-plugin.json", packed.getnames())
                self.assertEqual(len(packed.getnames()), 4)

    def test_the_index_is_what_usdcc_reads(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "out"
            pack = a_pack(temporary)
            archive = pack_index.make_archive(pack, out / pack_index.archive_name("26.12.1", "windows-x64", "alpha"))
            item = pack_index.entry(pack, archive, {"title": "Alpha", "description": "A pack", "license": "MIT",
                                                    "homepage": "https://example.org", "renderers": ["Alpha"]})
            index = pack_index.write_index(out, "26.12.1", "windows-x64", [item])
            self.assertEqual(index.name, "usdcc-26.12.1-windows-x64-packs.json")
            read = json.loads(index.read_text(encoding="utf-8"))
            self.assertEqual((read["schema"], read["usdcc"], read["platform"]), (1, "26.12.1", "windows-x64"))
            self.assertEqual(len(read["packs"]), 1)
            got = read["packs"][0]
            self.assertEqual((got["name"], got["title"], got["version"], got["license"]), ("alpha", "Alpha", "1.2", "MIT"))
            self.assertEqual(got["file"], archive.name)
            self.assertEqual(got["size"], archive.stat().st_size)
            self.assertEqual(len(got["sha256"]), 64)
            self.assertEqual(got["content"], pack_index.content_hash(pack))
            self.assertEqual(got["installed_size"], pack_index.installed_size(pack))

    def test_a_release_without_packs_has_an_empty_index(self):
        with tempfile.TemporaryDirectory() as temporary:
            empty = Path(temporary) / "packs"
            empty.mkdir()
            self.assertEqual(quiet(pack_index.main, ["--packs", str(empty), "--version", "26.12.1", "--platform", "windows-arm64",
                                                     "--out", str(Path(temporary) / "out")]), 0)
            read = json.loads((Path(temporary) / "out/usdcc-26.12.1-windows-arm64-packs.json").read_text(encoding="utf-8"))
            self.assertEqual(read["packs"], [])

    def test_the_test_pack_for_several_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.assertEqual(quiet(make_test_pack.main, [temporary, "--version", "1.0.0", "--version", "1.0.1", "--version", "1.0.2",
                                                         "--change", "1.0.2", "--platform", "windows-x64"]), 0)
            contents = [json.loads((Path(temporary) / f"usdcc-{version}-windows-x64-packs.json").read_text(encoding="utf-8"))
                        ["packs"][0]["content"] for version in ("1.0.0", "1.0.1", "1.0.2")]
            self.assertEqual(contents[0], contents[1])
            self.assertNotEqual(contents[1], contents[2])
            self.assertTrue((Path(temporary) / "usdcc-1.0.1-windows-x64-pack-hello.zip").is_file())


class PackFolderTests(unittest.TestCase):
    """deploy._pack_folder(): a pack's files, placed as a deploy has them, as the folder usdcc installs."""

    def deployed(self, root, libraries):
        names = ["plugin/usd/hdThing.dll", "plugin/usd/hdThing/resources/plugInfo.json", f"{libraries}/Thing.dll",
                 "licenses/OpenUSD-plugins/cycles/LICENSE.txt", "usd-plugins/cycles.json"]
        for name in names:
            write(Path(root) / name)
        return names

    def test_on_windows_its_dlls_stay_in_its_bin(self):
        with tempfile.TemporaryDirectory() as temporary:
            names = self.deployed(Path(temporary) / "from", "bin")
            target = quiet(deploy._pack_folder, "cycles", Path(temporary) / "from", names, Path(temporary) / "to/cycles", "5.3", True)
            for name in names:
                self.assertTrue((target / name).is_file(), name)
            self.assertEqual(json.loads((target / "plugin/usd/plugInfo.json").read_text(encoding="utf-8")), {"Includes": ["*/resources/"]})
            manifest = json.loads((target / "usdcc-plugin.json").read_text(encoding="utf-8"))
            self.assertEqual((manifest["id"], manifest["version"], manifest["api"], manifest["usd"]),
                             ("org.usdcc.pack.cycles", "5.3", 1, ["plugin/usd"]))
            self.assertEqual(manifest["name"], "Cycles")  # deps/packs.json
            # What tools/ci/pack_index.py goes by.
            self.assertEqual(json.loads((target / "usdcc-plugin.json").read_text(encoding="utf-8"))["name"],
                             pack_index.catalog()["cycles"]["title"])

    def test_elsewhere_a_pack_with_libraries_in_lib_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            names = self.deployed(Path(temporary) / "from", "lib")
            with self.assertRaises(SystemExit):
                quiet(deploy._pack_folder, "cycles", Path(temporary) / "from", names, Path(temporary) / "to/cycles", "5.3", False)
            self.assertFalse((Path(temporary) / "to/cycles/usdcc-plugin.json").exists())

    def test_elsewhere_a_pack_that_keeps_to_plugin_usd_is_taken(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "from"
            names = ["plugin/usd/moonray/lib/libmoonray.so", "plugin/usd/hd_moonray/resources/plugInfo.json",
                     "licenses/OpenUSD-plugins/moonray/LICENSE", "usd-plugins/moonray.json"]
            for name in names:
                write(source / name)
            target = quiet(deploy._pack_folder, "moonray", source, names, Path(temporary) / "to/moonray", "2026.29.1", False)
            self.assertTrue((target / "plugin/usd/moonray/lib/libmoonray.so").is_file())
            self.assertFalse((target / "lib").exists())  # usdcc makes it, a link to its own libraries
            self.assertEqual(json.loads((target / "usdcc-plugin.json").read_text(encoding="utf-8"))["name"], "MoonRay")

    def test_a_pack_with_nothing_to_register_or_a_stray_file_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "from"
            write(source / "licenses/x/LICENSE")
            with self.assertRaises(SystemExit):
                quiet(deploy._pack_folder, "empty", source, ["licenses/x/LICENSE"], Path(temporary) / "to/empty", "1", True)
            write(source / "plugin/usd/a/resources/plugInfo.json")
            write(source / "include/a.h")
            with self.assertRaises(SystemExit):
                quiet(deploy._pack_folder, "stray", source, ["plugin/usd/a/resources/plugInfo.json", "include/a.h"],
                      Path(temporary) / "to/stray", "1", True)

    def test_a_pack_with_a_plugin_folder_of_its_own_has_no_usd_folder_for_usdcc_to_register(self):
        # Arnold's, in plugin/arnold: its plugins start Arnold's core, so usdcc registers
        # them itself, once Settings names an Arnold (docs/USD_PLUGINS.md section 3).
        for windows in (True, False):
            with tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary) / "from"
                names = ["plugin/arnold/plugInfo.json", "plugin/arnold/hdArnold.so", "plugin/arnold/hdArnold/resources/plugInfo.json",
                         "licenses/OpenUSD-plugins/arnold/arnold/LICENSE.md", "usd-plugins/arnold.json"]
                for name in names:
                    write(source / name)
                write(source / "usd-plugins/arnold.json", json.dumps({"pack": "arnold", "version": "7.5.4.2", "license": "Apache-2.0"}))
                target = quiet(deploy._pack_folder, "arnold", source, names, Path(temporary) / "to/arnold", "7.5.4.2", windows)
                manifest = json.loads((target / "usdcc-plugin.json").read_text(encoding="utf-8"))
                self.assertEqual((manifest["id"], manifest["name"], manifest["version"], manifest["usd"]),
                                 ("org.usdcc.pack.arnold", "Arnold", "7.5.4.2", []))
                self.assertFalse((target / "plugin/usd").exists())
                # What usdcc's enableArnold() looks for under the pack's folder.
                self.assertTrue((target / "plugin/arnold/plugInfo.json").is_file())
                self.assertTrue((target / "usd-plugins/arnold.json").is_file())
                entry = pack_index.entry(target, target / "usdcc-plugin.json")
                self.assertEqual((entry["title"], entry["version"], entry["license"], entry["renderers"]),
                                 ("Arnold", "7.5.4.2", "Apache-2.0", ["Arnold"]))


class LocalPackTests(unittest.TestCase):
    """A pack of the local set whose pin says "publish": true (Arnold): built where its SDK
    is, attached to the OpenUSD release from there, and a download in the app only."""

    def test_arnold_is_published_and_not_built_here(self):
        spec = usd_plugin_asset.load("arnold")
        self.assertEqual((spec["set"], spec["publish"]), ("local", True))
        self.assertTrue(fetch_usd.published(spec))
        for platform in spec["platforms"]:
            self.assertNotIn("arnold", usd_plugin_asset.ci_packs(platform))
        # Nothing of the SDK's is asked into the pack: not its EULA either.
        self.assertNotIn("licenses", spec)

    def test_the_local_set_is_published_only_when_its_pin_says_so(self):
        self.assertFalse(fetch_usd.published({"set": "local"}))
        self.assertFalse(fetch_usd.published({"set": "local", "publish": False}))
        self.assertTrue(fetch_usd.published({"set": "local", "publish": True}))
        self.assertTrue(fetch_usd.published({"set": "optional"}))
        self.assertFalse(fetch_usd.published({"set": "optional", "publish": False}))

    def test_a_release_takes_it_as_a_download_in_the_app_only(self):
        packs = {"arnold": {"set": "local"}, "cycles": {"set": "optional"}, "adobe-fileformats": {"set": "standard"},
                 "usdfbx": {"set": "local"}}
        self.assertTrue(deploy._app_only("arnold", packs["arnold"]))
        self.assertFalse(deploy._app_only("cycles", packs["cycles"]))
        self.assertFalse(deploy._app_only("usdfbx", packs["usdfbx"]))  # no pin, so nothing says to publish it
        self.assertEqual(sorted(deploy._download_packs(packs, app_only=True)), ["arnold", "cycles"])
        # Without --pack-folders there is nowhere for it to go.
        self.assertEqual(sorted(deploy._download_packs(packs, app_only=False)), ["cycles"])

    def test_the_users_own_arnold_brings_its_core(self):
        self.assertEqual(deploy._sdk_libraries("arnold", "macos-arm64"), ["libai.dylib"])
        self.assertEqual(deploy._sdk_libraries("arnold", "linux-x64"), ["libai.so"])
        self.assertEqual(deploy._sdk_libraries("arnold", "win64"), ["ai.dll"])
        self.assertEqual(deploy._sdk_libraries("cycles", "macos-arm64"), [])

    def test_the_install_check_runs_inside_usdcc(self):
        compile("RESULT = 'r'\nPACKS = {}\n" + check_pack_install.CHECK, "check.py", "exec")


if __name__ == "__main__":
    unittest.main()
