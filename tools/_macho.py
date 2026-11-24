"""Mach-O files on macOS: telling them apart and pointing their rpaths somewhere.

What tools/_elf.py is for Linux. A build has absolute rpaths (CMake's build tree,
build_usd.py's install folder), and some libraries absolute install names, which
whatever links them repeats. These are rewritten relative to @loader_path, so the
files load from wherever they are. Uses otool, install_name_tool and codesign (Xcode or
the Command Line Tools). Every change is followed by an ad-hoc signature: Apple Silicon
doesn't run code whose signature no longer fits. Standard library only.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

from _common import fail

# Thin 64-bit, and universal (fat) files. The latter's number is Java's class files' too,
# which have their version, 45 or more, where a fat file has its few architectures.
_THIN = (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf")
_FAT = (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf")


def is_macho(path):
    """Whether `path` is a regular file that is a Mach-O binary (not a symlink)."""
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with open(path, "rb") as file:
            head = file.read(8)
    except OSError:
        return False
    if head[:4] in _THIN:
        return True
    return head[:4] in _FAT and int.from_bytes(head[4:8], "big") < 20


def _is_linked(path):
    """Whether `path` is something the loader loads, not an object file or a static library."""
    kind = _otool("-hv", path)
    return any(name in kind for name in ("EXECUTE", "DYLIB", "BUNDLE"))


def macho_files(folder):
    """The programs, libraries and modules under `folder`, sorted."""
    return sorted(Path(root) / name for root, _, names in os.walk(folder) for name in names
                  if is_macho(Path(root) / name) and _is_linked(Path(root) / name))


def _otool(option, path):
    result = subprocess.run(["otool", option, str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"otool {option} couldn't read {path}: {result.stderr.strip()}")
    return result.stdout


def _load_commands(path):
    """(command, value) of each load command with a path, once each (a universal file
    has them per architecture)."""
    found, command = [], None
    for line in _otool("-l", path).splitlines():
        words = line.split(None, 1)
        if len(words) < 2:
            continue
        if words[0] == "cmd":
            command = words[1]
        elif words[0] in ("path", "name") and command:
            entry = (command, words[1].rsplit(" (offset", 1)[0])
            if entry not in found:
                found.append(entry)
    return found


def rpaths(path):
    """The rpath entries of a Mach-O file."""
    return [value for command, value in _load_commands(path) if command == "LC_RPATH"]


def install_id(path):
    """A library's own install name, or None."""
    return next((value for command, value in _load_commands(path) if command == "LC_ID_DYLIB"), None)


def needed(path):
    """What a Mach-O file links, as it names it: "@rpath/libx.dylib", "/usr/lib/libSystem.B.dylib"."""
    return [value for command, value in _load_commands(path)
            if command in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LAZY_LOAD_DYLIB")]


def is_system(name):
    """Whether an install name is the system's, which every Mac has."""
    return name.startswith(("/usr/lib/", "/System/Library/"))


def edit(path, new_rpaths=None, new_id=None, changes=None):
    """Sets a file's rpaths to `new_rpaths` (a list, or None: as they are), its install
    name to `new_id`, and what it links from the keys of `changes` to their values; then
    signs it ad hoc. Nothing to change leaves the file alone. Returns whether it changed."""
    path = Path(path)
    removals, arguments = [], []
    if new_rpaths is not None and rpaths(path) != list(new_rpaths):
        for entry in rpaths(path):
            removals += ["-delete_rpath", entry]
        for entry in new_rpaths:
            arguments += ["-add_rpath", entry]
    if new_id is not None and install_id(path) not in (None, new_id):
        arguments += ["-id", new_id]
    links = needed(path)
    for old, new in (changes or {}).items():
        if old in links and old != new:
            arguments += ["-change", old, new]
    if not removals and not arguments:
        return False
    # An entry removed and added again can't go in one call.
    for step in (removals, arguments):
        if step:
            _change(path, step)
    sign(path)
    return True


def _change(path, arguments):
    if not os.access(path, os.W_OK):
        path.chmod(path.stat().st_mode | 0o200)
    result = subprocess.run(["install_name_tool", *arguments, str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"install_name_tool couldn't change {path}: {result.stderr.strip()}")


def sign(path):
    """An ad-hoc signature, over whatever one there is."""
    result = subprocess.run(["codesign", "--force", "--sign", "-", str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"codesign couldn't sign {path}: {result.stderr.strip()}")


def loader_relative(path, folder):
    """`folder` as seen from Mach-O file `path`: @loader_path/<relative path>."""
    relative = os.path.relpath(folder, os.path.dirname(os.path.abspath(path)))
    return "@loader_path" if relative == "." else "@loader_path/" + relative
