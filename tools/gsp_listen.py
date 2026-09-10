#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""gsp_listen.py — a complete GSPro Connect listener, in about eighty lines.

Binds, prints every event, and can tell the connected launch monitors which
club the player has and whether a round is running.  This is what to run with a
real device on the mat before touching an application: it answers "does this
connector talk to us, and what exactly does it send" without anything else in
the way.

    tools/gsp_listen.py                        # 127.0.0.1:921
    tools/gsp_listen.py --host 0.0.0.0 --port 9210
    tools/gsp_listen.py --club PT --distance 4.2 --handed RH
    tools/gsp_listen.py --identifiers --idle-alarm 30

⚠ IDENTIFIERS ARE REDACTED BY DEFAULT.  A peer address identifies a household
and a DeviceID can carry a hardware serial ([GC2]).  ``--identifiers`` prints
them; design §9.2 is why it is not the default, and why a capture taken to
answer protocol §11's questions must be redacted before it becomes a fixture.

⚠ It is meant to be COPIED.  The pump below is the whole host contract, and a
Qt or Winsock host writes the same shape against the same Server.
"""

from __future__ import annotations

import argparse
import asyncio
import signal
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import gspro  # noqa: E402
from gspro.asyncio_transport import AsyncioTransport  # noqa: E402
from gspro.wire import Recording, Writer  # noqa: E402


def build_player(args) -> gspro.PlayerInfo | None:
    """⚠ UNKNOWN members are omitted from the JSON rather than invented, and a
    player with nothing known sends nothing at all (design §5.6)."""
    handed = gspro.handed_parse(args.handed) if args.handed else gspro.Handed.UNKNOWN
    club = gspro.club_parse(args.club) if args.club else gspro.Club.UNKNOWN
    if args.club and club == gspro.Club.UNKNOWN:
        raise SystemExit(
            f"⚠ {args.club!r} is not a club code.  The vocabulary is "
            "DR, W2-W7, H2-H7, I1-I9, PW, GW, SW, LW, PT (protocol §5.2)."
        )
    if handed == gspro.Handed.UNKNOWN and club == gspro.Club.UNKNOWN \
            and args.distance is None and not args.surface:
        return None
    return gspro.player_info(handed, club, args.distance, args.surface or "")


async def run(args) -> int:
    server = gspro.Server(
        max_connections=args.max_connections,
        idle_alarm_us=int(args.idle_alarm * 1_000_000),
        write_spacing_us=int(args.write_spacing * 1_000_000),
        reject_incomplete_shots=args.reject_incomplete,
        ack_text=args.ack_text or "",
        # ⚠ THE WIRE LOG IS OFF UNLESS A CAPTURE IS ASKED FOR (design §7).
        wire_ring=gspro.GSP_WIRE_RING_RECOMMENDED if args.record else 0,
        record_identifiers=bool(args.record and args.identifiers),
    )

    writer = None
    if args.record:
        writer = Writer(args.record, Recording(
            port=args.port, identifiers_recorded=bool(args.identifiers),
            note=args.note or ""))

    shots = 0

    def capture() -> None:
        """⚠ EVERY TIME ROUND, not at the end: the ring is drop-OLDEST, so a
        host that drains it lazily loses the start of the session it is
        recording — which is where the handshake and the first shot are."""
        if writer is not None:
            writer.write(server.drain_wire())

    def on_event(ev: gspro.Event) -> None:
        nonlocal shots
        if ev.type == gspro.EventType.SHOT:
            shots += 1
        capture()
        line = ev.format(args.identifiers)
        mark = "⚠ " if ev.type in (
            gspro.EventType.PROTOCOL_ERROR,
            gspro.EventType.CLOSE_REQUESTED,
            gspro.EventType.WARNING,
        ) else "  "
        print(f"{mark}{line}", flush=True)

        # ⚠ The flags are the point of a first session with real hardware: they
        # say which of protocol §11's unknowns this device just answered.
        if ev.type in (gspro.EventType.SHOT, gspro.EventType.HEARTBEAT,
                       gspro.EventType.STATUS):
            names = gspro.flag_names(ev.message.flags)
            if names:
                print(f"    findings: {' '.join(names)}", flush=True)

    transport = AsyncioTransport(server, on_event=on_event)
    try:
        await transport.start(args.host, args.port)
    except OSError as exc:
        # ⚠ Port 921 is very often already held — by GSPro itself, on the
        # machine this is most likely to run on (design §6.4).  Say so.
        extra = ""
        if args.port == gspro.GSP_DEFAULT_PORT:
            extra = (f"\n   GSPro Connect itself listens here.  It can be moved to "
                     f"{gspro.GSP_ALT_PORT} with\n"
                     "   <OpenAPIUseAltPort>true</OpenAPIUseAltPort> in "
                     "GSPconnect.exe.config ([SLX]).")
        print(f"⛔ cannot bind {args.host}:{args.port}: {exc}{extra}", file=sys.stderr)
        return 1

    print(f"listening on {args.host}:{transport.port}"
          f"   (identifiers {'SHOWN' if args.identifiers else 'redacted'})", flush=True)
    if writer is not None:
        # ⚠ A capture taken with --identifiers holds a peer address and any
        # DeviceID verbatim; design §9.2 is why that has to be a decision.
        print(f"recording to {args.record}   (identifiers "
              + ("RECORDED — ⚠ redact before sharing" if args.identifiers else "redacted")
              + ")", flush=True)

    player = build_player(args)
    if player is not None:
        await transport.set_player(player)
        print(f"player: handed={gspro.handed_text(gspro.Handed(player.handed)) or '-'} "
              f"club={gspro.club_code(gspro.Club(player.club)) or '-'}"
              + (f" distance={player.distance_to_target}" if player.has_distance else ""),
              flush=True)
    if args.session_active:
        await transport.set_session_state(gspro.SessionState.ACTIVE)
        print("session: ACTIVE (202 sent — [OSP]-style clients arm on this)", flush=True)

    # ⚠ A SIGNAL MUST REACH THE `finally` BELOW, or a --record session dies with
    # its capture still in a buffer.  ctrl-c raises KeyboardInterrupt through
    # asyncio.run(), but SIGTERM — what a supervisor, a script or `pkill` sends —
    # terminates CPython outright and unwinds nothing.
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, AttributeError):   # Windows has neither
            pass
    try:
        await stop.wait()
    except asyncio.CancelledError:
        pass
    finally:
        print(f"\n{shots} shot(s) received", flush=True)
        await transport.stop()
        server.close()
        for ev in server.drain_events():  # ⚠ drain once more: design §3.3
            print(f"  {ev.format(args.identifiers)}", flush=True)
        capture()                          # ⚠ and the wire ring once more too
        if writer is not None:
            writer.close()
            print(f"capture: {writer.chunks} chunk(s) to {args.record}", flush=True)
            if server.dropped_wire():
                # A capture with holes is still evidence, but not of ABSENCE.
                print(f"⚠ {server.dropped_wire()} chunk(s) were dropped before they "
                      "could be written; the capture has holes", flush=True)
        server.destroy()
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=gspro.GSP_DEFAULT_PORT and "127.0.0.1",
                    help="⚠ 0.0.0.0 to accept a device on another machine")
    ap.add_argument("--port", type=int, default=gspro.GSP_DEFAULT_PORT)
    ap.add_argument("--identifiers", action="store_true",
                    help="⚠ print peer addresses and DeviceIDs unredacted")
    ap.add_argument("--club", help="DR, W2-W7, H2-H7, I1-I9, PW, GW, SW, LW, PT")
    ap.add_argument("--handed", help="RH or LH")
    ap.add_argument("--distance", type=float,
                    help="⚠ [OSP] arms its device on a NON-ZERO value")
    ap.add_argument("--surface", help='e.g. "Green" — [OSP] only; vocabulary unknown')
    ap.add_argument("--session-active", action="store_true",
                    help="send a 202 — [OSP]-style clients do not arm without one")
    ap.add_argument("--idle-alarm", type=float, default=0.0, metavar="SECONDS",
                    help="report a client that has gone quiet.  ⚠ Never closes it")
    ap.add_argument("--write-spacing", type=float, default=0.0, metavar="SECONDS",
                    help="hold writes apart — ⚠ [PIT]'s thread dies on {200}{201}")
    ap.add_argument("--reject-incomplete", action="store_true",
                    help="⚠ answer an incomplete shot 501; a client may retry forever")
    ap.add_argument("--ack-text", help="⚠ the real GSPro misspells it (protocol U4)")
    ap.add_argument("--max-connections", type=int, default=0, help="0 → 4")
    ap.add_argument("--record", metavar="FILE",
                    help="⚠ capture the BYTES to a .gswire file — what package 7 is for")
    ap.add_argument("--note", help="a line in the capture's header: which device, and "
                                   "which question it was taken to answer")
    args = ap.parse_args(argv)

    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
