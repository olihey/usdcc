"""Review and package the Linux Blender-enabled distribution (BSD-3-Clause tooling)."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request
from collections import Counter
from pathlib import Path

from _common import REPO, cache_value
from _elf import elf_files
from blender_distribution import (
    GPL_PACKS,
    PORT_LICENSES,
    check_gpl_packs,
    gpl_pack_inputs,
    port_owners,
    require,
    sha256,
)

from deploy import _ldd

REVIEW = REPO / "docs/BLENDER_LINUX_DISTRIBUTION_REVIEW.md"
LOCK = REPO / "deps/blender-linux-distribution.json"
PORT_LICENSES = {
    **PORT_LICENSES,
    "dbus": "GPL-2.0-or-later (GPLv3 selected)",
    "fontconfig": "MIT",
    "libb2": "CC0-1.0",
    "libcap": "BSD-3-Clause",
    "libjpeg-turbo": "IJG AND BSD-3-Clause AND Zlib",
    "libmount": "LGPL-2.1-or-later (libmount/libblkid)",
    "libsystemd": "LGPL-2.1-or-later",
    "libxcrypt": "LGPL-2.1-or-later",
    "lz4": "BSD-2-Clause",
    "sqlite3": "public-domain",
}
CORE_SYSTEM_SOURCES = {"gcc-14", "glibc"}


def command(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def elf_identity(path):
    """Build ID plus loadable code/data sections unaffected by RUNPATH relocation."""
    data = path.read_bytes()
    require(data[:6] == b"\x7fELF\x02\x01", f"Unsupported ELF class/endianness: {path}")
    require(struct.unpack_from("<H", data, 18)[0] == 62, f"ELF is not x86-64: {path}")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    headers = [
        struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * size) for i in range(count)
    ]
    string_header = headers[names_index]
    names = data[string_header[4] : string_header[4] + string_header[5]]
    selected = []
    build_id = None
    for header in headers:
        start = header[0]
        name = names[start : names.index(b"\0", start)].decode()
        payload = data[header[4] : header[4] + header[5]] if header[1] != 8 else b""
        if name == ".note.gnu.build-id":
            build_id = payload.hex()
        # patchelf moves some ELF metadata, but never code, constants or static data.
        if (
            header[2] & 2
            and name not in (".dynamic", ".dynstr", ".dynsym", ".gnu.hash", ".hash")
            and not name.startswith(".note")
        ):
            selected.append(
                (name, header[1], header[5], hashlib.sha256(payload).hexdigest())
            )
    require(build_id and selected, f"Missing ELF provenance sections: {path}")
    return hashlib.sha256(json.dumps(sorted(selected)).encode()).hexdigest(), build_id


def dpkg_owner(path):
    candidates = [str(path), str(path.resolve())]
    if str(path).startswith("/usr/"):
        candidates.append(str(path)[4:])
    for candidate in candidates:
        result = subprocess.run(
            ["dpkg-query", "-S", candidate], capture_output=True, text=True, check=False
        )
        if result.returncode == 0:
            package = result.stdout.split(": ", 1)[0].split(",", 1)[0]
            text = command(
                "dpkg-query",
                "-W",
                "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}",
                package,
            )
            binary, version, source, source_version = text.split("\t")
            return {
                "binary_package": binary,
                "version": version,
                "source_package": source,
                "source_version": source_version,
            }
    return None


def inventory(package, build):
    usd = Path(cache_value(build, "USD_INSTALL"))
    roots = [
        ("vcpkg", build / "vcpkg_installed"),
        ("blender", build / "plugins/blender_mesh_tools"),
        ("OpenUSD", usd / "lib"),
        ("OpenUSD", usd / "plugin"),
        ("usdcc", build / "src/cpp"),
        ("usdcc", build / "python/usdcc"),
        ("usdcc", build / "plugins"),
        ("ruff", build / "ruff"),
        ("uv", build / "uv"),
    ]
    paths = elf_files(package)
    wanted = {path.name for path in paths}
    matches = {}
    # Before OpenUSD's folders, which the packs were unzipped into.
    for component, path in gpl_pack_inputs(usd):
        if path.name in wanted and path.read_bytes()[:4] == b"\x7fELF":
            matches.setdefault((path.name, elf_identity(path)), (component, str(path)))
    for component, root in roots:
        if not root.exists():
            continue
        for parent, dirs, files in os.walk(root):
            dirs[:] = [
                name
                for name in dirs
                if name not in (".git", "CMakeFiles", "debug", "__pycache__")
            ]
            for name in files:
                if name not in wanted:
                    continue
                path = Path(parent) / name
                if path.read_bytes()[:4] != b"\x7fELF":
                    continue
                matches.setdefault((name, elf_identity(path)), (component, str(path)))
    records = []
    for path in paths:
        identity = elf_identity(path)
        component, original = matches.get((path.name, identity), ("UNKNOWN", ""))
        distro = None
        if component == "UNKNOWN":
            candidates = [
                Path("/usr/bin") / path.name,
                Path("/usr/lib/x86_64-linux-gnu") / path.name,
            ]
            candidates += list(Path("/usr/lib/python3.12/lib-dynload").glob(path.name))
            for candidate in candidates:
                if (
                    candidate.is_file()
                    and candidate.read_bytes()[:4] == b"\x7fELF"
                    and elf_identity(candidate) == identity
                ):
                    distro = dpkg_owner(candidate)
                    if distro:
                        component, original = "Ubuntu", str(candidate)
                        break
        dynamic = command("readelf", "-d", path)
        records.append(
            {
                "path": path.relative_to(package).as_posix(),
                "sha256": sha256(path),
                "identity": identity,
                "component": component,
                "input_file": original,
                "ubuntu": distro,
                "imports": sorted(re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)),
            }
        )
    return records


def system_libraries(package, rows):
    found = {}
    clean = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}
    for row in rows:
        for name, resolved in _ldd(package / row["path"], clean).items():
            require(
                resolved is not None, f"Unresolved ELF dependency {name}: {row['path']}"
            )
            path = Path(resolved).resolve()
            if path.is_relative_to(package):
                continue
            require(
                str(path).startswith(("/usr/lib/", "/lib/")),
                f"Build path leaked into runtime closure: {path}",
            )
            if name not in found:
                owner = dpkg_owner(path)
                require(owner is not None, f"Unknown system runtime origin: {path}")
                found[name] = {
                    "soname": name,
                    "file": str(path),
                    "sha256": sha256(path),
                    "ubuntu": owner,
                }
    return sorted(found.values(), key=lambda row: row["soname"])


def validate_rows(rows, system, package, build):
    owners = port_owners(build)
    allowed_plugins = {
        "hdEmbree.so",
        "hdStorm.so",
        "hioAvif.so",
        "hioOiio.so",
        "hioOpenEXR.so",
        "sdrGlslfx.so",
        "usdShaders.so",
    }
    for row in rows:
        path, component = row["path"], row["component"]
        require(component != "UNKNOWN", "Unknown ELF provenance: " + path)
        require(
            not any(
                word in path.lower()
                for word in (
                    "rtx_render",
                    "vray",
                    "_debug.so",
                    "tbbbind",
                    "tbb_preview",
                )
            ),
            "Unreviewed/excluded runtime: " + path,
        )
        if component == "vcpkg":
            port = owners.get(row["input_file"].lower())
            require(port in PORT_LICENSES, f"Unreviewed runtime port {port}: {path}")
            row.update(port=port, license_choice=PORT_LICENSES[port])
            require(
                (package / "licenses/vcpkg" / (port + ".txt")).is_file(),
                "Missing runtime license: " + port,
            )
        elif component == "OpenUSD":
            allowed = path.startswith("lib/python3.12/site-packages/pxr/")
            allowed |= (
                path.startswith("plugin/usd/") and Path(path).name in allowed_plugins
            )
            allowed |= path.startswith("lib/") and Path(path).name.startswith(
                (
                    "libusd_",
                    "libboost_",
                    "libosd",
                    "libIex-",
                    "libIlmThread-",
                    "libImath-",
                    "libMaterialX",
                    "libOpenColorIO.",
                    "libOpenEXR",
                    "libOpenImageIO",
                    "libembree4.",
                    "libpng16.",
                    "libtbb.",
                    "libtbbmalloc.",
                    "libtbbmalloc_proxy.",
                    "libtiff.",
                    "libtiffxx.",
                    "libturbojpeg.",
                )
            )
            require(allowed, "Unreviewed USD runtime: " + path)
        elif component == "usdcc":
            require(
                Path(path).name.startswith(
                    ("usdcc", "libusdcc_", "_usd.", "cpp_example.")
                ),
                "Unreviewed app runtime: " + path,
            )
        elif component == "blender":
            require(
                Path(path).name.startswith(
                    ("blender_mesh_tools.", "libusdcc_blender_mesh_backend.", "libfmt.")
                ),
                "Unreviewed plugin runtime: " + path,
            )
        elif component in {about["component"] for about in GPL_PACKS.values()}:
            licences = next(about["binaries"] for about in GPL_PACKS.values()
                            if about["component"] == component)
            require(path.lower() in licences, f"Unreviewed {component} runtime: " + path)
            row["license_choice"] = licences[path.lower()]
        elif component == "Ubuntu":
            require(
                row["ubuntu"]["source_package"]
                in {"python3.12", "python3-stdlib-extensions"},
                "Unreviewed bundled Ubuntu library: " + path,
            )
        else:
            require(component in ("ruff", "uv"), "Unreviewed component: " + component)
    for row in system:
        require(row["ubuntu"] is not None, "Unknown system library")
    return rows


def file_manifest(package):
    result = []
    for path in sorted(package.rglob("*")):
        if path == package / "distribution-readiness.json":
            continue
        relative = path.relative_to(package).as_posix()
        if path.is_symlink():
            require(
                path.resolve().is_relative_to(package) and path.exists(),
                "Escaping or broken package symlink: " + relative,
            )
            result.append(
                {"path": relative, "type": "symlink", "target": os.readlink(path)}
            )
        elif path.is_file():
            result.append(
                {
                    "path": relative,
                    "type": "file",
                    "sha256": sha256(path),
                    "mode": path.stat().st_mode & 0o777,
                }
            )
    return result


def verify(package):
    report = json.loads((package / "distribution-readiness.json").read_text())
    require(
        report.get("ready_for_distribution") is True
        and report["platform"] == "linux-x86_64",
        "Linux package has not passed distribution review",
    )
    require(
        report["files"] == file_manifest(package),
        "Package content, permissions or symlinks changed after review",
    )
    print("Linux distribution file/symlink manifest verified")


def finalize(package, work, source_bundle):
    from blender_distribution import install_notices
    from blender_linux_source_package import evidence, tool_sources

    report_path = package / "distribution-readiness.json"
    report_path.write_text(
        json.dumps({"ready_for_distribution": False, "review": "in progress"}) + "\n"
    )
    info = json.loads(Path(str(source_bundle) + ".json").read_text())
    require(
        info.get("schema") == 3 and info.get("platform") == "linux-x86_64",
        "A Linux source receipt is required; Windows receipts cannot approve Linux",
    )
    require(
        sha256(source_bundle) == info["archive_sha256"],
        "Source archive checksum mismatch",
    )
    require(
        sha256(REVIEW) == info["license_policy_sha256"]
        and sha256(LOCK) == info["review_lock_sha256"],
        "Review policy/lock changed after source collection",
    )
    rows = inventory(package, REPO / "build")
    system = system_libraries(package, rows)
    validate_rows(rows, system, package, REPO / "build")
    require(
        info["binary_files"]
        == [{"path": row["path"], "sha256": row["sha256"]} for row in rows],
        "Runtime binaries differ from source receipt",
    )
    current = evidence(rows, system, work)
    require(
        current
        == info["reviewed_inputs"]
        == json.loads(LOCK.read_text())["reviewed_inputs"],
        "Linux dependency/source inputs require renewed review",
    )
    for row in info["source_files"]:
        path = REPO / Path(row["path"]).relative_to("usdcc")
        require(
            path.is_file() and sha256(path) == row["sha256"],
            "Source changed after collection: " + str(path),
        )
    with tarfile.open(source_bundle) as archive:
        names = set(archive.getnames())
        embedded = json.load(archive.extractfile("SOURCE-INVENTORY.json"))
    require(
        embedded
        == {key: value for key, value in info.items() if key != "archive_sha256"},
        "Embedded source inventory disagrees with sidecar",
    )
    required = {
        "BUILD-SOURCE.md",
        "DISTRIBUTION-REVIEW.md",
        "sources/OpenUSD/LICENSE.txt",
        "sources/OpenUSD/NOTICE.txt",
        "inventory/app-CMakeCache.txt",
        "inventory/blender-CMakeCache.txt",
    }
    required.update(row["path"] for row in info["source_files"])
    required.update(row["archive_path"] for row in info["resources"])
    required.update(
        "sources/supplemental/" + row["file"] for row in info["supplemental_resources"]
    )
    required.update(
        "sources/ubuntu/" + row["folder"] + "/" + item["file"]
        for row in info["ubuntu_sources"]
        for item in row["files"]
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
        "Incomplete source archive: " + str(sorted(required - names)[:8]),
    )
    tools = tool_sources()
    packs = check_gpl_packs(package, json.loads(LOCK.read_text()).get("usd_plugin_packs", {}), tools)
    supplemental = (
        {"resources": tools},
        [
            (
                REPO / "external/blender-work/package-downloads" / row["file"],
                "sources/supplemental/" + row["file"],
            )
            for row in tools
        ],
    )
    install_notices(
        package,
        work / "OpenUSD",
        info,
        REPO / "build",
        supplemental=supplemental,
        usd_src=work / "src",
        review_path=REVIEW,
    )
    shutil.copy2(
        REPO / "docs/BLENDER_DISTRIBUTION_REVIEW.md",
        package / "licenses/BLENDER_DISTRIBUTION_REVIEW.md",
    )
    ubuntu_notices = package / "licenses/Ubuntu"
    ubuntu_notices.mkdir(exist_ok=True)
    unique = {
        row["ubuntu"]["binary_package"]: row["ubuntu"]
        for row in rows + system
        if row.get("ubuntu")
    }
    for name in sorted(unique):
        original = Path("/usr/share/doc") / name.split(":")[0] / "copyright"
        require(original.is_file(), "Missing Ubuntu copyright file: " + name)
        shutil.copy2(original, ubuntu_notices / (name.replace(":", "_") + ".txt"))
    shutil.copytree(
        "/usr/share/common-licenses",
        ubuntu_notices / "common-licenses",
        dirs_exist_ok=True,
    )
    (ubuntu_notices / "source-publications.json").write_text(
        json.dumps(info["ubuntu_sources"], indent=2) + "\n"
    )
    sources = package / "source"
    sources.mkdir(exist_ok=True)
    for path in (source_bundle, Path(str(source_bundle) + ".json")):
        if (sources / path.name).resolve() != path.resolve():
            shutil.copy2(path, sources / path.name)
    shutil.copy2(
        REPO / "docs/BLENDER_LINUX_SOURCE_BUILD.md", sources / "BUILD-SOURCE.md"
    )
    (package / "BLENDER-MESH-DISTRIBUTION.txt").write_text(
        "usdcc with Blender Mesh Tools — GPL-3.0-or-later combined Linux distribution.\n"
        "Copyright, warranty and upstream notices: LICENSE.txt and licenses/.\n"
        "Original usdcc/adapter sources retain BSD-3-Clause; other components retain their notices.\n"
        "Accompanying source, exact Ubuntu DSC/orig/debian archives, patches and build instructions: source/.\n"
        "Distribute all of source/ and licenses/ alongside the binaries without an extra source charge.\n"
        "Recipients may copy, modify and redistribute; no additional EULA, activation or signature restriction applies.\n"
        "Qt/ADS and the private backend remain replaceable shared libraries; see source/BUILD-SOURCE.md.\n"
        "Ruff/uv are separately licensed unmodified CLI tools aggregated with this package.\n"
        + "".join(f"USD plugin pack {pack} ({asset}): licences in licenses/OpenUSD-plugins/{pack}/,\n"
                  "its complete source in source/.\n"
                  for pack, asset in packs.items())
        + "Requires the Ubuntu 24.04 x86-64 system interfaces listed in binary-inventory.json.\n"
        "Blender is descriptive attribution; no Blender Foundation/Pixar endorsement is claimed.\n",
        encoding="utf-8",
    )
    (package / "binary-inventory.json").write_text(
        json.dumps(
            {"schema": 3, "binaries": rows, "system_libraries": system}, indent=2
        )
        + "\n"
    )
    report = {
        "schema": 3,
        "platform": "linux-x86_64",
        "ready_for_distribution": True,
        "configuration": "GPL-3.0-or-later combined distribution",
        "review_date": "2026-11-16",
        "legal_distribution_review": "complete: licenses/DISTRIBUTION-REVIEW.md",
        "corresponding_source_review": "complete: source accompanies the inventoried Linux binaries",
        "proprietary_renderer_integrations": "excluded (RTX, V-Ray)",
        "usd_plugin_packs": packs,
        "source_bundle": {
            "file": "source/" + source_bundle.name,
            "sha256": info["archive_sha256"],
        },
        "files": file_manifest(package),
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(
        f"Reviewed Linux distribution: {len(rows)} ELF binaries; {len(info['ubuntu_sources'])} exact Ubuntu source packages"
    )


def make_archive(package, output):
    verify(package)
    output.parent.mkdir(parents=True, exist_ok=True)
    expected = {row["path"]: row for row in file_manifest(package)}
    expected["distribution-readiness.json"] = {
        "type": "file",
        "sha256": sha256(package / "distribution-readiness.json"),
        "mode": (package / "distribution-readiness.json").stat().st_mode & 0o777,
    }
    with tarfile.open(output, "w:gz", compresslevel=1, dereference=False) as archive:
        archive.add(package, "usdcc-blender-linux-x86_64")
    seen = set()
    with tarfile.open(output) as archive:
        for member in archive:
            if member.isdir():
                continue
            name = member.name.removeprefix("usdcc-blender-linux-x86_64/")
            require(
                name in expected and name not in seen,
                "Unexpected/duplicate distribution archive member",
            )
            seen.add(name)
            row = expected[name]
            if row["type"] == "symlink":
                require(
                    member.issym() and member.linkname == row["target"],
                    "Archive changed symlink target",
                )
            else:
                require(
                    (member.isfile() or member.islnk())
                    and member.mode & 0o777 == row["mode"],
                    "Archive changed file type/mode",
                )
                require(
                    hashlib.file_digest(
                        archive.extractfile(member), "sha256"
                    ).hexdigest()
                    == row["sha256"],
                    "Distribution archive file checksum mismatch: " + name,
                )
    require(seen == set(expected), "Incomplete distribution archive")
    with gzip.open(output, "rb") as stream:
        while stream.read(1024 * 1024):
            pass
    Path(str(output) + ".sha256").write_text(sha256(output) + "  " + output.name + "\n")
    print(f"Verified Linux archive: {output}; {output.stat().st_size} bytes")


def check_restore(source_bundle):
    from blender_source_restore import restore
    from blender_ubuntu_sources import verify_receipts

    info = json.loads(Path(str(source_bundle) + ".json").read_text())
    require(
        sha256(source_bundle) == info["archive_sha256"],
        "Source archive checksum mismatch",
    )
    wanted = {
        "SOURCE-INVENTORY.json",
        "sources/blender-" + info["blender"]["commit"] + ".tar.gz",
    }
    wanted.update(row["archive_path"] for row in info["resources"])
    wanted.update(
        "sources/supplemental/" + row["file"] for row in info["supplemental_resources"]
    )
    wanted.update(
        "sources/git/" + row["name"] + ".bundle" for row in info["git_bundles"]
    )
    wanted.update(
        "sources/ubuntu/" + row["folder"] + "/" + item["file"]
        for row in info["ubuntu_sources"]
        for item in row["files"]
    )
    log = REPO / "build/blender-linux-source-restore.log"
    with tempfile.TemporaryDirectory(
        prefix="blender-linux-source-check-", dir=REPO / "build"
    ) as temporary:
        target = Path(temporary)
        with tarfile.open(source_bundle, "r|gz") as archive:
            for member in archive:
                if member.name in wanted:
                    archive.extract(member, target, filter="data")
        restore(target)
        cache = target / "usdcc/external/blender-work/ubuntu-sources"
        verify_receipts(info["ubuntu_sources"], cache)
        (target / "unpacked").mkdir()
        with log.open("w") as stream:
            for row in info["ubuntu_sources"]:
                dsc = next(
                    item["file"]
                    for item in row["files"]
                    if item["file"].endswith(".dsc")
                )
                subprocess.run(
                    [
                        "dpkg-source",
                        "--no-check",
                        "-x",
                        str(cache / row["folder"] / dsc),
                        str(target / "unpacked" / row["folder"]),
                    ],
                    check=True,
                    stdout=stream,
                    stderr=subprocess.STDOUT,
                )
        for row in info["git_bundles"]:
            folder = target / "usdcc/external" / row["name"]
            require(
                command("git", "-C", folder, "rev-parse", "HEAD") == row["commit"],
                "Restored Git identity mismatch",
            )
            command("git", "-C", folder, "cat-file", "-e", row["commit"] + "^{tree}")
    result = {
        "passed": True,
        "source_sha256": info["archive_sha256"],
        "ubuntu_source_packages_unpacked": len(info["ubuntu_sources"]),
        "vcpkg_resources": len(info["resources"]),
        "tool_source_archives": len(info["supplemental_resources"]),
        "network_required": False,
    }
    (REPO / "build/blender-linux-source-restore-result.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print(
        "Source caches, Git identities and all Ubuntu source patches restored without network access"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--source-uris", nargs=2, metavar=("PACKAGE", "VERSION"))
    parser.add_argument("--fetch-ubuntu", action="store_true")
    parser.add_argument("--fetch-vcpkg", action="store_true")
    parser.add_argument("--tool-source-cache", type=Path)
    parser.add_argument("--work", type=Path, default=Path.home() / "deps/usd-work")
    parser.add_argument(
        "--collect",
        action="store_true",
        help="collect evidence for manual review, without approving a release",
    )
    parser.add_argument(
        "--sources",
        type=Path,
        help="create corresponding-source archive using the reviewed Linux lock",
    )
    parser.add_argument("--finalize", type=Path, metavar="SOURCE_ARCHIVE")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--restore-check", type=Path, metavar="SOURCE_ARCHIVE")
    args = parser.parse_args()
    package = args.package.resolve()
    if args.restore_check:
        check_restore(args.restore_check.resolve())
        return
    if args.verify:
        verify(package)
        return
    if args.archive:
        make_archive(package, args.archive.resolve())
        return
    if args.finalize:
        finalize(package, args.work.resolve(), args.finalize.resolve())
        return
    if args.sources:
        from blender_linux_source_package import create

        create(
            package,
            args.work.resolve(),
            args.sources.resolve(),
            REVIEW,
            json.loads(LOCK.read_text()),
        )
        return
    if args.collect:
        from blender_linux_source_package import evidence

        rows = inventory(package, REPO / "build")
        system = system_libraries(package, rows)
        validate_rows(rows, system, package, REPO / "build")
        data = {
            "reviewed_inputs": evidence(rows, system, args.work.resolve()),
            "binaries": rows,
            "system_libraries": system,
        }
        (REPO / "build/blender-linux-review-inputs.json").write_text(
            json.dumps(data, indent=2) + "\n"
        )
        print(
            f"Collected evidence for {len(rows)} ELF binaries; approval remains pending"
        )
        return
    if args.fetch_vcpkg:
        from blender_mesh_package import resources

        records, _, _ = resources(
            REPO / "build", REPO / "external/blender-work/build", True
        )
        require(
            all(row["present"] for row in records), "Missing vcpkg source resources"
        )
        (REPO / "build/blender-linux-vcpkg-resources.json").write_text(
            json.dumps(records, indent=2) + "\n"
        )
        print(f"Verified {len(records)} vcpkg source resources")
        return
    if args.tool_source_cache:
        records = json.loads((REPO / "deps/blender-distribution.json").read_text())[
            "resources"
        ]
        records = [
            row
            for row in records
            if row["name"] in ("ruff", "uv") or row["name"].startswith("crate-")
        ]
        target = REPO / "external/blender-work/package-downloads"
        target.mkdir(parents=True, exist_ok=True)
        for row in records:
            source = args.tool_source_cache / row["file"]
            require(sha256(source) == row["sha256"], "Tool source archive changed")
            output = target / row["file"]
            if not output.exists():
                shutil.copy2(source, output)
        print(f"Verified/copied {len(records)} CLI tool source archives")
        return
    if args.fetch_ubuntu:
        from blender_ubuntu_sources import collect

        current = json.loads((REPO / "build/blender-linux-inventory.json").read_text())
        sources = {
            (row["ubuntu"]["source_package"], row["ubuntu"]["source_version"])
            for row in current["binaries"] + current["system_libraries"]
            if row.get("ubuntu")
            and row["ubuntu"]["source_package"] not in ("gcc-14", "glibc")
        }
        records = collect(sources, REPO / "external/blender-work/ubuntu-sources")
        (REPO / "build/blender-ubuntu-source-receipts.json").write_text(
            json.dumps(records, indent=2) + "\n"
        )
        print(f"Collected {len(records)} exact Ubuntu source packages")
        return
    if args.source_uris:
        name, version = args.source_uris
        url = (
            "https://api.launchpad.net/1.0/ubuntu/+archive/primary?"
            + urllib.parse.urlencode(
                {
                    "ws.op": "getPublishedSources",
                    "source_name": name,
                    "version": version,
                    "exact_match": "true",
                }
            )
        )
        with urllib.request.urlopen(url, timeout=90) as stream:
            result = json.load(stream)
        for row in result["entries"]:
            with urllib.request.urlopen(
                row["self_link"] + "?ws.op=sourceFileUrls", timeout=90
            ) as stream:
                print(
                    row["source_package_name"],
                    row["source_package_version"],
                    json.load(stream),
                )
            break
        return
    package = args.package.resolve()
    rows = inventory(package, REPO / "build")
    system = system_libraries(package, rows)
    output = REPO / "build/blender-linux-inventory.json"
    output.write_text(
        json.dumps({"binaries": rows, "system_libraries": system}, indent=2) + "\n"
    )
    print(f"Inventoried {len(rows)} binaries: {output}")
    for row in rows:
        if row["component"] == "UNKNOWN":
            print("Unmatched:", row["path"])
    sources = {
        (row["ubuntu"]["source_package"], row["ubuntu"]["source_version"])
        for row in rows
        if row["ubuntu"]
    }
    print("Ubuntu sources:", sorted(sources))
    print("Python source:", dpkg_owner(Path("/usr/bin/python3.12")))
    work = Path(cache_value(REPO / "build", "USD_INSTALL")).with_name("usd-work")
    print(
        "USD sources:",
        [path.name for path in (work / "src").iterdir() if path.is_dir()],
    )
    print("Components:", Counter(row["component"] for row in rows))
    owners = port_owners(REPO / "build")
    print(
        "Runtime ports:",
        sorted(
            {
                owners.get(row["input_file"].lower(), "UNKNOWN")
                for row in rows
                if row["component"] == "vcpkg"
            }
        ),
    )
    print(
        "System source packages:",
        sorted(
            {
                (row["ubuntu"]["source_package"], row["ubuntu"]["source_version"])
                for row in system
            }
        ),
    )
    print(
        "USD support binaries:",
        [
            row["path"]
            for row in rows
            if row["component"] == "OpenUSD"
            and not any(s in row["path"] for s in ("libusd_", "site-packages/pxr"))
        ],
    )
    print(
        "USD dependency caches:",
        [str(path) for path in (work / "build").rglob("CMakeCache.txt")],
    )


if __name__ == "__main__":
    main()
