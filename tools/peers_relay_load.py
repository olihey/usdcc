"""Load on a relay (docs/PEERS.md §15): many rooms of a few peers each, all talking at
once, and how long the relay takes to pass a message on.

    build/peers-venv/bin/python tools/peers_relay_load.py --rooms 200 --workers 8

It starts a relay (`python -m usdcc_peers.relay`) on the loopback in a process of its own,
or loads one with --relay. Each room is a host (a usdcc_peers Host behind its RelayBridge)
and members, all in worker processes; every peer sends `--rate` small messages a second for
`--seconds`. A message carries when it was sent; each member notes how long it took to
arrive (one machine, one clock). The target of §15 to start with: 200 rooms of five peers,
each sending 20 messages a second, 99 of 100 messages forwarded within 50 ms. What is
measured is end to end: the sender's bridge or connection, the relay, the host, back
through the relay, and the receiver.

Run by hand and before a release, not in CI.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import multiprocessing
import os
import subprocess
import sys
import time

PACKAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "usdcc_peers")
sys.path.insert(0, PACKAGE)


def worker(relay: str, rooms: int, peers: int, rate: float, seconds: float, connection) -> None:
    from usdcc_peers import Host, Identity, Member
    from usdcc_peers.relay_bridge import RelayBridge

    async def run() -> None:
        identity = Identity.generate()
        latencies: list[float] = []
        everyone = []
        bridges = []
        for index in range(rooms):
            host = Host(name=f"Room {index}", identity=identity)
            await host.start("127.0.0.1", 0)
            bridge = RelayBridge(host, relay)
            await bridge.start(60)
            bridges.append((bridge, host))
            members = [await Member.join(bridge.link(), name=f"P{i}", identity=identity, instance=f"r{index}p{i}",
                                         timeout=60) for i in range(peers - 1)]
            for member in members:
                member.channel("load").on_message(
                    lambda m: latencies.append(time.time() - m.body["t"]))
            everyone += [host] + members
        connection.send(("ready",))
        connection.recv()
        start = time.monotonic()
        interval = 1.0 / rate
        n = 0
        while time.monotonic() - start < seconds:
            for peer in everyone:
                peer.channel("load").send("ping", {"t": time.time(), "n": n})
            n += 1
            await asyncio.sleep(max(0.0, start + n * interval - time.monotonic()))
        await asyncio.sleep(2)
        connection.send(("latencies", latencies, n * len(everyone)))
        for peer in everyone:
            await peer.close()
        for bridge, _ in bridges:
            await bridge.close()

    asyncio.run(run())


def _cpu_seconds(pid: int) -> float:
    """A process's CPU time so far, from ps (macOS and Linux)."""
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    total = 0.0
    for part in out.replace("-", ":").split(":"):
        total = total * 60 + float(part)
    return total


def raw_worker(relay: str, rooms: int, peers: int, rate: float, seconds: float, connection) -> None:
    """Hosts and members that speak only the relay's frames (PEERS_PROTOCOL.md §11): what is
    measured is the relay alone. Each member's message goes to its host, which sends it back
    to every member of the room as one data-to-several frame, as a room envelope would."""
    import struct

    from websockets.asyncio.client import connect

    async def run() -> None:
        latencies: list[float] = []
        sockets = []

        async def host_loop(host, members: list[int]) -> None:
            async for data in host:
                if isinstance(data, str):
                    continue
                if data[0] == 3:  # opened
                    members.append(struct.unpack(">I", data[1:5])[0])
                elif data[0] == 1 and len(members) == peers - 1:
                    payload = data[5:]
                    await host.send(bytes([2]) + struct.pack(f">H{len(members)}I", len(members), *members) + payload)

        async def member_loop(member) -> None:
            async for data in member:
                latencies.append(time.time() - struct.unpack(">d", data[:8])[0])

        tasks = []
        members_of_room = []
        for _ in range(rooms):
            host = await connect(relay + "/host", compression=None, max_size=None)
            room = json.loads(await host.recv())["room"]
            members: list[int] = []
            tasks.append(asyncio.create_task(host_loop(host, members)))
            room_members = []
            for _ in range(peers - 1):
                member = await connect(f"{relay}/r/{room}", compression=None, max_size=None)
                room_members.append(member)
                tasks.append(asyncio.create_task(member_loop(member)))
            while len(members) < peers - 1:
                await asyncio.sleep(0.01)
            sockets += [host] + room_members
            members_of_room.append(room_members)
        connection.send(("ready",))
        connection.recv()
        start = time.monotonic()
        n = 0
        while time.monotonic() - start < seconds:
            for room_members in members_of_room:
                for member in room_members:
                    await member.send(struct.pack(">d", time.time()) + b"x" * 100)
            n += 1
            await asyncio.sleep(max(0.0, start + n * (1.0 / rate) - time.monotonic()))
        await asyncio.sleep(2)
        connection.send(("latencies", latencies, n * rooms * (peers - 1)))
        for socket in sockets:
            await socket.close()
        for task in tasks:
            task.cancel()

    asyncio.run(run())


def bulk_worker(relay: str, mb_per_second: float, seconds: float, connection) -> None:
    """One more room, whose member sends 1 MB pieces to its host as fast as `mb_per_second`
    lets it (the relay drops a room over its rate, 32 MB/s by default): a transfer."""
    from websockets.asyncio.client import connect

    async def run() -> None:
        host = await connect(relay + "/host", compression=None, max_size=None)
        room = json.loads(await host.recv())["room"]
        member = await connect(f"{relay}/r/{room}", compression=None, max_size=None)
        received = 0

        async def drain() -> None:
            nonlocal received
            async for data in host:
                if not isinstance(data, str) and data[0] == 1:
                    received += len(data) - 5

        task = asyncio.create_task(drain())
        connection.send(("ready",))
        connection.recv()
        piece = b"x" * (1024 * 1024)
        start = time.monotonic()
        sent = 0
        while time.monotonic() - start < seconds:
            await member.send(piece)
            sent += 1
            await asyncio.sleep(max(0.0, start + sent / mb_per_second - time.monotonic()))
        await asyncio.sleep(1)
        connection.send(("bulk", received))
        task.cancel()
        await member.close()
        await host.close()

    asyncio.run(run())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--rooms", type=int, default=200)
    parser.add_argument("--peers", type=int, default=5, help="per room, the host included")
    parser.add_argument("--rate", type=float, default=20, help="messages a second from each peer")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--workers", type=int, default=max(1, min(8, (os.cpu_count() or 2) - 1)))
    parser.add_argument("--relay", help="a relay's URL to load; without it, one of its own")
    parser.add_argument("--bulk", type=float, default=0, metavar="MB_PER_SECOND",
                        help="with --raw: one more room transfers this many MB a second meanwhile")
    parser.add_argument("--raw", action="store_true",
                        help="hosts and members that speak only the relay's frames: the relay alone, measured")
    arguments = parser.parse_args()

    relay_process = None
    relay = arguments.relay
    if not relay:
        environment = dict(os.environ, PYTHONPATH=PACKAGE)
        relay_process = subprocess.Popen([sys.executable, "-m", "usdcc_peers.relay", "--address", "127.0.0.1",
                                          "--port", "0", "--print-port", "--max-rooms", str(arguments.rooms + 10),
                                          "--log-level", "warning"], stdout=subprocess.PIPE, text=True,
                                         env=environment)
        relay = f"ws://127.0.0.1:{json.loads(relay_process.stdout.readline())['port']}"
    pipes = []
    share = -(-arguments.rooms // arguments.workers)
    for index in range(arguments.workers):
        count = min(share, arguments.rooms - index * share)
        if count <= 0:
            break
        ours, theirs = multiprocessing.Pipe()
        multiprocessing.Process(target=raw_worker if arguments.raw else worker, args=(relay, count, arguments.peers, arguments.rate, arguments.seconds,
                                                     theirs), daemon=True).start()
        pipes.append(ours)
    bulk = None
    if arguments.bulk:
        bulk, theirs = multiprocessing.Pipe()
        multiprocessing.Process(target=bulk_worker, args=(relay, arguments.bulk, arguments.seconds, theirs),
                                daemon=True).start()
        bulk.recv()
    started = time.monotonic()
    for pipe in pipes:
        pipe.recv()
    print(f"{arguments.rooms} rooms of {arguments.peers} on the relay after {time.monotonic() - started:.1f} s")
    relay_cpu = _cpu_seconds(relay_process.pid) if relay_process else float("nan")
    wall = time.monotonic()
    for pipe in pipes + ([bulk] if bulk else []):
        pipe.send(("go",))
    latencies, sent = [], 0
    for pipe in pipes:
        _, got, count = pipe.recv()
        latencies += got
        sent += count
    if relay_process:
        busy = (_cpu_seconds(relay_process.pid) - relay_cpu) / (time.monotonic() - wall)
        print(f"The relay was busy {busy * 100:.0f}% of one core while they talked")
    if bulk:
        moved = bulk.recv()[1]
        print(f"Meanwhile one room moved {moved / 1024 / 1024:.0f} MB through the relay")
    latencies.sort()
    expected = sent * (arguments.peers - 1) if arguments.raw else sent * (arguments.peers - 1)
    if latencies:
        def at(share: float) -> float:
            return latencies[min(len(latencies) - 1, int(share * len(latencies)))] * 1000

        print(f"{sent} messages sent, {len(latencies)} arrivals at members (about {expected} expected): "
              f"median {at(0.5):.0f} ms, 99th percentile {at(0.99):.0f} ms, worst {latencies[-1] * 1000:.0f} ms")
    if relay_process:
        relay_process.terminate()
        relay_process.wait(10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
