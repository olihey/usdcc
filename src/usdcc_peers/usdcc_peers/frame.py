"""The words of the protocol and its frame (docs/PEERS_PROTOCOL.md §2, §8).

A message is a JSON header and, optionally, raw bytes. On the wire a plain frame is a
big-endian u32 header length, the header, then the bytes.
"""

from __future__ import annotations

import json
import re
import struct
from dataclasses import dataclass, field
from urllib.parse import parse_qs, quote

PROTOCOL = 1
ROOM_CHANNEL = "room"

_CHANNEL_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,127}$")
_COLOR = re.compile(r"^#[0-9a-fA-F]{6}$")


def is_valid_channel_name(name) -> bool:
    """Letters, digits and `.`, `_`, `-`, `/`, at most 128 characters, not `room`."""
    return isinstance(name, str) and name != ROOM_CHANNEL and bool(_CHANNEL_NAME.match(name))


def clean_name(name, fallback: str) -> str:
    text = " ".join(name.split())[:64] if isinstance(name, str) else ""
    return text or fallback


def clean_color(color) -> str:
    return color.lower() if isinstance(color, str) and _COLOR.match(color) else ""


@dataclass
class Peer:
    """Someone in a room: the host, a peer with a key, or a guest (PEERS.md §4, §6.1)."""

    id: str
    name: str = ""
    kind: str = "peer"  # "host", "peer" or "guest"
    fingerprint: str = ""
    instance: str = ""
    color: str = ""
    app: str = ""
    public_key: bytes = b""

    def to_json(self) -> dict:
        from .crypto import b64

        result = {"id": self.id, "name": self.name, "kind": self.kind}
        if self.fingerprint:
            result["fingerprint"] = self.fingerprint
            result["instance"] = self.instance
        if self.public_key:
            result["key"] = b64(self.public_key)
        if self.color:
            result["color"] = self.color
        if self.app:
            result["app"] = self.app
        return result

    @classmethod
    def from_json(cls, data) -> Peer:
        from .crypto import fingerprint_of, unb64

        data = data if isinstance(data, dict) else {}
        kind = data.get("kind")
        peer = cls(id=str(data.get("id", "")), name=str(data.get("name", "")),
                   kind=kind if kind in ("host", "guest") else "peer",
                   fingerprint=str(data.get("fingerprint", "")), instance=str(data.get("instance", "")),
                   color=str(data.get("color", "")), app=str(data.get("app", "")))
        # Only a key the fingerprint is of: the host can't hand out a key of its own
        # under someone's name.
        key = unb64(data.get("key"))
        if key and peer.fingerprint and fingerprint_of(key) == peer.fingerprint:
            peer.public_key = key
        return peer


@dataclass
class Message:
    channel: str
    type: str = ""
    body: dict = field(default_factory=dict)
    data: bytes = b""
    sender: str = ""  # `from` on the wire: set by the host, never taken from the sender
    to: str = ""
    seq: int = 0
    sealed: bool = False  # Opened here from a private message (PEERS.md §6.2)


def _header(message: Message, latest: bool) -> dict:
    header = {"channel": message.channel}
    if message.type:
        header["type"] = message.type
    if message.sender:
        header["from"] = message.sender
    if message.to:
        header["to"] = message.to
    if message.seq:
        header["seq"] = message.seq
    if message.body:
        header["body"] = message.body
    if latest:
        header["latest"] = True
    return header


def dumps(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def encode(message: Message, latest: bool = False) -> bytes:
    header = dumps(_header(message, latest))
    return struct.pack(">I", len(header)) + header + bytes(message.data)


class FrameError(ValueError):
    pass


def _from_header(text: bytes) -> tuple[Message, bool]:
    try:
        header = json.loads(text)
    except (ValueError, UnicodeDecodeError) as error:
        raise FrameError("The header isn't a JSON object") from error
    if not isinstance(header, dict):
        raise FrameError("The header isn't a JSON object")
    values = {}
    for key in ("channel", "type", "from", "to"):
        value = header.get(key, "")
        if not isinstance(value, str):
            raise FrameError(f"`{key}` isn't a string")
        values[key] = value
    if not values["channel"]:
        raise FrameError("No channel")
    seq = header.get("seq", 0)
    if isinstance(seq, bool) or not isinstance(seq, (int, float)):
        raise FrameError("`seq` isn't a number")
    body = header.get("body", {})
    if not isinstance(body, dict):
        raise FrameError("`body` isn't an object")
    message = Message(channel=values["channel"], type=values["type"], body=body, sender=values["from"],
                      to=values["to"], seq=int(seq))
    return message, header.get("latest") is True


def decode(binary: bytes) -> tuple[Message, bool]:
    """(message, latest) from a binary frame; FrameError for anything else."""
    if len(binary) < 4:
        raise FrameError("Shorter than a frame's length")
    length = struct.unpack(">I", binary[:4])[0]
    if length > len(binary) - 4:
        raise FrameError("The header is longer than the frame")
    message, latest = _from_header(binary[4:4 + length])
    message.data = bytes(binary[4 + length:])
    return message, latest


def decode_text(text: str) -> tuple[Message, bool]:
    return _from_header(text.encode("utf-8"))


def control(type_: str, body: dict) -> Message:
    return Message(channel=ROOM_CHANNEL, type=type_, body=body)


# --- Links (PEERS.md §5.3) -------------------------------------------------------------


@dataclass
class JoinLink:
    address: str = ""  # "host:port"; empty for a room on a relay
    fingerprint: str = ""
    secret: str = ""
    relay: str = ""  # wss://relay.example/r/<room>: a room reached through a relay (PEERS.md §5.4)


def make_link(link: JoinLink) -> str:
    fragment = "k=" + quote(link.fingerprint, safe="") + "&s=" + quote(link.secret, safe="")
    if link.relay:
        return "usdcc://join?relay=" + quote(link.relay, safe=":/") + "#" + fragment
    return "usdcc://join/" + link.address + "#" + fragment


def parse_link(text: str):
    """A JoinLink, or None for text that isn't a usdcc:// join link."""
    text = text.strip() if isinstance(text, str) else ""
    prefix = "usdcc://join"
    if not text.lower().startswith(prefix):
        return None
    rest = text[len(prefix):]
    fragment = ""
    if "#" in rest:
        rest, fragment = rest.split("#", 1)
    values = parse_qs(fragment, keep_blank_values=True)
    link = JoinLink(fingerprint=values.get("k", [""])[0], secret=values.get("s", [""])[0])
    if rest.startswith("?"):
        query = parse_qs(rest[1:], keep_blank_values=True)
        link.relay = query.get("relay", [""])[0]
        if not link.relay.lower().startswith(("ws://", "wss://")):
            return None
        return link
    if not rest.startswith("/"):
        return None
    address = rest[1:].rstrip("/")
    host, _, port = address.rpartition(":")
    if not host or not port.isdigit() or not 0 < int(port) < 65536:
        return None
    link.address = address
    return link
