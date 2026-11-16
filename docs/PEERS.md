# Peers (plan)

Running usdcc instances find each other, connect, and exchange messages. Chat comes first, then
sending things to each other (prims, files, sessions), then editing one stage together.

**Status:** planned 2026-11-16. Nothing is built yet. "Checked" below means looked at in this
repository or its pinned vcpkg; everything else is an assumption until the spike (§14).

## 1. Can it work without a server?

Mostly. It depends on where the other instance is:

| The other instance is | Found by | Server |
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

- **Words.** A *peer* is a running usdcc instance. A *room* is a group of connected peers. Not
  "session": that is a saved working state ([SESSIONS.md](SESSIONS.md)).
- **One peer hosts a room, the others join.** A star, not a mesh. The host puts messages in one
  order, which editing together needs (§10.4), and only the host has to accept connections.
- **WebSocket everywhere.** A joiner connects to the host, or both connect to the relay. The frames
  are the same either way, and WebSocket passes proxies and firewalls that raw TCP doesn't.
- **Qt WebSockets, not libcurl.** [REMOTE_ASSETS.md](REMOTE_ASSETS.md) §8 avoids Qt Network because
  resolvers run on any thread and in tools with no event loop. This is the opposite case: app code
  on the event loop, needing a server too, which libcurl doesn't have. `usdcc_assets` stays Qt-free.
- **Off until asked.** usdcc opens no port and sends nothing to the network until the user hosts a
  room or looks for one.
- **Messages go over named channels.** Chat, presence, transfers and stage edits are channels.
  A plugin adds its own from Python or C++ (§7).
- **Always encrypted, end to end** (§6). The relay and the network see sizes and timing only.
- **Editing together goes through a live layer** (§10): one in-memory layer on top of the stage
  that all edits go to and that is kept equal on every peer. Files on disk don't change until
  someone merges it.
- **Nothing received acts by itself.** No script runs, no stage opens and no file leaves the inbox
  without a click.

## 3. The parts

| Part | Where | Holds |
|---|---|---|
| `usdcc_peers` (DLL; Qt Core, Network, WebSockets; no USD, no widgets) | `src/cpp/peers/` | Identity, discovery, hosting and joining, the handshake, channels, transfers |
| Stage sync | `usdcc_usd_ui`, beside `UndoManager` | The live layer, sending and applying edits, the snapshot for a joiner |
| UI | `usdcc_ui`, `usdcc_usd_ui` | The Peers side panel (rooms, people, chat), toasts, presence in the Outliner and viewport, Settings > Peers |
| Python | `usdcc.peers` (pybind11, like `usdcc.usd`) | Rooms, peers, channels |
| `usdcc-relay` | `src/cpp/relay/`, a console program | Forwards frames between the members of a room (§5.4) |

## 4. Identity

Each profile has a display name (the OS user name to start with), a color, and a key pair made on
first use. The private key is kept in the OS credential store, as
[REMOTE_ASSETS.md](REMOTE_ASSETS.md) §8 does for passwords. A peer is known by the fingerprint of
its public key, plus an id per running instance, so two instances of one profile are two peers.

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
key fingerprint, the protocol version and whether joining asks the host. The stage's name is in it
only if a setting says so.

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

### 5.4 The relay

`usdcc-relay` is a console program built from the same tree (Qt Core and WebSockets, the same
framing code). It listens on one port, behind any HTTPS reverse proxy for `wss://`.

- A room on it is a random id. It forwards each frame to the room's other members and keeps nothing.
- It can't read anything (§6). It logs connections and byte counts.
- Limits per room: members, frame size, bytes per second. An optional shared token keeps strangers
  off.
- No accounts and no database. A studio runs one on a machine everyone reaches; Settings > Peers
  holds its URL.

## 6. Connecting and trust

- **Hello.** Both sides send the protocol version, peer id, name, color, usdcc version and OpenUSD
  version. A different protocol version is refused with a message naming both.
- **Encryption.** A key exchange between the joiner and the host (libsodium, *checked:* in vcpkg),
  then every frame is encrypted and authenticated. Over the relay it is the same exchange, so the
  relay only passes ciphertext. The host decrypts and sends on to each member under that member's
  key. Plain `ws://` plus this, rather than TLS with self-signed certificates: it is the same on a
  LAN and through a relay, and nobody has to click through a certificate warning.
- **Who may join.**
  - *By link:* the secret in it is the invitation. A setting makes the host ask anyway.
  - *By discovery:* the host gets a toast ([NOTIFICATIONS.md](NOTIFICATIONS.md)), "Anna wants to
    join Shot 12", with **Allow**, **Always allow Anna** and **Deny**. A room can also have a
    password.
- **Roles.** The host can set a member to view-only, remove a member, and close the room.
- **The room ends when its host leaves,** in the first version (§16).

## 7. Messages and channels

One WebSocket binary message is a JSON header followed by raw bytes for a body that isn't JSON.

| Field | Meaning |
|---|---|
| `channel` | `chat`, `presence`, `transfer`, `stage/<id>`, `plugin/<plugin>/<name>` |
| `type` | The channel's own |
| `from` | The sender's peer id. Set by the host, never taken from the sender |
| `to` | Empty for everyone, or one peer id |
| `seq` | The number the host gives every ordered message |

- **Three kinds of delivery.** *Ordered:* everyone gets every message in the host's order (chat,
  stage edits). *Latest only:* a newer message replaces an older one still waiting, and may be
  dropped (presence, a drag's preview). *Direct:* to one peer (transfers).
- **Large bodies are sent in pieces,** so chat isn't stuck behind a file.
- **A slow peer** has a send queue with a limit. One that falls too far behind is disconnected and
  rejoins from a snapshot.
- **Gone:** a ping every 10 s; 30 s of silence and the peer has left.
- **Reconnecting:** a joiner retries, then asks for what it missed by `seq`. The host keeps the
  recent ordered messages; older than that means a fresh snapshot.

```cpp
namespace usdcc::peers {

class Room : public QObject {
public:
    QList<Peer> peers() const;
    Channel* channel(const QString& name, Delivery delivery = Delivery::Ordered);
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

}  // namespace usdcc::peers
```

```python
import usdcc.peers as peers

room = peers.host("Shot 12 review")            # or peers.join(link), peers.current_room()
pins = room.channel("plugin/notes/pins")
pins.on_message(lambda m: print(m.sender.name, m.type, m.body))
pins.send("add", {"prim": "/World/Hero", "text": "too dark"})
```

A plugin's channel reaches only peers that have the plugin. The others ignore it.

## 8. Chat

The first thing built on it, and the proof that discovery, joining and channels work.

- **The Peers side panel** (a `SidePanel`): rooms found on this machine and this network, **Host
  Room…**, **Join by Link…**, the people in the room with their colors, and the chat.
- **A toast** for a message when the panel is closed (a setting).
- **A prim path in a message is a link** that selects the prim, if that stage is open here.
- **History** lasts as long as the room, in memory. A joiner gets the recent messages. It isn't
  saved, and isn't part of a session.
- **The status bar** shows the room and how many are in it.

## 9. Sending things

**Send to…** on things usdcc can already write out. The receiver gets a toast and decides.

| Thing | Sent as | The receiver can |
|---|---|---|
| The selected prims | USDA text | Paste them into a stage |
| A layer or a stage file | The file, with its dependencies if asked (the ones the Dependencies panel lists) | Open it from the inbox |
| A session | Its JSON, as [SESSIONS.md](SESSIONS.md) §5.1 exports it | Import it. This replaces sending the file by mail; missing paths are located as that section describes |
| A Script Editor tab | Its text | Open it as a new tab. It is never run |
| A camera | Its position and lens | Look through it |
| A stage on a server | Its URL only ([REMOTE_ASSETS.md](REMOTE_ASSETS.md)) | Open it; both resolve it themselves |

Files land in `<AppLocalData>/peers/inbox/<room>/`. Anything over a size limit (100 MB to start
with) asks before it is received.

## 10. Editing a stage together

### 10.1 The live layer

The host picks an open stage and **shares** it in the room. Every member opens the same root
layer, and each gets one more layer on top: the **live layer**, anonymous and in memory, the
strongest sublayer of the session layer. While the stage is shared it is everyone's edit target.
Every edit is an opinion in it, and it is the only thing kept in sync.

Omniverse's live sessions work this way (a `.live` layer in the session layer, merged by the owner
at the end), which is the nearest prior art.

- **No file changes under anyone.** The layers below are read as they are on disk.
- **One layer to sync,** not a layer stack.
- **Leaving is clean.** Dropping the live layer gives back the stage as it was.

The cost is that edits are overrides. The rules are those of editing on any stronger layer:

- **Deleting** a prim that comes from a layer below deactivates it ([PRIM_DELETE.md](PRIM_DELETE.md)).
- **Renaming and reparenting** a prim that comes from below are refused while sharing. They would
  rewrite the layers below, or need `relocates`, which [OUTLINER_REPARENT.md](OUTLINER_REPARENT.md)
  found fragile. Prims created in the live layer move freely.
- **The edit target can't be changed** while sharing. The Layer Editor shows the live layer and why.

### 10.2 Everyone needs the same stage

The layers below the live layer must be equal on every peer.

- **The same path or URL for everyone** (a shared drive, or `webdav://` and `https://` assets,
  whose URLs mean the same on every machine). On joining, the host sends each used layer's
  identifier and a hash of its content. Differences are listed, and the joiner decides.
- **Different folders** on each machine: the root is located once, as an imported session's files
  are ([SESSIONS.md](SESSIONS.md) §5.1).
- **Not there at all:** the host sends the files (phase 7), packed with
  `UsdUtilsLocalizeAsset`. *Not tried.*
- **Unsaved edits** in the stage's file layers: sharing asks to save first. An in-memory stage
  sends its root layer as USDA text, as a session keeps it.

### 10.3 What is sent

The undo recorder already sees every primitive change to a layer, before it is applied.
*Checked:* `EditRecorder` in [undo_manager.cpp](../src/cpp/usd/src/undo_manager.cpp) is the
layer's `SdfLayerStateDelegate` and gets nine calls: set field, set dictionary value, set time
sample, create, delete and move spec, push and pop child. Today it keeps the inverse of each, for
undo. For the live layer it also keeps the call itself.

- **An op** is one of those calls with its arguments.
- **A batch** is what one undo step recorded: one `apply()`, or one edit session. It is sent when
  the step ends, as an ordered message.
- **During a drag** the values so far go out as latest-only messages, at most 20 a second, so the
  others see the prim move. The batch at the end is what counts.
- **It catches everything.** The recorder sits on the layer, so edits from scripts, plugins and
  tools are sent too, whether or not they went through `UndoManager`.

**Values.** An op's value is any `VtValue`, of any of the types the Attribute Editor handles. The
plan is to let USD serialize it: the batch's fields are written to a scratch layer and sent in
that layer's file format. USDA text handles every type but is slow and large for a mesh's points;
crate is compact but is written to a file. Which, and where the size limit between them is, comes
from the spike.

### 10.4 Order and conflicts

The host numbers every batch. That order is the truth.

1. A peer applies its own batch at once and sends it to the host.
2. The host numbers it and sends it to everyone, the sender too, as the acknowledgement.
3. A peer that gets someone else's batch while its own are still unacknowledged applies it, then
   applies again those of its own ops that touch the same spec and field. The host will number
   them later, so they win.

Everyone ends with the same live layer. For one field of one spec, the last writer in the host's
order wins. An op on a spec that no longer exists is dropped.

- **The unit is the field.** Anna moving a prim while Ben changes its color is no conflict. Both
  moving it is: the later one stays. A whole array, a mesh's points, is one field.
- **No locks and no merging of values** in the first version. Presence (§10.7) shows who is
  working on what, which avoids most collisions before they happen.
- **A remote batch is applied in one change block** with recording off: `Stage::changed` fires
  once, every panel refreshes as for a local edit, and nothing lands on the local undo stack.
- **A check.** When the room is quiet the host sends a hash of its live layer. A peer whose hash
  differs logs a warning and takes a fresh snapshot.

### 10.5 Undo

Each peer's undo stack holds its own steps only, as today (one `QUndoStack` per stage).

- **Undo is an edit.** Playing a step's inverses is recorded and sent like any batch.
- **Undoing under someone else's later change** puts the old value back anyway: last writer wins,
  as for any edit (§16).
- **An inverse for a spec someone else deleted** is dropped.

### 10.6 Joining late

The host sends the live layer as it is, with the number of the last batch in it. The joiner
applies it and then every batch after that number. A reconnect too far behind does the same.

### 10.7 Presence

Latest-only messages, a few a second:

- **Selection:** a dot in each peer's color beside the prims they have selected in the Outliner,
  and an outline in that color in the viewport.
- **Camera:** where each peer is looking, drawn as a small frustum, and **Follow Anna**, which
  takes over the view and the current frame until the user moves.
- **Dragging:** which prim a peer has in a gizmo right now.

Each peer keeps its own selection, time, cameras, muted layers, load rules and session layer.
None of them is shared, only shown.

### 10.8 Ending

The host owns the files, so the host decides:

- **Merge** the live layer into the root layer, the layer that was the edit target, or a new
  layer file. Then it is an ordinary unsaved change, and Save writes it.
- **Keep** it as `<stage>.live.usda` beside the stage, to share again later.
- **Discard** it.

A member who leaves can keep a copy of the live layer as a local sublayer.

## 11. Settings > Peers

Name and color; whether rooms on this network are looked for; whether the stage's name is
announced; the port; relay URLs; ask before a join by link; toast on chat; the inbox's size limit.
([SETTINGS.md](SETTINGS.md))

## 12. Security and privacy

- **Nothing leaves the machine until the user hosts or looks.** An announcement holds the room's
  name and the host's display name.
- **No remote code.** A received script is text in a tab. A plugin channel hands data to a plugin
  installed on both sides.
- **A shared stage makes the joiner resolve its asset paths.** The HTTP allow list still asks
  ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §4).
- **A misbehaving peer** meets limits on frame size, rate and batch size, and the host can remove
  it. View-only members' edits are refused by the host, not just hidden in the UI.
- **Secrets** (the private key, a room's password, a link's secret) are never logged. The log
  category is `usdcc.peers` ([LOGGING.md](LOGGING.md)).

## 13. Build and deploy

- **vcpkg:** `qtwebsockets`, `mdns` and `libsodium` (*checked:* all three are ports in the pinned
  vcpkg). Qt Network and its OpenSSL backend are already built: *checked,* `network` and `openssl`
  are default features of the `qtbase` in use. New licences: mdns (Unlicense) and libsodium (ISC).
- **CMake:** `Network` and `WebSockets` join `find_package(Qt6 … Widgets OpenGL Svg Test)`.
- **Worktrees:** new ports need a private vcpkg install root until they are on master
  ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12).
- **Deploy:** `Qt6Network.dll`, `Qt6WebSockets.dll` and `usdcc_peers.dll` in `bin/`, and
  `usdcc-relay` in the package. TLS is only needed for `wss://` to a relay, which also needs Qt's
  TLS plugin and OpenSSL. PySide6 ships a `libcrypto` of the same file name
  ([REMOTE_ASSETS.md](REMOTE_ASSETS.md) §12), so that is checked in phase 4.
- **PySide6** needs no QtWebSockets binding: `usdcc.peers` is pybind11.

## 14. Phases

| Phase | Builds | Done when |
|---|---|---|
| **0. Spike** | Two throwaway programs: a WebSocket host and joiner with mDNS, and a recorder that replays one layer's ops onto another | It answers: what Windows Firewall does for the host and for a peer that only looks; whether `mdns` works beside Windows' own mDNS, with two instances on one machine and across two machines; how to serialize an op's value, timed on a mesh of a million points; whether replaying the recorded ops gives an identical layer |
| **1. `usdcc_peers`** | Identity, hosting and joining by address, the handshake and encryption, channels, `usdcc.peers` | Its tests pass on loopback: ordered messages arrive in one order on three peers, a reconnect catches up, a slow peer is dropped, a wrong secret is refused |
| **2. Discovery and chat** | The files of §5.1, mDNS, the Peers panel, chat, toasts, settings | Two usdcc on two machines find each other with no address typed, and chat |
| **3. Sending things** | Transfers, the inbox, **Send to…** for prims, files, sessions, scripts and cameras | A session sent from one instance loads on the other |
| **4. Links and the relay** | Invite links, `usdcc-relay`, `wss://` | Two instances on different networks chat through a relay whose log shows only sizes |
| **5. A shared stage** | The live layer, ops, order, the snapshot, undo, roles, merge. Same files on both sides only | Two instances move different prims of one stage and each sees both; both move the same prim and end equal; undo on one side undoes only its own step; Merge and Save write the result |
| **6. Presence** | Selections, cameras, Follow | A peer's selection and camera show in the other's Outliner and viewport |
| **7. Sending the stage** | Packing and sending the files to a peer who doesn't have them | A peer with none of the files joins a shared stage |
| **Later** | Moving the room to another host, locks per prim, `usdcc://` registered with the OS | |

Phases 1 to 3 need no server and no decision about one. Phase 5 is the large one, and doesn't
depend on phase 4.

## 15. Testing

- **`usdcc_peers`** (`tests/peers_test.cpp`): several rooms and joiners in one process over
  `127.0.0.1`, with a clock the test moves for pings and timeouts. Discovery sits behind an
  interface with a fake, since multicast isn't dependable in CI ([CI.md](CI.md): no external
  network).
- **Stage sync** (`tests/stage_sync_test.cpp`): two stages in one process, their live layers joined
  by a pipe that delays and reorders. Random edits from both sides must end in identical layers.
  Also the snapshot, undo, and an op for a deleted spec.
- **The relay:** started by the fixture on a free port, like the HTTP test server
  ([HTTP_RESOLVER.md](HTTP_RESOLVER.md) §7).
- **UI** (`usdcc_ui_tests`): the Peers panel against a fake room.
- **By hand:** two instances with two test profiles on one machine, then two machines.

## 16. Open questions

| # | Question | Recommendation |
|---|---|---|
| 1 | The names: peers and rooms? | Yes. "Session" is taken, and "collaboration" names only the last third of this |
| 2 | mDNS through the `mdns` library, the OS's services, or our own broadcast? | The library, if the spike shows it works beside Windows' own mDNS. Otherwise the OS's services on Windows and the library elsewhere |
| 3 | Does the project run a public relay? | No. It ships the program, and a studio or user runs it. A public one costs money and attracts abuse; revisit when people ask |
| 4 | What happens when the host leaves? | The room ends, and each member keeps their copy of the live layer. Moving the room to another member is later |
| 5 | Undo under someone else's later change: put the old value back, or skip? | Put it back, as for any edit. Change it if it surprises people in use |
| 6 | Locks ("Anna is editing this prim")? | Not in the first version. Presence shows it; a soft warning before a hard lock |
| 7 | May a member edit a layer other than the live layer? | No, in the first version. It is what keeps files safe and the sync small |
| 8 | Is chat history saved? | No |
| 9 | Our own encryption (libsodium), or TLS between peers? | libsodium: it is the same directly and through a relay, with no certificates to trust by hand |
| 10 | Is the protocol open to other apps? | It is WebSocket and JSON headers, so a script could join. Nothing is promised until it has settled |

## 17. Not planned

- **WebRTC and hole punching** (`libdatachannel`). It still needs a server to introduce the peers
  and a relay for the routers it fails on, and it is a large dependency. Worth another look only
  if a relay's bandwidth becomes the problem.
- **A mesh with no host.** It needs every peer to accept connections, and gives no single order.
- **CRDTs for USD.** With a host giving the order, last writer wins per field is enough to start.
- **Accounts, a user directory, a cloud service.**
- **Syncing the files on disk.** That is what a shared drive, version control and
  [REMOTE_ASSETS.md](REMOTE_ASSETS.md) are for.
- **Voice and video.**
