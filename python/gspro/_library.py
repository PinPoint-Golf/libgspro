# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""Finding, loading and declaring the shared object.

The library is ``libgspro_ffi`` — one shared object built by the ``gspro_ffi``
target, separate from the static ``gspro`` a C consumer links.

⚠ EVERY PROTOTYPE IS DECLARED.  ctypes defaults an undeclared function to
returning ``int``, which on a 64-bit host silently truncates a pointer or a
``gsp_time_us`` — a crash if you are lucky and a wrong timestamp if you are
not.  A function absent from ``_PROTOTYPES`` below is not callable through this
module, deliberately.
"""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import (
    CDLL,
    POINTER,
    c_bool,
    c_char_p,
    c_double,
    c_int,
    c_size_t,
    c_uint8,
    c_uint32,
    c_void_p,
)
from pathlib import Path

from . import _types as T
from ._types import gsp_conn_id, gsp_time_us

__all__ = [
    "ABI_VERSION",
    "VERSION",
    "AbiMismatch",
    "GsProError",
    "LibraryNotFound",
    "check",
    "lib",
    "library_path",
]


class LibraryNotFound(RuntimeError):
    """The shared object could not be located or loaded."""


class AbiMismatch(RuntimeError):
    """The loaded library was built from different headers than this binding.

    ⚠ Raised at import, which is the whole point: a binding that guesses wrong
    about a struct returns plausible numbers with no error anywhere.
    """


class GsProError(RuntimeError):
    """A library call returned a negative ``gsp_status``."""

    def __init__(self, status: int, what: str = ""):
        try:
            self.status = T.Status(status)
            name = self.status.name
        except ValueError:  # a status this binding does not know
            self.status = status
            name = str(status)
        self.detail = lib.gsp_status_str(status).decode()
        super().__init__(f"{what or 'call'} failed: {self.detail} ({name})")


def check(status: int, what: str = "") -> int:
    """⚠ NEGATIVE IS FAILURE; ``PENDING`` IS NOT AN ERROR.

    Returns the status, so a caller can still tell OK from PENDING.
    """
    if status < T.Status.OK:
        raise GsProError(status, what)
    return status


# ---------------------------------------------------------------------------
# Locating the shared object
# ---------------------------------------------------------------------------
def _candidate_names() -> list[str]:
    if sys.platform == "darwin":
        return ["libgspro_ffi.dylib"]
    if sys.platform == "win32":
        return ["gspro_ffi.dll", "libgspro_ffi.dll"]
    return ["libgspro_ffi.so", "libgspro_ffi.so.0"]


def _search_paths() -> list[Path]:
    """⚠ Ordered so an explicit choice always wins over a discovered one.

    ``GSPRO_LIBRARY`` first, because pointing the binding at a specific build —
    a sanitizer build, a release build, the one under test — is something
    somebody does deliberately and should never have to fight a search order
    for.  A test that discovered ``build/dev`` while checking ``build/san``
    would report on the wrong artefact and pass.
    """
    here = Path(__file__).resolve().parent
    repo = here.parent.parent  # python/gspro/ -> python/ -> repo root
    return [
        here,  # bundled beside the package (a wheel, one day)
        repo / "build" / "dev",
        repo / "build" / "rel",
        repo / "build" / "san",
        repo / "build" / "release",
    ]


def _load() -> tuple[CDLL, str]:
    explicit = os.environ.get("GSPRO_LIBRARY")
    if explicit:
        path = Path(explicit)
        if not path.exists():
            raise LibraryNotFound(f"GSPRO_LIBRARY={explicit} does not exist")
        return CDLL(str(path)), str(path)

    tried: list[str] = []
    for directory in _search_paths():
        for name in _candidate_names():
            path = directory / name
            tried.append(str(path))
            if path.exists():
                return CDLL(str(path)), str(path)

    for name in _candidate_names():  # last resort: the platform loader's path
        try:
            return CDLL(name), name
        except OSError:
            tried.append(f"{name} (loader search path)")

    raise LibraryNotFound(
        "cannot find libgspro_ffi.  Build it with\n"
        "    cmake --preset dev && cmake --build --preset dev\n"
        "or set GSPRO_LIBRARY to its path.  Tried:\n  " + "\n  ".join(tried)
    )


lib, library_path = _load()

# ---------------------------------------------------------------------------
# Prototypes — (restype, [argtypes]) per symbol.  `None` restype is C `void`.
# ---------------------------------------------------------------------------
_server_p = c_void_p  # opaque gsp_server *
_status = c_int  # gsp_status is an enum: int-sized

_PROTOTYPES: dict[str, tuple[object, list]] = {
    # --- version.h ---------------------------------------------------------
    "gsp_version_string": (c_char_p, []),
    "gsp_abi_version": (c_uint32, []),
    "gsp_abi_sizes_get": (None, [POINTER(T.gsp_abi_sizes)]),
    "gsp_abi_check": (_status, [POINTER(T.gsp_abi_sizes)]),
    # --- types.h -----------------------------------------------------------
    "gsp_status_str": (c_char_p, [_status]),
    # --- protocol.h --------------------------------------------------------
    "gsp_units_parse": (c_int, [c_char_p]),
    "gsp_units_text": (c_char_p, [c_int]),
    "gsp_handed_parse": (c_int, [c_char_p]),
    "gsp_handed_text": (c_char_p, [c_int]),
    "gsp_club_parse": (c_int, [c_char_p]),
    "gsp_club_code": (c_char_p, [c_int]),
    "gsp_club_name": (c_char_p, [c_int]),
    # --- message.h ---------------------------------------------------------
    "gsp_ball_data_derive": (_status, [POINTER(T.gsp_ball_data)]),
    "gsp_message_is_shot": (c_bool, [POINTER(T.gsp_message)]),
    "gsp_message_ball_zero_speed": (c_bool, [POINTER(T.gsp_message)]),
    "gsp_message_ball_complete": (c_bool, [POINTER(T.gsp_ball_data)]),
    "gsp_message_format": (
        c_size_t,
        [POINTER(T.gsp_message), c_char_p, c_size_t, c_bool],
    ),
    "gsp_message_flag_name": (c_char_p, [c_int]),
    "gsp_player_info_equal": (
        c_bool,
        [POINTER(T.gsp_player_info), POINTER(T.gsp_player_info)],
    ),
    # --- codec.h -----------------------------------------------------------
    "gsp_frame_find": (
        _status,
        [POINTER(c_uint8), c_size_t, POINTER(c_size_t), POINTER(c_size_t)],
    ),
    "gsp_message_decode": (
        _status,
        [POINTER(c_uint8), c_size_t, POINTER(T.gsp_message)],
    ),
    "gsp_response_encode": (
        _status,
        [
            c_int,
            c_char_p,
            POINTER(T.gsp_player_info),
            c_char_p,
            c_size_t,
            POINTER(c_size_t),
        ],
    ),
    "gsp_message_encode": (
        _status,
        [POINTER(T.gsp_message), c_bool, c_char_p, c_size_t, POINTER(c_size_t)],
    ),
    "gsp_response_decode": (
        _status,
        [POINTER(c_uint8), c_size_t, POINTER(T.gsp_response)],
    ),
    # --- event.h -----------------------------------------------------------
    "gsp_event_type_name": (c_char_p, [c_int]),
    "gsp_event_is_sensitive": (c_bool, [POINTER(T.gsp_event)]),
    "gsp_event_format": (c_size_t, [POINTER(T.gsp_event), c_char_p, c_size_t, c_bool]),
    # --- server.h ----------------------------------------------------------
    "gsp_server_policy_default": (T.gsp_server_policy, []),
    "gsp_server_config_default": (T.gsp_server_config, []),
    "gsp_server_create": (_status, [POINTER(T.gsp_server_config), POINTER(_server_p)]),
    "gsp_server_close": (None, [_server_p]),
    "gsp_server_destroy": (None, [_server_p]),
    "gsp_server_on_connection_opened": (
        _status,
        [_server_p, gsp_conn_id, c_char_p, gsp_time_us],
    ),
    "gsp_server_on_bytes": (
        _status,
        [_server_p, gsp_conn_id, POINTER(c_uint8), c_size_t, gsp_time_us],
    ),
    "gsp_server_on_connection_closed": (
        _status,
        [_server_p, gsp_conn_id, c_int, gsp_time_us],
    ),
    "gsp_server_next_due_us": (gsp_time_us, [_server_p]),
    "gsp_server_tick": (None, [_server_p, gsp_time_us]),
    "gsp_server_set_player": (
        _status,
        [_server_p, POINTER(T.gsp_player_info), gsp_time_us],
    ),
    "gsp_server_get_player": (None, [_server_p, POINTER(T.gsp_player_info)]),
    "gsp_server_send_player_info": (
        _status,
        [_server_p, gsp_conn_id, POINTER(T.gsp_player_info), gsp_time_us],
    ),
    "gsp_server_set_session_state": (_status, [_server_p, c_int, gsp_time_us]),
    "gsp_server_get_session_state": (c_int, [_server_p]),
    "gsp_server_poll_writes": (
        c_size_t,
        [_server_p, POINTER(T.gsp_write_request), c_size_t],
    ),
    "gsp_server_poll_events": (c_size_t, [_server_p, POINTER(T.gsp_event), c_size_t]),
    "gsp_server_poll_wire": (c_size_t, [_server_p, POINTER(T.gsp_wire_chunk), c_size_t]),
    "gsp_server_dropped_events": (c_uint32, [_server_p]),
    "gsp_server_dropped_wire": (c_uint32, [_server_p]),
    "gsp_server_connection_count": (c_size_t, [_server_p]),
    "gsp_server_connection_ids": (c_size_t, [_server_p, POINTER(gsp_conn_id), c_size_t]),
    "gsp_server_connection_info": (
        _status,
        [_server_p, gsp_conn_id, POINTER(T.gsp_connection_info)],
    ),
}

# ⚠ Unused imports would be a lie about what this module needs; c_double is
# here because gsp_player_info carries one and a future prototype will want it.
_ = c_double


def _declare() -> None:
    missing: list[str] = []
    for name, (restype, argtypes) in _PROTOTYPES.items():
        try:
            fn = getattr(lib, name)
        except AttributeError:
            missing.append(name)
            continue
        fn.restype = restype
        fn.argtypes = argtypes
    if missing:
        raise LibraryNotFound(
            f"{library_path} is missing {len(missing)} expected symbol(s): "
            + ", ".join(missing[:8])
            + ("..." if len(missing) > 8 else "")
            + "\n⚠ A partial shared object is a wrong one.  Rebuild the "
            "`gspro_ffi` target."
        )


_declare()


# ---------------------------------------------------------------------------
# The load-time guard
# ---------------------------------------------------------------------------
def _abi_guard() -> None:
    """⚠ FAIL AT IMPORT, NOT AT RANDOM.

    ``gsp_abi_check()`` compares the struct sizes this binding believes in
    against the ones the library was built with.  It is what stops a binding
    compiled against one set of headers quietly misreading a shared object
    built from another.

    ⚠ IT COVERS ELEVEN STRUCTS AND ONLY THEIR SIZES.  Field offsets and enum
    values are pinned separately and at development time, by
    ``tests/test_python_abi.py`` against ``tools/gs_abi_table.c``.  Neither
    check subsumes the other: this one runs on a user's machine against
    whatever they installed, that one runs against the compiler.
    """
    actual = T.gsp_abi_sizes()
    lib.gsp_abi_sizes_get(ctypes.byref(actual))

    expected = T.gsp_abi_sizes(
        abi_version=T.GSP_ABI_VERSION,
        message=ctypes.sizeof(T.gsp_message),
        ball_data=ctypes.sizeof(T.gsp_ball_data),
        club_data=ctypes.sizeof(T.gsp_club_data),
        shot_options=ctypes.sizeof(T.gsp_shot_options),
        player_info=ctypes.sizeof(T.gsp_player_info),
        event=ctypes.sizeof(T.gsp_event),
        write_request=ctypes.sizeof(T.gsp_write_request),
        wire_chunk=ctypes.sizeof(T.gsp_wire_chunk),
        connection_info=ctypes.sizeof(T.gsp_connection_info),
        server_config=ctypes.sizeof(T.gsp_server_config),
        message_layout_version=T.GSP_MESSAGE_LAYOUT_VERSION,
    )

    if lib.gsp_abi_check(ctypes.byref(expected)) != T.Status.OK:
        differences = [
            f"{field}: binding {getattr(expected, field)} "
            f"vs library {getattr(actual, field)}"
            for field, _ctype in T.gsp_abi_sizes._fields_
            if getattr(expected, field) != getattr(actual, field)
        ]
        raise AbiMismatch(
            f"{library_path} was built from different headers than this binding.\n"
            + ("\n".join("  " + d for d in differences) or "  (sizes agree; ABI version differs)")
        )


_abi_guard()

VERSION = lib.gsp_version_string().decode()
ABI_VERSION = lib.gsp_abi_version()
