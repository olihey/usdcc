"""Builds, runs and tests usdcc on Linux in WSL 2, from Windows.

Copies this checkout's working tree (tracked files and untracked ones that
aren't ignored, edits included) into a clone on WSL's own disk, then runs a
command there and passes its output and exit code through (docs/LINUX_WSL.md).
Code is edited and committed on Windows only; the clone never commits.

    python tools/wsl.py setup                       # once: clone, submodules, vcpkg
    python tools/wsl.py usd                         # OpenUSD into ~/deps/usd (tools/build_usd.py)
    python tools/wsl.py build                       # tools/build.py --usd-install ~/deps/usd
    python tools/wsl.py run --scene test_data/cube_and_sphere.usda
    python tools/wsl.py test -R usdcc_tests         # ctest in the clone's build folder
    python tools/wsl.py --headless test             # ...under Xvfb, no window on screen
    python tools/wsl.py exec -- git status          # anything else, in the clone
    python tools/wsl.py sync                        # only copy the files

OpenGL goes through Mesa's d3d12 driver on the GPU named by --gpu (Mesa doesn't
pick it by itself under WSLg); --software uses llvmpipe, like a hosted runner.
Qt uses X11 (XWayland) unless --wayland.

Standard library only; Windows only.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path, PureWindowsPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import REPO, WINDOWS, fail  # noqa: E402

DEFAULT_DISTRO = os.environ.get("USDCC_WSL_DISTRO", "Ubuntu-24.04")
DEFAULT_CLONE = os.environ.get("USDCC_WSL_CLONE", "~/dev/usdcc")
DEFAULT_USD = os.environ.get("USDCC_WSL_USD", "~/deps/usd")


def wsl_path(path):
    """C:\\a\\b as WSL sees it: /mnt/c/a/b."""
    path = PureWindowsPath(path)
    drive = path.drive.rstrip(":").lower()
    return "/mnt/" + drive + "/" + "/".join(path.parts[1:])


def shell_path(path):
    """A path for bash, with a leading ~ left unquoted so that it expands."""
    if path == "~" or path.startswith("~/"):
        return "~" + (shlex.quote(path[1:]) if len(path) > 1 else "")
    return shlex.quote(path)


def submodule_paths():
    result = subprocess.run(["git", "-C", str(REPO), "config", "--file", ".gitmodules", "--get-regexp", r"\.path$"],
                            capture_output=True, text=True)
    return {line.split(" ", 1)[1].strip() for line in result.stdout.splitlines() if " " in line}


def working_tree_files():
    """This checkout's files, as git lists them (NUL-separated), without submodules."""
    result = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                            capture_output=True)
    if result.returncode != 0:
        fail("git ls-files failed")
    skip = submodule_paths()
    files = [name for name in result.stdout.split(b"\0") if name and name.decode("utf-8", "replace") not in skip]
    return b"\0".join(files) + b"\0"


def wsl(distro, script, stdin=None):
    """Runs a bash script in the distro; returns its exit code."""
    command = ["wsl.exe", "-d", distro, "--cd", "~", "-e", "bash", "-lc", script]
    sys.stdout.flush()
    return subprocess.run(command, input=stdin).returncode


def sync_script(clone):
    # The clone's own list too, with --delete-missing-args: a file deleted here
    # is deleted there. Only files are listed, so -a doesn't recurse into
    # folders (build/, external/vcpkg stay as they are). Everything under
    # /mnt/c looks executable, so the clone keeps its own file modes; and the
    # Windows checkout's CRLF files count as unchanged (autocrlf=input).
    return f"""set -e
cd {shell_path(clone)}
git config core.autocrlf input
{{ cat; git ls-files -z --cached --others --exclude-standard | grep -zv -x -F -f <(git config --file .gitmodules --get-regexp '\\.path$' | cut -d' ' -f2) || true; }} \\
  | sort -zu | rsync -rlt --from0 --files-from=- --delete-missing-args --info=stats0 {shlex.quote(wsl_path(REPO))}/ ./
"""


def environment(args):
    if args.software:
        lines = ["unset GALLIUM_DRIVER MESA_D3D12_DEFAULT_ADAPTER_NAME", "export LIBGL_ALWAYS_SOFTWARE=1"]
    else:
        lines = ["export GALLIUM_DRIVER=d3d12", f"export MESA_D3D12_DEFAULT_ADAPTER_NAME={shlex.quote(args.gpu)}"]
    # Qt would pick Wayland under WSLg (it sets WAYLAND_DISPLAY); X11 is what
    # has been checked there (docs/LINUX_WSL.md), and what X11 tools can see.
    if not args.wayland:
        lines.append("export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    if not WINDOWS:
        fail("tools/wsl.py runs on Windows; in WSL itself use tools/build.py, tools/run.py and ctest directly.")
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog="\n\n".join(__doc__.split("\n\n")[1:-1]))
    parser.add_argument("--distro", default=DEFAULT_DISTRO, help=f"default: {DEFAULT_DISTRO} ($USDCC_WSL_DISTRO)")
    parser.add_argument("--clone", default=DEFAULT_CLONE, help=f"the clone in WSL (default: {DEFAULT_CLONE})")
    parser.add_argument("--usd-install", default=DEFAULT_USD, help=f"OpenUSD in WSL (default: {DEFAULT_USD})")
    parser.add_argument("--gpu", default="NVIDIA", help="the adapter d3d12 uses, by name (default: NVIDIA)")
    parser.add_argument("--software", action="store_true", help="llvmpipe instead of the GPU")
    parser.add_argument("--headless", action="store_true", help="under xvfb-run, not on WSLg's display")
    parser.add_argument("--wayland", action="store_true", help="let Qt use WSLg's Wayland (default: X11)")
    parser.add_argument("--no-sync", action="store_true", help="don't copy the working tree first")
    parser.add_argument("command", choices=["setup", "sync", "usd", "build", "run", "test", "exec"])
    parser.add_argument("arguments", nargs=argparse.REMAINDER, help="passed on to the command")
    args = parser.parse_args(argv)
    arguments = [argument for argument in args.arguments if argument != "--"] if args.command == "exec" \
        else args.arguments

    clone = args.clone
    if args.command == "setup":
        script = f"""set -e
mkdir -p {shell_path(os.path.dirname(clone) or "~")}
[ -d {shell_path(clone)}/.git ] || git clone {shlex.quote(wsl_path(REPO))} {shell_path(clone)}
cd {shell_path(clone)}
git remote get-url win >/dev/null 2>&1 || git remote rename origin win
git submodule update --init --recursive
[ -x external/vcpkg/vcpkg ] || external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
"""
        return wsl(args.distro, script)

    if not args.no_sync:
        code = wsl(args.distro, sync_script(clone), stdin=working_tree_files())
        if code != 0:
            return code
    if args.command == "sync":
        return 0

    quoted = " ".join(shlex.quote(argument) for argument in arguments)
    usd = shell_path(args.usd_install)
    if args.command == "usd":
        command = f"python3 tools/build_usd.py {quoted or usd}"
    elif args.command == "build":
        command = f"python3 tools/build.py --usd-install {usd} --without-rtx {quoted}"
    elif args.command == "run":
        command = f"python3 tools/run.py --usd-install {usd} --without-rtx {quoted}"
    elif args.command == "test":
        command = f"ctest --test-dir build --output-on-failure --timeout 900 {quoted}"
    else:
        if not arguments:
            fail("exec needs a command")
        # One argument is a shell snippet ("cd build && ls"); several are a command and its arguments.
        command = arguments[0] if len(arguments) == 1 else quoted
    if args.headless:
        command = f"xvfb-run -a -s '-screen 0 1920x1080x24' {command}"
    return wsl(args.distro, f"cd {shell_path(clone)}\n{environment(args)}{command}\n")


if __name__ == "__main__":
    sys.exit(main())
