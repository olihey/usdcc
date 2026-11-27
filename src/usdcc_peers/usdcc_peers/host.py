"""A host: a room other programs and usdcc instances join (docs/PEERS_PROTOCOL.md).

    room = Host(name="Studio", identity=Identity.load_or_create("hub.key"))
    await room.start("0.0.0.0", 47800)
    print(room.link("hub.studio.example"))

It speaks the host's side of the protocol as the app's `Room::host()` does: the
handshake, who gets in (§4), ordering and routing (§5), the room key and its changes,
the history a peer that reconnects catches up from (§6), and the limits (§7). Guests
(web pages) aren't taken: a connection that carries an Origin is refused.
"""

from __future__ import annotations

import asyncio
import json
import time
from collections import deque
from dataclasses import dataclass

import websockets
from websockets.asyncio.server import serve

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
    dumps,
    encode,
    is_valid_channel_name,
    make_link,
)
from .member import HOST_PROOF, PEER_PROOF, SEALED_TYPE
from .room import MAX_MAP_BYTES, MAX_VALUE_BYTES, MISSING, RoomBase, log

HANDSHAKE_FRAME_BYTES = 64 * 1024


@dataclass
class Limits:
    """What a host lets each member do (PEERS.md §7.4), as the app's defaults."""

    max_members: int = 50
    max_frame_bytes: int = 4 * 1024 * 1024
    messages_per_second: float = 200
    message_burst: int = 400
    max_queued_bytes: int = 16 * 1024 * 1024
    hello_timeout: float = 10.0
    ping_interval: float = 10.0
    silence_timeout: float = 30.0
    reconnect_grace: float = 30.0
    history_messages: int = 2000
    history_bytes: int = 8 * 1024 * 1024
    max_peer_state_members: int = 50
    join_request_timeout: float = 120.0


class JoinRequest:
    """Someone without the secret asks to join (admission "ask"). Unanswered, it's a no."""

    def __init__(self, room: Host, member: _Member):
        self._room = room
        self._member = member
        self.peer = member.peer
        self.settled = False

    def allow(self) -> None:
        if not self.settled:
            self.settled = True
            self._room._settle(self._member, True, "")

    def deny(self, reason: str = "") -> None:
        if not self.settled:
            self.settled = True
            self._room._settle(self._member, False, reason)


class _Member:
    def __init__(self, websocket, limits: Limits):
        self.websocket = websocket
        self.outbox = Outbox()
        self.peer = Peer(id="")
        self.phase = "hello"  # hello, auth, asking, welcomed
        self.encrypted = False
        self.tx = crypto.Sealer()
        self.rx = crypto.Sealer()
        self.member_eph = b""
        self.host_eph = b""
        self.room_key = False
        self.away = False
        self.away_since = 0.0
        self.gone = False
        self.leave_reason = ""
        self.tokens = float(limits.message_burst)
        self.refilled = time.monotonic()
        self.started = time.monotonic()
        self.request: JoinRequest | None = None
        self.done = asyncio.Event()  # The connection is to end
        self.close_code = 1000
        self.close_reason = ""


class Host(RoomBase):
    is_host = True

    def __init__(self, *, name: str, identity: crypto.Identity | None = None, type: str = "chat",
                 admission: str = "invited", password: str = "", secret: str = "", ask_before_link_join: bool = False,
                 trusted: list[str] | None = None, limits: Limits | None = None, app: str = "usdcc_peers",
                 instance: str = "", color: str = ""):
        super().__init__()
        if admission not in ("invited", "ask", "open"):
            raise ValueError("admission is invited, ask or open")
        self.identity = identity or crypto.Identity.generate()
        self.name = clean_name(name, "Room")
        self.type = type
        self.admission = admission
        self.password = password
        self.secret = secret or crypto.random_secret()
        self.ask_before_link_join = ask_before_link_join
        self.admitted: set[str] = set(trusted or [])
        self.limits = limits or Limits()
        self.instance = (instance or crypto.random_id(6))[:32]
        fingerprint = self.identity.fingerprint
        self.me = Peer(id=f"{fingerprint}-{self.instance}", name=clean_name(name, "Host"),
                       kind="host", fingerprint=fingerprint, instance=self.instance, color=clean_color(color),
                       app=app, public_key=self.identity.public_key)
        self.host = self.me
        self.port = 0
        self.address = ""
        self._members: list[_Member] = []
        self._seq = 0
        self._history: deque[tuple[int, bytes]] = deque()
        self._history_bytes = 0
        self._epoch = 1
        self._room_key = crypto.os.urandom(32)
        self._map_bytes: dict[str, int] = {}
        self._server = None
        self._ticker: asyncio.Task | None = None
        self._closing = False
        self._closed_event = asyncio.Event()

    # --- Starting and stopping ---------------------------------------------------------

    async def start(self, address: str = "127.0.0.1", port: int = 0) -> None:
        """Listens; OSError when the port is taken."""
        self._server = await serve(self._connection, address, port, process_request=self._check_request,
                                   max_size=max(16 * 1024 * 1024, 2 * self.limits.max_frame_bytes),
                                   ping_interval=self.limits.ping_interval, ping_timeout=self.limits.silence_timeout,
                                   compression=None, server_header=None, backlog=1024)
        sockets = list(self._server.sockets)
        self.port = sockets[0].getsockname()[1]
        self.address = address
        self._ticker = asyncio.create_task(self._tick())
        log.info("Hosting %s room %s on %s port %d, admission %s", self.type, self.name, address, self.port,
                 self.admission)

    def link(self, address: str = "") -> str:
        """usdcc://join/<address>:<port>#k=…&s=…, which invites a peer. Never logged."""
        host = address or ("127.0.0.1" if self.address in ("", "0.0.0.0", "::", "127.0.0.1") else self.address)
        return make_link(JoinLink(address=f"{host}:{self.port}", fingerprint=self.me.fingerprint, secret=self.secret))

    async def close(self, reason: str = "") -> None:
        """Closes the room for everyone."""
        if self.closed:
            return
        reason = reason or "The host closed the room"
        log.info("Closing room %s - %s", self.name, reason)
        self._closing = True
        for member in list(self._members):
            if member.phase == "welcomed" and not member.away:
                self._drop(member, reason, tell="closed")
            else:
                self._drop(member, reason)
        self.closed = True
        self.close_reason = reason
        if self._ticker:
            self._ticker.cancel()
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        self._closed_event.set()
        self._emit("closed", reason)

    async def wait_closed(self) -> str:
        await self._closed_event.wait()
        return self.close_reason

    # --- What a program calls ----------------------------------------------------------

    def peers(self) -> list[Peer]:
        return [self.me] + [m.peer for m in self._welcomed(with_away=True)]

    def rename(self, name: str) -> bool:
        cleaned = clean_name(name, "")
        if not cleaned or self.closed:
            return False
        if cleaned != self.name:
            self.name = cleaned
            self._broadcast(control("renamed", {"name": cleaned}), ordered=True)
            self._emit("renamed", cleaned)
        return True

    def remove(self, peer_id: str, reason: str = "") -> None:
        member = self._welcomed_member(peer_id)
        if member:
            why = reason or "Removed by the host"
            if member.away:
                self._leave(member, why)
            else:
                self._drop(member, why, tell="removed")

    def trust(self, fingerprint: str) -> None:
        self.admitted.add(fingerprint)

    def queued_bytes(self, peer_id: str) -> int:
        member = self._welcomed_member(peer_id)
        return member.outbox.queued_bytes if member else 0

    # --- Connections -------------------------------------------------------------------

    def _check_request(self, connection, request):
        if request.path.split("?", 1)[0] != "/ws":
            return connection.respond(404, "Not found\n")
        if request.headers.get("Origin"):
            # A page can only ever be a guest, and this host takes none.
            return connection.respond(403, "No guests\n")
        return None

    async def _connection(self, websocket) -> None:
        if self.closed:
            return
        member = _Member(websocket, self.limits)
        member.outbox.sealer = member.tx.seal
        self._members.append(member)
        writer = asyncio.create_task(member.outbox.run(websocket))
        try:
            async for data in websocket:
                if member.gone:
                    break
                self._on_data(member, data)
                if member.done.is_set():
                    break
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            if not member.gone:
                self._on_disconnected(member)
            if member.done.is_set():
                # What was queued last (denied, removed, closed) goes out before it closes.
                await websocket.wait_closed()
            writer.cancel()

    def _on_data(self, member: _Member, data) -> None:
        limits = self.limits
        now = time.monotonic()
        member.tokens = min(limits.message_burst, member.tokens + (now - member.refilled) * limits.messages_per_second)
        member.refilled = now
        if member.tokens < 1:
            self._drop(member, "Sent too many messages", tell="removed")
            return
        member.tokens -= 1
        text = isinstance(data, str)
        raw = data.encode("utf-8") if text else data
        limit = limits.max_frame_bytes if member.phase == "welcomed" else HANDSHAKE_FRAME_BYTES
        if len(raw) > limit + 17:
            self._drop(member, "Sent a message over the size limit", tell="removed")
            return
        try:
            if member.encrypted:
                plain = None if text else member.rx.open(raw)
                if plain is None:
                    self._drop(member, f"Sent something that isn't sealed for this connection ({len(raw)} bytes)")
                    return
                message, latest = decode(plain)
            else:
                message, latest = decode_text(data) if text else decode(raw)
        except FrameError as error:
            self._drop(member, f"Sent something that isn't a frame: {error}", tell="removed")
            return
        is_control = message.channel == ROOM_CHANNEL
        if member.phase == "hello":
            if is_control and message.type == "hello":
                self._on_hello(member, message)
            else:
                self._deny(member, "The first message has to be hello")
            return
        if member.phase == "auth":
            if is_control and message.type == "auth":
                self._on_auth(member, message)
            else:
                self._deny(member, "Expected auth")
            return
        if member.phase == "asking":
            return
        if is_control:
            self._on_member_control(member, message)
        else:
            self._route(message, latest, member)

    def _on_disconnected(self, member: _Member) -> None:
        if member.phase != "welcomed":
            self._drop(member, "Went away while joining")
        elif member.leave_reason:
            self._drop(member, member.leave_reason)
        elif member.peer.kind == "peer" and self.limits.reconnect_grace > 0:
            self._go_away(member)
        else:
            self._drop(member, "The connection was lost")

    # --- The handshake -----------------------------------------------------------------

    def _on_hello(self, member: _Member, message: Message) -> None:
        protocol = message.body.get("protocol")
        if protocol != PROTOCOL:
            self._deny(member, f"This room speaks protocol {PROTOCOL}, and this member protocol {protocol}")
            return
        eph = crypto.unb64(message.body.get("eph"))
        if not eph:
            self._deny(member, "Wrong secret: the link isn't this room's")  # A guest; none taken here
            return
        public, secret = crypto.kx_key_pair()
        keys = crypto.kx_session_keys(True, public, secret, eph)
        if not keys:
            self._deny(member, "The handshake's key is no good")
            return
        member.member_eph = eph
        member.host_eph = public
        self._send_to(member, control("challenge", {
            "protocol": PROTOCOL, "eph": crypto.b64(public), "key": crypto.b64(self.identity.public_key),
            "sig": crypto.b64(self.identity.sign(HOST_PROOF + public + eph))}))
        receive, send = keys
        member.tx.key, member.rx.key = send, receive
        member.encrypted = True
        member.phase = "auth"

    def _on_auth(self, member: _Member, message: Message) -> None:
        body = message.body
        key = crypto.unb64(body.get("key"))
        if not crypto.verify(key, PEER_PROOF + member.member_eph + member.host_eph, crypto.unb64(body.get("sig"))):
            self._deny(member, "The member's key didn't sign the handshake")
            return
        fingerprint = crypto.fingerprint_of(key)
        instance = str(body.get("instance") or "")[:32] or crypto.random_id(6)
        member.peer = Peer(id=f"{fingerprint}-{instance}", name=clean_name(body.get("name"), "Peer"), kind="peer",
                           fingerprint=fingerprint, instance=instance, color=clean_color(body.get("color")),
                           app=str(body.get("app") or "")[:64], public_key=key)
        previous = self._welcomed_member(member.peer.id)
        if previous and previous is not member and previous.peer.kind == "peer":
            # The same peer back after a lost connection: its key proves it (§6).
            resume = body.get("resume") if isinstance(body.get("resume"), dict) else {}
            last_seq = resume.get("seq", -1)
            last_seq = int(last_seq) if isinstance(last_seq, (int, float)) else -1
            member.peer = previous.peer
            member.phase = "welcomed"
            member.room_key = True
            previous.gone = True
            had_socket = not previous.away
            self._members.remove(previous)
            if had_socket:
                previous.done.set()
                previous.outbox.close()
                asyncio.ensure_future(previous.websocket.close(1000, "Replaced"))
                self._rotate_room_key()
            log.info("%s %s is back in room %s", member.peer.name, member.peer.id, self.name)
            self._welcome(member, resumed=True, last_seq=last_seq)
            return
        given = body.get("secret") or ""
        if given:
            if crypto.same_secret(given, self.secret):
                if self.ask_before_link_join and fingerprint not in self.admitted:
                    self._ask(member)
                else:
                    self._admit(member)
            else:
                self._deny(member, "Wrong secret: the link isn't this room's")
            return
        if self.admission == "invited":
            self._deny(member, "This room takes only those invited with its link")
            return
        if self.password and not crypto.same_secret(body.get("password") or "", self.password):
            self._deny(member, "Wrong password")
            return
        if self.admission == "open" or fingerprint in self.admitted:
            self._admit(member)
            return
        self._ask(member)

    def _ask(self, member: _Member) -> None:
        member.phase = "asking"
        request = JoinRequest(self, member)
        member.request = request
        log.info("%s asks to join room %s", member.peer.name, self.name)

        async def time_out():
            await asyncio.sleep(self.limits.join_request_timeout)
            request.deny("The host didn't answer")

        asyncio.ensure_future(time_out())
        if not self._callbacks.get("join_requested"):
            request.deny("The host doesn't take questions")
            return
        self._emit("join_requested", request)

    def _settle(self, member: _Member, allow: bool, reason: str) -> None:
        if member.gone or member.phase != "asking":
            return
        if allow:
            self._admit(member)
        else:
            self._deny(member, reason or "The host said no")

    def _admit(self, member: _Member) -> None:
        if len(self._welcomed(with_away=True)) >= self.limits.max_members:
            self._deny(member, "The room is full")
            return
        self.admitted.add(member.peer.fingerprint)
        member.room_key = member.encrypted
        member.phase = "welcomed"
        log.info("Peer %s %s joined room %s", member.peer.name, member.peer.id, self.name)
        self._welcome(member, resumed=False, last_seq=-1)
        self._broadcast(control("joined", member.peer.to_json()), ordered=True, except_=member)
        self._emit("peer_joined", member.peer)

    def _welcome(self, member: _Member, resumed: bool, last_seq: int) -> None:
        caught_up = resumed and last_seq >= 0 and (
            last_seq == self._seq or (bool(self._history) and self._history[0][0] <= last_seq + 1))
        body = {"protocol": PROTOCOL, "id": member.peer.id, "name": self.name, "host": self.me.id, "seq": self._seq,
                "peers": [peer.to_json() for peer in self.peers()],
                "channels": [{"name": c.name, "delivery": c.delivery} for c in self.channels.values()],
                "admission": self.admission, "type": self.type}
        if resumed:
            body["resumed"] = True
            body["caughtUp"] = caught_up
        if member.room_key:
            body["roomKey"] = {"epoch": self._epoch, "key": crypto.b64(self._room_key)}
        self._send_to(member, control("welcome", body))
        if caught_up:
            for seq, frame in self._history:
                if seq > last_seq:
                    self._enqueue(member, frame, seal=member.encrypted)
            self._send_peer_states(member)
        else:
            for state in self.maps.values():
                self._send_to(member, control("state", {"map": state.name, "items": state.items()}))
            self._send_peer_states(member)
        self._drop_slow()

    def _send_peer_states(self, member: _Member) -> None:
        for state in self.peer_states.values():
            self._send_to(member, control("peers.state", {"map": state.name, "values": dict(state.values)}))

    def _deny(self, member: _Member, reason: str) -> None:
        log.info("Refused a member: %s", reason)
        self._drop(member, reason, tell="denied")

    # --- Messages ----------------------------------------------------------------------

    def _on_member_control(self, member: _Member, message: Message) -> None:
        body = message.body
        if message.type == "leave":
            reason = body.get("reason") if isinstance(body.get("reason"), str) else ""
            member.leave_reason = reason[:200] or "Left"
            self._drop(member, member.leave_reason, close_code=1000)
            return
        name = body.get("map")
        if message.type in ("set", "remove"):
            key = body.get("key") if isinstance(body.get("key"), str) else ""

            def refuse(reason: str) -> None:
                self._send_to(member, control("refused", {"channel": ROOM_CHANNEL, "type": message.type,
                                                          "map": name if isinstance(name, str) else "",
                                                          "key": key, "reason": reason}))

            if not is_valid_channel_name(name):
                refuse("Not a map's name")
                return
            state = self.state(name)
            if not state.may_send(member.peer):
                refuse("Not allowed to change this map")
                return
            value = MISSING
            if message.type == "set":
                if "value" not in body:
                    refuse("No value")
                    return
                value = body["value"]
                size = _json_size(value)
                before = _json_size(state.get(key)) + len(key) if key in state else 0
                if size > MAX_VALUE_BYTES:
                    refuse("The value is over 256 KB")
                    return
                if self._map_bytes.get(name, 0) - before + size + len(key) > MAX_MAP_BYTES:
                    refuse("The map would be over 16 MB")
                    return
            self._set_state(state, key, value, member.peer)
            return
        if message.type == "peer.set" and is_valid_channel_name(name):
            self._set_peer_state(self.peer_state(name), member.peer, body.get("value", MISSING), member)

    def _set_state(self, state, key: str, value, by: Peer) -> None:
        before = _json_size(state.get(key)) + len(key) if key in state else 0
        after = 0 if value is MISSING or value is None else _json_size(value) + len(key)
        self._map_bytes[state.name] = self._map_bytes.get(state.name, 0) + after - before
        body = {"map": state.name, "key": key, "by": by.id}
        if value is not MISSING:
            body["value"] = value
        message = control("changed", body)
        message.sender = self.me.id
        self._broadcast(message, ordered=True, reaches=lambda m: m.peer.kind != "guest")
        state._apply(key, value, by, False)

    def _send_peer_state(self, state, value) -> None:
        self._set_peer_state(state, self.me, value, None)

    def _set_peer_state(self, state, peer: Peer, value, sender) -> None:
        state._apply(peer, value)
        if len(self._welcomed()) > self.limits.max_peer_state_members:
            return
        body = {"map": state.name, "peer": peer.id}
        if value is not MISSING and value is not None:
            body["value"] = value
        message = control("peer.changed", body)
        message.sender = self.me.id
        self._broadcast(message, ordered=False, latest_key=f"peer.changed\n{state.name}\n{peer.id}", except_=sender)

    def _send(self, channel, type_: str, body: dict, data: bytes, to: str) -> None:
        if self.closed:
            return
        message = Message(channel=channel.name, type=type_, body=body, data=data, to=to)
        self._route(message, channel.delivery == "latest", None)

    def _send_private(self, channel, type_: str, body: dict, to: str) -> bool:
        target = self.peer(to)
        if not target or not target.public_key or target.id == self.me.id:
            return False
        inner = {"id": crypto.random_id(16), "channel": channel.name, "to": to, "type": type_, "body": body}
        box = crypto.seal_for_peer(self.identity, target.public_key, json.dumps(inner, separators=(",", ":")).encode())
        if not box:
            return False
        self._send(channel, SEALED_TYPE, {"box": crypto.b64(box)}, b"", to)
        return True

    def _route(self, message: Message, latest: bool, sender: _Member | None) -> None:
        sent_by = sender.peer if sender else self.me
        message.sender = sent_by.id
        message.seq = 0
        channel = self.channels.get(message.channel)

        def refuse(reason: str) -> None:
            if sender:
                self._send_to(sender, control("refused", {"channel": message.channel, "type": message.type,
                                                          "reason": reason}))
                self._drop_slow()
            elif channel:
                channel._refused(message.type, reason)

        if not is_valid_channel_name(message.channel):
            refuse("Not a channel name")
            return
        if channel and not channel.may_send(sent_by):
            refuse("Not allowed to send on this channel")
            return
        delivery = channel.delivery if channel else ("latest" if latest else "ordered")
        if not message.to and delivery == "direct":
            message.to = self.me.id
        if message.to:
            if message.to == self.me.id:
                if channel:
                    self._deliver(channel, message)
                return
            target = self._welcomed_member(message.to)
            if not target:
                refuse("Nobody in the room has this id")
                return
            key = _latest_key(message) if delivery == "latest" else ""
            self._enqueue(target, encode(message), key, seal=target.encrypted)
            self._drop_slow()
            return
        if delivery == "latest":
            self._broadcast(message, ordered=False, latest_key=_latest_key(message), except_=sender)
            if sender and channel:
                channel._received(message)
            return
        self._broadcast(message, ordered=True)
        if channel:
            channel._received(message)

    def _deliver(self, channel, message: Message) -> None:
        if message.type != SEALED_TYPE:
            channel._received(message)
            return
        sender = self.peer(message.sender)
        plain = crypto.open_from_peer(self.identity, sender.public_key if sender else b"",
                                      crypto.unb64(message.body.get("box")))
        if plain is None:
            return
        try:
            inner = json.loads(plain)
        except ValueError:
            return
        if inner.get("channel") != channel.name or inner.get("to") != self.me.id:
            return
        message.type = str(inner.get("type", ""))
        message.body = inner.get("body") if isinstance(inner.get("body"), dict) else {}
        message.sealed = True
        channel._received(message)

    def _broadcast(self, message: Message, *, ordered: bool, latest_key: str = "", except_: _Member | None = None,
                   reaches=None) -> None:
        if ordered:
            self._seq += 1
            message.seq = self._seq
        plain = encode(message)
        if ordered:
            self._history.append((message.seq, plain))
            self._history_bytes += len(plain)
            while self._history and (len(self._history) > self.limits.history_messages
                                     or self._history_bytes > self.limits.history_bytes):
                self._history_bytes -= len(self._history.popleft()[1])
        envelope = b""
        for member in self._welcomed():
            if member is except_ or (reaches and not reaches(member)):
                continue
            if member.room_key:
                if not envelope:
                    envelope = crypto.seal_for_room(self._room_key, self._epoch, plain, self.identity)
                self._enqueue(member, envelope, latest_key, seal=False)
            else:
                self._enqueue(member, plain, latest_key, seal=member.encrypted)
        self._drop_slow()

    def _send_to(self, member: _Member, message: Message) -> None:
        self._enqueue(member, encode(message), seal=member.encrypted)

    def _enqueue(self, member: _Member, data: bytes, key: str = "", seal: bool = False) -> None:
        if member.gone or member.away:
            return
        member.outbox.push(data, key, seal)

    def _drop_slow(self) -> None:
        for member in self._welcomed():
            if member.outbox.queued_bytes > self.limits.max_queued_bytes:
                log.info("%s %s fell behind by %d bytes", member.peer.name, member.peer.id,
                         member.outbox.queued_bytes)
                self._drop(member, "Too slow: fell too far behind")

    # --- Members leaving ---------------------------------------------------------------

    def _drop(self, member: _Member, reason: str, tell: str = "", close_code: int | None = None) -> None:
        if member.gone:
            return
        member.outbox.clear()
        if tell:
            plain = encode(control(tell, {"reason": reason, "protocol": PROTOCOL}))
            data = member.tx.seal(plain) if member.encrypted else plain
            member.outbox.push(data)  # Already sealed: goes out as it is
        member.close_code = close_code or (1008 if tell in ("denied", "removed") else 1000)
        member.close_reason = reason[:120]
        member.done.set()
        if not tell:
            member.outbox.close()
            asyncio.ensure_future(member.websocket.close(member.close_code, member.close_reason))
        else:
            asyncio.ensure_future(_close_after_flush(member))
        if member.request and not member.request.settled:
            member.request.settled = True
            self._emit("join_cancelled", member.request)
        if member.phase == "welcomed":
            self._leave(member, reason)
        else:
            member.gone = True
            if member in self._members:
                self._members.remove(member)

    def _go_away(self, member: _Member) -> None:
        log.info("%s %s lost its connection; waiting for it", member.peer.name, member.peer.id)
        member.outbox.close()
        member.away = True
        member.away_since = time.monotonic()
        if member.room_key:
            self._rotate_room_key()

    def _leave(self, member: _Member, reason: str) -> None:
        if member.gone:
            return
        member.gone = True
        had_key = member.room_key and not member.away
        if member in self._members:
            self._members.remove(member)
        if self._closing:
            return
        if had_key:
            self._rotate_room_key()
        self._broadcast(control("left", {"id": member.peer.id, "reason": reason}), ordered=True)
        for state in self.peer_states.values():
            if member.peer.id in state.values:
                state._apply(member.peer, MISSING)
        self._emit("peer_left", member.peer, reason)

    def _rotate_room_key(self) -> None:
        if self._closing:
            return
        self._epoch += 1
        self._room_key = crypto.os.urandom(32)
        message = control("key", {"epoch": self._epoch, "key": crypto.b64(self._room_key)})
        for member in self._welcomed():
            if member.room_key:
                self._send_to(member, message)

    def _welcomed_member(self, peer_id: str):
        for member in self._members:
            if member.phase == "welcomed" and not member.gone and member.peer.id == peer_id:
                return member
        return None

    def _welcomed(self, with_away: bool = False) -> list[_Member]:
        return [m for m in self._members if m.phase == "welcomed" and not m.gone and (with_away or not m.away)]

    async def _tick(self) -> None:
        interval = max(0.1, min(self.limits.ping_interval, self.limits.hello_timeout, self.limits.reconnect_grace))
        while True:
            await asyncio.sleep(interval)
            now = time.monotonic()
            for member in list(self._members):
                if member.gone:
                    continue
                if member.away:
                    if now - member.away_since > self.limits.reconnect_grace:
                        self._leave(member, "The connection was lost")
                elif member.phase in ("hello", "auth") and now - member.started > self.limits.hello_timeout:
                    self._deny(member, "Didn't finish the handshake")


async def _close_after_flush(member: _Member) -> None:
    # What _drop queued (denied, removed, closed) goes out, then the connection closes.
    for _ in range(40):
        if member.outbox.queued_bytes == 0:
            break
        await asyncio.sleep(0.025)
    await asyncio.sleep(0.05)
    member.outbox.close()
    try:
        await member.websocket.close(member.close_code, member.close_reason)
    except Exception:  # noqa: BLE001 - already gone
        pass


def _latest_key(message: Message) -> str:
    return "\n".join((message.channel, message.type, message.sender, message.to))


def _json_size(value) -> int:
    return len(dumps([value]))
