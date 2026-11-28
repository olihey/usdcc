"""Downloads the prebuilt OpenUSD and USD plugin packs the current pins name.

What CI builds usdcc against, into a folder of yours: the OpenUSD asset that
deps/usd.json describes (docs/CI.md section 2) and, unzipped over it, the
"standard" packs of deps/usd-plugins (docs/USD_PLUGINS.md), which are the ones a
usdcc release ships. Then pass the folder to tools/build.py with --usd-install.
A new machine needs no OpenUSD build this way.

    python tools/fetch_usd.py C:\\deps\\usd
    python tools/fetch_usd.py C:\\deps\\usd --packs-only --require-packs

Run again after a pin changed, it replaces the packs that are out of date: a
pack's usd-plugins/<pack>.json names its build and lists its files. An OpenUSD
that is out of date is refused, since nothing lists its files; pass a new folder.

--packs-only leaves OpenUSD alone, for a folder that already holds the asset
(build.yml and release.yml unzip it themselves). A pack that isn't published is
a warning, as deps-usd-plugins.yml may still be building it, and an error with
--require-packs. --optional also puts the published packs of the optional set
over it, which release.yml ships as downloads of their own, and those of the local
set whose pin says "publish": true (Arnold): built where their SDK is, not by
deps-usd-plugins.yml, and attached to the same release from there
(docs/ARNOLD_CI.md). --zips names a folder to look for the zips in before
downloading, such as one a local tools/build_usd_plugin.py build was zipped into.

They are release assets of a public repository, so this needs no login.
Standard library only; on Linux it unzips with unzip, which keeps the links.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
from _common import ASSET_PLATFORM, REPO, WINDOWS, fail, run, warn  # noqa: E402
import usd_asset  # noqa: E402
import usd_plugin_asset  # noqa: E402

DEFAULT_REPOSITORY = "olihey/usdcc"
MANIFEST_DIR = "usd-plugins"
# Which OpenUSD asset the folder holds: the asset itself doesn't say.
USD_MARKER = "usd-asset.txt"


def published(spec):
    """Whether the pin's pack is attached to the OpenUSD release. The optional set's is
    unless its pin says no; the local set's only when its pin says yes: nothing of an SDK
    is published by default."""
    if spec["set"] == "local":
        return spec.get("publish") is True
    return bool(spec.get("publish", True))


def standard_packs(optional=False):
    """The packs a usdcc release ships on this platform; with `optional`, also the
    published packs of the optional and the local set, which a release attaches as
    downloads of their own."""
    packs = []
    for file in sorted(usd_plugin_asset.PINS.glob("*.json")):
        spec = usd_plugin_asset.load(file.stem)
        if ASSET_PLATFORM not in spec["platforms"]:
            continue
        if spec["set"] == "standard" or (optional and spec["set"] in ("optional", "local") and published(spec)):
            packs.append(file.stem)
    return packs


def _notice(message):
    """A warning that a workflow run also shows on its summary page."""
    if os.environ.get("GITHUB_ACTIONS"):
        print(f"::warning::{message}", flush=True)
    else:
        warn(message)


def obtain(zip_name, tag, repository, zips, work):
    """The zip, from `zips` or downloaded into `work`; None if it isn't published."""
    if zips and (zips / zip_name).is_file():
        print(f"  {zip_name}: from {zips}", flush=True)
        return zips / zip_name
    url = f"https://github.com/{repository}/releases/download/{tag}/{zip_name}"
    target = work / zip_name
    print(f"  {zip_name}: downloading {url}", flush=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as response, target.open("wb") as output:
            total = int(response.headers.get("Content-Length") or 0)
            done = shown = 0
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
                done += len(chunk)
                if done - shown >= 50 * 1024 * 1024:
                    shown = done
                    print(f"    {done // 2**20} of {total // 2**20} MB", flush=True)
    except urllib.error.HTTPError as error:
        target.unlink(missing_ok=True)
        if error.code == 404:
            return None
        fail(f"Downloading {url} failed: {error}")
    except OSError as error:
        target.unlink(missing_ok=True)
        fail(f"Downloading {url} failed: {error}")
    return target


def unzip(zip_file, folder):
    folder.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        with zipfile.ZipFile(zip_file) as archive:
            archive.extractall(folder)
    else:
        # zipfile would write a link (libfoo.so -> libfoo.so.1) as a text file.
        run(["unzip", "-q", "-o", zip_file, "-d", folder])


def installed_pack(folder, pack):
    """The manifest of the pack in `folder`, or None."""
    manifest = folder / MANIFEST_DIR / f"{pack}.json"
    return json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else None


def remove_pack(folder, pack, manifest):
    """Deletes the files the pack's manifest lists, and the folders that empties."""
    for name in [*manifest["files"], f"{MANIFEST_DIR}/{pack}.json"]:
        file = folder / name
        if file.is_file():
            file.unlink()
        parent = file.parent
        while parent != folder and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent


def fetch_openusd(folder, repository, zips, work):
    asset = usd_asset.describe(REPO / "deps/usd.json")
    marker = folder / USD_MARKER
    if (folder / "pxrConfig.cmake").is_file():
        held = marker.read_text(encoding="utf-8").strip() if marker.is_file() else "one this script didn't put there"
        if held == asset["name"]:
            print(f"OpenUSD: {folder} has {asset['name']}")
            return
        fail(f"{folder} holds another OpenUSD ({held}), not {asset['name']}. Pass a new folder, or --packs-only "
             "to put the packs over this one.")
    if folder.is_dir() and any(folder.iterdir()):
        fail(f"{folder} isn't empty and holds no OpenUSD; pass an empty or new folder")
    print(f"OpenUSD: {asset['name']}")
    zip_file = obtain(asset["zip"], asset["tag"], repository, zips, work)
    if not zip_file:
        fail(f"No {asset['zip']} in release {asset['tag']} of {repository}. Run the OpenUSD workflow "
             "(deps-usd.yml) for the current deps/usd.json first.")
    unzip(zip_file, folder)
    if not (folder / "pxrConfig.cmake").is_file():
        fail(f"{asset['zip']} unzipped into {folder} holds no pxrConfig.cmake")
    marker.write_text(asset["name"] + "\n", encoding="utf-8", newline="\n")


def fetch_packs(folder, repository, zips, work, required, optional=False):
    """Puts every standard pack over the OpenUSD in `folder`, and with `optional` the
    published optional ones; returns the packs that aren't published."""
    missing = []
    for pack in standard_packs(optional):
        asset = usd_plugin_asset.describe(pack)
        installed = installed_pack(folder, pack)
        if installed and installed.get("asset") == asset["name"]:
            print(f"{pack}: {folder} has {asset['name']}")
            continue
        print(f"{pack}: {asset['name']}")
        zip_file = obtain(asset["zip"], asset["tag"], repository, zips, work)
        if not zip_file:
            missing.append(pack)
            if asset["set"] == "local":
                # Built where its SDK is, and attached from there (docs/ARNOLD_CI.md).
                message = (f"No {asset['zip']} in release {asset['tag']} of {repository}. Its SDK isn't here to build "
                           f"it with: run the pack's own workflow for the current pin (docs/ARNOLD_CI.md), which "
                           "attaches it.")
            else:
                message = (f"No {asset['zip']} in release {asset['tag']} of {repository}. Run the USD plugins workflow "
                           "(deps-usd-plugins.yml) for the current pins.")
            if required:
                fail(message)
            had = f" {folder} keeps the older {installed.get('asset')}." if installed else ""
            _notice(f"{message} Without it usdcc builds, but has no {pack}.{had}")
            continue
        if installed:
            print(f"  replacing {installed.get('asset')}")
            remove_pack(folder, pack, installed)
        unzip(zip_file, folder)
        if not installed_pack(folder, pack):
            fail(f"{asset['zip']} unzipped into {folder} holds no {MANIFEST_DIR}/{pack}.json")
    return missing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("folder", type=Path, help="where OpenUSD goes (then usdcc's --usd-install)")
    parser.add_argument("--packs-only", action="store_true",
                        help="the folder already holds the OpenUSD asset: only put the packs over it")
    parser.add_argument("--require-packs", action="store_true",
                        help="a pack that isn't published is an error, not a warning")
    parser.add_argument("--optional", action="store_true",
                        help="also the published packs of the optional set (MoonRay, Cycles) and of the local one "
                             "(Arnold), which a release ships as downloads of their own (tools/deploy.py "
                             "--optional-packs, --pack-folders)")
    parser.add_argument("--zips", type=Path, help="look for the zips in this folder before downloading")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY") or DEFAULT_REPOSITORY,
                        help=f"the GitHub repository whose releases hold them (default: {DEFAULT_REPOSITORY})")
    args = parser.parse_args(argv)

    folder = args.folder.expanduser().resolve()
    zips = args.zips.expanduser().resolve() if args.zips else None
    if zips and not zips.is_dir():
        fail(f"No such folder: {zips}")
    with tempfile.TemporaryDirectory(prefix="usdcc-fetch-") as work:
        if args.packs_only:
            if not (folder / "pxrConfig.cmake").is_file():
                fail(f"No OpenUSD in {folder} (no pxrConfig.cmake); run this without --packs-only to download it")
        else:
            fetch_openusd(folder, args.repository, zips, Path(work))
        missing = fetch_packs(folder, args.repository, zips, Path(work), args.require_packs, args.optional)
    without = f", without {', '.join(missing)}" if missing else ""
    print(f"{folder} is ready{without}. Build usdcc against it with: python tools/build.py --usd-install {folder}")


if __name__ == "__main__":
    main()
