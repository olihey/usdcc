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

On Linux the libraries, plugins and Python modules have RUNPATHs naming the
install folder too (OpenImageIO's has nothing else). Those entries become
$ORIGIN-relative, and each library in lib/ gets $ORIGIN itself, so they find each
other wherever the install is unpacked. That needs patchelf. On macOS the same goes for
their rpaths, with @loader_path, and for install names in the install folder, which
become @rpath ones. So does libpython's, outside the install, with the folder it was built
with as the last rpath: any program that names its Python's folder in an rpath, as Python
itself does, finds its own there.

Then it checks that no CMake file still names the install folder, and that the
Python folder is left only in pxrConfig.cmake's guarded lines, and on Linux and macOS that
no RUNPATH, rpath or install name names the install folder. Any leftover fails the script.

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
from _common import MACOS, WINDOWS, fail  # noqa: E402

if MACOS:
    from _macho import edit, install_id, loader_relative, macho_files, needed, rpaths  # noqa: E402
elif not WINDOWS:
    from _elf import elf_files, origin_relative, runpath, set_runpath  # noqa: E402

MARKER = "# Made relocatable by usdcc tools/ci/relocate_usd.py"
# What this script wrote while it was relocate-usd.ps1.
MARKERS = (MARKER, "# Made relocatable by usdcc tools/ci/relocate-usd.ps1")
# libpython as macOS names it.
PYTHON_LIBRARY = re.compile(r"/libpython3[.0-9]*[.]dylib$")
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


def relocate_runpaths(install, built_at, leftovers):
    """Linux: RUNPATH entries in the install folder become $ORIGIN-relative."""
    rewritten = 0
    files = [file for top in ("lib", "plugin", "bin") if os.path.isdir(os.path.join(install, top))
             for file in elf_files(os.path.join(install, top))]
    lib = os.path.join(install, "lib")
    for file in files:
        entries = runpath(file)
        relocated = []
        for entry in entries:
            if entry == built_at or entry.startswith(built_at + "/"):
                entry = origin_relative(file, install + entry[len(built_at):])
            if entry not in relocated:
                relocated.append(entry)
        if os.path.samefile(os.path.dirname(file), lib) and "$ORIGIN" not in relocated:
            relocated.insert(0, "$ORIGIN")
        if relocated != entries:
            set_runpath(file, relocated)
            rewritten += 1
        for entry in runpath(file):
            if entry == built_at or entry.startswith(built_at + "/"):
                leftovers.append(f"{os.path.relpath(file, install)}: RUNPATH {entry}")
    print(f"Rewrote the RUNPATH of {rewritten} of {len(files)} ELF file(s).")


def relocate_rpaths(install, built_at, leftovers):
    """macOS: rpaths in the install folder become @loader_path-relative, and install names
    and links in it @rpath ones."""
    def inside(entry):
        return entry == built_at or entry.startswith(built_at + "/")

    rewritten = 0
    files = [file for top in ("lib", "plugin", "bin") if os.path.isdir(os.path.join(install, top))
             for file in macho_files(os.path.join(install, top))]
    lib = os.path.join(install, "lib")
    for file in files:
        entries = []
        for entry in rpaths(file):
            entry = loader_relative(file, install + entry[len(built_at):]) if inside(entry) else entry
            entry = "@loader_path" if entry in ("@loader_path/", "@loader_path/.") else entry
            if entry not in entries:
                entries.append(entry)
        links = {name: "@rpath/" + os.path.basename(name) for name in needed(file) if inside(name)}
        # libpython, wherever this machine has it.
        pythons = [name for name in needed(file) if PYTHON_LIBRARY.search(name) and name.startswith("/")]
        links.update({name: "@rpath/" + os.path.basename(name) for name in pythons})
        own = install_id(file)
        if links or any(name.startswith("@rpath/") for name in needed(file)):
            for folder in (os.path.dirname(file), lib):
                if loader_relative(file, folder) not in entries:
                    entries.append(loader_relative(file, folder))
        entries += [folder for folder in map(os.path.dirname, pythons) if folder not in entries]
        if edit(file, new_rpaths=entries, new_id="@rpath/" + os.path.basename(own) if own and inside(own) else None, changes=links):
            rewritten += 1
        for entry in [*rpaths(file), *needed(file), install_id(file) or ""]:
            if inside(entry):
                leftovers.append(f"{os.path.relpath(file, install)}: {entry}")
    print(f"Rewrote the paths of {rewritten} of {len(files)} Mach-O file(s).")


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

    if MACOS:
        relocate_rpaths(install, built_at, leftovers)
    elif not WINDOWS:
        relocate_runpaths(install, built_at, leftovers)

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
