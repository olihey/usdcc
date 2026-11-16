"""Build the optional native Blender mesh backend, independently of usdcc.

    python tools/build_blender_mesh.py --fetch

The first explicit build also installs its private vcpkg dependencies. Normal
usdcc configuration never calls this script or downloads Blender.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def run(command):
    print("+ " + subprocess.list2cmdline([str(arg) for arg in command]), flush=True)
    subprocess.run([str(arg) for arg in command], check=True)


def source_tree(spec, work, fetch):
    archive = work / "downloads" / f"blender-{spec['commit']}.tar.gz"
    if not archive.is_file():
        if not fetch:
            raise RuntimeError("Source archive missing: run again with --fetch")
        archive.parent.mkdir(parents=True, exist_ok=True)
        partial = archive.with_suffix(".partial")
        print(f"Downloading {spec['archive']}", flush=True)
        with (
            urllib.request.urlopen(spec["archive"], timeout=120) as response,
            partial.open("wb") as output,
        ):
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
        with partial.open("rb") as stream:
            downloaded = hashlib.file_digest(stream, "sha256").hexdigest()
        if downloaded != spec["sha256"]:
            raise RuntimeError(
                f"Downloaded source checksum mismatch: {partial}: {downloaded}"
            )
        partial.replace(archive)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != spec["sha256"]:
        raise RuntimeError(f"Source checksum mismatch: {archive}: {digest}")
    source = work / "source" / f"blender-{spec['commit']}"
    stamp = source / ".usdcc-verified-source.json"
    if not stamp.is_file():
        source.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as package:
            expected = source.name + "/"
            for member in package.getmembers():
                if member.name != source.name and not member.name.startswith(expected):
                    raise RuntimeError(f"Unexpected source archive path: {member.name}")
            package.extractall(source.parent, filter="data")
        stamp.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
    elif json.loads(stamp.read_text(encoding="utf-8")) != spec:
        raise RuntimeError(
            "Source stamp differs from deps/blender.json; use a fresh --work directory"
        )
    return source


def collect_inventory(spec, source, build, install, configuration):
    """Record the actual build inputs and upstream notices for local review."""
    backend = REPO / "src/plugins/blender_mesh_tools/backend"
    licenses = install / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(
        REPO / "src/plugins/blender_mesh_tools/licenses/README.md",
        licenses / "README.md",
    )
    shutil.copy2(REPO / "LICENSE", licenses / "usdcc-BSD-3-Clause.txt")
    shutil.copytree(
        source / "release/license", licenses / "blender", dirs_exist_ok=True
    )
    shutil.copy2(source / "COPYING", licenses / "blender/COPYING")
    records = []
    inputs = (build / "backend-inputs.txt").read_text(encoding="utf-8").splitlines()
    inputs += [
        "probe_api.cpp",
        "probe.cpp",
        "probe_api.h",
        "mesh_api.cpp",
        "mesh_api.h",
        "worker.h",
        "CMakeLists.txt",
        "extract_sources.py",
        "sources.cmake",
        "vcpkg.json",
    ]
    for name in inputs:
        path = Path(name)
        if not path.is_absolute():
            path = backend / path
        data = path.read_bytes()
        identifier = re.search(rb"SPDX-License-Identifier:\s*([^\r\n*]+)", data)
        records.append(
            {
                "path": str(path.resolve()),
                "sha256": hashlib.sha256(data).hexdigest(),
                "license": identifier[1].decode().strip()
                if identifier
                else "see upstream/generated notices",
            }
        )
    dependencies = []
    for share in (build / "vcpkg_installed").glob("*/share"):
        for package in ["eigen3", "fmt", "pthreads"]:
            folder = share / package
            if not folder.is_dir():
                continue
            destination = licenses / package
            destination.mkdir(exist_ok=True)
            for filename in ["copyright", "vcpkg.spdx.json", "vcpkg_abi_info.txt"]:
                if (folder / filename).is_file():
                    shutil.copy2(folder / filename, destination / filename)
            document = json.loads(
                (folder / "vcpkg.spdx.json").read_text(encoding="utf-8")
            )
            dependencies.append(
                {
                    "package": package,
                    "build": document["name"],
                    "spdx": f"licenses/{package}/vcpkg.spdx.json",
                }
            )
    # Preserve notices accompanying vendored support code in addition to Blender's
    # overall license collection.
    for name in ["xxhash", "wcwidth"]:
        folder = source / "extern" / name
        for path in folder.iterdir():
            if path.is_file() and (
                "license" in path.name.lower() or path.name.startswith("README")
            ):
                destination = licenses / name
                destination.mkdir(exist_ok=True)
                shutil.copy2(path, destination / path.name)
    generated = install / "source-excerpts"
    shutil.copytree(build / "generated", generated, dirs_exist_ok=True)
    shutil.copy2(backend / "extract_sources.py", generated / "extract_sources.py")
    shutil.copy2(backend / "sources.cmake", generated / "sources.cmake")
    info = {
        "schema": 1,
        "mesh_abi": 1,
        "blender": spec,
        "configuration": configuration,
        "toolchain": json.loads(
            (build / "build-toolchain.json").read_text(encoding="utf-8")
        ),
        "platform": platform.platform(),
        "python_build_driver": sys.version,
        "source_files": records,
        "dependencies": dependencies,
        "features": {
            "python": False,
            "openusd": False,
            "tbb": False,
            "eigen_mpl2_only": True,
        },
        "distribution": "backend inventory only; combined artifacts require tools/blender_distribution.py review",
    }
    (install / "build-info.json").write_text(
        json.dumps(info, indent=2) + "\n", encoding="utf-8"
    )
    shutil.copy2(build / "CMakeCache.txt", install / "build-cache.txt")


def main(argv=None):
    if sys.version_info < (3, 12):
        raise RuntimeError(
            "The Blender mesh build driver requires Python 3.12 or later"
        )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fetch",
        action="store_true",
        help="allow downloading the pinned source archive",
    )
    parser.add_argument(
        "--fetch-only",
        action="store_true",
        help="verify/extract source without building",
    )
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--work", type=Path, default=REPO / "external/blender-work")
    parser.add_argument(
        "--install", type=Path, default=REPO / "external/blender-mesh-install"
    )
    parser.add_argument(
        "--configuration",
        choices=["Release", "RelWithDebInfo"],
        default="RelWithDebInfo",
    )
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    spec = json.loads((REPO / "deps/blender.json").read_text(encoding="utf-8"))
    if spec.get("patches"):
        raise RuntimeError("This driver does not yet support source patches")
    work = args.work.resolve()
    source = source_tree(spec, work, args.fetch)
    print(f"Verified Blender {spec['version']} at {source}", flush=True)
    if args.fetch_only:
        return
    build = work / "build"
    run(
        [
            sys.executable,
            REPO / "src/plugins/blender_mesh_tools/backend/extract_sources.py",
            source,
            build / "generated",
        ]
    )
    command = [
        "cmake",
        "-S",
        REPO / "src/plugins/blender_mesh_tools/backend",
        "-B",
        build,
        f"-DBLENDER_SOURCE_DIR={source.as_posix()}",
        f"-DCMAKE_TOOLCHAIN_FILE={(REPO / 'external/vcpkg/scripts/buildsystems/vcpkg.cmake').as_posix()}",
        f"-DCMAKE_INSTALL_PREFIX={args.install.resolve().as_posix()}",
        f"-DCMAKE_BUILD_TYPE={args.configuration}",
    ]
    if sys.platform == "win32":
        command += ["-G", "Visual Studio 17 2022", "-A", "x64"]
    else:
        command += ["-DVCPKG_TARGET_TRIPLET=x64-linux-dynamic"]
    run(command)
    if args.configure_only:
        return
    run(
        [
            "cmake",
            "--build",
            build,
            "--config",
            args.configuration,
            "--parallel",
            args.jobs,
        ]
    )
    run(["ctest", "--test-dir", build, "-C", args.configuration, "--output-on-failure"])
    install = args.install.resolve()
    run(["cmake", "--install", build, "--config", args.configuration])
    collect_inventory(spec, source, build, install, args.configuration)
    print(
        f"Backend and development license inventory installed in {install}", flush=True
    )


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Blender mesh build failed: {error}", file=sys.stderr)
        sys.exit(1)
