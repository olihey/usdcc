"""Names the prebuilt PySide6 release asset for the current Qt, PySide tag and
Python.

tools/build_pyside.py builds PySide6/shiboken6 against vcpkg's Qt, so the asset
has to match the Qt usdcc builds against (docs/CI.md, phase 4). The name carries
the PySide tag and the Qt version, and a hash of those with qtbase's vcpkg port
version, the Python version (deps/usd.json), and this script and build_pyside.py
themselves, so a change to any of them gives a new asset. Line endings are
normalised before hashing.

Prints the result as JSON, and with --github-output also appends it to
$GITHUB_OUTPUT for later workflow steps:
  name    pyside-<tag>-qt<qt version>-<hash>-win64-py312
  tag     deps/pyside-<tag>-qt<qt version>-<hash>
  zip     <name>.zip
  pyside, qt, python

    python tools/ci/pyside_asset.py
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import ASSET_PLATFORM, REPO, fail, normalized_text, report, short_hash  # noqa: E402


def describe():
    """The asset for this checkout, as report() takes it."""
    build_script = REPO / "tools/build_pyside.py"
    build_text = normalized_text(build_script)

    # The PySide tag build_pyside.py builds.
    match = re.search(r'^PYSIDE_TAG = "([^"]+)"', build_text, re.MULTILINE)
    if not match:
        fail(f"No PYSIDE_TAG in {build_script}")
    pyside_tag = match.group(1)

    # qtbase as vcpkg builds it: an overlay port in cmake/ports wins over vcpkg's own.
    qtbase_port = REPO / "cmake/ports/qtbase/vcpkg.json"
    if not qtbase_port.is_file():
        qtbase_port = REPO / "external/vcpkg/ports/qtbase/vcpkg.json"
    if not qtbase_port.is_file():
        fail(f"No qtbase port at {qtbase_port} (is the vcpkg submodule checked out?)")
    qtbase = json.loads(qtbase_port.read_text(encoding="utf-8-sig"))
    qt_version = qtbase.get("version") or qtbase.get("version-semver") or qtbase.get("version-string")
    qt = f"{qt_version}#{qtbase.get('port-version', 0)}"

    usd = json.loads((REPO / "deps/usd.json").read_text(encoding="utf-8-sig"))
    python = usd["python"].split(".")

    inputs = f"pyside={pyside_tag}\nqt={qt}\npython={usd['python']}\n" + build_text + normalized_text(__file__)
    base = f"pyside-{pyside_tag.lstrip('v')}-qt{qt_version}-{short_hash(inputs, 4)}"
    name = f"{base}-{ASSET_PLATFORM}-py{python[0]}{python[1]}"
    return {
        "name": name,
        "tag": f"deps/{base}",
        "zip": f"{name}.zip",
        "pyside": pyside_tag,
        "qt": qt,
        "python": usd["python"],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--github-output", action="store_true", help="also append the result to $GITHUB_OUTPUT")
    args = parser.parse_args(argv)
    report(describe(), github_output=args.github_output)


if __name__ == "__main__":
    main()
