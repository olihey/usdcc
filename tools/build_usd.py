"""Builds OpenUSD as deps/usd.json pins it, on this machine.

The steps .github/workflows/deps-usd.yml runs, as one script: fetches the pinned
commit, applies deps/patches, runs OpenUSD's build_usd.py with the pin's flags,
and collects the dependencies' licences into <install>/licenses. Then pass the
install folder to tools/build.py with --usd-install.

    python3 tools/build_usd.py ~/deps/usd
    python3 tools/build_usd.py ~/deps/usd --work ~/deps/usd-work --jobs 24

deps/usd.json describes the Windows build that CI publishes. Elsewhere this drops
its --generator flag (build_usd.py picks the platform's own) and builds with the
Python that runs this script, which only has to match the pin's minor version
(docs/LINUX_WSL.md). A second run reuses build_usd.py's source and build
folders, so it only builds what's missing.

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "ci"))
from _common import MACOS, REPO, fail, run  # noqa: E402
from usd_asset import platform_flags, platform_patches  # noqa: E402


def fetch(spec, pin_dir, source):
    """Checks out the pinned commit into `source` and applies the patches once."""
    if not (source / ".git").is_dir():
        source.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q", source])
        run(["git", "-C", source, "config", "core.autocrlf", "false"])
        run(["git", "-C", source, "remote", "add", "origin", spec["repository"]])
    head = subprocess.run(["git", "-C", source, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != spec["commit"]:
        run(["git", "-C", source, "fetch", "--depth", "1", "origin", spec["commit"]])
        run(["git", "-C", source, "checkout", "-q", "--force", "FETCH_HEAD"])
    for patch in [(pin_dir / name).resolve() for name in spec.get("patches", [])] + platform_patches(pin_dir):
        applied = subprocess.run(["git", "-C", source, "apply", "--reverse", "--check", patch],
                                 capture_output=True).returncode == 0
        if applied:
            print(f"{patch.name}: already applied")
        else:
            run(["git", "-C", source, "apply", "--verbose", patch])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("install", type=Path, help="where OpenUSD gets installed (then usdcc's --usd-install)")
    parser.add_argument("--work", type=Path,
                        help="for OpenUSD's checkout and build_usd.py's source and build folders "
                             "(default: <install>-work)")
    parser.add_argument("--pin", type=Path, default=REPO / "deps/usd.json", help="default: deps/usd.json")
    parser.add_argument("--jobs", type=int, help="parallel jobs (default: build_usd.py's, the core count)")
    args = parser.parse_args(argv)

    install = args.install.expanduser().resolve()
    work = (args.work.expanduser() if args.work else install.with_name(install.name + "-work")).resolve()
    pin = args.pin.resolve()
    spec = json.loads(pin.read_text(encoding="utf-8-sig"))

    wanted = spec["python"].split(".")[:2]
    if [str(part) for part in sys.version_info[:2]] != wanted:
        fail(f"deps/usd.json pins Python {spec['python']}; run this with a Python {'.'.join(wanted)} "
             f"(this is {sys.version.split()[0]} at {sys.executable})")

    source = work / "OpenUSD"
    fetch(spec, pin.parent, source)

    command = [sys.executable, source / "build_scripts" / "build_usd.py", *platform_flags(spec["flags"]),
               "--src", work / "src", "--build", work / "build"]
    if args.jobs:
        command += ["--jobs", str(args.jobs)]
    command.append(install)
    print("Building OpenUSD:", " ".join(str(part) for part in command), flush=True)
    # CMake 4 refuses projects that ask for a CMake older than 3.5, as some of the
    # dependencies' own dependencies do (yaml-cpp in OpenColorIO); this lets it try.
    environment = dict(os.environ)
    environment.setdefault("CMAKE_POLICY_VERSION_MINIMUM", "3.5")
    if MACOS:
        # Nor through pkg-config: --ignore-homebrew (usd_asset.MACOS_FLAGS) is CMake's only.
        environment["PKG_CONFIG_LIBDIR"] = str(install / "lib/pkgconfig")
    result = subprocess.run([str(part) for part in command], env=environment)
    if result.returncode != 0:
        log = work / "src" / "log.txt"
        fail(f"build_usd.py failed ({result.returncode}); its log: {log}")

    run([sys.executable, REPO / "tools" / "ci" / "usd_licenses.py", "--src", work / "src", "--build", work / "build",
         "--destination", install / "licenses", "--openusd-source", source])
    print(f"OpenUSD is in {install}. Build usdcc against it with: python tools/build.py --usd-install {install}")


if __name__ == "__main__":
    main()
