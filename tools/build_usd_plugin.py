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
  submodules          true: the checkout's submodules too, and their licences;
                      or a list of the submodules' folders: only those
  patches             applied to the checkout, relative to deps/usd-plugins; an
                      entry {"patch": <file>, "in": <folder>} applies to a submodule
  hook                a Python file in deps/usd-plugins whose build(ctx) builds and
                      installs into the pack instead of "cmake" and the rest, for
                      what isn't one CMake project (MoonRay); it returns the licences
                      of what it put in, {name: folder}
  downloads           {name: {platform: {url, sha256}}}: an archive each, unpacked;
                      null for a platform that doesn't need it
  sdks                {name: {description, files}}: an SDK that can't be downloaded
                      (it needs a login, as the Arnold SDK does), passed as
                      --sdk <name>=<folder>, which must have the files listed;
                      `files` a list, or {platform: list} where they differ
  cmake               arguments; {usd}, {pack}, {<download name>} and {<sdk name>} are folders
  targets             what to build (default: everything)
  install_prefix      where in the pack the install goes (default: its root)
  install_components  the install components to take (default: all of it)
  licenses            {name: [files]}: more licence files, {<download name>} and {<sdk name>} allowed

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
from _common import ASSET_PLATFORM, LINUX, MACOS, REPO, VS_ARCH, VS_GENERATOR, WINDOWS, copy_file, copy_tree, fail, remove_tree, run  # noqa: E402
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


def _relative_runpaths(pack_dir, usd):
    """Linux: makes the pack's binaries find their libraries from wherever the pack
    is, over an OpenUSD install or in a deploy. Two kinds of file are changed:

      - what the build linked by absolute path (the OpenUSD install's lib folder):
        that is pointed at the same place relative to the file;
      - what needs a library of the lib folder, the pack's or the install's, with a
        RUNPATH that doesn't lead there. Adobe's plugins are installed with
        "$ORIGIN/." only, and the library they share is in lib.

    Both get the file's own folder and the lib folder, relative to $ORIGIN. A
    prebuilt library that came with an SDK is neither and is left as it is.
    Returns the files it changed."""
    from _elf import elf_files, needed, origin_relative, runpath, set_runpath
    lib = pack_dir / "lib"
    in_lib = {file.name for folder in (lib, usd / "lib") if folder.is_dir() for file in folder.iterdir()}

    def folder_of(entry, file):
        origin = str(file.parent)
        return os.path.normpath(entry.replace("${ORIGIN}", origin).replace("$ORIGIN", origin))

    changed = []
    for file in elf_files(pack_dir):
        entries = runpath(file)
        kept = [entry for entry in entries if not entry.startswith("/")]
        reached = {folder_of(entry, file) for entry in kept}
        if len(kept) == len(entries) and (
                os.path.normpath(lib) in reached or not any(name in in_lib for name in needed(file))):
            continue
        for entry in ("$ORIGIN", origin_relative(file, lib)):
            if folder_of(entry, file) not in reached:
                kept.append(entry)
                reached.add(folder_of(entry, file))
        set_runpath(file, kept)
        changed.append(file.relative_to(pack_dir).as_posix())
    return changed


def _relative_rpaths(pack_dir, usd):
    """macOS: as _relative_runpaths(), and what the pack links by an absolute path in the
    OpenUSD install or the pack (a library without an @rpath name) becomes @rpath too."""
    from _macho import edit, install_id, is_system, loader_relative, macho_files, needed, rpaths
    lib = pack_dir / "lib"
    in_lib = {file.name for folder in (lib, usd / "lib") if folder.is_dir() for file in folder.iterdir()}

    def folder_of(entry, file):
        return os.path.normpath(entry.replace("@loader_path", str(file.parent)))

    changed = []
    for file in macho_files(pack_dir):
        # And libpython, which is found as OpenUSD's own libraries find it (relocate_usd.py).
        links = {name: "@rpath/" + Path(name).name for name in needed(file)
                 if name.startswith("/") and not is_system(name)
                 and (Path(name).name in in_lib or Path(name).name.startswith("libpython3"))}
        own = install_id(file)
        entries = rpaths(file)
        kept = [entry for entry in entries if not entry.startswith("/")]
        reached = {folder_of(entry, file) for entry in kept}
        wants_lib = any(Path(name).name in in_lib for name in needed(file) if not is_system(name))
        if len(kept) != len(entries) or (wants_lib and os.path.normpath(lib) not in reached):
            for entry in ("@loader_path", loader_relative(file, lib)):
                if folder_of(entry, file) not in reached:
                    kept.append(entry)
                    reached.add(folder_of(entry, file))
        if edit(file, new_rpaths=kept, new_id="@rpath/" + file.name if own and own.startswith("/") else None, changes=links):
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


def _submodules(source, only=None):
    """Checks out the checkout's submodules, at the commits it names, or only the
    folders `only` lists, without their Git LFS files; their paths."""
    update = ["git", "-C", str(source), "submodule", "update", "--init", "--recursive"]
    paths = []
    if only:
        update = ["git", "-C", str(source), "-c", "filter.lfs.smudge=", "-c", "filter.lfs.process=",
                  "-c", "filter.lfs.required=false", "submodule", "update", "--init"]
        paths = ["--", *only]
    sys.stdout.flush()
    if subprocess.run(update + ["--depth", "1"] + paths).returncode != 0:
        # A server that doesn't hand out a commit by itself: the whole history then.
        run(update + paths)
    status = run(["git", "-C", source, "submodule", "status", "--recursive"], capture_output=True, text=True)
    # An entry that isn't checked out starts with "-".
    return [line.split()[1] for line in status.stdout.splitlines()
            if len(line.split()) > 1 and not line.startswith("-")]


def _submodule_patches(spec, source):
    """Applies the pin's patches that name a submodule, once."""
    for entry in spec.get("patches", []):
        if not isinstance(entry, dict):
            continue
        patch, folder = (usd_plugin_asset.PINS / entry["patch"]).resolve(), source / entry["in"]
        if subprocess.run(["git", "-C", folder, "apply", "--reverse", "--check", patch], capture_output=True).returncode == 0:
            print(f"{patch.name}: already applied")
        else:
            run(["git", "-C", folder, "apply", "--verbose", patch])


def _hook(spec):
    """The pin's build hook, as a module."""
    import importlib.util
    file = usd_plugin_asset.PINS / spec["hook"]
    loader = importlib.util.spec_from_file_location(f"usd_plugin_hook_{file.stem}", file)
    module = importlib.util.module_from_spec(loader)
    loader.loader.exec_module(module)
    return module


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


def _sdks(spec, pack, given):
    """The folders of the SDKs the pin names, from --sdk <name>=<folder>, checked."""
    folders = {}
    for entry in given:
        name, separator, folder = entry.partition("=")
        if not separator or not name or not folder:
            fail(f"--sdk {entry}: give it as <name>=<folder>")
        folders[name] = _folder(folder)
    wanted = spec.get("sdks", {})
    for name in sorted(set(folders) - set(wanted)):
        fail(f"--sdk {name}: the {pack} pack needs no SDK of that name"
             + (f" (it needs {', '.join(sorted(wanted))})" if wanted else ""))
    for name, sdk in wanted.items():
        if name not in folders:
            fail(f"The {pack} pack needs the {sdk['description']}, which can't be downloaded: "
                 f"pass --sdk {name}=<its folder>")
        files = sdk.get("files", [])
        for file in files.get(ASSET_PLATFORM, []) if isinstance(files, dict) else files:  # by platform, or the same on all
            if not (folders[name] / file).is_file():
                fail(f"{folders[name]} isn't the {sdk['description']}: it has no {file}")
    return folders


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("pack", help="the pack to build, as in deps/usd-plugins/<pack>.json")
    parser.add_argument("--usd-install", type=Path, required=True, help="the OpenUSD install to build against")
    parser.add_argument("--work", type=Path,
                        help="for the checkout, the downloads and the build tree (default: build/usd-plugins/<pack>)")
    parser.add_argument("--pack-dir", type=Path, help="where the pack goes (default: <work>/pack)")
    parser.add_argument("--install", action="store_true", help="also copy the pack over the OpenUSD install")
    parser.add_argument("--jobs", type=int, help="parallel jobs (default: the build tool's)")
    parser.add_argument("--sdk", action="append", default=[], metavar="NAME=FOLDER",
                        help="an SDK the pin's \"sdks\" names, which can't be downloaded; repeatable")
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
    sdks = _sdks(spec, pack, args.sdk)
    work = _folder(args.work) if args.work else REPO / "build" / "usd-plugins" / pack
    pack_dir = _folder(args.pack_dir) if args.pack_dir else work / "pack"
    source = work / "src" / pack  # alone in src/, which is how usd_licenses.collect() names it
    build = work / "build"

    _git_long_paths()
    _pristine(source)
    # The submodules' patches are applied below, in their own checkouts.
    fetch({**spec, "patches": [patch for patch in spec.get("patches", []) if not isinstance(patch, dict)]},
          usd_plugin_asset.PINS, source)
    only = spec["submodules"] if isinstance(spec.get("submodules"), list) else None
    submodules = _submodules(source, only) if spec.get("submodules") else []
    for folder in submodules:
        _pristine(source / folder)
    _submodule_patches(spec, source)
    _empty_pack_dir(pack_dir, pack)

    places = {"usd": usd.as_posix(), "pack": pack_dir.as_posix(), **{name: folder.as_posix() for name, folder in sdks.items()}}
    downloads = {}
    for name, platforms in spec.get("downloads", {}).items():
        if ASSET_PLATFORM not in platforms:
            fail(f"deps/usd-plugins/{pack}.json: the download '{name}' has no entry for {ASSET_PLATFORM}")
        if platforms[ASSET_PLATFORM] is None:  # not needed on this platform
            continue
        downloads[name] = platforms[ASSET_PLATFORM]
        places[name] = _download(name, downloads[name], work / "downloads").as_posix()

    prefix = pack_dir / spec.get("install_prefix", "")
    hook_licences = {}
    if spec.get("hook"):
        from types import SimpleNamespace
        hook_licences = _hook(spec).build(SimpleNamespace(
            source=source, work=work, usd=usd, prefix=prefix, jobs=args.jobs, run=run,
            places={name: Path(folder) for name, folder in places.items()},
            python_root=Path(sys.base_prefix).as_posix())) or {}
    else:
        _cmake_build(spec, pack, source, build, prefix, places, args.jobs)
    _finish(spec, pack, args, usd, work, pack_dir, source, build, submodules, places, downloads, hook_licences)


def _cmake_build(spec, pack, source, build, prefix, places, jobs):
    """Configures, builds and installs the pin's one CMake project into `prefix`."""
    command = ["cmake", "-S", source, "-B", build]
    if WINDOWS:
        # The toolset OpenUSD and usdcc are built with (docs/CI.md section 2).
        command += ["-G", VS_GENERATOR, "-A", VS_ARCH]
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
    run(["cmake", "--build", build, "--config", "Release", "--parallel", *([str(jobs)] if jobs else []),
         *(["--target", *spec["targets"]] if spec.get("targets") else [])])
    for component in spec.get("install_components") or [None]:
        run(["cmake", "--install", build, "--config", "Release", "--prefix", prefix,
             *(["--component", component] if component else [])])


def _finish(spec, pack, args, usd, work, pack_dir, source, build, submodules, places, downloads, hook_licences):
    """Prunes the pack, makes it relocatable, collects its licences and writes its record."""
    _prune(pack_dir)
    if not pack_files(pack_dir):
        fail(f"The {pack} build installed nothing that runs into {pack_dir}")
    if MACOS:
        print(f"rpaths relative to @loader_path: {', '.join(_relative_rpaths(pack_dir, usd)) or 'none to change'}", flush=True)
    if LINUX and not spec.get("hook"):  # a hook sets its binaries' own
        print(f"RUNPATHs relative to $ORIGIN: {', '.join(_relative_runpaths(pack_dir, usd)) or 'none to change'}", flush=True)

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
    for name, folder in hook_licences.items():
        if usd_licenses._copy_licences(Path(folder), licences / name):
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
    sources += [f"patch:   deps/usd-plugins/{usd_plugin_asset.patch_file(patch)}"
                + (f" (in {patch['in']})" if isinstance(patch, dict) else "") for patch in spec.get("patches", [])]
    if spec.get("hook"):
        sources.append(f"built by: deps/usd-plugins/{spec['hook']}")
    sources += [f"{name}: {entry['url']} (SHA-256 {entry['sha256']})" for name, entry in downloads.items()]
    # Not the folder: it's this machine's, and the SDK isn't in the pack.
    sources += [f"{name}: the {sdk['description']}, given to the build; not in the pack"
                for name, sdk in spec.get("sdks", {}).items()]
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
