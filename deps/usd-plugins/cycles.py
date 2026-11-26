"""The build of the cycles pack: Cycles' Hydra render delegate, hdCycles, CPU only
(docs/USD_PLUGINS.md section 7.1).

tools/build_usd_plugin.py calls build() instead of its own configure, build and
install when deps/usd-plugins/cycles.json says "hook": "cycles.py". By then the
Cycles checkout is at the pinned commit with patches/cycles-usdcc.patch applied.

1. Blender's precompiled libraries for this platform, lib/<platform> in the checkout,
   at the commit the checkout names. That repository is several GB of Git LFS
   files; only the folders in LIBRARIES are checked out (sparse) and fetched.
   Everything else Cycles needs is the OpenUSD install's: OpenImageIO, OpenVDB,
   OpenSubdiv, TBB, OpenColorIO, OpenEXR, Imath and Embree, which Cycles is built
   against, so one copy of each is loaded.
2. Cycles' CMake build, the Hydra delegate only: no standalone cycles, no GPU
   devices, no OSL, no Alembic.
3. Into the pack: plugin/usd/hdCycles.<dll|so|dylib> and its resources, and Open
   Image Denoise's base library, core and CPU device (bin/ on Windows, lib/
   elsewhere). Its GPU devices aren't taken.

Blender's library folders have no licence files: LICENCES downloads each one's at
the version Blender's lib/<platform>/deps.md names, checked by SHA-256. Returns
them as {name: folder}.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import sysconfig
import time
import urllib.error
import urllib.request
from pathlib import Path

from _common import ASSET_PLATFORM, LINUX, MACOS, VS_ARCH, VS_GENERATOR, WINDOWS, copy_file, fail, remove_tree

# The folder of Blender's precompiled libraries for each platform.
LIB_PLATFORM = {"win64": "windows_x64", "win-arm64": "windows_arm64", "linux-x64": "linux_x64",
                "macos-arm64": "macos_arm64"}
# What Cycles takes from them: Open Image Denoise (shipped), pugixml and zstd
# (linked in), epoxy (its headers; Cycles' Hydra display driver), zlib (linked in;
# Cycles hides the system's headers once it uses these libraries, and on macOS uses
# the SDK's), and on Windows pthreads (linked in). On arm64, sse2neon, a header that
# maps the SSE intrinsics Cycles uses onto NEON.
LIBRARIES = (["openimagedenoise", "pugixml", "zstd", "epoxy"] + ([] if MACOS else ["zlib"])
             + (["pthreads"] if WINDOWS else []) + (["sse2neon"] if ASSET_PLATFORM.endswith("arm64") else []))

# Their licences, at the versions lib/<platform>/deps.md names (Blender 5.2's libraries).
LICENCES = {
    "openimagedenoise": [
        ("LICENSE.txt", "https://raw.githubusercontent.com/RenderKit/oidn/v2.5.0/LICENSE.txt",
         "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"),
        ("LICENSE-third-party-programs.txt", "https://raw.githubusercontent.com/RenderKit/oidn/v2.5.0/third-party-programs.txt",
         "43905db59c86bc1de8cee0f24878d9a127c319d87c57f53f68d904043eefe98f"),
        ("LICENSE-third-party-programs-oneTBB.txt",
         "https://raw.githubusercontent.com/RenderKit/oidn/v2.5.0/third-party-programs-oneTBB.txt",
         "1634857eb058dce5278c57a832b8974190785145f4be9acdbdb511f3a0c4091c"),
    ],
    "pugixml": [("LICENSE.md", "https://raw.githubusercontent.com/zeux/pugixml/v1.10/LICENSE.md",
                 "9ddc2f8e3003601aff5a4d37f3caf3b6b60a729ebbc466bafd965cdf0883b12c")],
    "zstd": [("LICENSE", "https://raw.githubusercontent.com/facebook/zstd/v1.5.7/LICENSE",
              "7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8")],
    "epoxy": [("COPYING", "https://raw.githubusercontent.com/anholt/libepoxy/1.5.10/COPYING",
               "8d5144666f9c4df9bbd69b8900086d5979259152a1060421cdcc0fb9061a1c12")],
}
if "sse2neon" in LIBRARIES:
    LICENCES["sse2neon"] = [
        ("LICENSE", "https://raw.githubusercontent.com/DLTcollab/sse2neon/227cc413fb2d50b2a10073087be96b59d5364aea/LICENSE",
         "5c347d431a555ec6e5bc9720ceba2119570357dab5203372d469be9bcce374cc")]
if "zlib" in LIBRARIES:
    LICENCES["zlib"] = [("LICENSE", "https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE",
                         "845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243")]
if WINDOWS:
    # pthreads4w 3.0.0; this mirror has no tags, so a commit.
    LICENCES["pthreads"] = [
        ("LICENSE", "https://raw.githubusercontent.com/fwbuilder/pthreads4w/44daa2441137b90477b449663abe9755b2c9a16b/LICENSE",
         "58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd"),
        ("NOTICE", "https://raw.githubusercontent.com/fwbuilder/pthreads4w/44daa2441137b90477b449663abe9755b2c9a16b/NOTICE",
         "ab620b4fe865583cca883b258f728f7f2234802f7c1b8d74fdb6a4dd2642ae2c"),
    ]

# Open Image Denoise's libraries the pack takes: not its CUDA, HIP and SYCL devices.
OIDN_PARTS = ("OpenImageDenoise", "OpenImageDenoise_core", "OpenImageDenoise_device_cpu")


def _git(*arguments, cwd, env=None, check=True):
    print("git", " ".join(arguments), flush=True)
    result = subprocess.run(["git", *arguments], cwd=cwd, env=env)
    if check and result.returncode != 0:
        fail(f"git {' '.join(arguments)} failed ({result.returncode})")
    return result.returncode


def _libraries(source, work):
    """Blender's libraries for this platform, the folders in LIBRARIES only, in the
    checkout's lib/<platform>, where Cycles' CMake looks. Returns that folder."""
    platform = LIB_PLATFORM.get(ASSET_PLATFORM) or fail(f"No Blender libraries for {ASSET_PLATFORM}")
    path = f"lib/{platform}"
    entry = subprocess.run(["git", "ls-tree", "HEAD", path], cwd=source, capture_output=True, text=True).stdout.split()
    if len(entry) < 3 or entry[1] != "commit":
        fail(f"The Cycles checkout names no commit for {path}")
    commit = entry[2]
    url = subprocess.run(["git", "config", "-f", ".gitmodules", f"submodule.{path}.url"], cwd=source,
                         capture_output=True, text=True).stdout.strip() or fail(f"No URL for {path} in .gitmodules")
    folder = source / path
    marker = work / f"lib-{platform}.txt"
    wanted = f"{commit} {' '.join(LIBRARIES)}\n"
    if marker.is_file() and marker.read_text(encoding="ascii") == wanted and all((folder / name).is_dir() for name in LIBRARIES):
        print(f"Blender's libraries: {path} at {commit[:10]}, already here", flush=True)
        return folder
    if folder.exists():
        remove_tree(folder)
    folder.mkdir(parents=True)
    _git("init", "-q", cwd=folder)
    _git("remote", "add", "origin", url, cwd=folder)
    _git("lfs", "install", "--local", "--skip-smudge", cwd=folder)
    _git("sparse-checkout", "set", "--no-cone", *[f"/{name}/" for name in LIBRARIES], cwd=folder)
    # Without the blobs of the folders left out, where the server can do that.
    # projects.blender.org has refused some runners now and then (403), and served them
    # on the next run: a few tries, a minute apart.
    for attempt in range(4):
        if attempt:
            print(f"Fetching {path} again in a minute (try {attempt + 1} of 4)", flush=True)
            time.sleep(60)
        if not _git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit, cwd=folder, check=False):
            break
    else:
        if _git("fetch", "-q", "--depth", "1", "origin", commit, cwd=folder, check=False):
            # What the server said, for a refusal (a 403 from some runners).
            probe = f"{url}/info/refs?service=git-upload-pack"
            try:
                urllib.request.urlopen(urllib.request.Request(probe, headers={"User-Agent": "git/2.47"}), timeout=60)
                print(f"{probe}: answers", flush=True)
            except urllib.error.HTTPError as error:
                print(f"{probe}: {error.code} {error.reason}\n{error.headers}\n{error.read(2000).decode('utf-8', 'replace')}",
                      flush=True)
            except OSError as error:
                print(f"{probe}: {error}", flush=True)
            fail(f"Fetching {path} from {url} failed")
    _git("checkout", "-q", "FETCH_HEAD", cwd=folder)
    _git("lfs", "pull", "--include", ",".join(f"{name}/**" for name in LIBRARIES), cwd=folder)
    for name in LIBRARIES:
        if not (folder / name).is_dir():
            fail(f"{path} at {commit} has no {name}")
    marker.write_text(wanted, encoding="ascii")
    return folder


def _licences(work):
    """The licence files LICENCES names, downloaded and checked; {name: folder}."""
    import hashlib
    folders = {}
    for name, files in LICENCES.items():
        folder = work / "licences" / name
        folder.mkdir(parents=True, exist_ok=True)
        for file_name, url, sha256 in files:
            target = folder / file_name
            if not (target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == sha256):
                print(f"Downloading {url}", flush=True)
                try:
                    with urllib.request.urlopen(url, timeout=120) as response:
                        data = response.read()
                except OSError as error:
                    fail(f"Downloading {url} failed: {error}")
                if hashlib.sha256(data).hexdigest() != sha256:
                    fail(f"{url} isn't the file cycles.py names (SHA-256 {hashlib.sha256(data).hexdigest()})")
                target.write_bytes(data)
        folders[f"blender-libs-{name}"] = folder
    return folders


def _python(work, python_root):
    """CMake arguments for the pin's Python, which this runs on."""
    if WINDOWS:
        # Cycles expects Blender's layout there: <root>/313/{include,libs}.
        root = work / "python"
        version = f"{sys.version_info.major}{sys.version_info.minor}"
        for folder in ("include", "libs"):
            if (root / version / folder).exists():
                remove_tree(root / version / folder)
            shutil.copytree(Path(python_root) / folder, root / version / folder)
        return [f"-DPYTHON_ROOT_DIR={root.as_posix()}"]
    library = Path(sysconfig.get_config_var("LIBDIR")) / sysconfig.get_config_var("LDLIBRARY")
    return [f"-DPYTHON_ROOT_DIR={python_root}", f"-DPYTHON_INCLUDE_DIR={sysconfig.get_paths()['include']}",
            f"-DPYTHON_LIBRARY={library.as_posix()}", f"-DPYTHON_LIBPATH={library.parent.as_posix()}"]


def build(ctx):
    run, work, usd, source, prefix = ctx.run, ctx.work, ctx.usd, ctx.source, ctx.prefix
    libraries = _libraries(source, work)
    licences = _licences(work)

    embree = next((file for pattern in ("embree4.lib", "libembree4.so", "libembree4.dylib")
                   for file in (usd / "lib").glob(pattern)), None) or fail(f"No Embree 4 in {usd / 'lib'}")
    # OpenVDB's headers include Boost's, which Cycles doesn't put on the include path. On
    # Windows OpenUSD has them in include/boost-<version>; Cycles sets CMAKE_CXX_FLAGS
    # itself, so they go to the compiler through MSVC's CL variable. Elsewhere they are
    # in include/, which is on the path already.
    boost = sorted((usd / "include").glob("boost-*"))
    env = dict(os.environ)
    if WINDOWS and boost:
        env["CL"] = f"/I{boost[-1]} " + env.get("CL", "")
    if LINUX and shutil.which("g++-14"):
        # Cycles is C++20, and GCC 13 refuses a constructor in OpenUSD's
        # retainedDataSource.h there, which GCC 14 only warns about.
        env.update(CC=shutil.which("gcc-14"), CXX=shutil.which("g++-14"))
    build_dir, install = work / "build", work / "install"
    command = ["cmake", "-S", source, "-B", build_dir]
    command += ["-G", VS_GENERATOR, "-A", VS_ARCH] if WINDOWS else ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
    command += [
        f"-DPXR_ROOT={usd.as_posix()}",
        f"-DCMAKE_INSTALL_PREFIX={install.as_posix()}",
        f"-DOPENEXR_ROOT_DIR={usd.as_posix()}", f"-DIMATH_ROOT_DIR={usd.as_posix()}",
        f"-DEMBREE_ROOT_DIR={usd.as_posix()}", f"-DEMBREE_INCLUDE_DIR={(usd / 'include').as_posix()}",
        f"-DEMBREE_EMBREE4_LIBRARY={embree.as_posix()}",
        "-DWITH_CYCLES_EMBREE=ON", "-DWITH_CYCLES_HYDRA_RENDER_DELEGATE=ON", "-DWITH_CYCLES_USD=OFF",
        "-DWITH_CYCLES_STANDALONE=OFF", "-DWITH_CYCLES_STANDALONE_GUI=OFF",
        "-DWITH_CYCLES_DEVICE_CUDA=OFF", "-DWITH_CYCLES_DEVICE_OPTIX=OFF", "-DWITH_CYCLES_DEVICE_HIP=OFF",
        "-DWITH_CYCLES_DEVICE_HIPRT=OFF", "-DWITH_CYCLES_DEVICE_ONEAPI=OFF", "-DWITH_CYCLES_DEVICE_METAL=OFF",
        "-DWITH_CYCLES_OSL=OFF", "-DWITH_CYCLES_ALEMBIC=OFF", "-DWITH_CYCLES_OPENIMAGEDENOISE=ON",
        *_python(work, ctx.python_root),
    ]
    if MACOS:
        command += ["-DCMAKE_OSX_ARCHITECTURES=arm64"]
    print("Configuring Cycles:", " ".join(str(part) for part in command), flush=True)
    run(command, env=env)
    run(["cmake", "--build", build_dir, "--config", "Release", "--parallel", str(ctx.jobs or os.cpu_count() or 4)], env=env)
    if install.exists():
        remove_tree(install)
    run(["cmake", "--install", build_dir, "--config", "Release"])

    # The delegate, where OpenUSD's plugin/usd/plugInfo.json finds it (*/resources/).
    extension = ".dll" if WINDOWS else ".dylib" if MACOS else ".so"
    library = next((file for file in (install / "hydra").glob(f"*hdCycles{extension}")), None) \
        or fail(f"Cycles' install has no hdCycles{extension} in {install / 'hydra'}")
    resources = install / "hydra" / "hdCycles" / "resources"
    if not (resources / "plugInfo.json").is_file():
        fail(f"Cycles' install has no {resources / 'plugInfo.json'}")
    plugins = prefix / "plugin" / "usd"
    plugins.mkdir(parents=True, exist_ok=True)
    copy_file(library, plugins)
    shutil.copytree(resources, plugins / "hdCycles" / "resources")

    # Open Image Denoise, beside the libraries it's found with.
    oidn = libraries / "openimagedenoise" / ("bin" if WINDOWS else "lib")
    target = prefix / ("bin" if WINDOWS else "lib")
    taken = []
    for file in sorted(oidn.iterdir()):
        stem = file.name.split(".")[0]
        if stem.startswith("lib"):
            stem = stem[3:]
        if stem in OIDN_PARTS and (file.suffix == ".dll" if WINDOWS else ".so" in file.name or ".dylib" in file.name):
            target.mkdir(parents=True, exist_ok=True)
            if file.is_symlink():
                os.symlink(os.readlink(file), target / file.name)
            else:
                copy_file(file, target)
            taken.append(file.name)
    if not any(name.split(".")[0].removeprefix("lib") == "OpenImageDenoise_device_cpu" for name in taken):
        fail(f"No Open Image Denoise CPU device in {oidn}")
    print(f"Open Image Denoise: {', '.join(taken)}", flush=True)

    # Cycles' own notices of the code it adapted (src/doc/license), with its LICENSE,
    # which tools/build_usd_plugin.py takes from the top of the checkout.
    notices = prefix / "licenses" / "usd-plugins" / "cycles" / "cycles-notices"
    shutil.copytree(source / "src" / "doc" / "license", notices, ignore=shutil.ignore_patterns("CMakeLists.txt"))

    if LINUX:
        # The delegate finds OpenUSD's libraries and Open Image Denoise in lib/;
        # Open Image Denoise finds its modules beside itself.
        from _elf import elf_files, set_runpath
        set_runpath(plugins / library.name, ["$ORIGIN/../../lib"])
        for file in elf_files(target):
            if file.name.split(".")[0].removeprefix("lib") in OIDN_PARTS:
                set_runpath(file, ["$ORIGIN"])
    return licences
