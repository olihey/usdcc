"""Rooms for a plain script, with no asyncio of its own (PEERS.md §9).

    import usdcc_peers
    room = usdcc_peers.join(link, name="Render Farm", key_file="farm.key")
    room.notify("Render finished", "shot_012 · 240 frames", kind="success", to="anna")
    room.state("farm.jobs").set("8812", {"shot": "shot_012", "progress": 1.0})
    room.close()

The room lives on an event loop in a thread of its own, one for all the rooms a
program joins. A call from the script runs there and waits for it; callbacks run
there too, so one that takes long holds up the room's messages, not the script.
"""

from __future__ import annotations

import asyncio
import atexit
import threading

from . import crypto
from .member import Member, default_key_file

_loop: asyncio.AbstractEventLoop | None = None
_lock = threading.Lock()


def _event_loop() -> asyncio.AbstractEventLoop:
    global _loop
    with _lock:
        if _loop is None:
            _loop = asyncio.new_event_loop()
            thread = threading.Thread(target=_loop.run_forever, name="usdcc_peers", daemon=True)
            thread.start()
        return _loop


def _run(coroutine, timeout: float | None = None):
    return asyncio.run_coroutine_threadsafe(coroutine, _event_loop()).result(timeout)


def _call(function, *arguments):
    async def call():
        return function(*arguments)

    return _run(call())


class _Proxy:
    """Calls an object's methods on the rooms' loop."""

    def __init__(self, target):
        object.__setattr__(self, "_target", target)

    def __getattr__(self, name):
        value = getattr(self._target, name)
        if not callable(value):
            return value

        def call(*arguments, **keywords):
            async def run():
                return _wrap(value(*arguments, **keywords))

            return _run(run())

        return call


def _wrap(value):
    from .room import Channel, PeerState, StateMap

    if isinstance(value, (Channel, StateMap, PeerState)):
        return _Proxy(value)
    return value


class Room(_Proxy):
    """A joined room. Every method of Member, called from any thread."""

    def close(self, reason: str = "") -> None:
        _run(self._target.close(reason))

    def wait_closed(self, timeout: float | None = None) -> str:
        return _run(self._target.wait_closed(), timeout)


def join(where: str, *, name: str = "", key_file: str | None = None, identity: crypto.Identity | None = None,
         timeout: float = 15.0, **options) -> Room:
    """Joins a usdcc:// link or "host:port"; usdcc_peers.JoinError if it can't.

    The program's key is read from `key_file`, made there on first use; without one, from
    ~/.config/usdcc_peers/identity.key. A key is who the program is to the rooms it joins.
    """
    if identity is None:
        identity = crypto.Identity.load_or_create(key_file or default_key_file())
    room = _run(Member.join(where, name=name, identity=identity, timeout=timeout, **options))
    atexit.register(_leave_at_exit, room)
    return Room(room)


def _leave_at_exit(room: Member) -> None:
    if not room.closed and _loop is not None and _loop.is_running():
        try:
            _run(room.close("The program ended"), 2.0)
        except Exception:  # noqa: BLE001 - ending anyway
            pass
