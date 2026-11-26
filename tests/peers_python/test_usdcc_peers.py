"""usdcc_peers, the Python package of docs/PEERS.md §9, against itself: hosting and
joining, notifications and who may send them, shared state, reconnecting, and the hub
with its HTTP side. The C++ side against it is tests/peers_python_test.cpp.

Run by CTest as usdcc_peers_python_tests, with build/peers-venv's Python:
    build/peers-venv/bin/python -m unittest discover -s tests/peers_python
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "src", "usdcc_peers"))

from usdcc_peers.frame import JoinLink, make_link, parse_link
from usdcc_peers.hub import Hub, HubConfig, RoomConfig
from usdcc_peers.relay import Relay
from usdcc_peers.relay_bridge import RelayBridge

CERTS = os.path.join(os.path.dirname(__file__), "..", "..", "test_data", "peers")

import usdcc_peers
from usdcc_peers import Host, Identity, JoinError, Member, NotificationError, Senders, notification


async def until(condition, timeout: float = 5.0) -> bool:
    end = asyncio.get_running_loop().time() + timeout
    while not condition():
        if asyncio.get_running_loop().time() > end:
            return False
        await asyncio.sleep(0.01)
    return True


class Links(unittest.TestCase):
    def test_round_trip(self):
        identity = Identity.generate()
        self.assertEqual(len(identity.fingerprint), 32)
        text = make_link(JoinLink(address="192.168.1.20:47800", fingerprint=identity.fingerprint, secret="s3cr+t/="))
        link = parse_link(text)
        self.assertEqual((link.address, link.fingerprint, link.secret),
                         ("192.168.1.20:47800", identity.fingerprint, "s3cr+t/="))
        relay = parse_link(make_link(JoinLink(relay="wss://relay.example/r/8f3k", fingerprint="abc", secret="x")))
        self.assertEqual((relay.relay, relay.address), ("wss://relay.example/r/8f3k", ""))
        self.assertIsNone(parse_link("http://example.com"))
        self.assertIsNone(parse_link("usdcc://join/nohost#k=a"))

    def test_key_file_is_the_apps(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "id.key")
            first = Identity.load_or_create(path)
            self.assertEqual(os.path.getsize(path), 64)  # libsodium's secret key, as the app stores it
            if os.name != "nt":
                self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
            self.assertEqual(Identity.load_or_create(path).fingerprint, first.fingerprint)

    def test_notifications_are_checked(self):
        self.assertEqual(notification("Done", kind="success"), {"kind": "success", "title": "Done"})
        with self.assertRaises(NotificationError):
            notification("")
        with self.assertRaises(NotificationError):
            notification("x", kind="loud")
        with self.assertRaises(NotificationError):
            notification("x", actions=[{"label": "Run", "run": "rm -rf /"}])
        with self.assertRaises(NotificationError):
            notification("x", actions=[{"label": "Open", "open": "javascript:alert(1)"}])
        ok = notification("x", actions=[usdcc_peers.action("Open Folder", open="file:///P:/render"),
                                        usdcc_peers.action("Select", select=["/World/hero"])])
        self.assertEqual(ok["actions"][1], {"label": "Select", "select": ["/World/hero"]})


class Rooms(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.host = Host(name="Studio")
        self.host.channel("notify").set_senders(Senders.NAMED, [])
        await self.host.start()
        self.members: list[Member] = []

    async def asyncTearDown(self):
        for member in self.members:
            await member.close()
        await self.host.close()

    async def join(self, name: str, **options) -> Member:
        member = await Member.join(self.host.link(), name=name, **options)
        self.members.append(member)
        return member

    async def test_chat_notify_and_state(self):
        anna = await self.join("Anna")
        ben = await self.join("Ben")
        said, notes, toasts, refused = [], [], [], []
        ben.channel("chat").on_message(lambda m: said.append((m.sender, m.body["text"])))
        ben.state("notes").on_change(lambda key, value, by: notes.append((key, value, by.id)))
        ben.channel("notify").on_message(lambda m: toasts.append(m.body))
        anna.channel("notify").on_refused(lambda type_, reason: refused.append(reason))

        anna.channel("chat").send("say", {"text": "hi"})
        anna.state("notes").set("/World/hero", {"text": "too shiny"})
        self.host.notify("Render finished", "shot_012", kind="success", to="ben", id="farm/1")
        anna.notify("Not allowed", "anna isn't a service")
        self.assertTrue(await until(lambda: said and notes and toasts and refused))
        self.assertEqual(said, [(anna.me.id, "hi")])
        self.assertEqual(notes, [("/World/hero", {"text": "too shiny"}, anna.me.id)])
        self.assertEqual(toasts, [{"kind": "success", "title": "Render finished", "body": "shot_012", "id": "farm/1"}])
        self.assertEqual(refused, ["Not allowed to send on this channel"])
        self.assertEqual([p.name for p in anna.peers()], ["Studio", "Anna", "Ben"])

        late = await self.join("Late")
        self.assertEqual(late.state("notes").items(), {"/World/hero": {"text": "too shiny"}})

    async def test_named_service_may_notify(self):
        service = Identity.generate()
        self.host.channel("notify").set_senders(Senders.NAMED, [service.fingerprint])
        farm = await self.join("Farm", identity=service)
        anna = await self.join("Anna")
        toasts = []
        anna.channel("notify").on_message(lambda m: toasts.append((m.sender, m.body["title"])))
        self.assertEqual(farm.notify("Frames done", to="anna"), 1)
        self.assertTrue(await until(lambda: toasts))
        self.assertEqual(toasts, [(farm.me.id, "Frames done")])

    async def test_who_gets_in(self):
        with self.assertRaisesRegex(JoinError, "Wrong secret"):
            await Member.join(self.host.link().replace("&s=", "&s=x"), name="Mallory")
        other = Host(name="Elsewhere")
        await other.start()
        try:
            # The right address, the wrong key: a link names the host's fingerprint.
            wrong = self.host.link().replace(f":{self.host.port}", f":{other.port}")
            with self.assertRaisesRegex(JoinError, "Wrong key"):
                await Member.join(wrong, name="Mallory")
        finally:
            await other.close()
        with self.assertRaisesRegex(JoinError, "invited"):
            await Member.join(f"127.0.0.1:{self.host.port}", name="Finder")

    async def test_reconnect_catches_up(self):
        anna = await self.join("Anna", reconnect_for=10)
        ben = await self.join("Ben")
        said, resumed = [], []
        anna.channel("chat").on_message(lambda m: said.append(m.body["text"]))
        anna.on("resumed", resumed.append)
        await anna._websocket.close()  # As a network that drops would, without a leave
        ben.channel("chat").send("say", {"text": "while you were away"})
        self.assertTrue(await until(lambda: resumed and said, 8))
        self.assertEqual((resumed, said), ([True], ["while you were away"]))
        self.assertEqual(len(self.host.peers()), 3)

    async def test_host_closes(self):
        anna = await self.join("Anna")
        await self.host.close("Maintenance")
        self.assertEqual(await asyncio.wait_for(anna.wait_closed(), 3), "Maintenance")


class HubTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.config = HubConfig(rooms=[RoomConfig(name="Studio"), RoomConfig(name="Show B")], address="127.0.0.1",
                                http_port=0, http_address="127.0.0.1", token="t0ken", data=self.folder.name)
        self.hub = Hub(self.config)
        await self.hub.start()

    async def asyncTearDown(self):
        await self.hub.close()
        self.folder.cleanup()

    async def post(self, path: str, body: dict, token: str = "t0ken"):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.hub.http_port)
        data = json.dumps(body).encode()
        writer.write(f"POST {path} HTTP/1.1\r\nHost: hub\r\nAuthorization: Bearer {token}\r\n"
                     f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
        answer = await reader.read()
        writer.close()
        head, _, payload = answer.partition(b"\r\n\r\n")
        return int(head.split()[1]), json.loads(payload)

    async def test_webhook_and_state(self):
        links = self.hub.links()
        self.assertEqual([entry["room"] for entry in links], ["Studio", "Show B"])
        anna = await Member.join(links[0]["link"], name="Anna")
        try:
            toasts = []
            anna.channel("notify").on_message(lambda m: toasts.append(m.body))
            self.assertEqual(await self.post("/notify", {"title": "Hi"}, token="wrong"),
                             (401, {"error": "The token is missing or wrong"}))
            self.assertEqual((await self.post("/notify", {"title": ""}))[0], 400)
            self.assertEqual((await self.post("/notify", {"title": "x", "to": "nobody"}))[0], 404)
            self.assertEqual(await self.post("/notify", {"title": "Published", "kind": "info", "to": "Anna"}),
                             (200, {"sent": 1}))
            status, _ = await self.post("/state", {"map": "farm.jobs", "key": "8812", "value": {"progress": 1.0}})
            self.assertEqual(status, 200)
            self.assertTrue(await until(lambda: toasts and "8812" in anna.state("farm.jobs")))
            self.assertEqual(toasts, [{"kind": "info", "title": "Published"}])
            self.assertEqual(anna.state("farm.jobs").get("8812"), {"progress": 1.0})
            # Members read `notify`; only the hub and named services write it.
            refused = []
            anna.channel("notify").on_refused(lambda type_, reason: refused.append(reason))
            anna.notify("Spam")
            self.assertTrue(await until(lambda: refused))
        finally:
            await anna.close()

    async def test_links_stay_the_same(self):
        before = self.hub.links()
        await self.hub.close()
        self.hub = Hub(self.config)
        await self.hub.start()
        after = self.hub.links()
        self.assertEqual([entry["link"].split("#")[1] for entry in before],
                         [entry["link"].split("#")[1] for entry in after])
        with open(os.path.join(self.folder.name, "links.json"), encoding="utf-8") as file:
            self.assertEqual(json.load(file), after)


class RelayTests(unittest.IsolatedAsyncioTestCase):
    """A host and its members on a relay (PEERS.md §5.4): the relay forwards what it can't read."""

    async def asyncSetUp(self):
        self.relay = Relay(token="relay-token")
        await self.relay.start("127.0.0.1", 0)
        self.host = Host(name="Far Away")
        await self.host.start("127.0.0.1", 0)
        self.bridge = RelayBridge(self.host, f"ws://127.0.0.1:{self.relay.port}", token="relay-token")
        await self.bridge.start()
        self.members = []

    async def asyncTearDown(self):
        for member in self.members:
            await member.close()
        await self.bridge.close()
        await self.host.close()
        await self.relay.close()

    async def test_chat_through_the_relay(self):
        link = self.bridge.link()
        self.assertTrue(link.startswith(f"usdcc://join?relay=ws://127.0.0.1:{self.relay.port}/r/"), link)
        anna = await Member.join(link, name="Anna")
        ben = await Member.join(link, name="Ben")
        cara = await Member.join(link, name="Cara")
        self.members += [anna, ben, cara]
        said = []
        ben.channel("chat").on_message(lambda m: said.append(m.body["text"]))
        cara.channel("chat").on_message(lambda m: said.append(m.body["text"]))
        anna.channel("chat").send("say", {"text": "secret plans"})
        self.assertTrue(await until(lambda: len(said) == 2))
        self.assertEqual(said, ["secret plans", "secret plans"])
        self.assertEqual(len(self.host.peers()), 4)
        # Sealed once for the room, sent to the relay once for Anna, Ben and Cara.
        self.assertGreater(self.bridge.copies_saved, 0)
        room = self.relay.rooms[self.bridge.room_id]
        self.assertGreater(room.bytes_in, 0)

    async def test_wrong_token_and_no_room(self):
        other = RelayBridge(self.host, f"ws://127.0.0.1:{self.relay.port}", token="wrong")
        with self.assertRaises(asyncio.TimeoutError):
            await other.start(timeout=1.5)
        await other.close()
        with self.assertRaisesRegex(JoinError, "Couldn't reach"):
            await Member.join(f"usdcc://join?relay=ws://127.0.0.1:{self.relay.port}/r/nosuchroom#k=a&s=b",
                              name="Lost", timeout=5)

    async def test_back_after_the_relay_restarts(self):
        anna = await Member.join(self.bridge.link(), name="Anna", reconnect_for=30)
        ben = await Member.join(self.bridge.link(), name="Ben")
        self.members += [anna, ben]
        resumed, said = [], []
        anna.on("resumed", resumed.append)
        anna.channel("chat").on_message(lambda m: said.append(m.body["text"]))
        port, room_id = self.relay.port, self.bridge.room_id
        await self.relay.close()
        self.relay = Relay(token="relay-token")
        await self.relay.start("127.0.0.1", port)
        # The bridge gets the same room id back, so the link still works.
        self.assertTrue(await until(lambda: room_id in self.relay.rooms, 15))
        self.assertTrue(await until(lambda: resumed, 20))
        ben.channel("chat").send("say", {"text": "you're back"})
        self.assertTrue(await until(lambda: said == ["you're back"], 10))

    async def test_hub_through_the_relay(self):
        with tempfile.TemporaryDirectory() as folder:
            config = HubConfig(rooms=[RoomConfig(name="Studio")], address="127.0.0.1", data=folder,
                               relay=f"ws://127.0.0.1:{self.relay.port}", relay_token="relay-token")
            hub = Hub(config)
            await hub.start()
            try:
                links = hub.links()
                self.assertIn("relayLink", links[0])
                anna = await Member.join(links[0]["relayLink"], name="Anna")
                self.members.append(anna)
                self.assertEqual(anna.name, "Studio")
                room_id = hub.bridges["Studio"].room_id
            finally:
                await hub.close()
            # Started again, it asks for the same id on the relay, and gets it: the link holds.
            hub = Hub(config)
            await hub.start()
            try:
                self.assertEqual(hub.bridges["Studio"].room_id, room_id)
            finally:
                await hub.close()

    async def test_wss(self):
        import ssl

        await self.relay.close()
        server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server.load_cert_chain(os.path.join(CERTS, "relay.pem"), os.path.join(CERTS, "relay.key"))
        client = ssl.create_default_context(cafile=os.path.join(CERTS, "relay_ca.pem"))
        self.relay = Relay()
        await self.relay.start("127.0.0.1", 0, server)
        bridge = RelayBridge(self.host, f"wss://127.0.0.1:{self.relay.port}", ssl=client)
        await bridge.start()
        try:
            anna = await Member.join(bridge.link(), name="Anna", ssl=client)
            self.members.append(anna)
            self.assertEqual([p.name for p in anna.peers()], ["Far Away", "Anna"])
            with self.assertRaises(JoinError):  # Without the CA, the certificate isn't trusted
                await Member.join(bridge.link(), name="Untrusting", timeout=5)
        finally:
            await bridge.close()


if __name__ == "__main__":
    unittest.main()
