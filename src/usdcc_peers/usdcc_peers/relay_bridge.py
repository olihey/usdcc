"""A Host reached through a relay (PEERS.md §5.4), as the app's relay_bridge.cpp.

    room = Host(name="Studio")
    await room.start("127.0.0.1", 0)
    bridge = RelayBridge(room, "wss://relay.example", token="…")
    await bridge.start()
    print(bridge.link())

The bridge connects out to the relay and gets a room id there. Each member that connects
to that room gets a WebSocket of its own to the host's port on the loopback, so the host
takes it like any member. A message the host sealed once for the room goes to the relay
once, with the members it is for.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import struct

import websockets
from websockets.asyncio.client import connect

from .crypto import ROOM_MARKER
from .frame import JoinLink, make_link
from .relay import CLOSED, DATA, DATA_MANY, OPENED
from .room import log


class RelayBridge:
    def __init__(self, host, relay: str, token: str = "", ssl=None):
        self.host = host
        self.relay = relay.rstrip("/")
        self.token = token
        self.ssl = ssl
        self.room_id = ""
        self.copies_saved = 0
        self._socket = None
        self._members: dict[int, object] = {}
        self._tasks: dict[int, asyncio.Task] = {}
        self._batch: list[tuple[bytes, list[int]]] = []
        self._flushing = False
        self._task: asyncio.Task | None = None
        self._ready = asyncio.Event()
        self._stopped = False

    async def start(self, timeout: float = 15.0) -> None:
        self._task = asyncio.create_task(self._run())
        await asyncio.wait_for(self._ready.wait(), timeout)

    def room_url(self) -> str:
        return f"{self.relay}/r/{self.room_id}"

    def link(self) -> str:
        return make_link(JoinLink(relay=self.room_url(), fingerprint=self.host.me.fingerprint,
                                  secret=self.host.secret))

    async def close(self) -> None:
        self._stopped = True
        if self._task:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._task

    async def _run(self) -> None:
        retries = 0
        while not self._stopped:
            url = f"{self.relay}/host" + (f"?room={self.room_id}" if self.room_id else "")
            headers = {"Authorization": f"Bearer {self.token}"} if self.token else None
            tls = {"ssl": self.ssl} if self.ssl is not None and url.startswith("wss://") else {}
            try:
                async with connect(url, additional_headers=headers, max_size=64 * 1024 * 1024, compression=None,
                                   **tls) as socket:
                    self._socket = socket
                    async for data in socket:
                        if isinstance(data, str):
                            room = json.loads(data).get("room", "")
                            if room:
                                self.room_id = room
                                retries = 0
                                self._ready.set()
                                log.info("The relay has room %s as %s", self.host.name, room)
                            continue
                        await self._from_relay(data)
            except asyncio.CancelledError:
                raise
            except (OSError, websockets.exceptions.WebSocketException) as error:
                log.info("Lost the relay: %s", error)
            finally:
                self._socket = None
                for task in self._tasks.values():
                    task.cancel()
                for member in self._members.values():
                    with contextlib.suppress(Exception):
                        await member.close(1001, "The relay was lost")
                self._members.clear()
                self._tasks.clear()
                self._batch.clear()
            await asyncio.sleep(min(30.0, 0.5 * 2 ** min(retries, 6)))
            retries += 1

    async def _from_relay(self, data: bytes) -> None:
        if len(data) < 5:
            return
        member_id = struct.unpack(">I", data[1:5])[0]
        if data[0] == OPENED:
            self._tasks[member_id] = asyncio.create_task(self._member(member_id))
        elif data[0] == DATA:
            member = self._members.get(member_id)
            if member is None:
                await asyncio.sleep(0)  # Its loopback connection is still opening
                for _ in range(200):
                    member = self._members.get(member_id)
                    if member is not None or member_id not in self._tasks:
                        break
                    await asyncio.sleep(0.005)
            if member is not None:
                with contextlib.suppress(websockets.exceptions.ConnectionClosed):
                    await member.send(data[5:])
        elif data[0] == CLOSED:
            member = self._members.pop(member_id, None)
            task = self._tasks.pop(member_id, None)
            if member is not None:
                # As the member's own connection would end: the host keeps it for a while.
                member.transport.abort()
            if task:
                task.cancel()

    async def _member(self, member_id: int) -> None:
        try:
            async with connect(f"ws://127.0.0.1:{self.host.port}/ws", max_size=64 * 1024 * 1024,
                               compression=None, ping_interval=None) as member:
                self._members[member_id] = member
                async for data in member:
                    raw = data.encode("utf-8") if isinstance(data, str) else data
                    await self._from_room(member_id, raw)
                code, reason = member.close_code or 1000, member.close_reason or ""
        except (OSError, websockets.exceptions.WebSocketException):
            code, reason = 1006, ""
        except asyncio.CancelledError:
            return
        if self._members.pop(member_id, None) is not None or member_id in self._tasks:
            self._tasks.pop(member_id, None)
            await self._flush()
            await self._send(bytes([CLOSED]) + struct.pack(">IH", member_id, code) + reason.encode()[:120])

    async def _from_room(self, member_id: int, data: bytes) -> None:
        if data[:1] == bytes([ROOM_MARKER]):
            for payload, members in self._batch:
                if payload == data and member_id not in members:
                    members.append(member_id)
                    return
            self._batch.append((data, [member_id]))
            if not self._flushing:
                self._flushing = True
                asyncio.get_running_loop().call_later(0.001, lambda: asyncio.ensure_future(self._flush()))
            return
        await self._flush()
        await self._send(bytes([DATA]) + struct.pack(">I", member_id) + data)

    async def _flush(self) -> None:
        self._flushing = False
        batch, self._batch = self._batch, []
        for payload, members in batch:
            if len(members) == 1:
                await self._send(bytes([DATA]) + struct.pack(">I", members[0]) + payload)
            else:
                self.copies_saved += (len(members) - 1) * len(payload)
                await self._send(bytes([DATA_MANY]) + struct.pack(f">H{len(members)}I", len(members), *members)
                                 + payload)

    async def _send(self, frame: bytes) -> None:
        if self._socket is not None:
            with contextlib.suppress(websockets.exceptions.ConnectionClosed):
                await self._socket.send(frame)
