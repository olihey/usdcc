"""Copies everything usdcc needs to run into one folder, to distribute it.

Collects a built usdcc (run tools/build.py first) with its runtime dependencies
into a self-contained folder that runs on a machine without the dev environment:

    <destination>\\
      usdcc.exe     starts bin\\usdcc.exe; what the installer's shortcuts start
                    and Velopack runs for its hooks (docs/UPDATES.md section 3),
                    when the build has Velopack
      usdcc.cmd     starts bin\\usdcc.exe
      LICENSE.txt   usdcc's licence (BSD-3-Clause)
      licenses\\     the licences of everything else in the folder, and in
                    SOURCES.txt where each library's source is
                    (docs/RELEASE.md section 6)
      bin\\          usdcc.exe, its DLLs (app, Qt, vcpkg, OpenUSD and the
                    libraries OpenUSD uses, Velopack's), Qt's platform plugin
      bin\\usd\\      OpenUSD's built-in plugins (schemas, Hydra, MaterialX
                    standard library, ...)
      plugin\\usd\\   OpenUSD's other plugins (Storm, Embree, image readers),
                    and those of the USD plugin packs (file formats)
      usd-plugins\\ a file per USD plugin pack: what it is and which files are its
      python\\       the Python OpenUSD was built with (Lib, DLLs, and python.exe,
                    which uv needs to install packages), and in
                    python\\Lib\\site-packages: usdcc, pxr, PySide6, shiboken6
      plugins\\      V-Ray Render and, when enabled, native RTX Render
      resources\\preview\\  the shapes and HDRIs the Material Editor's preview
                    ships with (docs/MATERIAL_EDITOR.md section 7)
      resources\\workspaces\\  the shipped workspaces (docs/WORKSPACES.md section 5)
      examples\\plugins\\   the example plugins, loaded with --plugin-path or
                    added in Settings > Plugins
      sdk\\          headers, import libraries and CMake files for C++ plugins
      tools\\ruff\\   ruff.exe and its LICENSE, for the Script Editor's Ruff
                    Format and Ruff Check (the build's pinned version)
      tools\\debugpy\\  debugpy, for attaching a debugger to its Python
      tools\\usdcc_peers\\  the Peers package for other programs and the studio's
                    hub (docs/PEERS.md section 9), to pip install where it runs
      tools\\uv\\     uv.exe and its licenses, which install the Python packages
                    plugins ask for (the build's pinned version)

OpenUSD finds its plugins relative to usd_plug.dll (bin\\usd and ..\\plugin\\usd),
and usdcc uses python\\ instead of the build's Python when it finds it next to
bin\\, so nothing needs setting up on the target machine. The folder is replaced
if it holds an earlier deploy; any other non-empty folder is refused, unless
--clean says to delete it first, whatever it holds.

On Linux the folder is a prefix of its own, which is also the Python's
(docs/LINUX_WSL.md):

    <destination>/
      usdcc         starts bin/usdcc
      bin/          usdcc, usdcc_node with its web page (node-web), and python3.XY
                    (with a python3 link), which uv needs
      lib/          usdcc's, Qt's, vcpkg's and OpenUSD's libraries, libpython
      lib/usd/      OpenUSD's built-in plugins
      lib/python3.XY/   the standard library, and in site-packages: usdcc, pxr
      Qt6/plugins/  Qt's plugins (platforms: xcb, wayland, offscreen, ...)
      plugin/usd/   OpenUSD's other plugins, and those of the USD plugin packs,
                    whose libraries are in lib/
      usd-plugins/  a file per USD plugin pack, as on Windows
      plugins/, examples/plugins/, resources/preview/, resources/workspaces/, tools/ruff/, tools/uv/,
      sdk/ (its lib/ links to the libraries in lib/), LICENSE.txt, licenses/

Every ELF file's RUNPATH is rewritten relative to $ORIGIN (with patchelf), so the
folder runs from wherever it is unpacked.

--optional-packs DIR (Linux and macOS): the packs of the optional set in the install
(MoonRay) are deployed too and checked with the rest, then moved to
DIR/<pack>/<destination's name>/, laid out as the deploy is. That folder isn't
shipped: --pack-folders makes from it the pack usdcc downloads and installs, which
is the only way a release has it. A pack's files under plugin/ keep the RUNPATHs
its build gave them, which are relative to an OpenUSD install laid out as the
deploy is. On macOS DIR/<pack>/<destination's name>/ has a whole usdcc.app with the
pack in it instead, checked and signed as one, so each of the pack's binaries has
its own signature when --pack-folders takes them from it.

--pack-folders DIR: each of those packs also as the folder usdcc installs when it
downloads one, DIR/<pack>/ (docs/USD_PLUGINS.md section 10). And the packs of the local
set whose pin says "publish": true (Arnold): plugins built against an SDK that isn't
theirs to ship, which load only once the user names their own copy of it. Those too
are a download in the app and nothing else, and --optional-packs leaves no folder of theirs.

    python tools/deploy.py                       into .\\deploy
    python tools/deploy.py D:\\dist\\usdcc --configuration Release
    python tools/deploy.py D:\\dist\\usdcc --clean
    python tools/deploy.py --usd-install C:\\usd\\install

On macOS the folder holds usdcc.app (docs/MACOS.md):

    <destination>/usdcc.app/Contents/
      Info.plist
      MacOS/        usdcc and usdcc_node
      Resources/    what a Linux deploy's folder has (lib/, bin/ with the Python,
                    Qt6/plugins/, plugin/usd/, plugins/, sdk/, licenses/, ...), usdcc.icns,
                    node-web/, and the qt.conf that says where Qt's plugins are

Everything is in Resources, not Frameworks: OpenUSD and Python find their data beside their
libraries, and codesign takes only signed code in Frameworks. The Mach-O files' rpaths are
relative to @loader_path, the Python is compiled (a __pycache__ written later would break
the signature), and everything is signed, ad hoc unless --sign names an identity.

Windows, Linux and macOS. Standard library only, patchelf on Linux and Xcode on macOS.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
import usd_licenses  # noqa: E402
import build_vsix  # noqa: E402
from _common import (  # noqa: E402
    CONFIGURATIONS, MACOS, REPO, VS_ARCH, WINDOWS, cache_on, cache_value, copy_file, copy_tree, fail, remove_tree,
    vcpkg_triplet, warn)

BUILD_DIR = REPO / "build"

# What only a build needs, next to the files a deploy takes.
BUILD_ONLY_FILES = ["*.pdb", "*.lib", "*.exp", "*.ilk"]

# plugins\ is where the app always looks for the plugins it ships (docs/PLUGINS.md).
# The examples sit apart, so they don't load unless asked: usdcc.cmd --plugin-path
# examples\plugins, or Settings > Plugins.
SHIPPED_PLUGINS = ["vray_render"]
# PySide6's headers, typesystems and import libraries, only for building bindings.
PYSIDE_BUILD_ONLY = ["__pycache__", "include", "typesystems", "glue", "lib", "doc", "scripts"]
# Built ones, from build/plugins: Jolt Physics, with Jolt's library and licence in its
# folder (docs/PHYSICS.md section 4.1). MIT, so in every artifact. And the protocols of
# remote assets, http, webdav and s3 (docs/REMOTE_ASSETS.md section 11): a usd/ folder each,
# on bin's usdcc_assets, libcurl, zlib and pugixml. Not beside the plugin: OpenUSD loads
# a plugin's library with plain LoadLibraryW, which doesn't look there.
SHIPPED_NATIVE_PLUGINS = ["physics_jolt", "http", "webdav", "s3", "viewport_broadcast", "uv_view"]

# Qt modules offered only under GPL-3.0 or a commercial licence, with no LGPL option
# (Qt's documentation of each module). The standard release is BSD-3-Clause and ships
# RTX Render, which loads NVIDIA's proprietary runtime into the same process, so none
# of these may be in it (docs/RELEASE.md section 6); Qt HTTP Server was, from 26.11.1 to
# 26.11.11. Only the GPL-3.0 configuration (--with-blender-mesh-tools) may have one.
# Matched by library name, PySide6's bindings too: Qt6Charts covers Qt6ChartsQml, Qt6Quick3D its
# physics and XR.
GPL_ONLY_QT_MODULES = [
    "HttpServer", "Charts", "DataVisualization", "Graphs", "VirtualKeyboard", "Quick3D", "QuickTimeline",
    "Bodymovin", "Lottie", "Coap", "Mqtt", "OpcUa", "WaylandCompositor", "Protobuf", "Grpc", "ApplicationManager",
    "InterfaceFramework", "QuickEffectMaker",
]
_GPL_ONLY_QT = re.compile(
    r"^(?:lib)?Qt6?(?:" + "|".join(GPL_ONLY_QT_MODULES) + r")\w*"
    r"(?:\.framework|(?:\.[\w-]+)*\.(?:dll|so|dylib|pyd)(?:\.\d+)*)$", re.IGNORECASE)


def gpl_only_qt_libraries(root):
    """The libraries of GPL-only Qt modules (GPL_ONLY_QT_MODULES) anywhere under `root`,
    relative to it."""
    root = Path(root)
    return sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if _GPL_ONLY_QT.match(path.name))


def check_no_gpl_only_qt(root, args):
    """Fails a deploy of the standard configuration that has a GPL-only Qt module."""
    if getattr(args, "with_blender_mesh_tools", False):
        return
    found = gpl_only_qt_libraries(root)
    if found:
        fail("The deploy has Qt modules that are GPL-3.0 only, which the standard (BSD-3-Clause) release "
             "can't ship (docs/RELEASE.md section 6):\n  " + "\n  ".join(found[:40]))


def _build_features():
    """build/features.json, which the configure writes (docs/BUILD_OPTIONS.md section 4):
    each feature, whether this build has it, and its plugin folders."""
    path = BUILD_DIR / "features.json"
    if not path.is_file():
        fail(f"No {path}: configure the build again (docs/BUILD_OPTIONS.md).")
    return json.loads(path.read_text(encoding="utf-8"))["features"]


def _has_feature(name):
    """Whether this build has the feature `name` (docs/BUILD_OPTIONS.md)."""
    return any(feature["name"] == name and feature["included"] for feature in _build_features())


def _left_out_folders():
    """The plugin folders of the features this build was configured without."""
    return {folder for feature in _build_features() if not feature["included"] for folder in feature["folders"]}


def _shipped(plugins):
    """Those of `plugins` this build has: a feature left out isn't deployed."""
    left_out = _left_out_folders()
    return [plugin for plugin in plugins if plugin not in left_out]


def _full(path):
    return os.path.abspath(path).rstrip("\\/")


def _same(first, second):
    return os.path.normcase(first) == os.path.normcase(second)


def _names(folder, pattern):
    return sorted(file.name for file in Path(folder).glob(pattern) if file.is_file())


def _rtx_runtime_roots():
    """The pinned native SDK roots to bundle, checked against deps/rtx-native.json."""
    pins = json.loads((REPO / "deps/rtx-native.json").read_text(encoding="utf-8-sig"))
    roots = {}
    for name in ("ovrtx", "ovstage"):
        root = Path(cache_value(BUILD_DIR, f"USDCC_RTX_{name.upper()}_ROOT") or "")
        info = json.loads((root / f"{name}-build-info.json").read_text(encoding="utf-8-sig"))
        loader = hashlib.sha256((root / f"bin/{name}-dynamic.dll").read_bytes()).hexdigest()
        if (info.get("build_string", "").lower() != pins[name]["build"].lower()
                or loader.lower() != pins[name]["loader_sha256"].lower()):
            fail(f"The {name} runtime does not match deps/rtx-native.json.")
        for licence in ("LICENSE", "THIRD-PARTY-NOTICES.txt"):
            if not (root / licence).exists():
                fail(f"Missing {name}/{licence}")
        roots[name] = root
    return roots


def _vcpkg_installed():
    """vcpkg's install tree: the build's own, or the one it was configured with, as a
    worktree's build shares the main checkout's (VCPKG_INSTALLED_DIR)."""
    return Path(cache_value(BUILD_DIR, "VCPKG_INSTALLED_DIR") or BUILD_DIR / "vcpkg_installed")


def _vcpkg_licences(licenses_dir, sources):
    """vcpkg's packages: a copyright file each, and in `sources` where its source
    is, from the SPDX file vcpkg writes (the LGPL ones, Qt and Qt Advanced Docking
    System, have to say so). vcpkg's own build helpers aren't shipped."""
    share = _vcpkg_installed() / vcpkg_triplet(BUILD_DIR) / "share"
    destination = licenses_dir / "vcpkg"
    destination.mkdir(parents=True, exist_ok=True)
    for port in sorted((entry for entry in share.iterdir() if entry.is_dir()), key=lambda entry: entry.name):
        if port.name.startswith("vcpkg-") or port.name == "pkgconf":
            continue
        if not (port / "copyright").is_file():
            continue
        copy_file(port / "copyright", destination / f"{port.name}.txt")
        spdx = port / "vcpkg.spdx.json"
        if spdx.is_file():
            packages = json.loads(spdx.read_text(encoding="utf-8-sig")).get("packages", [])
            if not packages:
                continue
            location = re.sub(r"^git\+", "", packages[0].get("downloadLocation", ""))
            sources.append(f"{port.name} {packages[0].get('versionInfo', '')}")
            sources.append(f"  vcpkg port: {location}")
            for package in packages[1:]:
                location = package.get("downloadLocation")
                if location and location != "NONE":
                    sources.append("  source:     " + re.sub(r"^git\+", "", location))


# OpenUSD's own plugins in plugin/usd, with deps/usd.json's build_usd.py flags.
# Reviewed against the pinned OpenUSD source, not read off the install: a local
# one can hold a third-party renderer someone installed into it by hand.
OPENUSD_PLUGINS = ("hdStorm", "hdEmbree", "hioAvif", "hioImageIO", "hioOiio", "hioOpenEXR", "sdrGlslfx", "usdAbc", "usdDraco",
                   "usdShaders")

# Where the USD plugin packs' own files are, in an OpenUSD install and in a deploy.
USD_PACKS_DIR = "usd-plugins"
# The packs the GPL-3.0 configuration (--with-blender-mesh-tools) ships, when the
# OpenUSD install has them: reviewed for it, by the exact build, in
# docs/BLENDER_DISTRIBUTION_REVIEW.md and BLENDER_LINUX_DISTRIBUTION_REVIEW.md
# (docs/USD_PLUGINS.md section 5). The standard set isn't among them: that review
# doesn't cover it.
GPL_CONFIGURATION_PACKS = ("gatling",)


def _openusd_plugins(source, destination, copier):
    """Copies OpenUSD's own plugins from its plugin/usd, and nothing else in it."""
    destination.mkdir(parents=True, exist_ok=True)
    for name in OPENUSD_PLUGINS:
        if (source / name).is_dir():
            copier(source / name, destination / name)
        for suffix in (".dll", ".so", ".dylib"):
            if (source / (name + suffix)).is_file():
                copy_file(source / (name + suffix), destination)
    if (source / "plugInfo.json").is_file():
        copy_file(source / "plugInfo.json", destination)


def _usd_packs(usd_install):
    """The USD plugin packs unzipped over the OpenUSD install (docs/USD_PLUGINS.md),
    by name: each one's usd-plugins/<pack>.json, which tools/build_usd_plugin.py writes."""
    folder = usd_install / USD_PACKS_DIR
    if not folder.is_dir():
        return {}
    return {file.stem: json.loads(file.read_text(encoding="utf-8")) for file in sorted(folder.glob("*.json"))}


def _shipped_packs(usd_packs, gpl_configuration):
    """The packs a deploy ships in it: the standard set, or for the GPL-3.0
    configuration the packs reviewed for it."""
    if gpl_configuration:
        return {pack: manifest for pack, manifest in usd_packs.items() if pack in GPL_CONFIGURATION_PACKS}
    return {pack: manifest for pack, manifest in usd_packs.items() if manifest.get("set") == "standard"}


def _pack_pin(pack):
    """deps/usd-plugins/<pack>.json, or {} for a pack this checkout doesn't pin."""
    file = REPO / "deps/usd-plugins" / f"{pack}.json"
    return json.loads(file.read_text(encoding="utf-8-sig")) if file.is_file() else {}


def _app_only(pack, manifest):
    """Whether the pack is one of the local set that a release offers all the same: its pin
    says "publish": true. Its plugins need an SDK usdcc can't ship (Arnold's core), so it is
    built where that SDK is, never loads in a deploy by itself, and ships only as a download
    in the app (docs/USD_PLUGINS.md sections 5 and 10)."""
    return manifest.get("set") == "local" and _pack_pin(pack).get("publish") is True


def _download_packs(usd_packs, app_only):
    """The packs a release has as downloads of their own: the optional set and, with
    `app_only`, the published ones of the local set."""
    return {pack: manifest for pack, manifest in usd_packs.items()
            if manifest.get("set") == "optional" or (app_only and _app_only(pack, manifest))}


def _sdk_libraries(pack, platform):
    """The names of the libraries the user's own SDK brings and usdcc loads before the
    pack's plugins (the pin's check.preload: Arnold's core). No deploy has them."""
    return sorted(Path(entry).name for entry in _pack_pin(pack).get("check", {}).get("preload", {}).get(platform, []))


def _pack_dlls(packs):
    """The names of the packs' DLLs (.so files on Linux) among OpenUSD's own, in its
    bin and lib."""
    return [Path(file).name for manifest in packs.values() for file in manifest["files"]
            if file.count("/") == 1 and file.split("/")[0] in ("bin", "lib")]


def _deploy_usd_pack(pack, manifest, usd_install, deploy_dir, sources, libraries="bin"):
    """Copies a pack's files to where a deploy has such files, and writes the
    deploy's usd-plugins/<pack>.json, which lists them there. `libraries` is the
    folder OpenUSD's own libraries are in: bin on Windows, lib on Linux."""
    licences = f"licenses/{USD_PACKS_DIR}/{pack}/"
    deployed = []
    for file in manifest["files"]:
        folder = file.split("/")[0]
        if folder in ("bin", "lib") and file.count("/") == 1:
            target = f"{libraries}/{Path(file).name}"  # with OpenUSD's own libraries
        elif folder == "plugin":
            target = file
        elif file.startswith(licences):
            target = f"licenses/OpenUSD-plugins/{pack}/{file[len(licences):]}"
        else:
            fail(f"The {pack} pack has a file tools/deploy.py has no place for: {file}")
        (deploy_dir / target).parent.mkdir(parents=True, exist_ok=True)
        if (usd_install / file).is_symlink():  # libfoo.so -> libfoo.so.1 stays a link, beside it
            (deploy_dir / target).unlink(missing_ok=True)
            os.symlink(os.readlink(usd_install / file), deploy_dir / target)
        else:
            copy_file(usd_install / file, deploy_dir / target)
        deployed.append(target)
    record = {key: manifest.get(key, "") for key in ("pack", "asset", "repository", "commit", "version", "license")}
    record["files"] = sorted(deployed)
    (deploy_dir / USD_PACKS_DIR).mkdir(parents=True, exist_ok=True)
    (deploy_dir / USD_PACKS_DIR / f"{pack}.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8",
                                                           newline="\n")
    sources.append(f"{pack} {record['version']} ({record['license']}), a USD plugin pack built by "
                   "tools/build_usd_plugin.py (https://github.com/olihey/usdcc)")
    sources.append(f"  source:     {record['repository']} at {record['commit']}")


def _pack_folder(pack, source_root, names, target, version, windows):
    """Writes an optional pack as usdcc installs one it downloads (docs/USD_PLUGINS.md
    section 10): `target`, a plugin folder laid out as an install, which
    tools/ci/pack_index.py archives. `names` are the pack's files under `source_root`,
    placed as a deploy has them.

    The folder lives in the user's data folder, not in usdcc's. On Windows its DLLs
    are in its own bin, which usdcc puts on its PATH. Elsewhere its lib is a link
    usdcc makes to its own libraries, where the files' RUNPATHs lead: so a pack
    can't have files of its own in lib, and has to keep them under plugin/."""
    source_root = Path(source_root)
    target = Path(target)
    if target.exists():
        remove_tree(target)
    in_lib = [name for name in names if not windows and name.split("/")[0] in ("bin", "lib")]
    if in_lib:
        fail(f"The {pack} pack has files in lib/, where an installed pack has the link to usdcc's own libraries: "
             f"{', '.join(in_lib[:5])}. Its build has to keep them under plugin/usd/ (deps/usd-plugins/{pack}.*), "
             "as MoonRay's does (docs/USD_PLUGINS.md section 10).")
    # plugin/usd is what usdcc registers at its start. A pack that keeps its plugins in a
    # folder of its own (plugin/arnold) has usdcc register them itself, when it can.
    usd_folders = ["plugin/usd"] if any(name.startswith("plugin/usd/") for name in names) else []
    if not any(name.startswith("plugin/") for name in names):
        fail(f"The {pack} pack has nothing in plugin/, where a pack's plugins are.")
    for name in names:
        if name.split("/")[0] not in ("bin", "plugin", "licenses", USD_PACKS_DIR):
            fail(f"The {pack} pack has a file an installed pack has no place for: {name}")
        (target / name).parent.mkdir(parents=True, exist_ok=True)
        if (source_root / name).is_symlink():
            os.symlink(os.readlink(source_root / name), target / name)
        else:
            copy_file(source_root / name, (target / name).parent)
    # OpenUSD's own plugin/usd has this file; a folder of its own needs one too.
    plug_info = target / "plugin/usd/plugInfo.json"
    if usd_folders and not plug_info.exists():
        plug_info.write_text('{\n    "Includes": [ "*/resources/" ]\n}\n', encoding="utf-8", newline="\n")
    catalog = REPO / "deps/packs.json"
    shown = json.loads(catalog.read_text(encoding="utf-8")).get(pack, {}) if catalog.is_file() else {}
    manifest = {"id": f"org.usdcc.pack.{pack}", "name": shown.get("title", pack), "version": version or "0",
                "description": shown.get("description", ""), "api": 1, "usd": usd_folders}
    (target / "usdcc-plugin.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    size = sum(file.stat().st_size for file in target.rglob("*") if file.is_file() and not file.is_symlink())
    print(f"  {pack}: {target} ({size / 2**20:,.0f} MB), as usdcc installs it")
    return target


def _usd_notices(usd_install, licenses_dir):
    target = licenses_dir / "OpenUSD"
    target.mkdir(parents=True, exist_ok=True)
    home = cache_value(usd_install / "build/OpenUSD", "CMAKE_HOME_DIRECTORY")
    candidates = [usd_install / "licenses/OpenUSD", usd_install]
    candidates.append(usd_install.with_name(usd_install.name + "-work") / "OpenUSD")
    if home:
        candidates.append(Path(home))
    for source in candidates:
        if (source / "LICENSE.txt").is_file() and (source / "NOTICE.txt").is_file():
            for name in ("LICENSE.txt", "NOTICE.txt"):
                copy_file(source / name, target)
            return
    fail("OpenUSD LICENSE.txt/NOTICE.txt are missing. Rebuild the USD dependency notice bundle; "
         "the install root's MaterialX LICENSE/THIRD-PARTY.md are not OpenUSD's license.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("destination", nargs="?", default="deploy",
                        help="folder to deploy into (default: deploy, in the current folder)")
    parser.add_argument("--configuration", choices=CONFIGURATIONS, default="RelWithDebInfo",
                        help="build configuration to deploy (default: RelWithDebInfo)")
    parser.add_argument("--usd-install", metavar="DIR",
                        help="the OpenUSD install to take OpenUSD's files from (default: the one the build was "
                             "configured with). usdcc is compiled against that one, so another one gets a "
                             "warning: it may not run with it")
    parser.add_argument("--require-vsix", action="store_true",
                        help="fail without the VS Code extension (no npm, say), rather than warn; for releases")
    parser.add_argument("--clean", action="store_true",
                        help="delete the destination folder first, whatever it holds; without it only an earlier "
                             "deploy is replaced, and any other non-empty folder is refused")
    parser.add_argument("--omit-rtx", action="store_true",
                        help="omit RTX even when enabled in the CMake cache; ignore stale native build files")
    parser.add_argument("--bundle-rtx-runtime", action="store_true",
                        help="copy the pinned native SDK bin trees, metadata and licenses into RTX; otherwise "
                             "configure external runtime roots on the target")
    parser.add_argument("--optional-packs", metavar="DIR",
                        help="also deploy the optional set's USD plugin packs, each into DIR/<pack>/<name>/: on Linux "
                             "its checked files, on macOS a usdcc.app with it, signed; neither is shipped, "
                             "--pack-folders takes the packs from them (docs/USD_PLUGINS.md section 5)")
    parser.add_argument("--pack-folders", metavar="DIR",
                        help="also write the optional set's packs, and the published ones of the local set (Arnold), "
                             "as usdcc installs one it downloads, DIR/<pack>/, for tools/ci/pack_index.py "
                             "(docs/USD_PLUGINS.md section 10); on Linux and macOS with --optional-packs, whose "
                             "checked files they are")
    parser.add_argument("--with-blender-mesh-tools", action="store_true",
                        help="include the GPL-linked mesh tools configuration; excludes proprietary renderer integrations")
    parser.add_argument("--blender-source-bundle", metavar="FILE",
                        help="source candidate from tools/blender_mesh_package.py, copied beside the combined artifact")
    parser.add_argument("--sign", default="-", metavar="IDENTITY",
                        help="macOS: the signing identity, a Developer ID (default: an ad-hoc signature)")
    args = parser.parse_args(argv)
    if args.with_blender_mesh_tools:
        if args.bundle_rtx_runtime:
            fail("The Blender configuration cannot bundle the proprietary RTX runtime.")
        args.omit_rtx = True
    elif args.blender_source_bundle:
        fail("--blender-source-bundle requires --with-blender-mesh-tools")
    if args.pack_folders and not WINDOWS and not args.optional_packs:
        fail("--pack-folders needs --optional-packs here: the folders are made from the packs it deploys and checks.")
    if MACOS:
        deploy_macos(args)
        return
    if args.sign != "-":
        fail("--sign is macOS's.")
    if not WINDOWS:
        deploy_linux(args)
        return
    if args.optional_packs:
        fail("--optional-packs isn't for Windows: no optional pack is published for it.")

    configuration = args.configuration
    app_dir = BUILD_DIR / "src/cpp/app" / configuration
    deploy_dir = _full(args.destination)

    # --- what to deploy -------------------------------------------------------

    if not (app_dir / "usdcc.exe").is_file():
        fail(f"No {configuration} build at {app_dir}; run tools/build.py --configuration {configuration} first.")

    built_usd_install = cache_value(BUILD_DIR, "USD_INSTALL")
    if args.usd_install:
        if not os.path.isfile(os.path.join(args.usd_install, "lib/usd_plug.dll")):
            fail(f"No OpenUSD in '{args.usd_install}' (no lib\\usd_plug.dll).")
        usd_install = _full(args.usd_install)
        if not _same(usd_install, _full(built_usd_install) if built_usd_install else ""):
            warn(f"usdcc was built against the OpenUSD in '{built_usd_install}', not '{usd_install}'; the deploy "
                 f"may not run with it. Build with tools/build.py --usd-install '{args.usd_install}' first to match.")
    else:
        usd_install = built_usd_install
        if not usd_install or not os.path.isfile(os.path.join(usd_install, "lib/usd_plug.dll")):
            fail(f"OpenUSD not found (USD_INSTALL in build/CMakeCache.txt: '{usd_install}'); pass --usd-install.")
    usd_install = Path(usd_install)

    # The Python OpenUSD was built with, as cmake/modules/UsdccPython.cmake finds it.
    # pxrConfig.cmake may set Python3_EXECUTABLE as a plain variable, so it isn't
    # always cached; UsdccPython.cmake caches it as USDCC_PYTHON_EXECUTABLE_PATH.
    python_exe = cache_value(BUILD_DIR, "USDCC_PYTHON_EXECUTABLE_PATH") or cache_value(BUILD_DIR, "Python3_EXECUTABLE")
    if not python_exe or not os.path.isfile(python_exe):
        fail(f"OpenUSD's Python not found (USDCC_PYTHON_EXECUTABLE_PATH in build/CMakeCache.txt: '{python_exe}'); "
             "configure again.")
    python_home = Path(subprocess.run([python_exe, "-c", "import sys; print(sys.base_prefix)"],
                                      capture_output=True, text=True).stdout.strip())
    if not (python_home / "Lib/encodings/__init__.py").is_file():
        fail(f"No Python standard library in {python_home}.")

    pyside_dir = cache_value(BUILD_DIR, "USDCC_PYSIDE_DIR")
    has_pyside = bool(pyside_dir) and os.path.isfile(os.path.join(pyside_dir, "PySide6/__init__.py"))

    # The pinned ruff cmake/modules/UsdccRuff.cmake downloaded into build/ruff.
    ruff_exe = cache_value(BUILD_DIR, "USDCC_RUFF_EXE_PATH")
    has_ruff = bool(ruff_exe) and os.path.isfile(ruff_exe)

    # The pinned uv cmake/modules/UsdccUv.cmake downloaded into build/uv.
    uv_exe = cache_value(BUILD_DIR, "USDCC_UV_EXE_PATH")
    has_uv = bool(uv_exe) and os.path.isfile(uv_exe)

    # The pinned debugpy cmake/modules/UsdccDebugpy.cmake downloaded into build/debugpy.
    debugpy_dir = cache_value(BUILD_DIR, "USDCC_DEBUGPY_PATH")

    # The launcher at the top, built with Velopack (cmake/modules/UsdccVelopack.cmake),
    # and Velopack's licence, which that module downloaded beside its library.
    launcher_exe = BUILD_DIR / "src/cpp/launcher" / configuration / "usdcc.exe"
    has_launcher = launcher_exe.is_file()
    velopack_license = cache_value(BUILD_DIR, "USDCC_VELOPACK_LICENSE_PATH")
    has_velopack = has_launcher and bool(velopack_license) and os.path.isfile(velopack_license)
    has_updates = _has_feature("UPDATES")
    if has_launcher and not has_velopack:
        fail(f"The build has the launcher ({launcher_exe}) but not Velopack's licence ('{velopack_license}'); "
             "configure again.")

    package_dir = BUILD_DIR / "python/usdcc"
    if not (package_dir / "__init__.py").is_file():
        fail(f"The usdcc Python package is missing from {package_dir}; build first.")

    # --- the destination ------------------------------------------------------

    include_rtx = not args.omit_rtx and "rtx_render" not in _left_out_folders()
    native_rtx = BUILD_DIR / "plugins/rtx_render_native"
    if args.bundle_rtx_runtime and not include_rtx:
        fail("--bundle-rtx-runtime requires an enabled native RTX build without --omit-rtx.")
    if include_rtx and not (native_rtx / "bin/windows-x64/rtx_render_native.dll").is_file():
        fail("Native RTX is enabled but not built. Build rtx_render_native first, or pass --omit-rtx.")
    runtime_roots = _rtx_runtime_roots() if args.bundle_rtx_runtime else {}

    # Before replacing a deploy, reject roots and folders containing source/dependencies.
    for protected in (REPO, BUILD_DIR, usd_install, python_home):
        absolute = _full(protected)
        if _same(absolute, deploy_dir) or os.path.normcase(absolute).startswith(os.path.normcase(deploy_dir) + os.sep):
            fail(f"Unsafe deploy destination '{deploy_dir}' contains '{absolute}'.")
    if deploy_dir == os.path.splitdrive(deploy_dir)[0]:
        fail("Cannot deploy to a drive root.")
    deploy_dir = Path(deploy_dir)

    if deploy_dir.exists():
        if not deploy_dir.is_dir():
            fail(f"{deploy_dir} is a file, not a folder.")
        if args.clean:
            # Asked for, so whatever it holds; the checks above have kept it off
            # the folders the deploy is made from.
            print(f"Deleting {deploy_dir}")
            remove_tree(deploy_dir)
        elif (deploy_dir / "bin/usdcc.exe").is_file():
            print(f"Replacing the earlier deploy in {deploy_dir}")
            remove_tree(deploy_dir)
        elif any(deploy_dir.iterdir()):
            fail(f"{deploy_dir} exists and isn't an earlier deploy (no bin\\usdcc.exe); pick an empty or new folder, "
                 "or pass --clean to delete it first.")

    bin_dir = deploy_dir / "bin"
    python_dir = deploy_dir / "python"
    site_packages = python_dir / "Lib/site-packages"

    print(f"Deploying usdcc ({configuration}) to {deploy_dir}", flush=True)

    # --- bin: OpenUSD first, then the app's own files -------------------------
    # The app's DLLs win where both have one (z.dll, libpng16.dll, ...), as they
    # do when the build runs: its folder comes before OpenUSD's on the DLL path.

    # The USD plugin packs in the install (docs/USD_PLUGINS.md). The standard set is
    # shipped, file by file from each pack's list; the others' files stay behind.
    # The Blender configuration ships only the packs its review covers (Gatling).
    usd_packs = _usd_packs(usd_install)
    shipped_packs = _shipped_packs(usd_packs, args.with_blender_mesh_tools)
    pack_dlls = _pack_dlls(usd_packs)

    print(f"  OpenUSD libraries ({usd_install})", flush=True)
    copy_tree(usd_install / "lib", bin_dir, files="*.dll", top_level_only=True, exclude_files=pack_dlls)
    copy_tree(usd_install / "bin", bin_dir, files="*.dll", top_level_only=True,
              exclude_files=pack_dlls + (["tbb*_debug.dll", "tbbbind*.dll", "tbb_preview*.dll"]
                                         if args.with_blender_mesh_tools else []))

    print("  OpenUSD plugins", flush=True)
    copy_tree(usd_install / "lib/usd", bin_dir / "usd", exclude_files=BUILD_ONLY_FILES)
    _openusd_plugins(usd_install / "plugin/usd", deploy_dir / "plugin/usd", copy_tree)

    print("  usdcc", flush=True)
    # Without the Updates feature the app doesn't use Velopack's DLL: one an earlier build left
    # in the build folder stays there.
    copy_tree(app_dir, bin_dir, exclude_files=BUILD_ONLY_FILES + ([] if has_updates else ["velopack_libc.dll"]))
    if has_launcher:
        copy_file(launcher_exe, deploy_dir)
    elif (deploy_dir / "usdcc.exe").is_file():
        (deploy_dir / "usdcc.exe").unlink()  # an earlier deploy's, from a build that had it
    for dll in ("vcruntime140.dll", "vcruntime140_1.dll"):
        if not (bin_dir / dll).exists() and (python_home / dll).is_file():
            copy_file(python_home / dll, bin_dir)

    # --- python: the standard library and the packages ------------------------

    print(f"  Python ({python_home})", flush=True)
    copy_tree(python_home / "Lib", python_dir / "Lib",
              exclude_dirs=["site-packages", "__pycache__", "test", "idlelib", "tkinter", "turtledemo", "ensurepip"])
    copy_tree(python_home / "DLLs", python_dir / "DLLs", exclude_files=BUILD_ONLY_FILES)
    # uv can't work without an interpreter, not even to resolve. Its DLLs are in
    # bin, which the app puts on PATH when it runs uv (docs/PYTHON_PACKAGES.md).
    copy_file(python_home / "python.exe", python_dir)
    if (python_home / "LICENSE.txt").is_file():
        copy_file(python_home / "LICENSE.txt", python_dir)

    print("  Python packages: usdcc, pxr", flush=True)
    # The usdcc package folder also holds copies of DLLs for a plain python.exe;
    # the deployed app has them in bin (debug builds' *d.dll aren't needed at all).
    bin_dlls = {name.lower() for name in _names(bin_dir, "*.dll")}
    package_dlls = _names(package_dir, "*.dll")
    package_dll_set = {name.lower() for name in package_dlls}
    skip_dlls = [name for name in package_dlls
                 if name.lower() in bin_dlls
                 or (name.lower().endswith("d.dll") and len(name) > 5 and name[:-5].lower() + ".dll" in package_dll_set)]
    # Python imports an extension before a .py of the same name, so a module left
    # over from before it got a .py front (ui.pyd next to ui.py) would hide it.
    shadowing = [name for name in _names(package_dir, "*.pyd") if (package_dir / (Path(name).stem + ".py")).is_file()]
    for name in shadowing:
        warn(f"Leaving out {name}, a leftover build output that would hide {Path(name).stem}.py.")
    copy_tree(package_dir, site_packages / "usdcc", exclude_files=BUILD_ONLY_FILES + skip_dlls + shadowing,
              exclude_dirs=["__pycache__"])
    copy_tree(usd_install / "lib/site-packages", site_packages, exclude_files=BUILD_ONLY_FILES,
              exclude_dirs=["__pycache__"])

    if has_pyside:
        print("  Python packages: PySide6, shiboken6", flush=True)
        copy_tree(Path(pyside_dir) / "PySide6", site_packages / "PySide6",
                  exclude_files=BUILD_ONLY_FILES + ["rcc.exe", "uic.exe"], exclude_dirs=PYSIDE_BUILD_ONLY)
        copy_tree(Path(pyside_dir) / "shiboken6", site_packages / "shiboken6",
                  exclude_files=BUILD_ONLY_FILES, exclude_dirs=PYSIDE_BUILD_ONLY)
    else:
        warn(f"No PySide6 in '{pyside_dir}' (tools/build_pyside.py): the deploy works, but scripts can't make panels.")

    # --- tools ----------------------------------------------------------------

    if has_ruff:
        print("  ruff", flush=True)
        ruff_dir = deploy_dir / "tools/ruff"
        ruff_dir.mkdir(parents=True, exist_ok=True)
        copy_file(ruff_exe, ruff_dir)
        if (Path(ruff_exe).parent / "LICENSE").is_file():
            copy_file(Path(ruff_exe).parent / "LICENSE", ruff_dir)
    else:
        warn(f"No ruff in the build ('{ruff_exe}'; reconfigure with USDCC_BUNDLE_RUFF=ON while online): the deploy "
             "works, but Ruff Format and Ruff Check need a ruff on PATH.")

    if has_uv:
        print("  uv", flush=True)
        uv_dir = deploy_dir / "tools/uv"
        uv_dir.mkdir(parents=True, exist_ok=True)
        copy_file(uv_exe, uv_dir)
        for name in _names(Path(uv_exe).parent, "LICENSE*"):
            copy_file(Path(uv_exe).parent / name, uv_dir)
    else:
        warn(f"No uv in the build ('{uv_exe}'; reconfigure with USDCC_BUNDLE_UV=ON while online): the deploy "
             "works, but installing Python packages needs a uv on PATH.")

    _deploy_debugpy(debugpy_dir, deploy_dir, args)
    _deploy_peers_package(deploy_dir)
    _deploy_stubs(deploy_dir, args)
    _deploy_vsix(deploy_dir, args)

    # --- plugins --------------------------------------------------------------

    print("  Shipped plugins", flush=True)
    for plugin in ([] if args.with_blender_mesh_tools else _shipped(SHIPPED_PLUGINS)):
        copy_tree(REPO / "src/plugins" / plugin, deploy_dir / "plugins" / plugin, exclude_dirs=["__pycache__"])
    for plugin in _shipped(SHIPPED_NATIVE_PLUGINS):
        if not (BUILD_DIR / "plugins" / plugin / "usdcc-plugin.json").is_file():
            fail(f"The {plugin} plugin isn't built; build the usdcc target first.")
        copy_tree(BUILD_DIR / "plugins" / plugin, deploy_dir / "plugins" / plugin, exclude_files=BUILD_ONLY_FILES)
    if include_rtx:
        target = deploy_dir / "plugins/rtx_render"
        copy_tree(native_rtx, target, exclude_files=BUILD_ONLY_FILES + ["runtime.json"], exclude_dirs=["runtime"])
        for name, root in runtime_roots.items():
            runtime_target = target / "runtime" / name
            copy_tree(root / "bin", runtime_target / "bin",
                      exclude_files=["*.log", "crash_*.txt", "*.dmp", "ucode.elf"])
            for file in (f"{name}-build-info.json", "LICENSE", "THIRD-PARTY-NOTICES.txt"):
                copy_file(root / file, runtime_target)
    print("  Example plugins", flush=True)
    copy_tree(REPO / "src/plugins", deploy_dir / "examples/plugins",
              exclude_dirs=["__pycache__", "cpp_example", "rtx_render", "rtx_render_native", "blender_mesh_tools"]
              + SHIPPED_PLUGINS + SHIPPED_NATIVE_PLUGINS)
    copy_tree(BUILD_DIR / "plugins", deploy_dir / "examples/plugins", exclude_files=BUILD_ONLY_FILES,
              exclude_dirs=["rtx_render", "rtx_render_native", "blender_mesh_tools"] + SHIPPED_PLUGINS + SHIPPED_NATIVE_PLUGINS)
    if args.with_blender_mesh_tools:
        from blender_mesh_package import include_candidate
        include_candidate(deploy_dir, BUILD_DIR, args.blender_source_bundle)

    # --- Material Editor preview (docs/MATERIAL_EDITOR.md section 7) ----------

    # Next to bin\, where shippedPreviewFolder() looks: the shapes and HDRIs that
    # tools/gen_preview_assets.py made.
    print("  Material preview shapes and HDRIs", flush=True)
    copy_tree(REPO / "resources/preview", deploy_dir / "resources/preview")
    # Next to bin\, where WorkspaceLibrary::shippedFolder() looks (docs/WORKSPACES.md section 5).
    print("  Workspaces", flush=True)
    copy_tree(REPO / "resources/workspaces", deploy_dir / "resources/workspaces")

    # --- SDK for C++ plugins (docs/PLUGIN_SDK.md) -----------------------------

    print("  Plugin SDK", flush=True)
    sdk_dir = deploy_dir / "sdk"
    for library in ("core", "assets", "tools", "ui", "usd", "plugin_sdk"):
        copy_tree(REPO / "src/cpp" / library / "include", sdk_dir / "include")
    (sdk_dir / "lib").mkdir(parents=True, exist_ok=True)
    (sdk_dir / "cmake").mkdir(parents=True, exist_ok=True)
    for library in ("core/usdcc_core", "assets/usdcc_assets", "usd/usdcc_usd", "tools/usdcc_tools", "ui/usdcc_ui",
                    "usd/usdcc_render", "usd/usdcc_usd_ui"):
        folder, name = library.split("/")
        copy_file(BUILD_DIR / "src/cpp" / folder / configuration / f"{name}.lib", sdk_dir / "lib")
    copy_file(BUILD_DIR / "sdk/UsdccPluginConfig.cmake", sdk_dir)
    for module in ("UsdccPlugin.cmake", "FindUSD.cmake"):
        copy_file(REPO / "cmake/modules" / module, sdk_dir / "cmake")
    copy_tree(REPO / "src/plugins/cpp_example", sdk_dir / "example")

    # --- licences (docs/RELEASE.md section 6) ---------------------------------

    print("  Licences", flush=True)
    licenses_dir = deploy_dir / "licenses"
    licenses_dir.mkdir(parents=True, exist_ok=True)
    copy_file(REPO / "LICENSE", deploy_dir / "LICENSE.txt")

    # OpenUSD's own, and its dependencies': deps-usd.yml collects those into the
    # CI asset's licenses folder; a local build_usd.py install still has the
    # source and build folders to collect them from.
    _usd_notices(usd_install, licenses_dir)
    usd_dependencies = licenses_dir / "OpenUSD-dependencies"
    if (usd_install / "licenses").is_dir():
        copy_tree(usd_install / "licenses", usd_dependencies, exclude_dirs=[USD_PACKS_DIR])  # the packs' are apart
    elif (usd_install / "src").is_dir():
        found = usd_licenses.collect(usd_install / "src", usd_install / "build", usd_dependencies)
        print(f"Licences of {len(found)} of OpenUSD's dependencies: {', '.join(found)}", flush=True)
    else:
        warn(f"No licences of OpenUSD's dependencies in '{usd_install}' (no licenses or src folder): the deploy "
             "works, but can't be distributed as it is.")

    sources = [
        "Where the source of each library in this folder is. The LGPL ones (Qt, Qt Advanced Docking",
        "System, PySide6 and shiboken6) are separate DLLs, which you may replace with your own builds",
        "of the same version.",
        "",
    ]
    _vcpkg_licences(licenses_dir, sources)

    # Here, not with OpenUSD's plugins above: `sources` gets a line per pack.
    for pack, manifest in shipped_packs.items():
        print(f"  USD plugin pack: {pack} {manifest.get('version', '')}".rstrip(), flush=True)
        _deploy_usd_pack(pack, manifest, usd_install, deploy_dir, sources)
    # The optional set, as folders usdcc installs when it downloads one: not in the deploy.
    # Laid out first as a deploy has them, in a folder of their own, by the same function.
    pack_folders = Path(_full(args.pack_folders)) if args.pack_folders else None
    optional_packs = {} if not pack_folders or args.with_blender_mesh_tools else _download_packs(usd_packs, app_only=True)
    for pack, manifest in optional_packs.items():
        print(f"  USD plugin pack, a download of its own: {pack} {manifest.get('version', '')}".rstrip(), flush=True)
        staging = pack_folders / ".staging" / pack
        if staging.exists():
            remove_tree(staging)
        _deploy_usd_pack(pack, manifest, usd_install, staging, [])
        record = json.loads((staging / USD_PACKS_DIR / f"{pack}.json").read_text(encoding="utf-8"))
        _pack_folder(pack, staging, [*record["files"], f"{USD_PACKS_DIR}/{pack}.json"], pack_folders / pack,
                     manifest.get("version", ""), windows=True)
    if pack_folders and (pack_folders / ".staging").exists():
        remove_tree(pack_folders / ".staging")
    for pack in sorted(set(usd_packs) - set(shipped_packs) - set(optional_packs)):
        print(f"  USD plugin pack left out: {pack} (set: {usd_packs[pack].get('set')})", flush=True)

    if (python_home / "LICENSE.txt").is_file():
        (licenses_dir / "Python").mkdir(parents=True, exist_ok=True)
        copy_file(python_home / "LICENSE.txt", licenses_dir / "Python")
    if has_pyside:
        _pyside_licences(pyside_dir, python_exe, licenses_dir, sources)
    qwindowkit = REPO / "external/qwindowkit/LICENSE"
    if qwindowkit.is_file():
        (licenses_dir / "QWindowKit").mkdir(parents=True, exist_ok=True)
        copy_file(qwindowkit, licenses_dir / "QWindowKit")
    if has_velopack:
        # Its MIT licence; the Rust crates built into its library and Update.exe are
        # under permissive licences (and MPL-2.0), pinned by its Cargo.lock at the tag.
        (licenses_dir / "Velopack").mkdir(parents=True, exist_ok=True)
        copy_file(velopack_license, licenses_dir / "Velopack")
        version_file = Path(velopack_license).parent / "VERSION"
        velopack_version = version_file.read_text(encoding="utf-8").strip() if version_file.is_file() else ""
        sources.append(f"Velopack {velopack_version} ("
                       + ("bin/velopack_libc.dll, " if has_updates else "")
                       + "the launcher usdcc.exe, and an install's Update.exe)")
        sources.append(f"  source:     https://github.com/velopack/velopack/tree/{velopack_version}")
    for tool in ("ruff", "uv", "debugpy"):
        tool_dir = deploy_dir / "tools" / tool
        if tool_dir.is_dir():
            (licenses_dir / tool).mkdir(parents=True, exist_ok=True)
            for name in [*_names(tool_dir, "LICENSE*"), *_names(tool_dir, "ThirdPartyNotices*")]:
                copy_file(tool_dir / name, licenses_dir / tool)
    (licenses_dir / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")

    # --- launcher and checks --------------------------------------------------

    (deploy_dir / "usdcc.cmd").write_text('@echo off\nstart "" "%~dp0bin\\usdcc.exe" %*\n', encoding="ascii")

    required = [
        "bin/usdcc.exe", "bin/usdcc_node.exe", "bin/node-web/index.html", "bin/node-web/usdcc.css", "bin/usdcc_render.dll", "bin/usd_plug.dll", "bin/QtNodes.dll", "bin/platforms/qwindows.dll", "bin/usd/plugInfo.json",
        # What plugins on usdcc_peers load (docs/PEERS.md section 13).
        "bin/usdcc_peers.dll", "bin/Qt6Network.dll", "bin/Qt6WebSockets.dll",
        f"bin/libcrypto-3-{VS_ARCH.lower()}.dll", "bin/imageformats/qjpeg.dll", "bin/jpeg62.dll",
        "bin/libsodium.dll", "bin/tls/qschannelbackend.dll",
        "plugin/usd/hdStorm.dll", "python/python.exe", "python/Lib/encodings/__init__.py",
        "python/Lib/site-packages/usdcc/__init__.py", "python/Lib/site-packages/pxr/__init__.py",
        "python/Lib/site-packages/usdcc/_vendor/packaging/version.py",
        # Generating codeless schemas (docs/SCHEMAS.md section 5.3): usdGenSchema needs jinja2.
        "python/Lib/site-packages/usdcc/_vendor/jinja2/__init__.py",
        "python/Lib/site-packages/usdcc/_vendor/markupsafe/__init__.py",
        "python/Lib/site-packages/usdcc/_schema_check.py",
        *([f"plugins/{plugin}/usdcc-plugin.json" for plugin in _shipped(SHIPPED_PLUGINS)]
          if not args.with_blender_mesh_tools else []),
        "examples/plugins/measure_example/usdcc-plugin.json", "examples/plugins/cpp_example/usdcc-plugin.json",
        "sdk/UsdccPluginConfig.cmake", "sdk/lib/usdcc_ui.lib", "sdk/include/usdcc/plugin/plugin.h",
        "LICENSE.txt", "licenses/OpenUSD/LICENSE.txt", "licenses/OpenUSD/NOTICE.txt", "licenses/vcpkg/qtbase.txt", "licenses/SOURCES.txt",
        "bin/usdcc_assets.dll", "bin/libcurl.dll", "licenses/vcpkg/curl.txt", "licenses/vcpkg/pugixml.txt",
        "bin/pugixml.dll",
        # The remote asset protocols this build has: a usd/ folder each.
        *[f"plugins/{protocol}/usd/plugInfo.json" for protocol in _shipped(["http", "webdav", "s3"])],
    ]
    if has_velopack:
        required += ["usdcc.exe", "licenses/Velopack/LICENSE"] + (["bin/velopack_libc.dll"] if has_updates else [])
    if include_rtx:
        required += ["plugins/rtx_render/usdcc-plugin.json", "plugins/rtx_render/bin/windows-x64/rtx_render_native.dll"]
    missing = [name for name in required if not (deploy_dir / name).exists()]
    if missing:
        fail(f"The deploy is incomplete; missing: {', '.join(missing)}")
    check_no_gpl_only_qt(deploy_dir, args)

    # python.exe and uv together, as the app runs them: with bin on PATH for the DLLs.
    if has_uv:
        environment = dict(os.environ)
        environment["PATH"] = f"{bin_dir};{os.environ.get('SystemRoot', '')}\\System32"
        found = subprocess.run(
            [str(deploy_dir / "tools/uv/uv.exe"), "python", "find", "--no-config", "--no-python-downloads",
             str(python_dir / "python.exe")],
            env=environment, capture_output=True, text=True)
        if found.returncode != 0:
            fail(f"uv can't use the deploy's python.exe: {(found.stdout + found.stderr).strip()}")

    size = sum(file.stat().st_size for file in deploy_dir.rglob("*") if file.is_file())
    print(f"Done: {deploy_dir} ({size / 2**20:,.0f} MB). Start it with "
          f"{'usdcc.exe' if has_launcher else 'usdcc.cmd'} or bin\\usdcc.exe.")


def _deploy_debugpy(debugpy_dir, deploy_dir, args):
    """tools/debugpy, for attaching a debugger (docs/VSCODE.md section 5). Not with Blender mesh
    tools, whose configuration's review it isn't in (docs/BLENDER_DISTRIBUTION_REVIEW.md)."""
    if args.with_blender_mesh_tools:
        return
    if debugpy_dir and (Path(debugpy_dir) / "debugpy/__init__.py").is_file():
        print("  debugpy", flush=True)
        copy_tree(debugpy_dir, deploy_dir / "tools/debugpy", exclude_dirs=["__pycache__"])
    else:
        warn(f"No debugpy in the build ('{debugpy_dir}'; reconfigure with USDCC_BUNDLE_DEBUGPY=ON while online): "
             "the deploy works, but debugging Python in it needs a debugpy among the Python packages.")


def _deploy_peers_package(deploy_dir):
    """tools/usdcc_peers: the Python package other programs join rooms with, and the hub
    (docs/PEERS.md section 9). The app doesn't use it; it is there to pip install on a
    studio's server or a render farm. Its source only: it needs websockets and PyNaCl where
    it runs, not here."""
    source = REPO / "src/usdcc_peers"
    if (source / "usdcc_peers/__init__.py").is_file():
        print("  usdcc_peers", flush=True)
        copy_tree(source, deploy_dir / "tools/usdcc_peers", exclude_dirs=["__pycache__", "build", "*.egg-info"])


def _deploy_stubs(deploy_dir, args):
    """python/stubs, the types editors get (docs/VSCODE.md section 4). Not with Blender mesh tools, as debugpy."""
    if args.with_blender_mesh_tools:
        return
    stubs = cache_value(BUILD_DIR, "USDCC_STUBS_PATH")
    if stubs and (Path(stubs) / "_usdcc_app.pyi").is_file():
        print("  Types for editors", flush=True)
        copy_tree(stubs, deploy_dir / "python/stubs", exclude_files=["*-METADATA.txt"])
        metadata = Path(stubs) / "pxr/types-usd-METADATA.txt"
        if metadata.is_file():
            folder = deploy_dir / "licenses/types-usd"
            folder.mkdir(parents=True, exist_ok=True)
            copy_file(metadata, folder)
    else:
        warn(f"No types for editors in the build ('{stubs}'): the deploy works, but Set Up VS Code in it doesn't.")


def _deploy_vsix(deploy_dir, args):
    """tools/vscode/usdcc.vsix, the VS Code extension Set Up VS Code offers. Not with Blender mesh tools, as debugpy."""
    if args.with_blender_mesh_tools:
        return
    try:
        vsix = build_vsix.build()
    except build_vsix.VsixError as error:
        if args.require_vsix:
            fail(f"No VS Code extension: {error}")
        warn(f"No VS Code extension ({error}): the deploy works, but Set Up VS Code in it can't offer it.")
        return
    print("  VS Code extension", flush=True)
    (deploy_dir / "tools/vscode").mkdir(parents=True, exist_ok=True)
    copy_file(vsix, deploy_dir / "tools/vscode")


def _pyside_licences(pyside_dir, python_exe, licenses_dir, sources):
    """PySide6's licence texts, and its lines in SOURCES.txt."""
    # tools/build_pyside.py copies them in; an older PySide build has them in
    # pyside-setup's checkout.
    pyside_licenses = [folder for folder in (Path(pyside_dir) / "LICENSES", REPO / "external/pyside-setup/LICENSES")
                       if folder.is_dir()]
    if pyside_licenses:
        copy_tree(pyside_licenses[0], licenses_dir / "PySide6")
    else:
        warn(f"No PySide6 licences in '{Path(pyside_dir) / 'LICENSES'}': run tools/build_pyside.py again.")
    pyside_version = subprocess.run(
        [python_exe, "-I", "-c",
         "import sys; sys.path.insert(0, sys.argv[1]); import PySide6; print(PySide6.__version__)", pyside_dir],
        capture_output=True, text=True).stdout.strip()
    if pyside_version:
        sources.append(f"PySide6 and shiboken6 {pyside_version}, built by tools/build_pyside.py "
                       "(https://github.com/olihey/usdcc)")
        sources.append(f"  source:     https://code.qt.io/cgit/pyside/pyside-setup.git/tag/?h=v{pyside_version}")


def _velopack_licence(licenses_dir, sources):
    """Velopack's, built into usdcc on macOS and Linux (docs/UPDATES.md section 7)."""
    licence = cache_value(BUILD_DIR, "USDCC_VELOPACK_LICENSE_PATH")
    if not licence or not os.path.isfile(licence) or not _has_feature("UPDATES"):
        return
    (licenses_dir / "Velopack").mkdir(parents=True, exist_ok=True)
    copy_file(licence, licenses_dir / "Velopack")
    version = (Path(licence).parent / "VERSION").read_text(encoding="utf-8").strip()
    sources += ["", f"Velopack {version}", f"  source:     https://github.com/velopack/velopack/tree/{version}"]


# --- Linux --------------------------------------------------------------------

# Libraries a Linux deploy may take from the system: what every desktop has (glibc,
# libstdc++, X11, xcb, GL, fontconfig, ...). Anything else comes from the deploy.
SYSTEM_LIBRARY_FOLDERS = ("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/")

# What only a build needs, among Linux libraries.
LINUX_BUILD_ONLY_FILES = ["*.a", "*.la", "*.prl", "*.pc", "*.cmake"]

# The standard library's parts a deploy doesn't need (its tests, IDLE, Tk, pip's
# bootstrap, the static libpython in config-3.XY-*), and the packages folders.
PYTHON_LEFT_OUT = ["site-packages", "dist-packages", "__pycache__", "test", "idlelib", "tkinter", "turtledemo",
                   "ensurepip", "config-3*"]


def _excluded(name, patterns):
    return any(fnmatch.fnmatch(name, pattern) for pattern in patterns)


def _copy_linux_tree(source, destination, exclude_files=(), exclude_dirs=(), top_level_only=False):
    """Copies a folder, merging with what is there. A link to a file beside it
    (libfoo.so.1 -> libfoo.so.1.2) stays a link; any other link is copied as what
    it points at, or left out when that doesn't exist (Debian's stdlib links to
    /etc). Returns the names it replaced."""
    source = Path(source)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    replaced = []
    for entry in sorted(source.iterdir(), key=lambda entry: entry.name):
        target = destination / entry.name
        if entry.is_dir() and not entry.is_symlink():
            if not top_level_only and not _excluded(entry.name, exclude_dirs):
                replaced += _copy_linux_tree(entry, target, exclude_files, exclude_dirs)
            continue
        if _excluded(entry.name, exclude_files):
            continue
        if entry.is_symlink() and entry.is_dir():
            continue
        if target.is_symlink() or target.exists():
            replaced.append(entry.name)
            target.unlink()
        link = os.readlink(entry) if entry.is_symlink() else None
        if link is not None and "/" not in link:
            os.symlink(link, target)
        elif entry.is_file():  # follows a link
            shutil.copy2(entry, target)
    return replaced


def _ldd(path, environment=None):
    """What `path` loads, as {soname: resolved path or None}, from ldd."""
    result = subprocess.run(["ldd", str(path)], capture_output=True, text=True, env=environment)
    found = {}
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(\S+) => (not found|(\S+))", line)
        if match:
            found[match.group(1)] = match.group(3)
    return found


def _python_layout(python_exe):
    """The Python OpenUSD was built with, on Linux: its home and the files a deploy needs."""
    script = ("import os, sys, sysconfig; print(sys.base_prefix); print(sysconfig.get_config_var('py_version_short')); "
              "print(sysconfig.get_path('stdlib')); print(os.path.realpath(sys.executable))")
    result = subprocess.run([python_exe, "-I", "-c", script], capture_output=True, text=True)
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 4:
        fail(f"Couldn't ask {python_exe} where its files are: {result.stderr.strip()}")
    home, version, stdlib, executable = lines
    if not Path(stdlib, "encodings/__init__.py").is_file():
        fail(f"No Python standard library in {stdlib}.")
    return Path(home), version, Path(stdlib), Path(executable)


def _set_deploy_runpaths(deploy_dir, folders, own_folder_first=False, also=()):
    """Points the RUNPATH of every ELF file under `folders` at the deploy's lib/
    (and, for plugins, at their own folder first, as Windows finds a DLL's DLLs),
    then at the `also` folders."""
    from _elf import elf_files, origin_relative, set_runpath
    lib = deploy_dir / "lib"
    changed = []
    for folder in folders:
        if not folder.is_dir():
            continue
        for file in elf_files(folder):
            entries = [origin_relative(file, lib)]
            if own_folder_first and entries[0] != "$ORIGIN":
                entries.insert(0, "$ORIGIN")
            entries += [origin_relative(file, other) for other in also]
            set_runpath(file, entries)
            changed.append(file)
    return changed


def deploy_linux(args):
    """tools/deploy.py on Linux: the layout in the docstring at the top."""
    if not shutil.which("patchelf"):
        fail("patchelf not found; install it (apt install patchelf).")
    configuration = args.configuration
    build_type = cache_value(BUILD_DIR, "CMAKE_BUILD_TYPE")
    app_dir = BUILD_DIR / "src/cpp/app"
    if (app_dir / configuration / "usdcc").is_file():  # a multi-config generator
        app_dir = app_dir / configuration
    elif build_type and build_type != configuration:
        fail(f"The build is {build_type}, not {configuration}; build it with tools/build.py --configuration "
             f"{configuration}, or pass --configuration {build_type}.")
    exe = app_dir / "usdcc"
    if not exe.is_file():
        fail(f"No {configuration} build at {app_dir}; run tools/build.py --configuration {configuration} first.")
    deploy_dir = _full(args.destination)

    built_usd_install = cache_value(BUILD_DIR, "USD_INSTALL")
    usd_install = args.usd_install or built_usd_install
    if not usd_install or not os.path.isfile(os.path.join(usd_install, "lib/libusd_plug.so")):
        fail(f"No OpenUSD in '{usd_install}' (no lib/libusd_plug.so); pass --usd-install.")
    if args.usd_install and built_usd_install and not _same(_full(args.usd_install), _full(built_usd_install)):
        warn(f"usdcc was built against the OpenUSD in '{built_usd_install}', not '{usd_install}'; the deploy "
             f"may not run with it.")
    usd_install = Path(_full(usd_install))
    usd_python = next((folder for folder in (usd_install / "lib/site-packages", *sorted(usd_install.glob("lib/python3*/site-packages")),
                                             usd_install / "lib/python") if (folder / "pxr/__init__.py").is_file()), None)
    if not usd_python:
        fail(f"No pxr package in {usd_install}/lib.")

    python_exe = cache_value(BUILD_DIR, "USDCC_PYTHON_EXECUTABLE_PATH") or cache_value(BUILD_DIR, "Python3_EXECUTABLE")
    if not python_exe or not os.path.isfile(python_exe):
        fail(f"OpenUSD's Python not found (USDCC_PYTHON_EXECUTABLE_PATH in build/CMakeCache.txt: '{python_exe}'); "
             "configure again.")
    python_home, python_version, python_stdlib, python_binary = _python_layout(python_exe)
    # The libpython the build itself loads, wherever the Python keeps it.
    libpython = next((Path(path) for name, path in _ldd(exe).items() if name.startswith("libpython") and path), None)
    if not libpython:
        fail(f"{exe} doesn't load a libpython that ldd can find.")

    vcpkg_dir = _vcpkg_installed() / vcpkg_triplet(BUILD_DIR)
    own_libraries = sorted(file for file in (BUILD_DIR / "src/cpp").rglob("libusdcc_*.so")
                           if "CMakeFiles" not in file.parts and "plugins" not in file.parts)
    package_dir = BUILD_DIR / "python/usdcc"
    if not (package_dir / "__init__.py").is_file():
        fail(f"The usdcc Python package is missing from {package_dir}; build first.")
    ruff_exe = cache_value(BUILD_DIR, "USDCC_RUFF_EXE_PATH")
    uv_exe = cache_value(BUILD_DIR, "USDCC_UV_EXE_PATH")
    pyside_dir = cache_value(BUILD_DIR, "USDCC_PYSIDE_DIR")
    has_pyside = bool(pyside_dir) and os.path.isfile(os.path.join(pyside_dir, "PySide6/__init__.py"))
    if not has_pyside:
        warn(f"No PySide6 in '{pyside_dir}' (tools/build_pyside.py): the deploy works, but scripts can't make panels.")
    elif args.with_blender_mesh_tools:
        has_pyside = False  # not in that configuration's Linux review (docs/BLENDER_LINUX_DISTRIBUTION_REVIEW.md)

    # --- the destination ------------------------------------------------------

    for protected in (REPO, BUILD_DIR, usd_install, python_home, Path.home()):
        absolute = _full(protected)
        if absolute == deploy_dir or absolute.startswith(deploy_dir + os.sep):
            fail(f"Unsafe deploy destination '{deploy_dir}' contains '{absolute}'.")
    optional_dir = Path(_full(args.optional_packs)) if args.optional_packs else None
    if optional_dir and (str(optional_dir) == deploy_dir or str(optional_dir).startswith(deploy_dir + os.sep)
                         or deploy_dir.startswith(str(optional_dir) + os.sep)):
        fail(f"--optional-packs '{optional_dir}' can't be in the deploy, nor the deploy in it.")
    deploy_dir = Path(deploy_dir)
    if deploy_dir.exists():
        if not deploy_dir.is_dir():
            fail(f"{deploy_dir} is a file, not a folder.")
        if args.clean:
            print(f"Deleting {deploy_dir}")
            remove_tree(deploy_dir)
        elif (deploy_dir / "bin/usdcc").is_file():
            print(f"Replacing the earlier deploy in {deploy_dir}")
            remove_tree(deploy_dir)
        elif any(deploy_dir.iterdir()):
            fail(f"{deploy_dir} exists and isn't an earlier deploy (no bin/usdcc); pick an empty or new folder, "
                 "or pass --clean to delete it first.")
    bin_dir = deploy_dir / "bin"
    lib_dir = deploy_dir / "lib"
    python_lib = lib_dir / f"python{python_version}"
    site_packages = python_lib / "site-packages"
    print(f"Deploying usdcc ({configuration}) to {deploy_dir}", flush=True)

    # --- lib: OpenUSD first, then vcpkg's and the app's -----------------------
    # As on Windows, the app's libraries win where both have one (libpng16, ...).

    # The USD plugin packs in the install, as on Windows: the standard set is
    # shipped, file by file from each pack's list, and the others' files stay behind.
    # The Blender configuration ships only the packs its review covers (Gatling).
    usd_packs = _usd_packs(usd_install)
    shipped_packs = _shipped_packs(usd_packs, args.with_blender_mesh_tools)

    print(f"  OpenUSD libraries and plugins ({usd_install})", flush=True)
    mesh_exclusions = ["*_debug.so*", "libtbbbind*.so*", "libtbb_preview*.so*"] if args.with_blender_mesh_tools else []
    _copy_linux_tree(usd_install / "lib", lib_dir, top_level_only=True,
                     exclude_files=LINUX_BUILD_ONLY_FILES + mesh_exclusions + _pack_dlls(usd_packs))
    _copy_linux_tree(usd_install / "lib/usd", lib_dir / "usd", exclude_files=LINUX_BUILD_ONLY_FILES)
    _openusd_plugins(usd_install / "plugin/usd", deploy_dir / "plugin/usd", _copy_linux_tree)
    # Here, before the RUNPATHs are set: a pack's libraries get the deploy's too.
    pack_sources = []
    for pack, manifest in shipped_packs.items():
        print(f"  USD plugin pack: {pack} {manifest.get('version', '')}".rstrip(), flush=True)
        _deploy_usd_pack(pack, manifest, usd_install, deploy_dir, pack_sources, libraries="lib")
    optional_packs = {} if not optional_dir or args.with_blender_mesh_tools else _download_packs(
        usd_packs, app_only=bool(args.pack_folders))
    for pack in sorted(set(usd_packs) - set(shipped_packs) - set(optional_packs)):
        print(f"  USD plugin pack left out: {pack} (set: {usd_packs[pack].get('set')})", flush=True)

    print(f"  Qt and vcpkg's libraries ({vcpkg_dir})", flush=True)
    replaced = _copy_linux_tree(vcpkg_dir / "lib", lib_dir, exclude_files=LINUX_BUILD_ONLY_FILES, top_level_only=True)
    if replaced:
        print(f"    in place of OpenUSD's: {', '.join(sorted(set(replaced)))}")
    _copy_linux_tree(vcpkg_dir / "Qt6/plugins", deploy_dir / "Qt6/plugins", exclude_files=LINUX_BUILD_ONLY_FILES)

    print("  usdcc", flush=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, bin_dir / "usdcc")
    # The render node (docs/RENDER_NODE.md), and its queue's page, which it looks for beside it.
    node = app_dir / "usdcc_node"
    if not node.is_file():
        fail(f"No usdcc_node in {app_dir}; build it with tools/build.py.")
    shutil.copy2(node, bin_dir / "usdcc_node")
    copy_tree(app_dir / "node-web", bin_dir / "node-web")
    for library in own_libraries:
        shutil.copy2(library, lib_dir / library.name)
    shutil.copy2(libpython, lib_dir / libpython.name)

    # --- the Python: its program, standard library and packages ---------------

    print(f"  Python {python_version} ({python_home})", flush=True)
    _copy_linux_tree(python_stdlib, python_lib, exclude_dirs=PYTHON_LEFT_OUT)
    # uv can't work without an interpreter, not even to resolve.
    shutil.copy2(python_binary, bin_dir / f"python{python_version}")
    os.symlink(f"python{python_version}", bin_dir / "python3")

    print("  Python packages: usdcc, pxr", flush=True)
    # usdcc.ui's module loads PySide6's libraries.
    _copy_linux_tree(package_dir, site_packages / "usdcc", exclude_files=[] if has_pyside else ["_ui.so"],
                     exclude_dirs=["__pycache__"])
    _copy_linux_tree(usd_python, site_packages, exclude_dirs=["__pycache__"])
    if has_pyside:
        print("  Python packages: PySide6, shiboken6", flush=True)
        for package in ("PySide6", "shiboken6"):
            # Their libraries (libpyside6, libshiboken6) go with the others in lib, and
            # Qt is the one there: a second one in the process would be another Qt.
            folder = Path(pyside_dir) / package
            _copy_linux_tree(folder, site_packages / package, exclude_files=LINUX_BUILD_ONLY_FILES + ["lib*.so*"],
                             exclude_dirs=PYSIDE_BUILD_ONLY)
            for library in folder.glob("lib*.so*"):
                shutil.copy2(library, lib_dir / library.name)

    # --- tools ----------------------------------------------------------------

    for name, program in (("ruff", ruff_exe), ("uv", uv_exe)):
        if program and os.path.isfile(program):
            print(f"  {name}", flush=True)
            folder = deploy_dir / "tools" / name
            folder.mkdir(parents=True, exist_ok=True)
            shutil.copy2(program, folder)
            for licence in _names(Path(program).parent, "LICENSE*"):
                copy_file(Path(program).parent / licence, folder)
        else:
            warn(f"No {name} in the build ('{program}'; reconfigure while online): the deploy works, but it "
                 f"needs a {name} on PATH.")
    _deploy_debugpy(cache_value(BUILD_DIR, "USDCC_DEBUGPY_PATH"), deploy_dir, args)
    _deploy_peers_package(deploy_dir)
    _deploy_stubs(deploy_dir, args)
    _deploy_vsix(deploy_dir, args)

    # --- plugins, preview, SDK ------------------------------------------------

    print("  Shipped and example plugins", flush=True)
    # V-Ray Render's panel is PySide6: shipped without it, it would fail at every start.
    shipped = _shipped(SHIPPED_PLUGINS) if has_pyside and not args.with_blender_mesh_tools else []
    if not has_pyside and _shipped(SHIPPED_PLUGINS):
        warn(f"Without PySide6, {', '.join(_shipped(SHIPPED_PLUGINS))} isn't shipped: it needs it.")
    for plugin in shipped:
        copy_tree(REPO / "src/plugins" / plugin, deploy_dir / "plugins" / plugin, exclude_dirs=["__pycache__"])
    for plugin in _shipped(SHIPPED_NATIVE_PLUGINS):
        if not (BUILD_DIR / "plugins" / plugin / "usdcc-plugin.json").is_file():
            fail(f"The {plugin} plugin isn't built; build the usdcc target first.")
        _copy_linux_tree(BUILD_DIR / "plugins" / plugin, deploy_dir / "plugins" / plugin,
                         exclude_files=LINUX_BUILD_ONLY_FILES, exclude_dirs=["CMakeFiles"])
    copy_tree(REPO / "src/plugins", deploy_dir / "examples/plugins",
              exclude_dirs=["__pycache__", "cpp_example", "rtx_render", "rtx_render_native", "blender_mesh_tools"]
              + SHIPPED_PLUGINS + SHIPPED_NATIVE_PLUGINS)
    _copy_linux_tree(BUILD_DIR / "plugins", deploy_dir / "examples/plugins", exclude_files=LINUX_BUILD_ONLY_FILES,
                     exclude_dirs=["rtx_render", "rtx_render_native", "blender_mesh_tools", "CMakeFiles"]
                     + SHIPPED_PLUGINS + SHIPPED_NATIVE_PLUGINS)
    if args.with_blender_mesh_tools:
        from blender_mesh_package import include_candidate
        include_candidate(deploy_dir, BUILD_DIR, args.blender_source_bundle)
    print("  Material preview shapes and HDRIs", flush=True)
    copy_tree(REPO / "resources/preview", deploy_dir / "resources/preview")
    # Next to bin\, where WorkspaceLibrary::shippedFolder() looks (docs/WORKSPACES.md section 5).
    print("  Workspaces", flush=True)
    copy_tree(REPO / "resources/workspaces", deploy_dir / "resources/workspaces")

    print("  Plugin SDK", flush=True)
    sdk_dir = deploy_dir / "sdk"
    for library in ("core", "tools", "ui", "usd", "plugin_sdk"):
        copy_tree(REPO / "src/cpp" / library / "include", sdk_dir / "include")
    (sdk_dir / "lib").mkdir(parents=True, exist_ok=True)
    for library in own_libraries:
        os.symlink(f"../../lib/{library.name}", sdk_dir / "lib" / library.name)
    (sdk_dir / "cmake").mkdir(parents=True, exist_ok=True)
    copy_file(BUILD_DIR / "sdk/UsdccPluginConfig.cmake", sdk_dir)
    for module in ("UsdccPlugin.cmake", "FindUSD.cmake"):
        copy_file(REPO / "cmake/modules" / module, sdk_dir / "cmake")
    copy_tree(REPO / "src/plugins/cpp_example", sdk_dir / "example")

    # --- RUNPATHs -------------------------------------------------------------

    print("  RUNPATHs relative to $ORIGIN", flush=True)
    elves = _set_deploy_runpaths(deploy_dir, [bin_dir, deploy_dir / "plugin/usd", deploy_dir / "Qt6/plugins"])
    # pxr.UsdAbc and pxr.UsdDraco link their plugins, usdAbc.so and usdDraco.so, which aren't in lib/.
    elves += _set_deploy_runpaths(deploy_dir, [site_packages], also=[deploy_dir / "plugin/usd"])
    elves += _set_deploy_runpaths(deploy_dir, [deploy_dir / "plugins", deploy_dir / "examples/plugins"],
                                  own_folder_first=True)
    from _elf import is_elf, set_runpath
    for file in sorted(lib_dir.iterdir()):
        if is_elf(file):
            set_runpath(file, ["$ORIGIN"])
            elves.append(file)

    # The optional packs, after the RUNPATHs: their files under plugin/ keep their own,
    # relative to the install's lib/, which is the deploy's. They are checked below with
    # everything else, from inside the deploy, and then moved out of it.
    optional_files = {}
    for pack, manifest in optional_packs.items():
        print(f"  USD plugin pack, a download of its own: {pack} {manifest.get('version', '')}".rstrip(), flush=True)
        _deploy_usd_pack(pack, manifest, usd_install, deploy_dir, [], libraries="lib")
        record = json.loads((deploy_dir / USD_PACKS_DIR / f"{pack}.json").read_text(encoding="utf-8"))
        optional_files[pack] = [*record["files"], f"{USD_PACKS_DIR}/{pack}.json"]
        for name in record["files"]:
            file = deploy_dir / name
            if is_elf(file):
                if file.parent == lib_dir:
                    set_runpath(file, ["$ORIGIN"])
                elves.append(file)

    # --- licences (docs/RELEASE.md section 6) ---------------------------------

    print("  Licences", flush=True)
    licenses_dir = deploy_dir / "licenses"
    licenses_dir.mkdir(parents=True, exist_ok=True)
    copy_file(REPO / "LICENSE", deploy_dir / "LICENSE.txt")
    _usd_notices(usd_install, licenses_dir)
    if (usd_install / "licenses").is_dir():
        copy_tree(usd_install / "licenses", licenses_dir / "OpenUSD-dependencies", exclude_dirs=[USD_PACKS_DIR])
    else:
        warn(f"No licences of OpenUSD's dependencies in '{usd_install}/licenses' (tools/build_usd.py collects "
             "them): the deploy works, but can't be distributed as it is.")
    sources = [
        "Where the source of each library in this folder is. The LGPL ones (Qt, Qt Advanced Docking",
        "System, PySide6 and shiboken6) are separate shared libraries, which you may replace with your",
        "own builds of the same version.",
        "",
    ]
    _vcpkg_licences(licenses_dir, sources)
    sources += pack_sources
    if has_pyside:
        _pyside_licences(pyside_dir, python_exe, licenses_dir, sources)
    python_licence = next((file for file in (python_stdlib / "LICENSE.txt",
                                             Path(f"/usr/share/doc/python{python_version}/copyright"))
                           if file.is_file()), None)
    if python_licence:
        (licenses_dir / "Python").mkdir(parents=True, exist_ok=True)
        copy_file(python_licence, licenses_dir / "Python" / "LICENSE.txt")
    else:
        warn("No licence of the Python found.")
    if (REPO / "external/qwindowkit/LICENSE").is_file():
        (licenses_dir / "QWindowKit").mkdir(parents=True, exist_ok=True)
        copy_file(REPO / "external/qwindowkit/LICENSE", licenses_dir / "QWindowKit")
    for tool in ("ruff", "uv", "debugpy"):
        tool_dir = deploy_dir / "tools" / tool
        if tool_dir.is_dir():
            (licenses_dir / tool).mkdir(parents=True, exist_ok=True)
            for name in [*_names(tool_dir, "LICENSE*"), *_names(tool_dir, "ThirdPartyNotices*")]:
                copy_file(tool_dir / name, licenses_dir / tool)
    _velopack_licence(licenses_dir, sources)
    (licenses_dir / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")

    # --- launcher and checks --------------------------------------------------

    launcher = deploy_dir / "usdcc"
    launcher.write_text('#!/bin/sh\n# Starts usdcc from this folder, wherever it is.\n'
                        'exec "$(dirname "$(readlink -f "$0")")/bin/usdcc" "$@"\n', encoding="ascii")
    launcher.chmod(0o755)

    required = [
        "bin/usdcc", "bin/usdcc_node", "bin/node-web/index.html", "bin/node-web/usdcc.css",
        f"bin/python{python_version}", "lib/libusd_plug.so", "lib/libQt6Core.so.6", "lib/libusdcc_core.so",
        f"lib/{libpython.name}", "lib/usd/plugInfo.json", "plugin/usd/hdStorm.so",
        "Qt6/plugins/platforms/libqxcb.so", "Qt6/plugins/platforms/libqoffscreen.so",
        f"lib/python{python_version}/encodings/__init__.py", f"lib/python{python_version}/site-packages/usdcc/__init__.py",
        f"lib/python{python_version}/site-packages/pxr/__init__.py",
        f"lib/python{python_version}/site-packages/usdcc/_vendor/packaging/version.py",
        f"lib/python{python_version}/site-packages/usdcc/_vendor/jinja2/__init__.py",
        f"lib/python{python_version}/site-packages/usdcc/_vendor/markupsafe/__init__.py",
        f"lib/python{python_version}/site-packages/usdcc/_schema_check.py",
        *(f"plugins/{plugin}/usdcc-plugin.json" for plugin in shipped),
        "examples/plugins/measure_example/usdcc-plugin.json", "examples/plugins/cpp_example/usdcc-plugin.json",
        *((f"lib/python{python_version}/site-packages/{name}" for name in (
            "PySide6/__init__.py", "shiboken6/__init__.py", "usdcc/ui.py", "usdcc/_ui.so")) if has_pyside else ()),
        "sdk/UsdccPluginConfig.cmake", "sdk/lib/libusdcc_ui.so", "sdk/include/usdcc/plugin/plugin.h",
        "LICENSE.txt", "licenses/OpenUSD/LICENSE.txt", "licenses/OpenUSD/NOTICE.txt", "licenses/vcpkg/qtbase.txt", "licenses/SOURCES.txt",
    ]
    missing = [name for name in required if not (deploy_dir / name).exists()]
    if missing:
        fail(f"The deploy is incomplete; missing: {', '.join(missing)}")
    check_no_gpl_only_qt(deploy_dir, args)

    # Everything loads from the deploy or the system, in an environment without
    # LD_LIBRARY_PATH: nothing from the build, OpenUSD's install or the Python's.
    clean = {"PATH": "/usr/bin:/bin"}
    problems = []
    for file in elves:
        for name, path in _ldd(file, clean).items():
            if path is None:
                problems.append(f"{file.relative_to(deploy_dir)}: {name} not found")
            elif not path.startswith(str(deploy_dir) + "/") and not path.startswith(SYSTEM_LIBRARY_FOLDERS):
                problems.append(f"{file.relative_to(deploy_dir)}: {name} from {path}")
    if problems:
        fail("Libraries the deploy doesn't load from itself or the system:\n  " + "\n  ".join(sorted(set(problems))[:40]))

    # site-packages by hand, as the app adds it: Debian's own Python (the build's,
    # when OpenUSD was built with Ubuntu's) only ever puts dist-packages on sys.path.
    python = bin_dir / f"python{python_version}"
    pyside_import = "import usdcc.ui; " if has_pyside else ""
    check = subprocess.run(
        [str(python), "-c", f"import sys; sys.path.insert(0, {str(site_packages)!r}); {pyside_import}"
                            "import json, math, pxr, usdcc; from pxr import Usd, UsdImagingGL; "
                            "print(json.dumps([sys.prefix, pxr.__file__, usdcc.__file__]))"],
        env=clean, capture_output=True, text=True)
    if check.returncode != 0:
        fail(f"The deploy's Python can't import pxr and usdcc: {check.stderr.strip()}")
    for path in json.loads(check.stdout):
        if not os.path.realpath(path).startswith(str(deploy_dir.resolve())):
            fail(f"The deploy's Python took {path} from outside the deploy.")
    if uv_exe and (deploy_dir / "tools/uv/uv").is_file():
        found = subprocess.run([str(deploy_dir / "tools/uv/uv"), "python", "find", "--no-config",
                                "--no-python-downloads", str(python)], env=clean, capture_output=True, text=True)
        if found.returncode != 0:
            fail(f"uv can't use the deploy's Python: {(found.stdout + found.stderr).strip()}")

    # The optional packs out of the deploy, each into a folder of the deploy's name.
    # A published pack of the local set leaves nothing in --optional-packs' folder.
    staging = Path(_full(args.pack_folders)) / ".staging" if args.pack_folders else None
    for pack, names in optional_files.items():
        app_only = _app_only(pack, optional_packs[pack])
        target = (staging if app_only else optional_dir) / pack / deploy_dir.name
        if target.exists():
            remove_tree(target)
        for name in names:
            (target / name).parent.mkdir(parents=True, exist_ok=True)
            os.replace(deploy_dir / name, target / name)
            parent = (deploy_dir / name).parent
            while parent != deploy_dir and not any(parent.iterdir()):
                parent.rmdir()
                parent = parent.parent
        size = sum(file.stat().st_size for file in target.rglob("*") if file.is_file() and not file.is_symlink())
        if not app_only:
            print(f"  {pack}: {target} ({size / 2**20:,.0f} MB), checked in the deploy and moved out of it")
        # And as usdcc installs it when it downloads it itself (docs/USD_PLUGINS.md section 10).
        if args.pack_folders:
            _pack_folder(pack, target, names, Path(_full(args.pack_folders)) / pack, optional_packs[pack].get("version", ""),
                         windows=False)
    if staging and staging.exists():
        remove_tree(staging)

    size = sum(file.stat().st_size for file in deploy_dir.rglob("*") if file.is_file() and not file.is_symlink())
    print(f"Done: {deploy_dir} ({size / 2**20:,.0f} MB). Start it with ./usdcc or bin/usdcc.")


# --- macOS --------------------------------------------------------------------

def _macos_links(contents, lib_dir):
    """Every Mach-O file's links and rpaths, from what the build had to what the bundle
    has; the files. Signed afterwards, as a whole."""
    from _macho import edit, install_id, is_system, loader_relative, macho_files, needed
    plugin_folders = [contents / "Resources/plugins", contents / "Resources/examples/plugins"]
    files = macho_files(contents)
    for file in files:
        links = {name: "@rpath/" + Path(name).name for name in needed(file)
                 if not name.startswith("@") and not is_system(name)}
        entries = [loader_relative(file, lib_dir)]
        if any(name.startswith("@rpath/usd") for name in needed(file)):
            # pxr's modules of plugins (usdAbc, usdDraco), which link the plugin itself.
            entries.append(loader_relative(file, contents / "Resources/plugin/usd"))
        if any(folder in file.parents for folder in plugin_folders) and entries[0] != "@loader_path":
            entries.insert(0, "@loader_path")
        own = install_id(file)
        edit(file, new_rpaths=entries, new_id="@rpath/" + file.name if own and own.startswith("/") else None, changes=links)
    _macos_own_names(files, lib_dir)
    return files


def _macos_own_names(files, lib_dir):
    """Links by the library's own name, not a link's to it (libjpeg.62.dylib, whose name is
    libjpeg.62.4.0.dylib where vcpkg's is in place of OpenUSD's): the hardened runtime
    refuses a library whose name isn't the one asked for."""
    from _macho import edit, install_id, needed
    own_names = {library.name: Path(install_id(library.resolve()) or library.name).name
                 for library in lib_dir.iterdir() if library.name.endswith(".dylib") and library.is_file()}
    for file in files:
        edit(file, changes={name: "@rpath/" + own_names[Path(name).name] for name in needed(file)
                            if name.startswith("@rpath/") and own_names.get(Path(name).name, Path(name).name) != Path(name).name})


def _macos_problems(contents, files, provided=()):
    """What a bundle's Mach-O files link that the bundle (through their rpaths) and the
    system don't have, or have in an older version than they were linked with. `provided`
    names libraries that are loaded before these files are, from outside the bundle
    (_sdk_libraries())."""
    from _macho import install_id, is_system, library_versions, rpaths
    problems = []
    for file in files:
        own = install_id(file)
        folders = [Path(os.path.normpath(entry.replace("@loader_path", str(file.parent)))) for entry in rpaths(file)]
        for folder in folders:
            if contents not in folder.parents:
                problems.append(f"{file.relative_to(contents)}: rpath {folder}")
        for name, version in library_versions(file).items():
            if name == own or is_system(name) or Path(name).name in provided:
                continue
            if not name.startswith("@rpath/"):
                problems.append(f"{file.relative_to(contents)}: links {name}")
                continue
            found = next((folder / name[len("@rpath/"):] for folder in folders if (folder / name[len("@rpath/"):]).is_file()), None)
            if not found:
                problems.append(f"{file.relative_to(contents)}: {name} not found")
                continue
            if Path(install_id(found.resolve()) or name).name != Path(name).name:
                problems.append(f"{file.relative_to(contents)}: {name} is {install_id(found.resolve())}")
            has = library_versions(found.resolve()).get(install_id(found.resolve()), version)
            if has < version:
                problems.append(f"{file.relative_to(contents)}: {name} {'.'.join(map(str, has))} is older than "
                                f"{'.'.join(map(str, version))}, which it links")
    return problems


def _macos_optional_pack(pack, manifest, usd_install, app, target, identity):
    """A copy of the signed `app`, as `target`, with an optional pack in it: where the pack
    is checked and each of its binaries signed, for the folder usdcc installs
    (_pack_folder). The copy itself isn't shipped. A published pack of the local set is
    put into one too: its binaries are checked and signed there, as the others'."""
    from _macho import ENTITLEMENTS, is_macho, sign, sign_app
    if target.parent.exists():
        remove_tree(target.parent)
    target.parent.mkdir(parents=True)
    shutil.copytree(app, target, symlinks=True)
    contents = target / "Contents"
    root = contents / "Resources"
    sources = []
    _deploy_usd_pack(pack, manifest, usd_install, root, sources, libraries="lib")
    with (root / "licenses/SOURCES.txt").open("a", encoding="utf-8") as file:
        file.write("\n".join(sources) + "\n")
    record = json.loads((root / USD_PACKS_DIR / f"{pack}.json").read_text(encoding="utf-8"))
    # Their rpaths are relative to the install's lib/, which is the bundle's (as on Linux).
    files = [root / name for name in record["files"] if is_macho(root / name)]
    _macos_own_names(files, root / "lib")
    problems = _macos_problems(contents, files, provided=_sdk_libraries(pack, "macos-arm64"))
    if problems:
        fail(f"Libraries the {pack} pack doesn't load from the deploy or the system:\n  "
             + "\n  ".join(sorted(set(problems))[:40]))
    for file in files:
        # A program of the pack's loads libraries too, so it gets usdcc's entitlements.
        sign(file, identity, ENTITLEMENTS if os.access(file, os.X_OK) and file.parent.name == "bin" else None)
    sign_app(target, identity)
    size = sum(file.stat().st_size for file in target.rglob("*") if file.is_file() and not file.is_symlink())
    print(f"  {pack}: {target} ({size / 2**20:,.0f} MB), usdcc with the pack")


def deploy_macos(args):
    """tools/deploy.py on macOS: the layout in the docstring at the top."""
    from _macho import needed, sign, sign_app
    if args.with_blender_mesh_tools:
        fail("--with-blender-mesh-tools isn't set up for macOS: its distribution review is Windows' and Linux'.")
    configuration = args.configuration
    build_type = cache_value(BUILD_DIR, "CMAKE_BUILD_TYPE")
    app_dir = BUILD_DIR / "src/cpp/app"
    if (app_dir / configuration / "usdcc.app").is_dir():  # a multi-config generator
        app_dir = app_dir / configuration
    elif build_type and build_type != configuration:
        fail(f"The build is {build_type}, not {configuration}; build it with tools/build.py --configuration "
             f"{configuration}, or pass --configuration {build_type}.")
    built = app_dir / "usdcc.app/Contents"
    exe = built / "MacOS/usdcc"
    if not exe.is_file():
        fail(f"No {configuration} build at {app_dir}; run tools/build.py --configuration {configuration} first.")
    deploy_dir = _full(args.destination)

    built_usd_install = cache_value(BUILD_DIR, "USD_INSTALL")
    usd_install = args.usd_install or built_usd_install
    if not usd_install or not os.path.isfile(os.path.join(usd_install, "lib/libusd_plug.dylib")):
        fail(f"No OpenUSD in '{usd_install}' (no lib/libusd_plug.dylib); pass --usd-install.")
    if args.usd_install and built_usd_install and not _same(_full(args.usd_install), _full(built_usd_install)):
        warn(f"usdcc was built against the OpenUSD in '{built_usd_install}', not '{usd_install}'; the deploy "
             f"may not run with it.")
    usd_install = Path(_full(usd_install))
    usd_python = next((folder for folder in (usd_install / "lib/site-packages", *sorted(usd_install.glob("lib/python3*/site-packages")),
                                             usd_install / "lib/python") if (folder / "pxr/__init__.py").is_file()), None)
    if not usd_python:
        fail(f"No pxr package in {usd_install}/lib.")

    python_exe = cache_value(BUILD_DIR, "USDCC_PYTHON_EXECUTABLE_PATH") or cache_value(BUILD_DIR, "Python3_EXECUTABLE")
    if not python_exe or not os.path.isfile(python_exe):
        fail(f"OpenUSD's Python not found (USDCC_PYTHON_EXECUTABLE_PATH in build/CMakeCache.txt: '{python_exe}'); "
             "configure again.")
    python_home, python_version, python_stdlib, python_binary = _python_layout(python_exe)
    libpython = python_home / "lib" / f"libpython{python_version}.dylib"
    if not libpython.is_file():
        fail(f"No {libpython}: usdcc needs a Python with a shared library, as uv's (docs/MACOS.md).")
    if not any(Path(name).name == libpython.name for name in needed(exe)):
        fail(f"{exe} doesn't link {libpython.name}.")

    vcpkg_dir = _vcpkg_installed() / vcpkg_triplet(BUILD_DIR)
    own_libraries = sorted(file for file in (BUILD_DIR / "src/cpp").rglob("libusdcc_*.dylib")
                           if "CMakeFiles" not in file.parts and "plugins" not in file.parts)
    package_dir = BUILD_DIR / "python/usdcc"
    if not (package_dir / "__init__.py").is_file():
        fail(f"The usdcc Python package is missing from {package_dir}; build first.")
    ruff_exe = cache_value(BUILD_DIR, "USDCC_RUFF_EXE_PATH")
    uv_exe = cache_value(BUILD_DIR, "USDCC_UV_EXE_PATH")
    pyside_dir = cache_value(BUILD_DIR, "USDCC_PYSIDE_DIR")
    has_pyside = bool(pyside_dir) and os.path.isfile(os.path.join(pyside_dir, "PySide6/__init__.py"))
    if not has_pyside:
        warn(f"No PySide6 in '{pyside_dir}' (tools/build_pyside.py): the deploy works, but scripts can't make panels.")

    # --- the destination ------------------------------------------------------

    for protected in (REPO, BUILD_DIR, usd_install, python_home, Path.home()):
        absolute = _full(protected)
        if absolute == deploy_dir or absolute.startswith(deploy_dir + os.sep):
            fail(f"Unsafe deploy destination '{deploy_dir}' contains '{absolute}'.")
    optional_dir = Path(_full(args.optional_packs)) if args.optional_packs else None
    if optional_dir and (str(optional_dir) == deploy_dir or str(optional_dir).startswith(deploy_dir + os.sep)
                         or deploy_dir.startswith(str(optional_dir) + os.sep)):
        fail(f"--optional-packs '{optional_dir}' can't be in the deploy, nor the deploy in it.")
    deploy_dir = Path(deploy_dir)
    app = deploy_dir / "usdcc.app"
    if deploy_dir.exists():
        if not deploy_dir.is_dir():
            fail(f"{deploy_dir} is a file, not a folder.")
        # Finder leaves a .DS_Store in any folder it has shown.
        others = [entry.name for entry in deploy_dir.iterdir() if entry.name not in ("usdcc.app", ".DS_Store")]
        if args.clean:
            print(f"Deleting {deploy_dir}")
            remove_tree(deploy_dir)
        elif (app / "Contents/MacOS/usdcc").is_file() and not others:
            print(f"Replacing the earlier deploy in {deploy_dir}")
            remove_tree(deploy_dir)
        elif others or app.exists():
            fail(f"{deploy_dir} exists and isn't an earlier deploy (usdcc.app alone); pick an empty or new folder, "
                 "or pass --clean to delete it first.")
    contents = app / "Contents"
    root = contents / "Resources"  # installRoot() in usdcc/core/install_paths.h
    programs = contents / "MacOS"
    lib_dir = root / "lib"
    python_lib = lib_dir / f"python{python_version}"
    site_packages = python_lib / "site-packages"
    print(f"Deploying usdcc ({configuration}) to {app}", flush=True)

    # --- lib: OpenUSD first, then vcpkg's and the app's, as on Linux -----------

    usd_packs = _usd_packs(usd_install)
    shipped_packs = {pack: manifest for pack, manifest in usd_packs.items() if manifest.get("set") == "standard"}
    print(f"  OpenUSD libraries and plugins ({usd_install})", flush=True)
    _copy_linux_tree(usd_install / "lib", lib_dir, top_level_only=True,
                     exclude_files=LINUX_BUILD_ONLY_FILES + ["*_debug.dylib"] + _pack_dlls(usd_packs))
    _copy_linux_tree(usd_install / "lib/usd", lib_dir / "usd", exclude_files=LINUX_BUILD_ONLY_FILES)
    _openusd_plugins(usd_install / "plugin/usd", root / "plugin/usd", _copy_linux_tree)
    pack_sources = []
    for pack, manifest in shipped_packs.items():
        print(f"  USD plugin pack: {pack} {manifest.get('version', '')}".rstrip(), flush=True)
        _deploy_usd_pack(pack, manifest, usd_install, root, pack_sources, libraries="lib")
    optional_packs = _download_packs(usd_packs, app_only=bool(args.pack_folders)) if optional_dir else {}
    for pack in sorted(set(usd_packs) - set(shipped_packs) - set(optional_packs)):
        print(f"  USD plugin pack left out: {pack} (set: {usd_packs[pack].get('set')})", flush=True)

    print(f"  Qt and vcpkg's libraries ({vcpkg_dir})", flush=True)
    replaced = _copy_linux_tree(vcpkg_dir / "lib", lib_dir, exclude_files=LINUX_BUILD_ONLY_FILES, top_level_only=True)
    if replaced:
        print(f"    in place of OpenUSD's: {', '.join(sorted(set(replaced)))}")
    _copy_linux_tree(vcpkg_dir / "Qt6/plugins", root / "Qt6/plugins", exclude_files=LINUX_BUILD_ONLY_FILES)
    (root / "qt.conf").write_text("[Paths]\nPlugins = Resources/Qt6/plugins\n", encoding="ascii")

    print("  usdcc", flush=True)
    programs.mkdir(parents=True, exist_ok=True)
    for name in ("usdcc", "usdcc_node"):
        if not (built / "MacOS" / name).is_file():
            fail(f"No {name} in {built / 'MacOS'}; build it with tools/build.py.")
        shutil.copy2(built / "MacOS" / name, programs / name)
    copy_file(built / "Info.plist", contents)
    copy_file(built / "Resources/usdcc.icns", root)
    copy_tree(built / "Resources/node-web", root / "node-web")
    for library in own_libraries:
        shutil.copy2(library, lib_dir / library.name)
    shutil.copy2(libpython, lib_dir / libpython.name)

    # --- the Python: its program, standard library and packages ---------------

    print(f"  Python {python_version} ({python_home})", flush=True)
    # Tk isn't shipped, nor what links it.
    _copy_linux_tree(python_stdlib, python_lib, exclude_dirs=PYTHON_LEFT_OUT, exclude_files=["_tkinter*"])
    (root / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(python_binary, root / "bin" / f"python{python_version}")
    os.symlink(f"python{python_version}", root / "bin/python3")

    print("  Python packages: usdcc, pxr", flush=True)
    _copy_linux_tree(package_dir, site_packages / "usdcc", exclude_files=[] if has_pyside else ["_ui.so"],
                     exclude_dirs=["__pycache__"])
    _copy_linux_tree(usd_python, site_packages, exclude_dirs=["__pycache__"])
    if has_pyside:
        print("  Python packages: PySide6, shiboken6", flush=True)
        for package in ("PySide6", "shiboken6"):
            # As on Linux: their libraries with the others, and no Qt of their own.
            folder = Path(pyside_dir) / package
            _copy_linux_tree(folder, site_packages / package, exclude_files=LINUX_BUILD_ONLY_FILES + ["lib*.dylib"],
                             exclude_dirs=PYSIDE_BUILD_ONLY)
            for library in folder.glob("lib*.dylib"):
                shutil.copy2(library, lib_dir / library.name)

    # --- tools ----------------------------------------------------------------

    for name, program in (("ruff", ruff_exe), ("uv", uv_exe)):
        if program and os.path.isfile(program):
            print(f"  {name}", flush=True)
            folder = root / "tools" / name
            folder.mkdir(parents=True, exist_ok=True)
            shutil.copy2(program, folder)
            for licence in _names(Path(program).parent, "LICENSE*"):
                copy_file(Path(program).parent / licence, folder)
        else:
            warn(f"No {name} in the build ('{program}'; reconfigure while online): the deploy works, but it "
                 f"needs a {name} on PATH.")
    _deploy_debugpy(cache_value(BUILD_DIR, "USDCC_DEBUGPY_PATH"), root, args)
    _deploy_peers_package(root)
    _deploy_stubs(root, args)
    _deploy_vsix(root, args)

    # --- plugins, preview, SDK ------------------------------------------------

    print("  Shipped and example plugins", flush=True)
    shipped = _shipped(SHIPPED_PLUGINS) if has_pyside else []
    if not has_pyside and _shipped(SHIPPED_PLUGINS):
        warn(f"Without PySide6, {', '.join(_shipped(SHIPPED_PLUGINS))} isn't shipped: it needs it.")
    for plugin in shipped:
        copy_tree(REPO / "src/plugins" / plugin, root / "plugins" / plugin, exclude_dirs=["__pycache__"])
    for plugin in _shipped(SHIPPED_NATIVE_PLUGINS):
        if not (BUILD_DIR / "plugins" / plugin / "usdcc-plugin.json").is_file():
            fail(f"The {plugin} plugin isn't built; build the usdcc target first.")
        _copy_linux_tree(BUILD_DIR / "plugins" / plugin, root / "plugins" / plugin,
                         exclude_files=LINUX_BUILD_ONLY_FILES, exclude_dirs=["CMakeFiles"])
    copy_tree(REPO / "src/plugins", root / "examples/plugins",
              exclude_dirs=["__pycache__", "cpp_example", "rtx_render", "rtx_render_native", "blender_mesh_tools"]
              + SHIPPED_PLUGINS + SHIPPED_NATIVE_PLUGINS, exclude_files=[".DS_Store"])
    _copy_linux_tree(BUILD_DIR / "plugins", root / "examples/plugins", exclude_files=LINUX_BUILD_ONLY_FILES,
                     exclude_dirs=["rtx_render", "rtx_render_native", "blender_mesh_tools", "CMakeFiles"]
                     + SHIPPED_PLUGINS + SHIPPED_NATIVE_PLUGINS)
    print("  Material preview shapes and HDRIs", flush=True)
    copy_tree(REPO / "resources/preview", root / "resources/preview", exclude_files=[".DS_Store"])
    print("  Workspaces", flush=True)
    copy_tree(REPO / "resources/workspaces", root / "resources/workspaces", exclude_files=[".DS_Store"])

    print("  Plugin SDK", flush=True)
    sdk_dir = root / "sdk"
    for library in ("core", "tools", "ui", "usd", "plugin_sdk"):
        copy_tree(REPO / "src/cpp" / library / "include", sdk_dir / "include")
    (sdk_dir / "lib").mkdir(parents=True, exist_ok=True)
    for library in own_libraries:
        os.symlink(f"../../lib/{library.name}", sdk_dir / "lib" / library.name)
    (sdk_dir / "cmake").mkdir(parents=True, exist_ok=True)
    copy_file(BUILD_DIR / "sdk/UsdccPluginConfig.cmake", sdk_dir)
    for module in ("UsdccPlugin.cmake", "FindUSD.cmake"):
        copy_file(REPO / "cmake/modules" / module, sdk_dir / "cmake")
    copy_tree(REPO / "src/plugins/cpp_example", sdk_dir / "example", exclude_files=[".DS_Store"])

    # --- licences (docs/RELEASE.md section 6) ---------------------------------

    print("  Licences", flush=True)
    licenses_dir = root / "licenses"
    licenses_dir.mkdir(parents=True, exist_ok=True)
    copy_file(REPO / "LICENSE", root / "LICENSE.txt")
    _usd_notices(usd_install, licenses_dir)
    if (usd_install / "licenses").is_dir():
        copy_tree(usd_install / "licenses", licenses_dir / "OpenUSD-dependencies", exclude_dirs=[USD_PACKS_DIR])
    else:
        warn(f"No licences of OpenUSD's dependencies in '{usd_install}/licenses' (tools/build_usd.py collects "
             "them): the deploy works, but can't be distributed as it is.")
    sources = [
        "Where the source of each library in this bundle is. The LGPL ones (Qt, Qt Advanced Docking",
        "System, PySide6 and shiboken6) are separate shared libraries in Contents/Resources/lib, which you",
        "may replace with your own builds of the same version; sign the bundle again afterwards",
        "(codesign --force --deep --sign - usdcc.app).",
        "",
    ]
    _vcpkg_licences(licenses_dir, sources)
    sources += pack_sources
    if has_pyside:
        _pyside_licences(pyside_dir, python_exe, licenses_dir, sources)
    python_licence = next((file for file in (python_stdlib / "LICENSE.txt", python_home / "LICENSE.txt",
                                             python_home / "LICENSE") if file.is_file()), None)
    if python_licence:
        (licenses_dir / "Python").mkdir(parents=True, exist_ok=True)
        copy_file(python_licence, licenses_dir / "Python" / "LICENSE.txt")
    else:
        warn("No licence of the Python found.")
    if (REPO / "external/qwindowkit/LICENSE").is_file():
        (licenses_dir / "QWindowKit").mkdir(parents=True, exist_ok=True)
        copy_file(REPO / "external/qwindowkit/LICENSE", licenses_dir / "QWindowKit")
    for tool in ("ruff", "uv", "debugpy"):
        tool_dir = root / "tools" / tool
        if tool_dir.is_dir():
            (licenses_dir / tool).mkdir(parents=True, exist_ok=True)
            for name in [*_names(tool_dir, "LICENSE*"), *_names(tool_dir, "ThirdPartyNotices*")]:
                copy_file(tool_dir / name, licenses_dir / tool)
    _velopack_licence(licenses_dir, sources)
    (licenses_dir / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")

    # --- links, compiled Python, signatures -----------------------------------

    print("  Links and rpaths relative to @loader_path", flush=True)
    files = _macos_links(contents, lib_dir)

    required = [
        "Info.plist", "MacOS/usdcc", "MacOS/usdcc_node",
        *(f"Resources/{name}" for name in (
            "usdcc.icns", "qt.conf", "node-web/index.html", "node-web/usdcc.css", f"bin/python{python_version}",
            "lib/libusd_plug.dylib", "lib/libusd_hgiMetal.dylib", "lib/libQt6Core.6.dylib", "lib/libusdcc_core.dylib",
            f"lib/{libpython.name}", "lib/usd/plugInfo.json", "plugin/usd/hdStorm.dylib",
            "Qt6/plugins/platforms/libqcocoa.dylib", "Qt6/plugins/platforms/libqoffscreen.dylib",
            f"lib/python{python_version}/encodings/__init__.py",
            *(f"lib/python{python_version}/site-packages/{name}" for name in (
                "usdcc/__init__.py", "pxr/__init__.py", "usdcc/_vendor/packaging/version.py",
                "usdcc/_vendor/jinja2/__init__.py", "usdcc/_vendor/markupsafe/__init__.py", "usdcc/_schema_check.py",
                *(("PySide6/__init__.py", "shiboken6/__init__.py", "usdcc/ui.py", "usdcc/_ui.so") if has_pyside else ()))),
            *(f"plugins/{plugin}/usdcc-plugin.json" for plugin in shipped),
            "examples/plugins/measure_example/usdcc-plugin.json", "examples/plugins/cpp_example/usdcc-plugin.json",
            "sdk/UsdccPluginConfig.cmake", "sdk/lib/libusdcc_ui.dylib", "sdk/include/usdcc/plugin/plugin.h",
            "LICENSE.txt", "licenses/OpenUSD/LICENSE.txt", "licenses/OpenUSD/NOTICE.txt", "licenses/vcpkg/qtbase.txt",
            "licenses/SOURCES.txt")),
    ]
    missing = [name for name in required if not (contents / name).exists()]
    if missing:
        fail(f"The deploy is incomplete; missing: {', '.join(missing)}")
    check_no_gpl_only_qt(contents, args)
    # Every link from the bundle or the system: nothing from the build, OpenUSD's install or the Python's.
    problems = _macos_problems(contents, files)
    if problems:
        fail("Libraries the deploy doesn't load from itself or the system:\n  " + "\n  ".join(sorted(set(problems))[:40]))

    # Compiled now: written later, next to the signed files, the __pycache__ would break the
    # bundle's signature. Without timestamps, which a copy of the bundle may change. Templates
    # and vendored Python 2 code don't compile, and don't matter.
    print("  Compiling the Python", flush=True)
    python = root / "bin" / f"python{python_version}"
    sign(python, args.sign)
    clean = {"PATH": "/usr/bin:/bin"}
    subprocess.run([str(python), "-I", "-m", "compileall", "-q", "-j", "0", "--invalidation-mode", "unchecked-hash",
                    "-s", str(deploy_dir) + "/", str(python_lib), str(root / "plugins"), str(root / "examples"), str(root / "tools")],
                   env=clean, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not any((python_lib / "encodings/__pycache__").glob("*.pyc")):
        fail("The deploy's Python compiled nothing.")

    print(f"  Signing {'ad hoc' if args.sign == '-' else 'as ' + args.sign}", flush=True)
    for file in files:
        sign(file, args.sign)
    sign_app(app, args.sign)

    # --- checks, on the signed bundle -----------------------------------------

    pyside_import = "import usdcc.ui; " if has_pyside else ""
    check = subprocess.run(
        [str(python), "-B", "-c", f"import sys; {pyside_import}"
                                  "import json, math, ssl, sqlite3, pxr, usdcc; from pxr import Usd, UsdImagingGL; "
                                  "print(json.dumps([sys.prefix, pxr.__file__, usdcc.__file__]))"],
        env=clean, capture_output=True, text=True)
    if check.returncode != 0:
        fail(f"The deploy's Python can't import pxr and usdcc: {check.stderr.strip()}")
    for path in json.loads(check.stdout.splitlines()[-1]):
        if not os.path.realpath(path).startswith(str(deploy_dir.resolve())):
            fail(f"The deploy's Python took {path} from outside the deploy.")
    if uv_exe and (root / "tools/uv/uv").is_file():
        found = subprocess.run([str(root / "tools/uv/uv"), "python", "find", "--no-config",
                                "--no-python-downloads", str(python)], env={**clean, "HOME": str(Path.home())},
                               capture_output=True, text=True)
        if found.returncode != 0:
            fail(f"uv can't use the deploy's Python: {(found.stdout + found.stderr).strip()}")
    sign_check = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], capture_output=True, text=True)
    if sign_check.returncode != 0:
        fail(f"Checking the deploy broke its signature: {sign_check.stderr.strip()}")

    for pack, manifest in optional_packs.items():
        print(f"  USD plugin pack, a download of its own: {pack} {manifest.get('version', '')}".rstrip(), flush=True)
        _macos_optional_pack(pack, manifest, usd_install, app, optional_dir / pack / deploy_dir.name / "usdcc.app", args.sign)
        # And as usdcc installs it when it downloads it itself (docs/USD_PLUGINS.md section 10):
        # from that bundle, where each of its binaries got its own signature.
        if args.pack_folders:
            with_pack = optional_dir / pack / deploy_dir.name / "usdcc.app/Contents/Resources"
            record = json.loads((with_pack / USD_PACKS_DIR / f"{pack}.json").read_text(encoding="utf-8"))
            _pack_folder(pack, with_pack, [*record["files"], f"{USD_PACKS_DIR}/{pack}.json"], Path(_full(args.pack_folders)) / pack,
                         manifest.get("version", ""), windows=False)
        # A published pack of the local set leaves nothing in --optional-packs' folder.
        if _app_only(pack, manifest):
            remove_tree(optional_dir / pack)

    size = sum(file.stat().st_size for file in deploy_dir.rglob("*") if file.is_file() and not file.is_symlink())
    print(f"Done: {app} ({size / 2**20:,.0f} MB). Start it with: open {app}")


if __name__ == "__main__":
    main()
