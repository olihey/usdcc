"""Builds a USD plugin as deps/usd-plugins/<pack>.json pins it, into a pack.

A pack is a folder laid out like the OpenUSD install it was built against
(plugin/usd/..., bin/ for the DLLs the plugins need), holding only what runs:
unzipped or copied over that install, OpenUSD finds the plugins with no further
setup (docs/USD_PLUGINS.md). .github/workflows/deps-usd-plugins.yml runs this and
publishes the zip; locally it's the same build.

    python tools/build_usd_plugin.py adobe-fileformats --usd-install C:\\deps\\usd
    python tools/build_usd_plugin.py adobe-fileformats --usd-install C:\\deps\\usd --install

Fetches the pinned commit (with its submodules if the pin says so), applies the
pin's patches, downloads and checks the SDKs the pin lists, configures with CMake
against the OpenUSD install (Release, the release CRT, the pin's Python), builds
and installs. Then it drops what only a build needs (headers, import libraries,
CMake files), collects the licences of the plugin and of what went into it under
licenses/usd-plugins/<pack>/, with a SOURCES.txt saying what it was built from,
and writes usd-plugins/<pack>.json, which names the build and lists the pack's
files (tools/fetch_usd.py and tools/deploy.py go by it).

What a pin can say beyond the repository and commit (docs/USD_PLUGINS.md section 4):
  submodules          true: the checkout's submodules too, and their licences
  patches             applied to the checkout, relative to deps/usd-plugins
  downloads           {name: {platform: {url, sha256}}}: an archive each, unpacked
  cmake               arguments; {usd}, {pack} and {<download name>} are folders
  targets             what to build (default: everything)
  install_prefix      where in the pack the install goes (default: its root)
  install_components  the install components to take (default: all of it)
  licenses            {name: [files]}: more licence files, {<download name>} allowed

--install copies the pack over the OpenUSD install afterwards. Check a pack with
tools/ci/check_usd_plugin.py.

On Linux the pack's own binaries get RUNPATHs relative to $ORIGIN (patchelf), as
tools/ci/relocate_usd.py gives OpenUSD's.

Folders are kept as they are given, not resolved: on Windows a deep source tree
(OpenUSD's, a plugin's submodules) needs a short path, and a drive letter mapped
onto a long one (subst) is one.

Standard library only.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
from _common import ASSET_PLATFORM, LINUX, REPO, WINDOWS, copy_file, copy_tree, fail, remove_tree, run  # noqa: E402
from build_usd import fetch  # noqa: E402
import usd_asset  # noqa: E402
import usd_licenses  # noqa: E402
import usd_plugin_asset  # noqa: E402

# What a build leaves in an install that nothing needs at run time.
BUILD_ONLY_DIRS = ["include", "cmake", "lib/cmake", "lib/pkgconfig"]
BUILD_ONLY_FILES = ["*.lib", "*.exp", "*.ilk", "*.pdb", "*.a", "*.cmake", "*.pc"]

MANIFEST_DIR = "usd-plugins"


def manifest_path(pack_dir, pack):
    return Path(pack_dir) / MANIFEST_DIR / f"{pack}.json"


def pack_files(pack_dir):
    """Every file in the pack, as sorted paths relative to it with forward slashes."""
    pack_dir = Path(pack_dir)
    return sorted(file.relative_to(pack_dir).as_posix() for file in pack_dir.rglob("*") if file.is_file())


def _folder(path):
    """`path` made absolute but not resolved, so a mapped drive stays one."""
    return Path(os.path.abspath(Path(path).expanduser()))


def _empty_pack_dir(pack_dir, pack):
    """An empty `pack_dir`. Only ever deletes an earlier build of the same pack."""
    if pack_dir.is_dir() and any(pack_dir.iterdir()):
        if not manifest_path(pack_dir, pack).is_file():
            fail(f"{pack_dir} isn't empty and isn't an earlier build of this pack; pass an empty folder as --pack")
        remove_tree(pack_dir)
    pack_dir.mkdir(parents=True, exist_ok=True)


def _prune(pack_dir):
    """Drops the build-only files and the folders that leaves empty."""
    for folder in BUILD_ONLY_DIRS:
        if (pack_dir / folder).is_dir():
            remove_tree(pack_dir / folder)
    for file in list(pack_dir.rglob("*")):
        if file.is_file() and any(fnmatch.fnmatch(file.name, pattern) for pattern in BUILD_ONLY_FILES):
            file.unlink()
    for folder, _, _ in os.walk(pack_dir, topdown=False):
        if Path(folder) != pack_dir and not any(Path(folder).iterdir()):
            Path(folder).rmdir()


def _relative_runpaths(pack_dir):
    """Linux: points what the build linked by absolute path (the OpenUSD install's
    lib folder) at the same place relative to the file, so the pack works over an
    OpenUSD install or a deploy wherever that is. A prebuilt library that came with
    an SDK has no such path and is left as it is. Returns the files it changed."""
    from _elf import elf_files, origin_relative, runpath, set_runpath
    changed = []
    for file in elf_files(pack_dir):
        entries = runpath(file)
        if not any(entry.startswith("/") for entry in entries):
            continue
        kept = [entry for entry in entries if not entry.startswith("/")]
        for entry in ("$ORIGIN", origin_relative(file, pack_dir / "lib")):
            if entry not in kept:
                kept.append(entry)
        set_runpath(file, kept)
        changed.append(file.relative_to(pack_dir).as_posix())
    return changed


def _git_long_paths():
    """Lets every git this starts, the builds' own included, write paths past
    Windows' 260 characters: git resolves a mapped drive back to its long folder."""
    if WINDOWS and "GIT_CONFIG_COUNT" not in os.environ:
        os.environ.update({"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.longpaths", "GIT_CONFIG_VALUE_0": "true"})


def _pristine(source):
    """Puts an existing checkout back as its commit has it, so the pin's patches
    apply to what they were made against even after one of them changed. Files git
    ignores, and the submodules, stay."""
    if (source / ".git").exists():
        run(["git", "-C", source, "reset", "-q", "--hard"])
        run(["git", "-C", source, "clean", "-q", "-fd"])


def _submodules(source):
    """Checks out the checkout's submodules, at the commits it names; their paths."""
    update = ["git", "-C", str(source), "submodule", "update", "--init", "--recursive"]
    sys.stdout.flush()
    if subprocess.run(update + ["--depth", "1"]).returncode != 0:
        # A server that doesn't hand out a commit by itself: the whole history then.
        run(update)
    status = run(["git", "-C", source, "submodule", "status", "--recursive"], capture_output=True, text=True)
    return [line.split()[1] for line in status.stdout.splitlines() if len(line.split()) > 1]


def _sha256(file):
    digest = hashlib.sha256()
    with open(file, "rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _download(name, entry, folder):
    """The archive a pin's download names, checked and unpacked into `folder`/<name>.
    Returns what it holds: the one folder in it, if that is all there is."""
    url, wanted = entry["url"], entry["sha256"].lower()
    archive = folder / url.rsplit("/", 1)[-1]
    target = folder / name
    marker = folder / f"{name}.sha256"
    if not (target.is_dir() and marker.is_file() and marker.read_text(encoding="ascii").strip() == wanted):
        folder.mkdir(parents=True, exist_ok=True)
        if not archive.is_file() or _sha256(archive) != wanted:
            print(f"Downloading {url}", flush=True)
            partial = archive.with_name(archive.name + ".partial")
            try:
                with urllib.request.urlopen(url, timeout=120) as response, partial.open("wb") as output:
                    shutil.copyfileobj(response, output, 1024 * 1024)
            except OSError as error:
                fail(f"Downloading {url} failed: {error}")
            found = _sha256(partial)
            if found != wanted:
                fail(f"{url} isn't the file the pin names: its SHA-256 is {found}, not {wanted}")
            partial.replace(archive)
        if target.exists():
            remove_tree(target)
        print(f"Unpacking {archive.name}", flush=True)
        try:
            shutil.unpack_archive(archive, target, filter="data")
        except TypeError:  # a zip takes no filter
            shutil.unpack_archive(archive, target)
        marker.write_text(wanted + "\n", encoding="ascii")
    entries = list(target.iterdir())
    return entries[0] if len(entries) == 1 and entries[0].is_dir() else target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("pack", help="the pack to build, as in deps/usd-plugins/<pack>.json")
    parser.add_argument("--usd-install", type=Path, required=True, help="the OpenUSD install to build against")
    parser.add_argument("--work", type=Path,
                        help="for the checkout, the downloads and the build tree (default: build/usd-plugins/<pack>)")
    parser.add_argument("--pack-dir", type=Path, help="where the pack goes (default: <work>/pack)")
    parser.add_argument("--install", action="store_true", help="also copy the pack over the OpenUSD install")
    parser.add_argument("--jobs", type=int, help="parallel jobs (default: the build tool's)")
    args = parser.parse_args(argv)

    pack = args.pack
    spec = usd_plugin_asset.load(pack)
    if ASSET_PLATFORM not in spec["platforms"]:
        fail(f"The {pack} pack isn't set up for {ASSET_PLATFORM} (its pin lists {', '.join(spec['platforms'])})")

    usd_pin = json.loads((REPO / "deps/usd.json").read_text(encoding="utf-8-sig"))
    wanted = usd_pin["python"].split(".")[:2]
    if [str(part) for part in sys.version_info[:2]] != wanted:
        fail(f"deps/usd.json pins Python {usd_pin['python']}; run this with a Python {'.'.join(wanted)} "
             f"(this is {sys.version.split()[0]} at {sys.executable})")

    usd = _folder(args.usd_install)
    if not (usd / "pxrConfig.cmake").is_file():
        fail(f"No OpenUSD install at {usd} (no pxrConfig.cmake)")
    work = _folder(args.work) if args.work else REPO / "build" / "usd-plugins" / pack
    pack_dir = _folder(args.pack_dir) if args.pack_dir else work / "pack"
    source = work / "src" / pack  # alone in src/, which is how usd_licenses.collect() names it
    build = work / "build"

    _git_long_paths()
    _pristine(source)
    fetch(spec, usd_plugin_asset.PINS, source)
    submodules = _submodules(source) if spec.get("submodules") else []
    _empty_pack_dir(pack_dir, pack)

    places = {"usd": usd.as_posix(), "pack": pack_dir.as_posix()}
    downloads = {}
    for name, platforms in spec.get("downloads", {}).items():
        if ASSET_PLATFORM not in platforms:
            fail(f"deps/usd-plugins/{pack}.json: the download '{name}' has no entry for {ASSET_PLATFORM}")
        downloads[name] = platforms[ASSET_PLATFORM]
        places[name] = _download(name, downloads[name], work / "downloads").as_posix()

    prefix = pack_dir / spec.get("install_prefix", "")
    command = ["cmake", "-S", source, "-B", build]
    if WINDOWS:
        # The toolset OpenUSD and usdcc are built with (docs/CI.md section 2).
        command += ["-G", "Visual Studio 17 2022", "-A", "x64"]
    else:
        command += ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    command += [
        # On the prefix path, not just a hint for pxr: pxrConfig.cmake finds some of
        # its own dependencies there with plain find_dependency() calls.
        f"-DCMAKE_PREFIX_PATH={places['usd']}",
        f"-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}",
        # The prebuilt OpenUSD is Release-only, so its plugins use the release CRT.
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
        # pxrConfig.cmake looks for the pin's Python (its libraries, not the
        # interpreter): the one this runs on, not whichever CMake finds first.
        f"-DPython3_ROOT_DIR={Path(sys.base_prefix).as_posix()}",
        *[argument.format(**places) for argument in spec["cmake"]],
    ]
    print(f"Configuring {pack}:", " ".join(str(part) for part in command), flush=True)
    run(command)
    run(["cmake", "--build", build, "--config", "Release", "--parallel", *([str(args.jobs)] if args.jobs else []),
         *(["--target", *spec["targets"]] if spec.get("targets") else [])])
    for component in spec.get("install_components") or [None]:
        run(["cmake", "--install", build, "--config", "Release", "--prefix", prefix,
             *(["--component", component] if component else [])])

    _prune(pack_dir)
    if not pack_files(pack_dir):
        fail(f"The {pack} build installed nothing that runs into {pack_dir}")
    if LINUX:
        print(f"RUNPATHs relative to $ORIGIN: {', '.join(_relative_runpaths(pack_dir)) or 'none to change'}", flush=True)

    # The licences: the plugin's own and what CMake fetched into it, its
    # submodules', and the files the pin names (an SDK's, shipped beside it).
    licences = pack_dir / "licenses" / MANIFEST_DIR / pack
    found = usd_licenses.collect(work / "src", build, licences)
    if pack not in found:
        fail(f"No licence file at the top of {source}")
    for path in submodules:
        name = path.replace("/", "-")
        if usd_licenses._copy_licences(source / path, licences / name):
            found.append(name)
    for name, files in spec.get("licenses", {}).items():
        (licences / name).mkdir(parents=True, exist_ok=True)
        for file in files:
            copy_file(Path(file.format(**places)), licences / name)
        found.append(name)
    print(f"Licences: {', '.join(found)}")

    # What the binaries were built from, which the GPL and the LGPL want said.
    sources = [f"{pack} {spec.get('version', '')}".rstrip() + f" ({spec['license']}), built by tools/build_usd_plugin.py",
               "of https://github.com/olihey/usdcc as its deps/usd-plugins/" + pack + ".json pins it.",
               "", f"source:  {spec['repository']} at {spec['commit']}" + (", with its submodules" if submodules else "")]
    sources += [f"patch:   deps/usd-plugins/{patch}" for patch in spec.get("patches", [])]
    sources += [f"{name}: {entry['url']} (SHA-256 {entry['sha256']})" for name, entry in downloads.items()]
    (licences / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8", newline="\n")

    files = pack_files(pack_dir)
    manifest = manifest_path(pack_dir, pack)
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({
        "pack": pack,
        # What tools/fetch_usd.py compares, to tell whether an install has this build.
        "asset": usd_plugin_asset.describe(pack)["name"],
        "repository": spec["repository"],
        "commit": spec["commit"],
        "version": spec.get("version", ""),
        "license": spec["license"],
        "set": spec["set"],
        "usd": usd_asset.describe(REPO / "deps/usd.json")["base"],
        "platform": ASSET_PLATFORM,
        "files": files,
    }, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"The {pack} pack is in {pack_dir}: {len(files)} files.")

    if args.install:
        copy_tree(pack_dir, usd)
        print(f"Copied over {usd}.")


if __name__ == "__main__":
    main()
