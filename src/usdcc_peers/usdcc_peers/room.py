"""What a room holds, on a member and on a host alike: channels, shared state, per-peer
state (PEERS.md §7), and the callbacks a program registers.

Everything runs on one asyncio event loop. Callbacks are called on it; one that raises
is logged and doesn't stop the room.
"""

from __future__ import annotations

import logging
from collections.abc import Callable

from .frame import Message, Peer, is_valid_channel_name

log = logging.getLogger("usdcc_peers")

MISSING = object()  # A key that isn't in a map: unlike None, which is JSON's null
MAX_VALUE_BYTES = 256 * 1024
MAX_MAP_BYTES = 16 * 1024 * 1024
DELIVERIES = ("ordered", "latest", "direct")


def _call(callbacks, *arguments) -> None:
    for callback in list(callbacks):
        try:
            callback(*arguments)
        except Exception:
            log.exception("A callback failed")


class Senders:
    """Who may send on a channel, or write a map, on the host (PEERS.md §6). The host may always."""

    EVERYONE = "everyone"
    HOST = "host"
    NAMED = "named"


def _may(senders: str, named: list[str], peer: Peer) -> bool:
    if peer.kind == "host":
        return True
    if peer.kind == "guest":
        return False
    if senders == Senders.EVERYONE:
        return True
    if senders == Senders.NAMED:
        # A peer's id, or its fingerprint: every instance of that key.
        return peer.id in named or (bool(peer.fingerprint) and peer.fingerprint in named)
    return False


class Channel:
    """A named stream of messages in a room (PEERS.md §7.1)."""

    def __init__(self, room, name: str, delivery: str):
        self.room = room
        self.name = name
        self.delivery = delivery
        self.senders = Senders.EVERYONE
        self.named_senders: list[str] = []
        self.open_to_guests = False
        self._on_message: list[Callable[[Message], None]] = []
        self._on_refused: list[Callable[[str, str], None]] = []

    def send(self, type_: str, body: dict | None = None, data: bytes | None = None, to: str | None = None) -> None:
        """To everyone, or to one member. On a direct channel, nobody named means the host."""
        self.room._send(self, type_, body or {}, data or b"", to or "")

    def send_private(self, type_: str, body: dict, to: str) -> bool:
        """Sealed for one member's key, which the host passes on but can't read (PEERS.md §6.2)."""
        return self.room._send_private(self, type_, body, to)

    def on_message(self, callback: Callable[[Message], None]) -> Callable[[Message], None]:
        self._on_message.append(callback)
        return callback

    def on_refused(self, callback: Callable[[str, str], None]) -> Callable[[str, str], None]:
        """(type, reason): the host turned down something this member sent here."""
        self._on_refused.append(callback)
        return callback

    def set_senders(self, senders: str, named: list[str] | None = None) -> None:
        """The host's: who may send. `named` holds peer ids or fingerprints."""
        self.senders = senders
        self.named_senders = list(named or [])

    def may_send(self, peer: Peer) -> bool:
        if peer.kind == "guest" and not self.open_to_guests:
            return False
        return _may(self.senders, self.named_senders, peer)

    def _received(self, message: Message) -> None:
        _call(self._on_message, message)

    def _refused(self, type_: str, reason: str) -> None:
        _call(self._on_refused, type_, reason)


class StateMap:
    """Keys with JSON values, equal on every peer (PEERS.md §7.2).

    A change shows here at once and goes to the host, which orders it: for one key the
    last writer in the host's order wins. Callbacks get (key, value, by), with `value`
    MISSING for a removed key.
    """

    def __init__(self, room, name: str):
        self.room = room
        self.name = name
        self.senders = Senders.EVERYONE
        self.named_senders: list[str] = []
        self._confirmed: dict = {}
        self._pending: dict = {}  # key -> [value, count]: this member's changes not ordered yet
        self._on_change: list = []
        self._on_refused: list = []

    def _visible(self, key):
        if key in self._pending:
            return self._pending[key][0]
        return self._confirmed.get(key, MISSING)

    def get(self, key, default=None):
        value = self._visible(key)
        return default if value is MISSING else value

    def __contains__(self, key) -> bool:
        return self._visible(key) is not MISSING

    def keys(self) -> list:
        return sorted(key for key in set(self._confirmed) | set(self._pending) if key in self)

    def items(self) -> dict:
        return {key: self._visible(key) for key in self.keys()}

    def set(self, key: str, value) -> None:
        self._change(key, value)

    def remove(self, key: str) -> None:
        self._change(key, MISSING)

    def on_change(self, callback):
        self._on_change.append(callback)
        return callback

    def on_refused(self, callback):
        self._on_refused.append(callback)
        return callback

    def set_senders(self, senders: str, named: list[str] | None = None) -> None:
        self.senders = senders
        self.named_senders = list(named or [])

    def may_send(self, peer: Peer) -> bool:
        return _may(self.senders, self.named_senders, peer)

    def _change(self, key: str, value) -> None:
        if self.room.is_host:
            self.room._set_state(self, key, value, self.room.me)
            return
        if self.room.closed:
            return
        before = self._visible(key)
        pending = self._pending.setdefault(key, [value, 0])
        pending[0] = value
        pending[1] += 1
        if before != value:
            _call(self._on_change, key, value, self.room.me)
        self.room._send_state(self, key, value)

    def _apply(self, key: str, value, by: Peer, mine: bool) -> None:
        before = self._visible(key)
        if value is MISSING:
            self._confirmed.pop(key, None)
        else:
            self._confirmed[key] = value
        if mine and key in self._pending:
            self._pending[key][1] -= 1
            if self._pending[key][1] <= 0:
                del self._pending[key]
        after = self._visible(key)
        if before != after:
            _call(self._on_change, key, after, by)

    def _replace(self, items: dict) -> None:
        touched = set(self._confirmed) | set(items)
        before = {key: self._visible(key) for key in touched}
        self._confirmed = dict(items)
        for key in touched:
            after = self._visible(key)
            if before[key] != after:
                _call(self._on_change, key, after, self.room.host)

    def _refuse(self, key: str, reason: str) -> None:
        before = self._visible(key)
        if key in self._pending:
            self._pending[key][1] -= 1
            if self._pending[key][1] <= 0:
                del self._pending[key]
        after = self._visible(key)
        if before != after:
            _call(self._on_change, key, after, self.room.host)
        _call(self._on_refused, key, reason)


class PeerState:
    """One value per peer, set only by that peer, gone when it leaves (PEERS.md §7.2).

    Callbacks get (peer, value), with `value` MISSING when the peer removed it or left.
    """

    def __init__(self, room, name: str):
        self.room = room
        self.name = name
        self.open_to_guests = False
        self.values: dict[str, object] = {}
        self._on_change: list = []

    def set(self, value) -> None:
        """This peer's own; None removes it."""
        self.room._send_peer_state(self, MISSING if value is None else value)

    def get(self, peer_id: str, default=None):
        return self.values.get(peer_id, default)

    def on_change(self, callback):
        self._on_change.append(callback)
        return callback

    def _apply(self, peer: Peer, value) -> None:
        if value is MISSING or value is None:
            if self.values.pop(peer.id, MISSING) is not MISSING:
                _call(self._on_change, peer, MISSING)
            return
        if peer.id in self.values and self.values[peer.id] == value:
            return
        self.values[peer.id] = value
        _call(self._on_change, peer, value)


class RoomBase:
    """What a host and a member share."""

    is_host = False

    def __init__(self):
        self.name = ""
        self.type = "chat"
        self.me: Peer = Peer(id="")
        self.host: Peer = Peer(id="")
        self.closed = False
        self.close_reason = ""
        self.channels: dict[str, Channel] = {}
        self.maps: dict[str, StateMap] = {}
        self.peer_states: dict[str, PeerState] = {}
        self._callbacks: dict[str, list] = {}

    # --- What a program calls -------------------------------------------------------

    def channel(self, name: str, delivery: str = "ordered") -> Channel:
        existing = self.channels.get(name)
        if existing:
            return existing
        if not is_valid_channel_name(name):
            raise ValueError(f"Not a channel name: {name!r}")
        if delivery not in DELIVERIES:
            raise ValueError(f"Not a delivery: {delivery!r}; one of {', '.join(DELIVERIES)}")
        channel = Channel(self, name, delivery)
        self.channels[name] = channel
        return channel

    def state(self, name: str) -> StateMap:
        existing = self.maps.get(name)
        if existing:
            return existing
        if not is_valid_channel_name(name):
            raise ValueError(f"Not a map name: {name!r}")
        state = StateMap(self, name)
        self.maps[name] = state
        return state

    def peer_state(self, name: str) -> PeerState:
        existing = self.peer_states.get(name)
        if existing:
            return existing
        if not is_valid_channel_name(name):
            raise ValueError(f"Not a map name: {name!r}")
        state = PeerState(self, name)
        self.peer_states[name] = state
        return state

    def on(self, event: str, callback):
        """`peer_joined` (peer), `peer_left` (peer, reason), `closed` (reason),
        `resumed` (caught_up), `renamed` (name); a host also `join_requested` (request)."""
        self._callbacks.setdefault(event, []).append(callback)
        return callback

    def peer(self, peer_id: str) -> Peer | None:
        return next((peer for peer in self.peers() if peer.id == peer_id), None)

    def find(self, who: str) -> list[Peer]:
        """The peers `who` names: an id, else a name (any case)."""
        exact = [peer for peer in self.peers() if peer.id == who]
        if exact:
            return exact
        folded = who.casefold()
        return [peer for peer in self.peers() if peer.name.casefold() == folded]

    def notify(self, title: str, body: str = "", kind: str = "info", to: str | None = None, *, id: str = "",
               sticky: bool = False, actions: list | None = None) -> int:
        """A notification on the `notify` channel (PEERS.md §10.2): a toast on every member,
        or on those `to` names (an id or a name). Returns how many it went to by name;
        0 with `to` means nobody had that id or name."""
        from .notify import notification

        message = notification(title, body, kind, id=id, sticky=sticky, actions=actions)
        channel = self.channel("notify")
        if not to:
            channel.send("notify", message)
            return len(self.peers())
        targets = [peer for peer in self.find(to) if peer.id != self.me.id]
        for peer in targets:
            channel.send("notify", message, to=peer.id)
        return len(targets)

    # --- Shared ------------------------------------------------------------------------

    def _emit(self, event: str, *arguments) -> None:
        _call(self._callbacks.get(event, []), *arguments)

    def peers(self) -> list[Peer]:
        raise NotImplementedError

    def _deliver_sealed(self, channel: Channel, message: Message) -> None:
        raise NotImplementedError
