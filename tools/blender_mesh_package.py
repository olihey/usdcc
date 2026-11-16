"""Assemble a reviewable source bundle and dependency inventory for mesh tools.

This creates a distribution candidate, not a legal approval or a publication.
The app snapshot includes local implementation changes, excluding private agent
configuration. Source resources are verified against vcpkg's SPDX checksums.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path
from urllib.parse import quote

from _common import REPO, cache_value


def digest(path, algorithm="sha256"):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def git(folder, *args):
    return subprocess.check_output(
        ["git", "-c", f"safe.directory={folder.as_posix()}", "-C", str(folder), *args]
    )


def project_files():
    tracked = git(REPO, "ls-files", "--cached", "--recurse-submodules", "-z").split(
        b"\0"
    )
    others = git(REPO, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
    allowed = ("src/", "tools/", "tests/", "deps/", "docs/", "cmake/", ".github/")
    names = {name.decode() for name in tracked if name}
    names.update(
        name.decode() for name in others if name and name.decode().startswith(allowed)
    )
    return sorted(
        name
        for name in names
        if not name.startswith((".claude/", ".agents/", ".codex/"))
        and (REPO / name).is_file()
    )


def resources(build, backend_build, fetch):
    documents = sorted((build / "vcpkg_installed").glob("*/share/*/vcpkg.spdx.json"))
    documents += sorted(
        (backend_build / "vcpkg_installed").glob("*/share/*/vcpkg.spdx.json")
    )
    cache = REPO / "external/vcpkg/downloads"
    downloads = REPO / "external/blender-work/package-downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    records, archives, seen = [], [], set()
    cache_index = None
    for document in documents:
        info = json.loads(document.read_text(encoding="utf-8"))
        for package in info.get("packages", []):
            url = package.get("downloadLocation", "")
            checksums = {
                row["algorithm"].lower(): row["checksumValue"]
                for row in package.get("checksums", [])
            }
            checksum = checksums.get("sha512")
            if (
                not url.startswith(("https://", "git+https://"))
                or not checksum
                or checksum in seen
            ):
                continue
            seen.add(checksum)
            name = Path(package.get("packageFileName") or package["name"]).name
            path = cache / name
            target = downloads / (checksum[:16] + "-" + name)
            if not path.is_file() or digest(path, "sha512") != checksum:
                path = target
            if not path.is_file():
                if cache_index is None:
                    cache_index = {
                        digest(p, "sha512"): p for p in cache.iterdir() if p.is_file()
                    }
                path = cache_index.get(checksum, path)
            if (not path.is_file() or digest(path, "sha512") != checksum) and fetch:
                download_url = url
                if url.startswith("git+"):
                    repository, ref = url[4:].rsplit("@", 1)
                    if repository.startswith("https://github.com/"):
                        download_url = (
                            repository + "/archive/" + quote(ref, safe="") + ".tar.gz"
                        )
                    elif repository.startswith("https://gitlab.com/"):
                        download_url = (
                            repository
                            + "/-/archive/"
                            + quote(ref, safe="")
                            + "/"
                            + repository.rsplit("/", 1)[-1]
                            + "-"
                            + quote(ref, safe="")
                            + ".tar.gz"
                        )
                    else:
                        records.append(
                            {
                                "name": name,
                                "url": url,
                                "sha512": checksum,
                                "present": False,
                            }
                        )
                        continue
                temporary = target.with_suffix(target.suffix + ".partial")
                print(f"Source resource: {name}", flush=True)
                with (
                    urllib.request.urlopen(download_url, timeout=120) as response,
                    temporary.open("wb") as output,
                ):
                    shutil.copyfileobj(response, output)
                if digest(temporary, "sha512") != checksum:
                    raise RuntimeError(f"Source checksum mismatch: {name}")
                temporary.replace(target)
                path = target
            present = path.is_file() and digest(path, "sha512") == checksum
            entry = {
                "name": name,
                "url": url,
                "sha512": checksum,
                "present": present,
                "cache_file": name,
                "archive_path": "sources/vcpkg/" + checksum[:16] + "-" + path.name
                if present
                else None,
            }
            records.append(entry)
            if present:
                archives.append(
                    (path, "sources/vcpkg/" + checksum[:16] + "-" + path.name)
                )
    return records, archives, documents


def supplemental_resources(fetch=False):
    """Reviewed sources for binary dependencies outside vcpkg (Windows recipe)."""
    lock = json.loads((REPO / "deps/blender-distribution.json").read_text())
    cache = REPO / "external/blender-work/package-downloads"
    cache.mkdir(parents=True, exist_ok=True)
    result = []
    for row in lock["resources"]:
        path = cache / row["file"]
        if not path.is_file() and fetch:
            temporary = path.with_suffix(path.suffix + ".partial")
            with (
                urllib.request.urlopen(row["url"], timeout=120) as response,
                temporary.open("wb") as output,
            ):
                shutil.copyfileobj(response, output)
            if digest(temporary) != row["sha256"]:
                raise RuntimeError(
                    f"Supplemental source checksum mismatch: {row['name']}"
                )
            temporary.replace(path)
        if not path.is_file() or digest(path) != row["sha256"]:
            raise RuntimeError(f"Missing or changed supplemental source: {row['name']}")
        result.append((path, "sources/supplemental/" + row["file"]))
    return lock, result


def blender_compiler_inputs(backend_build, blender_archive, commit):
    """Verify the used original Blender headers/TUs against the immutable archive."""
    root = REPO / "external/blender-work/source" / ("blender-" + commit)
    used = {}
    for log in backend_build.glob("*.dir/RelWithDebInfo/*/CL.read.1.tlog"):
        if "probe" in log.parent.parent.parent.name:
            continue
        for line in log.read_text(encoding="utf-16").splitlines():
            for value in line.lstrip("^").split("|"):
                path = Path(value.strip())
                if path.is_file() and path.is_relative_to(root):
                    used[path.relative_to(root).as_posix().lower()] = path
    if len(used) < 200:
        raise RuntimeError("Blender compiler header/source input evidence is missing")
    records = []
    with tarfile.open(blender_archive) as archive:
        for member in archive:
            relative = member.name.partition("/")[2].lower()
            if member.isfile() and relative in used:
                data = archive.extractfile(member).read()
                expected = hashlib.sha256(data).hexdigest()
                if digest(used[relative]) != expected:
                    raise RuntimeError(
                        f"Modified original Blender input missing from source recipe: {relative}"
                    )
                if b"SPDX-License-Identifier: GPL-2.0-only" in data[:3000]:
                    raise RuntimeError(
                        f"GPL-2.0-only input needs renewed compatibility review: {relative}"
                    )
                records.append({"path": relative, "sha256": expected})
    if len(records) != len(used):
        raise RuntimeError(
            "Compiler used Blender files absent from the original archive"
        )
    return sorted(records, key=lambda row: row["path"])


def include_candidate(destination, build, source_bundle=None):
    """Called by deploy.py before Linux RUNPATH repair and dependency checks."""
    plugin = build / "plugins/blender_mesh_tools"
    if (
        not (plugin / "usdcc-plugin.json").is_file()
        or not (plugin / "build-info.json").is_file()
    ):
        raise RuntimeError("Build mesh tools before packaging")
    target = destination / "plugins/blender_mesh_tools"
    shutil.copytree(plugin, target, dirs_exist_ok=True)
    shutil.copytree(
        plugin / "licenses",
        destination / "licenses/Blender-mesh-tools",
        dirs_exist_ok=True,
    )
    gpl = plugin / "licenses/blender/spdx/GPL-3.0-or-later.txt"
    shutil.copy2(gpl, destination / "licenses/GPL-3.0-or-later.txt")
    readiness = {
        "configuration": "GPL-linked mesh tools candidate",
        "legal_distribution_review": "pending",
        "corresponding_source_review": "pending: verify OpenUSD bundled dependency source closure and library variants",
        "proprietary_renderer_integrations": "excluded",
        "source_bundle": None,
    }
    if source_bundle:
        source_bundle = Path(source_bundle).resolve()
        metadata = Path(str(source_bundle) + ".json")
        info = json.loads(metadata.read_text(encoding="utf-8"))
        if digest(source_bundle) != info["archive_sha256"]:
            raise RuntimeError("Source bundle checksum mismatch")
        sources = destination / "source"
        sources.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_bundle, sources / source_bundle.name)
        shutil.copy2(metadata, sources / metadata.name)
        readiness["source_bundle"] = {
            "file": "source/" + source_bundle.name,
            "sha256": info["archive_sha256"],
            "resources_complete": info["resources_complete"],
        }
    (destination / "BLENDER-MESH-DISTRIBUTION.txt").write_text(
        "This configuration links GPL-covered Blender code in-process. Treat the combined app/plugin\n"
        "as GPL-covered; our original component sources retain BSD-3-Clause notices.\n"
        "GPLv3 terms are in licenses/GPL-3.0-or-later.txt; upstream notices remain in licenses/.\n"
        "Corresponding-source evidence is in source/ when supplied. Publish source access beside binaries.\n"
        "This is a private distribution candidate. Review distribution-readiness.json before publication.\n",
        encoding="utf-8",
    )
    (destination / "distribution-readiness.json").write_text(
        json.dumps(readiness, indent=2) + "\n", encoding="utf-8"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO / "build/blender-mesh-corresponding-source.tar.gz",
    )
    parser.add_argument(
        "--usd-source",
        type=Path,
        required=True,
        help="the actual OpenUSD source checkout used to build the installed libraries",
    )
    parser.add_argument(
        "--fetch",
        action="store_true",
        help="download missing checksum-pinned source resources",
    )
    parser.add_argument(
        "--package",
        type=Path,
        required=True,
        help="exact deployed Blender package whose binaries this source accompanies",
    )
    args = parser.parse_args()
    build = REPO / "build"
    prefix = Path(cache_value(build, "USDCC_BLENDER_MESH_ROOT"))
    backend = json.loads((prefix / "build-info.json").read_text(encoding="utf-8"))
    backend_build = REPO / "external/blender-work/build"
    usd = Path(cache_value(build, "USD_INSTALL"))
    usd_pin = json.loads((REPO / "deps/usd.json").read_text(encoding="utf-8"))
    actual_usd = git(args.usd_source.resolve(), "rev-parse", "HEAD").decode().strip()
    if actual_usd != usd_pin["commit"]:
        raise RuntimeError("OpenUSD checkout does not match deps/usd.json")
    blender = backend["blender"]
    blender_archive = (
        REPO / f"external/blender-work/downloads/blender-{blender['commit']}.tar.gz"
    )
    if digest(blender_archive) != blender["sha256"]:
        raise RuntimeError("Blender archive checksum mismatch")
    source_records, archives, documents = resources(build, backend_build, args.fetch)
    supplement, supplemental_archives = supplemental_resources(args.fetch)
    archives.extend(supplemental_archives)
    info = {
        "schema": 2,
        "usdcc_commit": git(REPO, "rev-parse", "HEAD").decode().strip(),
        "working_tree_snapshot": True,
        "blender": blender,
        "openusd": usd_pin,
        "platform": supplement["platform"],
        "supplemental_resources": supplement["resources"],
        "resources": source_records,
        "resources_complete": all(row["present"] for row in source_records),
        "license_policy_sha256": digest(REPO / "docs/BLENDER_DISTRIBUTION_REVIEW.md"),
        "source_files": [],
    }
    from blender_distribution import inventory

    info["binary_files"] = [
        {"path": row["path"], "sha256": row["sha256"]}
        for row in inventory(args.package)
    ]
    info["openusd_license_sha256"] = digest(args.usd_source / "LICENSE.txt")
    info["openusd_notice_sha256"] = digest(args.usd_source / "NOTICE.txt")
    info["blender_compiler_inputs"] = blender_compiler_inputs(
        backend_build, blender_archive, blender["commit"]
    )
    info["openusd_diff_sha256"] = hashlib.sha256(
        git(args.usd_source.resolve(), "diff", "HEAD", "--binary")
    ).hexdigest()
    changed_usd = {
        name.decode()
        for name in git(
            args.usd_source.resolve(), "diff", "HEAD", "--name-only", "-z"
        ).split(b"\0")
        if name
    }
    if changed_usd != {"build_scripts/build_usd.py"}:
        raise RuntimeError(
            "OpenUSD source modifications differ from the reviewed GLM patch"
        )
    info["dependency_source_trees"] = sorted(
        path.name for path in (usd / "src").iterdir() if path.is_dir()
    )
    info["pyside_commit"] = (
        git(REPO / "external/pyside-setup", "rev-parse", "HEAD").decode().strip()
    )
    info["git_bundles"] = []
    bundles = []
    for name in ("vcpkg", "qwindowkit"):
        folder = REPO / "external" / name
        path = REPO / "external/blender-work" / (name + ".bundle")
        git(folder, "bundle", "create", str(path), "HEAD")
        commit = git(folder, "rev-parse", "HEAD").decode().strip()
        bundles.append((path, "sources/git/" + path.name))
        info["git_bundles"].append(
            {"name": name, "commit": commit, "sha256": digest(path)}
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w:gz", compresslevel=1) as package:
        for name in project_files():
            package.add(REPO / name, "usdcc/" + name, recursive=False)
            info["source_files"].append(
                {"path": "usdcc/" + name, "sha256": digest(REPO / name)}
            )
        package.add(blender_archive, "sources/" + blender_archive.name)
        package.add(prefix / "source-excerpts", "sources/blender-generated")
        package.add(prefix / "licenses", "licenses/blender-backend")
        for path, name in archives:
            package.add(path, name)
        for path, name in bundles:
            package.add(path, name)
        for i, path in enumerate(documents):
            package.add(path, f"inventory/vcpkg/{i}-{path.parent.name}.spdx.json")
            copyright_file = path.parent / "copyright"
            if copyright_file.is_file():
                package.add(
                    copyright_file, f"licenses/vcpkg/{i}-{path.parent.name}.txt"
                )
        for name in git(args.usd_source.resolve(), "ls-files", "-z").split(b"\0"):
            if name and (args.usd_source / name.decode()).is_file():
                if name.decode() in changed_usd:
                    path = args.usd_source / name.decode()
                    original = path.read_bytes()
                    first, rest = original.split(b"\n", 1)
                    data = (
                        first
                        + b"\n# Modified for usdcc (2026-11-15): GLM integration; see usdcc/deps/patches/build_usd-glm.patch.\n"
                        + rest
                    )
                    member = package.gettarinfo(
                        path, "sources/OpenUSD/" + name.decode()
                    )
                    member.size = len(data)
                    package.addfile(member, io.BytesIO(data))
                    continue
                package.add(
                    args.usd_source / name.decode(),
                    "sources/OpenUSD/" + name.decode(),
                    recursive=False,
                )
        # Exact dependency build options, including bundled FetchContent versions.
        for path in sorted((usd / "build").rglob("CMakeCache.txt")):
            package.add(
                path,
                "inventory/OpenUSD-build/" + path.relative_to(usd / "build").as_posix(),
            )
        for path in sorted(backend_build.glob("*.dir/RelWithDebInfo/*/CL.read.1.tlog")):
            package.add(
                path,
                "inventory/Blender-compiler-inputs/"
                + path.parent.parent.parent.name
                + ".tlog",
            )
        # Local build_usd.py sources include bundled dependency source trees.
        if (usd / "src").is_dir():
            # The complete extracted trees already contain the preferred source.
            # Avoid shipping duplicate download ZIPs (over 500 MB in this build).
            for folder in sorted((usd / "src").iterdir()):
                if folder.is_dir():
                    package.add(folder, "sources/OpenUSD-dependencies/" + folder.name)
        for parent in sorted((usd / "build").glob("*/_deps")):
            for folder in sorted(parent.glob("*-src")):
                package.add(
                    folder,
                    "sources/OpenUSD-bundled/" + parent.parent.name + "/" + folder.name,
                )
        for parent in sorted((usd / "build").glob("*/ext/build")):
            for folder in sorted(parent.iterdir()):
                if folder.is_dir():
                    package.add(
                        folder,
                        "sources/OpenUSD-bundled/"
                        + parent.parent.parent.name
                        + "/"
                        + folder.name,
                    )
        if (REPO / "external/pyside-setup").is_dir():

            def without_git(member):
                return (
                    None
                    if "/.git/" in member.name or member.name.endswith("/.git")
                    else member
                )

            package.add(
                REPO / "external/pyside-setup",
                "sources/pyside-setup",
                filter=without_git,
            )
        package.add(prefix / "build-info.json", "inventory/blender-build-info.json")
        package.add(build / "CMakeCache.txt", "inventory/app-CMakeCache.txt")
        package.add(REPO / "docs/BLENDER_MESH_TOOLS.md", "BUILD-AND-LICENSE-POLICY.md")
        package.add(
            REPO / "docs/BLENDER_DISTRIBUTION_REVIEW.md", "DISTRIBUTION-REVIEW.md"
        )
        package.add(REPO / "docs/BLENDER_SOURCE_BUILD.md", "BUILD-SOURCE.md")
        data = (json.dumps(info, indent=2) + "\n").encode()
        member = tarfile.TarInfo("SOURCE-INVENTORY.json")
        member.size = len(data)
        package.addfile(member, io.BytesIO(data))
    info["archive_sha256"] = digest(args.output)
    Path(str(args.output) + ".json").write_text(
        json.dumps(info, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"Source candidate: {args.output}; source resources complete: {info['resources_complete']}"
    )


if __name__ == "__main__":
    main()
