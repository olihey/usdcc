"""Collect Linux corresponding source and build evidence. Original tooling: BSD-3-Clause."""

from __future__ import annotations

import hashlib
import io
import json
import shlex
import tarfile
from pathlib import Path

from _common import REPO, cache_value
from blender_distribution import backend_identity, require, sha256
from blender_mesh_package import git, project_files, resources
from blender_ubuntu_sources import verify_receipts


def compiler_inputs(build, archive, commit):
    source = REPO / "external/blender-work/source" / ("blender-" + commit)
    used = {}
    for dependency in (build / "CMakeFiles").rglob("*.o.d"):
        for value in shlex.split(dependency.read_text().replace("\\\n", " ")):
            path = Path(value).resolve()
            if path.is_file() and path.is_relative_to(source):
                used[path.relative_to(source).as_posix()] = path
    require(len(used) >= 200, "Missing Linux compiler dependency evidence")
    records = []
    with tarfile.open(archive) as package:
        for member in package:
            relative = member.name.partition("/")[2]
            if member.isfile() and relative in used:
                data = package.extractfile(member).read()
                expected = hashlib.sha256(data).hexdigest()
                require(
                    expected == sha256(used[relative]),
                    "Modified original Blender input needs a source patch: " + relative,
                )
                require(
                    b"SPDX-License-Identifier: GPL-2.0-only" not in data[:3000],
                    "GPL-2.0-only Blender input needs renewed review",
                )
                records.append({"path": relative, "sha256": expected})
    require(
        len(records) == len(used),
        "Compiler used files absent from the original Blender archive: "
        + str(sorted(set(used) - {row["path"] for row in records})[:8]),
    )
    return sorted(records, key=lambda row: row["path"])


def tool_sources():
    rows = json.loads((REPO / "deps/blender-distribution.json").read_text())[
        "resources"
    ]
    return [
        row
        for row in rows
        if row["name"] in ("ruff", "uv") or row["name"].startswith("crate-")
    ]


def evidence(rows, system, work):
    build = REPO / "build"
    backend_build = REPO / "external/blender-work/build"
    prefix = Path(cache_value(build, "USDCC_BLENDER_MESH_ROOT"))
    backend = json.loads((prefix / "build-info.json").read_text())
    pin = backend["blender"]
    original = (
        REPO
        / "external/blender-work/downloads"
        / ("blender-" + pin["commit"] + ".tar.gz")
    )
    require(sha256(original) == pin["sha256"], "Blender archive changed")
    headers = compiler_inputs(backend_build, original, pin["commit"])
    source_records, _, documents = resources(build, backend_build, False)
    require(
        all(row["present"] for row in source_records), "Missing Linux vcpkg sources"
    )
    for row in backend["source_files"]:
        require(
            sha256(row["path"]) == row["sha256"], "Blender input drift: " + row["path"]
        )
    require(
        backend["features"]
        == {"python": False, "openusd": False, "tbb": False, "eigen_mpl2_only": True},
        "Unreviewed backend features",
    )
    source = work / "OpenUSD"
    usd_pin = json.loads((REPO / "deps/usd.json").read_text())
    require(
        git(source, "rev-parse", "HEAD").decode().strip() == usd_pin["commit"],
        "OpenUSD revision differs from pin",
    )
    ports = {}
    for path in documents:
        data = json.loads(path.read_text())
        ports[path.parent.name] = {
            "version": data["packages"][0].get("versionInfo"),
            "copyright_sha256": sha256(path.parent / "copyright")
            if (path.parent / "copyright").is_file()
            else None,
        }
    ubuntu = json.loads((build / "blender-ubuntu-source-receipts.json").read_text())
    verify_receipts(ubuntu, REPO / "external/blender-work/ubuntu-sources")
    required_sources = {
        (row["ubuntu"]["source_package"], row["ubuntu"]["source_version"])
        for row in rows + system
        if row.get("ubuntu")
        and row["ubuntu"]["source_package"] not in ("gcc-14", "glibc")
    }
    require(
        required_sources == {(row["package"], row["version"]) for row in ubuntu},
        "Ubuntu source receipt does not cover runtime closure",
    )
    tools = tool_sources()
    for row in tools:
        require(
            sha256(REPO / "external/blender-work/package-downloads" / row["file"])
            == row["sha256"],
            "Tool source missing/changed",
        )
    values = {
        "platform": "linux-x86_64",
        "os_release": Path("/etc/os-release").read_text(),
        "blender_pin": pin,
        "openusd_pin": usd_pin,
        "backend_input_sha256": backend_identity(backend),
        "backend_toolchain": backend["toolchain"],
        "blender_compiler_inputs": headers,
        "openusd_diff_sha256": hashlib.sha256(
            git(source, "diff", "HEAD", "--binary")
        ).hexdigest(),
        "submodule_commits": {
            name: git(REPO / "external" / name, "rev-parse", "HEAD").decode().strip()
            for name in ("vcpkg", "qwindowkit")
        },
        "vcpkg_resource_sha512": sorted(row["sha512"] for row in source_records),
        "vcpkg_ports": ports,
        "ubuntu_sources": ubuntu,
        "core_system_packages": sorted(
            {
                (row["ubuntu"]["source_package"], row["ubuntu"]["source_version"])
                for row in system
                if row["ubuntu"]["source_package"] in ("gcc-14", "glibc")
            }
        ),
        "supplemental_resources": tools,
        "tool_versions": {
            name: (build / name / "VERSION").read_text().strip()
            for name in ("ruff", "uv")
        },
        "usd_dependency_trees": sorted(
            path.name for path in (work / "src").iterdir() if path.is_dir()
        ),
        "pyside": "not shipped in this configuration",
    }
    require(
        'VERSION_ID="24.04"' in values["os_release"],
        "Unreviewed Linux distribution version",
    )
    # The checked lock is JSON; normalize tuple-valued evidence before comparison.
    return json.loads(json.dumps(values))


def add_bytes(package, path, data):
    member = tarfile.TarInfo(path)
    member.size = len(data)
    member.mode = 0o644
    package.addfile(member, io.BytesIO(data))


def create(package_root, work, output, review, lock):
    from blender_linux_distribution import inventory, system_libraries, validate_rows

    build, backend_build = REPO / "build", REPO / "external/blender-work/build"
    rows = inventory(package_root, build)
    system = system_libraries(package_root, rows)
    validate_rows(rows, system, package_root, build)
    inputs = evidence(rows, system, work)
    require(
        inputs == lock["reviewed_inputs"],
        "Linux inputs differ from the reviewed lock; renew review: "
        + str(
            [key for key in inputs if inputs[key] != lock["reviewed_inputs"].get(key)]
        ),
    )
    source_records, archives, documents = resources(build, backend_build, False)
    info = {
        "schema": 3,
        "platform": "linux-x86_64",
        "reviewed_inputs": inputs,
        "license_policy_sha256": sha256(review),
        "review_lock_sha256": sha256(REPO / "deps/blender-linux-distribution.json"),
        "usdcc_commit": git(REPO, "rev-parse", "HEAD").decode().strip(),
        "working_tree_snapshot": True,
        "blender": inputs["blender_pin"],
        "openusd": inputs["openusd_pin"],
        "resources": source_records,
        "resources_complete": True,
        "ubuntu_sources": inputs["ubuntu_sources"],
        "supplemental_resources": inputs["supplemental_resources"],
        "binary_files": [
            {"path": row["path"], "sha256": row["sha256"]} for row in rows
        ],
        "source_files": [],
        "git_bundles": [],
        "openusd_license_sha256": sha256(work / "OpenUSD/LICENSE.txt"),
        "openusd_notice_sha256": sha256(work / "OpenUSD/NOTICE.txt"),
    }
    bundles = []
    for name in ("vcpkg", "qwindowkit"):
        bundle = REPO / "external/blender-work" / ("linux-" + name + ".bundle")
        git(REPO / "external" / name, "bundle", "create", str(bundle), "HEAD")
        info["git_bundles"].append(
            {
                "name": name,
                "commit": inputs["submodule_commits"][name],
                "sha256": sha256(bundle),
            }
        )
        bundles.append((bundle, "sources/git/" + name + ".bundle"))
    prefix = Path(cache_value(build, "USDCC_BLENDER_MESH_ROOT"))

    def source_filter(member):
        pieces = Path(member.name).parts
        if any(
            piece
            in (".git", ".claude", ".agents", ".codex", "__pycache__", "CMakeFiles")
            for piece in pieces
        ):
            return None
        if (
            member.name.endswith((".o", ".a", ".pyc", ".so"))
            or ".so." in Path(member.name).name
        ):
            return None
        return member

    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz", compresslevel=1) as package:
        for name in project_files():
            package.add(REPO / name, "usdcc/" + name, recursive=False)
            info["source_files"].append(
                {"path": "usdcc/" + name, "sha256": sha256(REPO / name)}
            )
        blender_name = "blender-" + inputs["blender_pin"]["commit"] + ".tar.gz"
        package.add(
            REPO / "external/blender-work/downloads" / blender_name,
            "sources/" + blender_name,
        )
        package.add(prefix / "source-excerpts", "sources/blender-generated")
        package.add(prefix / "licenses", "licenses/blender-backend")
        for path, name in archives + bundles:
            package.add(path, name)
        for row in info["supplemental_resources"]:
            package.add(
                REPO / "external/blender-work/package-downloads" / row["file"],
                "sources/supplemental/" + row["file"],
            )
        for row in info["ubuntu_sources"]:
            folder = REPO / "external/blender-work/ubuntu-sources" / row["folder"]
            for item in row["files"]:
                package.add(
                    folder / item["file"],
                    "sources/ubuntu/" + row["folder"] + "/" + item["file"],
                )
        changed = {
            name.decode()
            for name in git(
                work / "OpenUSD", "diff", "HEAD", "--name-only", "-z"
            ).split(b"\0")
            if name
        }
        for value in git(work / "OpenUSD", "ls-files", "-z").split(b"\0"):
            if value and (work / "OpenUSD" / value.decode()).is_file():
                path = work / "OpenUSD" / value.decode()
                arcname = "sources/OpenUSD/" + value.decode()
                if value.decode() in changed:
                    require(path.suffix == ".py", "Unreviewed USD source modification")
                    first, rest = path.read_bytes().split(b"\n", 1)
                    add_bytes(
                        package,
                        arcname,
                        first
                        + b"\n# Modified for usdcc (2026-11-16); see usdcc/deps/patches and captured build configuration.\n"
                        + rest,
                    )
                else:
                    package.add(path, arcname, recursive=False)
        for folder in sorted((work / "src").iterdir()):
            if folder.is_dir():
                package.add(
                    folder,
                    "sources/OpenUSD-dependencies/" + folder.name,
                    filter=source_filter,
                )
        for parent in sorted((work / "build").glob("*/_deps")):
            for folder in sorted(parent.glob("*-src")):
                package.add(
                    folder,
                    "sources/OpenUSD-bundled/" + parent.parent.name + "/" + folder.name,
                    filter=source_filter,
                )
        for parent in sorted((work / "build").glob("*/ext/build")):
            for folder in sorted(parent.iterdir()):
                if folder.is_dir():
                    package.add(
                        folder,
                        "sources/OpenUSD-bundled/"
                        + parent.parent.parent.name
                        + "/"
                        + folder.name,
                        filter=source_filter,
                    )
        for path in sorted((work / "build").rglob("CMakeCache.txt")):
            package.add(
                path,
                "inventory/OpenUSD-build/"
                + path.relative_to(work / "build").as_posix(),
            )
        for name, folder in (("app", build), ("blender", backend_build)):
            package.add(
                folder / "CMakeCache.txt", "inventory/" + name + "-CMakeCache.txt"
            )
        package.add(prefix / "build-info.json", "inventory/blender-build-info.json")
        for i, path in enumerate(documents):
            package.add(path, f"inventory/vcpkg/{i}-{path.parent.name}.spdx.json")
            if (path.parent / "copyright").is_file():
                package.add(
                    path.parent / "copyright",
                    f"licenses/vcpkg/{i}-{path.parent.name}.txt",
                )
        package.add(review, "DISTRIBUTION-REVIEW.md")
        package.add(
            REPO / "docs/BLENDER_DISTRIBUTION_REVIEW.md",
            "BLENDER_DISTRIBUTION_REVIEW.md",
        )
        package.add(REPO / "docs/BLENDER_LINUX_SOURCE_BUILD.md", "BUILD-SOURCE.md")
        add_bytes(
            package,
            "SOURCE-INVENTORY.json",
            (json.dumps(info, indent=2) + "\n").encode(),
        )
    info["archive_sha256"] = sha256(output)
    Path(str(output) + ".json").write_text(json.dumps(info, indent=2) + "\n")
    print(
        f"Collected Linux corresponding source: {output} ({output.stat().st_size} bytes)"
    )
    return info
