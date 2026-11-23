"""Runs scripts and reloads plugins in a running usdcc (docs/VSCODE.md). Standard library only.

    python tools/usdcc_send.py list
    python tools/usdcc_send.py run setup.py
    python tools/usdcc_send.py run - < setup.py
    python tools/usdcc_send.py reload my_plugin/python/my_plugin/panel.py
    python tools/usdcc_send.py reload --id com.example.notes
    python tools/usdcc_send.py plugins

With several running, --pid picks one. One with a test profile is only found with
--test-profile. Exit code: 0 done, 1 the script or plugin failed, 2 it couldn't be asked.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import tempfile
import time
from pathlib import Path

WINDOWS = sys.platform == "win32"
API = 1
PIPE_BUSY = 231  # ERROR_PIPE_BUSY: every instance is taken for a moment


class LinkError(Exception):
    """usdcc couldn't be reached, or refused the request."""


def socket_folder():
    if WINDOWS:
        return "\\\\.\\pipe\\"
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    return os.path.join(runtime, "usdcc") if runtime else os.path.join(tempfile.gettempdir(), f"usdcc-{os.getuid()}")


def find_apps(test_profile=None):
    """The addresses of the running apps, by process id."""
    program = f"usdcc-test-{test_profile}" if test_profile else "usdcc"
    pattern = re.compile(re.escape(program) + r"-editor-(\d+)")
    apps = {}
    try:
        names = os.listdir(socket_folder())
    except OSError:
        return apps
    for name in names:
        match = pattern.fullmatch(name)
        if match:
            apps[int(match.group(1))] = os.path.join(socket_folder(), name)
    return apps


class Link:
    """A connection. Requests one at a time; a run's output goes to `on_output` as it comes."""

    def __init__(self, address):
        self._socket = None
        self._next_id = 0
        try:
            if WINDOWS:
                self._file = self._open_pipe(address)
            else:
                self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self._socket.connect(address)
                self._file = self._socket.makefile("rwb", buffering=0)
        except ConnectionRefusedError as error:
            if not WINDOWS:
                try:
                    os.unlink(address)  # left by one that crashed
                except OSError:
                    pass
            raise LinkError(f"usdcc at {address} isn't running") from error
        except OSError as error:
            raise LinkError(f"can't connect to {address}: {error}") from error
        self._buffer = b""

    @staticmethod
    def _open_pipe(address):
        deadline = time.monotonic() + 2
        while True:
            try:
                return open(address, "r+b", buffering=0)
            except OSError as error:
                if getattr(error, "winerror", None) not in (PIPE_BUSY, None) or time.monotonic() > deadline:
                    raise
                time.sleep(0.02)

    def close(self):
        self._file.close()
        if self._socket:
            self._socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _read_line(self):
        while b"\n" not in self._buffer:
            try:
                data = self._file.read(65536)
            except OSError as error:
                raise LinkError("usdcc closed the connection") from error
            if not data:
                raise LinkError("usdcc closed the connection")
            self._buffer += data
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line)

    def request(self, method, params=None, on_output=None):
        self._next_id += 1
        message = {"id": self._next_id, "method": method, "params": params or {}}
        data = memoryview(json.dumps(message).encode() + b"\n")
        try:
            while data:
                data = data[self._file.write(data):]
        except OSError as error:
            raise LinkError("usdcc closed the connection") from error
        while True:
            answer = self._read_line()
            if "event" in answer:
                if on_output and answer["event"] == "output" and answer.get("run") == self._next_id:
                    on_output(answer)
                continue
            if answer.get("id") != self._next_id:
                continue
            if "error" in answer:
                raise LinkError(f"{answer['error']['message']} ({answer['error']['code']})")
            return answer["result"]


def show(event):
    stream = sys.stdout if event["category"] == "python.stdout" else sys.stderr
    plain = event["source"] == "python" or event["category"] == "plugin"
    print(event["text"] if plain else f"{event['category']}: {event['text']}", file=stream, flush=True)


def connect(args):
    apps = find_apps(args.test_profile)
    if args.pid is not None:
        if args.pid not in apps:
            raise LinkError(f"no usdcc with process id {args.pid} lets editors connect")
        apps = {args.pid: apps[args.pid]}
    links = []
    for address in apps.values():
        try:
            link = Link(address)
        except LinkError:
            continue
        links.append((link, link.request("hello")))
    if len(links) != 1:
        for link, _ in links:
            link.close()
        if not links:
            raise LinkError("no running usdcc lets editors connect")
        listed = ", ".join(f"{hello['pid']} ({hello['stage'] or 'no stage'})" for _, hello in links)
        raise LinkError(f"several are running: pick one with --pid: {listed}")
    link, hello = links[0]
    if hello["api"] != API:
        link.close()
        raise LinkError(f"usdcc speaks version {hello['api']} of the protocol, and this {API}")
    return link


def command_list(args):
    for pid, address in sorted(find_apps(args.test_profile).items()):
        try:
            with Link(address) as link:
                hello = link.request("hello")
        except LinkError:
            continue
        print(f"{pid}  usdcc {hello['version']}  {hello['stage'] or 'no stage'}{'  busy' if hello['busy'] else ''}")
    return 0


def command_run(args):
    if args.file == "-":
        params = {"code": sys.stdin.buffer.read().decode("utf-8-sig"), "name": "<stdin>"}
    else:
        path = Path(args.file).resolve()
        params = {"code": path.read_text(encoding="utf-8-sig"), "file": path.as_posix()}
    if args.keep_variables:
        params["scope"] = "session"
    with connect(args) as link:
        result = link.request("run", params, show)
    steps = result["undoSteps"]
    undo = "" if steps == 0 else ", 1 undo step" if steps == 1 else f", {steps} undo steps"
    print(f"{'done in' if result['ok'] else 'failed after'} {result['ms']} ms{undo}", file=sys.stderr)
    return 0 if result["ok"] else 1


def command_reload(args):
    if bool(args.id) == bool(args.path):
        raise LinkError("reload takes a path in the plugin's folder, or --id")
    params = {"id": args.id} if args.id else {"path": Path(args.path).resolve().as_posix()}
    with connect(args) as link:
        result = link.request("reloadPlugin", params, show)
    for note in result["notes"]:
        print(note, file=sys.stderr)
    if result["ok"]:
        print(f"reloaded {', '.join(result['reloaded'])} in {result['ms']} ms", file=sys.stderr)
        return 0
    print(f"not running: {result['failure'] or result.get('plugin', {}).get('reason', '')}", file=sys.stderr)
    return 1


def command_plugins(args):
    with connect(args) as link:
        for plugin in link.request("plugins")["plugins"]:
            reason = f"  {plugin['reason']}" if plugin["reason"] else ""
            print(f"{plugin['id']}  {plugin['state']}  {plugin['folder']}{reason}")
    return 0


def command_reset(args):
    with connect(args) as link:
        link.request("resetSession")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pid", type=int, help="the usdcc to ask, when several are running")
    parser.add_argument("--test-profile", metavar="NAME", help="one started with USDCC_TEST_PROFILE=NAME")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="the running apps").set_defaults(run=command_list)
    run = commands.add_parser("run", help="run a file, or - for standard input")
    run.add_argument("file")
    run.add_argument("--keep-variables", action="store_true", help="in the session's variables, which are kept")
    run.set_defaults(run=command_run)
    reload = commands.add_parser("reload", help="reload a plugin")
    reload.add_argument("path", nargs="?", help="any file in its folder")
    reload.add_argument("--id")
    reload.set_defaults(run=command_reload)
    commands.add_parser("plugins", help="list the plugins").set_defaults(run=command_plugins)
    commands.add_parser("reset", help="forget the kept variables").set_defaults(run=command_reset)
    args = parser.parse_args()
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    try:
        return args.run(args)
    except (LinkError, OSError, UnicodeDecodeError) as error:
        print(f"usdcc_send: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
