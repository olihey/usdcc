"""Audit a concrete Blender-enabled deploy; never infer approval from file presence.

The reviewed dependency policy is in docs/BLENDER_DISTRIBUTION_REVIEW.md.
Original tooling is BSD-3-Clause, like usdcc.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tarfile
import tomllib
from pathlib import Path

from _common import REPO, WINDOWS, cache_value

REVIEW = REPO / "docs/BLENDER_DISTRIBUTION_REVIEW.md"
PORT_LICENSES = {
    "qtbase": "LGPL-3.0-only",
    "qtsvg": "LGPL-3.0-only",
    "qt-advanced-docking-system": "LGPL-2.1-or-later",
    "qtnodes": "BSD-3-Clause",
    "icu": "Unicode-3.0",
    "brotli": "MIT",
    "libpq": "PostgreSQL",
    "bzip2": "bzip2-1.0.6",
    "double-conversion": "BSD-3-Clause",
    "freetype": "FTL",
    "harfbuzz": "MIT",
    "libpng": "Libpng",
    "md4c": "MIT",
    "pcre2": "BSD-3-Clause",
    "zlib": "Zlib",
    "zstd": "BSD-3-Clause",
    "openssl": "Apache-2.0",
    "expat": "MIT",
    "liblzma": "public-domain AND LGPL-2.1-or-later",
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def backend_identity(backend):
    inputs = sorted(
        (Path(row["path"]).name, row["sha256"], row["license"])
        for row in backend["source_files"]
    )
    return hashlib.sha256(json.dumps(inputs).encode()).hexdigest()


def pe_imports(path):
    """Normal and delay imports, without executing the inspected binary."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 60)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE: {path}")
    sections, size = struct.unpack_from("<H12xH", data, pe + 6)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    directory = opt + (112 if magic == 0x20B else 96)
    image_base = struct.unpack_from(
        "<Q" if magic == 0x20B else "<I", data, opt + (24 if magic == 0x20B else 28)
    )[0]
    mapping = []
    for i in range(sections):
        virtual_size, rva, raw_size, raw = struct.unpack_from(
            "<IIII", data, opt + size + 40 * i + 8
        )
        mapping.append((rva, max(virtual_size, raw_size), raw))

    def offset(rva):
        for start, length, raw in mapping:
            if start <= rva < start + length:
                return raw + rva - start
        raise ValueError(f"Unmapped RVA in {path}")

    def string(rva):
        start = offset(rva)
        return data[start : data.index(b"\0", start)].decode("ascii").lower()

    imports = set()
    for index, stride, name_offset in ((1, 20, 12), (13, 32, 4)):
        rva, length = struct.unpack_from("<II", data, directory + index * 8)
        if not rva or not length:
            continue
        start = offset(rva)
        for row in range(start, start + length, stride):
            if not any(data[row : row + stride]):
                break
            name = struct.unpack_from("<I", data, row + name_offset)[0]
            if index == 13 and not struct.unpack_from("<I", data, row)[0] & 1:
                name -= image_base
            imports.add(string(name))
    return sorted(imports)


def inventory(package):
    rows = []
    for path in sorted(package.rglob("*")):
        if (
            not path.is_file()
            or path.is_symlink()
            or path.relative_to(package).parts[0] == "source"
        ):
            continue
        with path.open("rb") as stream:
            magic = stream.read(4)
        if magic[:2] == b"MZ":
            rows.append(
                {
                    "path": path.relative_to(package).as_posix(),
                    "sha256": sha256(path),
                    "imports": pe_imports(path),
                }
            )
        elif magic == b"\x7fELF":
            output = subprocess.check_output(["readelf", "-d", str(path)], text=True)
            rows.append(
                {
                    "path": path.relative_to(package).as_posix(),
                    "sha256": sha256(path),
                    "imports": sorted(re.findall(r"\(NEEDED\).*\[(.*?)\]", output)),
                }
            )
    return rows


def origins(rows, build):
    """Match shipped bytes to the actual installed inputs, not a DLL name guess."""
    usd = Path(cache_value(build, "USD_INSTALL"))
    python = Path(cache_value(build, "Python3_EXECUTABLE"))
    roots = [
        ("vcpkg", build / "vcpkg_installed"),
        ("blender", build / "plugins/blender_mesh_tools"),
        ("OpenUSD", usd / "bin"),
        ("OpenUSD", usd / "lib"),
        ("OpenUSD", usd / "plugin"),
        ("PySide6", REPO / "external/pyside-install"),
        ("CPython", python.parent),
        ("ruff", build / "ruff"),
        ("uv", build / "uv"),
        ("usdcc", build / "src/cpp"),
        ("usdcc", build / "python/usdcc"),
        ("usdcc", build / "plugins"),
    ]
    wanted = {Path(row["path"]).name.lower() for row in rows}
    matches = {}
    for component, root in roots:
        if not root.is_dir():
            continue
        for parent, directories, files in os.walk(root):
            directories[:] = [
                name
                for name in directories
                if name
                not in ("debug", ".git", "__pycache__", "site-packages", "CMakeFiles")
            ]
            # OpenUSD Python modules live under site-packages, so include those explicitly below.
            for name in files:
                if name.lower() not in wanted:
                    continue
                path = Path(parent) / name
                matches.setdefault((name.lower(), sha256(path)), (component, str(path)))
    for root in (usd / "lib/site-packages", REPO / "external/pyside-install"):
        for path in root.rglob("*"):
            if path.is_file() and path.name.lower() in wanted:
                matches.setdefault(
                    (path.name.lower(), sha256(path)),
                    (
                        "OpenUSD" if root.parts[-1] == "site-packages" else "PySide6",
                        str(path),
                    ),
                )
    for row in rows:
        component, path = matches.get(
            (Path(row["path"]).name.lower(), row["sha256"]), ("UNKNOWN", "")
        )
        if component == "usdcc" and not (
            Path(row["path"]).name.startswith(
                ("usdcc", "_ui.", "_usd.", "cpp_example.", "blender_mesh_tools.")
            )
        ):
            component = "UNKNOWN"
        row.update(component=component, input_file=path)
    return rows


def port_owners(build):
    owners = {}
    root = build / "vcpkg_installed"
    for path in (root / "vcpkg/info").glob("*.list"):
        port = path.name.split("_")[0]
        for line in path.read_text().splitlines():
            owners[str((root / line).resolve()).lower()] = port
    return owners


def check_binary_closure(rows, package, build):
    owners = port_owners(build)
    names = {Path(row["path"]).name.lower() for row in rows}
    system = set()
    for row in rows:
        require(
            row["component"] != "UNKNOWN", f"Unreviewed binary origin: {row['path']}"
        )
        name = row["path"].lower()
        require(
            not any(
                word in name
                for word in (
                    "rtx_render",
                    "vray",
                    "gatling",
                    "mdl_sdk",
                    "_debug.dll",
                    "tbbbind",
                    "tbb_preview",
                )
            ),
            f"Excluded runtime: {name}",
        )
        if row["component"] == "OpenUSD":
            basename = Path(name).name
            runtime = re.fullmatch(
                r"(?:msvcp|vcruntime|concrt)140(?:_[a-z0-9_]+)?\.dll", basename
            )
            known = basename.startswith(
                (
                    "usd_",
                    "boost_",
                    "embree4",
                    "iex-3_1",
                    "ilmthread-3_1",
                    "imath-3_1",
                    "materialx",
                    "opencolorio_2_2",
                    "openexr",
                    "openimageio",
                )
            )
            known |= basename in (
                "jpeg62.dll",
                "turbojpeg.dll",
                "tiff.dll",
                "tbb.dll",
                "tbb12.dll",  # oneTBB, which OpenUSD has been built with since deps/usd.json's --onetbb
                "tbbmalloc.dll",
                "tbbmalloc_proxy.dll",
            )
            known |= name.startswith("python/lib/site-packages/pxr/")
            known |= name.startswith("plugin/usd/") and basename in {
                p + ".dll"
                for p in (
                    "hdstorm",
                    "hdembree",
                    "hioavif",
                    "hiooiio",
                    "hioopenexr",
                    "sdrglslfx",
                    "usdshaders",
                )
            }
            require(
                runtime or known, f"USD install contains an unreviewed runtime: {name}"
            )
            if runtime:
                row["component"] = "MSVC-runtime"
                row["source_exception"] = (
                    "GPLv3 section 1 System Libraries; Microsoft redistributable runtime terms"
                )
        if row["component"] == "vcpkg":
            port = owners.get(row["input_file"].lower())
            require(port in PORT_LICENSES, f"Unreviewed runtime port: {port} ({name})")
            row["port"] = port
            row["license_choice"] = PORT_LICENSES[port]
            require(
                (package / f"licenses/vcpkg/{port}.txt").is_file(),
                f"Missing {port} notice",
            )
        for imported in row["imports"]:
            if imported in names:
                continue
            require(
                imported.startswith(("api-ms-win-", "ext-ms-win-"))
                or (
                    Path(os.environ.get("SystemRoot", "C:/Windows"))
                    / "System32"
                    / imported
                ).is_file(),
                f"Unresolved/non-system import {imported} in {name}",
            )
            system.add(imported)
    return sorted(system)


def source_matches(info, rows):
    require(
        info.get("schema") == 2, "Source bundle predates the complete review inventory"
    )
    require(
        info.get("platform") == "windows-x64" and WINDOWS,
        "This source receipt is reviewed for Windows x64 only",
    )
    require(
        info.get("resources_complete")
        and all(row["present"] for row in info["resources"]),
        "Incomplete vcpkg source resources",
    )
    expected = {row["path"]: row["sha256"] for row in info["binary_files"]}
    require(
        expected == {row["path"]: row["sha256"] for row in rows},
        "Binaries differ from the corresponding-source receipt",
    )
    require(
        info["license_policy_sha256"] == sha256(REVIEW),
        "License review changed after source collection",
    )
    for row in info["source_files"]:
        relative = Path(row["path"]).relative_to("usdcc")
        require(
            (REPO / relative).is_file() and sha256(REPO / relative) == row["sha256"],
            f"Source changed after collection: {relative}",
        )
    lock = json.loads((REPO / "deps/blender-distribution.json").read_text())
    require(
        info["supplemental_resources"] == lock["resources"],
        "Supplemental source lock differs",
    )
    reviewed = lock["reviewed_inputs"]
    require(
        sha256(REPO / "deps/blender.json") == reviewed["blender_pin_sha256"],
        "Blender pin requires renewed review",
    )
    require(
        sha256(REPO / "deps/usd.json") == reviewed["usd_pin_sha256"],
        "USD pin requires renewed review",
    )
    require(
        sorted(row["sha512"] for row in info["resources"])
        == reviewed["vcpkg_resource_sha512"],
        "vcpkg sources/patches require renewed license review",
    )
    require(
        info["pyside_commit"] == reviewed["pyside_commit"],
        "PySide source requires renewed review",
    )
    require(
        info["openusd_diff_sha256"] == reviewed["openusd_diff_sha256"],
        "OpenUSD modifications require renewed review",
    )
    commits = {row["name"]: row["commit"] for row in info["git_bundles"]}
    require(
        commits == reviewed["submodule_commits"],
        "Dependency build baseline requires renewed review",
    )
    required = {
        "boost_1_86_0",
        "embree-4.3.3",
        "glm-1.0.1",
        "libjpeg-turbo-3.1.4.1",
        "libtiff-v4.7.1",
        "MaterialX-1.39.5",
        "OpenColorIO-2.2.1",
        "openexr-3.1.13",
        "OpenImageIO-2.5.16.0",
        "OpenSubdiv-3_6_1",
    }
    require(
        required <= set(info["dependency_source_trees"]),
        "Incomplete USD dependency source trees",
    )
    require(
        {row["name"] for row in info["git_bundles"]} == {"vcpkg", "qwindowkit"},
        "Missing dependency build-system Git bundles",
    )


def check_source_archive(path, info):
    with tarfile.open(path) as archive:
        names = set(archive.getnames())
        embedded = json.load(archive.extractfile("SOURCE-INVENTORY.json"))
    require(
        embedded
        == {key: value for key, value in info.items() if key != "archive_sha256"},
        "Source archive inventory differs from sidecar",
    )
    required = {
        "BUILD-SOURCE.md",
        "DISTRIBUTION-REVIEW.md",
        "sources/OpenUSD/LICENSE.txt",
        "sources/OpenUSD/NOTICE.txt",
        "sources/pyside-setup/sources/pyside6/CMakeLists.txt",
        "inventory/blender-build-info.json",
        "inventory/app-CMakeCache.txt",
    }
    required.update(row["path"] for row in info["source_files"])
    required.update(row["archive_path"] for row in info["resources"])
    required.update(
        "sources/supplemental/" + row["file"] for row in info["supplemental_resources"]
    )
    required.update(
        "sources/git/" + row["name"] + ".bundle" for row in info["git_bundles"]
    )
    required.update(
        "sources/OpenUSD-dependencies/" + name + "/CMakeLists.txt"
        for name in (
            "embree-4.3.3",
            "MaterialX-1.39.5",
            "OpenColorIO-2.2.1",
            "openexr-3.1.13",
            "OpenImageIO-2.5.16.0",
        )
    )
    require(
        required <= names,
        "Source archive is missing required build/source members: "
        + str(sorted(required - names)[:8]),
    )


def install_notices(
    package,
    usd_source,
    info,
    build,
    *,
    supplemental=None,
    usd_src=None,
    review_path=REVIEW,
):
    from blender_mesh_package import supplemental_resources

    target = package / "licenses/OpenUSD"
    target.mkdir(parents=True, exist_ok=True)
    for name, key in (
        ("LICENSE.txt", "openusd_license_sha256"),
        ("NOTICE.txt", "openusd_notice_sha256"),
    ):
        require(
            sha256(usd_source / name) == info[key],
            f"OpenUSD {name} differs from source receipt",
        )
        shutil.copy2(usd_source / name, target / name)
    # These install-root files belong to MaterialX, not OpenUSD.
    materialx = package / "licenses/OpenUSD-dependencies/MaterialX-1.39.5"
    for name in ("LICENSE", "THIRD-PARTY.md"):
        if (target / name).exists():
            materialx.mkdir(parents=True, exist_ok=True)
            shutil.move(str(target / name), str(materialx / name))
    shutil.copy2(REPO / "LICENSE", package / "licenses/usdcc-BSD-3-Clause.txt")
    shutil.copy2(package / "licenses/GPL-3.0-or-later.txt", package / "LICENSE.txt")
    tabler = package / "licenses/Tabler-icons"
    tabler.mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO / "src/cpp/usd/resources/prim-icons/LICENSE", tabler / "LICENSE")
    shutil.copy2(review_path, package / "licenses/DISTRIBUTION-REVIEW.md")
    (package / "licenses/ATTRIBUTIONS.txt").write_text(
        "This software is based in part on the work of the Independent JPEG Group.\n"
        "Portions of this software are copyright The FreeType Project (www.freetype.org). All rights reserved.\n"
        "Prim icons: Tabler Icons, copyright 2020-2026 Pawel Kuna, MIT; see Tabler-icons/LICENSE.\n"
        "Standard USD Shader Ball: USD Working Group assets, CC BY 4.0; geometry/textures Chris Rydalch;\n"
        "specification/validation Andre Mazzone; inspiration/consultation Thomas Anagnostou.\n"
        "Source: https://github.com/usd-wg/assets, full_assets/StandardShaderBall.\n"
        "The usdcc preview wrapper selects the material surface and camera; upstream assets retain\n"
        "their README, LICENCE and font notices under resources/preview/shapes/Standard USD Shader Ball/.\n"
        "Other built-in preview geometry/HDRIs are generated by usdcc's tools/gen_preview_assets.py.\n",
        encoding="utf-8",
    )
    usd_sources = usd_src or Path(cache_value(build, "USD_INSTALL")) / "src"
    # OpenUSD's TBB sources: oneTBB (--onetbb) or TBB 2020, under their versioned folder.
    tbb = next((path.name for path in sorted(usd_sources.glob("oneTBB-*")) if path.is_dir()), "tbb")
    for relative in (
        "libjpeg-turbo-3.1.4.1/README.ijg",
        tbb + "/third-party-programs.txt",
        "embree-4.3.3/third-party-programs.txt",
    ):
        path = usd_sources / relative
        require(path.is_file(), f"Missing upstream notice: {relative}")
        output = package / "licenses/OpenUSD-dependencies" / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, output)
    records, archives = (
        supplemental if supplemental is not None else supplemental_resources()
    )
    crate_licenses = {}
    for row, (path, _) in zip(records["resources"], archives, strict=True):
        destination = (
            package / "licenses/supplemental" / (row["name"] + "-" + row["version"])
        )
        with tarfile.open(path) as archive:
            for member in archive:
                if not member.isfile():
                    continue
                relative = Path(*Path(member.name).parts[1:])
                require(
                    not relative.is_absolute() and ".." not in relative.parts,
                    "Unsafe source notice path",
                )
                name = relative.name.upper()
                if relative.as_posix() == "Cargo.toml" and row["name"].startswith(
                    "crate-"
                ):
                    metadata = tomllib.loads(
                        archive.extractfile(member).read().decode()
                    )
                    crate_licenses[row["name"] + "@" + row["version"]] = metadata[
                        "package"
                    ].get("license", metadata["package"].get("license-file"))
                if (
                    name.startswith(
                        ("LICENSE", "LICENCE", "COPYING", "NOTICE", "COPYRIGHT")
                    )
                    and member.size < 2_000_000
                ):
                    output = destination / relative
                    output.parent.mkdir(parents=True, exist_ok=True)
                    output.write_bytes(archive.extractfile(member).read())
    (package / "licenses/supplemental/crate-licenses.json").write_text(
        json.dumps(crate_licenses, indent=2) + "\n"
    )


def finalize(package, build, usd_source, source_bundle):
    # Invalidate any previous report before checking a changed package.
    report_path = package / "distribution-readiness.json"
    report_path.write_text(
        json.dumps({"ready_for_distribution": False, "review": "in progress"}) + "\n"
    )
    info_path = Path(str(source_bundle) + ".json")
    info = json.loads(info_path.read_text())
    require(
        sha256(source_bundle) == info["archive_sha256"],
        "Source archive checksum mismatch",
    )
    rows = origins(inventory(package), build)
    source_matches(info, rows)
    check_source_archive(source_bundle, info)
    system = check_binary_closure(rows, package, build)
    backend = json.loads(
        (build / "plugins/blender_mesh_tools/build-info.json").read_text()
    )
    reviewed = json.loads((REPO / "deps/blender-distribution.json").read_text())[
        "reviewed_inputs"
    ]
    require(
        backend_identity(backend) == reviewed["backend_input_sha256"],
        "Blender subset/build inputs require renewed review",
    )
    for tool, version in reviewed["tool_versions"].items():
        require(
            (build / tool / "VERSION").read_text().strip() == version,
            f"{tool} version requires renewed review",
        )
    for row in backend["source_files"]:
        require(
            sha256(row["path"]) == row["sha256"], f"Backend source drift: {row['path']}"
        )
    original = (
        REPO
        / "external/blender-work/source"
        / ("blender-" + backend["blender"]["commit"])
    )
    for row in info["blender_compiler_inputs"]:
        require(
            sha256(original / row["path"]) == row["sha256"],
            f"Blender compiler input drift: {row['path']}",
        )
    require(
        backend["features"]["eigen_mpl2_only"], "Eigen GPL/LGPL subset was not excluded"
    )
    require(
        not backend["features"]["python"]
        and not backend["features"]["openusd"]
        and not backend["features"]["tbb"],
        "Unreviewed Blender feature closure",
    )
    install_notices(package, usd_source, info, build)
    sources = package / "source"
    sources.mkdir(exist_ok=True)
    for path in (source_bundle, info_path):
        target = sources / path.name
        if target.resolve() != path.resolve():
            shutil.copy2(path, target)
    shutil.copy2(REPO / "docs/BLENDER_SOURCE_BUILD.md", sources / "BUILD-SOURCE.md")
    (package / "BLENDER-MESH-DISTRIBUTION.txt").write_text(
        "usdcc with Blender Mesh Tools — GPL-3.0-or-later combined distribution.\n"
        "Copyright and warranty notices: LICENSE.txt and licenses/.\n"
        "Original usdcc/adapter sources retain BSD-3-Clause; upstream components retain their notices.\n"
        "Complete accompanying source: source/ (archive, SHA-256 receipt and BUILD-SOURCE.md).\n"
        "Convey the entire source/ and licenses/ directories with the binaries, at no additional source charge.\n"
        "No additional EULA restricts copying, modification, replacement or reverse engineering for debugging changes.\n"
        "Qt/PySide/ADS remain replaceable dynamic libraries; see source/BUILD-SOURCE.md.\n"
        "Ruff and uv are separately licensed command-line tools aggregated with this package.\n"
        "Blender is used descriptively; no Blender Foundation or Pixar endorsement is claimed.\n"
        "See distribution-readiness.json for the exact reviewed scope and file hashes.\n",
        encoding="utf-8",
    )
    (package / "binary-inventory.json").write_text(
        json.dumps({"schema": 1, "binaries": rows, "system_imports": system}, indent=2)
        + "\n"
    )
    # Hash the whole deliverable (excluding this self-referential report).
    files = [
        {"path": path.relative_to(package).as_posix(), "sha256": sha256(path)}
        for path in sorted(package.rglob("*"))
        if path.is_file() and path != report_path
    ]
    report = {
        "schema": 2,
        "ready_for_distribution": True,
        "configuration": "GPL-3.0-or-later combined distribution",
        "platform": "windows-x64",
        "legal_distribution_review": "complete: docs/BLENDER_DISTRIBUTION_REVIEW.md",
        "corresponding_source_review": "complete: source accompanies the exact inventoried binaries",
        "proprietary_renderer_integrations": "excluded (RTX, V-Ray, Gatling/MDL SDK)",
        "source_bundle": {
            "file": "source/" + source_bundle.name,
            "sha256": info["archive_sha256"],
        },
        "review_date": "2026-11-15",
        "files": files,
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(
        f"Reviewed distribution: {package}; {len(rows)} native binaries, {len(files)} files"
    )


def verify(package):
    report = json.loads((package / "distribution-readiness.json").read_text())
    require(
        report.get("ready_for_distribution") is True,
        "Package is not reviewed for distribution",
    )
    expected = {row["path"]: row["sha256"] for row in report["files"]}
    current = {
        path.relative_to(package).as_posix(): sha256(path)
        for path in package.rglob("*")
        if path.is_file() and path != package / "distribution-readiness.json"
    }
    require(
        expected == current,
        "Package contents changed after review; regenerate and review",
    )
    print("Distribution file manifest verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--build", type=Path, default=REPO / "build")
    parser.add_argument("--finalize", action="store_true")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--usd-source", type=Path)
    parser.add_argument("--source-bundle", type=Path)
    args = parser.parse_args()
    if args.verify:
        verify(args.package.resolve())
        return
    if args.finalize:
        require(
            args.usd_source and args.source_bundle,
            "Finalization requires --usd-source and --source-bundle",
        )
        finalize(
            args.package.resolve(),
            args.build.resolve(),
            args.usd_source.resolve(),
            args.source_bundle.resolve(),
        )
        return
    rows = origins(inventory(args.package.resolve()), args.build)
    output = args.package / "binary-inventory.json"
    output.write_text(
        json.dumps({"schema": 1, "binaries": rows}, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Inventoried {len(rows)} binaries: {output}")
    for row in rows:
        if row["component"] == "UNKNOWN":
            print("Unmatched:", row["path"])


if __name__ == "__main__":
    main()
