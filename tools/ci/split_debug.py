"""Moves the debug info of usdcc's own Linux binaries in a deploy into a folder of its own.

The Linux counterpart of release.yml's PDB zip on Windows (docs/RELEASE.md §4):
a RelWithDebInfo build carries its debug info inside the binaries, which would
make the release several times bigger. For each of usdcc's own ELF files in the
deploy (bin/usdcc, bin/usdcc_node, lib/libusdcc_*.so, the usdcc Python package's modules, the
example plugins' libraries) this writes <symbols>/<path in the deploy>.debug,
strips the debug info from the deployed file (its symbol table stays), and leaves
a .gnu_debuglink in it naming the .debug file. gdb finds that next to the binary,
so unpacking the symbols over the deploy is all it takes. Other libraries (Qt,
OpenUSD) are left as they are.

    python3 tools/ci/split_debug.py --deploy ~/usdcc-0.2.0 --symbols ~/usdcc-0.2.0-symbols

Uses objcopy (binutils). Standard library only.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import fail  # noqa: E402
from _elf import is_elf  # noqa: E402

# usdcc's own, by path in the deploy.
OWN = ["bin/usdcc", "bin/usdcc_node", "lib/libusdcc_*.so", "lib/python*/site-packages/usdcc/*.so", "examples/plugins/*/bin/*/*.so",
       "plugins/*/bin/*/*.so"]


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"{' '.join(str(part) for part in command)} failed: {result.stderr.strip()}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--deploy", required=True, type=Path, help="the deploy (tools/deploy.py)")
    parser.add_argument("--symbols", required=True, type=Path, help="where the .debug files go")
    args = parser.parse_args(argv)
    if not shutil.which("objcopy"):
        fail("objcopy not found; install binutils.")
    deploy = args.deploy.resolve()
    symbols = args.symbols.resolve()

    files = sorted({file for pattern in OWN for file in deploy.glob(pattern) if is_elf(file)})
    if not files:
        fail(f"None of usdcc's binaries in {deploy}.")
    before = after = 0
    for file in files:
        relative = file.relative_to(deploy)
        debug = symbols / relative.parent / (file.name + ".debug")
        debug.parent.mkdir(parents=True, exist_ok=True)
        before += file.stat().st_size
        mode = file.stat().st_mode
        file.chmod(mode | 0o200)
        run(["objcopy", "--only-keep-debug", "--compress-debug-sections", file, debug])
        # The symbol table stays: a backtrace without the symbols still names functions.
        run(["objcopy", "--strip-debug", file])
        # The link holds the file name and a CRC; gdb looks for it next to the binary.
        run(["objcopy", f"--add-gnu-debuglink={debug}", file])
        file.chmod(mode)
        after += file.stat().st_size
        print(f"  {relative}: {os.path.getsize(debug) / 2**20:.1f} MB of debug info")
    print(f"{len(files)} binaries: {before / 2**20:,.0f} MB -> {after / 2**20:,.0f} MB; symbols in {symbols}")


if __name__ == "__main__":
    main()
