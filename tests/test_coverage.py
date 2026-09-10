#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""Every case in docs/conformance.md has a test, and every test has a case.

⚠ THE SUITE'S OWN COMPLETENESS IS A TEST, because nothing else can be.  A `CT-`
row added to the document without a case is a claim nobody checks; a case with
no row is a preference somebody encoded without saying which client demands it.
Both read as ordinary work and neither fails anything — which is the shape of
failure this project has a rule about.

⚠ ONE EXCEPTION, AND IT IS THE POINT OF conformance §3.8: a CT-T row is about a
HOST rather than about the library, so it is written ONCE PER HOST ADAPTER — the
asyncio transport and the C reference transport today, PinPoint's QTcpServer one
when package 8 lands.  Those ids are expected in two files and are checked for
being in BOTH.

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
# Deferred cases: written in the document, with no case anywhere yet.
#
# ⚠ ONE ENTRY, AND IT IS NOT DEFERRED FOR CONVENIENCE.  CT-T06 needs a client
# on a SECOND MACHINE, which no amount of code on this one can simulate: it is
# the case that tells a listener bound to 0.0.0.0 from one bound to loopback,
# and that is the difference between a launch monitor working and not
# (design §6.1).  Both transport suites print how to run it by hand, and both
# refuse to pretend: it is deferred here rather than approximated there.
#
# The other nine CT-T rows ran as soon as there was a socket to run them
# against — package 4's asyncio transport and tools/gsp_shoot.py — and run a
# SECOND time, against the C reference transport, as of package 5.
# ---------------------------------------------------------------------------
DEFERRED = {
    "T06": "needs a real LAN and a second host; run by hand, see test_python_transport.py",
}


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                        else pathlib.Path(__file__).resolve().parent.parent)
    doc_path = root / "docs" / "conformance.md"
    if not doc_path.exists():
        print(f"cannot find {doc_path}", file=sys.stderr)
        return 2

    documented = set(re.findall(r"CT-([A-Z]\d+[a-z]?)\b", doc_path.read_text()))
    implemented: dict[str, list[str]] = {}

    # ⚠ TWO LANGUAGES, ONE REGISTRY.  The C cases test the library, which owns
    # no socket; the Python ones test a HOST driving it over a real one
    # (conformance §3.8).  A case is a case wherever it lives, and counting only
    # the C ones would report every CT-T row as missing the day it landed.
    sources = [
        (sorted((root / "tests").glob("test_*.c")), r"GS_TEST\(CT_([A-Z]\d+[a-z]?)_"),
        (sorted((root / "tests").glob("test_python_*.py")),
         r"^async def CT_([A-Z]\d+[a-z]?)_|^def CT_([A-Z]\d+[a-z]?)_"),
    ]
    for paths, pattern in sources:
        for src in paths:
            for match in re.finditer(pattern, src.read_text(), re.MULTILINE):
                case = match.group(1) or match.group(2)
                where = implemented.setdefault(case, [])
                # ⚠ ONE CASE, ONE PLACE — EXCEPT THE HOST-TRANSPORT FAMILY.  A CT
                # id written twice is normally a case somebody duplicated instead
                # of finding, and the two copies drift.  CT-T is the exception the
                # document itself names (conformance §3.8): those rows are about a
                # HOST rather than about the library, so the checklist is run ONCE
                # PER HOST ADAPTER — the asyncio transport, the C reference
                # transport, and PinPoint's QTcpServer one — and a row covered by
                # only one of them is a row nobody has checked on the others.
                # Twice in the SAME file is still a duplicate.
                if where and (not case.startswith("T") or src.name in where):
                    print(f"FAIL CT-{case} is defined twice "
                          f"({', '.join(where)} and {src.name})", file=sys.stderr)
                    return 1
                where.append(src.name)

    failures = 0

    missing = sorted(documented - set(implemented) - set(DEFERRED))
    for case in missing:
        print(f"FAIL CT-{case} is in conformance.md with no case in tests/", file=sys.stderr)
        failures += 1

    orphan = sorted(set(implemented) - documented)
    for case in orphan:
        print(f"FAIL CT-{case} is a case in {', '.join(implemented[case])} with no row "
              f"in conformance.md — say which client demands it", file=sys.stderr)
        failures += 1

    # ⚠ AND A HOST-TRANSPORT ROW RUN AGAINST ONLY ONE ADAPTER IS HALF-CHECKED.
    # Both reference adapters exist now (package 4 and package 5), so a T row
    # that reaches only one of them is either a gap or a claim that the row
    # cannot apply to the other — which is a sentence somebody has to write.
    adapters = {"test_python_transport.py": "asyncio", "test_net.c": "C reference"}
    for case in sorted(c for c in implemented if c.startswith("T")):
        missing_adapters = sorted(name for src, name in adapters.items()
                                  if src not in implemented[case])
        if missing_adapters:
            print(f"FAIL CT-{case} runs against only "
                  f"{', '.join(adapters.get(s, s) for s in implemented[case])} — "
                  f"conformance §3.8 wants it against {', '.join(missing_adapters)} too",
                  file=sys.stderr)
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

    runs = sum(len(v) for v in implemented.values())
    print(f"coverage: {len(implemented)} case(s) implemented in {runs} run(s) "
          f"({sum(1 for v in implemented.values() if len(v) > 1)} host-transport "
          f"row(s) on two adapters), {len(DEFERRED)} deferred, {len(on_disk)} "
          f"fixture(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
