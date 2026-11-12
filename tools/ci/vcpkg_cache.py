"""Sets up vcpkg's binary cache for a CI job and names its actions/cache keys.

vcpkg's files cache holds one zip per package build, named after the package's
ABI hash (which covers its port, its dependencies and the compiler); docs/CI.md
section 4 describes how the workflows keep it.

Points VCPKG_BINARY_SOURCES at $RUNNER_TEMP/vcpkg-cache (through $GITHUB_ENV, for
the later steps) and appends to $GITHUB_OUTPUT:
  dir    the cache folder
  deps   the key prefix for these dependencies: vcpkg.json, the overlay ports in
         cmake/ports with vcpkg-configuration.json, and the vcpkg submodule commit
  any    the key prefix for any dependencies
  key    deps plus this run's id and attempt, for saving

--access is readwrite for a job that builds packages and saves the cache, read
for one that only uses it.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import REPO, VCPKG_TRIPLET, append_line, fail, normalized_text, report, short_hash  # noqa: E402


def dependencies_hash():
    """A hash of everything in the repo that says which packages vcpkg builds."""
    files = [REPO / name for name in ("vcpkg.json", "vcpkg-configuration.json") if (REPO / name).is_file()]
    ports = REPO / "cmake/ports"
    if ports.is_dir():
        files += [file for file in ports.rglob("*") if file.is_file()]
    names = sorted("/" + file.relative_to(REPO).as_posix() for file in files)
    hashed = "".join(f"{name}\n{normalized_text(REPO / name[1:])}" for name in names)
    return short_hash(hashed, 8)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--access", choices=("readwrite", "read"), default="readwrite")
    args = parser.parse_args(argv)

    commit = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD:external/vcpkg"],
                            capture_output=True, text=True)
    if commit.returncode != 0:
        fail("No vcpkg submodule commit")

    for name in ("RUNNER_TEMP", "GITHUB_ENV"):
        if not os.environ.get(name):
            fail(f"{name} isn't set: this only works in a GitHub Actions step.")
    cache_dir = os.path.join(os.environ["RUNNER_TEMP"], "vcpkg-cache")
    os.makedirs(cache_dir, exist_ok=True)
    append_line(os.environ["GITHUB_ENV"], f"VCPKG_BINARY_SOURCES=clear;files,{cache_dir},{args.access}")

    any_key = f"vcpkg-{VCPKG_TRIPLET}-{os.environ.get('ImageOS', '')}-"
    deps_key = f"{any_key}{dependencies_hash()}-{commit.stdout.strip()}-"
    run_id = f"{os.environ.get('GITHUB_RUN_ID', '')}-{os.environ.get('GITHUB_RUN_ATTEMPT', '')}"
    report({"dir": cache_dir, "any": any_key, "deps": deps_key, "key": deps_key + run_id})


if __name__ == "__main__":
    main()
