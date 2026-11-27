"""What waits to go out on one WebSocket (PEERS.md §7.4), as the app's SendQueue.

Frames wait here, where a latest-only one can still replace an older one of its kind,
and the queue's size says how far behind the other side is. A frame sealed for the
connection is sealed only as it is handed over: its nonce is the connection's counter,
so replacing a waiting one mustn't reorder them.
"""

from __future__ import annotations

import asyncio
from collections import deque


class Outbox:
    def __init__(self, sealer=None):
        self._entries: deque[list] = deque()  # [data, key, seal]
        self._ready = asyncio.Event()
        self.sealer = sealer
        self.queued_bytes = 0
        self.open = True
        self.closed = False
        self._sending = False

    def push(self, data: bytes, key: str = "", seal: bool = False) -> None:
        if self.closed:
            return
        if key:
            for entry in self._entries:
                if entry[1] == key:
                    self.queued_bytes += len(data) - len(entry[0])
                    entry[0] = data
                    entry[2] = seal
                    return
        self._entries.append([data, key, seal])
        self.queued_bytes += len(data)
        if self.open:
            self._ready.set()

    def set_open(self, open_: bool) -> None:
        self.open = open_
        if open_ and self._entries:
            self._ready.set()

    def clear(self) -> None:
        self._entries.clear()
        self.queued_bytes = 0

    async def drain(self, timeout: float) -> bool:
        """Waits until what's queued has been handed to the socket; False if it took too long."""
        loop = asyncio.get_running_loop()
        end = loop.time() + timeout
        while (self._entries or self._sending) and self.open and not self.closed:
            if loop.time() >= end:
                return False
            await asyncio.sleep(0.01)
        return not self._entries

    def close(self) -> None:
        self.closed = True
        self.clear()
        self._ready.set()

    async def run(self, websocket) -> None:
        """Hands frames to the socket until closed; returns when the socket fails."""
        while not self.closed:
            if not self._entries or not self.open:
                self._ready.clear()
                await self._ready.wait()
                continue
            data, _, seal = self._entries.popleft()
            self.queued_bytes -= len(data)
            if seal and self.sealer:
                data = self.sealer(data)
            self._sending = True
            try:
                await websocket.send(data)
            finally:
                self._sending = False
