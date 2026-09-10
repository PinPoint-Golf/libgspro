#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""test_python_wire.py — the `.gswire` container, written in Python, read in C.

⚠ ONE FORMAT, TWO IMPLEMENTATIONS, CHECKED BOTH WAYS.  A container defined by
exactly one program is a container defined by that program's bugs — and this one
has to outlive a release, because the capture it holds was taken beside a launch
monitor that is not coming back (design §7, §11 package 7).  So:

    this file            Python writes it   →   the C `gswire` tool reads it
    test_gsplisten.py    C records it       →   python/gspro/wire.py reads it

⚠ NO CT IDS: docs/conformance.md numbers what the PROTOCOL and its clients
demand, and this is a container of ours.  What the CORE must record is
CT-W01…W12 in tests/test_wire.c.

Usage: test_python_wire.py <path to libgspro_ffi> <path to gswire>
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

import os  # noqa: E402

if len(sys.argv) > 1:
    os.environ["GSPRO_LIBRARY"] = sys.argv[1]

import gspro  # noqa: E402
from gspro.wire import Chunk, Reader, Recording, Writer  # noqa: E402

GSWIRE = sys.argv[2] if len(sys.argv) > 2 else None

failures = 0


def check(condition, what: str) -> None:
    global failures
    if condition:
        print(f"[ ok ] {what}")
    else:
        failures += 1
        print(f"[FAIL] {what}")


def run_tool(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([GSWIRE, *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


SHOT = b'{"DeviceID":"PY-TEST-DEVICE","ShotNumber":41,"ShotDataOptions":' \
       b'{"ContainsBallData":true,"ContainsClubData":false},' \
       b'"BallData":{"Speed":100.1,"SpinAxis":-3.2,"TotalSpin":3100.0,' \
       b'"HLA":1.1,"VLA":14.2}}'


def capture_a_session(path: Path, *, identifiers: bool) -> int:
    """Drive a Server with the wire log on and write what it recorded.

    ⚠ THE BINDING RECORDS THE SAME WAY THE C HOST DOES — poll_wire, write, every
    time round the loop — because it is the same ring underneath.
    """
    server = gspro.Server(wire_ring=64, record_identifiers=identifiers)
    info = Recording(port=921, identifiers_recorded=identifiers,
                     note="written by tests/test_python_wire.py")
    try:
        with Writer(path, info) as writer:
            server.on_connection_opened(1, "203.0.113.9:50000", 1_000_000)
            writer.write(server.drain_wire())
            server.on_bytes(1, SHOT, 1_000_100)
            for w in server.drain_writes():   # the reply reaches the log on poll
                pass
            writer.write(server.drain_wire())
            server.on_connection_closed(1, gspro.CloseCause.REMOTE_CLOSED, 1_000_200)
            writer.write(server.drain_wire())
            return writer.chunks
    finally:
        server.close()
        server.destroy()


def main() -> int:
    if GSWIRE is None:
        print("no gswire binary given; nothing to check against")
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "python_written.gswire"
        written = capture_a_session(path, identifiers=False)
        check(written >= 4, f"a session became {written} chunk(s)")

        # --- the C tool reads what Python wrote ---------------------------
        stats = run_tool("stats", str(path))
        check(stats.returncode == 0, "the C tool reads a Python-written capture")
        check(f"chunks      {written}" in stats.stdout,
              f"and counts the same {written} chunk(s)")
        check("identifiers redacted" in stats.stdout,
              "and reads the header back as redacted")
        check("written by tests/test_python_wire.py" in stats.stdout,
              "including the note")

        # --- and replays it through the library ---------------------------
        replay = run_tool("replay", str(path))
        check(replay.returncode == 0, "the C tool replays it")
        if "messages    1  (1 shot(s))" not in replay.stdout:
            print(replay.stdout)   # only when it is going to fail
        check("messages    1  (1 shot(s))" in replay.stdout,
              "and the shot decodes on the way back through")
        check("1 matched" in replay.stdout,
              "and the reply it produces now matches the one recorded then")

        # --- the payload survived byte for byte ---------------------------
        extract = subprocess.run([GSWIRE, "extract", str(path), "1"],
                                 capture_output=True)
        check(extract.returncode == 0, "one message extracts")
        # ⚠ REDACTED, and the same length: the C core overwrote the DeviceID
        # value in place so the JSON still parses (design §9.2, CT-W09).
        check(len(extract.stdout) == len(SHOT),
              f"the extracted message is the same length ({len(extract.stdout)})")
        check(b"PY-TEST-DEVICE" not in extract.stdout,
              "and the DeviceID is not in it")
        check(b"<redacted>" in extract.stdout, "because it was overwritten in place")

        # --- Python reads its own file back -------------------------------
        with Reader(path) as reader:
            chunks = list(reader)
        check(len(chunks) == written, "Python reads back what it wrote")
        check(chunks[0].direction == int(gspro.WireDirection.META),
              "the first chunk is the connection opening")
        check(any(c.direction == int(gspro.WireDirection.CLIENT_TO_SERVER)
                  for c in chunks), "the shot is in there")
        check(any(c.direction == int(gspro.WireDirection.SERVER_TO_CLIENT)
                  for c in chunks), "and the reply")
        check([c.sequence for c in chunks] == sorted(c.sequence for c in chunks),
              "sequence numbers are in order")

        # --- and refuses what it should -----------------------------------
        bad = Path(tmp) / "not_a_capture.gswire"
        bad.write_bytes(b'{"DeviceID":"A"}\n')
        try:
            Reader(bad)
            check(False, "a file that is not a capture is refused")
        except ValueError:
            check(True, "a file that is not a capture is refused")

        truncated = Path(tmp) / "truncated.gswire"
        truncated.write_bytes(path.read_bytes()[:-4])
        try:
            list(read_all(truncated))
            check(False, "a file that ends mid-record is refused")
        except ValueError:
            check(True, "a file that ends mid-record is refused")

        # ⚠ And the identifiers=recorded header is not a detail: it is what says
        # whether a capture can be shared (design §9.2).
        kept = Path(tmp) / "with_identifiers.gswire"
        capture_a_session(kept, identifiers=True)
        with Reader(kept) as reader:
            check(reader.info.identifiers_recorded,
                  "a capture taken with identifiers says so in its header")
            payloads = b"".join(c.data for c in reader)
        check(b"PY-TEST-DEVICE" in payloads,
              "and really does hold them, which is why the header matters")
        held = run_tool("stats", str(kept))
        check("identifiers RECORDED" in held.stdout,
              "and the C tool warns about it in the same words")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


def read_all(path: Path) -> list[Chunk]:
    with Reader(path) as reader:
        return list(reader)


if __name__ == "__main__":
    sys.exit(main())
