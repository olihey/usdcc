"""Checks a build made with `tools/build.py --without all --without-rtx`: that it has no
optional feature (build/features.json), and that its usdcc_node starts and lists each as
left out (docs/BUILD_OPTIONS.md). Nothing is deployed, so USD's libraries come from its install.

    python tools/ci/check_features_left_out.py --usd-install C:\\deps\\usd
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import EXE_SUFFIX, MACOS, REPO, WINDOWS, cache_value, fail  # noqa: E402

BUILD_DIR = REPO / "build"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--usd-install", required=True, metavar="DIR")
    parser.add_argument("--configuration", default="RelWithDebInfo")
    args = parser.parse_args()

    features = json.loads((BUILD_DIR / "features.json").read_text(encoding="utf-8"))["features"]
    if not features:
        fail("build/features.json names no feature.")
    included = [feature["label"] for feature in features if feature["included"]]
    if included:
        fail("The build should have no optional feature, but has: " + ", ".join(included))

    folder = BUILD_DIR / "src/cpp/app"
    if cache_value(BUILD_DIR, "CMAKE_CONFIGURATION_TYPES"):
        folder = folder / args.configuration
    environment = dict(os.environ)
    variable = "PATH" if WINDOWS else "DYLD_LIBRARY_PATH" if MACOS else "LD_LIBRARY_PATH"
    folders = [os.path.join(args.usd_install, name) for name in (("bin", "lib") if WINDOWS else ("lib",))]
    environment[variable] = os.pathsep.join(folders + [environment.get(variable, "")]).rstrip(os.pathsep)

    try:
        node = subprocess.run([str(folder / f"usdcc_node{EXE_SUFFIX}"), "--version", "--no-gpu"], env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    except subprocess.TimeoutExpired:
        fail("usdcc_node --version didn't finish in two minutes.")
    output = node.stdout.decode("utf-8", "replace")
    print(output, flush=True)
    if node.returncode:
        fail(f"usdcc_node --version failed with exit code {node.returncode}")
    lines = output.splitlines()
    unlisted = [feature["label"] for feature in features
                if not any(feature["label"] in line and "Left out of this build" in line for line in lines)]
    if unlisted:
        fail("usdcc_node --version should list as left out: " + ", ".join(unlisted))
    print(f"{len(features)} features left out")


if __name__ == "__main__":
    main()
