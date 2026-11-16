"""Copies everything usdcc needs to run into one folder, to distribute it.

Collects a built usdcc (run tools/build.py first) with its runtime dependencies
into a self-contained folder that runs on a machine without the dev environment:

    <destination>\\
      usdcc.cmd     starts bin\\usdcc.exe
      LICENSE.txt   usdcc's licence (BSD-3-Clause)
      licenses\\     the licences of everything else in the folder, and in
                    SOURCES.txt where each library's source is
                    (docs/RELEASE.md section 6)
      bin\\          usdcc.exe, its DLLs (app, Qt, vcpkg, OpenUSD and the
                    libraries OpenUSD uses), Qt's platform plugin
      bin\\usd\\      OpenUSD's built-in plugins (schemas, Hydra, MaterialX
                    standard library, ...)
      plugin\\usd\\   OpenUSD's other plugins (Storm, Embree, image readers)
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
      bin/          usdcc, and python3.XY (with a python3 link), which uv needs
      lib/          usdcc's, Qt's, vcpkg's and OpenUSD's libraries, libpython
      lib/usd/      OpenUSD's built-in plugins
      lib/python3.XY/   the standard library, and in site-packages: usdcc, pxr
      Qt6/plugins/  Qt's plugins (platforms: xcb, wayland, offscreen, ...)
      plugin/usd/   OpenUSD's other plugins
      plugins/, examples/plugins/, resources/preview/, resources/workspaces/, tools/ruff/, tools/uv/,
      sdk/ (its lib/ links to the libraries in lib/), LICENSE.txt, licenses/

Every ELF file's RUNPATH is rewritten relative to $ORIGIN (with patchelf), so the
folder runs from wherever it is unpacked.

    python tools/deploy.py                       into .\\deploy
    python tools/deploy.py D:\\dist\\usdcc --configuration Release
    python tools/deploy.py D:\\dist\\usdcc --clean
    python tools/deploy.py --usd-install C:\\usd\\install

Windows and Linux. Standard library only, and patchelf on Linux.
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
from _common import (  # noqa: E402
    CONFIGURATIONS, MACOS, REPO, WINDOWS, cache_on, cache_value, copy_file, copy_tree, fail, remove_tree,
    vcpkg_triplet, warn)

BUILD_DIR = REPO / "build"

# What only a build needs, next to the files a deploy takes.
BUILD_ONLY_FILES = ["*.pdb", "*.lib", "*.exp", "*.ilk"]

# plugins\ is where the app always looks for the plugins it ships (docs/PLUGINS.md).
# The examples sit apart, so they don't load unless asked: usdcc.cmd --plugin-path
# examples\plugins, or Settings > Plugins.
SHIPPED_PLUGINS = ["vray_render"]


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


def _vcpkg_licences(licenses_dir, sources):
    """vcpkg's packages: a copyright file each, and in `sources` where its source
    is, from the SPDX file vcpkg writes (the LGPL ones, Qt and Qt Advanced Docking
    System, have to say so). vcpkg's own build helpers aren't shipped."""
    share = BUILD_DIR / "vcpkg_installed" / vcpkg_triplet(BUILD_DIR) / "share"
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


def _mesh_usd_plugins(source, destination, copier):
    # Local USD prefixes can contain unrelated, stale third-party renderers.
    # This list is reviewed against the pinned OpenUSD source, not the install.
    names = ("hdStorm", "hdEmbree", "hioAvif", "hioOiio", "hioOpenEXR", "sdrGlslfx", "usdShaders")
    destination.mkdir(parents=True, exist_ok=True)
    for name in names:
        if (source / name).is_dir():
            copier(source / name, destination / name)
        for suffix in (".dll", ".so", ".dylib"):
            if (source / (name + suffix)).is_file():
                copy_file(source / (name + suffix), destination)
    if (source / "plugInfo.json").is_file():
        copy_file(source / "plugInfo.json", destination)


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
    parser.add_argument("--clean", action="store_true",
                        help="delete the destination folder first, whatever it holds; without it only an earlier "
                             "deploy is replaced, and any other non-empty folder is refused")
    parser.add_argument("--omit-rtx", action="store_true",
                        help="omit RTX even when enabled in the CMake cache; ignore stale native build files")
    parser.add_argument("--bundle-rtx-runtime", action="store_true",
                        help="copy the pinned native SDK bin trees, metadata and licenses into RTX; otherwise "
                             "configure external runtime roots on the target")
    parser.add_argument("--with-blender-mesh-tools", action="store_true",
                        help="include the GPL-linked mesh tools configuration; excludes proprietary renderer integrations")
    parser.add_argument("--blender-source-bundle", metavar="FILE",
                        help="source candidate from tools/blender_mesh_package.py, copied beside the combined artifact")
    args = parser.parse_args(argv)
    if args.with_blender_mesh_tools:
        if args.bundle_rtx_runtime:
            fail("The Blender configuration cannot bundle the proprietary RTX runtime.")
        args.omit_rtx = True
    elif args.blender_source_bundle:
        fail("--blender-source-bundle requires --with-blender-mesh-tools")
    if MACOS:
        fail("tools/deploy.py doesn't support macOS yet: it knows Windows' and Linux' layouts.")
    if not WINDOWS:
        deploy_linux(args)
        return

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
    python_exe = cache_value(BUILD_DIR, "Python3_EXECUTABLE")
    if not python_exe or not os.path.isfile(python_exe):
        fail(f"OpenUSD's Python not found (Python3_EXECUTABLE in build/CMakeCache.txt: '{python_exe}').")
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

    package_dir = BUILD_DIR / "python/usdcc"
    if not (package_dir / "__init__.py").is_file():
        fail(f"The usdcc Python package is missing from {package_dir}; build first.")

    # --- the destination ------------------------------------------------------

    include_rtx = not args.omit_rtx and cache_on(BUILD_DIR, "USDCC_BUILD_RTX_PLUGIN")
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

    print(f"  OpenUSD libraries ({usd_install})", flush=True)
    copy_tree(usd_install / "lib", bin_dir, files="*.dll", top_level_only=True)
    copy_tree(usd_install / "bin", bin_dir, files="*.dll", top_level_only=True,
              exclude_files=["tbb*_debug.dll", "tbbbind*.dll", "tbb_preview*.dll"] if args.with_blender_mesh_tools else [])

    print("  OpenUSD plugins", flush=True)
    copy_tree(usd_install / "lib/usd", bin_dir / "usd", exclude_files=BUILD_ONLY_FILES)
    if args.with_blender_mesh_tools:
        _mesh_usd_plugins(usd_install / "plugin/usd", deploy_dir / "plugin/usd", copy_tree)
    else:
        copy_tree(usd_install / "plugin/usd", deploy_dir / "plugin/usd", exclude_files=BUILD_ONLY_FILES)

    print("  usdcc", flush=True)
    copy_tree(app_dir, bin_dir, exclude_files=BUILD_ONLY_FILES)
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
        # Headers, typesystems and import libraries are only for building bindings.
        pyside_build_only = ["__pycache__", "include", "typesystems", "glue", "lib", "doc", "scripts"]
        copy_tree(Path(pyside_dir) / "PySide6", site_packages / "PySide6",
                  exclude_files=BUILD_ONLY_FILES + ["rcc.exe", "uic.exe"], exclude_dirs=pyside_build_only)
        copy_tree(Path(pyside_dir) / "shiboken6", site_packages / "shiboken6",
                  exclude_files=BUILD_ONLY_FILES, exclude_dirs=pyside_build_only)
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

    # --- plugins --------------------------------------------------------------

    print("  Shipped plugins", flush=True)
    for plugin in ([] if args.with_blender_mesh_tools else SHIPPED_PLUGINS):
        copy_tree(REPO / "src/plugins" / plugin, deploy_dir / "plugins" / plugin, exclude_dirs=["__pycache__"])
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
              exclude_dirs=["__pycache__", "cpp_example", "rtx_render", "rtx_render_native", "blender_mesh_tools"] + SHIPPED_PLUGINS)
    copy_tree(BUILD_DIR / "plugins", deploy_dir / "examples/plugins", exclude_files=BUILD_ONLY_FILES,
              exclude_dirs=["rtx_render", "rtx_render_native", "blender_mesh_tools"] + SHIPPED_PLUGINS)
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
    for library in ("core", "tools", "ui", "usd", "plugin_sdk"):
        copy_tree(REPO / "src/cpp" / library / "include", sdk_dir / "include")
    (sdk_dir / "lib").mkdir(parents=True, exist_ok=True)
    (sdk_dir / "cmake").mkdir(parents=True, exist_ok=True)
    for library in ("core/usdcc_core", "usd/usdcc_usd", "tools/usdcc_tools", "ui/usdcc_ui", "usd/usdcc_usd_ui"):
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
        copy_tree(usd_install / "licenses", usd_dependencies)
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

    if (python_home / "LICENSE.txt").is_file():
        (licenses_dir / "Python").mkdir(parents=True, exist_ok=True)
        copy_file(python_home / "LICENSE.txt", licenses_dir / "Python")
    if has_pyside:
        # tools/build_pyside.py copies them in; an older PySide build has them in
        # pyside-setup's checkout.
        pyside_licenses = [folder for folder in (Path(pyside_dir) / "LICENSES", REPO / "external/pyside-setup/LICENSES")
                           if folder.is_dir()]
        if pyside_licenses:
            copy_tree(pyside_licenses[0], licenses_dir / "PySide6")
        else:
            warn(f"No PySide6 licences in '{pyside_dir}\\LICENSES': run tools/build_pyside.py again.")
        pyside_version = subprocess.run(
            [python_exe, "-I", "-c",
             "import sys; sys.path.insert(0, sys.argv[1]); import PySide6; print(PySide6.__version__)", pyside_dir],
            capture_output=True, text=True).stdout.strip()
        if pyside_version:
            sources.append(f"PySide6 and shiboken6 {pyside_version}, built by tools/build_pyside.py "
                           "(https://github.com/olihey/usdcc)")
            sources.append(f"  source:     https://code.qt.io/cgit/pyside/pyside-setup.git/tag/?h=v{pyside_version}")
    qwindowkit = REPO / "external/qwindowkit/LICENSE"
    if qwindowkit.is_file():
        (licenses_dir / "QWindowKit").mkdir(parents=True, exist_ok=True)
        copy_file(qwindowkit, licenses_dir / "QWindowKit")
    for tool in ("ruff", "uv"):
        tool_dir = deploy_dir / "tools" / tool
        if tool_dir.is_dir():
            (licenses_dir / tool).mkdir(parents=True, exist_ok=True)
            for name in _names(tool_dir, "LICENSE*"):
                copy_file(tool_dir / name, licenses_dir / tool)
    (licenses_dir / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")

    # --- launcher and checks --------------------------------------------------

    (deploy_dir / "usdcc.cmd").write_text('@echo off\nstart "" "%~dp0bin\\usdcc.exe" %*\n', encoding="ascii")

    required = [
        "bin/usdcc.exe", "bin/usd_plug.dll", "bin/QtNodes.dll", "bin/platforms/qwindows.dll", "bin/usd/plugInfo.json",
        "plugin/usd/hdStorm.dll", "python/python.exe", "python/Lib/encodings/__init__.py",
        "python/Lib/site-packages/usdcc/__init__.py", "python/Lib/site-packages/pxr/__init__.py",
        "python/Lib/site-packages/usdcc/_vendor/packaging/version.py",
        *([] if args.with_blender_mesh_tools else ["plugins/vray_render/usdcc-plugin.json"]),
        "examples/plugins/measure_example/usdcc-plugin.json", "examples/plugins/cpp_example/usdcc-plugin.json",
        "sdk/UsdccPluginConfig.cmake", "sdk/lib/usdcc_ui.lib", "sdk/include/usdcc/plugin/plugin.h",
        "LICENSE.txt", "licenses/OpenUSD/LICENSE.txt", "licenses/OpenUSD/NOTICE.txt", "licenses/vcpkg/qtbase.txt", "licenses/SOURCES.txt",
    ]
    if include_rtx:
        required += ["plugins/rtx_render/usdcc-plugin.json", "plugins/rtx_render/bin/windows-x64/rtx_render_native.dll"]
    missing = [name for name in required if not (deploy_dir / name).exists()]
    if missing:
        fail(f"The deploy is incomplete; missing: {', '.join(missing)}")

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
    print(f"Done: {deploy_dir} ({size / 2**20:,.0f} MB). Start it with usdcc.cmd or bin\\usdcc.exe.")


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


def _set_deploy_runpaths(deploy_dir, folders, own_folder_first=False):
    """Points the RUNPATH of every ELF file under `folders` at the deploy's lib/
    (and, for plugins, at their own folder first, as Windows finds a DLL's DLLs)."""
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

    vcpkg_dir = BUILD_DIR / "vcpkg_installed" / vcpkg_triplet(BUILD_DIR)
    own_libraries = sorted(file for file in (BUILD_DIR / "src/cpp").rglob("libusdcc_*.so")
                           if "CMakeFiles" not in file.parts and "plugins" not in file.parts)
    package_dir = BUILD_DIR / "python/usdcc"
    if not (package_dir / "__init__.py").is_file():
        fail(f"The usdcc Python package is missing from {package_dir}; build first.")
    ruff_exe = cache_value(BUILD_DIR, "USDCC_RUFF_EXE_PATH")
    uv_exe = cache_value(BUILD_DIR, "USDCC_UV_EXE_PATH")
    pyside_dir = cache_value(BUILD_DIR, "USDCC_PYSIDE_DIR")
    has_pyside = bool(pyside_dir) and os.path.isfile(os.path.join(pyside_dir, "PySide6/__init__.py"))
    if has_pyside:
        warn("PySide6 isn't deployed on Linux yet: the deploy works, but scripts can't make panels.")

    # --- the destination ------------------------------------------------------

    for protected in (REPO, BUILD_DIR, usd_install, python_home, Path.home()):
        absolute = _full(protected)
        if absolute == deploy_dir or absolute.startswith(deploy_dir + os.sep):
            fail(f"Unsafe deploy destination '{deploy_dir}' contains '{absolute}'.")
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

    print(f"  OpenUSD libraries and plugins ({usd_install})", flush=True)
    mesh_exclusions = ["*_debug.so*", "libtbbbind*.so*", "libtbb_preview*.so*"] if args.with_blender_mesh_tools else []
    _copy_linux_tree(usd_install / "lib", lib_dir, exclude_files=LINUX_BUILD_ONLY_FILES + mesh_exclusions, top_level_only=True)
    _copy_linux_tree(usd_install / "lib/usd", lib_dir / "usd", exclude_files=LINUX_BUILD_ONLY_FILES)
    if args.with_blender_mesh_tools:
        _mesh_usd_plugins(usd_install / "plugin/usd", deploy_dir / "plugin/usd", _copy_linux_tree)
    else:
        _copy_linux_tree(usd_install / "plugin/usd", deploy_dir / "plugin/usd", exclude_files=LINUX_BUILD_ONLY_FILES)

    print(f"  Qt and vcpkg's libraries ({vcpkg_dir})", flush=True)
    replaced = _copy_linux_tree(vcpkg_dir / "lib", lib_dir, exclude_files=LINUX_BUILD_ONLY_FILES, top_level_only=True)
    if replaced:
        print(f"    in place of OpenUSD's: {', '.join(sorted(set(replaced)))}")
    _copy_linux_tree(vcpkg_dir / "Qt6/plugins", deploy_dir / "Qt6/plugins", exclude_files=LINUX_BUILD_ONLY_FILES)

    print("  usdcc", flush=True)
    bin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, bin_dir / "usdcc")
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
    _copy_linux_tree(package_dir, site_packages / "usdcc", exclude_dirs=["__pycache__"])
    _copy_linux_tree(usd_python, site_packages, exclude_dirs=["__pycache__"])

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

    # --- plugins, preview, SDK ------------------------------------------------

    print("  Shipped and example plugins", flush=True)
    # V-Ray Render's panel is PySide6, which Linux doesn't deploy yet: shipped, it
    # would fail at every start. It stays out until then.
    shipped = SHIPPED_PLUGINS if has_pyside and not args.with_blender_mesh_tools else []
    if not has_pyside:
        warn(f"Without PySide6, {', '.join(SHIPPED_PLUGINS)} isn't shipped: it needs it.")
    for plugin in shipped:
        copy_tree(REPO / "src/plugins" / plugin, deploy_dir / "plugins" / plugin, exclude_dirs=["__pycache__"])
    copy_tree(REPO / "src/plugins", deploy_dir / "examples/plugins",
              exclude_dirs=["__pycache__", "cpp_example", "rtx_render", "rtx_render_native", "blender_mesh_tools"] + SHIPPED_PLUGINS)
    _copy_linux_tree(BUILD_DIR / "plugins", deploy_dir / "examples/plugins", exclude_files=LINUX_BUILD_ONLY_FILES,
                     exclude_dirs=["rtx_render", "rtx_render_native", "blender_mesh_tools", "CMakeFiles"] + SHIPPED_PLUGINS)
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
    elves += _set_deploy_runpaths(deploy_dir, [site_packages])
    elves += _set_deploy_runpaths(deploy_dir, [deploy_dir / "plugins", deploy_dir / "examples/plugins"],
                                  own_folder_first=True)
    from _elf import is_elf, set_runpath
    for file in sorted(lib_dir.iterdir()):
        if is_elf(file):
            set_runpath(file, ["$ORIGIN"])
            elves.append(file)

    # --- licences (docs/RELEASE.md section 6) ---------------------------------

    print("  Licences", flush=True)
    licenses_dir = deploy_dir / "licenses"
    licenses_dir.mkdir(parents=True, exist_ok=True)
    copy_file(REPO / "LICENSE", deploy_dir / "LICENSE.txt")
    _usd_notices(usd_install, licenses_dir)
    if (usd_install / "licenses").is_dir():
        copy_tree(usd_install / "licenses", licenses_dir / "OpenUSD-dependencies")
    else:
        warn(f"No licences of OpenUSD's dependencies in '{usd_install}/licenses' (tools/build_usd.py collects "
             "them): the deploy works, but can't be distributed as it is.")
    sources = [
        "Where the source of each library in this folder is. The LGPL ones (Qt and Qt Advanced Docking",
        "System) are separate shared libraries, which you may replace with your own builds of the same",
        "version.",
        "",
    ]
    _vcpkg_licences(licenses_dir, sources)
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
    for tool in ("ruff", "uv"):
        tool_dir = deploy_dir / "tools" / tool
        if tool_dir.is_dir():
            (licenses_dir / tool).mkdir(parents=True, exist_ok=True)
            for name in _names(tool_dir, "LICENSE*"):
                copy_file(tool_dir / name, licenses_dir / tool)
    (licenses_dir / "SOURCES.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")

    # --- launcher and checks --------------------------------------------------

    launcher = deploy_dir / "usdcc"
    launcher.write_text('#!/bin/sh\n# Starts usdcc from this folder, wherever it is.\n'
                        'exec "$(dirname "$(readlink -f "$0")")/bin/usdcc" "$@"\n', encoding="ascii")
    launcher.chmod(0o755)

    required = [
        "bin/usdcc", f"bin/python{python_version}", "lib/libusd_plug.so", "lib/libQt6Core.so.6", "lib/libusdcc_core.so",
        f"lib/{libpython.name}", "lib/usd/plugInfo.json", "plugin/usd/hdStorm.so",
        "Qt6/plugins/platforms/libqxcb.so", "Qt6/plugins/platforms/libqoffscreen.so",
        f"lib/python{python_version}/encodings/__init__.py", f"lib/python{python_version}/site-packages/usdcc/__init__.py",
        f"lib/python{python_version}/site-packages/pxr/__init__.py",
        f"lib/python{python_version}/site-packages/usdcc/_vendor/packaging/version.py",
        *(f"plugins/{plugin}/usdcc-plugin.json" for plugin in shipped),
        "examples/plugins/measure_example/usdcc-plugin.json", "examples/plugins/cpp_example/usdcc-plugin.json",
        "sdk/UsdccPluginConfig.cmake", "sdk/lib/libusdcc_ui.so", "sdk/include/usdcc/plugin/plugin.h",
        "LICENSE.txt", "licenses/OpenUSD/LICENSE.txt", "licenses/OpenUSD/NOTICE.txt", "licenses/vcpkg/qtbase.txt", "licenses/SOURCES.txt",
    ]
    missing = [name for name in required if not (deploy_dir / name).exists()]
    if missing:
        fail(f"The deploy is incomplete; missing: {', '.join(missing)}")

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
    check = subprocess.run(
        [str(python), "-c", f"import sys; sys.path.insert(0, {str(site_packages)!r}); "
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

    size = sum(file.stat().st_size for file in deploy_dir.rglob("*") if file.is_file() and not file.is_symlink())
    print(f"Done: {deploy_dir} ({size / 2**20:,.0f} MB). Start it with ./usdcc or bin/usdcc.")


if __name__ == "__main__":
    main()
