"""Names the prebuilt pack of a USD plugin that deps/usd-plugins/<name>.json pins.

A pack is a zip built against the prebuilt OpenUSD asset and unzipped over it
(docs/USD_PLUGINS.md). Its name carries a hash of the pin, the patches it lists,
tools/build_usd_plugin.py, this script and the OpenUSD asset's name, so a change
to any of them, a new OpenUSD included, gives a new pack and a stale one is never
used. Line endings are normalised before hashing. The pack is attached to the
OpenUSD asset's own release, so one release holds an OpenUSD and everything
built against it.

Prints the result as JSON, and with --github-output also appends it to
$GITHUB_OUTPUT for later workflow steps:
  pack      the pack's name, as in deps/usd-plugins/<pack>.json
  name      usdplugin-<pack>-<short commit>-<hash>-<platform>-py313
  zip       <name>.zip
  tag      the OpenUSD asset's release
  usd_zip   the OpenUSD asset the pack is built against
  commit, repository, python, license, set

    python tools/ci/usd_plugin_asset.py adobe-fileformats

--matrix prints, instead, the packs CI builds for each platform (those that list
it and aren't in the "local" set) as JSON lists, for a workflow's matrices:
  win64, linux-x64
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import ASSET_PLATFORM, REPO, fail, normalized_text, report, short_hash  # noqa: E402
import usd_asset  # noqa: E402

PINS = REPO / "deps/usd-plugins"
SETS = ("standard", "optional", "local")
_KEYS = {"repository", "commit", "version", "patches", "cmake", "platforms", "license", "set", "check"}
_REQUIRED = {"repository", "commit", "cmake", "platforms", "license", "set"}


def pin_file(pack):
    return PINS / f"{pack}.json"


def load(pack):
    """deps/usd-plugins/<pack>.json, checked: a misspelt key would otherwise be
    ignored without a word."""
    file = pin_file(pack)
    if not file.is_file():
        known = ", ".join(sorted(path.stem for path in PINS.glob("*.json"))) or "none"
        fail(f"No pack '{pack}' ({file} doesn't exist). Packs: {known}")
    spec = json.loads(file.read_text(encoding="utf-8-sig"))
    unknown = sorted(set(spec) - _KEYS)
    if unknown:
        fail(f"{file}: unknown key(s) {', '.join(unknown)}")
    missing = sorted(_REQUIRED - set(spec))
    if missing:
        fail(f"{file}: missing {', '.join(missing)}")
    if spec["set"] not in SETS:
        fail(f"{file}: \"set\" is '{spec['set']}', not one of {', '.join(SETS)}")
    return spec


def describe(pack):
    """The asset the pack's pin describes, as report() takes it."""
    spec = load(pack)
    usd = usd_asset.describe(REPO / "deps/usd.json")
    patches = [PINS / patch for patch in spec.get("patches", [])]

    hashed = f"usd={usd['base']}\n"
    for file in [pin_file(pack), *patches, REPO / "tools/build_usd_plugin.py", Path(__file__)]:
        if not file.is_file():
            fail(f"{pin_file(pack)} lists a patch that doesn't exist: {file}")
        hashed += normalized_text(file)

    python = usd["python"].split(".")
    suffix = f"{ASSET_PLATFORM}-py{python[0]}{python[1]}"
    name = f"usdplugin-{pack}-{spec['commit'][:9]}-{short_hash(hashed, 4)}-{suffix}"
    return {
        "pack": pack,
        "name": name,
        "zip": f"{name}.zip",
        "tag": usd["tag"],
        "usd_zip": usd["zip"],
        "commit": spec["commit"],
        "repository": spec["repository"],
        "python": usd["python"],
        "license": spec["license"],
        "set": spec["set"],
    }


def ci_packs(platform=ASSET_PLATFORM):
    """The packs CI builds for `platform` (win64, linux-x64, ...)."""
    packs = []
    for file in sorted(PINS.glob("*.json")):
        spec = load(file.stem)
        if platform in spec["platforms"] and spec["set"] != "local":
            packs.append(file.stem)
    return packs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("pack", nargs="?", help="the pack to name, as in deps/usd-plugins/<pack>.json")
    parser.add_argument("--matrix", action="store_true",
                        help="list the packs CI builds instead, per platform: win64=[...], linux-x64=[...]")
    parser.add_argument("--github-output", action="store_true", help="also append the result to $GITHUB_OUTPUT")
    args = parser.parse_args(argv)
    if args.matrix:
        report({platform: json.dumps(ci_packs(platform)) for platform in ("win64", "linux-x64")},
               github_output=args.github_output)
    elif args.pack:
        report(describe(args.pack), github_output=args.github_output)
    else:
        parser.error("name a pack, or pass --matrix")


if __name__ == "__main__":
    main()
