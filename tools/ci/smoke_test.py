"""Starts a deploy (tools/deploy.py) the way a user would, and checks it.

Runs <deploy>\\bin\\usdcc.exe with a PATH of Windows' own folders only, on Qt's
offscreen platform, with a script that records usdcc's version and where pxr,
usdcc and PySide6 were loaded from, then quits the app. Fails if the app doesn't
get there, if the version isn't --expect-version, or if anything came from
outside the deploy: a DLL or Python file the build's own folders were hiding
(docs/RELEASE.md section 4).

The deploy only has Qt's windows platform plugin; the offscreen one comes from
--platform-plugins (vcpkg's Qt6\\plugins\\platforms).

    python tools/ci/smoke_test.py --deploy deploy --expect-version 0.2.0
        --platform-plugins build\\vcpkg_installed\\x64-windows\\Qt6\\plugins\\platforms

Windows only so far, like the deploy it checks.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import fail, remove_tree, require_windows  # noqa: E402

CHECK = """\
import json
import sys

from PySide6 import QtCore, QtWidgets
import PySide6
import pxr
from pxr import Usd
import usdcc
import usdcc.app

result = {
    "version": usdcc.app.version(),
    "release": usdcc.app.is_release_build(),
    "usd": ".".join(str(part) for part in Usd.GetVersion()),
    "python": sys.version.split()[0],
    "pyside": PySide6.__version__,
    "files": {"pxr": pxr.__file__, "usdcc": usdcc.__file__, "PySide6": PySide6.__file__},
}
with open(RESULT, "w") as f:
    json.dump(result, f)
QtCore.QTimer.singleShot(0, QtWidgets.QApplication.quit)
"""


def _print_latest_log(started):
    """The end of the log the app wrote, to say why it failed."""
    folder = Path(os.environ.get("LOCALAPPDATA", "")) / "usdcc/logs"
    if not folder.is_dir():
        return
    logs = [file for file in folder.iterdir() if file.is_file() and file.stat().st_mtime >= started - 5]
    if logs:
        log = max(logs, key=lambda file: file.stat().st_mtime)
        print(f"--- {log}")
        print("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[-60:]))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--deploy", required=True, help="the deploy to start")
    parser.add_argument("--expect-version", help="the version the app has to report")
    parser.add_argument("--platform-plugins", required=True, help="a folder holding Qt's qoffscreen.dll")
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args(argv)
    require_windows("tools/ci/smoke_test.py")

    deploy_dir = os.path.abspath(args.deploy)
    exe = os.path.join(deploy_dir, "bin", "usdcc.exe")
    if not os.path.isfile(exe):
        fail(f"No usdcc.exe in {deploy_dir}\\bin")
    platform_plugins = os.path.abspath(args.platform_plugins)
    if not os.path.isfile(os.path.join(platform_plugins, "qoffscreen.dll")):
        fail(f"No qoffscreen.dll in {args.platform_plugins}")

    work = Path(tempfile.gettempdir()) / f"usdcc-smoke-{os.getpid()}"
    work.mkdir(parents=True, exist_ok=True)
    result = work / "result.json"
    script = work / "check.py"
    script.write_text(f"RESULT = {str(result)!r}\n" + CHECK, encoding="utf-8")

    # Only what a user's machine has: Windows, and the deploy itself.
    environment = dict(os.environ)
    for name in ("PYTHONHOME", "PYTHONPATH", "PXR_PLUGINPATH_NAME", "QT_PLUGIN_PATH"):
        environment.pop(name, None)
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    environment["PATH"] = f"{system_root}\\System32;{system_root};{system_root}\\System32\\Wbem"
    environment["QT_QPA_PLATFORM"] = "offscreen"
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = platform_plugins

    problems = []
    started = time.time()
    process = subprocess.Popen([exe, "--no-startup-scripts", "--script", str(script)], env=environment)
    try:
        if process.wait(timeout=args.timeout_seconds) != 0:
            problems.append(f"usdcc exited with {process.returncode}")
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        problems.append(f"usdcc didn't quit within {args.timeout_seconds} s")

    if result.is_file():
        checked = json.loads(result.read_text(encoding="utf-8"))
        print(f"usdcc {checked['version']} (release: {checked['release']}), OpenUSD {checked['usd']}, "
              f"Python {checked['python']}, PySide6 {checked['pyside']}")
        if args.expect_version and checked["version"] != args.expect_version:
            problems.append(f"the version is {checked['version']}, not {args.expect_version}")
        for module, file in checked["files"].items():
            file = os.path.abspath(file)
            print(f"  {module}: {file}")
            if not os.path.normcase(file).startswith(os.path.normcase(deploy_dir)):
                problems.append(f"{module} came from outside the deploy: {file}")
    else:
        problems.append("the check script wrote nothing")
    try:
        remove_tree(work)
    except OSError:
        pass

    if problems:
        _print_latest_log(started)
        fail(f"The deploy's smoke test failed: {'; '.join(problems)}")
    print("The deploy starts and runs from its own folder.")


if __name__ == "__main__":
    main()
