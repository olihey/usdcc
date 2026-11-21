"""Names the prebuilt OpenUSD release asset that deps/usd.json describes.

The name carries a hash of deps/usd.json and every patch it lists, so any change
to the pin, the flags or a patch gives a new asset, and a stale one is never used
(docs/CI.md section 2). Line endings are normalised before hashing, so a CRLF
checkout gives the same name as an LF one.

The platform in the name is the one this runs on (win64, linux-x64, ...), so each
platform's zip sits in the same release, under the same tag. deps/usd.json's own
"platform" is the Windows build's, which named the assets before there were others.

Prints the result as JSON, and with --github-output also appends it to
$GITHUB_OUTPUT for later workflow steps:
  name    usd-<short commit>-<hash>-<platform>-py313
  base    usd-<short commit>-<hash>, what every platform's asset shares
  tag     deps/<base>
  zip     <name>.zip
  commit, repository, python, patches (;-separated)
  flags   build_usd.py's flags on this platform (space-separated)

    python tools/ci/usd_asset.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import ASSET_PLATFORM, REPO, VS_ARCH, VS_GENERATOR, WINDOWS, fail, normalized_text, report, short_hash  # noqa: E402


def platform_flags(flags):
    """deps/usd.json's build_usd.py flags for this platform: without the Windows-only
    --generator elsewhere, where build_usd.py picks the platform's own. On Windows
    ARM64 the generator is VS_GENERATOR's (Visual Studio 2026), and --onetbb is
    added, as build_usd.py refuses classic TBB there."""
    if WINDOWS and VS_ARCH == "ARM64":
        return [flag for flag in flags if not flag.startswith("--generator")] + [f"--generator={VS_GENERATOR}", "--onetbb"]
    if WINDOWS:
        return list(flags)
    return [flag for flag in flags if not flag.startswith("--generator")]


def describe(pin):
    """The asset `pin` (a deps/usd.json) describes, as report() takes it."""
    pin = Path(pin).resolve()
    spec = json.loads(pin.read_text(encoding="utf-8-sig"))
    patches = [pin.parent / patch for patch in spec.get("patches", [])]

    hashed = b""
    for file in [pin] + patches:
        if not file.is_file():
            fail(f"deps/usd.json lists a patch that doesn't exist: {file}")
        hashed += normalized_text(file).encode("utf-8")

    python = spec["python"].split(".")
    base = f"usd-{spec['commit'][:9]}-{short_hash(hashed, 4)}"
    name = f"{base}-{ASSET_PLATFORM}-py{python[0]}{python[1]}"
    return {
        "name": name,
        "base": base,
        "tag": f"deps/{base}",
        "zip": f"{name}.zip",
        "commit": spec["commit"],
        "repository": spec["repository"],
        "python": spec["python"],
        "flags": " ".join(platform_flags(spec["flags"])),
        "patches": ";".join(patch.as_posix() for patch in patches),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pin", default=REPO / "deps/usd.json", help="the pin to name (default: deps/usd.json)")
    parser.add_argument("--github-output", action="store_true", help="also append the result to $GITHUB_OUTPUT")
    args = parser.parse_args(argv)
    report(describe(args.pin), github_output=args.github_output)


if __name__ == "__main__":
    main()
