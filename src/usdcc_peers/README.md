# usdcc_peers

The Peers protocol of [usdcc](../../docs/PEERS.md) for programs that aren't usdcc: a render
farm, an asset tracker, a CI job or a studio's own tool joins a room the way a usdcc instance
does, and sends it notifications, messages and shared state. It also has the **hub**, which
gives a studio rooms that are always there.

It needs Python 3.10 or later, `websockets` and `PyNaCl`, and neither usdcc nor Qt. The wire
protocol is [docs/PEERS_PROTOCOL.md](../../docs/PEERS_PROTOCOL.md); the app's C++ library and
this package are tested against each other.

```
pip install ./src/usdcc_peers
```

## From a script

```python
import usdcc_peers

room = usdcc_peers.join("usdcc://join/10.0.0.5:47800#k=…&s=…", name="Render Farm", key_file="farm.key")
room.notify("Render finished", "shot_012 · 240 frames · 1 h 12 m", kind="success", to="anna")
room.state("farm.jobs").set("8812", {"shot": "shot_012", "progress": 1.0})
room.channel("farm.events").on_message(lambda m: print(m.sender, m.type, m.body))
room.close()
```

- **The key** in `key_file` is who the program is to every room it joins, made there on first
  use and readable by its owner only. A hub names the services that may send notifications by
  this key's fingerprint: `usdcc_peers.Identity.load_or_create("farm.key").fingerprint`.
- **Callbacks** run on the rooms' own thread, one for all the rooms of a program.
- **`to`** names one person, by their id or their name (any case). Without it, everyone.
- **A notification** has a `kind` (info, success, warning, error), a title, a body, an `id`
  (the same id again replaces the toast that is up, for progress), `sticky`, and at most
  three actions from a fixed list, never one that runs anything:
  `usdcc_peers.action("Open Folder", open="file:///P:/shots/012/render")`, `open_stage=`,
  `select=["/World/hero"]`, `copy=`.

From asyncio code, use `usdcc_peers.Member.join()` and `usdcc_peers.Host` directly: the same
calls, without the thread.

## The hub

```
python -m usdcc_peers.hub --name Studio --port 47800 --http 8080 --token-file hub.token \
    --notify-sender <farm's fingerprint>
```

Installing the package also gives it as the command `usdcc-hub`.

- **It hosts a room that never closes.** Its link is in `links.json` in the data folder
  (`--data`, by default `~/.local/share/usdcc_peers/hub`), readable by its owner only. Artists
  add it under Settings > Peers > Rooms joined at start, or join it once and pick **Join When
  usdcc Starts** from its menu in the Peers panel.
- **The links stay the same** from one run to the next: the hub's key and each room's secret
  are kept in the data folder.
- **Members read `notify`; only the hub and the services named with `--notify-sender` write
  it.** The hub enforces it: another member's notification is refused.
- **It keeps each room's shared state while it runs**, so a map like `farm.jobs` is there for
  whoever joins.
- **Several rooms**, one per show or department, each with its own port and link, come from a
  configuration file:

  ```json
  {
    "address": "0.0.0.0",
    "linkAddress": "hub.studio.example",
    "http": 8080,
    "rooms": [
      {"name": "Show A", "port": 47800, "notifySenders": ["<farm's fingerprint>"]},
      {"name": "Show B", "port": 47801}
    ]
  }
  ```

  ```
  python -m usdcc_peers.hub --config hub.json --token-file hub.token
  ```

### Its HTTP side

For services that can only call a webhook. Off without `--token-file`. Every request needs
`Authorization: Bearer <token>`. Put it behind HTTPS when it isn't on a closed network.

```
curl -H "Authorization: Bearer $(cat hub.token)" \
     -d '{"title": "Render finished", "kind": "success", "to": "anna"}' http://hub:8080/notify
```

| Request | Body | Does |
|---|---|---|
| `POST /notify` | A notification, with `to` (a name or an id) and `room` (a room's name), both optional | A toast on every member of the room (the first, without `room`), or on the one named |
| `POST /state` | `{room?, map, key, value}`; without `value` the key is removed | Sets a key of a shared map, as the hub |
| `GET /rooms` | | The rooms and who is in each |

## The relay

For people on different networks, behind routers, who can't reach each other (PEERS.md §5.4):
both sides connect out to the relay, which passes on what they say without being able to read
it. It needs only `websockets`.

```
python -m usdcc_peers.relay --port 8443 --token-file relay.token
```

Installing the package also gives it as `usdcc-relay`. Put it behind an HTTPS reverse proxy for
`wss://`, or give it `--certfile` and `--keyfile`. The certificate file holds the full chain, the
relay's certificate then its CA's: Windows doesn't trust a relay that sends its certificate alone. The token is what a host needs to open a room
on it; members need only the room's link. It logs connections and byte counts, never what it
forwards, and limits members per room (`--max-members`), one message (`--max-frame-mb`) and a
room's traffic (`--max-mb-per-second`).

- **In usdcc:** Settings > Peers > Relay (`wss://relay.example`) and Relay token, then Host Room
  > **Through the relay**. The invite link then names the relay:
  `usdcc://join?relay=wss://relay.example/r/<room>#k=…&s=…`.
- **A hub behind a router:** `python -m usdcc_peers.hub … --relay wss://relay.example
  --relay-token-file relay.token`; `links.json` then has a `relayLink` for each room, which stays
  the same from one run to the next.
- **From Python:** `usdcc_peers.relay_bridge.RelayBridge(host, "wss://relay.example", token=…)`
  puts a `Host` on a relay; `bridge.link()` is its link.

### In a container

`Dockerfile` here builds an image with the package, which runs the relay by default:

```
docker build -t usdcc-peers src/usdcc_peers
docker run -d -p 8443:8443 -v /srv/relay:/data usdcc-peers usdcc-relay --port 8443 --token-file /data/relay.token
docker run -d -p 47800:47800 -p 8080:8080 -v /srv/hub:/data usdcc-peers \
    usdcc-hub --port 47800 --http 8080 --token-file /data/hub.token --data /data --link-address hub.studio.example
```

## Load

`tools/peers_load.py` puts many members on a hub and measures how long a message to everyone
takes to reach them all; `tools/peers_relay_load.py` puts many rooms on a relay and measures how
long it takes to pass a message on (PEERS.md §15, §14.5, §14.6).
