#!/usr/bin/env python3
"""Makes the AppDir that vpk packs into usdcc's AppImage (docs/UPDATES.md section 9).

    python tools/make_appdir.py <deploy> <folder>/usdcc.AppDir

A Linux deploy (tools/deploy.py), moved or copied into the AppDir's usr folder: its bin/
becomes usr/bin, where Velopack looks for the program it updates. Beside it what an
AppImage has at its top: AppRun, the desktop entry and the icon.

AppRun starts usdcc, and the render node when it is asked to:

    ./usdcc.AppImage usdcc_node render shot.usda
    ln -s usdcc.AppImage usdcc_node && ./usdcc_node render shot.usda
"""
import argparse
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RESOURCES = REPO / "src/cpp/app/resources/linux"

# ARGV0 is the name the AppImage was called by, which its runtime sets. It isn't passed on:
# zsh, started from usdcc, would take it as its own programs' name.
APPRUN = """#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
called="$(basename -- "${ARGV0:-usdcc}")"
unset ARGV0
if [ "$called" = usdcc_node ]; then
    exec "$here/usr/bin/usdcc_node" "$@"
fi
if [ "$1" = usdcc_node ]; then
    shift
    exec "$here/usr/bin/usdcc_node" "$@"
fi
exec "$here/usr/bin/usdcc" "$@"
"""


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("deploy", type=Path, help="the folder tools/deploy.py made")
    parser.add_argument("appdir", type=Path, help="the AppDir to make; its name ends with .AppDir, as vpk asks")
    parser.add_argument("--move", action="store_true", help="move the deploy instead of copying it")
    args = parser.parse_args(argv)
    if sys.platform == "win32":
        sys.exit("make_appdir.py: on Linux only, for the deploy's links and modes.")
    if not (args.deploy / "bin/usdcc").is_file() or not (args.deploy / "bin/usdcc_node").is_file():
        sys.exit(f"make_appdir.py: {args.deploy} isn't a Linux deploy (no bin/usdcc and bin/usdcc_node).")
    if not args.appdir.name.endswith(".AppDir"):
        sys.exit(f"make_appdir.py: {args.appdir} has to end with .AppDir.")
    if args.appdir.exists():
        sys.exit(f"make_appdir.py: {args.appdir} exists already.")

    args.appdir.mkdir(parents=True)
    if args.move:
        shutil.move(args.deploy, args.appdir / "usr")
    else:
        shutil.copytree(args.deploy, args.appdir / "usr", symlinks=True)
    apprun = args.appdir / "AppRun"
    apprun.write_text(APPRUN, encoding="utf-8", newline="\n")
    apprun.chmod(0o755)
    # A file manager starts it with the file: the desktop entry's Exec is for those that
    # install the AppImage.
    shutil.copy2(RESOURCES / "usdcc.desktop", args.appdir / "usdcc.desktop")
    icon = RESOURCES / "icons/hicolor/256x256/apps/usdcc.png"
    shutil.copy2(icon, args.appdir / "usdcc.png")
    shutil.copy2(icon, args.appdir / ".DirIcon")
    shutil.copytree(RESOURCES / "icons", args.appdir / "usr/share/icons", dirs_exist_ok=True)
    print(f"Made {args.appdir}")


if __name__ == "__main__":
    main()
