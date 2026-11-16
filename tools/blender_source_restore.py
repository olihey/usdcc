"""Restore build inputs from an extracted Blender corresponding-source archive.

Run with Python 3.12+: python usdcc/tools/blender_source_restore.py <extracted-root>
Does not fetch anything. General compiler/build tools are installed separately.
"""

import argparse
import json
import shutil
import subprocess
from pathlib import Path

from blender_distribution import require, sha256


def restore(root):
    info = json.loads((root / "SOURCE-INVENTORY.json").read_text())
    repo = root / "usdcc"
    downloads = repo / "external/vcpkg/downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    for row in info["resources"]:
        require(row["present"], f"Missing resource: {row['name']}")
        source = root / row["archive_path"]
        import hashlib

        with source.open("rb") as stream:
            require(
                hashlib.file_digest(stream, "sha512").hexdigest() == row["sha512"],
                f"Changed resource: {source}",
            )
        shutil.copy2(source, downloads / Path(row["cache_file"]).name)
    blender_downloads = repo / "external/blender-work/downloads"
    blender_downloads.mkdir(parents=True, exist_ok=True)
    name = f"blender-{info['blender']['commit']}.tar.gz"
    require(
        sha256(root / "sources" / name) == info["blender"]["sha256"],
        "Changed Blender source",
    )
    shutil.copy2(root / "sources" / name, blender_downloads / name)
    supplemental = repo / "external/blender-work/package-downloads"
    supplemental.mkdir(parents=True, exist_ok=True)
    for row in info["supplemental_resources"]:
        source = root / "sources/supplemental" / row["file"]
        require(
            sha256(source) == row["sha256"], f"Changed supplemental source: {source}"
        )
        shutil.copy2(source, supplemental / row["file"])
    for row in info["git_bundles"]:
        bundle = root / "sources/git" / (row["name"] + ".bundle")
        require(sha256(bundle) == row["sha256"], "Changed Git bundle")
        folder = repo / "external" / row["name"]
        if not (folder / ".git").exists():
            subprocess.run(["git", "init", "-q", str(folder)], check=True)
            subprocess.run(
                ["git", "-C", str(folder), "fetch", str(bundle), "HEAD"], check=True
            )
            subprocess.run(
                ["git", "-C", str(folder), "update-ref", "HEAD", row["commit"]],
                check=True,
            )
    for row in info.get("ubuntu_sources", []):
        folder = repo / "external/blender-work/ubuntu-sources" / row["folder"]
        folder.mkdir(parents=True, exist_ok=True)
        for item in row["files"]:
            source = root / "sources/ubuntu" / row["folder"] / item["file"]
            require(
                sha256(source) == item["sha256"],
                "Changed Ubuntu source: " + item["file"],
            )
            shutil.copy2(source, folder / item["file"])
        (folder / "receipt.json").write_text(json.dumps(row, indent=2) + "\n")
    print(f"Restored source inputs in {repo}. Follow BUILD-SOURCE.md.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    restore(parser.parse_args().root.resolve())
