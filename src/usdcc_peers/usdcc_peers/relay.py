"""The relay (PEERS.md §5.4): a room for two programs that can't reach each other.

    python -m usdcc_peers.relay --port 8443 --token-file relay.token

A host behind a router connects out to it and gets a room on it; members connect out to
that room. The relay forwards the bytes between them and can't read them: everything a
peer says is sealed for its connection or for the room (PEERS_PROTOCOL.md §2) before it
reaches the relay. It keeps nothing, and logs connections and byte counts only.

It listens on one port, behind any HTTPS reverse proxy for `wss://`, or with --certfile
and --keyfile for TLS of its own. Only `websockets` is needed, not PyNaCl.

## Its side of the protocol (PEERS_PROTOCOL.md §11)

- **A host** opens `GET /host` (WebSocket), with `Authorization: Bearer <token>` when the
  relay has a token, and `?room=<id>` to ask for a room id it had before (it gets it if no
  host has it now). The relay answers with a text message `{"room": "<id>"}`. Every binary
  message on this connection is a *relay frame*:

  | First byte | Layout | Way |
  |---|---|---|
  | 1, data | u32 member, then the bytes | Both: from a member, or to one |
  | 2, data to several | u16 count, count × u32 member, then the bytes | Host → relay: the same bytes to each |
  | 3, opened | u32 member | Relay → host: a member connected |
  | 4, closed | u32 member, u16 close code, the reason in UTF-8 | Both: a member's connection ended, or the host ends it |

  Integers are big-endian. The bytes are a member's WebSocket binary message, as it was.
- **A member** opens `GET /r/<id>` (WebSocket): what it sends goes to the host, and what the
  host sends for it comes back, binary for binary, text for text.
- When the host's connection ends, its members' do too (1001), and the room is gone.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import secrets
import signal
import ssl
import struct
import sys
import time
from dataclasses import dataclass, field

import websockets
from websockets.asyncio.server import serve

log = logging.getLogger("usdcc_peers.relay")

DATA, DATA_MANY, OPENED, CLOSED = 1, 2, 3, 4
_ROOM_ID = "abcdefghijkmnpqrstuvwxyz23456789"


@dataclass
class RelayLimits:
    members: int = 50  # Per room
    frame_bytes: int = 8 * 1024 * 1024  # One message, either way
    bytes_per_second: int = 32 * 1024 * 1024  # From one room's host and members together, averaged over a second
    rooms: int = 10000


@dataclass
class _Room:
    id: str
    host: object
    members: dict[int, object] = field(default_factory=dict)
    next_member: int = 1
    bytes_in: int = 0
    bytes_out: int = 0
    window_start: float = 0.0
    window_bytes: int = 0
    started: float = field(default_factory=time.monotonic)


class Relay:
    def __init__(self, token: str = "", limits: RelayLimits | None = None):
        self.token = token
        self.limits = limits or RelayLimits()
        self.rooms: dict[str, _Room] = {}
        self._server = None
        self.port = 0

    async def start(self, address: str = "0.0.0.0", port: int = 0, ssl_context: ssl.SSLContext | None = None) -> None:
        self._server = await serve(self._connection, address, port, process_request=self._check,
                                   max_size=self.limits.frame_bytes + 1024, ping_interval=10, ping_timeout=30,
                                   compression=None, server_header=None, ssl=ssl_context)
        self.port = list(self._server.sockets)[0].getsockname()[1]
        log.info("Relaying on %s port %d%s", address, self.port, " with TLS" if ssl_context else "")

    async def close(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    # --- Connections -------------------------------------------------------------------

    def _check(self, connection, request):
        path = request.path.split("?", 1)[0]
        if path == "/host":
            given = request.headers.get("Authorization", "")
            if self.token and not (given.startswith("Bearer ") and secrets.compare_digest(given[7:].strip(),
                                                                                           self.token)):
                return connection.respond(401, "The relay's token is missing or wrong\n")
            if len(self.rooms) >= self.limits.rooms:
                return connection.respond(503, "The relay has as many rooms as it takes\n")
            return None
        if path.startswith("/r/"):
            room = self.rooms.get(path[3:].strip("/"))
            if room is None:
                return connection.respond(404, "No such room on this relay\n")
            if len(room.members) >= self.limits.members:
                return connection.respond(503, "The room is full\n")
            return None
        if path == "/health":
            return connection.respond(200, "ok\n")
        return connection.respond(404, "Not found\n")

    async def _connection(self, websocket) -> None:
        path, _, query = websocket.request.path.partition("?")
        if path == "/host":
            await self._host(websocket, dict(part.split("=", 1) for part in query.split("&") if "=" in part))
        else:
            await self._member(websocket, path[3:].strip("/"))

    async def _host(self, websocket, query: dict) -> None:
        wanted = query.get("room", "")
        if wanted and all(c in _ROOM_ID for c in wanted) and 6 <= len(wanted) <= 32 and wanted not in self.rooms:
            room_id = wanted
        else:
            room_id = "".join(secrets.choice(_ROOM_ID) for _ in range(10))
            while room_id in self.rooms:
                room_id = "".join(secrets.choice(_ROOM_ID) for _ in range(10))
        room = _Room(room_id, websocket)
        self.rooms[room_id] = room
        peer = _remote(websocket)
        log.info("Room %s opened by a host at %s", room_id, peer)
        await websocket.send(json.dumps({"room": room_id}))
        try:
            async for data in websocket:
                if isinstance(data, str) or not data:
                    continue
                if not self._allowed(room, len(data)):
                    await websocket.close(1008, "Over the relay's rate for a room")
                    break
                await self._from_host(room, data)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            del self.rooms[room_id]
            for member in list(room.members.values()):
                await member.close(1001, "The host left the relay")
            log.info("Room %s closed after %.0f s: %d bytes from members, %d to them", room_id,
                     time.monotonic() - room.started, room.bytes_in, room.bytes_out)

    async def _from_host(self, room: _Room, data: bytes) -> None:
        kind = data[0]
        if kind == DATA and len(data) >= 5:
            member = room.members.get(struct.unpack(">I", data[1:5])[0])
            if member is not None:
                room.bytes_out += len(data) - 5
                await _send(member, data[5:])
        elif kind == DATA_MANY and len(data) >= 3:
            count = struct.unpack(">H", data[1:3])[0]
            head = 3 + 4 * count
            if len(data) < head:
                return
            payload = data[head:]
            for (member_id,) in struct.iter_unpack(">I", data[3:head]):
                member = room.members.get(member_id)
                if member is not None:
                    room.bytes_out += len(payload)
                    await _send(member, payload)
        elif kind == CLOSED and len(data) >= 7:
            member = room.members.get(struct.unpack(">I", data[1:5])[0])
            if member is not None:
                code = struct.unpack(">H", data[5:7])[0]
                await member.close(code if 1000 <= code < 5000 else 1000, data[7:].decode("utf-8", "replace")[:120])

    async def _member(self, websocket, room_id: str) -> None:
        room = self.rooms.get(room_id)
        if room is None:
            await websocket.close(1008, "No such room on this relay")
            return
        member_id = room.next_member
        room.next_member += 1
        room.members[member_id] = websocket
        log.info("A member at %s joined room %s (%d in it)", _remote(websocket), room_id, len(room.members))
        prefix = bytes([DATA]) + struct.pack(">I", member_id)
        try:
            await room.host.send(bytes([OPENED]) + struct.pack(">I", member_id))
            async for data in websocket:
                raw = data.encode("utf-8") if isinstance(data, str) else data
                if not self._allowed(room, len(raw)):
                    await websocket.close(1008, "Over the relay's rate for a room")
                    break
                room.bytes_in += len(raw)
                # Text from a member arrives at the host as binary: the host's protocol
                # takes a text frame's bytes the same way (PEERS_PROTOCOL.md §2).
                await room.host.send(prefix + raw)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            room.members.pop(member_id, None)
            code = websocket.close_code or 1006
            reason = (websocket.close_reason or "").encode("utf-8")[:120]
            try:
                await room.host.send(bytes([CLOSED]) + struct.pack(">IH", member_id, code) + reason)
            except websockets.exceptions.ConnectionClosed:
                pass
            log.info("A member left room %s (%d in it)", room_id, len(room.members))

    def _allowed(self, room: _Room, size: int) -> bool:
        now = time.monotonic()
        if now - room.window_start >= 1.0:
            room.window_start = now
            room.window_bytes = 0
        room.window_bytes += size
        return room.window_bytes <= self.limits.bytes_per_second


async def _send(websocket, data: bytes) -> None:
    try:
        await websocket.send(data)
    except websockets.exceptions.ConnectionClosed:
        pass


def _remote(websocket) -> str:
    address = websocket.remote_address
    return f"{address[0]}" if address else "?"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="usdcc-relay", description="Forwards usdcc rooms it can't read (PEERS.md §5.4).")
    parser.add_argument("--address", default="0.0.0.0", help="where to listen (default: every address)")
    parser.add_argument("--port", type=int, default=8443, help="the port (default: 8443; 0: any)")
    parser.add_argument("--token-file", help="a file holding the token a host needs to open a room")
    parser.add_argument("--certfile", help="TLS certificate chain (PEM: the relay's, then its CA's), for wss:// without a reverse proxy")
    parser.add_argument("--keyfile", help="its private key (PEM)")
    parser.add_argument("--max-members", type=int, default=50, help="members per room (default: 50)")
    parser.add_argument("--max-rooms", type=int, default=10000)
    parser.add_argument("--max-frame-mb", type=float, default=8, help="one message, in MB (default: 8)")
    parser.add_argument("--max-mb-per-second", type=float, default=32, help="per room, in MB (default: 32)")
    parser.add_argument("--print-port", action="store_true", help="print the port as JSON on standard output")
    parser.add_argument("--stop-on-eof", action="store_true", help="stop when standard input closes")
    parser.add_argument("--log-level", default="info", choices=("debug", "info", "warning", "error"))
    arguments = parser.parse_args(argv)
    logging.basicConfig(level=arguments.log_level.upper(), format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("websockets").setLevel(logging.WARNING)
    token = ""
    if arguments.token_file:
        with open(arguments.token_file, encoding="utf-8") as file:
            token = file.read().strip()
    context = None
    if arguments.certfile:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(arguments.certfile, arguments.keyfile)
    limits = RelayLimits(members=arguments.max_members, rooms=arguments.max_rooms,
                         frame_bytes=int(arguments.max_frame_mb * 1024 * 1024),
                         bytes_per_second=int(arguments.max_mb_per_second * 1024 * 1024))

    async def run() -> None:
        relay = Relay(token, limits)
        await relay.start(arguments.address, arguments.port, context)
        if arguments.print_port:
            print(json.dumps({"port": relay.port}), flush=True)
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop.set)
            except (NotImplementedError, RuntimeError):
                pass
        if arguments.stop_on_eof:
            import threading

            def watch() -> None:
                sys.stdin.read()
                loop.call_soon_threadsafe(stop.set)

            threading.Thread(target=watch, daemon=True).start()
        try:
            await stop.wait()
        finally:
            await relay.close()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
