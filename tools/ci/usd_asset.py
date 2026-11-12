"""Names the prebuilt OpenUSD release asset that deps/usd.json describes.

The name carries a hash of deps/usd.json and every patch it lists, so any change
to the pin, the flags or a patch gives a new asset, and a stale one is never used
(docs/CI.md section 2). Line endings are normalised before hashing, so a CRLF
checkout gives the same name as an LF one.

Prints the result as JSON, and with --github-output also appends it to
$GITHUB_OUTPUT for later workflow steps:
  name    usd-<short commit>-<hash>-win64-py312
  tag     deps/usd-<short commit>-<hash>
  zip     <name>.zip
  commit, repository, python, flags (space-separated), patches (;-separated)

    python tools/ci/usd_asset.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from _common import REPO, fail, normalized_text, report, short_hash  # noqa: E402


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
    name = f"{base}-{spec['platform']}-py{python[0]}{python[1]}"
    return {
        "name": name,
        "tag": f"deps/{base}",
        "zip": f"{name}.zip",
        "commit": spec["commit"],
        "repository": spec["repository"],
        "python": spec["python"],
        "flags": " ".join(spec["flags"]),
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
