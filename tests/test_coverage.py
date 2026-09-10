#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""Every case in docs/conformance.md has a test, and every test has a case.

⚠ THE SUITE'S OWN COMPLETENESS IS A TEST, because nothing else can be.  A `CT-`
row added to the document without a case is a claim nobody checks; a case with
no row is a preference somebody encoded without saying which client demands it.
Both read as ordinary work and neither fails anything — which is the shape of
failure this project has a rule about.

A case may be DEFERRED, but only here, by id, with a reason.  That turns "we
have not written it yet" from a thing you would have to notice into a line
somebody has to add.

Usage:  test_coverage.py [repo-root]
"""
from __future__ import annotations

import pathlib
import re
import sys

# ---------------------------------------------------------------------------
# Deferred cases: written in the document, not yet in C, with the package from
# docs/design.md §11 that brings them.
#
# ⚠ CT-T* need a real socket and the launch-monitor simulator (tools/gsp_shoot.py).
# They test the HOST's transport — PinPoint's QTcpServer adapter, the asyncio
# transport, the C reference transport — not the library, which owns no socket
# by construction.  There is nothing to run them against until package 4 or 5.
# ---------------------------------------------------------------------------
DEFERRED = {
    "T01": "package 4/5 — needs a socket and gsp_shoot.py",
    "T02": "package 4/5 — needs a socket and gsp_shoot.py",
    "T03": "package 4/5 — needs a socket and gsp_shoot.py",
    "T04": "package 4/5 — needs a socket and gsp_shoot.py",
    "T05": "package 4/5 — needs a socket and gsp_shoot.py",
    "T06": "package 4/5 — needs a second machine on the LAN",
    "T07": "package 4/5 — needs a socket and gsp_shoot.py",
    "T08": "package 4/5 — needs a socket and gsp_shoot.py",
    "T09": "package 4/5 — needs a socket and gsp_shoot.py",
    "T10": "package 4/5 — needs a socket and gsp_shoot.py",
}


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                        else pathlib.Path(__file__).resolve().parent.parent)
    doc_path = root / "docs" / "conformance.md"
    if not doc_path.exists():
        print(f"cannot find {doc_path}", file=sys.stderr)
        return 2

    documented = set(re.findall(r"CT-([A-Z]\d+[a-z]?)\b", doc_path.read_text()))
    implemented: dict[str, str] = {}
    for src in sorted((root / "tests").glob("test_*.c")):
        for case in re.findall(r"GS_TEST\(CT_([A-Z]\d+[a-z]?)_", src.read_text()):
            if case in implemented:
                print(f"FAIL CT-{case} is defined twice "
                      f"({implemented[case]} and {src.name})", file=sys.stderr)
                return 1
            implemented[case] = src.name

    failures = 0

    missing = sorted(documented - set(implemented) - set(DEFERRED))
    for case in missing:
        print(f"FAIL CT-{case} is in conformance.md with no case in tests/", file=sys.stderr)
        failures += 1

    orphan = sorted(set(implemented) - documented)
    for case in orphan:
        print(f"FAIL CT-{case} is a case in {implemented[case]} with no row in "
              f"conformance.md — say which client demands it", file=sys.stderr)
        failures += 1

    # A deferred id must still be documented, and must NOT already be written:
    # a stale entry here would hide a real gap the day somebody removes the row.
    for case, why in sorted(DEFERRED.items()):
        if case not in documented:
            print(f"FAIL CT-{case} is deferred but no longer in conformance.md — "
                  f"drop it from DEFERRED", file=sys.stderr)
            failures += 1
        if case in implemented:
            print(f"FAIL CT-{case} is implemented in {implemented[case]} but still "
                  f"listed as deferred ({why}) — drop it from DEFERRED", file=sys.stderr)
            failures += 1

    # Every fixture the document names must exist, and every fixture on disk
    # must be named — an unreferenced fixture is one nothing reads.
    fixtures_dir = root / "tests" / "fixtures"
    named = set(re.findall(r"`([a-z0-9_]+\.json)`", doc_path.read_text()))
    named |= set(re.findall(r"`([a-z0-9_]+\.json)`",
                            (fixtures_dir / "README.md").read_text()))
    on_disk = {p.name for p in fixtures_dir.glob("*.json")}
    for name in sorted(named - on_disk):
        print(f"FAIL fixture {name} is named in the documents but not on disk",
              file=sys.stderr)
        failures += 1
    for name in sorted(on_disk - named):
        print(f"FAIL fixture {name} is on disk but named in no document — "
              f"add a row saying what it pins", file=sys.stderr)
        failures += 1

    print(f"coverage: {len(implemented)} case(s) implemented, "
          f"{len(DEFERRED)} deferred, {len(on_disk)} fixture(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
