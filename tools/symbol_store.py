"""Keeps the PDBs and binaries of every usdcc build, so that a crash dump of a
build that has been rebuilt since still gets its names.

    python tools/symbol_store.py archive <list file>   # the build runs this
    python tools/symbol_store.py list                  # what the store holds

A debugger only takes the PDB, and the .exe/.dll it needs to walk a stack
through optimized code, that match the IDs the dump records. A rebuild
overwrites both. So after every build (the usdcc_symbols target, see
cmake/modules/UsdccSymbols.cmake) each binary the list file names, and the
PDB it records, is copied into a store laid out as Microsoft's symbol server
is: <name>/<ID>/<name>. cdb reads it with "srv*<store>", as
tools/analyze_crash.py does. A file already there is not copied again, and
only the newest 5 builds of each file are kept.

The store is %USERPROFILE%\\.usdcc-symbols\\builds, not in AppData: WinDbg from
winget or the Store is a packaged app, and its cdb doesn't see folders other
programs made in AppData. Standard library only, Windows (PE files) only.
"""

from __future__ import annotations

import os
import shutil
import struct
import sys
from pathlib import Path

STORE = Path.home() / ".usdcc-symbols/builds"
MICROSOFT_CACHE = Path.home() / ".usdcc-symbols/microsoft"
KEEP = 5

IMAGE_DEBUG_TYPE_CODEVIEW = 2


def read_pe(binary: Path) -> tuple[str, Path | None, str | None]:
    """The binary's store ID, and the PDB path and store ID it records."""
    with binary.open("rb") as f:

        def at(offset: int, fmt: str):
            f.seek(offset)
            return struct.unpack(fmt, f.read(struct.calcsize(fmt)))

        (pe,) = at(0x3C, "<I")
        if f.seek(pe) is None or f.read(4) != b"PE\0\0":
            raise ValueError(f"{binary} is not a PE file")
        sections, timestamp = at(pe + 6, "<HI")
        (optional_size,) = at(pe + 20, "<H")
        optional = pe + 24
        (magic,) = at(optional, "<H")
        (image_size,) = at(optional + 56, "<I")
        directories = optional + (112 if magic == 0x20B else 96)
        debug_rva, debug_size = at(directories + 6 * 8, "<II")
        image_id = f"{timestamp:08X}{image_size:x}"

        # The debug directory's file offset, through the section holding it.
        debug_offset = None
        for index in range(sections):
            size, address, raw_size, raw_offset = at(optional + optional_size + index * 40 + 8, "<IIII")
            if address <= debug_rva < address + max(size, raw_size):
                debug_offset = debug_rva - address + raw_offset
        if not debug_offset:
            return image_id, None, None

        for entry in range(debug_size // 28):
            kind, data_size, _, data_offset = at(debug_offset + entry * 28 + 12, "<IIII")
            if kind != IMAGE_DEBUG_TYPE_CODEVIEW:
                continue
            f.seek(data_offset)
            data = f.read(data_size)
            if data[:4] != b"RSDS":
                continue
            d1, d2, d3 = struct.unpack("<IHH", data[4:12])
            (age,) = struct.unpack("<I", data[20:24])
            pdb_id = f"{d1:08X}{d2:04X}{d3:04X}{data[12:20].hex().upper()}{age:X}"
            pdb = Path(data[24:].split(b"\0", 1)[0].decode("utf-8", "replace"))
            return image_id, pdb, pdb_id
    return image_id, None, None


def store(source: Path, file_id: str) -> bool:
    """Copies source into the store unless it's there already; True if copied."""
    target = STORE / source.name / file_id / source.name
    if target.exists():
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".partial")
    shutil.copy2(source, partial)
    os.replace(partial, target)
    return True


def prune() -> None:
    for name in STORE.iterdir() if STORE.is_dir() else []:
        builds = sorted((d for d in name.iterdir() if d.is_dir()), key=lambda d: d.stat().st_mtime, reverse=True)
        for old in builds[KEEP:]:
            shutil.rmtree(old, ignore_errors=True)


def archive(list_file: Path) -> None:
    binaries = [Path(line.strip()) for line in list_file.read_text(encoding="utf-8").splitlines() if line.strip()]
    copied = []
    for binary in binaries:
        if not binary.is_file():
            continue  # not built in this configuration
        image_id, pdb, pdb_id = read_pe(binary)
        if store(binary, image_id):
            copied.append(binary.name)
        if pdb and pdb_id and pdb.is_file() and store(pdb, pdb_id):
            copied.append(pdb.name)
    prune()
    if copied:
        print(f"usdcc_symbols: archived {', '.join(copied)} in {STORE}")


def list_store() -> None:
    total = 0
    for name in sorted(STORE.iterdir() if STORE.is_dir() else []):
        builds = [d for d in name.iterdir() if d.is_dir()]
        size = sum(f.stat().st_size for d in builds for f in d.iterdir())
        total += size
        print(f"{name.name:40} {len(builds)} builds  {size / 1e6:8.1f} MB")
    print(f"{'':40} total     {total / 1e6:8.1f} MB in {STORE}")


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "archive":
        archive(Path(sys.argv[2]))
    elif len(sys.argv) == 2 and sys.argv[1] == "list":
        list_store()
    else:
        sys.exit(__doc__.split("\n\n")[1])


if __name__ == "__main__":
    main()
