"""Measure USD snapshot, native conversion/operation, commit and process memory."""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
from pathlib import Path

from _common import REPO, WINDOWS, cache_value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", default="RelWithDebInfo")
    parser.add_argument(
        "--faces", nargs="+", type=int, default=[10000, 100000, 1000000]
    )
    parser.add_argument(
        "--output", type=Path, default=REPO / "build/blender-mesh-benchmark.json"
    )
    args = parser.parse_args()
    build = REPO / "build"
    folder = build / "tests"
    if cache_value(build, "CMAKE_CONFIGURATION_TYPES"):
        folder /= args.configuration
    exe = folder / (
        "usdcc_blender_mesh_benchmark.exe"
        if WINDOWS
        else "usdcc_blender_mesh_benchmark"
    )
    usd = Path(cache_value(build, "USD_INSTALL"))
    environment = dict(os.environ)
    variable = "PATH" if WINDOWS else "LD_LIBRARY_PATH"
    paths = [usd / "bin", usd / "lib"]
    if WINDOWS:
        paths.append(build / "vcpkg_installed/x64-windows/bin")
    environment[variable] = os.pathsep.join(
        [*(str(p) for p in paths), environment.get(variable, "")]
    )
    environment["PXR_PLUGINPATH_NAME"] = str(usd / "plugin/usd")
    report = {
        "platform": platform.platform(),
        "processor": platform.processor(),
        "configuration": args.configuration,
        "operation": "Recalculate Normals on an open triangular grid; includes USD authoring/verification",
        "cases": [],
    }
    for count in args.faces:
        result = subprocess.run(
            [str(exe), str(count)],
            env=environment,
            capture_output=True,
            text=True,
            timeout=900,
            check=True,
        )
        row = json.loads(
            next(
                line
                for line in reversed(result.stdout.splitlines())
                if line.startswith("{")
            )
        )
        report["cases"].append(row)
        print(json.dumps(row), flush=True)
    cancelled = subprocess.run(
        [str(exe), str(max(args.faces)), "cancel"],
        env=environment,
        capture_output=True,
        text=True,
        timeout=900,
        check=True,
    )
    report["cancellation"] = json.loads(
        next(
            line
            for line in reversed(cancelled.stdout.splitlines())
            if line.startswith("{")
        )
    )
    print(json.dumps(report["cancellation"]), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
