"""usdcc_peers: the Peers protocol for programs that aren't usdcc (docs/PEERS.md §9).

A render farm, an asset tracker, a CI job or a studio's own tool joins a room the way a
usdcc instance does, and sends it notifications, messages and shared state:

    import usdcc_peers
    room = usdcc_peers.join(link, name="Render Farm", key_file="farm.key")
    room.notify("Render finished", "shot_012 · 240 frames", kind="success", to="anna")

From asyncio code, use `Member.join()` and `Host` directly. The hub
(`python -m usdcc_peers.hub`) hosts a studio's room that is always there.

Needs `websockets` and `PyNaCl`, and neither usdcc nor Qt.
"""

from .crypto import Identity, fingerprint_of, short_fingerprint
from .frame import PROTOCOL, JoinLink, Message, Peer, make_link, parse_link
from .host import Host, JoinRequest, Limits
from .member import JoinError, Member
from .notify import NotificationError, action, notification
from .room import MISSING, Channel, PeerState, Senders, StateMap
from .sync import Room, join

__version__ = "1.0.0"

__all__ = [
    "MISSING", "PROTOCOL", "Channel", "Host", "Identity", "JoinError", "JoinLink", "JoinRequest", "Limits", "Member",
    "Message", "NotificationError", "Peer", "PeerState", "Room", "Senders", "StateMap", "action", "fingerprint_of",
    "join", "make_link", "notification", "parse_link", "short_fingerprint",
]
