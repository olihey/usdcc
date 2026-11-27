#!/usr/bin/env python3
"""Archives the optional packs of a release and writes their index, which usdcc reads to
install them (docs/USD_PLUGINS.md section 10).

  python tools/ci/pack_index.py --packs <folder> --version 26.12.1 --out <folder> [--platform linux-x64]

`--packs` holds one folder per pack, named as the pack and laid out as usdcc installs it
(tools/deploy.py --optional-packs writes them): usdcc-plugin.json, plugin/usd/..., bin/ on
Windows, licenses/. Into `--out` go, for the release to attach:

  usdcc-<version>-<platform>-pack-<name>.zip      on Windows
  usdcc-<version>-<platform>-pack-<name>.tar.xz   elsewhere, which keeps links and modes
  usdcc-<version>-<platform>-packs.json           the index; with no pack, an empty list

An index entry has what usdcc shows (title, description, licence, homepage, renderers:
deps/packs.json), what it checks a download against (size, sha256), and `content`: a
hash of the pack's files. Two releases' packs with one `content` are the same files, so
an update of usdcc copies the pack it has instead of downloading it again.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
import tarfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import PLATFORM, REPO, WINDOWS, fail  # noqa: E402

CATALOG = REPO / "deps/packs.json"
SCHEMA = 1
# What usdcc writes into an installed pack, which is no part of it.
NOT_CONTENT = ("pack.json", ".in-use", ".removed", "lib")


def release_platform():
    """What a release's files call this platform: windows-x64, linux-arm64, macos-arm64."""
    import platform

    arm = platform.machine().lower() in ("arm64", "aarch64")
    return f"{PLATFORM}-{'arm64' if arm else 'x64'}"


def archive_name(version, platform_name, pack, windows=None):
    windows = platform_name.startswith("windows") if windows is None else windows
    return f"usdcc-{version}-{platform_name}-pack-{pack}.{'zip' if windows else 'tar.xz'}"


def index_name(version, platform_name):
    return f"usdcc-{version}-{platform_name}-packs.json"


def _sha256(file):
    digest = hashlib.sha256()
    with open(file, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pack_files(folder):
    """The pack's files and links, as sorted paths relative to it with forward slashes."""
    folder = Path(folder)
    found = []
    for root, folders, files in os.walk(folder):
        relative = Path(root).relative_to(folder)
        if relative == Path():
            folders[:] = [name for name in folders if name not in NOT_CONTENT]
            files = [name for name in files if name not in NOT_CONTENT]
        # A link to a folder is a file of the pack, not a folder to walk into.
        for name in [name for name in folders if (Path(root) / name).is_symlink()]:
            folders.remove(name)
            files.append(name)
        found += [(relative / name).as_posix() for name in files]
    return sorted(found)


def content_hash(folder):
    """A hash of the pack's files: each one's path, what it is and what is in it. The same
    for two folders exactly when they hold the same files."""
    folder = Path(folder)
    digest = hashlib.sha256()
    for name in pack_files(folder):
        file = folder / name
        if file.is_symlink():
            what = "link " + os.readlink(file).replace("\\", "/")
        else:
            executable = not WINDOWS and bool(file.stat().st_mode & stat.S_IXUSR)
            what = ("program " if executable else "file ") + _sha256(file)
        digest.update(f"{name}\0{what}\n".encode())
    return digest.hexdigest()


def installed_size(folder):
    folder = Path(folder)
    return sum((folder / name).lstat().st_size for name in pack_files(folder))


def make_archive(folder, target):
    """Archives the pack `folder` as `target`, with the folder's name as the top folder."""
    folder = Path(folder)
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    names = pack_files(folder)
    if target.suffix == ".zip":
        if any((folder / name).is_symlink() for name in names):
            fail(f"{folder} has links, which a zip doesn't keep")
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name in names:
                archive.write(folder / name, f"{folder.name}/{name}")
    else:
        with tarfile.open(target, "w:xz", preset=9) as archive:
            for name in names:
                archive.add(folder / name, f"{folder.name}/{name}", recursive=False)
    return target


def catalog():
    return json.loads(CATALOG.read_text(encoding="utf-8")) if CATALOG.is_file() else {}


def entry(folder, archive, shown=None):
    """The index entry of the pack in `folder`, archived as `archive`."""
    folder = Path(folder)
    manifest = json.loads((folder / "usdcc-plugin.json").read_text(encoding="utf-8"))
    shown = catalog().get(folder.name, {}) if shown is None else shown
    record = {}
    for file in sorted((folder / "usd-plugins").glob("*.json")) if (folder / "usd-plugins").is_dir() else []:
        record = json.loads(file.read_text(encoding="utf-8"))
    return {
        "name": folder.name,
        "title": shown.get("title", manifest.get("name", folder.name)),
        "description": shown.get("description", manifest.get("description", "")),
        "version": manifest.get("version", ""),
        "license": shown.get("license", record.get("license", "")),
        "homepage": shown.get("homepage", ""),
        "renderers": shown.get("renderers", []),
        "file": Path(archive).name,
        "size": Path(archive).stat().st_size,
        "sha256": _sha256(archive),
        "installed_size": installed_size(folder),
        "content": content_hash(folder),
    }


def write_index(out, version, platform_name, entries):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    file = out / index_name(version, platform_name)
    index = {"schema": SCHEMA, "usdcc": version, "platform": platform_name,
             "packs": sorted(entries, key=lambda item: item["name"])}
    file.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8", newline="\n")
    return file


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--packs", type=Path, required=True, help="a folder per pack, laid out as usdcc installs it")
    parser.add_argument("--version", required=True, help="the usdcc version the packs are for")
    parser.add_argument("--platform", default=release_platform(), help="as a release's files name it (default: this one)")
    parser.add_argument("--out", type=Path, required=True, help="where the archives and the index go")
    args = parser.parse_args(argv)

    entries = []
    folders = sorted(folder for folder in args.packs.iterdir() if folder.is_dir()) if args.packs.is_dir() else []
    for folder in folders:
        if not (folder / "usdcc-plugin.json").is_file():
            fail(f"{folder} isn't a pack: it has no usdcc-plugin.json")
        target = make_archive(folder, args.out / archive_name(args.version, args.platform, folder.name))
        item = entry(folder, target)
        entries.append(item)
        print(f"{item['file']}: {item['size'] / 2**20:,.1f} MB ({item['installed_size'] / 2**20:,.1f} MB installed), "
              f"content {item['content'][:12]}")
        if item["size"] >= 2 * 2**30:
            fail(f"{item['file']} is over GitHub's 2 GiB per-asset limit")
    index = write_index(args.out, args.version, args.platform, entries)
    print(f"{index.name}: {len(entries)} pack{'' if len(entries) == 1 else 's'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
