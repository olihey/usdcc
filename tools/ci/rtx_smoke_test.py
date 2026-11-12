"""Validates a packaged native RTX plugin with an isolated profile and Windows-only PATH.

--mode Render requires --bundle-rtx-runtime in deploy.py and an RTX GPU.
MissingRuntime checks the actionable error without a GPU; Omitted checks a deploy
made with --omit-rtx. Results and SDK caches remain in --artifacts; result.json
names Qt's isolated profile.

    python tools/ci/rtx_smoke_test.py --deploy build/rtx-package

Windows only so far, like the deploy it checks.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import REPO, copy_tree, fail, require_windows  # noqa: E402

# What a developer's shell may have set, and the test must not inherit.
CLEARED = (
    "APPDATA", "LOCALAPPDATA", "PYTHONHOME", "PYTHONPATH", "PYTHONNOUSERSITE", "PXR_PLUGINPATH_NAME",
    "QT_PLUGIN_PATH", "QT_QPA_PLATFORM", "QT_QPA_PLATFORM_PLUGIN_PATH", "USDCC_RTX_OVRTX_ROOT",
    "USDCC_RTX_OVSTAGE_ROOT", "USDCC_RTX_VULKAN_VALIDATION", "USDCC_RTX_SMOKE_ROOT", "USDCC_RTX_SMOKE_WORK",
    "USDCC_RTX_SMOKE_MODE", "OMNICLIENT_HUB_MODE", "USDCC_TEST_PROFILE", "USDCC_PLUGIN_PATH", "USDCC_PYTHON_PATH")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--deploy", required=True, help="the deploy to check")
    parser.add_argument("--mode", choices=("Render", "MissingRuntime", "Omitted"), default="Render")
    parser.add_argument("--artifacts", default="build/rtx-packaged-smoke", help="where the results go")
    parser.add_argument("--timeout-seconds", type=int, default=660)
    args = parser.parse_args(argv)
    require_windows("tools/ci/rtx_smoke_test.py")

    deploy_dir = os.path.abspath(args.deploy)
    if not os.path.isdir(deploy_dir):
        fail(f"No deploy at {deploy_dir}")
    work = Path(os.path.abspath(args.artifacts))
    for folder in (work, work / "profile/local", work / "profile/roaming"):
        folder.mkdir(parents=True, exist_ok=True)
    script = REPO / "tests/rtx_packaged_smoke.py"
    fixture = REPO / "test_data/rtx_native_m1.usda"
    for file in (script, fixture):
        if not file.is_file():
            fail(f"Missing {file}")
    result = work / "result.json"
    for name in ("result.json", "progress.json", "clean.png"):
        if (work / name).is_file():
            (work / name).unlink()

    environment = {name: value for name, value in os.environ.items() if name.upper() not in CLEARED}
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    environment.update({
        "PATH": f"{system_root}\\System32;{system_root};{system_root}\\System32\\Wbem",
        "APPDATA": str(work / "profile/roaming"),
        "LOCALAPPDATA": str(work / "profile/local"),
        "PYTHONNOUSERSITE": "1",
        "OMNICLIENT_HUB_MODE": "disabled",
        "USDCC_TEST_PROFILE": "rtx-" + uuid.uuid4().hex,
        "USDCC_RTX_SMOKE_ROOT": deploy_dir,
        "USDCC_RTX_SMOKE_WORK": str(work),
        "USDCC_RTX_SMOKE_MODE": args.mode,
    })

    hidden = subprocess.STARTUPINFO()
    hidden.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    hidden.wShowWindow = subprocess.SW_HIDE
    with open(work / "stdout.log", "wb") as stdout, open(work / "stderr.log", "wb") as stderr:
        process = subprocess.Popen(
            [os.path.join(deploy_dir, "bin", "usdcc.exe"), "--no-startup-scripts", "--script", str(script), str(fixture)],
            cwd=work, env=environment, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, startupinfo=hidden)
        try:
            process.wait(timeout=args.timeout_seconds)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            fail(f"Packaged RTX timed out after {args.timeout_seconds} seconds; see {work}.")
    if process.returncode != 0:
        fail(f"Packaged app exited with {process.returncode}; see {work}.")
    if not result.is_file():
        fail(f"No packaged RTX result; see {work}.")
    checked = json.loads(result.read_text(encoding="utf-8-sig"))
    logs = Path(checked.get("profile_data") or "") / "logs"
    if checked.get("profile_data") and logs.is_dir():
        copy_tree(logs, work / "app-logs")
    if not checked.get("passed"):
        fail(f"Packaged RTX failed: {checked.get('error')}")
    print(f"Packaged RTX {args.mode} passed; results: {result}")


if __name__ == "__main__":
    main()
