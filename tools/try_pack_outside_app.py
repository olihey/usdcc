#!/usr/bin/env python3
"""Tries a USD plugin pack outside the app's folder (docs/USD_PLUGINS.md section 10.6).

A trial, not part of a build or of CI. It answers one question, on Linux and macOS: does
an optional pack still load and work when its files are in a folder of their own, laid out
as an install, with `lib` there a link to the app's own `lib`? A pack may hold plugins of
any kind; MoonRay's, of render delegates, is the first and the default here. What is tried
is what the pack's pin has under "check": its plugins load, and each "render" entry records
a frame. A pack without render delegates gets the first only; "open" entries aren't tried.

  python tools/try_pack_outside_app.py --deploy <a deploy that has the pack> [--pack moonray]

`--deploy` is a deploy with the pack unpacked into it: a Linux deploy's folder, or on macOS
usdcc.app or the folder it is in. It is only read. The trial copies it (on macOS the bundle
alone, not what else is in its folder) into `--work` (a new folder; in the temporary folder
if not given), by hard links on Linux and as clones on macOS, takes the pack's folders out of the copy into `<work>/data/usd-packs`, and starts
usdcc four times with a script that loads the pack's plugins, records a frame with each of
its render delegates if it has any, and lists where every library of the process came from:

  control   the deploy as given, the pack inside it
  nolink    the pack outside, nothing else: expected to fail, or the link isn't needed
  link      the pack outside, with <work>/data/usd-packs/lib -> the app's lib
  moved     the same after the app's folder was renamed and the link made again, as an
            AppImage is mounted somewhere else at each start

The pack is registered by usdcc's own plugin manager (--plugin-path), from a
usdcc-plugin.json the trial writes. Exit code 0 when `link` and `moved` both pass.

On macOS, taking files out of the copied bundle breaks its seal (`codesign --verify`
fails, which the trial reports); its programs and libraries keep their own signatures.
`--resign` signs the copy again, ad hoc, as tools/deploy.py does.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
import usd_plugin_asset
from _common import MACOS, REPO, WINDOWS, fail

# Runs inside usdcc (--script), after the constants the trial puts in front.
CHECK = """\
import json
import os
import time

result = {"plugins": {}, "renders": {}, "errors": []}
try:
    from pxr import Plug, Tf, Usd, UsdAppUtils, UsdGeom

    registry = Plug.Registry()
    for name in PLUGINS:
        plugin = registry.GetPluginWithName(name)
        if plugin is None:
            result["plugins"][name] = None
            continue
        try:
            plugin.Load()
        except Exception as error:  # noqa: BLE001
            result["errors"].append(f"{name}: {error}")
        result["plugins"][name] = {"loaded": plugin.isLoaded, "path": plugin.path}
    listed = sorted(t.typeName for t in Plug.Registry.GetAllDerivedTypes(Tf.Type.FindByName("HdRendererPlugin")))
    result["renderers"] = listed
    for entry in RENDERS:
        renderer = entry["renderer"]
        if renderer not in listed:
            result["renders"][renderer] = {"ok": False, "why": "not among the render delegates"}
            continue
        picture = f"{RESULT}.{renderer}.png"
        if os.path.exists(picture):
            os.remove(picture)
        started = time.time()
        try:
            stage = Usd.Stage.Open(entry["file"])
            camera = next(UsdGeom.Camera(prim) for prim in stage.Traverse() if prim.IsA(UsdGeom.Camera))
            recorder = UsdAppUtils.FrameRecorder(renderer, False)
            recorder.SetImageWidth(320)
            recorder.Record(stage, camera, Usd.TimeCode.Default(), picture)
            del recorder
        except Exception as error:  # noqa: BLE001
            result["errors"].append(f"{renderer}: {error}")
        size = os.path.getsize(picture) if os.path.exists(picture) else 0
        # A picture of one colour is a few hundred bytes as a PNG; the test scene's is 25 KB.
        result["renders"][renderer] = {"ok": size > 5000, "bytes": size, "seconds": round(time.time() - started, 1),
                                       "picture": picture}

    # Every library of the process, and how often each is there.
    if os.path.exists("/proc/self/maps"):
        starts = {}
        for line in open("/proc/self/maps").read().splitlines():
            parts = line.split(None, 5)
            if len(parts) == 6 and (parts[5].endswith(".so") or ".so." in parts[5]):
                starts[parts[5]] = starts.get(parts[5], 0) + (parts[2] == "00000000")
        images = [path for path, count in starts.items() for _ in range(max(count, 1))]
    else:
        import ctypes

        dyld = ctypes.CDLL(None)
        dyld._dyld_image_count.restype = ctypes.c_uint32
        dyld._dyld_get_image_name.restype = ctypes.c_char_p
        dyld._dyld_get_image_name.argtypes = [ctypes.c_uint32]
        images = [dyld._dyld_get_image_name(i).decode() for i in range(dyld._dyld_image_count())]
    images = [os.path.realpath(image) for image in images]
    libraries = sorted(set(images))
    system = ("/usr/lib/", "/lib/", "/System/")
    result["from_app"] = len([lib for lib in libraries if lib.startswith(APP + "/")])
    result["from_data"] = len([lib for lib in libraries if lib.startswith(DATA + "/")])
    result["from_system"] = len([lib for lib in libraries if lib.startswith(system)])
    result["from_elsewhere"] = [lib for lib in libraries
                                if not lib.startswith((APP + "/", DATA + "/")) and not lib.startswith(system)]
    result["loaded_twice"] = sorted({image for image in images if images.count(image) > 1})
    names = {}
    for lib in libraries:
        names.setdefault(os.path.basename(lib), []).append(lib)
    result["same_name_two_places"] = {name: paths for name, paths in names.items() if len(paths) > 1}
except Exception:  # noqa: BLE001
    import traceback

    result["errors"].append(traceback.format_exc())
with open(RESULT, "w") as file:
    json.dump(result, file, indent=1)
os._exit(0)
"""


class App:
    """A deploy: where its programs are, and the folder of everything else."""

    def __init__(self, given):
        given = Path(given).resolve()
        if MACOS:
            bundle = given if given.suffix == ".app" else given / "usdcc.app"
            self.top = bundle.parent  # renamed; only the bundle in it is copied
            self.inside = Path(bundle.name)
            self.exe = bundle / "Contents/MacOS/usdcc"
            self.root = bundle / "Contents/Resources"
        else:
            self.top = given
            self.inside = Path()
            self.exe = given / "bin/usdcc"
            self.root = given
        if not self.exe.is_file():
            fail(f"No usdcc in {self.exe.parent}")

    def at(self, top):
        """The same deploy, copied or moved to `top`."""
        return App(Path(top) / self.inside)


def _copy(source, target):
    """A copy that costs no room where that's possible: hard links, or clones on macOS."""
    for arguments in (["cp", "-Rc"] if MACOS else ["cp", "-al"]), ["cp", "-R" if MACOS else "-a"]:
        if subprocess.run([*arguments, str(source), str(target)], check=False, stderr=subprocess.DEVNULL).returncode == 0:
            return
        shutil.rmtree(target, ignore_errors=True)
    fail(f"Couldn't copy {source} to {target}")


def _run(name, app, work, data, spec, platform, timeout):
    result = work / f"{name}.json"
    script = work / f"{name}.py"
    renders = [{**entry, "file": str(REPO / entry["file"])} for entry in spec.get("render", [])]
    script.write_text(f"RESULT = {str(result)!r}\nAPP = {os.path.realpath(app.top)!r}\nDATA = {os.path.realpath(data)!r}\n"
                      f"PLUGINS = {spec.get('plugins', [])!r}\nRENDERS = {renders!r}\n" + CHECK, encoding="utf-8")
    # Only what a user's machine has: the system, and the deploy itself (tools/ci/smoke_test.py).
    environment = {"PATH": "/usr/bin:/bin", "HOME": str(Path.home()), "QT_QPA_PLATFORM": platform,
                   "USDCC_TEST_PROFILE": os.environ.get("USDCC_TEST_PROFILE", "pack-outside-trial")}
    for name_ in ("DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"):
        if name_ in os.environ:
            environment[name_] = os.environ[name_]
    command = [str(app.exe), "--no-startup-scripts"]
    if name != "control":
        command += ["--plugin-path", str(data)]
    with open(work / f"{name}.log", "wb") as log:
        process = subprocess.Popen([*command, "--script", str(script)], env=environment, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            code = "no end within the time"
    checked = json.loads(result.read_text(encoding="utf-8")) if result.is_file() else None
    result.unlink(missing_ok=True)
    if checked is None:
        print(f"{name:8} FAILED: usdcc wrote no result (exit: {code}); see {work / (name + '.log')}")
        return False
    (work / f"{name}.result.json").write_text(json.dumps(checked, indent=1), encoding="utf-8")
    plugins = checked["plugins"]
    loaded = [plugin for plugin in plugins.values() if plugin and plugin["loaded"]]
    outside = [plugin for plugin in loaded if not os.path.realpath(plugin["path"]).startswith(os.path.realpath(data) + os.sep)]
    rendered = [entry for entry in checked["renders"].values() if entry.get("ok")]
    passed = (len(loaded) == len(plugins) > 0 and len(rendered) == len(checked["renders"])
              and not checked.get("loaded_twice") and not checked.get("same_name_two_places")
              and (name == "control" or not outside))
    print(f"{name:8} {'passed' if passed else 'FAILED'}: {len(loaded)} of {len(plugins)} plugins load, "
          f"{len(rendered)} of {len(checked['renders'])} delegates render; libraries: {checked.get('from_app', '?')} the app's, "
          f"{checked.get('from_data', '?')} the pack folder's, {checked.get('from_system', '?')} the system's, "
          f"{len(checked.get('from_elsewhere', []))} from elsewhere, {len(checked.get('loaded_twice', []))} loaded twice, "
          f"{len(checked.get('same_name_two_places', {}))} names from two places")
    for error in checked["errors"][:3]:
        print(f"         {' '.join(error.split())[:400]}")
    for label in ("from_elsewhere", "loaded_twice"):
        for path in checked.get(label, [])[:5]:
            print(f"         {label}: {path}")
    return passed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--deploy", required=True, help="a deploy with the pack in it (on macOS usdcc.app or its folder)")
    parser.add_argument("--pack", default="moonray")
    parser.add_argument("--work", help="a new folder for the trial; one in the temporary folder if not given")
    parser.add_argument("--platform", default="offscreen", help="QT_QPA_PLATFORM for usdcc (default: offscreen)")
    parser.add_argument("--resign", action="store_true", help="macOS: sign the copy again once the pack is out of it")
    parser.add_argument("--timeout-seconds", type=int, default=300)
    args = parser.parse_args(argv)
    if WINDOWS:
        fail("Linux and macOS only: this tries the link to the app's lib, which Windows doesn't have. What was tried "
             "there, a pack's own bin on usdcc's PATH, is in docs/USD_PLUGINS.md section 10.6.")

    given = App(args.deploy)
    record = given.root / "usd-plugins" / f"{args.pack}.json"
    if not record.is_file():
        fail(f"{given.root} has no {args.pack} pack (no {record})")
    files = json.loads(record.read_text(encoding="utf-8"))["files"]
    stray = [file for file in files if not file.startswith(("plugin/usd/", "licenses/", "usd-plugins/"))]
    if stray:
        fail(f"The {args.pack} pack has files outside plugin/usd, which this route can't take: {', '.join(stray[:5])}")
    folders = sorted({file.split("/")[2] for file in files if file.startswith("plugin/usd/")})
    spec = usd_plugin_asset.load(args.pack).get("check", {})

    work = Path(args.work).resolve() if args.work else Path(tempfile.mkdtemp(prefix="usdcc-pack-outside-"))
    if work.exists() and any(work.iterdir()):
        fail(f"{work} isn't empty")
    work.mkdir(parents=True, exist_ok=True)
    print(f"The trial is in {work}")

    passed = {"control": _run("control", given, work, work / "data", spec, args.platform, args.timeout_seconds)}

    # The bundle alone: its folder may be /Applications.
    (work / "app" / given.inside).parent.mkdir(parents=True, exist_ok=True)
    _copy(given.top / given.inside, work / "app" / given.inside)
    app = given.at(work / "app")
    packs = work / "data" / "usd-packs"
    (packs / "plugin/usd").mkdir(parents=True)
    for folder in folders:
        shutil.move(str(app.root / "plugin/usd" / folder), str(packs / "plugin/usd" / folder))
    (app.root / "usd-plugins" / f"{args.pack}.json").unlink()
    (packs / "plugin/usd/plugInfo.json").write_text('{\n    "Includes": [ "*/resources/" ]\n}\n', encoding="utf-8")
    (packs / "usdcc-plugin.json").write_text(json.dumps(
        {"id": f"org.usdcc.trial.{args.pack}-pack", "name": f"{args.pack} pack (trial)", "version": "0.0.1", "api": 1,
         "usd": ["plugin/usd"]}, indent=2), encoding="utf-8")
    print(f"Outside the app: {', '.join(folders)}")
    if MACOS:
        bundle = work / "app" / app.inside
        if args.resign:
            import _macho

            _macho.sign_app(bundle)
            print("The copy is signed again, ad hoc")
        verify = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(bundle)], check=False, capture_output=True,
                                text=True)
        print(f"codesign --verify of the copy: {'whole' if verify.returncode == 0 else verify.stderr.strip()[:300]}")

    link = packs / "lib"
    passed["nolink"] = _run("nolink", app, work, work / "data", spec, args.platform, args.timeout_seconds)

    link.symlink_to(app.root / "lib")
    passed["link"] = _run("link", app, work, work / "data", spec, args.platform, args.timeout_seconds)

    (work / "app").rename(work / "app-moved")
    moved = given.at(work / "app-moved")
    link.unlink()
    link.symlink_to(moved.root / "lib")
    passed["moved"] = _run("moved", moved, work, work / "data", spec, args.platform, args.timeout_seconds)

    print("nolink failing is expected: it shows the link is what makes the difference."
          if not passed["nolink"] else "nolink passed: on this platform the link isn't needed for this pack.")
    print(f"Results, logs and pictures: {work}")
    return 0 if passed["link"] and passed["moved"] else 1


if __name__ == "__main__":
    sys.exit(main())
