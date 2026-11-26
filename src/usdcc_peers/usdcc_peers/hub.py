"""The hub: a studio's rooms that are always there (PEERS.md §9).

    python -m usdcc_peers.hub --name Studio --port 47800 --http 8080 --token-file hub.token

It hosts a room that never closes, or several (one per show or department) named in a
configuration file, each on its own port with its own link. Artists list a link under
Settings > Peers > Rooms joined at start. It keeps each room's shared state while it
runs. Members read the `notify` channel; only the hub and the services the
configuration names (by their key's fingerprint) send on it.

Its HTTP side is for services that can only call a webhook. Every request needs the
token, as `Authorization: Bearer <token>`:

| Request | Body | Does |
|---|---|---|
| `POST /notify` | A notification (usdcc_peers.notify), with `to` (a name or id) and `room` (a name), both optional | A toast on every member of the room, or on the one named |
| `POST /state` | `{room?, map, key, value}`; no `value` removes the key | Sets a key of a shared map, as the hub |
| `GET /rooms` | | The rooms, with who is in each |

Its key, and each room's secret, are kept in the data folder, so the links stay the same
from one run to the next. The links are written to `links.json` there, readable by its
owner only: they are invitations, and are never logged.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import signal
import sys
from dataclasses import dataclass, field

from . import crypto
from .frame import is_valid_channel_name
from .host import Host, Limits
from .notify import NotificationError, from_json
from .room import Senders, log

MAX_REQUEST_BYTES = 64 * 1024


@dataclass
class RoomConfig:
    name: str
    port: int = 0
    admission: str = "invited"
    password: str = ""
    notify_senders: list[str] = field(default_factory=list)  # Fingerprints, or peer ids


@dataclass
class HubConfig:
    rooms: list[RoomConfig]
    address: str = "0.0.0.0"
    link_address: str = ""  # The name or address in the links; this machine's own if empty
    http_port: int | None = None  # None: no HTTP side; 0: any free port
    http_address: str = "0.0.0.0"
    token: str = ""
    data: str = ""
    max_members: int = 2000


class Hub:
    def __init__(self, config: HubConfig):
        self.config = config
        os.makedirs(config.data, exist_ok=True)
        self.identity = crypto.Identity.load_or_create(os.path.join(config.data, "hub.key"))
        self.rooms: list[Host] = []
        self._http = None
        self._saved = _read_json(os.path.join(config.data, "rooms.json"))

    async def start(self) -> None:
        for room_config in self.config.rooms:
            saved = self._saved.get(room_config.name, {})
            room = Host(name=room_config.name, identity=self.identity, admission=room_config.admission,
                        password=room_config.password, secret=saved.get("secret", ""),
                        instance=saved.get("instance", ""), app="usdcc_peers hub",
                        limits=Limits(max_members=self.config.max_members))
            notify = room.channel("notify")
            notify.set_senders(Senders.NAMED, room_config.notify_senders)
            await room.start(self.config.address, room_config.port)
            self._saved[room_config.name] = {"secret": room.secret, "instance": room.instance}
            self.rooms.append(room)
        _write_private(os.path.join(self.config.data, "rooms.json"), self._saved)
        _write_private(os.path.join(self.config.data, "links.json"), self.links())
        if self.config.http_port is not None and self.config.token:
            self._http = await asyncio.start_server(self._serve_http, self.config.http_address,
                                                    self.config.http_port)
            log.info("The hub's HTTP side is on %s port %d", self.config.http_address, self.http_port)
        elif self.config.http_port is not None:
            log.warning("No token: the hub's HTTP side is off")

    @property
    def http_port(self) -> int:
        return self._http.sockets[0].getsockname()[1] if self._http else 0

    def links(self) -> list[dict]:
        address = self.config.link_address or _this_machine(self.config.address)
        return [{"room": room.name, "port": room.port, "link": room.link(address)} for room in self.rooms]

    def room(self, name: str | None) -> Host | None:
        if not name:
            return self.rooms[0] if self.rooms else None
        for room in self.rooms:
            if room.name == name:
                return room
        return None

    async def close(self) -> None:
        if self._http:
            self._http.close()
            await self._http.wait_closed()
        for room in self.rooms:
            await room.close("The hub stopped")

    # --- HTTP ----------------------------------------------------------------------------

    async def _serve_http(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            status, answer = await asyncio.wait_for(self._handle(reader), 10)
        except (TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError):
            status, answer = 400, {"error": "Not a request this hub understands"}
        body = json.dumps(answer).encode("utf-8")
        reasons = {200: "OK", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found", 405: "Method Not Allowed",
                   413: "Payload Too Large"}
        writer.write(f"HTTP/1.1 {status} {reasons.get(status, 'Error')}\r\nContent-Type: application/json\r\n"
                     f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode("ascii") + body)
        try:
            await writer.drain()
        finally:
            writer.close()

    async def _handle(self, reader: asyncio.StreamReader):
        head = await reader.readuntil(b"\r\n\r\n")
        if len(head) > 16 * 1024:
            return 400, {"error": "The request's head is too long"}
        lines = head.decode("latin-1").split("\r\n")
        method, target, _ = lines[0].split(" ", 2)
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0") or 0)
        if length > MAX_REQUEST_BYTES:
            return 413, {"error": f"At most {MAX_REQUEST_BYTES // 1024} KB"}
        payload = await reader.readexactly(length) if length else b""
        given = headers.get("authorization", "")
        if not given.startswith("Bearer ") or not crypto.same_secret(given[7:].strip(), self.config.token):
            return 401, {"error": "The token is missing or wrong"}
        path = target.split("?", 1)[0]
        if path == "/rooms":
            if method != "GET":
                return 405, {"error": "GET only"}
            return 200, {"rooms": [{"name": room.name, "port": room.port,
                                    "peers": [{"id": p.id, "name": p.name, "app": p.app} for p in room.peers()]}
                                   for room in self.rooms]}
        if method != "POST":
            return 405, {"error": "POST only"}
        try:
            data = json.loads(payload or b"{}")
        except ValueError:
            return 400, {"error": "The body isn't JSON"}
        if not isinstance(data, dict):
            return 400, {"error": "The body isn't a JSON object"}
        room = self.room(data.get("room"))
        if room is None:
            return 404, {"error": "No room of that name"}
        if path == "/notify":
            return self._notify(room, data)
        if path == "/state":
            return self._state(room, data)
        return 404, {"error": "No such endpoint"}

    def _notify(self, room: Host, data: dict):
        try:
            message = from_json(data)
        except NotificationError as error:
            return 400, {"error": str(error)}
        to = data.get("to")
        if to is not None and not isinstance(to, str):
            return 400, {"error": "`to` is a name or an id"}
        if to:
            targets = [peer for peer in room.find(to) if peer.id != room.me.id]
            if not targets:
                return 404, {"error": "Nobody in the room has that name or id"}
            for peer in targets:
                room.channel("notify").send("notify", message, to=peer.id)
            return 200, {"sent": len(targets)}
        room.channel("notify").send("notify", message)
        return 200, {"sent": len(room.peers()) - 1}

    def _state(self, room: Host, data: dict):
        name = data.get("map")
        key = data.get("key")
        if not is_valid_channel_name(name):
            return 400, {"error": "`map` isn't a map's name"}
        if not isinstance(key, str):
            return 400, {"error": "`key` is text"}
        state = room.state(name)
        if "value" in data:
            state.set(key, data["value"])
        else:
            state.remove(key)
        return 200, {"map": name, "key": key, "value": state.get(key, None) if key in state else None}


def _read_json(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as file:
            value = json.load(file)
            return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def _write_private(path: str, value) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as file:
        json.dump(value, file, indent=2)


def _this_machine(address: str) -> str:
    if address not in ("0.0.0.0", "::", ""):
        return address
    import socket

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("192.0.2.1", 9))  # No packet is sent: it picks the outgoing address
            return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def _config(arguments) -> HubConfig:
    data = arguments.data or os.path.join(os.path.expanduser("~"), ".local", "share", "usdcc_peers", "hub")
    token = ""
    if arguments.token_file:
        with open(arguments.token_file, encoding="utf-8") as file:
            token = file.read().strip()
    file = _read_json(arguments.config) if arguments.config else {}
    if arguments.config and not file:
        raise SystemExit(f"{arguments.config} isn't a JSON object")
    rooms = []
    for entry in file.get("rooms", []):
        rooms.append(RoomConfig(name=entry["name"], port=int(entry.get("port", 0)),
                                admission=entry.get("admission", "invited"), password=entry.get("password", ""),
                                notify_senders=list(entry.get("notifySenders", []))))
    if not rooms:
        rooms.append(RoomConfig(name=arguments.name, port=arguments.port, admission=arguments.admission,
                                notify_senders=list(arguments.notify_sender)))
    return HubConfig(rooms=rooms, address=arguments.address or file.get("address", "0.0.0.0"),
                     link_address=arguments.link_address or file.get("linkAddress", ""),
                     http_port=arguments.http if arguments.http is not None else file.get("http"),
                     http_address=file.get("httpAddress", arguments.address or "0.0.0.0"), token=token, data=data,
                     max_members=int(file.get("maxMembers", arguments.max_members)))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="usdcc-hub", description="Hosts a studio's usdcc rooms (PEERS.md §9).")
    parser.add_argument("--name", default="Studio", help="the room's name, without --config")
    parser.add_argument("--port", type=int, default=47800, help="the room's port, without --config (0: any)")
    parser.add_argument("--admission", default="invited", choices=("invited", "ask", "open"),
                        help="who may join without the link: invited (nobody), open (anyone who finds it)")
    parser.add_argument("--notify-sender", action="append", default=[], metavar="FINGERPRINT",
                        help="a service that may send notifications, by its key's fingerprint; repeatable")
    parser.add_argument("--config", help="a JSON file naming several rooms (see the package's README)")
    parser.add_argument("--address", help="where to listen (default: every address)")
    parser.add_argument("--link-address", help="the name or address in the links (default: this machine's)")
    parser.add_argument("--http", type=int, help="the HTTP side's port (0: any); off without --token-file")
    parser.add_argument("--token-file", help="a file holding the HTTP side's token")
    parser.add_argument("--data", help="where the hub keeps its key and rooms (default: ~/.local/share/usdcc_peers/hub)")
    parser.add_argument("--max-members", type=int, default=2000, help="members per room (default: 2000)")
    parser.add_argument("--print-links", action="store_true",
                        help="print the rooms' links and ports as JSON on standard output (for tests)")
    parser.add_argument("--stop-on-eof", action="store_true",
                        help="stop when standard input closes (for a program that runs the hub, on any system)")
    parser.add_argument("--log-level", default="info", choices=("debug", "info", "warning", "error"))
    arguments = parser.parse_args(argv)
    logging.basicConfig(level=arguments.log_level.upper(), format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("websockets").setLevel(logging.WARNING)
    config = _config(arguments)

    async def run() -> None:
        hub = Hub(config)
        await hub.start()
        log.info("The hub's links are in %s", os.path.join(config.data, "links.json"))
        if arguments.print_links:
            print(json.dumps({"rooms": hub.links(), "http": hub.http_port,
                              "fingerprint": hub.identity.fingerprint}), flush=True)
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop.set)
            except (NotImplementedError, RuntimeError):
                pass  # Windows: Ctrl+C raises KeyboardInterrupt instead
        if arguments.stop_on_eof:
            import threading

            def watch() -> None:
                sys.stdin.read()  # Until the other end closes it
                loop.call_soon_threadsafe(stop.set)

            threading.Thread(target=watch, daemon=True).start()
        try:
            await stop.wait()
        finally:
            await hub.close()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
