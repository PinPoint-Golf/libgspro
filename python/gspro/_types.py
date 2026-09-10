# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""The public structs and enums, declared a second time in ctypes.

⚠ THIS FILE IS A SECOND COPY OF THE HEADERS AND IS TREATED AS ONE.

A field one slot out of place here decodes every shot into plausible nonsense
with no error anywhere — a `spin_axis` read at `total_spin`'s offset gives a
wrong-but-believable spin axis on every shot, forever.  ``gsp_abi_check()``
does not close it: version.h says outright that it compares struct SIZES and
that "a binding whose field is one slot out passes this check and returns
plausible numbers".

So nothing here is trusted on inspection.  ``tests/test_python_abi.py``
compares every struct's size, every field's name and offset, every enumerator
and every bound against ``tools/gs_abi_table.c``, which reads them out of the
compiler.

⚠ THE COMMENTS HERE ARE THE SHORT FORM.  Each names the trap and points at the
header carrying the argument; the headers are not summarised, because a summary
that drifts is worse than a pointer.  Read ``include/gspro/`` before changing a
meaning.
"""

from __future__ import annotations

import enum
from ctypes import (
    POINTER,
    Structure,
    Union,
    c_bool,
    c_char,
    c_double,
    c_int32,
    c_int64,
    c_uint8,
    c_uint16,
    c_uint32,
    c_void_p,
)

# --- types.h ---------------------------------------------------------------
# ⚠ Microseconds on a clock THE CALLER OWNS and the library never reads.  It
# must be monotonic: ``time.monotonic_ns() // 1000``.  A wall clock stepped by
# NTP produces a session whose ordering is a fiction.
gsp_time_us = c_int64

# ⚠ And the arrival stamp is the ONLY clock a shot has (protocol §8).  A launch
# monitor measures, computes and sends some hundreds of milliseconds after
# impact: treat it as an upper bound on the strike, never an estimate of it.
GSP_TIME_UNKNOWN = -(2**63)  # INT64_MIN — structurally unavailable
GSP_TIME_NEVER = 2**63 - 1  # INT64_MAX — nothing is pending, the ordinary case

# The connection id is the HOST's (design §3.2): any non-zero uint32 it can map
# back to its own socket.
gsp_conn_id = c_uint32
GSP_CONN_NONE = 0
GSP_CONN_ALL = 0xFFFFFFFF  # broadcast, where accepted

# --- bounds that appear in struct layouts ----------------------------------
GSP_DEVICE_ID_MAX = 64
GSP_UNITS_TEXT_MAX = 16
GSP_API_VERSION_MAX = 8
GSP_SURFACE_MAX = 16
GSP_PEER_MAX = 64
GSP_ERROR_SNIPPET_MAX = 32
GSP_WARNING_TEXT_MAX = 64
GSP_ACK_TEXT_MAX = 64
GSP_WRITE_MAX = 256
GSP_WIRE_CHUNK_MAX = 512
GSP_RESPONSE_TEXT_MAX = 96

GSP_MESSAGE_LAYOUT_VERSION = 1
GSP_ABI_VERSION = 1

# --- protocol.h ------------------------------------------------------------
GSP_DEFAULT_PORT = 921
GSP_ALT_PORT = 922  # [SLX]: GSPconnect.exe.config's <OpenAPIUseAltPort>
GSP_API_VERSION_TEXT = "1"

GSP_TEXT_SHOT_RECEIVED = "Shot received successfully"
GSP_TEXT_PLAYER_INFO = "GSPro Player Information"
# ⚠ FIXED, NOT POLICY: [OSP] compares these two exactly (protocol §5.1).
GSP_TEXT_READY = "GSPro ready"
GSP_TEXT_ROUND_ENDED = "GSPro round ended"
GSP_TEXT_BAD_JSON = "Bad JSON data"
GSP_TEXT_TOO_LARGE = "Payload too large"
GSP_TEXT_INCOMPLETE = "Incomplete ball data"

# --- server.h defaults -----------------------------------------------------
GSP_MAX_CONNECTIONS_DEFAULT = 4
GSP_EVENT_RING_DEFAULT = 64
GSP_WRITE_RING_DEFAULT = 32
GSP_WIRE_RING_RECOMMENDED = 256  # ⚠ the wire ring is OFF unless asked for

# --- sentinels (bounds, not wire values) -----------------------------------
GSP_CLUB_COUNT = 28
GSP_EVENT_TYPE_COUNT = 15
GSP_WARNING_CODE_COUNT = 6


# ---------------------------------------------------------------------------
# Enums
#
# ⚠ Every one is pinned against the compiler in tests/test_python_abi.py.  A
# value that drifts lands on its neighbour: a club that reads one place out
# puts a launch monitor into the wrong mode with nothing reporting a fault.
# ---------------------------------------------------------------------------
class Status(enum.IntEnum):
    """⚠ NON-NEGATIVE IS SUCCESS.  Test ``st < Status.OK``, never ``!=`` —
    PENDING is not an error."""

    OK = 0
    PENDING = 1

    ERR_INVALID_ARG = -1
    ERR_INVALID_STATE = -2
    ERR_NO_MEMORY = -3
    ERR_BUFFER_TOO_SMALL = -4
    ERR_NOT_SUPPORTED = -5
    ERR_TOO_MANY_CONNECTIONS = -6
    ERR_UNKNOWN_CONNECTION = -7
    ERR_MALFORMED = -8
    ERR_MESSAGE_TOO_LARGE = -9
    ERR_QUEUE_FULL = -10  # ⚠ the WRITE ring — design §3.4
    ERR_CLOSED = -11


class Units(enum.IntEnum):
    """⚠ What the field GOVERNS is not stated by any source (protocol §3.6).
    The library converts nothing and keeps the original string beside this."""

    UNKNOWN = 0
    YARDS = 1
    METERS = 2


class Handed(enum.IntEnum):
    UNKNOWN = 0  # omitted from the 201 JSON when sending
    RIGHT = 1
    LEFT = 2


class Club(enum.IntEnum):
    """⚠ THE SHOT MESSAGE CARRIES NO CLUB.  This is for the 201 the SERVER
    sends; nothing a launch monitor sends names a club (protocol §5.2)."""

    UNKNOWN = 0
    DR = 1
    W2 = 2
    W3 = 3
    W4 = 4
    W5 = 5
    W6 = 6
    W7 = 7
    H2 = 8
    H3 = 9
    H4 = 10
    H5 = 11
    H6 = 12
    H7 = 13
    I1 = 14
    I2 = 15
    I3 = 16
    I4 = 17
    I5 = 18
    I6 = 19
    I7 = 20
    I8 = 21
    I9 = 22
    PW = 23
    GW = 24
    SW = 25
    LW = 26
    PT = 27  # ⚠ the one clients act on: [MLM] switches to putting mode on "PT"


class ResponseCode(enum.IntEnum):
    """The five codes the library can emit, and the only five it will."""

    SHOT_RECEIVED = 200
    PLAYER_INFO = 201
    READY = 202  # ⚠ [OSP] does not arm its device until it has seen one
    ROUND_ENDED = 203
    FAILURE = 501


class BallField(enum.IntFlag):
    """⚠ A CLEAR BIT MEANS THE KEY WAS NOT ON THE WIRE.  A set bit with 0.0
    means the client sent zero, and only the application knows whether that
    device means it (design §4.3)."""

    SPEED = 1 << 0
    SPIN_AXIS = 1 << 1
    TOTAL_SPIN = 1 << 2
    BACK_SPIN = 1 << 3
    SIDE_SPIN = 1 << 4
    HLA = 1 << 5
    VLA = 1 << 6
    CARRY_DISTANCE = 1 << 7


class ClubField(enum.IntFlag):
    SPEED = 1 << 0
    ANGLE_OF_ATTACK = 1 << 1
    FACE_TO_TARGET = 1 << 2
    LIE = 1 << 3
    LOFT = 1 << 4
    PATH = 1 << 5
    SPEED_AT_IMPACT = 1 << 6
    VERTICAL_FACE_IMPACT = 1 << 7
    HORIZONTAL_FACE_IMPACT = 1 << 8  # ⚠ [MLM] puts FACE-TO-PATH here
    CLOSURE_RATE = 1 << 9


class OptionField(enum.IntFlag):
    CONTAINS_BALL_DATA = 1 << 0
    CONTAINS_CLUB_DATA = 1 << 1
    LAUNCH_MONITOR_IS_READY = 1 << 2
    LAUNCH_MONITOR_BALL_DETECTED = 1 << 3
    IS_HEARTBEAT = 1 << 4


class MessageKind(enum.IntEnum):
    """⚠ Derived from three booleans; there is no Type field on the wire
    (protocol §3.4).  Every kind is answered 200."""

    NONE = 0
    SHOT = 1
    HEARTBEAT = 2
    STATUS = 3


class MessageFlag(enum.IntFlag):
    """A message that parsed is DELIVERED, whatever it is missing; the findings
    land here and the application decides (design §4.5)."""

    MISSING_DEVICE_ID = 1 << 0
    MISSING_SHOT_NUMBER = 1 << 1
    SHOT_NUMBER_NOT_INTEGER = 1 << 2  # ⚠ 13.0 is an integer and is NOT flagged
    MISSING_API_VERSION = 1 << 3
    UNKNOWN_API_VERSION = 1 << 4
    MISSING_OPTIONS = 1 << 5
    BALL_FLAG_WITHOUT_OBJECT = 1 << 6
    BALL_OBJECT_WITHOUT_FLAG = 1 << 7
    CLUB_FLAG_WITHOUT_OBJECT = 1 << 8
    CLUB_OBJECT_WITHOUT_FLAG = 1 << 9
    BALL_INCOMPLETE = 1 << 10
    UNKNOWN_UNITS = 1 << 11
    KEY_CASE_MISMATCH = 1 << 12  # "APIVersion" [R10], "Backspin" [MLM]
    UNKNOWN_KEYS = 1 << 13
    SHOT_NUMBER_REPEATED = 1 << 14  # [MLM]'s re-send after a slow reply
    STRING_TRUNCATED = 1 << 15
    TYPE_COERCED = 1 << 16  # a number or boolean arrived as a JSON string [SB]


class EventType(enum.IntEnum):
    NONE = 0
    CONNECTION_OPENED = 1
    CONNECTION_CLOSED = 2
    CLIENT_IDENTIFIED = 3
    SHOT = 4
    HEARTBEAT = 5
    STATUS = 6
    CLIENT_STATE = 7
    PLAYER_INFO_SENT = 8
    SESSION_STATE_SENT = 9
    PROTOCOL_ERROR = 10
    CLOSE_REQUESTED = 11  # ⚠ the HOST should close; the library cannot
    CLIENT_IDLE = 12
    CLIENT_ACTIVE = 13
    WARNING = 14


class CloseCause(enum.IntEnum):
    """Why a connection went away, AS THE HOST REPORTED IT.  The library sees
    no socket and cannot classify on its own."""

    UNKNOWN = 0
    REMOTE_CLOSED = 1
    LOCAL_REQUEST = 2
    TRANSPORT_ERROR = 3
    SERVER_CLOSED = 4


class PlayerInfoReason(enum.IntEnum):
    CHANGED = 0
    ON_CONNECT = 1
    EXPLICIT = 2


class ProtocolErrorReason(enum.IntEnum):
    LEADING_GARBAGE = 0
    BAD_JSON = 1
    TOO_LARGE = 2


class WarningCode(enum.IntEnum):
    NONE = 0
    DEVICE_ID_CHANGED = 1
    STRING_TRUNCATED = 2
    EVENTS_DROPPED = 3
    WIRE_DROPPED = 4
    WRITE_RING_FULL = 5


class WriteKind(enum.IntEnum):
    ACK = 0
    PLAYER_INFO = 1
    READY = 2
    ROUND_ENDED = 3
    FAILURE = 4


class WireDirection(enum.IntEnum):
    CLIENT_TO_SERVER = 0
    SERVER_TO_CLIENT = 1
    META = 2


class WireFlag(enum.IntFlag):
    REDACTED = 1 << 0
    LOST = 1 << 1
    CONTINUES = 1 << 2


class SessionState(enum.IntEnum):
    NONE = 0
    ACTIVE = 1  # → 202, and on connect if policy says so
    ENDED = 2  # → 203


# ---------------------------------------------------------------------------
# Structs
#
# ⚠ FIELD ORDER IS THE LAYOUT.  ctypes lays a Structure out in declaration
# order with the platform's own alignment, so a field moved, renamed or omitted
# here silently re-points every field after it.
# ---------------------------------------------------------------------------
class gsp_allocator(Structure):
    """⚠ The two function pointers are declared as opaque addresses: a binding
    that supplied a Python allocator would have the library call back into the
    interpreter, which is not a contract this library offers."""

    _fields_ = [("alloc", c_void_p), ("free", c_void_p), ("ctx", c_void_p)]


class gsp_ball_data(Structure):
    _fields_ = [
        ("speed", c_double),
        ("spin_axis", c_double),
        ("total_spin", c_double),
        ("back_spin", c_double),
        ("side_spin", c_double),
        ("hla", c_double),
        ("vla", c_double),
        ("carry_distance", c_double),
        ("present", c_uint32),  # BallField bits: has a value
        ("derived", c_uint32),  # BallField bits: came from gsp_ball_data_derive()
    ]


class gsp_club_data(Structure):
    _fields_ = [
        ("speed", c_double),
        ("angle_of_attack", c_double),
        ("face_to_target", c_double),
        ("lie", c_double),
        ("loft", c_double),
        ("path", c_double),
        ("speed_at_impact", c_double),
        ("vertical_face_impact", c_double),
        ("horizontal_face_impact", c_double),
        ("closure_rate", c_double),
        ("present", c_uint32),
        ("reserved", c_uint32),
    ]


class gsp_shot_options(Structure):
    _fields_ = [
        ("contains_ball_data", c_uint8),
        ("contains_club_data", c_uint8),
        ("launch_monitor_is_ready", c_uint8),
        ("launch_monitor_ball_detected", c_uint8),
        ("is_heartbeat", c_uint8),
        ("present", c_uint8),  # OptionField bits: which were on the wire
        ("reserved", c_uint8 * 2),
    ]


class gsp_message(Structure):
    """⚠ ONE MESSAGE TYPE.  Every client→server message has one shape and the
    KIND is derived from ShotDataOptions (design §4.2)."""

    _fields_ = [
        ("conn", gsp_conn_id),
        ("sequence", c_uint32),
        ("host_recv_us", gsp_time_us),
        ("kind", c_uint8),
        ("units", c_uint8),
        ("reserved0", c_uint8 * 2),
        ("flags", c_uint32),
        # ⚠ PROVENANCE ONLY, never a key: it restarts when the client does and
        # is 0 for [R10] heartbeats (protocol §4.3).
        ("shot_number", c_int64),
        ("device_id", c_char * GSP_DEVICE_ID_MAX),
        ("units_text", c_char * GSP_UNITS_TEXT_MAX),
        ("api_version", c_char * GSP_API_VERSION_MAX),
        ("options", gsp_shot_options),
        ("ball", gsp_ball_data),
        ("club", gsp_club_data),
        ("wire_length", c_uint16),
        ("unknown_keys", c_uint16),
        ("reserved1", c_uint8 * 4),
    ]


class gsp_player_info(Structure):
    _fields_ = [
        ("handed", c_uint8),
        ("club", c_uint8),
        ("has_distance", c_uint8),
        ("reserved", c_uint8 * 5),
        # ⚠ [OSP] arms its launch monitor on a NON-ZERO value (protocol §5.2).
        ("distance_to_target", c_double),
        ("surface", c_char * GSP_SURFACE_MAX),
    ]


class gsp_connection_info(Structure):
    _fields_ = [
        ("conn", gsp_conn_id),
        ("identified", c_uint8),
        ("ready", c_uint8),
        ("ball_detected", c_uint8),
        ("reserved0", c_uint8),
        ("opened_us", gsp_time_us),
        ("last_message_us", gsp_time_us),  # GSP_TIME_UNKNOWN until the first
        ("messages", c_uint32),
        ("shots", c_uint32),
        ("protocol_errors", c_uint32),
        ("consecutive_errors", c_uint32),
        ("last_shot_number", c_int64),  # INT64_MIN if none
        # ⚠ Both of these are identifiers (design §9.2).
        ("peer", c_char * GSP_PEER_MAX),
        ("device_id", c_char * GSP_DEVICE_ID_MAX),
    ]


class gsp_connection_event(Structure):
    _fields_ = [
        ("info", gsp_connection_info),
        ("cause", c_uint8),
        ("reserved", c_uint8 * 7),
    ]


class gsp_client_state_event(Structure):
    """Emitted only on CHANGE — [FB] repeats both flags every five seconds."""

    _fields_ = [
        ("ready", c_uint8),
        ("ball_detected", c_uint8),
        ("previous_ready", c_uint8),
        ("previous_ball_detected", c_uint8),
        ("reserved", c_uint8 * 4),
    ]


class gsp_player_info_event(Structure):
    _fields_ = [
        ("player", gsp_player_info),
        ("reason", c_uint8),
        ("reserved", c_uint8 * 7),
    ]


class gsp_session_state_event(Structure):
    _fields_ = [
        ("state", c_uint8),
        ("reason", c_uint8),
        ("reserved", c_uint8 * 6),
    ]


class gsp_protocol_error_event(Structure):
    _fields_ = [
        ("reason", c_uint8),
        ("snippet_length", c_uint8),
        ("reserved", c_uint8 * 2),
        ("discarded_bytes", c_uint32),
        ("consecutive", c_uint32),
        # ⚠ Raw client bytes, and the first thing in a GSPro message is usually
        # its DeviceID — an identifier whatever else it is.
        ("snippet", c_uint8 * GSP_ERROR_SNIPPET_MAX),
    ]


class gsp_warning_event(Structure):
    _fields_ = [
        ("code", c_uint16),
        ("reserved", c_uint16),
        ("value", c_uint32),
        ("text", c_char * GSP_WARNING_TEXT_MAX),
    ]


class gsp_event_payload(Union):
    """⚠ Which member is live is decided by ``gsp_event.type`` and by nothing
    else.  Reading the wrong one returns whatever bytes happen to be there."""

    _fields_ = [
        ("connection", gsp_connection_event),
        ("message", gsp_message),
        ("client_state", gsp_client_state_event),
        ("player_info", gsp_player_info_event),
        ("session_state", gsp_session_state_event),
        ("protocol_error", gsp_protocol_error_event),
        ("warning", gsp_warning_event),
    ]


class gsp_event(Structure):
    """⚠ POD with no pointers, so an event survives being copied, queued,
    logged or moved to another thread.  An event carrying a message carries the
    WHOLE message inline, by value (design §3.4)."""

    _fields_ = [
        ("type", c_uint8),
        ("reserved0", c_uint8 * 3),
        ("sequence", c_uint32),  # ⚠ a gap means the ring dropped
        ("host_time_us", gsp_time_us),
        ("conn", gsp_conn_id),
        ("reserved1", c_uint32),
        ("u", gsp_event_payload),
    ]


class gsp_write_request(Structure):
    """⚠ ONE WRITE PER REQUEST, IN RING ORDER, and every byte is one of five
    message shapes (design §9.1).  There is no send_raw()."""

    _fields_ = [
        ("conn", gsp_conn_id),
        ("length", c_uint16),
        ("kind", c_uint8),
        ("reserved", c_uint8),
        ("data", c_uint8 * GSP_WRITE_MAX),
    ]


class gsp_wire_chunk(Structure):
    _fields_ = [
        ("host_time_us", gsp_time_us),
        ("conn", gsp_conn_id),
        ("length", c_uint16),
        ("direction", c_uint8),
        ("flags", c_uint8),
        ("sequence", c_uint32),
        ("reserved", c_uint32),
        ("data", c_uint8 * GSP_WIRE_CHUNK_MAX),
    ]


class gsp_server_policy(Structure):
    """Every field answers one of protocol §11's unknowns, or chooses between
    two behaviours real clients tolerate.  server.h carries the reasons."""

    _fields_ = [
        ("max_message_bytes", c_uint32),  # 0 → 16 KiB
        ("idle_alarm_us", gsp_time_us),  # 0 → off, and next_due is NEVER
        ("protocol_error_close_threshold", c_uint32),  # 0 → never ask
        ("write_spacing_us", gsp_time_us),  # ⚠ [PIT] dies on {200}{201}
        ("reject_incomplete_shots", c_bool),  # ⚠ OFF: a 501 may loop a client
        ("announce_player_on_connect", c_bool),
        ("announce_ready_on_connect", c_bool),
        ("record_identifiers", c_bool),  # ⚠ OFF: identifiers are redacted
        ("reserved", c_uint8 * 4),
        ("ack_text", c_char * GSP_ACK_TEXT_MAX),  # "" → the documented string
    ]


class gsp_server_config(Structure):
    """Sizes are in ITEMS and 0 takes the default, so the struct stays
    zero-initialisable and a binding need not know the numbers."""

    _fields_ = [
        ("max_connections", c_uint32),
        ("event_ring", c_uint32),
        ("write_ring", c_uint32),  # ⚠ NOT drop-oldest — design §3.4
        ("wire_ring", c_uint32),  # ⚠ 0 → the wire log is OFF
        ("policy", gsp_server_policy),
        ("allocator", gsp_allocator),
    ]


class gsp_abi_sizes(Structure):
    """⚠ Sizes are necessary and nowhere near sufficient — version.h says so.
    Field offsets are pinned separately, by tests/test_python_abi.py."""

    _fields_ = [
        ("abi_version", c_uint32),
        ("message", c_uint32),
        ("ball_data", c_uint32),
        ("club_data", c_uint32),
        ("shot_options", c_uint32),
        ("player_info", c_uint32),
        ("event", c_uint32),
        ("write_request", c_uint32),
        ("wire_chunk", c_uint32),
        ("connection_info", c_uint32),
        ("server_config", c_uint32),
        ("message_layout_version", c_uint32),
    ]


class gsp_response(Structure):
    """One server→client object, decoded — for a test client."""

    _fields_ = [
        ("code", c_int32),
        ("has_message", c_uint8),
        ("has_player", c_uint8),
        ("reserved", c_uint8 * 2),
        ("message", c_char * GSP_RESPONSE_TEXT_MAX),
        ("player", gsp_player_info),
    ]


# ---------------------------------------------------------------------------
# What the ABI test pins
#
# ⚠ BOTH DICTS ARE CHECKED FOR COMPLETENESS, not just for agreement.  A struct
# added to this module and left out of PINNED_STRUCTS would be a layout nobody
# compares against the compiler, which is the same silence the whole file
# exists to avoid.
# ---------------------------------------------------------------------------
PINNED_STRUCTS = {
    "gsp_allocator": gsp_allocator,
    "gsp_ball_data": gsp_ball_data,
    "gsp_club_data": gsp_club_data,
    "gsp_shot_options": gsp_shot_options,
    "gsp_message": gsp_message,
    "gsp_player_info": gsp_player_info,
    "gsp_connection_info": gsp_connection_info,
    "gsp_connection_event": gsp_connection_event,
    "gsp_client_state_event": gsp_client_state_event,
    "gsp_player_info_event": gsp_player_info_event,
    "gsp_session_state_event": gsp_session_state_event,
    "gsp_protocol_error_event": gsp_protocol_error_event,
    "gsp_warning_event": gsp_warning_event,
    "gsp_event": gsp_event,
    "gsp_write_request": gsp_write_request,
    "gsp_wire_chunk": gsp_wire_chunk,
    "gsp_server_policy": gsp_server_policy,
    "gsp_server_config": gsp_server_config,
    "gsp_abi_sizes": gsp_abi_sizes,
    "gsp_response": gsp_response,
}

# python enum -> (C enum name, the prefix its enumerators carry)
PINNED_ENUMS = {
    Status: ("gsp_status", "GSP_"),
    Units: ("gsp_units", "GSP_UNITS_"),
    Handed: ("gsp_handed", "GSP_HANDED_"),
    Club: ("gsp_club", "GSP_CLUB_"),
    ResponseCode: ("gsp_response_code", "GSP_CODE_"),
    BallField: ("gsp_ball_field", "GSP_BALL_"),
    ClubField: ("gsp_club_field", "GSP_CLUB_"),
    OptionField: ("gsp_option_field", "GSP_OPT_"),
    MessageKind: ("gsp_message_kind", "GSP_MSG_"),
    MessageFlag: ("gsp_message_flag", "GSP_MSGF_"),
    EventType: ("gsp_event_type", "GSP_EV_"),
    CloseCause: ("gsp_close_cause", "GSP_CLOSE_"),
    PlayerInfoReason: ("gsp_player_info_reason", "GSP_PI_"),
    ProtocolErrorReason: ("gsp_protocol_error_reason", "GSP_PE_"),
    WarningCode: ("gsp_warning_code", "GSP_WARN_"),
    WriteKind: ("gsp_write_kind", "GSP_WRITE_"),
    WireDirection: ("gsp_wire_direction", "GSP_WIRE_"),
    WireFlag: ("gsp_wire_flag", "GSP_WIRE_"),
    SessionState: ("gsp_session_state", "GSP_SESSION_"),
}

# ⚠ A bound is part of a layout too: a stale GSP_WRITE_MAX here reads a write
# request short.  Module name -> C name.
PINNED_CONSTANTS = {
    "GSP_DEVICE_ID_MAX": GSP_DEVICE_ID_MAX,
    "GSP_UNITS_TEXT_MAX": GSP_UNITS_TEXT_MAX,
    "GSP_API_VERSION_MAX": GSP_API_VERSION_MAX,
    "GSP_SURFACE_MAX": GSP_SURFACE_MAX,
    "GSP_PEER_MAX": GSP_PEER_MAX,
    "GSP_ERROR_SNIPPET_MAX": GSP_ERROR_SNIPPET_MAX,
    "GSP_WARNING_TEXT_MAX": GSP_WARNING_TEXT_MAX,
    "GSP_ACK_TEXT_MAX": GSP_ACK_TEXT_MAX,
    "GSP_WRITE_MAX": GSP_WRITE_MAX,
    "GSP_WIRE_CHUNK_MAX": GSP_WIRE_CHUNK_MAX,
    "GSP_RESPONSE_TEXT_MAX": GSP_RESPONSE_TEXT_MAX,
    "GSP_MESSAGE_LAYOUT_VERSION": GSP_MESSAGE_LAYOUT_VERSION,
    "GSP_ABI_VERSION": GSP_ABI_VERSION,
    "GSP_DEFAULT_PORT": GSP_DEFAULT_PORT,
    "GSP_ALT_PORT": GSP_ALT_PORT,
    "GSP_MAX_CONNECTIONS_DEFAULT": GSP_MAX_CONNECTIONS_DEFAULT,
    "GSP_EVENT_RING_DEFAULT": GSP_EVENT_RING_DEFAULT,
    "GSP_WRITE_RING_DEFAULT": GSP_WRITE_RING_DEFAULT,
    "GSP_WIRE_RING_RECOMMENDED": GSP_WIRE_RING_RECOMMENDED,
    "GSP_CLUB_COUNT": GSP_CLUB_COUNT,
    "GSP_EVENT_TYPE_COUNT": GSP_EVENT_TYPE_COUNT,
    "GSP_WARNING_CODE_COUNT": GSP_WARNING_CODE_COUNT,
}

__all__ = [name for name in dir() if not name.startswith("_")]
