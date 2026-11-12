"""Builds PySide6/Shiboken6 from source against usdcc's vcpkg Qt build.

pip-installed PySide6 ships its own, independently-compiled Qt binaries. Loading
those alongside usdcc's vcpkg-built Qt in one process causes "DLL load failed:
the specified procedure could not be found" at essentially random points,
because Windows resolves a same-named DLL (Qt6Widgets.dll, etc.) to whichever
copy is already loaded in the process, not necessarily the one usdcc's own code
was compiled against. There's no reliable way around this other than making
sure there's only one Qt build in the process — hence building
PySide6/Shiboken6 ourselves against the *same* vcpkg Qt.

It's built for OpenUSD's Python, the one usdcc embeds (docs/SCRIPTING.md section
3.1): the Python3_EXECUTABLE usdcc's build found (build/CMakeCache.txt), unless
--python names another.

This script automates the recipe that was worked out by hand:
  1. Bootstrap the vcpkg build (for Qt + qtpaths6) if not already done.
  2. Fetch a prebuilt libclang (shiboken6's C++ parser) if not present.
  3. Clone pyside-setup at a matching tag if not present.
  4. Build shiboken6-generator, shiboken6, and pyside6 individually against
     vcpkg's Qt (building them together in one `setup.py build` invocation only
     keeps the *last* stage's packaged output — the packaging step wipes and
     reuses one shared directory per stage).
  5. Assemble PySide6 + shiboken6 + shiboken6_generator into one directory,
     patching in the runtime DLLs pyside-setup's packaging step doesn't know to
     copy for a non-official Qt build (ICU, zstd, etc. — whatever vcpkg's Qt
     actually links against).

Output: external/pyside-install/{PySide6,shiboken6,shiboken6_generator}, where
usdcc's CMake looks for it (USDCC_PYSIDE_DIR): re-run CMake after this to build
the usdcc.ui module (src/cpp/ui/python/CMakeLists.txt).

Known workarounds baked in below, each tied to a specific build failure
encountered while developing this script:
  - CMAKE_PREFIX_PATH env var: shiboken6_generator's own CMake configure step
    doesn't inherit -DCMAKE_PREFIX_PATH the way the top-level setup.py
    orchestration does.
  - share/Qt6/metatypes junction: pyside-setup's packaging step expects Qt's
    metatypes JSON files there; vcpkg installs them at the top-level metatypes/
    dir instead.
  - install/plugins/designer: pyside-setup unconditionally copies this even when
    Qt6Designer isn't part of --module-subset.
  - extra runtime DLLs: pyside-setup's packaging step bundles a fixed list of
    third-party DLLs it expects an official Qt build to need; vcpkg's Qt has a
    different (larger) list.

    python tools/build_pyside.py

Windows only so far: the compiler setup, the libclang download and the DLL list
are Windows'. Standard library only.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import REPO, cache_value, copy_file, copy_tree, fail, remove_tree, require_windows, run, vcpkg_triplet  # noqa: E402

# tools/ci/pyside_asset.py reads the tag from this line.
PYSIDE_TAG = "v6.11.1"
LIBCLANG_VERSION = "19.1.0"
LIBCLANG_URL = ("https://download.qt.io/development_releases/prebuilt/libclang/"
                f"libclang-release_{LIBCLANG_VERSION}-based-windows-vs2019_64.7z")

BUILD_DIR = REPO / "build"
EXTERNAL_DIR = REPO / "external"
LIBCLANG_DIR = EXTERNAL_DIR / "libclang"
PYSIDE_SRC_DIR = EXTERNAL_DIR / "pyside-setup"
PYSIDE_INSTALL_DIR = EXTERNAL_DIR / "pyside-install"

# The stages pyside-setup builds one at a time, and the package each one makes.
STAGES = {"shiboken6-generator": "shiboken6_generator", "shiboken6": "shiboken6", "pyside6": "PySide6"}

# vcpkg's Qt links against a specific set of third-party DLLs that pyside-setup's
# packaging step doesn't know about (it bundles a fixed list tuned for official
# Qt builds). Mirror what CMake's vcpkg-applocal-deps already copies next to
# usdcc.exe.
EXTRA_RUNTIME_DLLS = (
    "icuin78.dll", "icuuc78.dll", "icudt78.dll", "z.dll", "double-conversion.dll",
    "pcre2-16.dll", "zstd.dll", "libpng16.dll", "harfbuzz.dll", "freetype.dll",
    "bz2.dll", "brotlidec.dll", "brotlicommon.dll", "md4c.dll")


def enter_visual_studio_environment():
    """Takes on the Visual Studio developer environment, as a developer prompt has it."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    found = subprocess.run(
        [str(vswhere), "-products", "*", "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True) if vswhere.is_file() else None
    visual_studio = found.stdout.strip().splitlines()[0] if found and found.stdout.strip() else None
    if not visual_studio:
        fail("No Visual Studio with the C++ x64 tools found (vswhere).")
    setup = Path(visual_studio) / "Common7/Tools/VsDevCmd.bat"
    # Run it in a cmd, then read back the environment it left.
    variables = subprocess.run(f'"{setup}" -arch=x64 -host_arch=x64 -no_logo && set', shell=True,
                               capture_output=True, encoding="oem", errors="replace")
    if variables.returncode != 0:
        fail(f"'{setup}' failed: {variables.stderr.strip()}")
    for line in variables.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator and name:
            os.environ[name] = value


def fetch_libclang():
    print(f"Downloading libclang {LIBCLANG_VERSION} (~700 MB)...", flush=True)
    archive = Path(tempfile.gettempdir()) / "libclang.7z"
    urllib.request.urlretrieve(LIBCLANG_URL, archive)
    extract_dir = Path(tempfile.gettempdir()) / "libclang-extract"
    if extract_dir.exists():
        remove_tree(extract_dir)
    if subprocess.run(["7z", "x", str(archive), f"-o{extract_dir}", "-y"], stdout=subprocess.DEVNULL).returncode != 0:
        fail("7z extraction of libclang failed")
    LIBCLANG_DIR.mkdir(parents=True, exist_ok=True)
    for entry in (extract_dir / "libclang").iterdir():
        shutil.move(str(entry), str(LIBCLANG_DIR / entry.name))
    remove_tree(extract_dir)
    archive.unlink()


def pyside_build_dir():
    """pyside-setup's build folder for this Python and Qt, once a build has made it."""
    folders = sorted(folder for folder in (PYSIDE_SRC_DIR / "build").glob("qfpa-*") if folder.is_dir())
    return folders[0] if folders else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--python", help="the Python to build for (default: OpenUSD's, as found by usdcc's build)")
    parser.add_argument("--module-subset", default="Core,Gui,Widgets",
                        help="Qt modules to build PySide6 bindings for (default: Core,Gui,Widgets, what usdcc uses). "
                             "A larger subset needs a full re-run, not --reuse-build, since new modules haven't "
                             "been configured or compiled before")
    args = parser.parse_args(argv)
    require_windows("tools/build_pyside.py")

    vcpkg_installed = BUILD_DIR / "vcpkg_installed" / vcpkg_triplet(BUILD_DIR)

    # --- prerequisites --------------------------------------------------------

    python = args.python or cache_value(BUILD_DIR, "Python3_EXECUTABLE")
    if not python:
        fail(f"No Python3_EXECUTABLE in {BUILD_DIR / 'CMakeCache.txt'}. Run tools/build.py first, or pass --python.")
    version = run([python, "-c", "import sys; print(sys.version.split()[0])"], capture_output=True, text=True)
    print(f"Building for {python} ({version.stdout.strip()})", flush=True)

    # pyside-setup builds with Ninja and cl.exe, so it needs the Visual Studio
    # developer environment when this isn't already running in one.
    if not shutil.which("cl.exe"):
        enter_visual_studio_environment()

    qtpaths = vcpkg_installed / "tools/Qt6/bin/qtpaths6.exe"
    if not qtpaths.is_file():
        fail(f"vcpkg Qt build not found at {vcpkg_installed}. Run tools/build.py first.")

    if not shutil.which("ninja"):
        print("Installing ninja (build tool required by pyside-setup)...", flush=True)
        run([python, "-m", "pip", "install", "--user", "ninja"])
        # Where pip put ninja.exe (with --user, not next to the package).
        ninja_bin = run([python, "-c", "import ninja; print(ninja.BIN_DIR)"], capture_output=True, text=True)
        os.environ["PATH"] = ninja_bin.stdout.strip() + os.pathsep + os.environ["PATH"]

    print("Installing pyside-setup Python build dependencies...", flush=True)
    run([python, "-m", "pip", "install", "--user", "packaging", "ordered-set", "more-itertools", "jaraco.text",
         "importlib_metadata", "importlib_resources", "tomli", "platformdirs", "wheel", "setuptools"])

    if not (LIBCLANG_DIR / "lib/cmake/clang/ClangConfig.cmake").is_file():
        fetch_libclang()

    if not (PYSIDE_SRC_DIR / "setup.py").is_file():
        print(f"Cloning pyside-setup ({PYSIDE_TAG})...", flush=True)
        run(["git", "clone", "--branch", PYSIDE_TAG, "--depth", "1", "https://code.qt.io/pyside/pyside-setup.git",
             PYSIDE_SRC_DIR])

    # pyside-setup's packaging step expects Qt's metatypes at share/Qt6/metatypes;
    # vcpkg installs them at the top-level metatypes/ dir instead.
    metatypes_link = vcpkg_installed / "share/Qt6/metatypes"
    if not metatypes_link.exists():
        run(["cmd", "/c", "mklink", "/J", metatypes_link, vcpkg_installed / "metatypes"], stdout=subprocess.DEVNULL)

    os.environ["LLVM_INSTALL_DIR"] = str(LIBCLANG_DIR)
    os.environ["CMAKE_PREFIX_PATH"] = str(vcpkg_installed)

    cmake = shutil.which("cmake")
    if not cmake:
        fail("'cmake' not found; is it installed and on PATH?")
    jobs = os.cpu_count() or 1

    def setup_build(stage):
        run([python, "setup.py", "build", f"--qtpaths={qtpaths}", f"--cmake={cmake}",
             f"--module-subset={args.module_subset}", "--limited-api=yes", "--ignore-git", f"--parallel={jobs}",
             "--skip-docs", "--disable-pyi", "--reuse-build", f"--internal-build-type={stage}"], cwd=PYSIDE_SRC_DIR)

    # pyside-setup unconditionally copies install/plugins/designer during
    # packaging even when Qt6Designer isn't in --module-subset.
    build_dir = pyside_build_dir()
    if build_dir:
        (build_dir / "install/plugins/designer").mkdir(parents=True, exist_ok=True)

    print("Building shiboken6-generator, shiboken6, and pyside6 against vcpkg Qt...")
    print("(first run compiles all of Qt Core/Gui/Widgets bindings from source; can take 20+ minutes)", flush=True)
    setup_build("shiboken6-generator")

    # The designer plugin dir may only be created by the build above (its own
    # build dir didn't exist until now on a first-ever run) — ensure it exists
    # before proceeding to the next stage, which also packages.
    build_dir = pyside_build_dir()
    if not build_dir:
        fail(f"pyside-setup made no build folder in {PYSIDE_SRC_DIR / 'build'}")
    (build_dir / "install/plugins/designer").mkdir(parents=True, exist_ok=True)

    setup_build("shiboken6")
    setup_build("pyside6")

    # --- assemble the redistributable install dir -----------------------------

    package_dir = build_dir / "package"

    print(f"Assembling {PYSIDE_INSTALL_DIR} ...", flush=True)
    if PYSIDE_INSTALL_DIR.exists():
        remove_tree(PYSIDE_INSTALL_DIR)
    PYSIDE_INSTALL_DIR.mkdir(parents=True)

    # Each of the three stages above packages into the same folder, overwriting
    # the previous stage's output — so re-run each stage's install step alone
    # (--reuse-build makes this fast) right before harvesting it.
    for stage, package in STAGES.items():
        setup_build(stage)
        copy_tree(package_dir / package, PYSIDE_INSTALL_DIR / package)

    # shiboken6.abi3.dll is needed by PySide6's own .pyd files but only gets
    # packaged into the shiboken6 folder, not alongside PySide6.
    copy_file(PYSIDE_INSTALL_DIR / "shiboken6/shiboken6.abi3.dll", PYSIDE_INSTALL_DIR / "PySide6")

    # The generator (shiboken6.exe, which usdcc's build runs) needs the extra
    # DLLs too, for its Qt6Core.dll, and libclang.dll, its C++ parser.
    for dll in EXTRA_RUNTIME_DLLS:
        source = vcpkg_installed / "bin" / dll
        if source.is_file():
            copy_file(source, PYSIDE_INSTALL_DIR / "PySide6")
            copy_file(source, PYSIDE_INSTALL_DIR / "shiboken6_generator")
    copy_file(LIBCLANG_DIR / "bin/libclang.dll", PYSIDE_INSTALL_DIR / "shiboken6_generator")

    # The licences PySide6 and shiboken6 are under (LGPL v3, among others), which
    # tools/deploy.py ships in the deploy's licenses folder (docs/RELEASE.md section 6).
    copy_tree(PYSIDE_SRC_DIR / "LICENSES", PYSIDE_INSTALL_DIR / "LICENSES")

    print("Done. PySide6/shiboken6/shiboken6_generator built against vcpkg's Qt at:")
    print(f"  {PYSIDE_INSTALL_DIR}")
    print()
    print("Re-run CMake (tools/build.py) to build the usdcc.ui module against it.")


if __name__ == "__main__":
    main()
