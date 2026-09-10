#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""Cross-check the conformance fixtures against docs/protocol.md, in Python.

⚠ THIS DELIBERATELY TOUCHES NO C.  The fixtures are evidence about sixteen
launch-monitor clients (fixtures/README.md); the C suite is a claim about
libgspro.  Checking the evidence with a second, independent implementation is
what stops a fixture and a decoder being wrong together — the failure mode where
a suite is green because both sides share one mistake.

It also means there is something to run before the library exists.

Usage:  test_fixtures.py [fixtures-dir]
"""
from __future__ import annotations

import json
import pathlib
import sys

# Every key the protocol defines, by object.  protocol.md §3.
ROOT_KEYS = {"DeviceID", "Units", "ShotNumber", "APIversion", "BallData",
             "ClubData", "ShotDataOptions"}
BALL_KEYS = {"Speed", "SpinAxis", "TotalSpin", "BackSpin", "SideSpin", "HLA",
             "VLA", "CarryDistance"}
CLUB_KEYS = {"Speed", "AngleOfAttack", "FaceToTarget", "Lie", "Loft", "Path",
             "SpeedAtImpact", "VerticalFaceImpact", "HorizontalFaceImpact",
             "ClosureRate"}
OPT_KEYS = {"ContainsBallData", "ContainsClubData", "LaunchMonitorIsReady",
            "LaunchMonitorBallDetected", "IsHeartBeat"}

# Fixtures that are NOT valid JSON on purpose, and why.
NOT_JSON = {"gsp_full_commented.json": "the vendor's page shows // comments"}

# Fixtures whose bytes must end in a newline — protocol.md §2.  Three of the
# sixteen clients delimit; the rest do not, and getting this wrong in either
# direction is the thing a framer must tolerate.
MUST_END_NEWLINE = {"fb_shot_newline.json", "fb_heartbeat_newline.json",
                    "gg_shot_newline.json", "pit_shot.json",
                    "pit_keepalive.json"}

# Known, deliberate spellings that are NOT the vendor's — protocol.md §9.1.
KNOWN_MISSPELLINGS = {"Backspin", "APIVersion", "Apiversion"}
# Known extra keys no version of the protocol defines — protocol.md §9.7.
KNOWN_EXTRA = {"ClubName"}

failures: list[str] = []
checks = 0


def check(cond: bool, what: str) -> None:
    global checks
    checks += 1
    if not cond:
        failures.append(what)


def canonical(key: str) -> str:
    """Fold a key the way a conforming server must (protocol.md §9.1)."""
    return key.lower()


def check_fixture(path: pathlib.Path) -> None:
    raw = path.read_bytes()
    name = path.name

    check(len(raw) > 0, f"{name}: empty")
    check((raw[-1:] == b"\n") == (name in MUST_END_NEWLINE),
          f"{name}: trailing-newline expectation "
          f"(has={raw[-1:]!r}, expected={'newline' if name in MUST_END_NEWLINE else 'none'})")

    if name in NOT_JSON:
        try:
            json.loads(raw)
        except ValueError:
            return  # correct: it must not parse
        failures.append(f"{name}: parses as JSON but must not ({NOT_JSON[name]})")
        return

    try:
        obj = json.loads(raw)
    except ValueError as e:
        failures.append(f"{name}: not valid JSON: {e}")
        return

    check(isinstance(obj, dict), f"{name}: top level is not an object")
    if not isinstance(obj, dict):
        return

    # Response fixtures are checked elsewhere; these are all client messages.
    for key in obj:
        known = canonical(key) in {canonical(k) for k in ROOT_KEYS}
        check(known or key in KNOWN_EXTRA,
              f"{name}: unknown root key {key!r}")
        if known and key not in ROOT_KEYS:
            check(key in KNOWN_MISSPELLINGS,
                  f"{name}: undocumented case variant {key!r}")

    ball = obj.get("BallData")
    if isinstance(ball, dict):
        for key in ball:
            known = canonical(key) in {canonical(k) for k in BALL_KEYS}
            check(known, f"{name}: unknown BallData key {key!r}")
            if known and key not in BALL_KEYS:
                check(key in KNOWN_MISSPELLINGS,
                      f"{name}: undocumented BallData case variant {key!r}")

    club = obj.get("ClubData")
    if isinstance(club, dict):
        for key in club:
            check(canonical(key) in {canonical(k) for k in CLUB_KEYS},
                  f"{name}: unknown ClubData key {key!r}")

    opts = obj.get("ShotDataOptions")
    if isinstance(opts, dict):
        for key in opts:
            check(canonical(key) in {canonical(k) for k in OPT_KEYS},
                  f"{name}: unknown ShotDataOptions key {key!r}")
        # The two the vendor marks required are present in every fixture that
        # has the object at all.
        have = {canonical(k) for k in opts}
        check("containsballdata" in have,
              f"{name}: ShotDataOptions without ContainsBallData")
        check("containsclubdata" in have,
              f"{name}: ShotDataOptions without ContainsClubData")


def check_claims(fixtures: pathlib.Path) -> None:
    """The specific claims fixtures/README.md makes, as assertions."""
    def load(n):
        return json.loads((fixtures / n).read_text())

    # [TL] sends no Units key — its own bug, and the only real source for the
    # absent-Units path (CT-D21).
    check("Units" not in load("tl_minimal.json"),
          "tl_minimal.json: should carry no Units key")

    # [MLM] misspells backspin and still works against GSPro (CT-D03).
    check("Backspin" in load("mlm_backspin.json")["BallData"],
          "mlm_backspin.json: should spell it 'Backspin'")

    # [R10] capitalises the V and writes nulls (CT-D04, CT-D07).
    r10 = load("r10_apiversion.json")
    check("APIVersion" in r10, "r10_apiversion.json: should use 'APIVersion'")
    check(r10["ClubData"] is None, "r10_apiversion.json: ClubData should be null")

    # [OSG] sends an integer as a float (CT-D05).
    sn = load("osg_float_shot_number.json")["ShotNumber"]
    check(isinstance(sn, float) and sn == 13.0,
          "osg_float_shot_number.json: ShotNumber should be the float 13.0")

    # [PIT] places a zero where the total spin belongs (CT-D13).
    pit = load("pit_shot.json")["BallData"]
    check(pit["TotalSpin"] == 0.0 and pit["BackSpin"] != 0.0,
          "pit_shot.json: TotalSpin should be a 0.0 placeholder beside a real pair")

    # [OB] sends a null object and an undefined key (CT-D07, CT-D08).
    ob = load("ob_shot.json")
    check(ob["ClubData"] is None, "ob_shot.json: ClubData should be null")
    check("ClubName" in ob, "ob_shot.json: should carry the extra ClubName key")

    # [SB] sends everything as strings (CT-D09).
    sb = load("sb_strings.json")
    check(isinstance(sb["BallData"]["Speed"], str),
          "sb_strings.json: Speed should be a string")
    check(isinstance(sb["ShotDataOptions"]["ContainsBallData"], str),
          "sb_strings.json: ContainsBallData should be a string")

    # [GC2] leaks a serial into DeviceID (CT-D18).
    check("(" in load("gc2_shot.json")["DeviceID"],
          "gc2_shot.json: DeviceID should carry the unit serial")

    # [SLX] the zero-speed shot (CT-D12).
    slx = load("slx_zero_speed.json")
    check(slx["BallData"]["Speed"] == 0 and slx["ShotDataOptions"]["ContainsBallData"],
          "slx_zero_speed.json: should be a shot claiming ball data with zero speed")

    # [OF] and [GC2] agree with the derivation identity (protocol §3.2), and the
    # vendor's own example does not — which is why derive() reports rather than
    # corrects (CT-D14, CT-D15).
    import math
    for n, consistent in (("of_shot.json", True), ("gc2_shot.json", True),
                          ("gsp_full.json", False)):
        b = load(n)["BallData"]
        hyp = math.hypot(b["BackSpin"], b["SideSpin"])
        agree = abs(hyp - b["TotalSpin"]) <= 0.01 * max(1.0, b["TotalSpin"])
        check(agree == consistent,
              f"{n}: spin pair vs total consistency should be {consistent} "
              f"(hypot={hyp:.1f}, total={b['TotalSpin']})")


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                        else pathlib.Path(__file__).parent / "fixtures")
    files = sorted(root.glob("*.json"))
    if not files:
        print(f"no fixtures under {root}", file=sys.stderr)
        return 2

    for path in files:
        check_fixture(path)
    check_claims(root)

    for f in failures:
        print(f"FAIL {f}", file=sys.stderr)
    print(f"fixtures: {len(files)} file(s), {checks} check(s), {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
