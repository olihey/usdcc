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
   instance, color, app, key?}`, and `channels` `[{name, delivery}]` the member reaches. `roomKey` is
   `{epoch, key}`, for peers only. Then, unless the member caught up (§6), the snapshots: `state`
   for each map, `peers.state` for each per-peer map, and a `snapshot` on each channel that has
   one.

A peer's id is `<fingerprint>-<instance>`. The fingerprint is 32 lower-case base32 characters of
a 20-byte BLAKE2b hash of its Ed25519 public key. `key` is that public key, base64, so members can
seal messages for each other (§5); a member keeps it only if the fingerprint is of it. A guest
has none, nor does a peer whose host is older.

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

**Sealed for one member** (`Channel::sendPrivate()`, PEERS.md §6.2): a direct message of type
`sealed`, body `{box}`. `box` is base64 of `[nonce 24][crypto_box_easy]` between the sender's and
the recipient's Ed25519 keys turned into X25519 ones. Inside is JSON `{id, channel, to, type,
body}`. The recipient opens it with the key of `from`, which the host sets, and drops it unless
`channel` and `to` are where it came and `id` is new to it. It then arrives as `type` with `body`.
The host passes it on like any direct message and can't read it.

Guests get only channels the host opened to guests. A message the host turns down comes back as
`refused` `{channel, type, reason}` on `room`.

**On `room`, member → host:** `leave` `{reason}`, `set` `{map, key, value}`, `remove` `{map,
key}`, `peer.set` `{map, value?}`.

**On `room`, host → members:**

| Type | Body | Order |
|---|---|---|
| `joined` | A peer object | Ordered |
| `left` | `{id, reason}` | Ordered |
| `renamed` | `{name}`: the room's new name (`Room::rename()`), trimmed, at most 64 characters | Ordered |
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

## 9. Transfers

A bundle of files sent to one member (PEERS.md §7.3), on a channel the sending feature picks
(`transfer` unless it says otherwise). Every message names one member in `to`, so the host passes
it to that member alone, unnumbered and outside the history (§5). Each body has `id`, the
transfer's id: 1 to 64 of `A-Z a-z 0-9 _ -`, chosen by the sender (a UUID).

**The manifest** is `[{path, size, sha256}]`, in order. `path` is relative, with `/`: no empty
part, `.` or `..`, no `\`, `:`, `<>"|?*` or control character, no part that ends in a dot or a
space or is a name Windows keeps (`con`, `prn`, `aux`, `nul`, `com0`–`com9`, `lpt0`–`lpt9`, with
any extension), at most 1024 characters. No two paths may differ only in case, and no file may
also be a folder of another. `sha256` is 64 lower-case hex characters. The bundle's **hash** is
the sha256, in lower-case hex, of the manifest written as compact JSON with each object's keys in
order (`path`, `sha256`, `size`), as Qt writes it.

**Pieces.** With a piece size *c* (1 KB to 1 MB), each file of size *s* is ⌈*s*/*c*⌉ pieces, of
*c* bytes but the last; an empty file has none. Pieces are numbered from 0 across the files in
order: piece *i* is in the file whose first piece is the last one at or before *i*.

| Type | Way | Body | Raw bytes |
|---|---|---|---|
| `offer` | Sender → receiver | `{id, files, chunk}`: the manifest, and *c* | |
| `accept` | Receiver → sender | `{id, next}`: send from piece `next` | |
| `have` | Receiver → sender | `{id}`: the bundle is here already; nothing to send | |
| `chunk` | Sender → receiver | `{id, i, f, o}`: piece `i`, in file `f` (its index) at offset `o` | The piece |
| `ack` | Receiver → sender | `{id, next}`: every piece before `next` arrived | |
| `done` | Receiver → sender | `{id}`: every file arrived and matched its sha256 | |
| `refuse` | Receiver → sender | `{id, reason}`: the offer is turned down, or the transfer can't go on | |
| `cancel` | Either way | `{id, reason}`: stopped | |

**The receiver**, offered a bundle:

1. Refuses it (`refuse`) if the sender is a guest, the manifest breaks a rule above, *c* is out of
   range, it is over the inbox's limits (64 GB and 100,000 files to start with) or its disk's free
   space, or the inbox's owner says no. A manifest it already has (`<inbox>/<hash>/`) gets `have`.
2. Otherwise writes into `<inbox>/<hash>.part/`, and answers `accept` with the first piece it
   doesn't have: 0, or further for a part from before (a broken transfer of the same bundle, from
   anyone). Files completed and checked are listed in `<inbox>/<hash>.verified`.
3. Takes pieces in order. One it has is ignored; one after a gap gets `accept` with the missing
   piece (once a second at most). A piece whose `f`, `o` or length don't fit, or a file that
   doesn't match its sha256 when its last piece arrives, ends it with `refuse`, and that file is
   deleted.
4. Acknowledges with `ack`, gathered over 20 ms. After the last piece, moves the part to
   `<inbox>/<hash>/` and sends `done`.

An `offer` again with the same `id` is answered with where it is: `accept` (not again within 2 s
of the last one for the same piece), `done`, `refuse` or `cancel`.

**The sender** starts at `next` whenever an `accept` comes, also going back. It sends while what
is sent and not acknowledged is under its window (4 MB), its own send queue under 2 MB, and its
room's pieces under 100 a second. `have` or `done` ends it.

**Breaks.** The side whose connection to the host was restored speaks first: the receiver with
`accept`, the sender with `offer`. A sender that hears nothing for 5 s sends `offer` again. Either
side gives up after 2 minutes of silence, or when the other leaves the room. A receiver keeps the
part unless it was cancelled.

**Cancelling** (`cancel`) ends it at both ends; the receiver deletes the part.
