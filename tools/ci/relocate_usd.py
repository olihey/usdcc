"""Makes an OpenUSD install's CMake files work from any folder.

build_usd.py writes absolute paths into some CMake files: the install folder
itself (pxrConfig.cmake's TBB target, MaterialX_DIR, Imath_DIR; pxrTargets.cmake;
the boost and OpenImageIO package files) and the Python it was built with. So an
install only works where it was built. This rewrites them (docs/CI.md section 2):

  - The install folder becomes ${_usdcc_usd_prefix}, which a line added at the
    top of each changed file works out from the file's own folder. A bracket
    argument ([[...]], which CMake doesn't expand) holding it becomes a quoted
    string.
  - pxrConfig.cmake only uses its stored Python paths when they exist on this
    machine; otherwise CMake finds Python itself.
  - The Python include folder in other files becomes ${Python3_INCLUDE_DIRS},
    which pxrConfig.cmake's find_dependency(Python3) has set by then.

Then it checks that no CMake file still names the install folder, and that the
Python folder is left only in pxrConfig.cmake's guarded lines. Any leftover fails
the script.

Running it twice is harmless: the second run finds nothing to change.

    python tools/ci/relocate_usd.py --install C:\\deps\\usd

--built-at is the folder the install was built into, if it has been moved since.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import WINDOWS, fail  # noqa: E402

MARKER = "# Made relocatable by usdcc tools/ci/relocate_usd.py"
# What this script wrote while it was relocate-usd.ps1.
MARKERS = (MARKER, "# Made relocatable by usdcc tools/ci/relocate-usd.ps1")
PREFIX = "${_usdcc_usd_prefix}"

# Windows' file names don't tell upper case from lower.
FLAGS = re.IGNORECASE if WINDOWS else 0


def path_pattern(path):
    """`path` as a regular expression that takes either slash."""
    return r"[\\/]".join(re.escape(part) for part in re.split(r"[\\/]", path))


def cmake_files(install):
    """The install's CMake files, without build_usd.py's own src and build folders."""
    found = []
    for folder, folders, files in os.walk(install):
        if os.path.samefile(folder, install):
            folders[:] = [name for name in folders if name.lower() not in ("src", "build")]
        folders.sort()
        found += [os.path.join(folder, name) for name in sorted(files) if name.lower().endswith(".cmake")]
    return found


def read(file):
    return Path(file).read_bytes().decode("utf-8-sig", errors="replace")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--install", required=True, help="the OpenUSD install folder (holding pxrConfig.cmake)")
    parser.add_argument("--built-at", help="the folder it was built into, if moved since (default: --install)")
    args = parser.parse_args(argv)

    install = os.path.abspath(args.install).rstrip("\\/")
    built_at = (args.built_at or install).rstrip("\\/")
    pxr_config = os.path.join(install, "pxrConfig.cmake")
    if not os.path.isfile(pxr_config):
        fail(f"No pxrConfig.cmake in {install}")

    # The Python OpenUSD was built with, from pxrConfig.cmake: its folder on
    # Windows, where the headers are in its Include folder. Elsewhere they are
    # somewhere of their own, which pxrConfig.cmake names too.
    config_text = read(pxr_config)
    python = None
    include = None
    match = re.search(r"set\(Python3_EXECUTABLE \[\[(.+?)\]\]\)", config_text)
    if match and WINDOWS:
        python = path_pattern(re.split(r"[\\/][^\\/]*$", match.group(1))[0].rstrip("\\/"))
        include = python + r"[\\/]Include"
    elif match:
        match = re.search(r"set\(Python3_INCLUDE_DIR \[\[(.+?)\]\]\)", config_text)
        if match:
            python = include = path_pattern(match.group(1).rstrip("/"))
    prefix = path_pattern(built_at)

    files = cmake_files(install)
    changed = []
    for file in files:
        is_config = os.path.normcase(file) == os.path.normcase(pxr_config)
        original = text = read(file)

        # [[<prefix>rest]] -> "${_usdcc_usd_prefix}rest" (forward slashes: a
        # quoted CMake string treats a backslash as an escape).
        text = re.sub(r"\[\[" + prefix + r"([^\]]*)\]\]",
                      lambda found: '"' + PREFIX + found.group(1).replace("\\", "/") + '"', text, flags=FLAGS)
        text = re.sub(prefix, lambda found: PREFIX, text, flags=FLAGS)

        if python and is_config and WINDOWS:
            # if (NOT [[<python>...]] STREQUAL "") -> if (EXISTS [[<python>...]])
            text = re.sub(r"if \(NOT (\[\[" + python + r'[^\]]*\]\]) STREQUAL ""\)',
                          lambda found: f"if (EXISTS {found.group(1)})", text, flags=FLAGS)
        elif python and is_config:
            # The same, for each stored Python path wherever it is.
            text = re.sub(r'if \(NOT (\[\[[^\]]*\]\]) STREQUAL ""\)(\s*set\(Python3_\w+ \[\[)',
                          lambda found: f"if (EXISTS {found.group(1)}){found.group(2)}", text)
        elif python:
            text = re.sub(include, lambda found: "${Python3_INCLUDE_DIRS}", text, flags=FLAGS)

        if text != original:
            if PREFIX in text and not text.startswith(MARKERS):
                relative = os.path.relpath(install, os.path.dirname(file)).replace("\\", "/")
                text = (MARKER + "\n" + 'get_filename_component(_usdcc_usd_prefix "${CMAKE_CURRENT_LIST_DIR}/'
                        + relative + '" ABSOLUTE)\n' + text)
            Path(file).write_bytes(text.encode("utf-8"))
            changed.append(os.path.relpath(file, install))

    # Check: nothing names the build folder any more, and the Python folder only
    # survives in pxrConfig.cmake's EXISTS-guarded lines.
    leftovers = []
    for file in files:
        is_config = os.path.normcase(file) == os.path.normcase(pxr_config)
        for number, line in enumerate(re.split(r"\r\n|\n|\r", read(file)), start=1):
            bad = bool(re.search(prefix, line, FLAGS))
            if not bad and python and re.search(python, line, FLAGS):
                guarded = is_config and re.search(r"^\s*(if \(EXISTS|set\(Python3_\w+) \[\[", line, re.IGNORECASE)
                bad = not guarded
            if bad:
                leftovers.append(f"{os.path.relpath(file, install)}:{number}: {line.strip()}")

    print(f"Rewrote {len(changed)} file(s):")
    for name in changed:
        print(f"  {name}")
    if leftovers:
        print("Absolute paths left:")
        for leftover in leftovers:
            print(f"  {leftover}")
        fail(f"{len(leftovers)} absolute path(s) left in the install's CMake files")
    print("No absolute build paths left.")


if __name__ == "__main__":
    main()
