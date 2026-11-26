# Peers

A general way for running usdcc instances, their plugins and other programs to find each other
and share data: messages, state that is kept equal everywhere, and files. Chat, notifications from
a studio's services, sending things to a colleague and editing one stage together
([SHARED_STAGE.md](SHARED_STAGE.md)) are all uses of it, and a plugin can add its own from Python.

**Status:** planned 2026-11-16, reworked 2026-11-17 as a framework first, with stage sharing moved
to its own page. "Checked" below means looked at in this repository or its pinned vcpkg; everything
else is an assumption until the spike (§14).
Also 2026-11-17: rooms and channels (phase 1a) are built first, for
[VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md), which also brought browser guests (§6.1).
**Built** (2026-11-17), all in C++: phase 1a (rooms and channels, §14.1), phase 1b without
the Python module (keys, encryption, reconnecting, shared state, several rooms, §14.2), and
phase 2 (rooms found on this machine and network, the Peers panel with chat, §14.3). Since built:
phase 5's transfers without UI or Python (§14.4), phase 6, the shared stage
([SHARED_STAGE.md](SHARED_STAGE.md)), and **phase 3** (2026-11-26, §14.5): the `usdcc_peers`
Python package, the hub, rooms joined at start, and notifications; and **phase 4** (§14.6): the
relay, rooms hosted through it, its links, and `wss://`. Not built: the rest of phase 5. The protocol is written down in [PEERS_PROTOCOL.md](PEERS_PROTOCOL.md).
`usdcc.peers` for Python, set aside on 2026-11-17, is built since (§8.2, 2026-11-26), and with it
1b's last criterion. Two machines haven't been tried; everything so far ran on one.

## 1. Can it work without a server?

Mostly. It depends on where the other side is:

| The other side is | Found by | Server |
|---|---|---|
| On this machine | A small file each hosting instance writes (§5.1) | None |
| On this network | mDNS, the way printers are found (§5.2) | None |
| On another network, with an address that can be reached (a VPN, Tailscale, a forwarded port) | An invite link holding the address (§5.3) | None |
| On another network, both behind routers | An invite link through a relay (§5.4) | A relay: one small program, run by the studio or the user, that only forwards encrypted bytes |

**The last row can't be avoided.** Two machines behind home or office routers can't open a
connection to each other. A third machine both can reach has to at least introduce them. Hole
punching (WebRTC) still needs that machine, and falls back to relaying on the routers it fails on,
so it removes no server (§17). The plan is one dumb relay, and no relay at all for the first three
rows.

## 2. Decisions

- **A framework, with the features on top.** Three building blocks (§7): *messages*, *shared
  state* and *transfers*. Everything in §10 is built from them with the same API a plugin gets.
  Nothing in `usdcc_peers` knows about chat, toasts or USD.
- **Words.** A *peer* is anything in a room: a usdcc instance, or another program. A *room* is a
  group of connected peers. Not "session": that is a saved working state
  ([SESSIONS.md](SESSIONS.md)).
- **One peer hosts a room, the others join.** A star, not a mesh. The host puts messages in one
  order, which shared state needs (§7.2), and only the host has to accept connections.
- **Several rooms at once.** A peer can be in a studio's room for notifications and in a
  colleague's room for a review, and host one while joined to others.
- **Python is a first-class user.** `usdcc.peers` gives a plugin everything C++ has (§8.2).
- **The protocol is open.** Other programs join or host with a small Python package that needs
  neither usdcc nor Qt (§9). This is how a render farm or an asset tracker reaches the app.
- **WebSocket everywhere.** A joiner connects to the host, or both connect to the relay. The frames
  are the same either way, and WebSocket passes proxies and firewalls that raw TCP doesn't.
- **Qt WebSockets in the app, not libcurl.** [REMOTE_ASSETS.md](REMOTE_ASSETS.md) §8 avoids Qt
  Network because resolvers run on any thread and in tools with no event loop. This is the opposite
  case: app code on the event loop, needing a server too, which libcurl doesn't have.
  [MCP.md](MCP.md) and [VSCODE.md](VSCODE.md) link Qt Network for their endpoints as well.
- **Off until asked.** usdcc opens no port and sends nothing to the network until the user hosts a
  room, looks for one, or lists one to join at start.
- **Always encrypted, end to end, between peers** (§6). The relay and the network see sizes and
  timing only.
- **A member can be a web page.** A *guest* watches and chats from a browser, with nothing
  installed. It is not a peer, and reaches only what a room opens to guests (§6.1).
- **Nothing received acts by itself.** No script runs, no stage opens and no file leaves the inbox
  without a click.

## 3. The parts

| Part | Where | Holds |
|---|---|---|
| `usdcc_peers` (DLL; Qt Core, Network, WebSockets, HttpServer; no USD, no widgets) | `src/cpp/peers/` | Identity, discovery, hosting and joining, the handshake, messages, shared state, transfers, the page for guests |
| `usdcc.peers` (pybind11, like `usdcc.usd`) | Inside the app | The same for scripts and plugins (§8.2) |
| `usdcc_peers` (pure Python: `websockets`, `PyNaCl`) | `src/python/usdcc_peers/`, installable with pip | The protocol for other programs, the hub (§9) and the relay (§5.4) |
| UI | `usdcc_ui` | The Peers side panel (rooms, people, chat, notifications), toasts, Settings > Peers |
| Stage sharing | `usdcc_usd_ui` | [SHARED_STAGE.md](SHARED_STAGE.md) |

## 4. Identity

Each profile has a display name (the OS user name to start with), a color, and a key pair made on
first use. The private key is kept in the OS credential store, as
[REMOTE_ASSETS.md](REMOTE_ASSETS.md) §8 does for passwords. A peer is known by the fingerprint of
its public key, plus an id per running instance, so two instances of one profile are two peers.
Another program has a key pair in a file it names.

A test profile (`USDCC_TEST_PROFILE`) has its own identity and its own peers folder, so tests never
see real rooms.

## 5. Finding a room

### 5.1 On this machine

A hosting instance writes `<AppLocalData>/peers/rooms/<instance id>.json` (room name, host name,
port) and removes it on exit. Other instances watch the folder. A file whose port doesn't answer is
stale and is removed. The host always listens on `127.0.0.1` as well, which never involves a
firewall.

### 5.2 On this network

mDNS/DNS-SD, service type `_usdcc._tcp`. The TXT record holds the room's name, the host's name and
key fingerprint, the protocol version and whether joining asks the host.

- **Only a host announces.** A peer that is looking only sends queries, and only while the Peers
  panel shows the room list.
- **Library:** `mdns`, one public-domain C header (*checked:* vcpkg port 1.4.3, Unlicense), the
  same code on every platform. The alternatives are the OS's own services (`DnsServiceBrowse` and
  `DnsServiceRegister` on Windows 10 and later, Avahi, Bonjour: three code paths, but they share
  port 5353 properly with the system) and a plain UDP broadcast of our own (the least code, but
  invisible to standard tools). The spike decides (§14).
- **Windows Firewall.** The first time usdcc listens on a network address, Windows asks whether to
  allow it, and allowing needs an administrator. Denied, nobody on the network can join; the Peers
  panel says so and offers the invite link and relay instead. Joining never asks. This is why only
  the host listens, and why hosting is something the user starts.
- **Where it doesn't reach:** across subnets, on guest Wi-Fi and many company networks that block
  multicast, and through most VPNs. Then the invite link is the way.

### 5.3 Anywhere: an invite link

```
usdcc://join/192.168.1.20:47800#k=<host key fingerprint>&s=<secret>
usdcc://join?relay=wss://relay.studio.example/r/8f3k2m#k=<…>&s=<…>
```

**Copy Invite Link** in the panel, **Join by Link…** on the other side. The part after `#` never
leaves the two apps: it isn't sent to the relay. `k` lets the joiner check that it reached the real
host; `s` proves to the host that the joiner was invited. Registering `usdcc://` with the OS, so a
click in a chat program opens usdcc, comes with the installer ([RELEASE.md](RELEASE.md)); until
then the link is pasted.

**Rooms joined at start.** Settings > Peers keeps a list of links that are joined whenever usdcc
runs, and joined again after a lost connection. This is how every artist is in the studio's room
(§9) without doing anything. *As built* (§14.5): `Peers/startRooms`, links separated by spaces,
and **Join When usdcc Starts** in a room's menu in the Peers panel for a room joined by a link. A
room on the list that is lost after the reconnect of §6 gave up is joined again after 5 s,
doubling to 5 minutes; one the user left isn't, until usdcc starts again.

### 5.4 The relay

A small program in the `usdcc_peers` Python package (§9), supported and tested like the rest, not
an example:

```
python -m usdcc_peers.relay --port 8443 --token-file relay.token
```

It listens on one port, behind any HTTPS reverse proxy for `wss://`. Installing the package also
gives it as the command `usdcc-relay`, and a Dockerfile comes with it.

- **Python, not C++.** A studio's server gets `pip install` or a container instead of Qt's
  libraries. The relay forwards bytes it can't read, so it shares no code with the app's protocol,
  and it needs only `websockets`, not `PyNaCl`.
- **It can be replaced.** Its side of the protocol is small and written down (§9). A studio that
  outgrows it (§7.5) can put another implementation in its place without touching usdcc.

- A room on it is a random id. It forwards each frame to the room's other members and keeps nothing.
- A frame for everyone is sent to it once and copied to each member (the room key of §6 makes the
  bytes the same for all), so a host on a home connection doesn't upload it once per member.
- It serves the guests' page (§6.1) for a room that takes guests.
- It can't read anything (§6). It logs connections and byte counts.
- Limits per room: members, frame size, bytes per second. An optional shared token keeps strangers
  off.
- No accounts and no database. A studio runs one on a machine everyone reaches; Settings > Peers
  holds its URL.

*As built* (§14.6): `usdcc_peers.relay` (`usdcc-relay`), with the Dockerfile beside the package.
Its side of the protocol is [PEERS_PROTOCOL.md](PEERS_PROTOCOL.md) §11. A host on it is a room in
usdcc whose port is on the loopback only, with a bridge (`relay_bridge.cpp`, and
`usdcc_peers.relay_bridge` for a Python host or the hub's `--relay`) that gives each member coming
through the relay a connection of its own to that port. The relay doesn't serve the guests' page:
a viewport broadcast still needs a reachable address. Settings > Peers holds the relay's URL and
its token, and Host Room then offers **Through the relay**.

### 5.5 Room types

A room says what it is for (`RoomType`, `HostOptions::type`), and each part of usdcc uses only
its own:

| Type | What it is | Where |
|---|---|---|
| `chat` | people talking | the Peers panel (§10.1), which lists only these |
| `broadcast` | a viewport shown to watchers | Viewport Broadcast |
| `render_queue` | instances rendering each other's jobs | Render > Swarm ([FINAL_RENDER.md](FINAL_RENDER.md) §7) |

- **Said by the host:** in its `welcome` (`type`), and in its announcement (the room file's
  `type`, the TXT record's `type`), so a room is known for what it is before joining it.
  `Peers::foundRooms(type)` and `Peers::rooms(type)` give one kind.
- **Expected by the joiner:** `JoinOptions::type`. A host that says it is another kind is left at
  once, saying so ("That is a chat room, not a render_queue room"). A room that says nothing (an
  older host) is taken to be what was expected; for an announcement, a chat room.
- **One instance, several rooms:** each is announced on its own (by its host's id and its port),
  so a chat room and a render queue hosted by the same instance are both found.
- **Debug > Peer Rooms:** every room, of every type, with its peers, channels (what arrived since
  the window opened), maps and per-peer states, as raw JSON.

## 6. Connecting and trust

- **Hello.** Both sides send the protocol version, peer id, name, color, and what they are (usdcc
  and its version, or a program's own name). A different protocol version is refused with a message
  naming both.
- **Encryption.** A key exchange between the joiner and the host (libsodium, *checked:* in vcpkg;
  `PyNaCl` is the same library for Python), then every frame is encrypted and authenticated. Over
  the relay it is the same exchange, so the relay only passes ciphertext. Plain `ws://` plus this,
  rather than TLS with self-signed certificates: it is the same on a LAN and through a relay, and
  nobody has to click through a certificate warning.
- **A room key for messages to everyone.** Encrypting a message once per member would cost a
  room of 2,000 as many encryptions for every notification. So the host makes a key for the room
  and gives it to each member over that member's own connection. A message to everyone is
  encrypted once under it and signed by the host, so a member who has the key still can't pose as
  the host. The key changes when a member leaves or is removed. A message to one peer uses that
  peer's own key.
- **Who may join.**
  - *By link:* the secret in it is the invitation. A setting makes the host ask anyway.
  - *By discovery:* the host gets a toast ([NOTIFICATIONS.md](NOTIFICATIONS.md)), "Anna wants to
    join Shot 12", with **Allow**, **Always allow Anna** and **Deny**. A room can also have a
    password.
  - *A public room:* anyone who finds it on this machine or network is in, without asking (a
    password still applies). It is how a team has one room for everyone before there is a hub
    (§9). Added 2026-11-17.
  - *Invited only:* not announced, and refuses anyone without the link.
- **Who may send.** The host says, per channel: everyone (the default), named peers, or only the
  host. It refuses the rest itself, so a member can't get around it. The host can also remove a
  member and close the room.
- **The room ends when its host leaves,** in the first version (§16).

### 6.1 Guests

A guest is a member that is a web page, not a usdcc instance. The first use is watching a
viewport ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md)).

- **Only if the room allows them.** A room takes guests when something in it asks for them, such
  as a broadcast. A room for a shared stage takes none unless the host says so.
- **The room serves the page** on its own port, beside the WebSocket, so a guest's link is a web
  link: `http://<address>:<port>/#s=<secret>`. The secret is the invitation, as in §5.3, and is
  given in the guest's first message, not in the request.
- **A name and nothing else.** A guest has no key pair, so it is known only while it is connected.
  The host gives it its id.
- **Only the channels opened to guests.** `chat`, `presence`, and what the feature that invited
  them adds (`view` and `nav` for a broadcast). Never `transfer` or `stage/<id>`: the host refuses
  those from a guest, and sends a guest nothing on them.
- **The same frames** (§7), so the host treats a guest as any member: ordered, latest only,
  direct, limits, removal.
- **Encryption.** Through a relay, the page does the key exchange of §6 itself (libsodium has a
  build for browsers), with the host's fingerprint from the link, so the relay still sees sizes
  only. That holds as long as the relay serves the real page, which the relay's operator controls.
  On a plain `http://` link on a LAN a guest is **not** encrypted in the first version: the page
  itself arrives unprotected there, so encrypting after it would only stop someone listening,
  not someone interfering (§16 question 11).

### 6.2 Private messages

A message for one member alone can be **sealed for its key** (`Channel::sendPrivate()`): the
host passes it on but can't read it, so private chats (§10.1) aren't readable by whoever hosts
the room, nor in its logs.

- **The keys come from the host,** in each peer object (`key`). A member keeps one only if the
  peer's fingerprint is of it.
- **A box between the two keys** (libsodium's `crypto_box`, with the Ed25519 identities turned
  into X25519 ones). It also proves which of the two sealed it. What is inside names the channel,
  the recipient and a fresh id, so the host can't hand it on elsewhere or twice.
- **The limit:** this protects against a host that reads along, not one that lies about who is
  who. A host could make up a peer with a key of its own under someone's name. The fingerprint in
  each person's tooltip is how two people check each other outside usdcc.
- **Guests and older peers** have no key, so they can't be messaged privately.

## 7. The building blocks

Everything is addressed by a **channel name**. Names without a dot are usdcc's own (`chat`,
`notify`, `presence`, `transfer`, `stage/<id>`). A plugin's start with its id (`com.example.notes.pins`), a program's with a
name of its own. A peer that doesn't know a channel ignores it.

### 7.1 Messages

For things that happen: a chat line, a notification, "the bake finished".

One WebSocket binary message is a JSON header followed by raw bytes for a body that isn't JSON.

| Field | Meaning |
|---|---|
| `channel` | The channel's name |
| `type` | The channel's own |
| `from` | The sender's peer id. Set by the host, never taken from the sender |
| `to` | Empty for everyone, or one peer id |
| `seq` | The number the host gives every ordered message |

Three kinds of delivery, chosen per channel:

- **Ordered:** everyone gets every message, in the host's order. The default.
- **Latest only:** a newer message replaces an older one still waiting, and may be dropped. For
  values that change many times a second.
- **Direct:** to one peer.

### 7.2 Shared state

For things that are: who is here, the notes on a shot, a review's list of approved prims. A plugin
that only sent messages would have to solve late joiners, lost connections and two people editing
at once by itself. Shared state solves them once.

**A map** is a named set of keys with JSON values that is equal on every peer.

- **Anyone sets or removes a key.** It applies locally at once and goes to the host, which orders
  it. For one key the last writer in the host's order wins; a peer whose own change lost is told
  through the same change callback.
- **A joiner gets the whole map,** then the changes. So does a peer that reconnects too late to
  catch up.
- **Limits:** a value up to 256 KB and a map up to 16 MB, to start with. Larger data is a transfer
  (§7.3) with its id in the map.
- **It lives as long as the room.** A plugin that wants it to last saves it itself (§16).

**Per-peer state** is a map with one value per peer, which only that peer sets and which goes
away when it leaves. Delivered latest-only. usdcc's own is `presence`: a name and color, and the
stage a peer has open. A feature adds its own, such as a selection. It is for small rooms only
(§7.5).

**When a map isn't enough,** a channel can supply its own snapshot: a callback on the host that
writes the current state for a joiner, and one on the joiner that reads it, with ordered messages
as the changes after it. The shared stage does this with its live layer
([SHARED_STAGE.md](SHARED_STAGE.md) §7).

### 7.3 Transfers

For bytes: a file, a packed stage, a screenshot.

- **Offered, then accepted.** The receiver sees the name, size and sender first. A plugin's own
  transfers can be accepted by the plugin, up to a size limit.
- **Sent in pieces,** so chat isn't stuck behind a file, with a hash checked at the end.
- **To one peer or to everyone.** Through the host; a relay carries it like any frame.
- **Files land in `<AppLocalData>/peers/inbox/<room>/`,** or in memory for a small one a plugin
  asked for.

**As built** (the generic part, §14.4): `Transfer` and `TransferInbox` (`transfer.h`), on any
channel a feature picks. A transfer is a *bundle*: a manifest of files (`{path, size, sha256}`)
offered to one member, then its pieces, sent to that member alone.

- **The inbox is addressed by content:** a bundle lands in `<AppLocalData>/peers/inbox/<hash>/`,
  where `<hash>` is the sha256 of its manifest. A bundle already there is acknowledged at once and
  nothing is sent; a render node that has a shot's textures gets only the frames' new layers.
- **Pieces of at most 1 MB,** direct (`to`), so they skip the room's history, and paced: at most
  4 MB sent and not yet acknowledged, nothing more while this side's send queue holds 2 MB, and
  100 pieces a second for all of a room's transfers together, half the room's rate limit.
- **Each file is checked** against its sha256 as its last piece arrives; one that doesn't match
  ends the transfer and is deleted.
- **Resuming:** what arrived stays in `<hash>.part/`. After a break, the receiver says from which
  piece to go on, and so does a later transfer of the same bundle, from anyone.
- **Either end cancels,** and the receiver then throws away what arrived.
- **Only safe paths:** relative, with `/`, no `..`, no drive letters, no names Windows keeps for
  itself, and never written through a link. A limit on a transfer's size (64 GB to start with)
  and its number of files.
- **Not built yet:** asking the user about an offer (the inbox takes a callback that decides),
  the inbox by room, transfers in memory, to everyone at once, and Python.

### 7.4 What holds for all three

- **A slow peer** has a send queue with a limit. One that falls too far behind is disconnected and
  rejoins, getting the state of §7.2 afresh.
- **Gone:** a ping every 10 s; 30 s of silence and the peer has left.
- **Reconnecting:** a joiner retries, then asks for what it missed by `seq`. The host keeps the
  recent ordered messages; older than that means fresh state.
- **Limits** on frame size and rate per peer, so one member can't flood a room.

### 7.5 Large rooms and many rooms

A studio's room can have hundreds or thousands of members, and a relay hundreds of rooms. What
each thing costs the host:

| Thing | Cost | In a room of 2,000 |
|---|---|---|
| An idle connection | A ping every 10 s | Nothing to speak of |
| A message to everyone, or a map change | One encryption and signature (§6), then one send per member | Fine for notifications, and for state that few write |
| A message to one peer | One send | Fine |
| Per-peer state | One send per member, for every update of every member | Members times members: far too much |

- **Per-peer state is for small rooms.** The host turns it off above a member limit (50 to start
  with, set per room). The people list still works: joining and leaving are ordered messages.
- **A room per show or department,** rather than one for the whole studio. It keeps notifications
  relevant as well as cheap. A hub hosts several (§9).
- **More than one core.** One process uses one: the hub and the relay are Python. Rooms don't depend on each other and a room's id is in its URL, so several relay or
  hub processes run behind the proxy, which sends each room to one of them.
- **Bulk data is the relay's real load.** A 2 GB transfer to five peers is 10 GB through it.
  Hence the limit of bytes per second per room (§5.4), and sending a URL instead of a file when
  the files are on a server (§10.3).
- **These are aims, not measurements.** The load test of §15 turns them into numbers.

## 8. The API

### 8.1 C++

```cpp
namespace usdcc::peers {

class Room : public QObject {
public:
    QList<Peer> peers() const;
    Channel* channel(const QString& name, Delivery delivery = Delivery::Ordered);
    StateMap* state(const QString& name);
    PeerState* peerState(const QString& name);
    Transfer* send(const QString& channel, const QString& file, const PeerId& to = {});
Q_SIGNALS:
    void peerJoined(const Peer&);
    void peerLeft(const Peer&);
    void closed(const QString& reason);
};

class Channel : public QObject {
public:
    void send(const QString& type, const QJsonObject& body, const QByteArray& bytes = {}, const PeerId& to = {});
Q_SIGNALS:
    void received(const Message&);
};

class StateMap : public QObject {
public:
    QJsonValue value(const QString& key) const;
    void set(const QString& key, const QJsonValue& value);
    void remove(const QString& key);
Q_SIGNALS:
    void changed(const QString& key, const QJsonValue& value, const Peer& by);
};

}  // namespace usdcc::peers
```

C++ plugins get it through the SDK ([PLUGIN_SDK.md](PLUGIN_SDK.md)).

### 8.2 Python and plugins

`usdcc.peers` has all of it. A plugin that keeps notes on prims, shared by everyone in a room:

```python
# python/notes/__init__.py
import usdcc.peers, usdcc.toast

def activate(plugin):
    usdcc.peers.connect("room_joined", lambda room: share(plugin, room))

def share(plugin, room):
    notes = room.state(plugin.id + ".notes")            # equal on every peer, late joiners too
    notes.on_change(lambda key, value, by: refresh_panel(notes))

    pings = room.channel(plugin.id + ".ping")           # things that happen
    pings.on_message(lambda m: usdcc.toast.show(m.sender.name, m.body["text"]))

def add_note(room, plugin, prim_path, text):
    room.state(plugin.id + ".notes").set(prim_path, {"text": text, "by": usdcc.peers.me().name})
    room.channel(plugin.id + ".ping").send("added", {"text": "New note on " + prim_path})
```

| Call | Gives |
|---|---|
| `usdcc.peers.rooms()`, `host(name)`, `join(link)`, `me()` | Rooms and this peer |
| `usdcc.peers.connect(event, callback)` | `room_joined`, `room_left`, `peer_joined`, `peer_left`, as `usdcc.app.connect()` does for app events |
| `room.channel(name, delivery="ordered")` | `send(type, body, data=None, to=None)`, `on_message(callback)` |
| `room.state(name)` | `get`, `set`, `remove`, `items`, `on_change(callback)` |
| `room.peer_state(name)` | `set(value)`, `get(peer)`, `on_change(callback)` |
| `room.send_file(channel, path, to=None)`, `room.on_file(channel, callback)` | Transfers |
| `room.notify(title, body, kind="info", to=None)` | A notification (§10.2) |

- **Callbacks run on the main thread,** like every script ([SCRIPTING.md](SCRIPTING.md) §3.3).
  A slow one blocks the UI, not the network.
- **Bodies are JSON:** dicts, lists, strings, numbers. `data` is `bytes`.
- **A plugin's channels are its own.** What it opened is closed when it is turned off or
  reloaded, as its panels and tools are ([PLUGINS.md](PLUGINS.md)).
- **Both sides need the plugin.** A peer without it ignores its channels. The room's people list
  shows who has which plugin's channels open, so "Ben doesn't see my notes" has an answer.
- **A plugin can host or join by itself,** but the Peers panel always shows every room the app is
  in, and the user can leave any of them.

*As built* (2026-11-26): [src/python/usdcc/peers.py](../src/python/usdcc/peers.py) over
`_usdcc_app`'s `peers_*` and `room_*` functions (`src/cpp/scripting/src/peers_module.cpp`), on the
app's `PeersController` (`usdcc::scripting::setPeersController()`, which `main.cpp` calls). All of
the table but transfers (`send_file`, `on_file`), which come with phase 5's rest. Also
`room.channel(…).send_private()`, `on_refused()`, `room.on("peer_joined" | "peer_left" |
"closed")`, `room.find(name)`, and `host(…, through_relay=, on_network=)`. A room crosses as a
number held with a `QPointer`, so a closed room's calls raise instead of reaching freed memory;
JSON crosses as text. Every `on_…()` and `connect()` returns a `Connection`, tracked for the
plugin that made it, so it ends when the plugin is turned off or reloaded; a plugin's channels
and maps stay open in the room, with nobody listening. `room.notify()` checks a notification as a
receiver does (§10.2). Tested by `python_peers_rooms_from_a_script` (`usdcc_python_tests`): a
script hosts a room and a C++ member joins; messages with bytes, maps, per-peer state, a private
message and a notification both ways; a disconnected callback hears nothing more; a closed
room's calls raise.

## 9. Other programs

A render farm, an asset tracker, a CI job or a studio's own tool is a peer like any other. The
protocol is written down (`docs/PEERS_PROTOCOL.md`, with phase 1b) and has a version.

**`usdcc_peers`,** a pure-Python package, speaks it with no usdcc and no Qt, with the same calls as
§8.2 where they make sense:

```python
import usdcc_peers

room = usdcc_peers.join(link, name="Render Farm", key_file="farm.key")
room.notify("Render finished", "shot_012 · 240 frames · 1 h 12 m", kind="success", to="anna")
room.state("farm.jobs").set("8812", {"shot": "shot_012", "progress": 1.0})
```

**The hub** is the ready-made way to give a studio one room that is always there:

```
python -m usdcc_peers.hub --name "Studio" --port 47800 --http 8080 --token-file hub.token
```

- **It hosts a room** that never closes. Artists list its link under rooms joined at start (§5.3).
- **Or several rooms,** one per show or department, each with its own link, named in a
  configuration file (§7.5).
- **It takes plain HTTP** for services that can only call a webhook:
  `POST /notify` with a token and a JSON body becomes a notification to the room, or to one person.
- **It decides who may send.** Members read `notify`; only the hub and named services write it
  (§6).
- **It keeps shared state while it runs,** so a map like `farm.jobs` is there for whoever joins.

Two implementations of one protocol, C++ in the app and Python outside, is a cost. They are tested
against each other (§15).

**One machine, one usdcc:** a program that wants to drive a single running usdcc should use
[MCP.md](MCP.md)'s endpoint instead. Peers is for many instances and for data, not for commands.

## 10. Built on it

Each of these uses only §7, and is the test that §7 is enough.

### 10.1 Chat

The first thing built, and the proof that discovery, joining and messages work. Channel `chat`,
ordered.

- **The Peers side panel** (a `SidePanel`): rooms found on this machine and this network, the rooms
  joined, **Host Room…**, **Join by Link…**, the people in a room with their colors, and the chat.
- **A toast** for a message when the panel is closed (a setting).
- **A prim path in a message is a link** that selects the prim, if that stage is open here.
- **History** lasts as long as the room, in memory. A joiner gets the recent messages. It isn't
  saved, and isn't part of a session.
- **The status bar** shows the rooms and how many are in them.
- **Private chats** between two people in a room, sealed so only they can read them (§6.2): a tab
  over the room's chat for each, with an unread count.
- **Presence:** whether someone is around (active, away, do not disturb) beside their name, and
  "Anna is typing…" under the chat.
- **Log messages** shared from the Log panel, to the room or to one person.

### 10.1.1 Shared images

The Render Queue's Share ([FINAL_RENDER.md](FINAL_RENDER.md) §5.5) puts an image in the chat:
to the room, or to one person: then it is in their private conversation (§10.1), sealed for
them when they have a key (§6.2), sent with `to` otherwise; a joiner's history leaves it out.

- **On the wire:** `chat` message `image` {text, preview, previewName, previewBytes, and when
  offered original, originalName, originalBytes, and sharedPath}. Each file is a bundle of one
  file whose hash names it. The receiver asks the sender with `fetch` {hash} (to the sender);
  the sender answers with a transfer on the Direct channel `chat-files`, or `gone` {hash, reason}.
- **Nothing comes unasked.** Every image line waits for **Accept** (the preview and the
  original), **Preview Only** or **Decline**, in the chat or on its toast; the original can be
  downloaded later with a click. There are no exceptions for trusted people. `Peers/acceptImages`
  `never`: lines say images are off, and nothing can be downloaded.
- **The receiver takes** only bundles a line announced, from whoever announced them, that it
  asked for, of one file with an image name (jpg, png, exr, tif) no larger than announced, and
  while the received images stay under `Peers/receivedImagesMaxGB` (2 GB).
- **What arrives is checked:** its first bytes must be the type its name says (PNG, JPEG, EXR,
  TIFF signatures); a file that isn't is deleted at once and the line says "Not an image;
  removed". A preview larger than 8192 px a side isn't decoded. Open uses the default app for
  that type; nothing received is run. A shared-drive path is only shown in Explorer.
- **Flooding:** more than 10 image lines a minute from one sender are left out, with one line
  saying so.
- **The sender** offers an image for 24 hours, to the room or the one person, and can **Stop
  Sharing** it from its line. Received images are deleted after `Peers/keepReceivedImagesDays`
  (30) when usdcc starts; Settings > Peers > Clear Received Images deletes them all.
- They're kept in `<AppLocalData>/peers/chat-images/<hash>/`.

### 10.2 Notifications

Channel `notify`, ordered. Anything in a room that may send on it can raise a toast on every
member, or on one: a colleague's usdcc, a plugin, or a service through the hub.

```json
{ "id": "farm/8812", "kind": "success", "title": "Render finished",
  "body": "shot_012 · 240 frames · 1 h 12 m", "sticky": false,
  "actions": [{ "label": "Open Folder", "open": "file:///P:/shots/012/render" }] }
```

- **It is a toast** ([NOTIFICATIONS.md](NOTIFICATIONS.md)): kind, title, body, timed or sticky.
  It always names the room and the sender, which the host sets and a sender can't fake.
- **The same `id` again replaces the one that is up,** so a job can show its progress. This is new
  to toasts.
- **Actions come from a fixed list:** open a URL or a folder, select prims, open a stage, copy
  text. Each happens on a click, and a URL is shown before it opens. There is no "run".
- **A list in the Peers panel** keeps the recent ones, since toasts go away.
- **Mute** a room or a sender. Settings > Notifications applies as for any toast.
- **Python:** `room.notify(…)` to send. A plugin that wants to react to one, not only show it,
  listens on the channel.

*As built* (§14.5): the body is in [PEERS_PROTOCOL.md](PEERS_PROTOCOL.md) §10. The list in the
Peers panel is the room's chat, where each notification is a line with its actions as links
(`PeersController::notifications()` gives the last 50). A notification with an action not on
the list is turned down whole, and logged. A room this instance hosts lets everyone in it send
them, as it lets them chat; the hub lets only itself and the services it names. Settings > Peers
> Show notifications as toasts, and Do Not Disturb, turn the toasts off; the lines stay. The
same id replaces the toast that is up by closing it and showing the new one. Sending from
usdcc's own Python waits for `usdcc.peers`; from another program it is `usdcc_peers` (§9).

### 10.3 Sending things

**Send to…** on things usdcc can already write out. A transfer, or a message for the small ones.
The receiver gets a toast and decides.

| Thing | Sent as | The receiver can |
|---|---|---|
| The selected prims | USDA text | Paste them into a stage |
| A layer or a stage file | The file, with its dependencies if asked (the ones the Dependencies panel lists) | Open it from the inbox |
| A session | Its JSON, as [SESSIONS.md](SESSIONS.md) §5.1 exports it | Import it. This replaces sending the file by mail; missing paths are located as that section describes |
| A Script Editor tab | Its text | Open it as a new tab. It is never run |
| A camera | Its position and lens | Look through it |
| A stage on a server | Its URL only ([REMOTE_ASSETS.md](REMOTE_ASSETS.md)) | Open it; both resolve it themselves |

Anything over a size limit (100 MB to start with) asks before it is received.

### 10.4 A shared stage

Editing one stage together, each seeing the others' edits, selections and cameras. The largest use,
with a page of its own: [SHARED_STAGE.md](SHARED_STAGE.md). It takes an ordered channel with its
own snapshot for the edits, per-peer state for presence, and a transfer when a peer lacks the files.

### 10.4a Remote support

Showing usdcc's windows to a colleague and, if allowed, letting them work in them: a room with the
helper as a guest. Off by default and optional in the build: [REMOTE_SUPPORT.md](REMOTE_SUPPORT.md).

### 10.5 What a plugin could build

Not planned here; listed to check that §7 covers them.

| Idea | Uses |
|---|---|
| Review notes pinned to prims | A map |
| A shot's approval status from the tracker, shown in the Outliner | A map the tracker's service writes |
| Render and bake jobs with progress | A map, and notifications |
| "A new version of this asset was published. Reload?" | A notification with an action |
| A renderer plugin showing a colleague's finished frame | A transfer |
| A laser pointer in the viewport during a review | Per-peer state |

Built since: final renders shared between instances
([FINAL_RENDER.md](FINAL_RENDER.md) §7). It uses per-peer state for what each instance renders,
direct messages for jobs and tasks, and transfers for scenes and images.

## 11. Settings > Peers

Name and color; whether rooms on this network are looked for; the port; rooms joined at start;
relay URLs; ask before a join by link; toast on chat; muted rooms and senders; the inbox's size
limit. ([SETTINGS.md](SETTINGS.md))

## 12. Security and privacy

- **Nothing leaves the machine until the user hosts, looks, or lists a room to join.** An
  announcement holds the room's name and the host's display name.
- **No remote code.** A received script is text in a tab. A notification's actions come from a
  fixed list. A plugin's channel hands data to a plugin the user installed; what the plugin does
  with it is the plugin's.
- **A room joined at start is trusted like a plugin folder:** someone put it there. Its link holds
  the host's key, so another machine can't pose as the studio's hub.
- **The hub's HTTP side needs a token,** and should sit behind HTTPS when it isn't on a closed
  network.
- **A misbehaving peer** meets the limits of §7.4, and the host can remove it. Who may send is
  enforced by the host, not just hidden in the UI.
- **Secrets** (the private key, a room's password, a link's secret, the hub's token) are never
  logged. The log category is `usdcc.peers` ([LOGGING.md](LOGGING.md)).

## 13. Build and deploy

- **vcpkg:** `qtwebsockets`, `mdns` and `libsodium` (*checked:* all three are ports in the pinned
  vcpkg), and `qthttpserver` for the guests' page, from an overlay port that makes it depend on
  `qtwebsockets`: upstream's doesn't, and is then built without its WebSocket upgrade
  ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md) §4.1). Phase 1a needs only the two Qt ones. Qt
  Network and its OpenSSL backend are already built: *checked,* `network` and `openssl` are
  default features of the `qtbase` in use. New licences: mdns (Unlicense) and libsodium (ISC).
  All four are in `vcpkg.json` since phases 1a to 2.
- **CMake:** `Network`, `WebSockets` and `HttpServer` join `find_package(Qt6 … Widgets OpenGL Svg Test)`
  (done in phase 1a, §14.1).
- **Worktrees:** new ports need a private vcpkg install root until they are on master
  ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12).
- **Deploy:** `Qt6Network.dll`, `Qt6WebSockets.dll` and `usdcc_peers.dll` in `bin/`, and the
  `usdcc_peers` Python package (hub and relay included) in the package. TLS is only needed for `wss://` to a relay,
  which also needs Qt's TLS plugin. **OpenSSL's `libcrypto-3-x64.dll` is needed from phase 1a,
  TLS or not:** Qt Network as built here links it at load time, and without it `Qt6Network.dll`
  doesn't load (*measured* in the broadcast spike, [VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md)
  §8). PySide6 ships a `libcrypto` of the same file name ([REMOTE_ASSETS.md](REMOTE_ASSETS.md)
  §12); both are OpenSSL 3.6.3 today (*checked*), so they don't clash yet.
- **PySide6** needs no QtWebSockets binding: `usdcc.peers` is pybind11.
- **The Python package** needs `websockets` and `PyNaCl` where it runs. The app doesn't use it.

## 14. Phases

| Phase | Builds | Done when |
|---|---|---|
| **0. Spike** | Throwaway programs: a Qt WebSocket host and joiner with mDNS, and a Python joiner | It answers: what Windows Firewall does for the host and for a peer that only looks; whether `mdns` works beside Windows' own mDNS, with two instances on one machine and across two machines; whether a Python peer (`websockets`, `PyNaCl`) completes the handshake with the C++ host; a first number for how many small messages a second one Python process forwards with 2,000 connections open |
| **1a. Rooms and channels** | `usdcc_peers` with hosting and joining by address, the frame, channels and their three kinds of delivery, who may send, limits, and guests with their page (§6.1). Built for [VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md) phase 1, its first user; until 1b only guests join | Its tests pass on loopback: ordered messages arrive in one order on three members, a latest-only message replaces one still waiting, a slow member is dropped, a wrong secret is refused, a guest is refused a channel not opened to it |
| **1b. The rest of the framework** | Identity, the handshake and encryption with the room key, reconnecting, shared state, several rooms, `usdcc.peers`. The protocol written down | Two peers join by address and nothing readable is on the wire; a reconnect catches up; a wrong key is refused; a map ends equal after both sides set the same key; a late joiner has the map. A test plugin shares a map from Python |
| **2. Discovery and chat** | The files of §5.1, mDNS, the Peers panel, chat, toasts, settings | Two usdcc on two machines find each other with no address typed, and chat |
| **3. Other programs and notifications** | The `usdcc_peers` Python package, the hub, rooms joined at start, the `notify` channel and its list. **Built** 2026-11-26 (§14.5) | Two usdcc join a hub at start; a script's `notify` and a `curl` to the hub both show as toasts on both; a member that isn't allowed to send is refused; the hub meets its load target (§15) |
| **4. Links and the relay** | Invite links, `usdcc_peers.relay` and its Dockerfile, `wss://`. **Built** 2026-11-26 (§14.6) | Two instances on different networks chat through a relay whose log shows only sizes; the relay meets its load targets (§15) |
| **5. Transfers** | Transfers, the inbox, **Send to…**. The generic part is built (§14.4) | A session sent from one instance loads on the other; a plugin sends a file from Python |
| **6. A shared stage** | [SHARED_STAGE.md](SHARED_STAGE.md) | Phases 0 and A to C built (2026-11-22); see there |
| **Later** | Moving the room to another host, `usdcc://` registered with the OS | |

Only phase 4 needs a relay or a decision about one: a hub on a machine the artists reach is
joined by its address. The shared stage needs only phases 1a and 1b, and can start whenever it is
wanted.

The broadcast's own spike ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md) §11.1) answers the
hosting half of phase 0. On one machine it showed that a room's page and WebSocket share one port
through Qt HttpServer. What Windows Firewall does for the host is still to be tried, from a second
machine. The mDNS half stays here and isn't
needed before phase 2. Replaying a layer's edits is now [SHARED_STAGE.md](SHARED_STAGE.md)'s own
spike.

### 14.1 Phase 1a as built

`usdcc_peers.dll`: `Room` and `Channel` (`room.h`, `channel.h`), the frame (`frame.h`), and the
types of §7.1 (`types.h`). Qt Core, Network, WebSockets and HttpServer; no USD, no widgets, no
`usdcc_core`. Log category `usdcc.peers`; no secret is logged.

**Its test** (`usdcc_peers_tests`, a program of its own, 13 tests, about 12 s): the five criteria of
row 1a, and also the frame and channel names, guests and `Origin`, the page, who may send, direct
messages, every limit, removing, leaving and closing. A member that has to fall behind runs on a
thread of its own whose event loop the test stops. Its socket then reads nothing, the operating
system's buffers fill, and the host's queue for it grows: a real slow member, not a fake one.

**Where it differs from the plan:**

- **Members that aren't guests join too,** by address (`Room::join("host:port", secret)`), with
  the room's secret and no identity. The plan had only guests join until 1b. The criteria need
  three members, and the broadcast's test needs a C++ member as well. Phase 1b replaces the
  secret with the handshake of §6; links (§5.3) are phase 4.
- **Two secrets:** one for peers and one for guests. A guest's link can't make a peer.
- **The frame, exactly:** a 4-byte big-endian length, the JSON header (`channel`, `type`, `from`,
  `to`, `seq`, and `body`, the JSON body), then the raw bytes. A WebSocket *text* message is a
  header with no bytes, which is easier from a page; the host always sends binary. A header may
  say `latest: true`, which matters only on a channel the host hasn't opened (below).
- **The host's channel `room`** carries the protocol itself: `hello` (the version, the secret, a
  name), `welcome` (the member's id, the people, the channels it reaches, the current `seq`,
  the room's `type`, §5.5),
  `denied`, `joined`, `left`, `leave`, `removed`, `closed`, and `refused` for a message the host
  turned down. Nobody else may open `room`. `joined` and `left` are ordered like any message.
- **Ordered messages come back to their sender,** with their `seq`, so every member, the host
  included, sees one order with its own messages in it.
- **Latest only** replaces a waiting message of the same channel, type, sender and recipient.
  "Waiting" is the host's queue for each member: frames go to a socket only while it holds less
  than 128 KB unwritten, so the queue, not the socket, is where they wait and can be replaced.
  A latest-only channel can't make a member too slow.
- **Direct:** naming a recipient makes any message direct (no `seq`, no echo). On a channel opened
  as `Delivery::Direct`, naming nobody means the host.
- **The host's channels rule.** Who may send (`setSenders`, `setNamedSenders`) and whether guests
  reach a channel (`setOpenToGuests`, off by default) are set on the host's `Channel`. On a channel
  the host hasn't opened, any peer may send, delivered as the sender's header says, and no guest
  reaches it.
- **Guests and `Origin`:** an upgrade with an `Origin` (a page) is let through only from the room's
  own address (the request's `Host`) or an origin the host lists (`HostOptions::allowedOrigins`,
  for a tunnel), and only if the room takes guests. A page can only be a guest, even with the
  peers' secret. The page is the host's (`HostOptions::page` or `pageDirectory`) or a plain built-in
  one with the people and `chat`.
- **Limits** (`Limits`, per room): members 50, guests 8, a message 4 MB, 200 messages a second
  with bursts of 400, 16 MB waiting for one member, 10 s to say hello, a ping every 10 s and 30 s
  of silence. A member over a limit is disconnected with the reason, which the others get in
  `left` and the host in `peerLeft`. Bytes per second aren't limited yet.
- **The API** has what §8.1 has without shared state and transfers, and also:
  `Room::host()` and `Room::join()` to make one, `state()`, `me()`, `hostPeer()`, `close()`
  (leave, or on the host end the room), `remove()`, `guestLink()`, `queuedBytes()`, a reason with
  `peerLeft`, and `Channel::refused`.
- **Ids** are random, given by the host for as long as a member is connected, the host's included.
- **No clock the test moves** (§15): the limits test runs with short real intervals instead.

**Built with it** ([VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md) §8 items 5 and 6):
`qtwebsockets` and `qthttpserver` in `vcpkg.json`, the latter from `cmake/ports/qthttpserver`,
which makes it depend on `qtwebsockets`; `Network`, `WebSockets` and `HttpServer` in
`find_package(Qt6 …)`. Beside `usdcc.exe`: `usdcc_peers.dll`, `Qt6Network.dll`,
`Qt6WebSockets.dll`, `Qt6HttpServer.dll`, `Qt6Concurrent.dll`, `libcrypto-3-x64.dll`, `jpeg62.dll`
and `imageformats/qjpeg.dll`, which `tools/deploy.py` checks for.

**Not tried yet:** a member on another machine, and with it Windows Firewall.

Superseded by phase 1b (§14.2): members now join with a key and the encrypted handshake, not the
secret alone, and the protocol is [PEERS_PROTOCOL.md](PEERS_PROTOCOL.md).

### 14.2 Phase 1b as built

**Its tests** (`tests/peers_rooms_test.cpp`, in `usdcc_peers_tests` with 1a's): the criteria of
row 1b, Python's aside. A small TCP proxy in the test sits between a member and the host, keeps
every byte and can cut the connection:

| Criterion | Test |
|---|---|
| Two peers join by address and nothing readable is on the wire | `peers_nothing_readable_on_the_wire`: chat, a map value, a latest-only picture, names and the secret don't appear in what passed the proxy |
| A reconnect catches up | `peers_reconnect_catches_up`: the proxy cuts a member, three messages go out, it comes back with all of them once, in order |
| A wrong key is refused | `peers_wrong_key_is_refused` |
| A map ends equal after both sides set the same key; a late joiner has the map | `peers_shared_state`, also with 300 random sets and removes from three sides |
| A test plugin shares a map from Python | Since 2026-11-26: a script shares a map, messages and per-peer state with a C++ member (`python_peers_rooms_from_a_script`, §8.2) |

Also tested: identities and links, a member away too long, per-peer state and channel snapshots,
the room key changing, and every kind of admission.

**What it is:**

- **Identity** (§4): an Ed25519 key pair from libsodium. On Windows it is kept in the credential
  store as "usdcc peers identity (<application name>)", so a test profile has its own. **On
  Linux and macOS it is a file** (`<AppLocalData>/peers/identity.key`, readable by its owner
  only): no credential store there yet. The key is made the first time a room is hosted or
  joined, not at start. A peer's id is `<fingerprint>-<instance>`. The fingerprint is 32 base32
  characters of a BLAKE2b hash, shown as `abcd-efgh`.
- **The handshake and encryption** (§6): exactly [PEERS_PROTOCOL.md](PEERS_PROTOCOL.md) §2–3.
  The room key changes whenever a peer's connection ends, also for a short break, not only when
  it leaves. A program that joins with the guests' secret is encrypted too; only a page on a plain
  link isn't (as planned).
- **Reconnecting** (§7.4): the host keeps a peer whose connection broke for 30 s without telling
  the others, and the last 2,000 ordered messages (or 8 MB). Back in time, the peer gets what it
  missed; later, everything afresh. A member that was dropped as too slow rejoins the same way,
  as §7.4 says. One told it was removed doesn't.
- **Shared state** (§7.2): `Room::state()` gives a `StateMap`, `Room::peerState()` a `PeerState`,
  and `Channel::setSnapshotProvider()` is the snapshot "when a map isn't enough". Added: the host
  may say who writes a map (`setSenders`), as for channels. Guests don't see maps; a per-peer map
  can be opened to them.
- **Several rooms** (§2): `Peers` holds every room of a program, hosted and joined, and finds rooms
  (§14.3).
- **Who may join** (§6): `Admission::Invited`, `Ask` and `Open`, a password, "always allow"
  (`HostOptions::trustedFingerprints`, `Room::trust()`), "ask before a join by link", and
  `Room::joinRequested` with a `JoinRequest` to allow or deny; unanswered, a no after 120 s.
- **Not built:** the pipe of §15 that delays and reorders (the random test runs over real
  sockets); a clock the test moves. `usdcc.peers` is built since (§8.2).

### 14.3 Phase 2 as built

**Its tests:** `tests/peers_discovery_test.cpp` (this machine's folder, a fake network, public
rooms, several rooms at once, a stale file), `peers_mdns` in the same file against the real network
(only with `USDCC_PEERS_MDNS_TEST=1`), and `tests/peers_panel_test.cpp` in `usdcc_ui_tests`. That
last one runs two instances' managers in one process: one finds the other's room in its panel and
joins it with a double-click, the people list shows their stages, chat goes both ways with a prim
path as a link, and a late joiner gets the chat so far.

**What it is:**

- **This machine** (§5.1): as planned, a file per room in `<AppLocalData>/peers/rooms/`, watched,
  and stale ones removed. An invited room isn't listed. Two instances of one test profile share
  the folder; two profiles don't.
- **This network** (§5.2): the `mdns` library. **Measured, one machine:** a room announced on
  port 5353 beside Windows' own mDNS was found at the machine's LAN address within a few seconds.
  A host announces only rooms on a network address that don't need an invitation. A member asks
  only while the Peers panel shows and "Look for rooms on this network" is on. Rooms are
  asked for and announced on every adapter that is up: left to itself the system multicasts on
  one, and with a VPN or virtual adapters that needn't be the LAN (*seen:* Windows, OpenVPN and
  WSL: a question on the default adapter got no answer, on Ethernet the room). Not tried: subnets.
- **Public rooms:** **Host Room…** offers "Only those with the invite link", "Anyone on this
  network, once I allow them" (the default) and "Anyone on this network (a public room)".
- **Hosting** listens on every address, on the port of Settings > Peers or any free one, so
  Windows asks about its firewall the first time. Not tried yet; it is the user's to answer.
  Its invite link is copied when the room starts.
- **The Peers panel** (§10.1): your name, **Host Room…** and **Join…** (an invite link, or an
  address that asks the host); your rooms, this machine's and this network's; the selected
  room's people with their color and stage (`presence`), and its chat. A room's menu: **Copy
  Invite Link** and **Rename Room…** on the host (everyone in it sees the new name, and it is
  announced by it), **Mute**, **Leave Room** / **Close Room**. A person's: **Mute**, and on the
  host **Always Allow** and **Remove**.
- **Chat** (§10.1): channel `chat`, ordered; history in memory, the last 1,000 lines a room; a
  joiner gets the last 200 as the channel's snapshot. A prim path in a message is a link that
  selects the prim in the current stage, if it has it (the message doesn't say which stage).
  Text is never HTML. Messages are grouped: the name and time on a line, what was said indented
  below, and what the same person says within five minutes under the same name; what the room
  itself says (joins, renames) is in grey and ends a group. A message may span lines: Enter sends it, Shift+Enter starts a new line,
  and the box grows to six lines before it scrolls. Beside it, two square buttons: a 🙂 that
  offers 40 emoji, the last 8 used first (kept in `usdcc.ini`), and Send, a paper plane.
  Windows' own picker (Win + .) works in the message too.
- **Private chats** (§6.2): **Message Anna** in a person's menu, or a double-click on them,
  opens a tab over the chat beside **Everyone**. A message there goes as `say` on `chat` through
  `sendPrivate()`, so no one else gets it, the host included. It's shown here at once, since nothing
  comes back. The tab shows the person's dot and an unread count. It closes with its ×, stays
  once they leave (saying so) but can't be written in, and is kept in memory only, like the
  room's chat. Guests and peers without a key can't be messaged ("needs a newer usdcc").
- **Share from the Log panel:** select messages, right-click, **Share ▸**, then a room and
  **Everyone** or **Anna (private)**. Type `log` on `chat`, body `{records: [{time, level,
  source, category, text, details?}]}`, at most 50 records and 64 KB; more and the menu says
  which go. The chat shows them under the sender's name, one line each as the Log panel does
  (time, level in its color, category, text), the details below. An older peer ignores `log`.
- **Presence:** `presence` also says `status`: `active`, `away` (no input in usdcc for five
  minutes, or the window minimized) or `dnd`. Do not disturb is chosen from the menu on **You:**
  and kept in `usdcc.ini`; it silences chat toasts, while join requests still ask. A small mark on
  each person's dot says which: green, amber, red. While someone writes, channel `typing` (latest
  only) says `typing` at most every 2 s, and `stopped` on sending. It goes to the room for the
  room's chat, and to the one person for a private one, so no one else learns who writes to whom.
  "Anna is typing…" shows under the chat until 5 s after the last.
- **Toasts:** a chat message while the panel is closed (a setting; muted rooms and people
  excepted, and none while you don't want to be disturbed; a private one says so, and opens its
  tab), "Anna wants to join" with **Allow**, **Always Allow Anna** and **Deny**, and a room
  that ends or refuses.
- **The status bar** says the rooms and how many are in them, and opens the panel.
- **Settings > Peers** (§11): name, color, looking on the network, port, asking before a join by
  link, chat toasts. Muted rooms and people and the trusted keys are kept in `usdcc.ini`. Not yet:
  rooms joined at start (phase 3), relay URLs (phase 4), the inbox's limit (phase 5).
- **Deploy:** also `libsodium.dll` beside `usdcc.exe`, and Qt's TLS plugin `tls/qschannelbackend.dll`
  (Windows' own TLS: without a TLS plugin Qt Network warns "No functional TLS backend was found",
  and `wss://` to a relay can't work), which `tools/deploy.py` checks for.

**Not tried yet:** two machines (the row's "done when"), the firewall's question, a LAN with
several interfaces.

**Across a network boundary, 2026-11-22.** Windows and WSL 2 in NAT mode on one PC stand in for
two machines: WSL has its own address (172.29.x), and its connections to Windows' LAN address
go through Windows Firewall. A `usdcc_node queue` hosted on Windows (`0.0.0.0:47911`, the LAN
address in its link), a `usdcc_node serve` on Linux joined by that link, and a usdcc on Windows
joined too. Handshake, room key, the members' list and transfers both ways worked: the Linux
node took the scene and sent back three EXRs (RENDER_NODE.md §7). **The firewall's question:**
Windows asked once per program path, and what was answered stayed a rule: an Allow for
`usdcc_node.exe`, a Block for `usdcc.exe` (the prompt cancelled). With the Block rule the app's
room was unreachable from WSL while the app showed and logged nothing about it. Not tried: mDNS
discovery across the boundary (WSL's NAT passes no multicast), a second physical machine, the
Peers panel's chat between the two.

### 14.4 Phase 5 as built so far: transfers

The generic part, for final-frame rendering to send a shot to its render nodes and get the
frames back: `Transfer` and `TransferInbox` in `usdcc_peers` (`transfer.h`, §7.3), with the
protocol in [PEERS_PROTOCOL.md](PEERS_PROTOCOL.md) §9. No UI, no **Send to…**, no Python yet.

**Its tests** (`tests/peers_transfer_test.cpp`, in `usdcc_peers_tests`, about 8 s):

| What | Test |
|---|---|
| Several files in folders, one empty, from one member to another through the host | `peers_transfer_files` |
| A 5 MB file in pieces of 1 MB | `peers_transfer_large_file` |
| The proxy cuts the receiver off halfway; it comes back and the rest arrives, without what had arrived | `peers_transfer_resumes_after_cut` |
| A piece whose bytes were changed, and one of the wrong size, are refused; the file isn't kept | `peers_transfer_corrupt_chunk_refused` |
| The same bundle again: acknowledged, nothing sent | `peers_transfer_cache_hit` |
| `..`, absolute paths, drive letters, `\`, and a bundle over the inbox's limit are refused, and nothing is written outside the inbox | `peers_transfer_unsafe_paths_refused`, `peers_transfer_paths` |
| The receiver cancels, then the sender does | `peers_transfer_cancel` |

The proxy that cuts a member off moved from `peers_rooms_test.cpp` to `peers_test_support.h`.

**What it is:**

- **The API:** `TransferManifest::fromDirectory()` or `fromFiles()` hash a bundle;
  `Transfer::send(room, channel, to, manifest)` offers it; a `TransferInbox` on the receiving
  member's room and channel takes it. Both ends have a `Transfer` with `progress`, `finished`,
  `failed`, `resumed` (the sender's) and `cancel()`. Not §8.1's `Room::send(channel, file)`: a
  bundle of files to one member is what rendering needs, and to everyone is later.
- **`Room::queuedBytes()` on a member** that joined is now its own send queue to the host, whoever
  the message is for, so that a sender there can pace itself too.
- **Pacing:** the host's queue for the receiver is what drops a member as too slow (16 MB), and a
  sender that isn't the host can't see it. The window of 4 MB unacknowledged is what keeps it
  under that. Several transfers to one member at once each have their own window.
- **After a break** either side may be the one that noticed: the one whose room resumed asks
  (`accept` from the receiver, `offer` again from the sender), a sender that hears nothing for
  5 s offers again, and a receiver that gets a piece after a gap asks for the missing one. A
  sender that hears nothing for 2 minutes, or whose receiver leaves, fails; the part is kept.
- **Hashing on the sender** is on the calling thread, and so is checking a part that was already
  complete when a transfer resumes.
- **Symbolic links:** a sender's bundle with one is refused (`fromDirectory`), and a receiver never
  writes through one. Not tested on Windows, where making one needs Developer Mode.

### 14.5 Phase 3 as built: other programs and notifications

Built 2026-11-26.

- **`usdcc_peers`** ([src/usdcc_peers](../src/usdcc_peers), its README says how to use it): the
  protocol in pure Python on asyncio, with `websockets` and `PyNaCl` (pinned in its
  `requirements.txt`). A member (`Member.join()`) and a host (`Host`), and `usdcc_peers.join()`
  for a script without asyncio, which runs the rooms on a thread of their own. Everything of
  PEERS_PROTOCOL.md but guests (a connection with an Origin is refused), transfers (§9 there) and
  discovery (§8): a program joins by link or address. A key file holds the same 64 bytes as the
  app's identity file.
- **The hub**, `python -m usdcc_peers.hub` (or `usdcc-hub`): one room or several from a JSON
  file, each on its port; `notify` limited to the hub and the services named by fingerprint
  (`--notify-sender`, a room's `notifySenders`); its key and each room's secret in its data folder,
  so the links stay the same, and the links in `links.json` there, readable by its owner only.
  The HTTP side: `POST /notify`, `POST /state` and `GET /rooms`, each with the token.
- **In the app:** the `notify` channel in every chat room (§10.2), and rooms joined at start
  (§5.3), in `PeersController`; a notification's line in the Peers panel's chat, and
  **Join When usdcc Starts** in a room's menu. The actions' hooks (`selectPrims`, `openStage`,
  `confirmOpen`, which asks before a URL opens) are set in `main.cpp`.
- **Not built:** a hub that writes its maps to disk (§16 #5); the hub hosting a shared stage
  ([SHARED_STAGE.md](SHARED_STAGE.md) §12 #4); several hub processes for one studio (§7.5).

**Its tests:**

| What | Where |
|---|---|
| The package against itself: links, key files, notifications checked, chat, `notify` refused to a member and allowed to a named service, maps and a late joiner, who gets in (wrong secret, wrong key, no invitation), a reconnect that catches up, the host closing, the hub's HTTP side and its token, the links staying the same | `tests/peers_python/test_usdcc_peers.py`, CTest `usdcc_peers_python_tests` |
| C++ members on the Python hub: chat through it, a webhook's notification to one person and to everyone, a member's notification refused, a map the hub sets and one a member sets, a member back through a cut proxy and caught up, the hub stopping | `peers_python_hub_with_cpp_members` (`tests/peers_python_test.cpp`, in `usdcc_peers_tests`) |
| A Python script in a room the app hosts: chat both ways, a map, a refused notification, leaving | `peers_python_member_in_cpp_room` |
| A notification's toast naming the room and sender, replaced by the same id, its Select action, one with a `run` action turned down, a muted room | `peers_notifications` (`usdcc_ui_tests`) |
| Rooms joined at start: from the setting, joined again after being removed, not after leaving, and the room menu's switch | `peers_rooms_joined_at_start` |

The Python tests and `peers_python_test.cpp` need `build/peers-venv`, which the build makes with
the bundled uv from `requirements.txt` (network needed the first time); without uv they are left
out.

**Load** (`tools/peers_load.py`, the hub in its own process, members spread over worker
processes, on one Apple M-series Mac, on the loopback):

| Members | Joining | A message to everyone reaches all | The hub idle |
|---|---|---|---|
| 500 (8 workers) | 8 s | 51 ms | 54 MB |
| 1,000 (10 workers) | 27 s | 69 ms | 70 MB |
| 2,000 (10 workers) | 99 s | 100 ms | 104 MB |

Joining grows faster than the room: each welcome lists everyone, and each join is announced to
everyone, so 1,000 joining at once (a studio logging in at nine) take 27 s in all; the hub's
listen backlog is 1,024 for that. Everyone leaving at once costs the same, as each departure
changes the room key for those still there (§6). Both are the protocol's, the C++ host's too.

### 14.6 Phase 4 as built: links and the relay

Built 2026-11-26.

- **The relay**, `python -m usdcc_peers.relay` (or `usdcc-relay`): needs only `websockets`. A host
  opens `/host` (with the token, if the relay has one) and gets a room id; members open
  `/r/<id>`. It forwards bytes it can't read, logs connections and byte counts, and limits members
  per room, a message's size and a room's bytes a second. `--certfile` and `--keyfile` give it
  TLS of its own; otherwise it sits behind an HTTPS proxy for `wss://`.
- **The Dockerfile** (`src/usdcc_peers/Dockerfile`) runs the relay by default, or the hub.
- **Links:** `usdcc://join?relay=<room's URL on the relay>#k=…&s=…`, made and read by
  `makeJoinLink()` and `parseJoinLink()` (and `usdcc_peers.make_link()`), whose `JoinLink` has a
  `relay` beside `address`. `Room::join()` takes a `ws://` or `wss://` URL as well as an address.
- **Hosting through it:** `HostOptions::relay` and `relayToken`. The room listens on 127.0.0.1;
  `RelayBridge` gets its id on the relay and opens a loopback connection for each member. What the
  room sealed once for everyone goes to the relay once with its members' list, and the relay
  copies it. `Room::link()` names the relay once it has the room (`relayReady()`); if the relay is
  lost, the bridge asks for the same id again, so the link still works, and the members come back
  through it and catch up. `relayFailed()` says why it isn't there, a wrong token included.
- **In the app:** Settings > Peers > Relay and Relay token; Host Room's **Through the relay**,
  which makes the room invited-only and copies its link once the relay has it.
- **The hub** can be reached through a relay too: `--relay`, `--relay-token-file`; `links.json`
  then has each room's `relayLink`, and the hub asks for the same ids each time it starts.
- **Not built:** guests through a relay (§6.1); one relay over several processes (§7.5): a studio
  that outgrows one runs two, each with its own URL; registering `usdcc://` with the OS (later).

**Its tests:**

| What | Where |
|---|---|
| A Python host through the relay with three members, and one copy sent for all; a wrong token; a room that isn't there; the relay restarting, the room getting its id back and a member catching up; `wss://` with a test CA, and refused without it; the hub through the relay, keeping its id | `tests/peers_python/test_usdcc_peers.py` (`RelayTests`) |
| A C++ host through the Python relay, two C++ members and a Python one; a wrong token; the link made and read; nothing said in the relay's log; the relay restarting on its port, the room on it again with the same URL, and the members back | `peers_relay_with_cpp_and_python` (`tests/peers_python_test.cpp`) |
| `wss://` from C++, with the test CA trusted by the test process | `peers_relay_wss` |

`test_data/peers` has the test certificate and its CA (whose key was thrown away).

**By hand:** the image built with `docker build`, the relay run from it, and a host with two
members on the Mac chatting through it; its log held only connections and byte counts. Two
machines on two networks haven't been tried.

**Load** (`tools/peers_relay_load.py --raw`: fake hosts and members that speak only the relay's
frames, in 12 worker processes, the relay in its own; one Mac, the loopback; each of a room's four
members sends 20 messages of 100 bytes a second, which its host sends back to all four as one
copy):

| Rooms of five | The relay's CPU | Forwarded: median, 99th percentile |
|---|---|---|
| 100 | 32% of a core | 14 ms, 20 ms |
| 100, while one more room moves 600 MB at 30 MB/s | 36% | 14 ms, 20 ms |
| 200 | 66% | 31 ms, 66 ms |

So one relay process meets §15's target (99 of 100 within 50 ms) for 100 rooms, and 200 need two
relays. Without `--raw`, with usdcc_peers hosts and members doing the crypto in the workers, the
workers are what is slow, not the relay (14% of a core for 40 rooms).

## 15. Testing

- **`usdcc_peers`** (`tests/peers_test.cpp`): several rooms and joiners in one process over
  `127.0.0.1`, with a clock the test moves for pings and timeouts. Shared state gets a pipe that
  delays and reorders: random sets and removes from every side must end in equal maps. Discovery
  sits behind an interface with a fake, since multicast isn't dependable in CI ([CI.md](CI.md): no
  external network).
- **Python in the app:** a test plugin opens a channel and a map, and loses them on reload.
- **C++ against Python:** the fixture starts the Python hub on a free port, like the HTTP test
  server ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §7); a C++ peer joins, gets a notification posted
  over HTTP, and sees a map the hub set. This is what keeps the two implementations equal.
- **The relay:** started by the fixture the same way.
- **Load** (`tools/peers_load.py`, run by hand and before a release, not in CI): many Python peers
  from one script against a hub or a relay, reporting how long delivery takes. The targets to
  start with, to be corrected by the first measurements:

  | Case | Target |
  |---|---|
  | A hub with 2,000 members sends a message to everyone | All have it within 100 ms |
  | A relay with 200 rooms of five peers, each peer sending 20 small messages a second | 99 of 100 messages are forwarded within 50 ms |
  | The same, while one room transfers 1 GB | The other rooms still meet it |
  | A hub or relay with 2,000 idle connections | Under 500 MB of memory |

  A case that misses its target with one process is run again with several (§7.5), to check that
  spreading rooms over processes works.
- **UI** (`usdcc_ui_tests`): the Peers panel against a fake room; a notification's toast, its
  replacement by id, and its actions.
- **By hand:** two instances with two test profiles on one machine, then two machines.

## 16. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | The names: peers and rooms? | Yes. "Session" is taken, and "collaboration" names only one use of this |
| 2 | mDNS through the `mdns` library, the OS's services, or our own broadcast? | The library, if the spike shows it works beside Windows' own mDNS. Otherwise the OS's services on Windows and the library elsewhere |
| 3 | Does the project run a public relay? | No. It ships the program, and a studio or user runs it. A public one costs money and attracts abuse; revisit when people ask |
| 4 | What happens when the host leaves? | The room ends, and each member keeps what it had. Moving the room to another member is later. A hub doesn't leave |
| 5 | Is shared state kept after the room ends? | No. A plugin saves what it wants to keep; the hub keeps its maps while it runs. A hub that writes them to disk is a small later step |
| 6 | Is chat history saved? | No |
| 7 | Our own encryption (libsodium), or TLS between peers? | libsodium: it is the same directly and through a relay, with no certificates to trust by hand, and Python has it as `PyNaCl` |
| 8 | A second implementation in Python, or only bindings to the C++ one? | A second one. A service shouldn't need Qt or a usdcc build to send a notification. The cost is keeping two in step, which the tests of §15 do |
| 9 | Should the hub and the relay be one program? | No: one package, two commands. The relay can't read what it forwards; the hub is a member that can. One process doing both would blur which one a studio trusts with what |
| 10 | Which actions may a notification carry? | The four of §10.2. More only when a use needs one, and never one that runs code |
| 11 | Is a guest on a plain `http://` LAN link encrypted? | Not in the first version (§6.1). The panel says so beside the link. Add the page's key exchange there too if people broadcast on networks they don't trust |
| 12 | How large a room and how many rooms are promised? | None until measured. The targets of §15 are the aim, and §7.5 says what to do beyond them |

## 17. Not planned

- **WebRTC and hole punching** (`libdatachannel`). It still needs a server to introduce the peers
  and a relay for the routers it fails on, and it is a large dependency. Worth another look only
  if a relay's bandwidth becomes the problem.
- **A mesh with no host.** It needs every peer to accept connections, and gives no single order.
- **CRDTs.** With a host giving the order, last writer wins per key is enough to start.
- **Accounts, a user directory, a cloud service.**
- **Commands to a running usdcc.** That is [MCP.md](MCP.md).
- **Operating-system notifications.** Toasts appear inside usdcc only
  ([NOTIFICATIONS.md](NOTIFICATIONS.md)).
- **Syncing the files on disk.** That is what a shared drive, version control and
  [REMOTE_ASSETS.md](REMOTE_ASSETS.md) are for.
- **Voice and video.**
