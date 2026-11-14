"""ELF files on Linux: telling them apart and pointing their RUNPATH somewhere.

A build has absolute RUNPATHs: CMake's build tree, build_usd.py's install folder.
tools/ci/relocate_usd.py and tools/deploy.py rewrite them relative to $ORIGIN, so
the files load from wherever they are unpacked. Uses patchelf (apt install
patchelf). Standard library only.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from _common import fail


def is_elf(path):
    """Whether `path` is a regular file that is an ELF binary (not a symlink)."""
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with open(path, "rb") as file:
            return file.read(4) == b"\x7fELF"
    except OSError:
        return False


def elf_files(folder):
    """The ELF binaries under `folder`, sorted."""
    return sorted(Path(root) / name for root, _, names in os.walk(folder) for name in names
                  if is_elf(Path(root) / name))


def _patchelf():
    found = shutil.which("patchelf")
    if not found:
        fail("patchelf not found; install it (apt install patchelf).")
    return found


def runpath(path):
    """The RUNPATH (or RPATH) entries of an ELF file."""
    result = subprocess.run([_patchelf(), "--print-rpath", str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        return []
    return [entry for entry in result.stdout.strip().split(":") if entry]


def set_runpath(path, entries):
    """Sets an ELF file's RUNPATH to `entries` (a list), or removes it when empty."""
    path = Path(path)
    writable = os.access(path, os.W_OK)
    if not writable:
        path.chmod(path.stat().st_mode | 0o200)
    command = [_patchelf(), "--set-rpath", ":".join(entries), str(path)] if entries else \
        [_patchelf(), "--remove-rpath", str(path)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"patchelf couldn't change {path}: {result.stderr.strip()}")


def origin_relative(path, folder):
    """`folder` as seen from ELF file `path`: $ORIGIN/<relative path>."""
    relative = os.path.relpath(folder, os.path.dirname(os.path.abspath(path)))
    return "$ORIGIN" if relative == "." else "$ORIGIN/" + relative.replace(os.sep, "/")
