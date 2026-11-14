"""Reads a crash dump of usdcc (or of a test program) and prints what crashed.

    python tools/analyze_crash.py                # the app's newest dump
    python tools/analyze_crash.py --list         # all usdcc*.dmp, test programs' too
    python tools/analyze_crash.py <dump file or pid> [--threads] [--raw]

Windows writes a dump of every crashing program to %LOCALAPPDATA%\\CrashDumps
(Windows Error Reporting's "LocalDumps", on by default; it keeps the last 10).
This runs cdb, the console debugger that comes with WinDbg
(winget install Microsoft.WinDbg), over one of them and prints:

- the exception and the crashing thread's stack, named from the PDBs the
  binaries record (usdcc's own, when that build still has them) and from
  Microsoft's symbol server (Windows, and names exported by OpenUSD and Qt);
- when that stack can't be walked (code without unwind data, such as a
  driver's generated code), the return addresses found on it;
- the main thread's stack, when another thread crashed;
- which usdcc modules had no matching PDB, so their frames are only offsets;
- the end of the log of the same run, matched by the process ID that both
  file names carry, while the log is still there (the app keeps 10 runs).

--threads adds every thread's stack, --raw prints cdb's whole output instead.
Symbols come from the builds tools/symbol_store.py archived, so a dump of a
build that has been rebuilt since still gets names, then from where the
binaries say their PDBs are, then from Microsoft (cached in
%USERPROFILE%\\.usdcc-symbols\\microsoft). _NT_SYMBOL_PATH goes first.
Windows x64 only.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from symbol_store import MICROSOFT_CACHE, STORE

LOCAL = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
DUMPS = LOCAL / "CrashDumps"
LOGS = LOCAL / "usdcc/logs"


def find_cdb() -> str:
    candidates = [os.environ.get("USDCC_CDB"), shutil.which("cdb")]
    # WinDbg from winget or the Store is a package in a versioned WindowsApps
    # folder, which a user can't list but can run from.
    package = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-AppxPackage Microsoft.WinDbg | Select-Object -First 1).InstallLocation"],
                             check=False, capture_output=True, text=True).stdout.strip()
    if package:
        candidates.append(str(Path(package) / "amd64/cdb.exe"))
    candidates.append(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe")
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    sys.exit("No cdb found. Install WinDbg (winget install Microsoft.WinDbg), or set USDCC_CDB to a cdb.exe.")


def all_dumps() -> list[Path]:
    return sorted(DUMPS.glob("usdcc*.dmp"), key=lambda p: p.stat().st_mtime, reverse=True)


def pick_dump(which: str | None) -> Path:
    if which and Path(which).is_file():
        return Path(which)
    dumps = all_dumps()
    if which:
        dumps = [d for d in dumps if d.name.endswith(f".{which}.dmp")]
    else:  # the app's, not a test program's
        dumps = [d for d in dumps if d.name.lower().startswith("usdcc.exe.")]
    if not dumps:
        sys.exit(f"No usdcc dump{' for process ' + which if which else ''} in {DUMPS}.")
    return dumps[0]


def symbol_path() -> str:
    paths = [os.environ.get("_NT_SYMBOL_PATH", ""), f"srv*{STORE}",
             f"srv*{MICROSOFT_CACHE}*https://msdl.microsoft.com/download/symbols"]
    return ";".join(path for path in paths if path)


def run_cdb(cdb: str, dump: Path, threads: bool) -> str:
    def section(name: str) -> str:
        return f".echo ####{name}"

    commands = [
        ".lines -e",
        section("exception"), ".exr -1",
        section("stack"), ".ecxr", "kn 50",
        section("scan"), ".ecxr", "dps @rsp L300",
        section("main"), "~0 kn 40",
        section("thread"), "~.",
        section("modules"), "ld *usdcc*", "lm m *usdcc*",
    ]
    if threads:
        commands += [section("threads"), "~*kn 40"]
    commands.append("q")
    command = [cdb, "-z", str(dump), "-y", symbol_path(), "-i", f"srv*{STORE}", "-c", "; ".join(commands)]
    result = subprocess.run(command,
                            check=False, capture_output=True, text=True, errors="replace")
    return result.stdout + result.stderr


def sections(output: str) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {"header": []}
    current = "header"
    for line in output.splitlines():
        # The memory map progress cdb prints is noise.
        line = re.sub(r"Building memory map: [0-9a-f`]+", "", line).rstrip()
        marker = re.search(r"####(\w+)", line)
        if marker:
            current = marker.group(1)
            found[current] = []
        elif line and not line.startswith(("NatVis script", "*** WARNING")):
            found[current].append(line)
    return found


def frames(lines: list[str]) -> list[str]:
    return [line for line in lines if re.match(r"^[0-9a-f]{2} [0-9a-f]{8}`[0-9a-f]{8} ", line)]


def print_block(title: str, lines: list[str]) -> None:
    print(f"\n== {title}")
    for line in lines:
        print("  " + line)


def report(dump: Path, output: str) -> None:
    parts = sections(output)
    header = parts["header"]
    print(f"Dump: {dump}")
    for line in header:
        if line.startswith(("Debug session time", "Process Uptime", "User Mini Dump", "User Full Dump")):
            print("  " + line)
    crashed = next((line for line in header if re.match(r"^\([0-9a-f]+\.[0-9a-f]+\): ", line)), None)
    if crashed:
        print("  " + crashed)

    print_block("Exception", parts.get("exception", [])[:8])

    stack = frames(parts.get("stack", []))
    print_block("Crashing thread", stack or ["(no frames)"])
    # Fewer than three frames: the walk stopped at code it has no unwind data
    # for. The return addresses still on the stack say who called it.
    if len(stack) < 3:
        seen, candidates = set(), []
        for line in parts.get("scan", []):
            match = re.match(r"^[0-9a-f]{8}`[0-9a-f]{8}  [0-9a-f]{8}`[0-9a-f]{8} (\S+!\S+)", line)
            if match and match.group(1) not in seen:
                seen.add(match.group(1))
                candidates.append(match.group(1))
        print_block("Return addresses on its stack (a scan: some may be stale)", candidates[:40] or ["(none)"])

    thread = " ".join(parts.get("thread", []))
    if not re.match(r"^\.\s+0\s+Id:", thread.strip()):
        print_block("Main thread", frames(parts.get("main", [])) or ["(no frames)"])

    if "threads" in parts:
        print_block("All threads", parts["threads"])

    modules = [line for line in parts.get("modules", []) if re.match(r"^[0-9a-f]{8}`", line)]
    unnamed = [line.split()[2] for line in modules if "pdb symbols" not in line and "private pdb" not in line]
    if unnamed:
        print(f"\n== No matching PDB for: {', '.join(unnamed)}")
        print("  Their frames are offsets only: that build made no PDBs (Release made none before"
              " November 2026), or it was rebuilt before tools/symbol_store.py archived it.")

    print_log(dump)


def print_log(dump: Path) -> None:
    match = re.search(r"\.(\d+)\.dmp$", dump.name)
    if not match or not dump.name.lower().startswith("usdcc.exe"):
        return
    logs = sorted(LOGS.glob(f"usdcc-*-{match.group(1)}.log"))
    if not logs:
        print(f"\n== Log: none for process {match.group(1)} in {LOGS} (the app keeps the last 10 runs)")
        return
    lines = logs[-1].read_text(encoding="utf-8", errors="replace").splitlines()
    print_block(f"End of the log {logs[-1]}", lines[-40:])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dump", nargs="?", help="a .dmp file, or the process ID in a dump's name (default: the newest)")
    parser.add_argument("--list", action="store_true", help="list the usdcc dumps, newest first")
    parser.add_argument("--threads", action="store_true", help="also print every thread's stack")
    parser.add_argument("--raw", action="store_true", help="print cdb's whole output")
    args = parser.parse_args()

    if args.list:
        import datetime
        for dump in all_dumps():
            when = datetime.datetime.fromtimestamp(dump.stat().st_mtime)
            print(f"{when:%Y-%m-%d %H:%M}  {dump.stat().st_size / 1e6:6.1f} MB  {dump}")
        return

    dump = pick_dump(args.dump)
    output = run_cdb(find_cdb(), dump, args.threads)
    if args.raw:
        print(output)
    else:
        report(dump, output)


if __name__ == "__main__":
    main()
