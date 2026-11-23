"""Builds PySide6/Shiboken6 from source against usdcc's vcpkg Qt build.

pip-installed PySide6 ships its own, independently-compiled Qt binaries. Loading
those alongside usdcc's vcpkg-built Qt in one process causes "DLL load failed:
the specified procedure could not be found" at essentially random points on
Windows, and two QApplications on Linux: Windows resolves a same-named DLL (Qt6Widgets.dll, etc.) to whichever
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
  5. Assemble PySide6 + shiboken6 + shiboken6_generator into one directory.
     On Windows, patching in the runtime DLLs pyside-setup's packaging step
     doesn't know to copy for a non-official Qt build (ICU, zstd, etc. — whatever
     vcpkg's Qt actually links against). On Linux and macOS no Qt is copied at all: the
     RUNPATHs lead from external/pyside-install to the Qt in
     build/vcpkg_installed, the one the app itself loads (tools/deploy.py points
     them at the deploy's lib instead).

Output: external/pyside-install/{PySide6,shiboken6,shiboken6_generator}, where
usdcc's CMake looks for it (USDCC_PYSIDE_DIR): re-run CMake after this to build
the usdcc.ui module (src/cpp/ui/python/CMakeLists.txt).

Known workarounds baked in below, each tied to a specific build failure
encountered while developing this script:
  - CMAKE_PREFIX_PATH env var: shiboken6_generator's own CMake configure step
    doesn't inherit -DCMAKE_PREFIX_PATH the way the top-level setup.py
    orchestration does.
  - share/Qt6/metatypes link: pyside-setup's packaging step expects Qt's
    metatypes JSON files there; vcpkg installs them at the top-level metatypes/
    dir instead.
  - install/plugins/designer: pyside-setup unconditionally copies this even when
    Qt6Designer isn't part of --module-subset.
  - extra runtime DLLs (Windows): pyside-setup's packaging step bundles a fixed list of
    third-party DLLs it expects an official Qt build to need; vcpkg's Qt has a
    different (larger) list.

    python tools/build_pyside.py

Windows and Linux, x64 and arm64, and macOS on Apple Silicon. Linux needs GCC, Ninja,
pip and patchelf; macOS Xcode, Ninja and pip.
Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import (  # noqa: E402
    ASSET_PLATFORM, EXE_SUFFIX, MACOS, REPO, VS_ARCH, WINDOWS, cache_value, copy_file, copy_tree, fail,
    remove_tree, run, vcpkg_triplet)

# tools/ci/pyside_asset.py reads the tag from this line.
PYSIDE_TAG = "v6.11.1"
LIBCLANG_VERSION = "19.1.0"
# Qt's build of it for each platform, and its SHA-256 (from the .sha256 beside it).
LIBCLANG_BUILDS = {
    "win64": ("windows-vs2019_64", "e06e793c133aaf22ff1ffab877e5ca36181f19edb765060bafa506ce5950698c"),
    "win-arm64": ("windows-vs2022_arm64", "3164e3a5c24e639f63383adf58f0d37175282fed113da60adb4b08764aad9dd5"),
    "linux-x64": ("linux-Ubuntu22.04-gcc11.4-x86_64",
                  "12921c4313e14bfaa1dedd0385b707fe7d538c8bec5636beea8baf127124d22e"),
    "linux-arm64": ("linux-Debian-11.6-gcc10.2-arm64",
                    "0b74255cddda352435b9e746e25de40a08f15d85cb9da038efea08fab81410ba"),
    "macos-arm64": ("macos-universal", "8f3bd9462b988abbb359affc26840f655ff1de24e24dd80c18274adc2e6bdb0d"),
}

BUILD_DIR = REPO / "build"
EXTERNAL_DIR = REPO / "external"
LIBCLANG_DIR = EXTERNAL_DIR / "libclang"
PYSIDE_SRC_DIR = EXTERNAL_DIR / "pyside-setup"
PYSIDE_INSTALL_DIR = EXTERNAL_DIR / "pyside-install"
# pyside-setup's Python build dependencies on Linux, where the system's Python
# may not take packages of its own (PEP 668).
PIP_DIR = EXTERNAL_DIR / "pyside-pip"
VENV_DIR = EXTERNAL_DIR / "pyside-venv"

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
    arch, component = ("arm64", "ARM64") if VS_ARCH == "ARM64" else ("x64", "x86.x64")
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    found = subprocess.run(
        [str(vswhere), "-products", "*", "-latest", "-requires", f"Microsoft.VisualStudio.Component.VC.Tools.{component}",
         "-property", "installationPath"],
        capture_output=True, text=True) if vswhere.is_file() else None
    visual_studio = found.stdout.strip().splitlines()[0] if found and found.stdout.strip() else None
    if not visual_studio:
        fail(f"No Visual Studio with the C++ {arch} tools found (vswhere).")
    setup = Path(visual_studio) / "Common7/Tools/VsDevCmd.bat"
    # Run it in a cmd, then read back the environment it left.
    variables = subprocess.run(f'"{setup}" -arch={arch} -host_arch={arch} -no_logo && set', shell=True,
                               capture_output=True, encoding="oem", errors="replace")
    if variables.returncode != 0:
        fail(f"'{setup}' failed: {variables.stderr.strip()}")
    for line in variables.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator and name:
            os.environ[name] = value


def fetch_libclang():
    if ASSET_PLATFORM not in LIBCLANG_BUILDS:
        fail(f"No libclang known for {ASSET_PLATFORM} (LIBCLANG_BUILDS).")
    build, sha256 = LIBCLANG_BUILDS[ASSET_PLATFORM]
    name = f"libclang-release_{LIBCLANG_VERSION}-based-{build}.7z"
    print(f"Downloading {name} (~700 MB)...", flush=True)
    with tempfile.TemporaryDirectory() as work:
        archive = Path(work) / name
        urllib.request.urlretrieve(f"https://download.qt.io/development_releases/prebuilt/libclang/{name}", archive)
        digest = hashlib.sha256()
        with open(archive, "rb") as file:
            while block := file.read(1 << 20):
                digest.update(block)
        if digest.hexdigest() != sha256:
            fail(f"{name} has the SHA-256 {digest.hexdigest()}, not {sha256}")
        # CMake reads 7z archives itself, so there needn't be a 7z.
        run(["cmake", "-E", "tar", "xf", archive], cwd=work)
        LIBCLANG_DIR.mkdir(parents=True, exist_ok=True)
        for entry in (Path(work) / "libclang").iterdir():
            shutil.move(str(entry), str(LIBCLANG_DIR / entry.name))


def pyside_build_dir():
    """pyside-setup's build folder for this Python and Qt, once a build has made it."""
    folders = sorted(folder for folder in (PYSIDE_SRC_DIR / "build").glob("qfp*-*") if folder.is_dir())
    return folders[0] if folders else None


def find_qtpaths(vcpkg_installed):
    """The qtpaths that describes the Qt in `vcpkg_installed`."""
    qtpaths = vcpkg_installed / f"tools/Qt6/bin/qtpaths6{EXE_SUFFIX}"
    if qtpaths.is_file():
        return qtpaths
    # On Linux vcpkg builds Qt's programs for the host triplet only (x64-linux,
    # beside x64-linux-dynamic), and the dynamic one's qt.conf says where its Qt is.
    qtconf = vcpkg_installed / "tools/Qt6/bin/qt.conf"
    host = vcpkg_installed.parent / vcpkg_installed.name.replace("-dynamic", "") / "tools/Qt6/bin/qtpaths6"
    if WINDOWS or not qtconf.is_file() or not host.is_file():
        fail(f"vcpkg Qt build not found at {vcpkg_installed}. Run tools/build.py first.")
    wrapper = EXTERNAL_DIR / "pyside-qtpaths/qtpaths6"
    wrapper.parent.mkdir(parents=True, exist_ok=True)
    wrapper.write_text(f'#!/bin/sh\nexec "{host}" --qtconf "{qtconf}" "$@"\n', encoding="utf-8")
    wrapper.chmod(0o755)
    return wrapper


def install_build_dependencies(python):
    """pyside-setup's Python build dependencies. Returns the Python to run it with."""
    print("Installing pyside-setup Python build dependencies...", flush=True)
    packages = ["packaging", "ordered-set", "more-itertools", "jaraco.text", "importlib_metadata",
                "importlib_resources", "tomli", "platformdirs", "wheel", "setuptools"]
    if WINDOWS:
        run([python, "-m", "pip", "install", "--user", *packages])
        return python
    run([python, "-m", "pip", "install", "--quiet", "--upgrade", "--target", PIP_DIR, *packages])
    os.environ["PYTHONPATH"] = os.pathsep.join(filter(None, [str(PIP_DIR), os.environ.get("PYTHONPATH")]))
    # A venv of it, as Debian's own Python installs into dist-packages, where
    # pyside-setup doesn't look. Without pip: Debian's lacks ensurepip.
    run([python, "-m", "venv", "--without-pip", VENV_DIR])
    return VENV_DIR / "bin/python"


def finish_windows(vcpkg_installed):
    """The DLLs pyside-setup's packaging leaves out, beside the modules and the generator."""
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


def finish_linux(vcpkg_installed):
    """libclang beside the generator, and RUNPATHs that lead to it, to shiboken6 and to vcpkg's Qt."""
    from _elf import elf_files, needed, origin_relative, set_runpath
    generator = PYSIDE_INSTALL_DIR / "shiboken6_generator"
    libclang = next((name for name in needed(generator / "shiboken6") if name.startswith("libclang.so")), None)
    if not libclang:
        fail(f"{generator / 'shiboken6'} doesn't link libclang")
    # Under the name it's loaded by, the file itself (the build's lib has links to it).
    shutil.copy2((LIBCLANG_DIR / "lib" / libclang).resolve(), generator / libclang)

    carried = {file.name for file in PYSIDE_INSTALL_DIR.rglob("libQt6*")}
    if carried:
        fail(f"{PYSIDE_INSTALL_DIR} has a Qt of its own: {', '.join(sorted(carried))}")
    for file in elf_files(PYSIDE_INSTALL_DIR):
        folders = [file.parent, PYSIDE_INSTALL_DIR / "PySide6", PYSIDE_INSTALL_DIR / "shiboken6", vcpkg_installed / "lib"]
        set_runpath(file, list(dict.fromkeys(origin_relative(file, folder) for folder in folders)))


def finish_macos(vcpkg_installed):
    """As finish_linux(), with Mach-O's rpaths."""
    from _macho import edit, loader_relative, macho_files, needed
    generator = PYSIDE_INSTALL_DIR / "shiboken6_generator"
    libclang = next((Path(name).name for name in needed(generator / "shiboken6") if "libclang" in name), None)
    if not libclang:
        fail(f"{generator / 'shiboken6'} doesn't link libclang")
    shutil.copy2((LIBCLANG_DIR / "lib" / libclang).resolve(), generator / libclang)

    carried = {file.name for file in PYSIDE_INSTALL_DIR.rglob("libQt6*")}
    if carried:
        fail(f"{PYSIDE_INSTALL_DIR} has a Qt of its own: {', '.join(sorted(carried))}")
    for file in macho_files(PYSIDE_INSTALL_DIR):
        folders = [file.parent, PYSIDE_INSTALL_DIR / "PySide6", PYSIDE_INSTALL_DIR / "shiboken6", vcpkg_installed / "lib"]
        edit(file, new_rpaths=list(dict.fromkeys(loader_relative(file, folder) for folder in folders)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--python", help="the Python to build for (default: OpenUSD's, as found by usdcc's build)")
    parser.add_argument("--module-subset", default="Core,Gui,Widgets",
                        help="Qt modules to build PySide6 bindings for (default: Core,Gui,Widgets, what usdcc uses). "
                             "A larger subset needs a full re-run, not --reuse-build, since new modules haven't "
                             "been configured or compiled before")
    args = parser.parse_args(argv)

    vcpkg_installed = BUILD_DIR / "vcpkg_installed" / vcpkg_triplet(BUILD_DIR)

    # --- prerequisites --------------------------------------------------------

    # The second is for where pxrConfig.cmake names it as a plain variable (Linux, macOS).
    python = (args.python or cache_value(BUILD_DIR, "Python3_EXECUTABLE")
              or cache_value(BUILD_DIR, "USDCC_PYTHON_EXECUTABLE_PATH"))
    if not python:
        fail(f"No Python3_EXECUTABLE in {BUILD_DIR / 'CMakeCache.txt'}. Run tools/build.py first, or pass --python.")
    version = run([python, "-c", "import sys; print(sys.version.split()[0])"], capture_output=True, text=True)
    print(f"Building for {python} ({version.stdout.strip()})", flush=True)

    # pyside-setup builds with Ninja and cl.exe, so it needs the Visual Studio
    # developer environment when this isn't already running in one.
    if WINDOWS and not shutil.which("cl.exe"):
        enter_visual_studio_environment()

    qtpaths = find_qtpaths(vcpkg_installed)

    if not shutil.which("ninja"):
        if not WINDOWS:
            fail(f"'ninja' not found; install it ({'brew install ninja' if MACOS else 'apt install ninja-build'}).")
        print("Installing ninja (build tool required by pyside-setup)...", flush=True)
        run([python, "-m", "pip", "install", "--user", "ninja"])
        # Where pip put ninja.exe (with --user, not next to the package).
        ninja_bin = run([python, "-c", "import ninja; print(ninja.BIN_DIR)"], capture_output=True, text=True)
        os.environ["PATH"] = ninja_bin.stdout.strip() + os.pathsep + os.environ["PATH"]
    if not WINDOWS and not MACOS and not shutil.which("patchelf"):
        fail("'patchelf' not found; install it (apt install patchelf).")

    python = install_build_dependencies(python)

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
        if WINDOWS:
            run(["cmd", "/c", "mklink", "/J", metatypes_link, vcpkg_installed / "metatypes"], stdout=subprocess.DEVNULL)
        else:
            metatypes_link.symlink_to("../../metatypes")

    os.environ["LLVM_INSTALL_DIR"] = str(LIBCLANG_DIR)
    os.environ["CMAKE_PREFIX_PATH"] = str(vcpkg_installed)

    cmake = shutil.which("cmake")
    if not cmake:
        fail("'cmake' not found; is it installed and on PATH?")
    jobs = os.cpu_count() or 1

    def setup_build(stage):
        # Each run makes a build_history folder named after the time, to the second,
        # and fails if it's there: a stage with nothing left to do takes less than one.
        time.sleep(1.1)
        run([python, "setup.py", "build", f"--qtpaths={qtpaths}", f"--cmake={cmake}",
             f"--module-subset={args.module_subset}", "--limited-api=yes", "--ignore-git", f"--parallel={jobs}",
             "--skip-docs", "--disable-pyi", "--reuse-build", f"--internal-build-type={stage}",
             # uic, rcc and the like, which the dynamic triplet's tree doesn't have.
             *([] if WINDOWS else ["--no-qt-tools"])], cwd=PYSIDE_SRC_DIR)

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

    if WINDOWS:
        finish_windows(vcpkg_installed)
    elif MACOS:
        finish_macos(vcpkg_installed)
    else:
        finish_linux(vcpkg_installed)

    # The licences PySide6 and shiboken6 are under (LGPL v3, among others), which
    # tools/deploy.py ships in the deploy's licenses folder (docs/RELEASE.md section 6).
    copy_tree(PYSIDE_SRC_DIR / "LICENSES", PYSIDE_INSTALL_DIR / "LICENSES")

    print("Done. PySide6/shiboken6/shiboken6_generator built against vcpkg's Qt at:")
    print(f"  {PYSIDE_INSTALL_DIR}")
    print()
    print("Re-run CMake (tools/build.py) to build the usdcc.ui module against it.")


if __name__ == "__main__":
    main()
