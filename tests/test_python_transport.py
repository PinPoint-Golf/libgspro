#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""test_python_transport.py — CT-T01 … CT-T10, docs/conformance.md §3.8.

⚠ THESE CASES ARE ABOUT THE HOST, NOT THE LIBRARY.  The library owns no socket
by construction, so nothing in tests/test_*.c can see reply latency, whether a
200 and a 201 reached the wire as two writes, or what happens when a client
vanishes mid-message.  They need a real listener and a real client, which is
what package 4 delivers: `python/gspro/asyncio_transport.py` on one side and
`tools/gsp_shoot.py` — the launch monitor simulator — on the other.

⚠ WHAT THIS THEREFORE CANNOT SEE.  It exercises ONE host adapter, the asyncio
reference transport, over loopback on one machine.  PinPoint's QTcpServer
adapter and the C reference transport are the same checklist run again against
different code (conformance §3.8), and loopback is the friendliest network
there is: it does not reorder, it rarely drops, and its segmentation is not a
LAN's.  Read a green run as "this host adapter is correct here", never as "the
transport contract holds everywhere".

Usage: test_python_transport.py [<path to libgspro_ffi>]
       GSP_SOAK=1 runs CT-T05's full five minutes instead of the default five
       seconds; the case prints which it did.
"""

from __future__ import annotations

import asyncio
import os
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

if len(sys.argv) > 1:
    os.environ["GSPRO_LIBRARY"] = sys.argv[1]

import gspro  # noqa: E402
import gsp_shoot  # noqa: E402
from gspro.asyncio_transport import AsyncioTransport  # noqa: E402

failures = 0
checks = 0
current_failed = False


def check(condition, what):
    global failures, checks, current_failed
    checks += 1
    if not condition:
        current_failed = True
        failures += 1
        print(f"    FAIL {what}")


def note(what):
    print(f"    note: {what}")


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


class Harness:
    """A listener and the events it produced, for one case."""

    def __init__(self, **server_kwargs):
        self.events: list[gspro.Event] = []
        self.server = gspro.Server(**server_kwargs)
        self.transport = AsyncioTransport(self.server, on_event=self.events.append)

    async def __aenter__(self) -> "Harness":
        await self.transport.start("127.0.0.1", 0)
        self.port = self.transport.port
        return self

    async def __aexit__(self, *exc):
        await self.transport.stop()
        self.server.close()
        self.server.destroy()

    def of_type(self, t) -> list[gspro.Event]:
        return [e for e in self.events if e.type == t]


def shot_bytes(style: str = "asis", fixture: str = "gsp_full.json") -> bytes:
    return gsp_shoot.restyle(gsp_shoot.load_fixture(fixture), style)


# ---------------------------------------------------------------------------
# CT-T01 — a reply, promptly.  [MLM] blocks for 2 s and then RE-SENDS the shot,
# so a late reply is a duplicated shot rather than an error.  500 ms is that
# budget with a wide margin.
# ---------------------------------------------------------------------------
@case
async def CT_T01_one_ack_within_500ms():
    async with Harness() as h:
        def client():
            with gsp_shoot.Shooter("127.0.0.1", h.port) as s:
                t0 = time.monotonic()
                s.send(shot_bytes())
                replies = s.read_responses(1, timeout=2.0)
                return replies, (time.monotonic() - t0) * 1000.0

        replies, ms = await asyncio.to_thread(client)
        check(len(replies) == 1, f"exactly one reply (got {len(replies)})")
        if replies:
            check(replies[0].code == 200, f"code 200 (got {replies[0].code})")
        check(ms < 500.0, f"within 500 ms (took {ms:.1f} ms)")
        note(f"{ms:.2f} ms round trip on loopback")


# ---------------------------------------------------------------------------
# CT-T02 — two shots back to back: two 200s, in order.
# ---------------------------------------------------------------------------
@case
async def CT_T02_two_shots_two_acks_in_order():
    async with Harness() as h:
        def client():
            with gsp_shoot.Shooter("127.0.0.1", h.port) as s:
                payload = shot_bytes()
                s.send(payload)
                s.send(payload)
                return s.read_responses(2, timeout=2.0), len(s.reads)

        replies, reads = await asyncio.to_thread(client)
        check(len(replies) == 2, f"two replies (got {len(replies)})")
        check(all(r.code == 200 for r in replies), "both are 200")
        # ⚠ Reported, not asserted.  design §5.5: nothing can make coalescing
        # impossible, and a case that demanded two segments would be testing
        # the kernel's scheduler rather than the host.
        note(f"{reads} read(s) for 2 replies — separate segments: {reads >= 2}")


# ---------------------------------------------------------------------------
# CT-T03 — ⚠ THE ONE THAT KILLS A CLIENT.  [PIT]'s receive thread DIES when a
# 200 and a 201 share a segment (protocol §9.8).  policy.write_spacing_us holds
# the second write back; without it they are two writes but may still coalesce.
# ---------------------------------------------------------------------------
@case
async def CT_T03_ack_and_player_info_are_spaced():
    spacing_us = 20_000
    async with Harness(write_spacing_us=spacing_us) as h:
        done = asyncio.Event()
        result = {}

        def client():
            with gsp_shoot.Shooter("127.0.0.1", h.port) as s:
                s.send(shot_bytes())
                stamps = []
                first = s.read_responses(1, timeout=2.0)
                stamps.append(time.monotonic())
                second = s.read_responses(1, timeout=2.0)
                stamps.append(time.monotonic())
                result["replies"] = first + second
                result["gap_ms"] = (stamps[1] - stamps[0]) * 1000.0
                result["reads"] = len(s.reads)
            done.set()

        task = asyncio.to_thread(client)
        # The club changes 10 ms after the shot, as a game would.
        async def change_club():
            await asyncio.sleep(0.010)
            await h.transport.set_player(
                gspro.player_info(gspro.Handed.RIGHT, gspro.Club.PT, 4.2)
            )

        await asyncio.gather(task, change_club())
        replies = result.get("replies", [])
        check(len(replies) == 2, f"a 200 and a 201 (got {len(replies)})")
        if len(replies) == 2:
            check(replies[0].code == 200, "the acknowledgement comes first")
            check(replies[1].code == 201, "the player information second")
            # ⚠ The 201 is queued ~10 ms after the 200 is polled, so spacing is
            # what separates them.  Allow a little slack for the event loop.
            gap = result["gap_ms"]
            check(gap >= (spacing_us / 1000.0) * 0.5,
                  f"held at least half the spacing apart (gap {gap:.1f} ms)")
            note(f"gap {gap:.1f} ms with spacing {spacing_us/1000:.0f} ms, "
                 f"{result['reads']} read(s)")


# ---------------------------------------------------------------------------
# CT-T04 — one byte per 10 ms.  CT-F02 end to end: every prefix must be
# PENDING and only the last byte may complete the message.
# ---------------------------------------------------------------------------
@case
async def CT_T04_one_byte_at_a_time():
    async with Harness() as h:
        def client():
            with gsp_shoot.Shooter("127.0.0.1", h.port) as s:
                payload = shot_bytes(fixture="tl_minimal.json")
                # ⚠ 10 ms x 216 bytes is 2 s of wall clock, which is [MLM]'s
                # whole patience.  1 ms keeps the shape and the suite quick;
                # the property under test is per-byte delivery, not the delay.
                s.send_bytewise(payload, gap=0.001)
                early = s.read_responses(1, timeout=0.5)
                return early, len(payload)

        replies, n = await asyncio.to_thread(client)
        check(len(replies) == 1, f"exactly one reply after {n} single-byte writes")
        if replies:
            check(replies[0].code == 200, "and it is a 200")
        shots = h.of_type(gspro.EventType.SHOT)
        check(len(shots) == 1, f"exactly one shot event (got {len(shots)})")


# ---------------------------------------------------------------------------
# CT-T05 — ⚠ SILENCE IS NOT A FAULT.  The protocol has no deadline (§9.6) and
# a client may connect and say nothing for a whole warm-up.  The library must
# never close for it, and neither must the host.
# ---------------------------------------------------------------------------
@case
async def CT_T05_silence_does_not_close():
    soak = os.environ.get("GSP_SOAK") == "1"
    window = 300.0 if soak else 5.0
    async with Harness() as h:
        # ⚠ Every socket call goes through a thread.  A blocking recv() on the
        # event loop thread would stop the listener servicing the very bytes it
        # is waiting for — a deadlock that reads as a protocol timeout, which
        # is exactly how this case failed when it was first written.
        conn = await asyncio.to_thread(
            socket.create_connection, ("127.0.0.1", h.port), 5.0
        )
        try:
            await asyncio.sleep(0.2)
            # The structural reason it is safe, checked as well as observed:
            # with no idle alarm armed the server schedules nothing at all, so
            # there is no timer that could ever fire and close anything.
            check(h.server.next_due_us() == gspro.GSP_TIME_NEVER,
                  "no deadline is armed")
            check(h.server.connection_count() == 1, "the connection is open")

            await asyncio.sleep(window)

            check(h.server.connection_count() == 1,
                  f"still open after {window:.0f} s of silence")
            check(not h.of_type(gspro.EventType.CONNECTION_CLOSED),
                  "and nothing reported it closed")

            def speak():
                conn.sendall(shot_bytes())
                conn.settimeout(2.0)
                return conn.recv(4096)

            reply = await asyncio.to_thread(speak)
            check(gspro.decode_response(reply).code == 200,
                  "a shot after the silence is still answered")
        finally:
            conn.close()
        note(f"observed {window:.0f} s"
             + ("" if soak else " — set GSP_SOAK=1 for the full 5 minutes the row names"))


# ---------------------------------------------------------------------------
# CT-T06 — a second machine on the LAN.  ⚠ NOT AUTOMATED AND NOT PRETENDED:
# see the note in the runner.
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# CT-T07 — two clients at once, each acknowledged independently.  An ordinary
# setup: a putting camera beside a full-swing device (design §5.1).
# ---------------------------------------------------------------------------
@case
async def CT_T07_two_simultaneous_clients():
    async with Harness() as h:
        def client(fixture):
            with gsp_shoot.Shooter("127.0.0.1", h.port) as s:
                s.send(shot_bytes(fixture=fixture))
                return s.read_responses(1, timeout=2.0)

        a, b = await asyncio.gather(
            asyncio.to_thread(client, "gsp_full.json"),
            asyncio.to_thread(client, "tl_minimal.json"),
        )
        check(len(a) == 1 and a[0].code == 200, "the first client is acknowledged")
        check(len(b) == 1 and b[0].code == 200, "the second client is acknowledged")
        shots = h.of_type(gspro.EventType.SHOT)
        check(len(shots) == 2, f"two shots, one per connection (got {len(shots)})")
        check(len({s.conn for s in shots}) == 2, "on two different connection ids")


# ---------------------------------------------------------------------------
# CT-T08 — a client that vanishes mid-message.  The partial object must produce
# no message event, and the close must still be reported with its counters.
# ---------------------------------------------------------------------------
@case
async def CT_T08_disconnect_mid_message():
    async with Harness() as h:
        def client():
            s = socket.create_connection(("127.0.0.1", h.port), timeout=2.0)
            payload = shot_bytes()
            s.sendall(payload[: len(payload) // 2])  # half an object, then gone
            time.sleep(0.05)
            s.close()

        await asyncio.to_thread(client)
        await asyncio.sleep(0.2)
        for kind in (gspro.EventType.SHOT, gspro.EventType.HEARTBEAT,
                     gspro.EventType.STATUS):
            check(not h.of_type(kind), f"no {kind.name} event for half an object")
        closed = h.of_type(gspro.EventType.CONNECTION_CLOSED)
        check(len(closed) == 1, f"the close is reported (got {len(closed)})")
        if closed:
            check(closed[0].connection.messages == 0, "with no messages counted")
        check(h.server.connection_count() == 0, "and the slot is free again")


# ---------------------------------------------------------------------------
# CT-T09 — the port is already held.  ⚠ On the machine this library is most
# likely to run on, port 921 is held by GSPro itself (design §6.4).  The host
# must report it, not appear to be listening.
# ---------------------------------------------------------------------------
@case
async def CT_T09_bind_conflict_is_reported():
    async with Harness() as first:
        server = gspro.Server()
        transport = AsyncioTransport(server)
        raised = None
        try:
            await transport.start("127.0.0.1", first.port)
        except OSError as exc:
            raised = exc
        finally:
            await transport.stop()
            server.close()
            server.destroy()
        check(raised is not None, "binding a held port raises rather than succeeding")
        if raised is not None:
            check(bool(str(raised)), f"with the reason intact: {raised}")
        check(first.server.connection_count() == 0,
              "and the listener that owns the port is unharmed")


# ---------------------------------------------------------------------------
# CT-T10 — ⚠ EVERY WRITE FITS GSP_WRITE_MAX, and each is one write() call.
# The bound is what lets a host put a write request on the stack.
# ---------------------------------------------------------------------------
@case
async def CT_T10_write_size_and_count():
    written: list[int] = []

    class CountingWriter:
        """⚠ Counts write() calls, because "one write per request" cannot be
        seen from the client side: TCP is free to merge two writes into one
        segment and to split one into two."""

        def __init__(self, inner):
            self._inner = inner
            self.sizes: list[int] = []

        def write(self, data):
            self.sizes.append(len(data))
            self._inner.write(data)

        def __getattr__(self, name):
            return getattr(self._inner, name)

    async with Harness() as h:
        connected = asyncio.Event()

        def connect():
            s = socket.create_connection(("127.0.0.1", h.port), timeout=2.0)
            return s

        sock = await asyncio.to_thread(connect)
        try:
            await asyncio.sleep(0.1)
            conn_id = next(iter(h.transport._writers))
            counter = CountingWriter(h.transport._writers[conn_id])
            h.transport._writers[conn_id] = counter
            connected.set()

            # A shot, then a club change: a 200 and a 201, which is the pair
            # [PIT] cannot survive in one segment.
            await asyncio.to_thread(sock.sendall, shot_bytes())
            await asyncio.sleep(0.15)
            await h.transport.set_player(
                gspro.player_info(gspro.Handed.LEFT, gspro.Club.PT, 12.5, "Green")
            )
            await asyncio.sleep(0.15)

            check(len(counter.sizes) == 2,
                  f"two replies became two write() calls (got {len(counter.sizes)})")
            check(all(n <= gspro.GSP_WRITE_MAX for n in counter.sizes),
                  f"each within GSP_WRITE_MAX: {counter.sizes}")
            note(f"write() sizes {counter.sizes}")
        finally:
            sock.close()

    # And every shape the ring can produce, measured directly.
    server = gspro.Server()
    try:
        server.on_connection_opened(1, None, 0)
        server.on_bytes(1, shot_bytes(), 0)
        server.set_player(
            gspro.player_info(gspro.Handed.LEFT, gspro.Club.PT, 123.456, "Fairway"), 0
        )
        server.set_session_state(gspro.SessionState.ACTIVE, 0)
        server.set_session_state(gspro.SessionState.ENDED, 0)
        server.on_bytes(1, b"!!!not json!!!", 0)
        kinds = set()
        for w in server.drain_writes():
            written.append(len(w.data))
            kinds.add(w.kind)
            check(len(w.data) <= gspro.GSP_WRITE_MAX,
                  f"{w.kind.name} is {len(w.data)} <= {gspro.GSP_WRITE_MAX}")
            check(w.data.endswith(b"}"), f"{w.kind.name} ends at its closing brace")
        check(len(kinds) == 5, f"all five write kinds were produced (got {len(kinds)})")
    finally:
        server.close()
        server.destroy()
    note(f"largest write {max(written) if written else 0} of {gspro.GSP_WRITE_MAX} bytes")


# ---------------------------------------------------------------------------
async def run_all() -> int:
    global current_failed
    print(f"libgspro {gspro.VERSION} — host transport conformance "
          f"(asyncio reference adapter)\n")
    for fn in CASES:
        current_failed = False
        name = fn.__name__
        try:
            await fn()
        except Exception as exc:  # noqa: BLE001 — a raising case is a failing case
            current_failed = True
            globals()["failures"] += 1
            print(f"    FAIL {name} raised {type(exc).__name__}: {exc}")
        print(f"[{'FAIL' if current_failed else ' ok '}] {name}")

    print("\n⚠ CT-T06 (a client on a SECOND MACHINE) is not run here and is not")
    print("  pretended: it needs a real LAN and a second host.  It is the one case")
    print("  that can tell a listener bound to 0.0.0.0 from one bound to loopback,")
    print("  which is the difference between a launch monitor working and not")
    print("  (design §6.1).  Run tools/gsp_listen.py --host 0.0.0.0 and aim")
    print("  tools/gsp_shoot.py at it from another machine.")

    print(f"\n{checks - failures}/{checks} check(s) passed, {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(run_all()))
