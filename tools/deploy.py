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

    python tools/deploy.py                       into .\\deploy
    python tools/deploy.py D:\\dist\\usdcc --configuration Release
    python tools/deploy.py D:\\dist\\usdcc --clean
    python tools/deploy.py --usd-install C:\\usd\\install

Windows only so far: the layout above is Windows'. Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
import usd_licenses  # noqa: E402
from _common import (  # noqa: E402
    CONFIGURATIONS, REPO, cache_on, cache_value, copy_file, copy_tree, fail, remove_tree, require_windows,
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
    args = parser.parse_args(argv)
    require_windows("tools/deploy.py")

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
    copy_tree(usd_install / "bin", bin_dir, files="*.dll", top_level_only=True)

    print("  OpenUSD plugins", flush=True)
    copy_tree(usd_install / "lib/usd", bin_dir / "usd", exclude_files=BUILD_ONLY_FILES)
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
    for plugin in SHIPPED_PLUGINS:
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
              exclude_dirs=["__pycache__", "cpp_example", "rtx_render", "rtx_render_native"] + SHIPPED_PLUGINS)
    copy_tree(BUILD_DIR / "plugins", deploy_dir / "examples/plugins", exclude_files=BUILD_ONLY_FILES,
              exclude_dirs=["rtx_render", "rtx_render_native"])

    # --- Material Editor preview (docs/MATERIAL_EDITOR.md section 7) ----------

    # Next to bin\, where shippedPreviewFolder() looks: the shapes and HDRIs that
    # tools/gen_preview_assets.py made.
    print("  Material preview shapes and HDRIs", flush=True)
    copy_tree(REPO / "resources/preview", deploy_dir / "resources/preview")

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
    usd_licenses_dir = licenses_dir / "OpenUSD"
    usd_licenses_dir.mkdir(parents=True, exist_ok=True)
    for file in ("LICENSE", "THIRD-PARTY.md"):
        if (usd_install / file).is_file():
            copy_file(usd_install / file, usd_licenses_dir)
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
        "plugins/vray_render/usdcc-plugin.json",
        "examples/plugins/measure_example/usdcc-plugin.json", "examples/plugins/cpp_example/usdcc-plugin.json",
        "sdk/UsdccPluginConfig.cmake", "sdk/lib/usdcc_ui.lib", "sdk/include/usdcc/plugin/plugin.h",
        "LICENSE.txt", "licenses/OpenUSD/LICENSE", "licenses/vcpkg/qtbase.txt", "licenses/SOURCES.txt",
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


if __name__ == "__main__":
    main()
