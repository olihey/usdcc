"""Load on a hub (docs/PEERS.md §15): many Python members, and how long a message to
everyone takes to reach them all.

    build/peers-venv/bin/python tools/peers_load.py --members 2000 --workers 8
    build/peers-venv/bin/python tools/peers_load.py --members 500 --link "usdcc://join/…"

Without --link it starts a hub (`python -m usdcc_peers.hub`) on the loopback, in a
process of its own. The members are spread over `--workers` processes, so that what is
measured is the hub, not one Python process opening every member's messages. One member
sends a message to everyone, ordered, through the hub; each worker notes when each of its
members had it (one clock: one machine). The targets of §15 to start with: everyone
within 100 ms with 2,000 members, and a hub under 500 MB with 2,000 idle connections.

Run by hand and before a release, not in CI. On macOS and Linux the open-file limit
(ulimit -n) must allow the connections.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import multiprocessing
import os
import statistics
import subprocess
import sys
import tempfile
import time

PACKAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "usdcc_peers")
sys.path.insert(0, PACKAGE)


def worker(link: str, first: int, count: int, total: int, connection) -> None:
    from usdcc_peers import Identity, Member

    async def run() -> None:
        identity = Identity.generate()
        members = []
        for start in range(first, first + count, 25):
            members += await asyncio.gather(*(Member.join(link, name=f"Load {i}", identity=identity,
                                                          instance=f"load{i}", timeout=120)
                                              for i in range(start, min(start + 25, first + count))))
        arrivals: dict[int, list[float]] = {}
        for member in members:
            member.channel("load").on_message(
                lambda message: arrivals.setdefault(message.body["n"], []).append(time.time()))
        connection.send(("joined", count))
        # Settled: every member has seen everyone join, so nothing is waiting to be read.
        while not all(len(member.peers()) >= total + 1 for member in members):
            await asyncio.sleep(0.1)
        connection.send(("settled",))
        while True:
            command = await asyncio.get_running_loop().run_in_executor(None, connection.recv)
            if command[0] == "send":
                members[0].channel("load").send("ping", {"n": command[1], "pad": "x" * command[2]})
            elif command[0] == "report":
                connection.send(("arrivals", {n: (len(times), max(times)) for n, times in arrivals.items()}))
            elif command[0] == "stop":
                for member in members:
                    await member.close()
                connection.send(("stopped",))
                return

    asyncio.run(run())


def hub_memory_mb(pid: int) -> float:
    try:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True, check=True)
        return int(out.stdout.strip()) / 1024
    except (OSError, ValueError, subprocess.CalledProcessError):
        return float("nan")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--members", type=int, default=200)
    parser.add_argument("--workers", type=int, default=max(1, min(8, (os.cpu_count() or 2) - 1)))
    parser.add_argument("--link", help="a hub's room to load; without it, a hub of its own")
    parser.add_argument("--rounds", type=int, default=10)
    parser.add_argument("--size", type=int, default=100, help="each message's padding in bytes")
    arguments = parser.parse_args()

    hub = None
    link = arguments.link
    folder = tempfile.TemporaryDirectory()
    if not link:
        environment = dict(os.environ, PYTHONPATH=PACKAGE)
        hub = subprocess.Popen([sys.executable, "-m", "usdcc_peers.hub", "--name", "Load", "--port", "0",
                                "--address", "127.0.0.1", "--data", folder.name, "--max-members",
                                str(arguments.members + 10), "--print-links", "--log-level", "warning"],
                               stdout=subprocess.PIPE, text=True, env=environment)
        link = json.loads(hub.stdout.readline())["rooms"][0]["link"]
    pipes = []
    started = time.monotonic()
    share = -(-arguments.members // arguments.workers)
    for index in range(arguments.workers):
        first = index * share
        count = min(share, arguments.members - first)
        if count <= 0:
            break
        ours, theirs = multiprocessing.Pipe()
        process = multiprocessing.Process(target=worker, args=(link, first, count, arguments.members, theirs),
                                          daemon=True)
        process.start()
        pipes.append(ours)
    for pipe in pipes:
        pipe.recv()
    joined = time.monotonic() - started
    print(f"{arguments.members} members in {len(pipes)} processes joined in {joined:.1f} s")
    for pipe in pipes:
        pipe.recv()
    print(f"Everyone saw everyone after {time.monotonic() - started:.1f} s")
    time.sleep(1)
    if hub:
        print(f"The hub with everyone idle: {hub_memory_mb(hub.pid):.0f} MB")

    times = []
    for n in range(arguments.rounds):
        sent = time.time()
        pipes[0].send(("send", n, arguments.size))
        time.sleep(max(0.5, arguments.members / 2000))
        latest, got = 0.0, 0
        for pipe in pipes:
            pipe.send(("report",))
        for pipe in pipes:
            arrivals = pipe.recv()[1].get(n, (0, 0.0))
            got += arrivals[0]
            latest = max(latest, arrivals[1])
        times.append((latest - sent) * 1000 if got == arguments.members else float("inf"))
    print(f"A message to everyone reached all {arguments.members}: median {statistics.median(times):.0f} ms, "
          f"worst {max(times):.0f} ms over {arguments.rounds} rounds")
    for pipe in pipes:
        pipe.send(("stop",))
    for pipe in pipes:
        pipe.recv()
    if hub:
        hub.terminate()
        hub.wait(10)
    folder.cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
