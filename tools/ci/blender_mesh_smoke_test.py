"""Check an isolated packaged app, native backend, menu editing, and shutdown."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import REPO, WINDOWS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deploy", type=Path, required=True)
    parser.add_argument(
        "--artifacts", type=Path, default=REPO / "build/blender-packaged-smoke"
    )
    parser.add_argument(
        "--platform-plugins",
        type=Path,
        help="Qt offscreen platform plugin folder on Windows",
    )
    parser.add_argument("--omitted", action="store_true")
    args = parser.parse_args()
    root, work = args.deploy.resolve(), args.artifacts.resolve()
    work.mkdir(parents=True, exist_ok=True)
    result = work / "result.json"
    result.unlink(missing_ok=True)
    fixture = work / "mesh.usda"
    fixture.write_text(
        '#usda 1.0\ndef Mesh "Mesh" {\n token subdivisionScheme = "none"\n point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]\n int[] faceVertexCounts = [4]\n int[] faceVertexIndices = [0,1,2,3]\n}\n'
    )
    if WINDOWS:
        environment = {
            name: value
            for name, value in os.environ.items()
            if name.upper()
            in {
                "SYSTEMROOT",
                "WINDIR",
                "COMSPEC",
                "TEMP",
                "TMP",
                "NUMBER_OF_PROCESSORS",
                "PROCESSOR_ARCHITECTURE",
            }
        }
        environment["PATH"] = os.environ.get("SystemRoot", "C:/Windows") + "/System32"
        environment["APPDATA"] = str(work / "profile/roaming")
        environment["LOCALAPPDATA"] = str(work / "profile/local")
        if args.platform_plugins:
            environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(
                args.platform_plugins.resolve()
            )
    else:
        environment = {
            "PATH": "/usr/bin:/bin",
            "HOME": str(work / "profile/home"),
            "XDG_DATA_HOME": str(work / "profile/data"),
            "XDG_CONFIG_HOME": str(work / "profile/config"),
        }
    environment.update(
        {
            "QT_QPA_PLATFORM": "offscreen",
            "PYTHONNOUSERSITE": "1",
            "USDCC_TEST_PROFILE": "blender-" + uuid.uuid4().hex,
            "USDCC_BLENDER_SMOKE_ROOT": str(root),
            "USDCC_BLENDER_SMOKE_WORK": str(work),
            "USDCC_BLENDER_SMOKE_OMITTED": "1" if args.omitted else "0",
        }
    )
    exe = root / "bin" / ("usdcc.exe" if WINDOWS else "usdcc")
    startup = None
    if WINDOWS:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
    with (
        (work / "stdout.log").open("wb") as stdout,
        (work / "stderr.log").open("wb") as stderr,
    ):
        process = subprocess.Popen(
            [
                str(exe),
                "--no-startup-scripts",
                "--script",
                str(REPO / "tests/blender_mesh_packaged_smoke.py"),
                str(fixture),
            ],
            cwd=work,
            env=environment,
            stdout=stdout,
            stderr=stderr,
            stdin=subprocess.DEVNULL,
            startupinfo=startup,
        )
        try:
            code = process.wait(timeout=180)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError(f"Packaged mesh check timed out; see {work}") from None
    checked = json.loads(result.read_text()) if result.is_file() else {}
    if code or not checked.get("passed"):
        raise RuntimeError(
            f"Packaged mesh check failed ({code}): {checked.get('error', 'no result')}; see {work}"
        )
    print(json.dumps(checked, indent=2))


if __name__ == "__main__":
    main()
