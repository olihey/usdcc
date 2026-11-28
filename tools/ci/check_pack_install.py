#!/usr/bin/env python3
"""Installs a release's optional packs the way a user's usdcc does, and checks that they
load (docs/USD_PLUGINS.md section 10).

  python tools/ci/check_pack_install.py --deploy <deploy> --packs <folder> [--platform-plugins <folder>]

`--packs` is what tools/ci/pack_index.py wrote: the index and the packs' archives. With a
test profile, so nothing of the machine's own is touched, the deploy's usdcc:

  1. installs every pack of the index (usdcc --install-packs, USDCC_PACKS_URL): the
     download from the folder, its size and SHA-256, the unpacking, the folder in place;
  2. starts, with the packs in its data folder and nowhere else: each pack has plugins
     OpenUSD registered from there; those its pin's "check" names load, with every
     library they need, and the renderers it names are listed;
  3. removes them again (usdcc --remove-packs), and nothing is left.

A pack whose plugins need an SDK the machine doesn't have (its pin's check.preload:
Arnold's core) can't load here. For it, 2 is that its folder is in the store, complete,
and that OpenUSD registered nothing from it: usdcc registers its plugins only once
Settings names the SDK, and a plugin registered without it would start it and fail.

An index without packs passes: a platform may have none. `--usdcc <program>` takes a
program other than a deploy's, with the environment as it is (a development build).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import usd_plugin_asset  # noqa: E402
from _common import MACOS, WINDOWS, fail  # noqa: E402

# Runs inside usdcc (--script), after the constants this puts in front.
CHECK = """\
import json
import os
import traceback

result = {"packs": {}, "errors": []}
try:
    import usdcc.app
    from pxr import Plug, Tf

    store = os.path.realpath(os.path.join(usdcc.app.user_data_folder(), "packs"))
    result["store"] = store
    registry = Plug.Registry()
    everything = registry.GetAllPlugins()
    renderers = sorted(t.typeName for t in Plug.Registry.GetAllDerivedTypes(Tf.Type.FindByName("HdRendererPlugin")))
    for pack, wanted in PACKS.items():
        found = {"folder": None, "registered": [], "plugins": {}, "renderers": {}, "complete": False}
        # <store>/<version>-<platform>/<pack>/, whether or not anything was registered from it.
        for version in sorted(os.listdir(store)) if os.path.isdir(store) else []:
            folder = os.path.join(store, version, pack)
            if not version.startswith(".") and os.path.isdir(folder):
                found["folder"] = folder
                found["complete"] = all(os.path.isfile(os.path.join(folder, name)) for name in ("pack.json", "usdcc-plugin.json"))
        for plugin in everything:
            for path in (plugin.path, plugin.resourcePath):
                real = os.path.realpath(path)
                if not real.startswith(store + os.sep):
                    continue
                # <store>/<version>-<platform>/<pack>/...
                parts = os.path.relpath(real, store).split(os.sep)
                if len(parts) > 1 and parts[1] == pack:
                    found["folder"] = os.path.join(store, parts[0], pack)
                    if plugin.name not in found["registered"]:
                        found["registered"].append(plugin.name)
        if wanted["needs_sdk"]:
            result["packs"][pack] = found
            continue
        for name in wanted["plugins"]:
            plugin = registry.GetPluginWithName(name)
            if plugin is None:
                found["plugins"][name] = "not registered"
                continue
            try:
                plugin.Load()
            except Exception as error:  # noqa: BLE001
                found["plugins"][name] = f"doesn't load: {error}"
                continue
            inside = os.path.realpath(plugin.path).startswith(store)
            found["plugins"][name] = "ok" if plugin.isLoaded and inside else f"loaded from {plugin.path}, not the pack's folder"
        for name in wanted["renderers"]:
            found["renderers"][name] = name in renderers
        result["packs"][pack] = found
except Exception:  # noqa: BLE001
    result["errors"].append(traceback.format_exc())
with open(RESULT, "w") as file:
    json.dump(result, file, indent=1)
os._exit(0)
"""


def _run(command, environment, timeout):
    try:
        return subprocess.run(command, env=environment, timeout=timeout, capture_output=True, text=True, errors="replace")
    except subprocess.TimeoutExpired:
        fail(f"No end within {timeout} s: {' '.join(str(part) for part in command)}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--deploy", help="the deploy whose usdcc installs them")
    parser.add_argument("--usdcc", help="a usdcc program instead of a deploy's, run with the environment as it is")
    parser.add_argument("--packs", type=Path, required=True, help="the index and archives, as tools/ci/pack_index.py wrote them")
    parser.add_argument("--platform-plugins", help="Windows, with --deploy: the folder of Qt's platform plugins, for qoffscreen.dll")
    parser.add_argument("--timeout-seconds", type=int, default=900)
    args = parser.parse_args(argv)
    if bool(args.deploy) == bool(args.usdcc):
        fail("One of --deploy and --usdcc.")

    indexes = sorted(args.packs.glob("usdcc-*-packs.json")) if args.packs.is_dir() else []
    if len(indexes) != 1:
        fail(f"{args.packs} has {len(indexes)} indexes of packs (usdcc-<version>-<platform>-packs.json), not one")
    index = json.loads(indexes[0].read_text(encoding="utf-8"))
    names = [entry["name"] for entry in index["packs"]]
    if not names:
        print(f"{indexes[0].name} has no packs: nothing to install")
        return 0
    for entry in index["packs"]:
        if not (args.packs / entry["file"]).is_file():
            fail(f"{indexes[0].name} names {entry['file']}, which isn't in {args.packs}")

    if args.usdcc:
        exe = os.path.abspath(args.usdcc)
        environment = dict(os.environ)
    else:
        deploy = os.path.abspath(args.deploy)
        exe = os.path.join(deploy, "usdcc.app/Contents/MacOS" if MACOS else "bin", "usdcc.exe" if WINDOWS else "usdcc")
        # Only what a user's machine has: the system, and the deploy itself (smoke_test.py).
        if WINDOWS:
            environment = dict(os.environ)
            for name in ("PYTHONHOME", "PYTHONPATH", "PXR_PLUGINPATH_NAME", "QT_PLUGIN_PATH"):
                environment.pop(name, None)
            system_root = os.environ.get("SystemRoot", r"C:\Windows")
            environment["PATH"] = f"{system_root}\\System32;{system_root};{system_root}\\System32\\Wbem"
            if args.platform_plugins:
                environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = os.path.abspath(args.platform_plugins)
        else:
            environment = {"PATH": "/usr/bin:/bin", "HOME": str(Path.home())}
            for name in ("DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "LIBGL_ALWAYS_SOFTWARE"):
                if name in os.environ:
                    environment[name] = os.environ[name]
    if not os.path.isfile(exe):
        fail(f"No usdcc at {exe}")
    environment.setdefault("QT_QPA_PLATFORM", "offscreen")
    environment["USDCC_TEST_PROFILE"] = f"pack-install-{os.getpid()}"
    environment["USDCC_PACKS_URL"] = str(args.packs.resolve())

    problems = []
    installed = _run([exe, "--install-packs", ",".join(names)], environment, args.timeout_seconds)
    print(installed.stderr.strip())
    if installed.returncode != 0:
        fail(f"usdcc --install-packs {','.join(names)} ended with {installed.returncode}")

    wanted = {}
    for name in names:
        check = usd_plugin_asset.load(name).get("check", {}) if usd_plugin_asset.pin_file(name).is_file() else {}
        wanted[name] = {"plugins": check.get("plugins", []), "renderers": check.get("renderers", []),
                        "needs_sdk": bool(check.get("preload"))}
    with tempfile.TemporaryDirectory(prefix="usdcc-pack-install-") as work:
        result = Path(work) / "result.json"
        script = Path(work) / "check.py"
        script.write_text(f"RESULT = {str(result)!r}\nPACKS = {wanted!r}\n" + CHECK, encoding="utf-8")
        started = _run([exe, "--no-startup-scripts", "--script", str(script)], environment, args.timeout_seconds)
        if not result.is_file():
            fail(f"usdcc wrote no result with the packs installed (exit {started.returncode}):\n{started.stderr[-2000:]}")
        checked = json.loads(result.read_text(encoding="utf-8"))
    problems += checked["errors"]
    folders = []
    for name in names:
        found = checked["packs"].get(name, {})
        if wanted[name]["needs_sdk"]:
            if not found.get("folder") or not found.get("complete"):
                problems.append(f"{name}: no complete pack in the store")
                continue
            folders.append(found["folder"])
            if found.get("registered"):
                problems.append(f"{name}: OpenUSD registered {', '.join(found['registered'])} from it, without the SDK "
                                "its plugins need")
            else:
                print(f"{name}: installed in {found['folder']}; nothing registered, as its plugins need an SDK "
                      "Settings doesn't name")
            continue
        if not found.get("registered"):
            problems.append(f"{name}: OpenUSD registered no plugin from its folder")
            continue
        folders.append(found["folder"])
        bad = [f"{plugin} {state}" for plugin, state in found["plugins"].items() if state != "ok"]
        bad += [f"the renderer {renderer} isn't listed" for renderer, listed in found["renderers"].items() if not listed]
        if bad:
            problems.append(f"{name}: " + "; ".join(bad))
        else:
            print(f"{name}: {len(found['registered'])} plugin(s) registered from {found['folder']}; "
                  f"{len(found['plugins'])} loaded, {len(found['renderers'])} renderer(s) listed")

    removed = _run([exe, "--remove-packs", ",".join(names)], environment, args.timeout_seconds)
    print(removed.stderr.strip())
    if removed.returncode != 0:
        problems.append(f"usdcc --remove-packs ended with {removed.returncode}")
    problems += [f"{folder} is still there after --remove-packs" for folder in folders if folder and os.path.isdir(folder)]

    if problems:
        fail("The packs don't install as they should:\n  " + "\n  ".join(problems))
    print(f"{len(names)} pack(s) install, load and are removed again: {', '.join(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
