# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""The transport contract of design §3.2, as Python methods.

Four calls in, three drains out, and a clock the caller owns:

    with gspro.Server() as s:
        s.on_connection_opened(conn_id, peer, now_us())
        s.on_bytes(conn_id, data, now_us())         # ANY length, as it came
        for w in s.poll_writes():
            sockets[w.conn].sendall(w.data)         # one write per request
        for ev in s.poll_events():
            handle(ev)

⚠ THE HOST MUST NOT PRE-SPLIT.  Not on newlines (a [TNB] message contains
them), not "a whole message" (it cannot know).  Hand over whatever the socket
returned, of any length including one byte, and the framer finds the messages
(design §3.2.1).

⚠ ONE THREAD.  Every call on a server must come from the same thread for the
whole life of that server; there are no locks, no atomics and no threads
anywhere in the library.  With asyncio that means the event loop thread.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from typing import Iterator

from . import _types as T
from ._library import check, lib

__all__ = ["Event", "PlayerInfo", "Server", "WriteRequest", "player_info"]


# ---------------------------------------------------------------------------
# What comes out
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class WriteRequest:
    """⚠ ONE SOCKET WRITE PER REQUEST, IN RING ORDER, with TCP_NODELAY.

    Six clients parse one read as one message and [PIT]'s receive thread dies
    outright when two replies share a segment (protocol §9.8).  Concatenating
    these before writing them is the one thing a host must not do.
    """

    conn: int
    kind: T.WriteKind
    data: bytes

    def __len__(self) -> int:
        return len(self.data)


@dataclass(frozen=True)
class Event:
    """⚠ `raw` is a COPY, not a view into the ring — the ring slot is reused as
    soon as the next event lands.  Every payload is POD with no pointers, so
    this object can be queued, logged or handed to another thread freely."""

    type: T.EventType
    sequence: int
    host_time_us: int
    conn: int
    text: str  # redacted; call format(True) for the unredacted line
    sensitive: bool
    raw: T.gsp_event

    def format(self, include_identifiers: bool = False) -> str:
        return _format_event(self.raw, include_identifiers)

    # Convenience accessors for the payload the type selects.  ⚠ Reading the
    # wrong one returns whatever bytes happen to be in the union.
    @property
    def message(self) -> T.gsp_message:
        return self.raw.u.message

    @property
    def connection(self) -> T.gsp_connection_info:
        return self.raw.u.connection.info

    @property
    def warning(self) -> T.gsp_warning_event:
        return self.raw.u.warning

    @property
    def protocol_error(self) -> T.gsp_protocol_error_event:
        return self.raw.u.protocol_error

    @property
    def client_state(self) -> T.gsp_client_state_event:
        return self.raw.u.client_state

    @property
    def player(self) -> T.gsp_player_info:
        return self.raw.u.player_info.player


def _format_event(raw: T.gsp_event, include_identifiers: bool) -> str:
    buf = ctypes.create_string_buffer(1024)
    lib.gsp_event_format(ctypes.byref(raw), buf, len(buf), include_identifiers)
    return buf.value.decode(errors="replace")


# ---------------------------------------------------------------------------
# Player information
# ---------------------------------------------------------------------------
PlayerInfo = T.gsp_player_info


def player_info(
    handed: T.Handed = T.Handed.UNKNOWN,
    club: T.Club = T.Club.UNKNOWN,
    distance_to_target: float | None = None,
    surface: str = "",
) -> T.gsp_player_info:
    """⚠ UNKNOWN is OMITTED from the JSON, never sent as a string GSPro never
    sends, and a player with nothing known sends nothing at all (design §5.6).

    ``distance_to_target=None`` means "not being sent" — which is a different
    thing from zero, and [OSP] arms its launch monitor on a NON-ZERO value.
    """
    p = T.gsp_player_info()
    p.handed = int(handed)
    p.club = int(club)
    p.has_distance = 1 if distance_to_target is not None else 0
    p.distance_to_target = float(distance_to_target or 0.0)
    p.surface = surface.encode()[: T.GSP_SURFACE_MAX - 1]
    return p


# ---------------------------------------------------------------------------
# The server
# ---------------------------------------------------------------------------
class Server:
    """⚠ Capacities only; the library owns every buffer and makes exactly one
    allocation at create and one free at destroy (design §3.4)."""

    __slots__ = ("_handle", "_closed")

    def __init__(
        self,
        *,
        max_connections: int = 0,  # 0 → 4
        event_ring: int = 0,  # 0 → 64
        write_ring: int = 0,  # 0 → 32
        wire_ring: int = 0,  # ⚠ 0 → OFF
        **policy: object,
    ):
        config = lib.gsp_server_config_default()
        config.max_connections = max_connections
        config.event_ring = event_ring
        config.write_ring = write_ring
        config.wire_ring = wire_ring
        for field, value in policy.items():
            if not hasattr(config.policy, field):
                raise TypeError(f"gsp_server_policy has no field {field!r}")
            if field == "ack_text" and isinstance(value, str):
                value = value.encode()[: T.GSP_ACK_TEXT_MAX - 1]
            setattr(config.policy, field, value)

        handle = ctypes.c_void_p()
        check(
            lib.gsp_server_create(ctypes.byref(config), ctypes.byref(handle)),
            "gsp_server_create",
        )
        self._handle = handle
        self._closed = False

    # --- lifetime ---------------------------------------------------------
    def __enter__(self) -> "Server":
        return self

    def __exit__(self, *exc) -> None:
        self.destroy()

    def close(self) -> None:
        """Seals the queues.  ⚠ Every open connection is reported closed first,
        so DRAIN ONCE MORE afterwards to see a complete log (design §3.3)."""
        if not self._closed and self._handle:
            lib.gsp_server_close(self._handle)
            self._closed = True

    def destroy(self) -> None:
        if self._handle:
            lib.gsp_server_destroy(self._handle)
            self._handle = ctypes.c_void_p()
            self._closed = True

    def __del__(self) -> None:
        try:
            self.destroy()
        except Exception:  # pragma: no cover — interpreter teardown
            pass

    # --- IN, from the host's socket code ----------------------------------
    def on_connection_opened(self, conn: int, peer: str | None, now_us: int) -> None:
        """⚠ `peer` is optional and is personal data (design §9.2)."""
        check(
            lib.gsp_server_on_connection_opened(
                self._handle, conn, peer.encode() if peer else None, now_us
            ),
            "gsp_server_on_connection_opened",
        )

    def on_bytes(self, conn: int, data: bytes, now_us: int) -> T.Status:
        """Bytes arrived.  ⚠ Returns a Status rather than raising on
        ``ERR_QUEUE_FULL``, because that one is FLOW CONTROL, not an error: the
        write ring is full, the bytes are kept, and the host drains writes and
        calls again with ``b""`` to resume (design §3.4).  A host that drains
        after every call never sees it.
        """
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data) if data else None
        st = lib.gsp_server_on_bytes(self._handle, conn, buf, len(data), now_us)
        if st == T.Status.ERR_QUEUE_FULL:
            return T.Status.ERR_QUEUE_FULL
        check(st, "gsp_server_on_bytes")
        return T.Status(st)

    def on_connection_closed(
        self, conn: int, cause: T.CloseCause = T.CloseCause.REMOTE_CLOSED, now_us: int = 0
    ) -> None:
        check(
            lib.gsp_server_on_connection_closed(self._handle, conn, int(cause), now_us),
            "gsp_server_on_connection_closed",
        )

    # --- the clock, which the host owns -----------------------------------
    def next_due_us(self) -> int:
        """⚠ ``GSP_TIME_NEVER`` is the ordinary answer: the protocol has no
        deadline of its own.  Re-read after EVERY call, including polls."""
        return lib.gsp_server_next_due_us(self._handle)

    def tick(self, now_us: int) -> None:
        lib.gsp_server_tick(self._handle, now_us)

    # --- host → clients ---------------------------------------------------
    def set_player(self, info: T.gsp_player_info, now_us: int) -> None:
        """Stores it, and queues a 201 to every open connection IF IT DIFFERS.
        An identical value queues nothing, so a host may call this on every UI
        change without spamming clients."""
        check(
            lib.gsp_server_set_player(self._handle, ctypes.byref(info), now_us),
            "gsp_server_set_player",
        )

    def get_player(self) -> T.gsp_player_info:
        out = T.gsp_player_info()
        lib.gsp_server_get_player(self._handle, ctypes.byref(out))
        return out

    def send_player_info(
        self, conn: int, info: T.gsp_player_info | None = None, now_us: int = 0
    ) -> None:
        """One 201, unconditionally, to `conn` or to ``GSP_CONN_ALL``."""
        check(
            lib.gsp_server_send_player_info(
                self._handle, conn, ctypes.byref(info) if info else None, now_us
            ),
            "gsp_server_send_player_info",
        )

    def set_session_state(self, state: T.SessionState, now_us: int) -> None:
        """⚠ ACTIVE queues a 202 and [OSP]-style clients do not arm their
        launch monitor until they have seen one (design §5.6.1)."""
        check(
            lib.gsp_server_set_session_state(self._handle, int(state), now_us),
            "gsp_server_set_session_state",
        )

    def get_session_state(self) -> T.SessionState:
        return T.SessionState(lib.gsp_server_get_session_state(self._handle))

    # --- OUT, drained by the host -----------------------------------------
    def poll_writes(self, max_count: int = 16) -> list[WriteRequest]:
        out = (T.gsp_write_request * max_count)()
        n = lib.gsp_server_poll_writes(self._handle, out, max_count)
        return [
            WriteRequest(
                conn=out[i].conn,
                kind=T.WriteKind(out[i].kind),
                data=bytes(out[i].data[: out[i].length]),
            )
            for i in range(n)
        ]

    def drain_writes(self) -> Iterator[WriteRequest]:
        """Every pending write, in ring order, until there are none."""
        while True:
            batch = self.poll_writes()
            if not batch:
                return
            yield from batch

    def poll_events(self, max_count: int = 32) -> list[Event]:
        out = (T.gsp_event * max_count)()
        n = lib.gsp_server_poll_events(self._handle, out, max_count)
        events = []
        for i in range(n):
            raw = T.gsp_event.from_buffer_copy(out[i])
            try:
                kind = T.EventType(raw.type)
            except ValueError:  # a type this binding does not know
                kind = raw.type
            events.append(
                Event(
                    type=kind,
                    sequence=raw.sequence,
                    host_time_us=raw.host_time_us,
                    conn=raw.conn,
                    text=_format_event(raw, False),
                    sensitive=bool(lib.gsp_event_is_sensitive(ctypes.byref(raw))),
                    raw=raw,
                )
            )
        return events

    def drain_events(self) -> Iterator[Event]:
        while True:
            batch = self.poll_events()
            if not batch:
                return
            yield from batch

    def poll_wire(self, max_count: int = 32) -> list[T.gsp_wire_chunk]:
        out = (T.gsp_wire_chunk * max_count)()
        n = lib.gsp_server_poll_wire(self._handle, out, max_count)
        return [T.gsp_wire_chunk.from_buffer_copy(out[i]) for i in range(n)]

    def drain_wire(self) -> Iterator[T.gsp_wire_chunk]:
        """Every chunk, however many polls that takes.

        ⚠ DRAIN IT EVERY TIME ROUND THE LOOP, not at the end: the wire ring is
        drop-OLDEST (design §3.4), so a host that drains it lazily loses the
        beginning of the session it is recording — and the beginning is where
        the connect handshake and the first shot are.
        """
        while True:
            batch = self.poll_wire()
            if not batch:
                return
            yield from batch

    def dropped_events(self) -> int:
        return lib.gsp_server_dropped_events(self._handle)

    def dropped_wire(self) -> int:
        return lib.gsp_server_dropped_wire(self._handle)

    # --- introspection ----------------------------------------------------
    def connection_count(self) -> int:
        return lib.gsp_server_connection_count(self._handle)

    def connection_ids(self) -> list[int]:
        n = lib.gsp_server_connection_ids(self._handle, None, 0)
        if n == 0:
            return []
        out = (T.gsp_conn_id * n)()
        got = lib.gsp_server_connection_ids(self._handle, out, n)
        return [out[i] for i in range(min(got, n))]

    def connection_info(self, conn: int) -> T.gsp_connection_info:
        out = T.gsp_connection_info()
        check(
            lib.gsp_server_connection_info(self._handle, conn, ctypes.byref(out)),
            f"gsp_server_connection_info({conn})",
        )
        return out
