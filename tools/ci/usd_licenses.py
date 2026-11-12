"""Collects the licences of OpenUSD's dependencies from build_usd.py's folders.

build_usd.py installs OpenUSD's own LICENSE and THIRD-PARTY.md, but not the
licences of most of the libraries it builds (TBB, OpenImageIO, OpenColorIO,
MaterialX, OpenSubdiv, Boost, ...). Those are in their source folders, and the
small libraries OpenColorIO and OpenEXR bundle are in the build folder. This
copies each one's licence files into <destination>/<library>/, for the release
zip (docs/RELEASE.md section 6).

deps-usd.yml runs it before deleting the source and build folders, into the
install's licenses folder; tools/deploy.py runs it for a local OpenUSD install
that still has them.

    python tools/ci/usd_licenses.py --src C:\\b\\src --build C:\\b\\build --destination C:\\deps\\usd\\licenses

--src is build_usd.py's source folder (its --src, or <install>/src), --build its
build folder (its --build, or <install>/build), which is optional.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import copy_file  # noqa: E402

# Licence texts: LICENSE, COPYING.txt, LICENSE-NVIDIA, ..., not code such as
# Boost's licence_info.cpp.
_LICENCE_NAME = re.compile(r"^(LICEN[SC]E|COPYING|NOTICE|COPYRIGHT)", re.IGNORECASE)
_CODE_SUFFIX = re.compile(r"^\.(c|cc|cpp|h|hpp|py|xml|rtf|html?|cmake|in)$", re.IGNORECASE)
_NOT_THE_LIBRARY = re.compile(r"[\\/](tests?|docs?|examples?)[\\/]", re.IGNORECASE)


def _is_licence(file):
    return (bool(_LICENCE_NAME.match(file.name)) and not _CODE_SUFFIX.match(file.suffix)
            and not _NOT_THE_LIBRARY.search(str(file)))


def _walk(folder, depth):
    """`folder` and its subfolders down to `depth` levels, each with its files."""
    try:
        with os.scandir(folder) as entries:
            entries = sorted(entries, key=lambda entry: entry.name.upper())
    except OSError:
        return
    yield Path(folder), [Path(entry.path) for entry in entries if entry.is_file()]
    if depth > 0:
        for entry in entries:
            if entry.is_dir():
                yield from _walk(entry.path, depth - 1)


def _copy_licences(source, destination, depth=0):
    """Copies the licence files in `source` into `destination`; returns how many."""
    files = [file for _, files in _walk(source, depth) for file in files if _is_licence(file)]
    if files:
        destination.mkdir(parents=True, exist_ok=True)
    for file in files:
        copy_file(file, destination / file.name)
    return len(files)


def collect(src, build, destination):
    """Copies the licences into `destination`; returns the libraries they are of."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    found = {}  # library, lower case -> as its folder spells it

    # Each extracted source: a folder per library, its licence at the top. The
    # OpenUSD checkout itself is installed with its own LICENSE.
    for folder in sorted(Path(src).iterdir(), key=lambda entry: entry.name.upper()):
        if not folder.is_dir() or folder.name.upper().startswith("OPENUSD"):
            continue
        if _copy_licences(folder, destination / folder.name):
            found[folder.name.lower()] = folder.name

    # Libraries the others build and link in themselves: OpenColorIO's
    # ext/build/<library> and CMake's FetchContent _deps/<library>-src.
    if build and Path(build).is_dir():
        for parent, _ in _walk(build, 3):
            for bundled in sorted(parent.iterdir(), key=lambda entry: entry.name.upper()):
                if not bundled.is_dir():
                    continue
                name = None
                if parent.name.lower() == "build" and parent.parent.name.lower() == "ext":
                    name = bundled.name
                elif parent.name.lower() == "_deps" and bundled.name.lower().endswith("-src"):
                    name = bundled.name[:-4]
                if not name or name.lower() in found:
                    continue
                if _copy_licences(bundled, destination / name, depth=3):
                    found[name.lower()] = name
    return list(found.values())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--src", required=True, help="build_usd.py's source folder")
    parser.add_argument("--build", help="build_usd.py's build folder")
    parser.add_argument("--destination", required=True, help="where the licences go")
    args = parser.parse_args(argv)

    found = collect(args.src, args.build, args.destination)
    print(f"Licences of {len(found)} of OpenUSD's dependencies: {', '.join(found)}")


if __name__ == "__main__":
    main()
