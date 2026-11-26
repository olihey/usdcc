"""A member: joins a room another program hosts (docs/PEERS_PROTOCOL.md §3, §5, §6).

    room = await Member.join("usdcc://join/10.0.0.5:47800#k=…&s=…", name="Render Farm",
                             identity=Identity.load_or_create("farm.key"))
    room.notify("Render finished", "shot_012", kind="success")
    await room.close()

It does what the app's joiner does: checks the host's key against the link, seals what
it sends for the connection, opens what the host seals for the room, and after a lost
connection tries again for a minute, catching up on what it missed.
"""

from __future__ import annotations

import asyncio
import json
import os
import time

import websockets
from websockets.asyncio.client import connect

from . import crypto
from ._outbox import Outbox
from .frame import (
    PROTOCOL,
    ROOM_CHANNEL,
    FrameError,
    JoinLink,
    Message,
    Peer,
    clean_color,
    clean_name,
    control,
    decode,
    decode_text,
    encode,
    parse_link,
)
from .room import MISSING, RoomBase, log

HOST_PROOF = b"usdcc-peers-1 host"
PEER_PROOF = b"usdcc-peers-1 peer"
SEALED_TYPE = "sealed"
OPENED_IDS_KEPT = 4096


class JoinError(ConnectionError):
    """The room couldn't be joined: unreachable, refused, or the wrong host."""


class Member(RoomBase):
    def __init__(self, *, identity: crypto.Identity | None = None, name: str = "", color: str = "", app: str = "",
                 instance: str = "", type: str = "chat", secret: str = "", password: str = "",
                 expected_fingerprint: str = "", reconnect_for: float = 60.0, ping_interval: float = 10.0,
                 silence_timeout: float = 30.0, max_size: int = 16 * 1024 * 1024, ssl=None):
        super().__init__()
        self.identity = identity or crypto.Identity.generate()
        self.profile_name = clean_name(name, "Peer")
        self.color = clean_color(color)
        self.app = app[:64]
        self.instance = (instance or crypto.random_id(6))[:32]
        self.type = type
        self.secret = secret
        self.password = password
        self.expected_fingerprint = expected_fingerprint
        self.reconnect_for = reconnect_for
        self.ping_interval = ping_interval
        self.silence_timeout = silence_timeout
        self.max_size = max_size
        self.ssl = ssl  # For wss://: an ssl.SSLContext, e.g. to trust a studio's own CA; None: the system's
        self.open_timeout = 10.0  # One connection's opening handshake; join() sets it from its timeout
        self.url = ""
        self.status = "connecting"  # connecting, open, reconnecting, closed
        self.host_key = b""
        self.room_keys: dict[int, bytes] = {}
        self.last_seq = 0
        self.joined_peers: dict[str, Peer] = {}  # By id, in the order they came
        self._ever_open = False
        self._ended = False
        self._outbox = Outbox()
        self._websocket = None
        self._task: asyncio.Task | None = None
        self._opened: asyncio.Future | None = None
        self._closed_event = asyncio.Event()
        self._opened_ids: dict[str, None] = {}

    # --- Starting ----------------------------------------------------------------------

    @classmethod
    async def join(cls, where: str, *, timeout: float = 15.0, **options) -> Member:
        """Joins a usdcc:// link, a "host:port" or a ws:// URL; JoinError if it can't."""
        link = parse_link(where)
        if link:
            options.setdefault("secret", link.secret)
            options.setdefault("expected_fingerprint", link.fingerprint)
        member = cls(**options)
        member.url = _url(link, where)
        member.open_timeout = max(10.0, timeout)
        await member.start(timeout)
        return member

    async def start(self, timeout: float = 15.0) -> None:
        loop = asyncio.get_running_loop()
        self._opened = loop.create_future()
        self._task = asyncio.create_task(self._run())
        try:
            await asyncio.wait_for(asyncio.shield(self._opened), timeout)
        except TimeoutError:
            await self.close("Didn't join in time")
            raise JoinError(f"Couldn't join {self.url}: no answer in {timeout:g} s") from None

    # --- What a program calls ----------------------------------------------------------

    def peers(self) -> list[Peer]:
        return list(self.joined_peers.values())

    def peer(self, peer_id: str) -> Peer | None:
        return self.joined_peers.get(peer_id)

    async def close(self, reason: str = "") -> None:
        """Leaves the room."""
        if self.closed:
            return
        reason = reason or "Left the room"
        self._ended = True
        websocket = self._websocket
        if websocket is not None and self.status == "open":
            self._outbox.push(encode(control("leave", {"reason": reason})), seal=True)
            await asyncio.sleep(0.05)
        self._finish(reason)
        if websocket is not None:
            await websocket.close(1000, reason[:120])
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass

    async def wait_closed(self) -> str:
        await self._closed_event.wait()
        return self.close_reason

    # --- Sending -----------------------------------------------------------------------

    def _send(self, channel, type_: str, body: dict, data: bytes, to: str) -> None:
        if self.closed:
            log.warning("Sent on %s after the room closed", channel.name)
            return
        latest = channel.delivery == "latest"
        if not to and channel.delivery == "direct" and self.host.id:
            to = self.host.id
        message = Message(channel=channel.name, type=type_, body=body, data=data, to=to)
        key = "\n".join((message.channel, message.type, "", message.to)) if latest else ""
        self._outbox.push(encode(message, latest), key, seal=True)

    def _send_private(self, channel, type_: str, body: dict, to: str) -> bool:
        target = self.peer(to)
        if not target or not target.public_key or target.id == self.me.id:
            return False
        inner = {"id": crypto.random_id(16), "channel": channel.name, "to": to, "type": type_, "body": body}
        box = crypto.seal_for_peer(self.identity, target.public_key,
                                   json.dumps(inner, separators=(",", ":")).encode())
        if not box:
            return False
        self._send(channel, SEALED_TYPE, {"box": crypto.b64(box)}, b"", to)
        return True

    def _send_state(self, state, key: str, value) -> None:
        body = {"map": state.name, "key": key}
        if value is not MISSING:
            body["value"] = value
        self._outbox.push(encode(control("remove" if value is MISSING else "set", body)), seal=True)

    def _send_peer_state(self, state, value) -> None:
        if self.closed:
            return
        state._apply(self.me, value)
        body = {"map": state.name}
        if value is not MISSING:
            body["value"] = value
        self._outbox.push(encode(control("peer.set", body)), "peer.set\n" + state.name, seal=True)

    # --- The connection ----------------------------------------------------------------

    async def _run(self) -> None:
        retries = 0
        lost_since = None
        while not self.closed:
            try:
                await self._connect_once()
                retries = 0
            except asyncio.CancelledError:
                raise
            except (OSError, websockets.exceptions.WebSocketException, JoinError) as error:
                if not self._ever_open:
                    self._fail(f"Couldn't reach the room: {error}")
                    return
                log.info("Lost the host of %s: %s", self.name, error)
            if self.closed:
                return
            if self._ended or not self._ever_open or self.reconnect_for <= 0:
                self._finish(self.close_reason or "The connection to the host was lost")
                return
            # Lost the host: try again for a while (PEERS.md §7.4).
            if lost_since is None or self.status != "reconnecting":
                lost_since = time.monotonic()
                self._outbox.set_open(False)
                self._set_state("reconnecting")
            if time.monotonic() - lost_since > self.reconnect_for:
                self._finish("Lost the connection to the host")
                return
            await asyncio.sleep(min(8.0, 0.25 * (2 ** min(retries, 5))))
            retries += 1

    async def _connect_once(self) -> None:
        tls = {"ssl": self.ssl} if self.ssl is not None and self.url.startswith("wss://") else {}
        async with connect(self.url, max_size=self.max_size, **tls, ping_interval=self.ping_interval,
                           ping_timeout=self.silence_timeout, open_timeout=self.open_timeout, compression=None) as websocket:
            self._websocket = websocket
            public, secret = crypto.kx_key_pair()
            await websocket.send(encode(control("hello", {"protocol": PROTOCOL, "eph": crypto.b64(public)})))
            tx = rx = None
            writer = None
            try:
                async for data in websocket:
                    text = isinstance(data, str)
                    raw = data.encode("utf-8") if text else data
                    if rx is None:
                        plain = raw
                    elif not text and raw[:1] == bytes([crypto.SEALED_MARKER]):
                        plain = rx.open(raw)
                    elif not text and raw[:1] == bytes([crypto.ROOM_MARKER]):
                        if crypto.room_epoch(raw) not in self.room_keys:
                            continue  # A latest-only message that overtook its key
                        plain = crypto.open_from_room(self.room_keys, self.host_key, raw)
                    else:
                        plain = None
                    if plain is None:
                        raise JoinError("The host sent something that couldn't be opened")
                    try:
                        message, _ = decode_text(plain.decode("utf-8")) if text else decode(plain)
                    except FrameError as error:
                        log.warning("The host sent something that isn't a frame: %s", error)
                        continue
                    if rx is None:
                        if message.channel == ROOM_CHANNEL and message.type == "challenge":
                            tx, rx = self._on_challenge(message, public, secret)
                            self._outbox = _carry(self._outbox, tx)
                            writer = asyncio.create_task(self._outbox.run(websocket))
                            writer.add_done_callback(lambda task, ws=websocket: _close_on_failure(task, ws))
                            auth = self._auth(public, crypto.unb64(message.body.get("eph")))
                            await websocket.send(tx.seal(encode(control("auth", auth))))
                            # The outbox seals with tx from here; nothing else has used it.
                        elif message.channel == ROOM_CHANNEL and message.type == "denied":
                            self._on_host_frame(message)
                        continue
                    self._on_host_frame(message)
                    if self.closed:
                        return
            finally:
                if writer:
                    writer.cancel()
                self._websocket = None

    def _on_challenge(self, message: Message, public: bytes, secret: bytes):
        body = message.body
        eph = crypto.unb64(body.get("eph"))
        key = crypto.unb64(body.get("key"))
        if body.get("protocol") != PROTOCOL:
            self._fail(f"The host speaks protocol {body.get('protocol')}, and this member protocol {PROTOCOL}")
            raise JoinError(self.close_reason)
        if not crypto.verify(key, HOST_PROOF + eph + public, crypto.unb64(body.get("sig"))):
            self._fail("The host's key didn't sign the handshake")
            raise JoinError(self.close_reason)
        if self.expected_fingerprint and crypto.fingerprint_of(key) != self.expected_fingerprint:
            self._fail("Wrong key: the host isn't the one the link names")
            raise JoinError(self.close_reason)
        if self.host_key and key != self.host_key:
            self._fail("Wrong key: another program answers at the host's address")
            raise JoinError(self.close_reason)
        keys = crypto.kx_session_keys(False, public, secret, eph)
        if not keys:
            self._fail("The host's handshake key is no good")
            raise JoinError(self.close_reason)
        self.host_key = key
        receive, send = keys
        return crypto.Sealer(send), crypto.Sealer(receive)

    def _auth(self, public: bytes, host_eph: bytes) -> dict:
        auth = {"key": crypto.b64(self.identity.public_key),
                "sig": crypto.b64(self.identity.sign(PEER_PROOF + public + host_eph)),
                "instance": self.instance, "name": self.profile_name, "color": self.color, "app": self.app}
        if self.secret:
            auth["secret"] = self.secret
        if self.password:
            auth["password"] = self.password
        if self._ever_open:
            auth["resume"] = {"id": self.me.id, "seq": self.last_seq}
        return auth

    # --- What the host says ------------------------------------------------------------

    def _on_host_frame(self, message: Message) -> None:
        if message.seq > 0:
            if message.seq <= self.last_seq:
                return  # Already had it, before a reconnect
            self.last_seq = message.seq
        if message.channel == ROOM_CHANNEL:
            self._on_control(message)
            return
        if self.status != "open":
            return
        channel = self.channels.get(message.channel)
        if channel:
            self._deliver(channel, message)

    def _on_control(self, message: Message) -> None:
        body = message.body
        reason = body.get("reason", "") if isinstance(body.get("reason"), str) else ""
        kind = message.type
        if kind == "welcome":
            self._on_welcome(body)
        elif kind == "joined":
            peer = Peer.from_json(body)
            if peer.id and peer.id != self.me.id and peer.id not in self.joined_peers:
                self.joined_peers[peer.id] = peer
                self._emit("peer_joined", peer)
        elif kind == "renamed":
            name = clean_name(body.get("name"), "")
            if name and name != self.name:
                self.name = name
                self._emit("renamed", name)
        elif kind == "left":
            self._remove_peer(str(body.get("id", "")), reason)
        elif kind in ("denied", "removed", "closed"):
            self._ended = True
            self._finish(reason or "The host ended the connection")
        elif kind == "refused":
            map_name = body.get("map")
            if map_name:
                state = self.maps.get(map_name)
                if state:
                    state._refuse(str(body.get("key", "")), reason)
            else:
                channel = self.channels.get(body.get("channel"))
                if channel:
                    channel._refused(str(body.get("type", "")), reason)
        elif kind == "key":
            epoch = int(body.get("epoch", 0))
            self.room_keys[epoch] = crypto.unb64(body.get("key"))
            # The one before stays for what was sealed before the change; older ones go.
            self.room_keys = {e: k for e, k in self.room_keys.items() if e + 1 >= epoch}
        elif kind == "state":
            name = body.get("map")
            if isinstance(name, str) and _valid(name):
                items = body.get("items")
                self.state(name)._replace(items if isinstance(items, dict) else {})
        elif kind == "changed":
            name = body.get("map")
            if isinstance(name, str) and _valid(name):
                by_id = str(body.get("by", ""))
                by = self.peer(by_id) or Peer(id=by_id)
                value = body["value"] if "value" in body else MISSING
                self.state(name)._apply(str(body.get("key", "")), value, by, by_id == self.me.id)
        elif kind == "peers.state":
            name = body.get("map")
            if isinstance(name, str) and _valid(name):
                state = self.peer_state(name)
                values = body.get("values") if isinstance(body.get("values"), dict) else {}
                for peer_id in list(state.values):
                    if peer_id not in values and peer_id != self.me.id:
                        state._apply(self.peer(peer_id) or Peer(id=peer_id), MISSING)
                for peer_id, value in values.items():
                    if peer_id != self.me.id:
                        state._apply(self.peer(peer_id) or Peer(id=peer_id), value)
        elif kind == "peer.changed":
            name = body.get("map")
            if isinstance(name, str) and _valid(name):
                peer_id = str(body.get("peer", ""))
                self.peer_state(name)._apply(self.peer(peer_id) or Peer(id=peer_id), body.get("value", MISSING))

    def _on_welcome(self, body: dict) -> None:
        room_type = body.get("type")
        if isinstance(room_type, str) and room_type != self.type:
            self._ended = True
            self._finish(f"That is a {room_type} room, not a {self.type} room")
            return
        self.name = str(body.get("name", ""))
        my_id = str(body.get("id", ""))
        host_id = str(body.get("host", ""))
        key = body.get("roomKey")
        if isinstance(key, dict):
            self.room_keys[int(key.get("epoch", 0))] = crypto.unb64(key.get("key"))
        caught_up = bool(body.get("caughtUp"))
        peers = [Peer.from_json(each) for each in body.get("peers", []) if isinstance(each, dict)]
        for peer in peers:
            if peer.id == my_id:
                self.me = peer
            if peer.id == host_id:
                self.host = peer
        before = self.joined_peers
        self.joined_peers = {peer.id: peer for peer in peers}
        if not caught_up:
            self.last_seq = int(body.get("seq", 0))
            for state in self.maps.values():
                state._replace({})
        was_open = self._ever_open
        self._ever_open = True
        self._outbox.set_open(True)
        self._set_state("open")
        if not was_open:
            if self._opened and not self._opened.done():
                self._opened.set_result(True)
            return
        for peer in before.values():
            if peer.id not in self.joined_peers:
                self._emit("peer_left", peer, "Left while this member was away")
        for peer in peers:
            if peer.id not in before:
                self._emit("peer_joined", peer)
        self._emit("resumed", caught_up)

    def _deliver(self, channel, message: Message) -> None:
        if message.type != SEALED_TYPE or message.to != self.me.id:
            channel._received(message)
            return
        sender = self.peer(message.sender)
        plain = crypto.open_from_peer(self.identity, sender.public_key if sender else b"",
                                      crypto.unb64(message.body.get("box")))
        if plain is None:
            log.warning("A private message on %s from %s didn't open; dropped", channel.name, message.sender)
            return
        try:
            inner = json.loads(plain)
        except ValueError:
            return
        opened = str(inner.get("id", ""))
        if inner.get("channel") != channel.name or inner.get("to") != self.me.id or not opened \
                or opened in self._opened_ids:
            log.warning("A private message on %s was for elsewhere or came twice; dropped", channel.name)
            return
        self._opened_ids[opened] = None
        while len(self._opened_ids) > OPENED_IDS_KEPT:
            self._opened_ids.pop(next(iter(self._opened_ids)))
        message.type = str(inner.get("type", ""))
        message.body = inner.get("body") if isinstance(inner.get("body"), dict) else {}
        message.sealed = True
        channel._received(message)

    def _remove_peer(self, peer_id: str, reason: str) -> None:
        peer = self.joined_peers.pop(peer_id, None)
        if peer is not None:
            for state in self.peer_states.values():
                state._apply(peer, MISSING)
            self._emit("peer_left", peer, reason)

    # --- Ending ------------------------------------------------------------------------

    def _set_state(self, state: str) -> None:
        self.status = state

    def _fail(self, reason: str) -> None:
        self._ended = True
        if self._opened and not self._opened.done():
            self._opened.set_exception(JoinError(reason))
            self._opened.exception()  # Retrieved here, so asyncio doesn't warn when nobody waits
        self._finish(reason)

    def _finish(self, reason: str) -> None:
        if self.closed:
            return
        self.closed = True
        self.close_reason = reason
        self.status = "closed"
        self._outbox.close()
        if self._opened and not self._opened.done():
            self._opened.set_exception(JoinError(reason))
            self._opened.exception()
        if self._websocket is not None and self._task is not asyncio.current_task():
            asyncio.ensure_future(self._websocket.close(1000, reason[:120]))
        self._closed_event.set()
        self._emit("closed", reason)


def _valid(name: str) -> bool:
    from .frame import is_valid_channel_name

    return is_valid_channel_name(name)


def _carry(old: Outbox, sealer: crypto.Sealer) -> Outbox:
    """A new connection's outbox, with what waited in the old one (sealed afresh)."""
    new = Outbox(sealer.seal)
    new.set_open(False)
    for data, key, seal in old._entries:
        new.push(data, key, seal)
    return new


def _close_on_failure(task: asyncio.Task, websocket) -> None:
    if not task.cancelled() and task.exception() is not None:
        asyncio.ensure_future(websocket.close(1011, "Couldn't send"))


def _url(link: JoinLink | None, where: str) -> str:
    if link:
        if link.relay:
            return link.relay
        return f"ws://{link.address}/ws"
    where = where.strip()
    if where.startswith(("ws://", "wss://")):
        return where
    return f"ws://{where}/ws"


def default_key_file(name: str = "usdcc_peers") -> str:
    """Where a program's key lives when it names none: ~/.config/<name>/identity.key."""
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, name, "identity.key")
