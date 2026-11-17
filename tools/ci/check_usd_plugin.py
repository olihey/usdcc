"""Checks a USD plugin pack against the OpenUSD it was built for.

A pack (tools/build_usd_plugin.py, docs/USD_PLUGINS.md) is unzipped over an
OpenUSD install. This checks, first with the two still apart:

  - the pack holds exactly the files its usd-plugins/<pack>.json lists;
  - no file of the pack is already in the OpenUSD install, and no DLL (or .so)
    in it has the name of one there: two with one name can't both load in a
    process, and the first one wins;
  - no text file in the pack names a --forbid folder (the build's).

Then it copies the pack over the install and, in a fresh Python with nothing of
this machine's environment, does what the pin's "check" asks:

  - "plugins": OpenUSD finds each one, inside the install, and loads it, so
    every DLL it depends on is there;
  - "extensions": each has a file format;
  - "open": each file opens as a stage with that many prims of each type.

--usd is changed by this (the pack is copied into it), so pass a throwaway copy
of the OpenUSD asset.

    python tools/ci/check_usd_plugin.py adobe-fileformats --usd C:\\t\\usd --pack C:\\t\\pack --forbid C:\\b
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
from _common import REPO, WINDOWS, copy_tree, fail  # noqa: E402
import usd_plugin_asset  # noqa: E402

MANIFEST_DIR = "usd-plugins"

# Runs in the fresh Python. Prints a line per check and exits 1 if one failed.
_RUNNER = r"""
import json, os, sys
install, spec = sys.argv[1], json.loads(sys.argv[2])
from pxr import Plug, Sdf, Usd
failed = []
def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what, flush=True)
    if not ok:
        failed.append(what)
inside = os.path.normcase(os.path.realpath(install)) + os.sep
for name in spec.get("plugins", []):
    plugin = Plug.Registry().GetPluginWithName(name)
    check(plugin is not None, f"OpenUSD finds the plugin {name}")
    if plugin is None:
        continue
    check(os.path.normcase(os.path.realpath(plugin.path)).startswith(inside), f"{name} is the install's ({plugin.path})")
    loaded = False
    try:
        loaded = plugin.Load()
    except Exception as error:
        print(f"        {error}")
    check(loaded, f"{name} loads")
for extension in spec.get("extensions", []):
    check(Sdf.FileFormat.FindByExtension(extension) is not None, f".{extension} has a file format")
for entry in spec.get("open", []):
    try:
        stage = Usd.Stage.Open(entry["file"])
    except Exception as error:
        print(f"        {error}")
        stage = None
    check(bool(stage), f"{os.path.basename(entry['file'])} opens")
    if not stage:
        continue
    counts = {}
    for prim in stage.Traverse():
        counts[prim.GetTypeName()] = counts.get(prim.GetTypeName(), 0) + 1
    for type_name, wanted in entry.get("prims", {}).items():
        check(counts.get(type_name, 0) == wanted,
              f"{os.path.basename(entry['file'])} has {wanted} {type_name} (found {counts.get(type_name, 0)})")
sys.exit(1 if failed else 0)
"""


def _files(folder):
    folder = Path(folder)
    return sorted(file.relative_to(folder).as_posix() for file in folder.rglob("*") if file.is_file())


def _is_library(name):
    name = name.lower()
    return name.endswith((".dll", ".dylib")) or name.endswith(".so") or ".so." in name


def _key(text):
    """As the file system compares names: Windows ignores case."""
    return text.lower() if WINDOWS else text


def static_checks(pack, usd, pack_dir, forbidden):
    """What can be told with the pack and the install still apart; the problems found."""
    problems = []
    manifest_file = pack_dir / MANIFEST_DIR / f"{pack}.json"
    if not manifest_file.is_file():
        fail(f"{pack_dir} isn't a {pack} pack: no {MANIFEST_DIR}/{pack}.json")
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    files = _files(pack_dir)
    own = f"{MANIFEST_DIR}/{pack}.json"

    listed, present = set(manifest["files"]), set(files) - {own}
    for file in sorted(listed - present):
        problems.append(f"listed in {own} but missing: {file}")
    for file in sorted(present - listed):
        problems.append(f"not listed in {own}: {file}")

    usd_files = _files(usd)
    in_usd = {_key(file) for file in usd_files}
    usd_libraries = {_key(Path(file).name): file for file in usd_files if _is_library(file)}
    for file in files:
        if _key(file) in in_usd:
            problems.append(f"already in the OpenUSD install: {file}")
        elif _is_library(file) and _key(Path(file).name) in usd_libraries:
            problems.append(f"{file} has the name of the OpenUSD install's {usd_libraries[_key(Path(file).name)]}")

    needles = []
    for folder in forbidden:
        folder = str(Path(folder).resolve())
        needles += [folder.replace("\\", "/").lower(), folder.replace("/", "\\").lower()]
    for file in files if needles else []:
        data = (pack_dir / file).read_bytes()
        if b"\0" in data[:8192]:
            continue  # a binary: a DLL names its build folder (its .pdb), which nothing reads
        text = data.decode("utf-8", errors="replace").lower()
        for needle in needles:
            if needle in text:
                problems.append(f"{file} names the build folder {needle}")
                break
    return problems, manifest


def clean_environment(usd):
    """Only what OpenUSD and Python need to start, so nothing of the machine's
    (another OpenUSD on PATH, a PYTHONPATH) can stand in for a missing file."""
    python_dir = str(Path(sys.executable).resolve().parent)
    # Where OpenUSD's build puts its bindings has changed between versions.
    bindings = [folder for pattern in ("lib/site-packages", "lib/python", "lib/python*/site-packages")
                for folder in sorted(usd.glob(pattern)) if (folder / "pxr" / "__init__.py").is_file()]
    if not bindings:
        fail(f"No pxr Python package in {usd / 'lib'}: the check needs an OpenUSD built with Python")
    environment = {"PYTHONPATH": str(bindings[0])}
    if WINDOWS:
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        environment["PATH"] = os.pathsep.join(
            [str(usd / "bin"), str(usd / "lib"), python_dir, rf"{system_root}\System32", system_root])
        for name in ("SystemRoot", "SystemDrive", "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA", "APPDATA"):
            if name in os.environ:
                environment[name] = os.environ[name]
    else:
        environment["PATH"] = os.pathsep.join([python_dir, "/usr/bin", "/bin"])
        environment["HOME"] = os.environ.get("HOME", "/tmp")
    return environment


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("pack", help="the pack's name, as in deps/usd-plugins/<pack>.json")
    parser.add_argument("--usd", type=Path, required=True,
                        help="a throwaway copy of the OpenUSD install; the pack is copied into it")
    parser.add_argument("--pack-dir", "--pack", type=Path, required=True, dest="pack_dir", help="the pack's folder")
    parser.add_argument("--forbid", type=Path, action="append", default=[],
                        help="a folder no text file of the pack may name (the build's); repeatable")
    args = parser.parse_args(argv)

    pack = args.pack
    spec = usd_plugin_asset.load(pack)
    usd, pack_dir = args.usd.resolve(), args.pack_dir.resolve()
    if not (usd / "pxrConfig.cmake").is_file():
        fail(f"No OpenUSD install at {usd} (no pxrConfig.cmake)")

    print(f"Checking the {pack} pack in {pack_dir} against {usd}", flush=True)
    problems, manifest = static_checks(pack, usd, pack_dir, args.forbid)
    for problem in problems:
        print(f"  FAIL  {problem}")
    if problems:
        fail(f"The {pack} pack can't go over this OpenUSD install: {len(problems)} problem(s)")
    print(f"  ok    {len(manifest['files'])} files, none of them in the OpenUSD install, no DLL name twice", flush=True)

    copy_tree(pack_dir, usd)

    check = dict(spec.get("check", {}))
    check["open"] = [{**entry, "file": str(REPO / entry["file"])} for entry in check.get("open", [])]
    for entry in check["open"]:
        if not Path(entry["file"]).is_file():
            fail(f"deps/usd-plugins/{pack}.json: no such file {entry['file']}")
    with tempfile.TemporaryDirectory() as empty:
        # -P: not the current folder on sys.path. An empty folder as that, too.
        result = subprocess.run([sys.executable, "-P", "-c", _RUNNER, str(usd), json.dumps(check)],
                                env=clean_environment(usd), cwd=empty)
    if result.returncode != 0:
        fail(f"The {pack} pack failed its check ({result.returncode})")
    print(f"The {pack} pack works over {usd}.")


if __name__ == "__main__":
    main()
