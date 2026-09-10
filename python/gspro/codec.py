# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""Framing, decoding and encoding WITHOUT a server.

codec.h is public for exactly this: a tool, a test or a binding can parse Open
Connect bytes without owning connection state.  ``gsp_shoot.py`` reads the
replies it gets back with ``decode_response`` here, and a host that builds a
message itself can apply the vendor's own completeness rule to it.

⚠ EVERY FUNCTION HERE IS THE LIBRARY'S, NOT A SECOND IMPLEMENTATION.  A Python
JSON parser beside the C one would be a second thing that can drift, and the
tolerances are the whole content of the C one — case-insensitive keys, a null
where an object belongs, a number that arrived as a string.
"""

from __future__ import annotations

import ctypes

from . import _types as T
from ._library import check, lib

__all__ = [
    "ball_data_derive",
    "ball_complete",
    "club_code",
    "club_name",
    "club_parse",
    "decode_message",
    "decode_response",
    "encode_message",
    "encode_response",
    "flag_name",
    "flag_names",
    "format_message",
    "frame_find",
    "handed_parse",
    "handed_text",
    "is_shot",
    "units_parse",
    "units_text",
    "zero_speed",
]


def _as_bytes(data: bytes):
    return (ctypes.c_uint8 * len(data)).from_buffer_copy(data) if data else None


# ---------------------------------------------------------------------------
# Framing
# ---------------------------------------------------------------------------
def frame_find(buf: bytes) -> tuple[T.Status, int, int]:
    """The first complete top-level object in `buf`, as ``(status, start, end)``.

    ⚠ Brace depth is counted OUTSIDE string literals, so a `{` inside a
    DeviceID does not open an object — a bare counter wedges forever on
    ``"PiTrac {v2}"`` with no error anywhere.

    ``OK`` — the object is ``buf[start:end]``.
    ``PENDING`` — the buffer ends inside an object, or is all whitespace.
    ``ERR_MALFORMED`` — a non-whitespace, non-``{`` byte precedes any object;
    `start` is its offset.  ⚠ A top-level ``[`` is malformed: the protocol has
    no arrays at top level.
    """
    start = ctypes.c_size_t(0)
    end = ctypes.c_size_t(0)
    st = lib.gsp_frame_find(
        _as_bytes(buf), len(buf), ctypes.byref(start), ctypes.byref(end)
    )
    if st == T.Status.ERR_INVALID_ARG:
        check(st, "gsp_frame_find")
    return T.Status(st), start.value, end.value


def frames(buf: bytes):
    """Every complete object in `buf`, in order, as ``bytes``.

    Stops at the first incomplete or malformed one — a convenience for a tool
    with a whole file in hand, never a substitute for the server's framer,
    which is incremental and does not rescan.
    """
    offset = 0
    while offset < len(buf):
        st, start, end = frame_find(buf[offset:])
        if st != T.Status.OK:
            return
        yield buf[offset + start : offset + end]
        offset += end


# ---------------------------------------------------------------------------
# Decoding
# ---------------------------------------------------------------------------
def decode_message(json_bytes: bytes) -> T.gsp_message:
    """One complete object — exactly what ``frame_find`` delimited.

    ⚠ Returns for anything that is a JSON object, however incomplete: the
    findings are in ``.flags`` and the application decides (design §4.5).
    Raises only for bytes that are not a JSON object at all.
    """
    out = T.gsp_message()
    check(
        lib.gsp_message_decode(_as_bytes(json_bytes), len(json_bytes), ctypes.byref(out)),
        "gsp_message_decode",
    )
    return out


def decode_response(json_bytes: bytes) -> T.gsp_response:
    """One server→client object, for a test client."""
    out = T.gsp_response()
    check(
        lib.gsp_response_decode(_as_bytes(json_bytes), len(json_bytes), ctypes.byref(out)),
        "gsp_response_decode",
    )
    return out


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------
def encode_response(
    code: int, message: str | None = None, player: T.gsp_player_info | None = None
) -> bytes:
    """⚠ `code` is validated against the five the library can emit.  There is
    no send_raw() (design §9.1)."""
    buf = ctypes.create_string_buffer(1024)
    written = ctypes.c_size_t(0)
    check(
        lib.gsp_response_encode(
            int(code),
            message.encode() if message is not None else None,
            ctypes.byref(player) if player else None,
            buf,
            len(buf),
            ctypes.byref(written),
        ),
        "gsp_response_encode",
    )
    return buf.raw[: written.value]


def encode_message(m: T.gsp_message, indent: bool = False) -> bytes:
    """A CLIENT→server message — the other direction, for tools and tests that
    play a launch monitor at this library.

    ⚠ This library is not a GSPro client (design §1); `gsp_shoot.py` refuses to
    aim this at port 921 on a non-loopback address.
    """
    buf = ctypes.create_string_buffer(8192)
    written = ctypes.c_size_t(0)
    check(
        lib.gsp_message_encode(
            ctypes.byref(m), bool(indent), buf, len(buf), ctypes.byref(written)
        ),
        "gsp_message_encode",
    )
    return buf.raw[: written.value]


# ---------------------------------------------------------------------------
# Message helpers
# ---------------------------------------------------------------------------
def is_shot(m: T.gsp_message) -> bool:
    return bool(lib.gsp_message_is_shot(ctypes.byref(m)))


def zero_speed(m: T.gsp_message) -> bool:
    """⚠ Not a flag, because zero is a value — but [SLX] shows a commercial
    connector emitting such "shots" and GSPro resetting the club for them, so
    the host needs to be able to ask (protocol §5.2)."""
    return bool(lib.gsp_message_ball_zero_speed(ctypes.byref(m)))


def ball_complete(ball: T.gsp_ball_data) -> bool:
    """The vendor's own completeness rule (protocol §3.2)."""
    return bool(lib.gsp_message_ball_complete(ctypes.byref(ball)))


def ball_data_derive(ball: T.gsp_ball_data) -> T.Status:
    """Fills whichever spin pair is absent from the one present, IN PLACE, and
    marks what it filled in ``.derived``.

    ⚠ It also treats a present-but-exactly-zero TotalSpin beside a non-zero
    BackSpin/SideSpin pair as a placeholder and recomputes it, because [PIT]
    sends exactly that on every shot.  Returns ``PENDING`` when both
    representations were present and disagree by more than 1 % — reported,
    never corrected, because the vendor's own example disagrees.
    """
    return T.Status(check(lib.gsp_ball_data_derive(ctypes.byref(ball)), "derive"))


def format_message(m: T.gsp_message, include_identifiers: bool = False) -> str:
    """⚠ Redacted by default: a DeviceID can carry a hardware serial ([GC2]),
    and so can the shot number beside it (design §9.2)."""
    buf = ctypes.create_string_buffer(1024)
    lib.gsp_message_format(ctypes.byref(m), buf, len(buf), include_identifiers)
    return buf.value.decode(errors="replace")


def flag_name(flag: int) -> str:
    """Names ONE bit; "" for a value that is not exactly one."""
    return lib.gsp_message_flag_name(int(flag)).decode()


def flag_names(flags: int) -> list[str]:
    return [
        name
        for bit in range(32)
        if flags & (1 << bit)
        for name in [flag_name(1 << bit) or f"BIT{bit}"]
    ]


# ---------------------------------------------------------------------------
# The vocabulary — the library's tables, never a second copy
# ---------------------------------------------------------------------------
def units_parse(text: str | None) -> T.Units:
    return T.Units(lib.gsp_units_parse(text.encode() if text else None))


def units_text(units: T.Units) -> str:
    return lib.gsp_units_text(int(units)).decode()


def handed_parse(text: str | None) -> T.Handed:
    return T.Handed(lib.gsp_handed_parse(text.encode() if text else None))


def handed_text(handed: T.Handed) -> str:
    return lib.gsp_handed_text(int(handed)).decode()


def club_parse(code: str | None) -> T.Club:
    """⚠ Unknown is UNKNOWN, never a default club: [PIT] defaults an
    unrecognised code to DRIVER, which is a client-side choice a library making
    it would hide from every consumer."""
    return T.Club(lib.gsp_club_parse(code.encode() if code else None))


def club_code(club: T.Club) -> str:
    return lib.gsp_club_code(int(club)).decode()


def club_name(club: T.Club) -> str:
    """A human name, for a UI — never the wire."""
    return lib.gsp_club_name(int(club)).decode()
