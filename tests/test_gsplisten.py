#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""test_gsplisten.py — the C listener tool, end to end, as a user runs it.

⚠ NOT A CONFORMANCE CASE, AND DELIBERATELY NOT NAMED LIKE ONE.  docs/conformance.md
numbers what the PROTOCOL and its clients demand; this is about one of our own
tools.  It carries no CT id and tests/test_coverage.py does not scan it.

⚠ WHY IT EXISTS ANYWAY.  tests/test_net.c exercises the transport by linking it;
nothing exercised `gsplisten` itself — argument parsing, the bind-failure report,
the event printing, the exit path — and a tool that only ever runs by hand is a
tool that breaks between the times somebody runs it.  This is the first thing a
user meets with a launch monitor on the mat (design §11 package 7), so it is
worth the twenty lines.

⚠ NO libgspro IMPORT HERE.  It speaks the wire with a plain socket and a fixture
file, so it needs neither the FFI object nor the binding: it must be able to fail
for a reason that is about the TOOL.

Usage: test_gsplisten.py <path to gsplisten> <fixture dir>
"""

from __future__ import annotations

import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

failures = 0


def check(condition, what: str) -> None:
    global failures
    if condition:
        print(f"[ ok ] {what}")
    else:
        failures += 1
        print(f"[FAIL] {what}")


def wait_for_port(proc: subprocess.Popen, timeout: float = 10.0) -> int:
    """⚠ Read the port off stdout rather than guessing one.  --port 0 asks the
    kernel for a free one, which is what keeps this from colliding with whatever
    else holds 921 on a developer's machine or a CI runner."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise RuntimeError("gsplisten exited before it announced a port")
            continue
        print(f"    | {line.rstrip()}")
        match = re.search(r"listening on \S+:(\d+)", line)
        if match:
            return int(match.group(1))
    raise RuntimeError("gsplisten never announced a port")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    tool = Path(sys.argv[1])
    fixtures = Path(sys.argv[2])

    # --help must work and must not need a socket.
    # ⚠ ENCODING SPELLED OUT.  The tool reports in the house style — ⚠, ⛔, § —
    # and on Windows a pipe hands CPython cp1252, where reading those bytes
    # raises UnicodeDecodeError and fails the test on its OUTPUT rather than its
    # subject.  errors="replace" so a mojibake byte is a mark, not an exception.
    helped = subprocess.run([str(tool), "--help"], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    check(helped.returncode == 0, "--help exits 0")
    check("--identifiers" in helped.stdout, "--help documents --identifiers")

    # ⚠ An unknown option must FAIL rather than be ignored: a typo in --host on
    # the machine with the launch monitor is a session spent debugging silence.
    bad = subprocess.run([str(tool), "--nonsense"], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    check(bad.returncode != 0, "an unknown option is refused")

    shot = (fixtures / "gsp_full.json").read_bytes()

    proc = subprocess.Popen(
        [str(tool), "--host", "127.0.0.1", "--port", "0", "--shots", "1",
         "--club", "PT", "--distance", "4.2", "--handed", "RH", "--session-active"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
        encoding="utf-8", errors="replace")
    try:
        port = wait_for_port(proc)
        with socket.create_connection(("127.0.0.1", port), timeout=5.0) as sock:
            sock.settimeout(5.0)
            # A 201 and a 202 are owed on connect (policy.announce_*_on_connect),
            # then a 200 for the shot.
            sock.sendall(shot)
            seen: list[int] = []
            buf = b""
            deadline = time.monotonic() + 5.0
            while len(seen) < 3 and time.monotonic() < deadline:
                buf += sock.recv(4096)
                while buf:
                    try:
                        obj, end = json.JSONDecoder().raw_decode(buf.decode("utf-8"))
                    except ValueError:
                        break
                    seen.append(int(obj["Code"]))
                    buf = buf[end:].lstrip()
            check(200 in seen, f"the shot is acknowledged with a 200 (saw {seen})")
            check(201 in seen, "the player information arrives on connect")
            check(202 in seen, "and the 202 an [OSP]-style client arms on")

        rest = proc.communicate(timeout=10)[0]
        print("".join(f"    | {line}\n" for line in rest.splitlines()))
        check(proc.returncode == 0, "--shots 1 exits cleanly after one shot")
        check("SHOT" in rest, "the shot event was printed")
        check("1 shot(s) received" in rest, "and counted in the summary")
        # ⚠ Identifiers are REDACTED unless asked for (design §9.2): the peer
        # address of the connection above must not be in this output.
        check("127.0.0.1:" not in rest, "the peer address was redacted")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
