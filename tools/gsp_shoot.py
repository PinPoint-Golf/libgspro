#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""gsp_shoot.py — a launch monitor simulator, for aiming at a libgspro listener.

Connects to a listener and sends shots taken from the conformance fixtures,
optionally in the byte patterns particular connectors produce: indented like
[TNB], `Backspin` like [MLM], newline-delimited like [FB], one byte at a time,
or two shots in one write.  It reads the replies back through the library's own
decoder, so what it reports is what a client would actually see.

    tools/gsp_shoot.py --port 9210 --fixture gsp_full.json
    tools/gsp_shoot.py --port 9210 --style mlm --repeat 5 --bytewise
    tools/gsp_shoot.py --port 9210 --heartbeat 5

⛔ THIS IS NOT A GSPro CLIENT AND MUST NOT BECOME ONE.  design §1 puts being a
client permanently out of scope: sending shots to a real GSPro drives somebody
else's game, and that is a different program with a different threat model.
So this refuses port 921 on a non-loopback address outright — the one
combination that means "a real GSPro somewhere else" — and says why.

⚠ IT REPLAYS FIXTURE BYTES RATHER THAN RE-SERIALISING THEM.  The fixtures are
byte-exact evidence about sixteen named clients (tests/fixtures/README.md); a
simulator that decoded and re-encoded them would test this library against its
own encoder instead of against what those clients send.
"""

from __future__ import annotations

import argparse
import ipaddress
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import gspro  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures"

__all__ = ["Shooter", "load_fixture", "restyle", "STYLES"]

# ---------------------------------------------------------------------------
# Byte patterns real connectors put on a socket
# ---------------------------------------------------------------------------
STYLES = {
    "asis": "the fixture's own bytes, unchanged",
    "compact": "no whitespace between tokens — [MLM] [R10] [OF] [GC2]",
    "indent": "Newtonsoft's two-space indented form — [TNB] [OSP]",
    "newline": "compact with a trailing '\\n' — [FB] [GG]",
    "mlm": "compact, and 'BackSpin' spelled 'Backspin' — [MLM]",
    "r10": "compact, and 'APIversion' spelled 'APIVersion' — [R10]",
}


def load_fixture(name: str) -> bytes:
    path = Path(name)
    if not path.exists():
        path = FIXTURES / name
    return path.read_bytes()


def restyle(raw: bytes, style: str) -> bytes:
    """⚠ Re-spacing only, never re-serialising.  `mlm` and `r10` rewrite ONE
    key each, which is the whole point of those two rows in conformance §1.1:
    both spellings reach GSPro today and a server that required the vendor's
    would lose every shot from those devices."""
    if style == "asis":
        return raw
    if style not in STYLES:
        raise ValueError(f"unknown style {style!r}; try {', '.join(STYLES)}")

    import json

    obj = json.loads(raw.decode())
    if style == "indent":
        out = json.dumps(obj, indent=2).encode()
    else:
        out = json.dumps(obj, separators=(",", ":")).encode()
    if style == "newline":
        out += b"\n"
    elif style == "mlm":
        out = out.replace(b'"BackSpin"', b'"Backspin"')
    elif style == "r10":
        out = out.replace(b'"APIversion"', b'"APIVersion"')
    return out


def _refuse_real_gspro(host: str, port: int, force: bool) -> None:
    """⛔ The one combination that means somebody else's game."""
    if port != gspro.GSP_DEFAULT_PORT or force:
        return
    try:
        loopback = ipaddress.ip_address(socket.gethostbyname(host)).is_loopback
    except (OSError, ValueError):
        loopback = False
    if loopback:
        return
    raise SystemExit(
        f"⛔ refusing to send shots to {host}:{port}.\n"
        f"   Port {gspro.GSP_DEFAULT_PORT} on a non-loopback address is a real GSPro, and "
        "driving\n"
        "   somebody else's game is out of scope for this project (design §1).\n"
        "   Aim at a libgspro listener, or pass --yes-i-mean-it if this is your own."
    )


class Shooter:
    """A launch monitor, as far as a listener can tell.

    ⚠ Reads replies through ``gspro.decode_response`` — the library's own
    decoder — so a reply this reports as a 200 is one a client would agree is
    a 200.  It does NOT require one: [PIT] never waits for a reply, and a
    simulator that blocked would be testing itself.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = gspro.GSP_DEFAULT_PORT,
                 *, force: bool = False, timeout: float = 5.0):
        _refuse_real_gspro(host, port, force)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buf = b""
        self.reads: list[bytes] = []

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self) -> "Shooter":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # --- sending ----------------------------------------------------------
    def send(self, payload: bytes) -> None:
        self.sock.sendall(payload)

    def send_bytewise(self, payload: bytes, gap: float = 0.0) -> None:
        """⚠ One byte per call, which is what TCP is allowed to do to any
        message and what CT-F02 pins at the framer.  A listener must answer
        after the LAST byte and not before."""
        for i in range(len(payload)):
            self.sock.sendall(payload[i : i + 1])
            if gap:
                time.sleep(gap)

    # --- reading ----------------------------------------------------------
    def read_responses(self, count: int = 1, timeout: float = 2.0) -> list[gspro.PlayerInfo]:
        """Read until `count` complete server→client objects have arrived.

        ⚠ Framed with the library's own framer, because GSPro concatenates
        replies ([SLX]) and a splitter written here would be a second
        implementation of the thing under test.
        """
        out = []
        deadline = time.monotonic() + timeout
        while len(out) < count:
            for obj in list(gspro.frames(self._buf)):
                out.append(gspro.decode_response(obj))
                self._buf = self._buf[self._buf.index(obj) + len(obj) :]
                if len(out) >= count:
                    return out
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.reads.append(chunk)  # ⚠ kept so a test can see the SEGMENTING
            self._buf += chunk
        return out

    def read_all(self, timeout: float = 1.0) -> list:
        """Everything that arrives within `timeout`, whatever it is.

        ⚠ A listener may greet a connection with an unsolicited 201 and 202
        before any shot is sent (design §5.6, §5.6.1), and [OSP]-style clients
        depend on exactly that.  A simulator that read "the next N objects" and
        called them replies would mislabel those two — so the CLI reads
        everything and counts the acknowledgements among it.
        """
        out = []
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            got = self.read_responses(1, timeout=remaining)
            if not got:
                break
            out.extend(got)
        return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=gspro.GSP_DEFAULT_PORT)
    ap.add_argument("--fixture", default="gsp_full.json",
                    help="a file, or a name under tests/fixtures/")
    ap.add_argument("--style", default="asis", choices=sorted(STYLES),
                    help="; ".join(f"{k}: {v}" for k, v in STYLES.items()))
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--bytewise", action="store_true",
                    help="send one byte at a time")
    ap.add_argument("--gap", type=float, default=0.0,
                    help="seconds between bytes, with --bytewise")
    ap.add_argument("--pair", action="store_true",
                    help="two shots in ONE write, as GSPro itself does")
    ap.add_argument("--heartbeat", type=float, default=0.0, metavar="SECONDS",
                    help="after the shots, send a heartbeat this often, forever")
    ap.add_argument("--yes-i-mean-it", dest="force", action="store_true",
                    help="⛔ send to port 921 off-loopback anyway")
    args = ap.parse_args(argv)

    payload = restyle(load_fixture(args.fixture), args.style)
    heartbeat = restyle(load_fixture("r10_heartbeat.json"), args.style)

    with Shooter(args.host, args.port, force=args.force) as shooter:
        print(f"connected to {args.host}:{args.port}  "
              f"({args.fixture}, style={args.style}, {len(payload)} bytes)")
        expected = 0
        for i in range(args.repeat):
            if args.pair:
                shooter.send(payload + payload)
                expected += 2
            elif args.bytewise:
                shooter.send_bytewise(payload, args.gap)
                expected += 1
            else:
                shooter.send(payload)
                expected += 1

        received = shooter.read_all(timeout=2.0)
        acks = 0
        for r in received:
            text = r.message.decode(errors="replace")
            extra = ""
            if r.has_player:
                club = gspro.club_code(gspro.Club(r.player.club)) or "-"
                extra = f"  player: club={club}"
                if r.player.has_distance:
                    extra += f" distance={r.player.distance_to_target}"
            if r.code == int(gspro.ResponseCode.SHOT_RECEIVED):
                acks += 1
            print(f"  <- {r.code} {text!r}{extra}")
        print(f"  {acks}/{expected} acknowledged, {len(received)} object(s) "
              f"in {len(shooter.reads)} read(s)")
        if acks < expected:
            print("  ⚠ fewer acknowledgements than shots sent — a client that waits "
                  "for one\n     ([MLM], 2 s) would re-send")

        if args.heartbeat > 0:
            print(f"heartbeat every {args.heartbeat}s — ctrl-c to stop")
            try:
                while True:
                    time.sleep(args.heartbeat)
                    shooter.send(heartbeat)
                    for r in shooter.read_responses(1, timeout=2.0):
                        print(f"  <- {r.code}")
            except KeyboardInterrupt:
                print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
