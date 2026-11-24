"""Makes build/vscode/usdcc.vsix, the VS Code extension (editors/vscode, docs/VSCODE.md). Needs
Node and npm; tools/deploy.py runs it, and copies the result into the deploy's tools/vscode.

    python tools/build_vsix.py

Its version is usdcc's release number, so that each release's replaces the one before.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import REPO  # noqa: E402

BUILD_DIR = REPO / "build"
SOURCE = REPO / "editors/vscode"


class VsixError(Exception):
    """Why there's no .vsix."""


def version():
    """usdcc's release number, which the build wrote for C++ plugins."""
    config = BUILD_DIR / "sdk/UsdccPluginConfig.cmake"
    match = re.search(r'USDCC_BUILT_FOR_USDCC "(\d+\.\d+\.\d+)', config.read_text(encoding="utf-8")) if config.is_file() else None
    if not match:
        raise VsixError(f"no version in {config}: configure first")
    return match.group(1)


def build(out=BUILD_DIR / "vscode/usdcc.vsix"):
    """The .vsix's path; VsixError when it can't be made."""
    npm = shutil.which("npm")
    if not npm:
        raise VsixError("npm isn't on PATH")

    def run(*arguments):
        done = subprocess.run([npm, *arguments], cwd=SOURCE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if done.returncode:
            raise VsixError(f"npm {' '.join(arguments)} failed:\n{done.stdout.decode('utf-8', 'replace')[-2000:]}")

    # The pinned tools; again when the lock file is newer than what's installed.
    installed = SOURCE / "node_modules/.package-lock.json"
    if not installed.is_file() or installed.stat().st_mtime < (SOURCE / "package-lock.json").stat().st_mtime:
        run("ci", "--no-audit", "--no-fund")
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    shutil.rmtree(SOURCE / "out", ignore_errors=True)
    run("exec", "--", "vsce", "package", version(), "--no-git-tag-version", "--no-update-package-json", "--no-dependencies", "-o", str(out))
    if not out.is_file():
        raise VsixError("vsce made no file")
    return out


def main():
    argparse.ArgumentParser(description=__doc__.split("\n\n")[0]).parse_args()
    try:
        print(build())
    except VsixError as error:
        print(f"build_vsix.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
