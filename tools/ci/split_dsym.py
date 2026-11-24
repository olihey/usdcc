"""Makes dSYMs of usdcc's own macOS binaries in a deploy, in a folder of its own.

The macOS counterpart of split_debug.py. There the debug info stays in the build's object
files, which the binaries name: for each of usdcc's own Mach-O files in the deploy, dsymutil
collects it into <symbols>/<path in the deploy>.dSYM. Then the names of the object files,
which are this machine's, are stripped from the deployed file (its symbol table stays), so
it is signed again: run this before anything is made of the deploy, and tools/deploy.py's
--sign for the identity. lldb finds a dSYM by its UUID, next to the binary or through Spotlight.

    python3 tools/ci/split_dsym.py --deploy ~/usdcc-0.2.0 --symbols ~/usdcc-0.2.0-symbols

Needs the build the deploy was made from. Uses dsymutil and strip (Xcode). Standard library only.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import fail  # noqa: E402
from _macho import is_macho, sign, sign_app  # noqa: E402

# usdcc's own, by path in the bundle's Contents.
OWN = ["MacOS/usdcc", "MacOS/usdcc_node", "Resources/lib/libusdcc_*.dylib",
       "Resources/lib/python*/site-packages/usdcc/*.so", "Resources/examples/plugins/*/bin/*/*.so",
       "Resources/plugins/*/bin/*/*.so", "Resources/plugins/*/usd/*.dylib"]


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"{' '.join(str(part) for part in command)} failed: {result.stderr.strip()}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--deploy", required=True, type=Path, help="the deploy (tools/deploy.py)")
    parser.add_argument("--symbols", required=True, type=Path, help="where the dSYMs go")
    parser.add_argument("--sign", default="-", metavar="IDENTITY", help="as tools/deploy.py's (default: ad hoc)")
    args = parser.parse_args(argv)
    app = args.deploy.resolve() / "usdcc.app"
    contents = app / "Contents"
    symbols = args.symbols.resolve()

    files = sorted({file for pattern in OWN for file in contents.glob(pattern) if is_macho(file)})
    if not files:
        fail(f"None of usdcc's binaries in {app}.")
    before = after = 0
    for file in files:
        relative = file.relative_to(args.deploy.resolve())
        dsym = symbols / relative.parent / (file.name + ".dSYM")
        dsym.parent.mkdir(parents=True, exist_ok=True)
        before += file.stat().st_size
        run(["dsymutil", file, "-o", dsym])
        run(["strip", "-S", file])
        sign(file, args.sign)
        after += file.stat().st_size
        size = sum(part.stat().st_size for part in dsym.rglob("*") if part.is_file())
        print(f"  {relative}: {size / 2**20:.1f} MB of debug info")
        if size < 4096:
            fail(f"{relative} has no debug info: is this the build's machine, with its object files?")
    sign_app(app, args.sign)
    print(f"{len(files)} binaries: {before / 2**20:,.0f} MB -> {after / 2**20:,.0f} MB; dSYMs in {symbols}")


if __name__ == "__main__":
    main()
