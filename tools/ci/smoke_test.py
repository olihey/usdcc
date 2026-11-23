"""Starts a deploy (tools/deploy.py) the way a user would, and checks it.

Runs <deploy>\\bin\\usdcc.exe with a PATH of Windows' own folders only, on Qt's
offscreen platform, with a script that records usdcc's version and where pxr,
usdcc and PySide6 were loaded from, then quits the app. Fails if the app doesn't
get there, if the version isn't --expect-version, or if anything came from
outside the deploy: a DLL or Python file the build's own folders were hiding
(docs/RELEASE.md section 4).

The same script asks OpenUSD for its own render delegates (Storm, Embree) and
for the plugins of each USD plugin pack the deploy has (its usd-plugins/*.json),
and does what the pack's pin asks under "check": the plugins load from the
deploy, the extensions have file formats, the render delegates are listed, the
sample files open
(docs/USD_PLUGINS.md). --expect-standard-usd-packs fails a deploy that lacks a
pack of the standard set, which a release has to have.

The deploy's render node, usdcc_node, has to start too, say the same version, and list
Embree among its renderers with the plugins loaded, and end without a crash.

On Windows the deploy only has Qt's windows platform plugin; the offscreen one
comes from --platform-plugins (vcpkg's Qt6\\plugins\\platforms). A Linux deploy
has its own, and the app starts with no more than PATH=/usr/bin:/bin and HOME: no
LD_LIBRARY_PATH, nothing of the build's. PySide6 is required,
unless --without-pyside: a panel made with usdcc.ui docks, and on Linux the process has
one Qt, the deploy's.

    python tools/ci/smoke_test.py --deploy deploy --expect-version 0.2.0
        --platform-plugins build\\vcpkg_installed\\x64-windows\\Qt6\\plugins\\platforms
    python3 tools/ci/smoke_test.py --deploy ~/usdcc-0.2.0 --expect-version 0.2.0
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
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import ASSET_PLATFORM, REPO, WINDOWS, fail, remove_tree  # noqa: E402
import usd_plugin_asset  # noqa: E402

# OpenUSD's own render delegates, which every deploy has (tools/deploy.py).
OPENUSD_PLUGINS = ["hdStorm", "hdEmbree"]

CHECK = """\
import json
import sys

try:
    from PySide6 import QtCore, QtWidgets
    import PySide6
except ImportError:
    if NEED_PYSIDE:
        raise
    PySide6 = None
import pxr
from pxr import Usd
import usdcc
import usdcc.app

result = {
    "version": usdcc.app.version(),
    "release": usdcc.app.is_release_build(),
    "usd": ".".join(str(part) for part in Usd.GetVersion()),
    "python": sys.version.split()[0],
    "pyside": PySide6.__version__ if PySide6 else None,
    "files": {"pxr": pxr.__file__, "usdcc": usdcc.__file__},
}
if PySide6:
    result["files"]["PySide6"] = PySide6.__file__
    import usdcc.ui

    class SmokePanel(usdcc.ui.SidePanel):
        def __init__(self):
            super().__init__("Smoke Test")

    usdcc.app.add_side_panel(SmokePanel())
    result["main_window"] = isinstance(usdcc.app.main_window(), QtWidgets.QMainWindow)
# Each Qt Core the process has mapped: one, or Python's Qt isn't the app's.
if sys.platform == "linux":
    with open("/proc/self/maps") as maps:
        result["qt"] = sorted({line.split(None, 5)[5].strip() for line in maps if "libQt6Core.so" in line})

# OpenUSD's plugins: its own render delegates, and what each USD plugin pack in
# the deploy is checked for (its pin's "check", docs/USD_PLUGINS.md).
from pxr import Plug, Sdf, Tf

def plugin(name):
    found = Plug.Registry().GetPluginWithName(name)
    if not found:
        return None
    try:
        loaded = bool(found.Load())
    except Exception:
        loaded = False
    return {"path": found.path, "loaded": loaded}

def prim_counts(file):
    try:
        stage = Usd.Stage.Open(file)
    except Exception:
        stage = None
    if not stage:
        return None
    counts = {}
    for prim in stage.Traverse():
        counts[prim.GetTypeName()] = counts.get(prim.GetTypeName(), 0) + 1
    return counts

result["plugins"] = {name: plugin(name) for name in PLUGINS}
result["extensions"] = {extension: Sdf.FileFormat.FindByExtension(extension) is not None for extension in EXTENSIONS}
# The render delegates OpenUSD lists, from the plugins' own descriptions.
result["renderers"] = sorted(
    found.typeName for found in Plug.Registry.GetAllDerivedTypes(Tf.Type.FindByName("HdRendererPlugin")))
result["opened"] = {file: prim_counts(file) for file in FILES}
with open(RESULT, "w") as f:
    json.dump(result, f)
if PySide6:
    QtCore.QTimer.singleShot(0, QtWidgets.QApplication.quit)
else:
    # No way to ask the app to quit without PySide6: it has started and run this, which is what is checked.
    import os

    os._exit(0)
"""


def _print_latest_log(started):
    """The end of the log the app wrote, to say why it failed."""
    if WINDOWS:
        folder = Path(os.environ.get("LOCALAPPDATA", "")) / "usdcc_config/logs"
    else:
        folder = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "usdcc/logs"
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
    parser.add_argument("--platform-plugins", required=WINDOWS,
                        help="a folder holding Qt's qoffscreen.dll (Windows; a Linux deploy has its own)")
    parser.add_argument("--expect-standard-usd-packs", action="store_true",
                        help="the deploy has to have every USD plugin pack of the standard set for this platform "
                             "(deps/usd-plugins); without it, only the packs it has are checked")
    parser.add_argument("--without-pyside", action="store_true", help="the deploy may lack PySide6")
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args(argv)

    deploy_dir = os.path.abspath(args.deploy)
    exe = os.path.join(deploy_dir, "bin", "usdcc.exe" if WINDOWS else "usdcc")
    if not os.path.isfile(exe):
        fail(f"No {os.path.basename(exe)} in {os.path.join(deploy_dir, 'bin')}")
    platform_plugins = None
    if WINDOWS:
        platform_plugins = os.path.abspath(args.platform_plugins)
        if not os.path.isfile(os.path.join(platform_plugins, "qoffscreen.dll")):
            fail(f"No qoffscreen.dll in {args.platform_plugins}")

    # The USD plugin packs the deploy says it has (tools/deploy.py writes a file
    # each), and what each one's pin wants checked.
    problems = []
    packs_dir = Path(deploy_dir) / "usd-plugins"
    packs = sorted(file.stem for file in packs_dir.glob("*.json")) if packs_dir.is_dir() else []
    if args.expect_standard_usd_packs:
        for pack in sorted(file.stem for file in usd_plugin_asset.PINS.glob("*.json")):
            spec = usd_plugin_asset.load(pack)
            if spec["set"] == "standard" and ASSET_PLATFORM in spec["platforms"] and pack not in packs:
                problems.append(f"the deploy doesn't have the {pack} USD plugin pack")
    pack_plugins, extensions, files, renderers = {}, [], {}, {}
    for pack in packs:
        check = usd_plugin_asset.load(pack).get("check", {})
        pack_plugins.update({name: pack for name in check.get("plugins", [])})
        extensions += check.get("extensions", [])
        renderers.update({name: pack for name in check.get("renderers", [])})
        files.update({str(REPO / entry["file"]): entry.get("prims", {}) for entry in check.get("open", [])})

    work = Path(tempfile.gettempdir()) / f"usdcc-smoke-{os.getpid()}"
    work.mkdir(parents=True, exist_ok=True)
    result = work / "result.json"
    script = work / "check.py"
    script.write_text(f"RESULT = {str(result)!r}\nNEED_PYSIDE = {not args.without_pyside!r}\n"
                      f"PLUGINS = {OPENUSD_PLUGINS + sorted(pack_plugins)!r}\nEXTENSIONS = {extensions!r}\n"
                      f"FILES = {sorted(files)!r}\n" + CHECK, encoding="utf-8")

    # Only what a user's machine has: the system, and the deploy itself.
    if WINDOWS:
        environment = dict(os.environ)
        for name in ("PYTHONHOME", "PYTHONPATH", "PXR_PLUGINPATH_NAME", "QT_PLUGIN_PATH"):
            environment.pop(name, None)
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        environment["PATH"] = f"{system_root}\\System32;{system_root};{system_root}\\System32\\Wbem"
        environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = platform_plugins
    else:
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(Path.home())}
    environment["QT_QPA_PLATFORM"] = "offscreen"

    # The render node (docs/RENDER_NODE.md), which the app never starts itself.
    node = os.path.join(deploy_dir, "bin", "usdcc_node.exe" if WINDOWS else "usdcc_node")
    if not os.path.isfile(node):
        problems.append(f"the deploy has no {os.path.basename(node)}")
    else:
        try:
            found = subprocess.run([node, "--version", "--no-gpu"], env=environment, timeout=args.timeout_seconds,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = found.stdout.decode("utf-8", "replace")
            first = next((line for line in output.splitlines() if line.startswith("usdcc_node ")), "")
            print(first)
            if found.returncode != 0 or not first:
                problems.append(f"usdcc_node --version exited with {found.returncode}: {output.strip()[-500:]}")
            elif args.expect_version and first.split()[1] != args.expect_version:
                problems.append(f"usdcc_node's version is {first.split()[1]}, not {args.expect_version}")
            found = subprocess.run([node, "renderers", "--no-gpu"], env=environment, timeout=args.timeout_seconds,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = found.stdout.decode("utf-8", "replace")
            if found.returncode != 0 or "HdEmbreeRendererPlugin" not in output:
                problems.append(f"usdcc_node renderers exited with {found.returncode}: {output.strip()[-500:]}")
            else:
                print("  usdcc_node renderers: Embree among them")
        except subprocess.TimeoutExpired:
            problems.append(f"usdcc_node didn't finish within {args.timeout_seconds} s")

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

        if checked.get("main_window") is False:
            problems.append("PySide6 doesn't see the main window as a QMainWindow")
        for file in checked.get("qt", []):
            print(f"  Qt: {file}")
            if not file.startswith(deploy_dir):
                problems.append(f"Qt came from outside the deploy: {file}")
        if len(checked.get("qt", [""])) != 1:
            problems.append(f"the process has {len(checked['qt'])} Qts, not one")

        for name, found in checked["plugins"].items():
            what = f"the {pack_plugins[name]} pack's {name}" if name in pack_plugins else f"OpenUSD's {name}"
            if not found:
                problems.append(f"OpenUSD doesn't find {what}")
            elif not os.path.normcase(os.path.abspath(found["path"])).startswith(os.path.normcase(deploy_dir)):
                problems.append(f"{what} came from outside the deploy: {found['path']}")
            elif not found["loaded"]:
                problems.append(f"{what} doesn't load")
        for name, pack in renderers.items():
            if name not in checked["renderers"]:
                problems.append(f"the {pack} pack's {name} isn't among the render delegates")
        for extension, known in checked["extensions"].items():
            if not known:
                problems.append(f".{extension} has no file format")
        for file, counts in checked["opened"].items():
            if counts is None:
                problems.append(f"{os.path.basename(file)} doesn't open")
                continue
            for type_name, wanted in files[file].items():
                if counts.get(type_name, 0) != wanted:
                    problems.append(f"{os.path.basename(file)} has {counts.get(type_name, 0)} {type_name}, not {wanted}")
        print(f"  OpenUSD plugins: {', '.join(sorted(checked['plugins']))}")
        print(f"  Render delegates: {', '.join(checked['renderers'])}")
        if packs:
            parts = [f"USD plugin packs: {', '.join(packs)}"]
            if extensions:
                parts.append(f"file formats: {', '.join(extensions)}")
            if files:
                parts.append(f"opened: {', '.join(os.path.basename(file) for file in sorted(files))}")
            print("  " + "; ".join(parts))
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
