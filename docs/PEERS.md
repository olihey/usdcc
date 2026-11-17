# Peers (plan)

A general way for running usdcc instances, their plugins and other programs to find each other
and share data: messages, state that is kept equal everywhere, and files. Chat, notifications from
a studio's services, sending things to a colleague and editing one stage together
([SHARED_STAGE.md](SHARED_STAGE.md)) are all uses of it, and a plugin can add its own from Python.

**Status:** planned 2026-11-16, reworked 2026-11-17 as a framework first, with stage sharing moved
to its own page. Nothing is built yet. "Checked" below means looked at in this repository or its
pinned vcpkg; everything else is an assumption until the spike (§14).
Also 2026-11-17: rooms and channels (phase 1a) are built first, for
[VIEWPORT_BROADCAST.md](VIEWPORT_BROADCAST.md), which also brought browser guests (§6.1).

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
(§9) without doing anything.

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
- **CMake:** `Network` and `WebSockets` join `find_package(Qt6 … Widgets OpenGL Svg Test)`.
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
| **3. Other programs and notifications** | The `usdcc_peers` Python package, the hub, rooms joined at start, the `notify` channel and its list | Two usdcc join a hub at start; a script's `notify` and a `curl` to the hub both show as toasts on both; a member that isn't allowed to send is refused; the hub meets its load target (§15) |
| **4. Links and the relay** | Invite links, `usdcc_peers.relay` and its Dockerfile, `wss://` | Two instances on different networks chat through a relay whose log shows only sizes; the relay meets its load targets (§15) |
| **5. Transfers** | Transfers, the inbox, **Send to…** | A session sent from one instance loads on the other; a plugin sends a file from Python |
| **6. A shared stage** | [SHARED_STAGE.md](SHARED_STAGE.md) | See there |
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
