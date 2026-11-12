"""Configures and builds usdcc.

Bootstraps the vcpkg submodule if needed, then runs CMake configure and build.
Mirrors the manual steps documented in readme.md.

    python tools/build.py
    python tools/build.py --configuration Debug --run
    python tools/build.py --usd-install C:\\usd\\install
    python tools/build.py --run --scene test_data/cube_and_sphere.usda

On Windows it builds with Visual Studio. On Linux and macOS it uses Ninja when
there is one on PATH (else CMake's default generator) and vcpkg's dynamic
triplet; neither has been built there yet (docs/CI.md).

Standard library only. `python tools/build.py --help` lists the options.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import (  # noqa: E402
    CONFIGURATIONS, EXE_SUFFIX, MACOS, REPO, VCPKG_BOOTSTRAP, VCPKG_EXE, VCPKG_TRIPLET, WINDOWS,
    cache_on, cache_value, fail, remove_tree, run)

BUILD_DIR = REPO / "build"
VCPKG_DIR = REPO / "external/vcpkg"


def parser(description=None):
    result = argparse.ArgumentParser(description=description or __doc__.split("\n\n")[0])
    result.add_argument("--configuration", choices=CONFIGURATIONS, default="RelWithDebInfo",
                        help="CMake build configuration (default: RelWithDebInfo)")
    result.add_argument("--generator", help='CMake generator (default on Windows: "Visual Studio 17 2022")')
    result.add_argument("--arch", default="x64", help="target architecture, for Visual Studio's -A (default: x64)")
    result.add_argument("--usd-install", metavar="DIR",
                        help="an OpenUSD install (holding pxrConfig.cmake); the build remembers it")
    result.add_argument("--usdcc-version", default="", metavar="VERSION",
                        help="the release's version (0.2.0, 0.2.0-beta.1), for a release build; release.yml "
                             "passes the release's tag without its v. Without it, it's a development build, "
                             "named after the last release tag (docs/RELEASE.md)")
    result.add_argument("--scene", metavar="FILE", help="a USD stage to open on launch; only with --run")
    result.add_argument("--with-rtx", action="store_true",
                        help="enable native RTX; give matching --ovrtx-root/--ovstage-root package roots and a "
                             "CUDA toolkit. Later builds keep these CMake cache values")
    result.add_argument("--without-rtx", action="store_true",
                        help="disable native RTX and GPU tests (what a fresh build does)")
    result.add_argument("--ovrtx-root", metavar="DIR")
    result.add_argument("--ovstage-root", metavar="DIR")
    result.add_argument("--rtx-gpu-tests", action="store_true",
                        help="enable labeled GPU tests; build rtx_gpu_test_bins before running CTest")
    result.add_argument("--clean", action="store_true", help="remove the build directory before configuring")
    result.add_argument("--run", action="store_true", help="launch usdcc after a successful build")
    return result


def _multi_config(generator):
    return bool(generator) and (generator.startswith("Visual Studio") or generator == "Xcode"
                                or "Multi-Config" in generator)


def executable(configuration):
    """Where the build puts usdcc."""
    folder = BUILD_DIR / "src/cpp/app"
    if cache_value(BUILD_DIR, "CMAKE_CONFIGURATION_TYPES"):
        folder = folder / configuration
    if MACOS:
        return folder / "usdcc.app/Contents/MacOS/usdcc"
    return folder / f"usdcc{EXE_SUFFIX}"


def launch(exe, usd_install, scene):
    # USD_INSTALL is a CMake cache variable: once configured, it sticks around
    # across future builds without needing --usd-install again. But the
    # environment below only has the value when the caller passes it, so read it
    # back from the cache as a fallback — otherwise a later `tools/run.py` with
    # no arguments launches without USD's libraries on the path and fails with
    # "usd_usdImagingGL.dll was not found" even though the build itself succeeds.
    usd_install = usd_install or cache_value(BUILD_DIR, "USD_INSTALL")

    environment = dict(os.environ)
    if usd_install:
        # USD's libraries (split across bin/ and lib/ on Windows) and Hydra
        # render-delegate plugins (e.g. hdEmbree) aren't copied next to usdcc;
        # point the launched process at the USD install instead.
        if WINDOWS:
            folders = [os.path.join(usd_install, "bin"), os.path.join(usd_install, "lib")]
            variable = "PATH"
        else:
            folders = [os.path.join(usd_install, "lib")]
            variable = "DYLD_LIBRARY_PATH" if MACOS else "LD_LIBRARY_PATH"
        environment[variable] = os.pathsep.join(folders + [environment.get(variable, "")]).rstrip(os.pathsep)
        environment["PXR_PLUGINPATH_NAME"] = os.path.join(usd_install, "plugin", "usd")

    arguments = []
    if cache_on(BUILD_DIR, "USDCC_BUILD_RTX_PLUGIN"):
        arguments += ["--plugin-path", str(BUILD_DIR / "plugins")]
    if scene:
        # Against the caller's folder, whatever the app's own turns out to be: a
        # scene it can't find leaves the app looking as if nothing was asked for.
        scene = Path(scene).resolve()
        if not scene.exists():
            fail(f"No such scene: {scene}")
        arguments.append(str(scene))

    # This is the user's interactive app: leave it running on its own.
    detached = ({"creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS
                else {"start_new_session": True})
    subprocess.Popen([str(exe)] + arguments, env=environment, stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **detached)


def main(argv=None, description=None):
    args = parser(description).parse_args(argv)
    rtx_options = args.with_rtx or args.ovrtx_root or args.ovstage_root or args.rtx_gpu_tests
    if args.without_rtx and rtx_options:
        fail("--without-rtx cannot be combined with native RTX options.")

    # --- vcpkg submodule ------------------------------------------------------

    bootstrap = VCPKG_DIR / VCPKG_BOOTSTRAP
    if not bootstrap.is_file():
        print("vcpkg submodule not initialized, running git submodule update --init --recursive...")
        run(["git", "-C", REPO, "submodule", "update", "--init", "--recursive"])
    if not (VCPKG_DIR / VCPKG_EXE).is_file():
        print("Bootstrapping vcpkg...")
        run(([] if WINDOWS else ["sh"]) + [bootstrap, "-disableMetrics"])

    # --- configure ------------------------------------------------------------

    if args.clean and BUILD_DIR.exists():
        print(f"Removing existing build directory: {BUILD_DIR}")
        remove_tree(BUILD_DIR)

    generator = args.generator
    if not generator:
        if WINDOWS:
            generator = "Visual Studio 17 2022"
        else:
            # The one the build folder already has, so a second run doesn't clash.
            generator = cache_value(BUILD_DIR, "CMAKE_GENERATOR") or ("Ninja" if shutil.which("ninja") else None)

    configure = ["cmake", "-S", REPO, "-B", BUILD_DIR]
    if generator:
        configure += ["-G", generator]
    if generator and generator.startswith("Visual Studio"):
        configure += ["-A", args.arch]
    if not _multi_config(generator):
        configure.append(f"-DCMAKE_BUILD_TYPE={args.configuration}")
    if not WINDOWS:
        configure.append(f"-DVCPKG_TARGET_TRIPLET={VCPKG_TRIPLET}")
    if args.usd_install:
        configure.append(f"-DUSD_INSTALL={args.usd_install}")
    # Always, so a release build's version doesn't stay in the CMake cache for
    # the development builds after it.
    configure.append(f"-DUSDCC_VERSION={args.usdcc_version}")
    if rtx_options:
        configure.append("-DUSDCC_BUILD_RTX_PLUGIN=ON")
    if args.without_rtx:
        configure += ["-DUSDCC_BUILD_RTX_PLUGIN=OFF", "-DUSDCC_RTX_GPU_TESTS=OFF"]
    if args.ovrtx_root:
        configure.append(f"-DUSDCC_RTX_OVRTX_ROOT={args.ovrtx_root}")
    if args.ovstage_root:
        configure.append(f"-DUSDCC_RTX_OVSTAGE_ROOT={args.ovstage_root}")
    if args.rtx_gpu_tests:
        configure.append("-DUSDCC_RTX_GPU_TESTS=ON")

    print(f"Configuring ({generator or 'default generator'})...")
    run(configure)

    # --- build ----------------------------------------------------------------

    print(f"Building ({args.configuration})...")
    # --parallel: several projects at a time (each compiles its files in
    # parallel too, within one shared limit; see the top-level CMakeLists.txt).
    run(["cmake", "--build", BUILD_DIR, "--config", args.configuration, "--parallel"])

    exe = executable(args.configuration)
    print(f"Build succeeded: {exe}")

    if args.run:
        print("Launching usdcc...")
        launch(exe, args.usd_install, args.scene)


if __name__ == "__main__":
    main()
