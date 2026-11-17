"""Builds a USD plugin as deps/usd-plugins/<pack>.json pins it, into a pack.

A pack is a folder laid out like the OpenUSD install it was built against
(plugin/usd/..., bin/ for the DLLs the plugins need), holding only what runs:
unzipped or copied over that install, OpenUSD finds the plugins with no further
setup (docs/USD_PLUGINS.md). .github/workflows/deps-usd-plugins.yml runs this and
publishes the zip; locally it's the same build.

    python tools/build_usd_plugin.py adobe-fileformats --usd-install C:\\deps\\usd
    python tools/build_usd_plugin.py adobe-fileformats --usd-install C:\\deps\\usd --install

Fetches the pinned commit, applies the pin's patches, configures with CMake
against the OpenUSD install (Release, the release CRT, the pin's Python), builds
and installs. Then it drops what only a build needs (headers, import libraries,
CMake files), collects the licences of the plugin and of what CMake fetched into
it under licenses/usd-plugins/<pack>/, and writes usd-plugins/<pack>.json, which
lists the pack's files.

--install copies the pack over the OpenUSD install afterwards. Check a pack with
tools/ci/check_usd_plugin.py.

Standard library only.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
from _common import ASSET_PLATFORM, REPO, WINDOWS, copy_tree, fail, remove_tree, run  # noqa: E402
from build_usd import fetch  # noqa: E402
import usd_asset  # noqa: E402
import usd_licenses  # noqa: E402
import usd_plugin_asset  # noqa: E402

# What a build leaves in an install that nothing needs at run time.
BUILD_ONLY_DIRS = ["include", "cmake", "lib/cmake", "lib/pkgconfig"]
BUILD_ONLY_FILES = ["*.lib", "*.exp", "*.ilk", "*.pdb", "*.a", "*.cmake", "*.pc"]

MANIFEST_DIR = "usd-plugins"


def manifest_path(pack_dir, pack):
    return Path(pack_dir) / MANIFEST_DIR / f"{pack}.json"


def pack_files(pack_dir):
    """Every file in the pack, as sorted paths relative to it with forward slashes."""
    pack_dir = Path(pack_dir)
    return sorted(file.relative_to(pack_dir).as_posix() for file in pack_dir.rglob("*") if file.is_file())


def _empty_pack_dir(pack_dir, pack):
    """An empty `pack_dir`. Only ever deletes an earlier build of the same pack."""
    if pack_dir.is_dir() and any(pack_dir.iterdir()):
        if not manifest_path(pack_dir, pack).is_file():
            fail(f"{pack_dir} isn't empty and isn't an earlier build of this pack; pass an empty folder as --pack")
        remove_tree(pack_dir)
    pack_dir.mkdir(parents=True, exist_ok=True)


def _prune(pack_dir):
    """Drops the build-only files and the folders that leaves empty."""
    for folder in BUILD_ONLY_DIRS:
        if (pack_dir / folder).is_dir():
            remove_tree(pack_dir / folder)
    for file in list(pack_dir.rglob("*")):
        if file.is_file() and any(fnmatch.fnmatch(file.name, pattern) for pattern in BUILD_ONLY_FILES):
            file.unlink()
    for folder, _, _ in os.walk(pack_dir, topdown=False):
        if Path(folder) != pack_dir and not any(Path(folder).iterdir()):
            Path(folder).rmdir()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("pack", help="the pack to build, as in deps/usd-plugins/<pack>.json")
    parser.add_argument("--usd-install", type=Path, required=True, help="the OpenUSD install to build against")
    parser.add_argument("--work", type=Path,
                        help="for the checkout and the build tree (default: build/usd-plugins/<pack>)")
    parser.add_argument("--pack-dir", type=Path, help="where the pack goes (default: <work>/pack)")
    parser.add_argument("--install", action="store_true", help="also copy the pack over the OpenUSD install")
    parser.add_argument("--jobs", type=int, help="parallel jobs (default: the build tool's)")
    args = parser.parse_args(argv)

    pack = args.pack
    spec = usd_plugin_asset.load(pack)
    if ASSET_PLATFORM not in spec["platforms"]:
        fail(f"The {pack} pack isn't set up for {ASSET_PLATFORM} (its pin lists {', '.join(spec['platforms'])})")

    usd_pin = json.loads((REPO / "deps/usd.json").read_text(encoding="utf-8-sig"))
    wanted = usd_pin["python"].split(".")[:2]
    if [str(part) for part in sys.version_info[:2]] != wanted:
        fail(f"deps/usd.json pins Python {usd_pin['python']}; run this with a Python {'.'.join(wanted)} "
             f"(this is {sys.version.split()[0]} at {sys.executable})")

    usd = args.usd_install.expanduser().resolve()
    if not (usd / "pxrConfig.cmake").is_file():
        fail(f"No OpenUSD install at {usd} (no pxrConfig.cmake)")
    work = (args.work.expanduser() if args.work else REPO / "build" / "usd-plugins" / pack).resolve()
    pack_dir = (args.pack_dir.expanduser() if args.pack_dir else work / "pack").resolve()
    source = work / "src" / pack  # alone in src/, which is how usd_licenses.collect() names it
    build = work / "build"

    fetch(spec, usd_plugin_asset.PINS, source)
    _empty_pack_dir(pack_dir, pack)

    places = {"usd": usd.as_posix(), "pack": pack_dir.as_posix()}
    command = ["cmake", "-S", source, "-B", build]
    if WINDOWS:
        # The toolset OpenUSD and usdcc are built with (docs/CI.md section 2).
        command += ["-G", "Visual Studio 17 2022", "-A", "x64"]
    else:
        command += ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    command += [
        # On the prefix path, not just a hint for pxr: pxrConfig.cmake finds some of
        # its own dependencies there with plain find_dependency() calls.
        f"-DCMAKE_PREFIX_PATH={places['usd']}",
        f"-DCMAKE_INSTALL_PREFIX={places['pack']}",
        # The prebuilt OpenUSD is Release-only, so its plugins use the release CRT.
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
        # pxrConfig.cmake looks for the pin's Python (its libraries, not the
        # interpreter): the one this runs on, not whichever CMake finds first.
        f"-DPython3_ROOT_DIR={Path(sys.base_prefix).as_posix()}",
        *[argument.format(**places) for argument in spec["cmake"]],
    ]
    print(f"Configuring {pack}:", " ".join(str(part) for part in command), flush=True)
    run(command)
    run(["cmake", "--build", build, "--config", "Release", "--parallel", *([str(args.jobs)] if args.jobs else [])])
    run(["cmake", "--install", build, "--config", "Release", "--prefix", pack_dir])

    _prune(pack_dir)
    if not pack_files(pack_dir):
        fail(f"The {pack} build installed nothing that runs into {pack_dir}")

    licences = pack_dir / "licenses" / MANIFEST_DIR / pack
    found = usd_licenses.collect(work / "src", build, licences)
    if pack not in found:
        fail(f"No licence file at the top of {source}")
    print(f"Licences: {', '.join(found)}")

    files = pack_files(pack_dir)
    manifest = manifest_path(pack_dir, pack)
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({
        "pack": pack,
        "repository": spec["repository"],
        "commit": spec["commit"],
        "version": spec.get("version", ""),
        "license": spec["license"],
        "set": spec["set"],
        "usd": usd_asset.describe(REPO / "deps/usd.json")["base"],
        "platform": ASSET_PLATFORM,
        "files": files,
    }, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"The {pack} pack is in {pack_dir}: {len(files)} files.")

    if args.install:
        copy_tree(pack_dir, usd)
        print(f"Copied over {usd}.")


if __name__ == "__main__":
    main()
