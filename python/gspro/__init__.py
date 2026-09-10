# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""Python bindings for libgspro — the sans-I/O library that plays the GSPro
Connect server role.

    import gspro

    with gspro.Server() as s:
        s.on_connection_opened(1, "192.0.2.7:51022", now_us())
        s.on_bytes(1, data, now_us())          # whatever the socket returned
        for w in s.poll_writes():
            sockets[w.conn].sendall(w.data)    # ONE write per request
        for ev in s.poll_events():
            print(ev.text)

The five facts about this protocol the API deliberately makes hard to ignore,
because smoothing any of them away loses shots with no error anywhere:

  1. THERE IS NO FRAMING.  JSON objects arrive back to back, split or merged by
     TCP as it likes, sometimes indented across lines.  Hand the bytes over as
     they came; do not pre-split.
  2. EVERY MESSAGE GETS EXACTLY ONE REPLY, PROMPTLY — a heartbeat is "a valid
     shot message" to GSPro, and a connector that waits 2 s for a reply
     re-sends the shot.
  3. ZERO IS NOT ABSENCE.  Every field carries a presence bit and no value is
     converted, inferred or filled in.
  4. CLIENTS MISSPELL KEYS AND STILL WORK against GSPro.  Keys match
     case-insensitively and the mismatch is flagged rather than hidden.
  5. IT SHOULD NOT OWN THE SOCKET.

⚠ THE CLOCK IS YOURS AND MUST BE MONOTONIC — ``time.monotonic_ns() // 1000``.
The library calls no clock function on any platform, ever; host time enters
only through the ``now_us`` arguments you supply.

⚠ ONE THREAD.  Every call on a server must come from the same thread for the
whole life of that server.  There are no locks, no atomics and no threads
anywhere in the library.

⚠ THE SOCKET IS NOT IN HERE AND IS NOT IMPORTED.
``gspro.asyncio_transport`` is an OPTIONAL reference listener; importing this
package does not import it.  The binding must not choose a socket API any more
than the C core does — a host on Qt, Winsock or IOCP embeds the same server.
"""

from __future__ import annotations

from ._library import (
    ABI_VERSION,
    VERSION,
    AbiMismatch,
    GsProError,
    LibraryNotFound,
    library_path,
)
from ._types import (
    GSP_ALT_PORT,
    GSP_CONN_ALL,
    GSP_CONN_NONE,
    GSP_DEFAULT_PORT,
    GSP_TEXT_READY,
    GSP_TEXT_ROUND_ENDED,
    GSP_TEXT_SHOT_RECEIVED,
    GSP_TIME_NEVER,
    GSP_TIME_UNKNOWN,
    GSP_WRITE_MAX,
    GSP_WIRE_CHUNK_MAX,
    GSP_WIRE_RING_RECOMMENDED,
    BallField,
    CloseCause,
    Club,
    ClubField,
    EventType,
    Handed,
    MessageFlag,
    MessageKind,
    OptionField,
    PlayerInfoReason,
    ProtocolErrorReason,
    ResponseCode,
    SessionState,
    Status,
    Units,
    WarningCode,
    WireDirection,
    WireFlag,
    WriteKind,
)
from .codec import (
    ball_complete,
    ball_data_derive,
    club_code,
    club_name,
    club_parse,
    decode_message,
    decode_response,
    encode_message,
    encode_response,
    flag_names,
    format_message,
    frame_find,
    frames,
    handed_parse,
    handed_text,
    is_shot,
    units_parse,
    units_text,
    zero_speed,
)
from .server import Event, PlayerInfo, Server, WriteRequest, player_info

__all__ = [
    "ABI_VERSION",
    "AbiMismatch",
    "BallField",
    "CloseCause",
    "Club",
    "ClubField",
    "Event",
    "EventType",
    "GSP_ALT_PORT",
    "GSP_CONN_ALL",
    "GSP_CONN_NONE",
    "GSP_DEFAULT_PORT",
    "GSP_TEXT_READY",
    "GSP_TEXT_ROUND_ENDED",
    "GSP_TEXT_SHOT_RECEIVED",
    "GSP_TIME_NEVER",
    "GSP_TIME_UNKNOWN",
    "GSP_WRITE_MAX",
    "GSP_WIRE_CHUNK_MAX",
    "GSP_WIRE_RING_RECOMMENDED",
    "GsProError",
    "Handed",
    "LibraryNotFound",
    "MessageFlag",
    "MessageKind",
    "OptionField",
    "PlayerInfo",
    "PlayerInfoReason",
    "ProtocolErrorReason",
    "ResponseCode",
    "Server",
    "SessionState",
    "Status",
    "Units",
    "VERSION",
    "WarningCode",
    "WireDirection",
    "WireFlag",
    "WriteKind",
    "WriteRequest",
    "ball_complete",
    "ball_data_derive",
    "club_code",
    "club_name",
    "club_parse",
    "decode_message",
    "decode_response",
    "encode_message",
    "encode_response",
    "flag_names",
    "format_message",
    "frame_find",
    "frames",
    "handed_parse",
    "handed_text",
    "is_shot",
    "library_path",
    "player_info",
    "units_parse",
    "units_text",
    "zero_speed",
]
