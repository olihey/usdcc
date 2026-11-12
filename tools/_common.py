"""What the scripts in tools/ and tools/ci share: where the repo is, what the
platform calls things, running a command, reading the CMake cache, copying a
tree, and the hashing and GitHub Actions output of the CI scripts.

Standard library only, and Python 3.9 or later: the workflows run some of these
scripts on the runner's own Python, before actions/setup-python.
"""

from __future__ import annotations

import fnmatch
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

WINDOWS = sys.platform == "win32"
MACOS = sys.platform == "darwin"
LINUX = not WINDOWS and not MACOS

_ARM = platform.machine().lower() in ("arm64", "aarch64")

# What differs between platforms, in one place. The Linux and macOS triplets are
# the dynamic ones: vcpkg's own defaults there build static libraries, which
# PySide6 and usdcc's plugins can't share a Qt through.
if WINDOWS:
    PLATFORM = "windows"
    EXE_SUFFIX = ".exe"
    SHARED_LIBRARY_SUFFIX = ".dll"
    VCPKG_TRIPLET = "x64-windows"
    VCPKG_BOOTSTRAP = "bootstrap-vcpkg.bat"
    VCPKG_EXE = "vcpkg.exe"
    ASSET_PLATFORM = "win64"
elif MACOS:
    PLATFORM = "macos"
    EXE_SUFFIX = ""
    SHARED_LIBRARY_SUFFIX = ".dylib"
    VCPKG_TRIPLET = "arm64-osx-dynamic" if _ARM else "x64-osx-dynamic"
    VCPKG_BOOTSTRAP = "bootstrap-vcpkg.sh"
    VCPKG_EXE = "vcpkg"
    ASSET_PLATFORM = "macos-arm64" if _ARM else "macos-x64"
else:
    PLATFORM = "linux"
    EXE_SUFFIX = ""
    SHARED_LIBRARY_SUFFIX = ".so"
    VCPKG_TRIPLET = "arm64-linux-dynamic" if _ARM else "x64-linux-dynamic"
    VCPKG_BOOTSTRAP = "bootstrap-vcpkg.sh"
    VCPKG_EXE = "vcpkg"
    ASSET_PLATFORM = "linux-arm64" if _ARM else "linux-x64"

CONFIGURATIONS = ("Debug", "RelWithDebInfo", "Release", "MinSizeRel")


def fail(message):
    """Stops the script with `message` on stderr and exit code 1."""
    raise SystemExit(message)


def warn(message):
    print(f"WARNING: {message}", file=sys.stderr, flush=True)


def require_windows(script):
    """For the scripts whose folder layout is only worked out for Windows."""
    if not WINDOWS:
        fail(f"{script} doesn't support {PLATFORM} yet: it only knows Windows' file names and folder layout.")


def run(command, **kwargs):
    """Runs `command` (a list), and stops the script if it fails."""
    command = [str(part) for part in command]
    sys.stdout.flush()
    try:
        result = subprocess.run(command, **kwargs)
    except FileNotFoundError:
        fail(f"'{command[0]}' not found; is it installed and on PATH?")
    if result.returncode != 0:
        fail(f"'{' '.join(command)}' failed with exit code {result.returncode}")
    return result


# --- CMake cache --------------------------------------------------------------

def cache_value(build_dir, name):
    """A value from `build_dir`'s CMakeCache.txt, or None."""
    cache = Path(build_dir) / "CMakeCache.txt"
    if not cache.is_file():
        return None
    pattern = re.compile(rf"^{re.escape(name)}:[A-Z]+=(.*)$")
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def cache_on(build_dir, name):
    """Whether a boolean in the CMake cache is on."""
    return (cache_value(build_dir, name) or "") in ("ON", "1", "YES", "TRUE")


def vcpkg_triplet(build_dir):
    """The triplet the build was configured with, else this platform's."""
    return cache_value(build_dir, "VCPKG_TARGET_TRIPLET") or VCPKG_TRIPLET


# --- Files --------------------------------------------------------------------

def _matches(name, patterns):
    # fnmatch ignores case where the file system does.
    return any(fnmatch.fnmatch(name, pattern) for pattern in patterns)


def _make_writable(path):
    os.chmod(path, stat.S_IWRITE | stat.S_IREAD)


def copy_file(source, destination):
    """Copies a file into a folder or onto a file, over a read-only one too."""
    destination = Path(destination)
    if not Path(source).is_file():
        fail(f"Nothing to copy at {source}")
    if destination.is_dir():
        destination = destination / Path(source).name
    try:
        shutil.copy2(source, destination)
    except PermissionError:
        _make_writable(destination)
        shutil.copy2(source, destination)


def copy_tree(source, destination, files="*", exclude_files=(), exclude_dirs=(), top_level_only=False):
    """Copies `source` into `destination`, merging with what is there.

    Takes the files matching `files` but not `exclude_files`, and skips the
    folders named in `exclude_dirs`; both are name patterns, and apply at every
    depth. `top_level_only` leaves the subfolders out.
    """
    source = Path(source)
    destination = Path(destination)
    if not source.is_dir():
        fail(f"Copying {source} to {destination} failed: no such folder")
    destination.mkdir(parents=True, exist_ok=True)
    with os.scandir(source) as entries:
        entries = sorted(entries, key=lambda entry: entry.name)
    for entry in entries:
        if entry.is_dir():
            if not top_level_only and not _matches(entry.name, exclude_dirs):
                copy_tree(entry.path, destination / entry.name, files, exclude_files, exclude_dirs)
        elif fnmatch.fnmatch(entry.name, files) and not _matches(entry.name, exclude_files):
            copy_file(entry.path, destination / entry.name)


def remove_tree(path):
    """Deletes a folder, read-only files included."""

    def retry(function, failed_path, _error):
        _make_writable(failed_path)
        function(failed_path)

    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=retry)
    else:
        shutil.rmtree(path, onerror=retry)


# --- CI: asset names and step outputs -----------------------------------------

def normalized_text(path):
    """A file's text with LF line endings, so a CRLF checkout hashes the same."""
    return Path(path).read_bytes().decode("utf-8-sig", errors="replace").replace("\r\n", "\n")


def short_hash(data, length):
    """The first `length` bytes of `data`'s SHA-256, in hex."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    return hashlib.sha256(data).hexdigest()[: length * 2]


def append_line(path, line):
    with open(path, "a", encoding="utf-8", newline="\n") as file:
        file.write(line + "\n")


def report(result, github_output=True):
    """Prints `result` as JSON, and gives it to the workflow's later steps."""
    if github_output:
        output = os.environ.get("GITHUB_OUTPUT")
        if not output:
            fail("GITHUB_OUTPUT isn't set: this only works in a GitHub Actions step.")
        for key, value in result.items():
            append_line(output, f"{key}={value}")
    print(json.dumps(result, indent=2))
