"""usdcc.peers: the rooms this usdcc is in, for scripts and plugins (docs/PEERS.md §8.2).

A plugin that keeps notes on prims, shared by everyone in a room:

    import usdcc.peers, usdcc.toast

    def activate(plugin):
        usdcc.peers.connect("room_joined", lambda room: share(plugin, room))

    def share(plugin, room):
        notes = room.state(plugin.id + ".notes")       # equal on every peer, late joiners too
        notes.on_change(lambda key, value, by: print(key, value, by.name))
        pings = room.channel(plugin.id + ".ping")      # things that happen
        pings.on_message(lambda m: usdcc.toast.show(m.sender.name, m.body["text"]))

    def add_note(room, plugin, prim_path, text):
        room.state(plugin.id + ".notes").set(prim_path, {"text": text, "by": usdcc.peers.me()["name"]})
        room.channel(plugin.id + ".ping").send("added", {"text": "New note on " + prim_path})

- **Callbacks run on the main thread,** like every script: a slow one blocks the UI, not the
  network.
- **Bodies and values are JSON:** dicts, lists, strings, numbers, booleans and None. `data` is
  bytes.
- **A plugin's channels are its own:** name them with its id (`plugin.id + ".notes"`). What a
  plugin connected ends when it is turned off or reloaded, as its panels do. Both sides need the
  plugin: a peer without it ignores its channels.
- **The Peers panel shows every room,** whoever opened it, and the user can leave any of them.

Programs that aren't usdcc use the `usdcc_peers` package instead (src/usdcc_peers), with the
same calls where they make sense.

Needs the running app.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from usdcc import _plugins

try:
    import _usdcc_app as _native  # compiled into the app
except ImportError:
    _native = None

DELIVERIES = ("ordered", "latest", "direct")
_MISSING = object()


def _app():
    if _native is None or not _native.peers_available():
        raise RuntimeError("usdcc.peers needs the running usdcc app")
    return _native


@dataclass(frozen=True)
class Peer:
    """Someone in a room: the host, a peer (a usdcc or another program), or a guest."""

    id: str
    name: str
    kind: str  # "host", "peer" or "guest"
    fingerprint: str = ""  # Of its key: the same for every instance of one profile
    color: str = ""
    app: str = ""  # "usdcc 26.11", or another program's own name

    @classmethod
    def _of(cls, data: dict) -> Peer:
        return cls(**data)


@dataclass(frozen=True)
class Message:
    channel: str
    type: str
    body: dict
    data: bytes
    sender: Peer  # Set by the host: a sender can't pose as another
    to: str  # A member's id, or "" for everyone
    seq: int  # The host's number for an ordered message to everyone; 0 otherwise
    sealed: bool  # Sent privately (Channel.send_private()): only its sender and this member could read it


class Connection:
    """What on_message(), on_change() and connect() return. disconnect() ends it; it ends by
    itself when the plugin that made it is turned off, or when the app quits."""

    def __init__(self, native_id: int):
        self._id = native_id

    def disconnect(self) -> None:
        if self._id and _native is not None:
            _native.peers_disconnect(self._id)
        self._id = 0


def _track(native_id: int, callback) -> Connection:
    connection = Connection(native_id)
    _plugins.track(connection.disconnect, callback)
    return connection


def _dumps(value) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def _value(text: str):
    return _MISSING if not text else json.loads(text)[0]


class Channel:
    """A named stream of messages in a room (PEERS.md §7.1)."""

    def __init__(self, room: Room, name: str, delivery: str):
        self.room = room
        self.name = name
        self.delivery = delivery

    def send(self, type: str, body: dict | None = None, data: bytes | None = None, to: str | Peer | None = None) -> None:
        """To everyone in the room, or to one member (`to`: a Peer or its id). On a "direct"
        channel, nobody named means the host."""
        _app().room_send(self.room._handle, self.name, self.delivery, type, _dumps(body or {}), bytes(data or b""),
                         _id_of(to))

    def send_private(self, type: str, body: dict, to: str | Peer) -> bool:
        """Sealed for one member's key: the host passes it on but can't read it (PEERS.md §6.2).
        False, and nothing sent, for a member without a key (a guest)."""
        return _app().room_send_private(self.room._handle, self.name, self.delivery, type, _dumps(body), _id_of(to))

    def on_message(self, callback: Callable[[Message], Any]) -> Connection:
        room = self.room

        def received(message: dict) -> None:
            sender = room.peer(message["sender"]) or Peer(message["sender"], "", "peer")
            callback(Message(message["channel"], message["type"], json.loads(message["body"] or "{}"),
                             message["data"], sender, message["to"], message["seq"], message["sealed"]))

        return _track(_app().room_listen(room._handle, self.name, self.delivery, received, None), callback)

    def on_refused(self, callback: Callable[[str, str], Any]) -> Connection:
        """`callback(type, reason)`: the host turned down something this instance sent here."""
        return _track(_app().room_listen(self.room._handle, self.name, self.delivery, None, callback), callback)


class StateMap:
    """Keys with JSON values, equal on every peer (PEERS.md §7.2). A change shows here at once;
    the host's order decides in the end: for one key the last writer wins."""

    def __init__(self, room: Room, name: str):
        self.room = room
        self.name = name

    def items(self) -> dict:
        return _value(_app().room_state_items(self.room._handle, self.name))

    def get(self, key: str, default=None):
        return self.items().get(key, default)

    def __contains__(self, key: str) -> bool:
        return key in self.items()

    def set(self, key: str, value) -> None:
        _app().room_state_set(self.room._handle, self.name, key, _dumps([value]))

    def remove(self, key: str) -> None:
        _app().room_state_set(self.room._handle, self.name, key, None)

    def on_change(self, callback: Callable[[str, Any, Peer], Any]) -> Connection:
        """`callback(key, value, by)`; `value` is None for a removed key."""

        def changed(key: str, value: str, by: dict) -> None:
            found = _value(value)
            callback(key, None if found is _MISSING else found, Peer._of(by))

        return _track(_app().room_state_listen(self.room._handle, self.name, changed), callback)


class PeerState:
    """One value per peer, set only by that peer, gone when it leaves (PEERS.md §7.2)."""

    def __init__(self, room: Room, name: str):
        self.room = room
        self.name = name

    def set(self, value) -> None:
        """This instance's own; None removes it."""
        _app().room_peer_state_set(self.room._handle, self.name, None if value is None else _dumps([value]))

    def values(self) -> dict[str, Any]:
        """By peer id."""
        return {peer: _value(text) for peer, text in _app().room_peer_state_values(self.room._handle, self.name).items()}

    def get(self, peer: str | Peer, default=None):
        return self.values().get(_id_of(peer), default)

    def on_change(self, callback: Callable[[Peer, Any], Any]) -> Connection:
        """`callback(peer, value)`; `value` is None when the peer removed it or left."""

        def changed(peer: dict, value: str) -> None:
            found = _value(value)
            callback(Peer._of(peer), None if found is _MISSING else found)

        return _track(_app().room_peer_state_listen(self.room._handle, self.name, changed), callback)


class Room:
    """A room this instance hosts or is in. Gone once it closes: its calls then raise."""

    def __init__(self, handle: int):
        self._handle = handle

    def __eq__(self, other) -> bool:
        return isinstance(other, Room) and other._handle == self._handle

    def __hash__(self) -> int:
        return hash(self._handle)

    def __repr__(self) -> str:
        try:
            return f"<usdcc.peers.Room {self.name!r}>"
        except RuntimeError:
            return "<usdcc.peers.Room (closed)>"

    @property
    def _info(self) -> dict:
        return _app().room_info(self._handle)

    @property
    def name(self) -> str:
        return self._info["name"]

    @property
    def type(self) -> str:
        """"chat", "broadcast" or "render_queue" (PEERS.md §5.5)."""
        return self._info["type"]

    @property
    def is_host(self) -> bool:
        return self._info["is_host"]

    @property
    def is_open(self) -> bool:
        return _app().room_is_open(self._handle) and self._info["state"] == 1

    @property
    def link(self) -> str:
        """The invite link, on the host; "" on a member, or a relayed room the relay hasn't got yet."""
        return self._info["link"]

    @property
    def me(self) -> Peer:
        return Peer._of(self._info["me"])

    @property
    def host(self) -> Peer:
        return Peer._of(self._info["host"])

    def peers(self) -> list[Peer]:
        """Everyone in the room, this instance included."""
        return [Peer._of(peer) for peer in _app().room_peers(self._handle)]

    def peer(self, id: str) -> Peer | None:
        return next((peer for peer in self.peers() if peer.id == id), None)

    def find(self, who: str) -> list[Peer]:
        """The peers `who` names: an id, else a name (any case)."""
        everyone = self.peers()
        exact = [peer for peer in everyone if peer.id == who]
        return exact or [peer for peer in everyone if peer.name.casefold() == who.casefold()]

    def channel(self, name: str, delivery: str = "ordered") -> Channel:
        """A channel (PEERS.md §7.1): "ordered" (everyone, in one order), "latest" (a newer
        message replaces one still waiting) or "direct" (to one member)."""
        if delivery not in DELIVERIES:
            raise ValueError(f"delivery is one of {', '.join(DELIVERIES)}")
        return Channel(self, name, delivery)

    def state(self, name: str) -> StateMap:
        return StateMap(self, name)

    def peer_state(self, name: str) -> PeerState:
        return PeerState(self, name)

    def notify(self, title: str, body: str = "", kind: str = "info", to: str | Peer | None = None, *, id: str = "",
               sticky: bool = False, actions: list[dict] | None = None) -> None:
        """A notification (PEERS.md §10.2): a toast on everyone in the room, or on the one `to`
        names (a Peer, an id or a name). The same `id` again replaces the toast that is up.
        Actions: {"label", and one of "open" (a URL), "openStage", "select" (prim paths),
        "copy"}."""
        notification = {"title": title, "body": body, "kind": kind, "sticky": sticky}
        if id:
            notification["id"] = id
        if actions:
            notification["actions"] = actions
        target = ""
        if to:
            target = _id_of(to)
            if not isinstance(to, Peer):
                found = self.find(target)
                if not found:
                    raise ValueError(f"Nobody in {self.name} has the name or id {target!r}")
                target = found[0].id
        _app().room_notify(self._handle, _dumps(notification), target)

    def send_file(self, channel: str, path: str, to: str | Peer | None = None) -> int:
        """Sends a file, or a folder with everything in it, on a channel of the plugin's own
        (PEERS.md §7.3): to one member, or to each. In pieces, checked against their hashes,
        resuming after a lost connection. Returns how many it was offered to."""
        return _app().room_send_files(self._handle, channel, str(path), _id_of(to))

    def on_file(self, channel: str, callback: Callable[[str, list[str], Peer], Any],
                accept: Callable[[Peer, list[str], int], bool] | None = None) -> Connection:
        """Takes files sent on `channel`: `callback(folder, files, sender)` once they arrived,
        `folder` holding them at their paths. `accept(sender, files, bytes)` says whether to take
        an offer (without it, every one up to Settings > Peers > Receive up to). A bundle that is
        here already isn't sent again."""
        native_accept = None
        if accept is not None:
            native_accept = lambda peer, files, size: bool(accept(Peer._of(peer), files, size))  # noqa: E731
        return _track(_app().room_on_files(self._handle, channel,
                                           lambda folder, files, peer: callback(folder, files, Peer._of(peer)),
                                           native_accept), callback)

    def on(self, event: str, callback: Callable[..., Any]) -> Connection:
        """"peer_joined" (peer), "peer_left" (peer, reason) or "closed" (reason)."""
        if event == "peer_joined":
            native = lambda peer: callback(Peer._of(peer))  # noqa: E731
        elif event == "peer_left":
            native = lambda peer, reason: callback(Peer._of(peer), reason)  # noqa: E731
        else:
            native = callback
        return _track(_app().room_on(self._handle, event, native), callback)

    def close(self, reason: str = "") -> None:
        """Leaves the room, or on the host, closes it for everyone."""
        _app().room_close(self._handle, reason)


def _id_of(who) -> str:
    if who is None:
        return ""
    return who.id if isinstance(who, Peer) else str(who)


def rooms() -> list[Room]:
    """The rooms this instance hosts or is in, of every kind."""
    return [Room(handle) for handle in _app().peers_rooms()]


def me() -> dict:
    """This instance's name, color and app, as Settings > Peers has them."""
    return dict(_app().peers_me())


def host(name: str = "", admission: str = "invited", password: str = "", through_relay: bool = False,
         type: str = "chat", on_network: bool = True) -> Room:
    """Hosts a room, as Host Room does: `admission` "invited" (only those with the link),
    "ask" (anyone on this network, once allowed) or "open". `through_relay` uses the relay of
    Settings > Peers. `on_network` False keeps it to this machine (127.0.0.1). Its `link`
    invites people."""
    return Room(_app().peers_host(name, admission, password, through_relay, type, on_network))


def join(link: str, password: str = "", type: str = "chat") -> Room:
    """Joins a usdcc:// link, or asks a host:port. The room opens once the host lets it in:
    connect("room_joined", …) says when."""
    return Room(_app().peers_join(link, password, type))


def connect(event: str, callback: Callable[[Room], Any]) -> Connection:
    """"room_joined": a room opened (hosted, or joined and let in); "room_left": it closed.
    For who comes and goes in one room, use its on()."""
    return _track(_app().peers_connect(event, lambda handle: callback(Room(handle))), callback)
