# Peers protocol

The wire protocol of [PEERS.md](PEERS.md), version **1**, as `usdcc_peers` (`src/cpp/peers/`)
speaks it. Another implementation (the Python package of PEERS.md §9) has to match this page.
Words as in PEERS.md: a *host* orders a *room*'s messages, *members* join it, a *peer* has a key
pair, a *guest* is a web page.

## 1. Transport

One port per room, HTTP and WebSocket together:

| Path | What |
|---|---|
| `GET /` | The guests' page, if the room takes guests; 404 otherwise |
| `GET /<file>` | A file beside the page (a plain name, no folders) |
| `GET /ws` with `Upgrade: websocket` | A member's connection |

An upgrade that carries `Origin` is a page. It is refused (403) unless the room takes guests and
the origin is the room's own (`http://` or `https://` + the request's `Host`) or one the host
allowed. A page can only ever be a guest.

## 2. Frames

A WebSocket binary message is one of three, told apart by its first byte:

| First byte | What | Layout |
|---|---|---|
| `0` | A plain frame | `u32` big-endian header length *n*, *n* bytes of JSON header, the raw bytes |
| `1` | Sealed for the connection | `1`, then XChaCha20-Poly1305 of a plain frame (tag included) |
| `2` | Sealed for the room | `2`, `u32` big-endian epoch, 24-byte nonce, 64-byte signature, ciphertext |

The header is a JSON object: `channel` (required), `type`, `from`, `to`, `seq`, `body` (an
object), and `latest: true` for a message its sender wants delivered latest-only on a channel the
host hasn't opened. Each is left out when empty or zero. A plain frame's first byte is 0 because
its header is under 16 MB.

A WebSocket text message is a header with no raw bytes. Only a page on a plain link sends text;
the host always sends binary.

**Sealed for the connection.** Each direction has its own key from the handshake (§3). The
nonce is a counter, 0 for the first message, as 8 bytes little-endian followed by 16 zero bytes,
counted separately each way. A message that doesn't open, or opens out of turn, ends the
connection.

**Sealed for the room.** Only the host sends these, for a message to everyone. Ciphertext is
XChaCha20-Poly1305 of a plain frame under the room key of that epoch, with the random nonce. The
signature is the host's Ed25519 signature of epoch ‖ nonce ‖ ciphertext. A member checks the
signature with the host's key from the handshake before opening it. One of an epoch the member
doesn't have yet is skipped: a latest-only message may overtake the key it was sealed under.

## 3. Joining

All on the channel `room`. Peers first:

1. **Member → host, plain:** `hello` `{protocol: 1, eph}`. `eph` is a fresh X25519 public key,
   base64 (libsodium's `crypto_kx`).
2. **Host → member, plain:** `challenge` `{protocol, eph, key, sig}`. `eph` is the host's X25519
   key for this connection, `key` its Ed25519 identity key, and `sig` signs
   `"usdcc-peers-1 host"` ‖ host eph ‖ member eph. The member checks the signature, and that
   the key's fingerprint is the one it expected (the link's `k`, or the announcement's `fp`), and
   on a reconnect that it is the same key as before. Otherwise it closes: *wrong key*.
3. Both derive the two keys: the member with `crypto_kx_client_session_keys`, the host with
   `crypto_kx_server_session_keys`. From here on, all a member sends is sealed (`1`).
4. **Member → host:** `auth` `{key, sig, instance, name, color, app, secret?, password?, resume?}`.
   `key` is the member's Ed25519 key, and `sig` signs `"usdcc-peers-1 peer"` ‖ member eph ‖ host
   eph. `resume` is `{id, seq}` on a reconnect (§6).
5. The host lets it in, asks its user, or refuses (§4).
6. **Host → member:** `welcome` `{protocol, id, name, host, seq, peers, channels, admission,
   roomKey?, resumed?, caughtUp?}`. `id` is the member's own, `host` the host's, `seq` the
   number of the last ordered message, `peers` everyone as objects `{id, name, kind, fingerprint,
   instance, color, app}`, and `channels` `[{name, delivery}]` the member reaches. `roomKey` is
   `{epoch, key}`, for peers only. Then, unless the member caught up (§6), the snapshots: `state`
   for each map, `peers.state` for each per-peer map, and a `snapshot` on each channel that has
   one.

A peer's id is `<fingerprint>-<instance>`. The fingerprint is 32 lower-case base32 characters of
a 20-byte BLAKE2b hash of its Ed25519 public key.

**Guests on a plain link.** `hello` `{protocol, secret, name}` with no `eph`, sent plain or as
text. With the guests' secret it gets a plain `welcome` and plain frames from then on; anything
else is refused. A guest's id is `guest-<random>`.

**Refused:** `denied` `{reason, protocol}`, then the connection closes (1008). Also before the
challenge, for a wrong protocol version: the reason names both versions.

## 4. Who gets in

| The member has | The room's admission | Result |
|---|---|---|
| The peers' secret | Any | In. The host is asked first if it set "ask before a join by link", unless it trusts the key |
| The guests' secret | Any, guests on | In, as a guest |
| Another secret | Any | Refused: wrong secret |
| No secret, a page | Any | Refused: invited only |
| No secret | `invited` | Refused: invited only |
| No secret | `ask` or `open` with a password | Refused unless `password` is right |
| No secret | `open` | In |
| No secret | `ask` | In if the host trusts its key or let it in before; otherwise the host is asked, and an unanswered question is a no after 120 s |

The room's member limit and guest limit apply in every case.

## 5. Messages

**Member → host:** frames on any channel but `room`. The host sets `from`, checks who may send
(PEERS.md §6), and routes:

- **With `to`**, or on the host's direct channel: to that member alone. No `seq`, and not back to
  the sender. To nobody known: `refused`.
- **Latest only** (the host's channel says so, or the header's `latest`): to everyone else. A
  newer one replaces one still waiting for a member with the same channel, type, sender and
  recipient.
- **Ordered** (the rest): numbered `seq` and sent to everyone, the sender included.

Guests get only channels the host opened to guests. A message the host turns down comes back as
`refused` `{channel, type, reason}` on `room`.

**On `room`, member → host:** `leave` `{reason}`, `set` `{map, key, value}`, `remove` `{map,
key}`, `peer.set` `{map, value?}`.

**On `room`, host → members:**

| Type | Body | Order |
|---|---|---|
| `joined` | A peer object | Ordered |
| `left` | `{id, reason}` | Ordered |
| `changed` | `{map, key, value?, by}` (no value: removed) | Ordered, peers only |
| `peer.changed` | `{map, peer, value?}` | Latest only |
| `state` | `{map, items}` | To one member, after `welcome` |
| `peers.state` | `{map, values}` | To one member, after `welcome` |
| `key` | `{epoch, key}` | To each peer, sealed for its connection |
| `refused` | `{channel, type, reason, map?, key?}` | To the sender |
| `removed`, `closed` | `{reason}` | To one member, then the connection closes |

**The room key** changes (`key`, epoch + 1) whenever a peer's connection ends, before anything
else is sealed under the new one. A member keeps the newest two.

**Shared state.** A `set` or `remove` is checked (who may write, a value up to 256 KB, a map up
to 16 MB), applied by the host, and sent as `changed` in the host's order. For one key the last
`changed` wins. A member that wrote a value shows it at once, and gives way when the host's
order says otherwise.

## 6. Reconnecting

The host keeps the last ordered messages (2,000 or 8 MB). When a peer's connection breaks, the
host keeps it in the room for 30 s, without telling anyone, and changes the room key. The member
retries with backoff (0.25 s, doubling, at most 8 s) for 60 s, doing the whole handshake again
with `resume` `{id, seq}` in `auth`. If its `seq` is still in the history, `welcome` says
`resumed` and `caughtUp`, the missed messages follow sealed for the connection, then the
per-peer maps. Otherwise it gets the snapshots as a new member does. A member ignores any ordered
message whose `seq` it already has.

A member that was dropped (too slow, or gone too long) joins again the same way and gets
everything afresh. One that was told `denied`, `removed` or `closed` doesn't try again.

## 7. Limits and liveness

| | Default |
|---|---|
| Members, guests | 50, 8 |
| One message from a member | 4 MB |
| Messages from one member | 200 a second, bursts of 400 |
| Waiting for one member | 16 MB, then it is dropped as too slow |
| Finishing the handshake | 10 s |
| Ping, silence | 10 s, 30 s |
| Per-peer state passed on | Rooms of up to 50 members |

## 8. Finding rooms

**This machine** (PEERS.md §5.1): the host writes `<AppLocalData>/peers/rooms/<host id>.json`,
`{protocol, fingerprint, instance, name, host, port, admission, password}`, and removes it when
the room closes. A file whose port refuses a connection is stale and is deleted.

**This network** (PEERS.md §5.2): DNS-SD service `_usdcc._tcp.local.`; the instance is
`<host id>._usdcc._tcp.local.`, and its host `usdcc-<instance>.local.`. The TXT record has `v`
(protocol), `fp`, `inst`, `name`, `host` (the host's name), `join` (`ask` or `open`), and `pw` (`1` or
`0`). A host answers on port 5353, one room per packet, and announces twice on start and once
with TTL 0 on close. A member asks from a port of its own every 3 s while it looks, for answers
sent straight back, and forgets a room that hasn't answered for 10 s. It joins at the address the
answer came from. A room that needs an invitation isn't announced.

**Links:** `usdcc://join/<address>:<port>#k=<fingerprint>&s=<secret>`. The part after `#` is
URL-encoded and never leaves the two programs.
