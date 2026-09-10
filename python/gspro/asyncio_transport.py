# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""An OPTIONAL reference listener on asyncio — layer 3 of design §3.1.

⚠ NOT IMPORTED BY ``import gspro``, and that is a design constraint rather than
tidiness.  The binding must not choose a socket API any more than the C core
does: a host on Qt's QTcpServer, on Winsock, on IOCP or inside PinPoint Studio
embeds the same ``Server`` and writes its own forty lines of this.  Importing
one here would make the choice for all of them.

⚠ THIS FILE IS THE WHOLE OF WHAT A HOST OWES THE LIBRARY, so read it as the
worked example it is.  Four things it does that a host must also do:

  1. Hands ``on_bytes`` whatever ``read()`` returned, of any length, unsplit.
     Not on newlines — a [TNB] message contains them — and not waiting for "a
     whole message", which it cannot recognise (design §3.2.1).
  2. ONE socket write per write request, with TCP_NODELAY, never concatenated.
     Six clients parse one read as one message and [PIT]'s receive thread DIES
     when a 200 and a 201 share a segment (protocol §9.8).
  3. Re-reads ``next_due_us()`` after every call INCLUDING the polls, because a
     write held back by ``write_spacing_us`` becomes due then.
  4. Supplies a MONOTONIC clock and never lets the library see a wall clock.

⚠ What it cannot do is make TCP coalescing impossible.  Nothing can; design
§5.5 says so, and ``write_spacing_us`` only makes it much less likely.
"""

from __future__ import annotations

import asyncio
import socket
import time
from typing import Callable, Optional

from . import _types as T
from .server import Event, Server

__all__ = ["AsyncioTransport", "now_us"]


def now_us() -> int:
    """⚠ MONOTONIC, and the only clock this library ever sees.  A wall clock
    stepped by NTP produces a session whose ordering is a fiction."""
    return time.monotonic_ns() // 1000


class AsyncioTransport:
    """Binds a socket, pumps a :class:`~gspro.server.Server`, and gets out of
    the way.

        server = gspro.Server()
        transport = AsyncioTransport(server, on_event=print)
        await transport.start("127.0.0.1", 921)

    ⚠ THE SERVER IS THE CALLER'S.  This never creates or destroys one, because
    a host that wants to keep reading counters after the listener stops must be
    able to.
    """

    def __init__(
        self,
        server: Server,
        *,
        on_event: Optional[Callable[[Event], None]] = None,
        backlog: int = 8,
    ):
        self._server = server
        self._on_event = on_event
        self._backlog = backlog
        self._listener: asyncio.AbstractServer | None = None
        self._writers: dict[int, asyncio.StreamWriter] = {}
        self._next_conn = 1
        self._timer: asyncio.TimerHandle | None = None
        self._closing = False

    # --- lifecycle --------------------------------------------------------
    @property
    def port(self) -> int:
        """The bound port — ⚠ ask AFTER start(), because 0 means "any"."""
        if self._listener is None or not self._listener.sockets:
            raise RuntimeError("not started")
        return self._listener.sockets[0].getsockname()[1]

    @property
    def connection_count(self) -> int:
        return len(self._writers)

    async def start(self, host: str = "127.0.0.1", port: int = T.GSP_DEFAULT_PORT) -> None:
        """⚠ A bind failure propagates as OSError with the reason intact.  Port
        921 is very often already held — by GSPro itself, on the machine this
        library is most likely to run on (design §6.4) — and a host must report
        that rather than appear to be listening (CT-T09).
        """
        self._listener = await asyncio.start_server(
            self._on_client, host, port, backlog=self._backlog, reuse_address=False
        )
        self._pump()

    async def stop(self) -> None:
        self._closing = True
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        if self._listener is not None:
            self._listener.close()
            await self._listener.wait_closed()
            self._listener = None
        for writer in list(self._writers.values()):
            writer.close()
        for writer in list(self._writers.values()):
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):  # already gone
                pass
        self._writers.clear()

    async def __aenter__(self) -> "AsyncioTransport":
        return self

    async def __aexit__(self, *exc) -> None:
        await self.stop()

    # --- one client -------------------------------------------------------
    async def _on_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        conn = self._next_conn
        self._next_conn += 1

        # ⚠ TCP_NODELAY, or Nagle holds a small reply back for up to 40 ms
        # waiting for company — on the one path where [MLM] is counting to two
        # seconds before it re-sends the shot.
        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:  # a platform that will not have it
                pass

        peer = writer.get_extra_info("peername")
        peer_text = f"{peer[0]}:{peer[1]}" if peer else None

        try:
            self._server.on_connection_opened(conn, peer_text, now_us())
        except Exception:
            # ⚠ GSP_ERR_TOO_MANY_CONNECTIONS lands here.  The host closes the
            # socket, and nothing it sends can tell the client why — the
            # protocol has no message for it (design §5.1).
            writer.close()
            return

        self._writers[conn] = writer
        await self._pump_async()

        cause = T.CloseCause.REMOTE_CLOSED
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                # ⚠ EXACTLY AS IT CAME.  No splitting, no parsing, no waiting.
                status = self._server.on_bytes(conn, data, now_us())
                await self._pump_async()
                while status == T.Status.ERR_QUEUE_FULL:
                    # The write ring filled; the bytes are kept.  Drain and
                    # resume with zero bytes, as server.h documents.
                    status = self._server.on_bytes(conn, b"", now_us())
                    await self._pump_async()
        except (ConnectionError, OSError):
            cause = T.CloseCause.TRANSPORT_ERROR
        finally:
            self._writers.pop(conn, None)
            if not self._closing:
                try:
                    self._server.on_connection_closed(conn, cause, now_us())
                except Exception:  # the server was closed under us
                    pass
                await self._pump_async()
            writer.close()

    # --- the pump ---------------------------------------------------------
    def _pump(self) -> None:
        """Synchronous half: drain the rings and re-arm the timer.

        ⚠ Writes are drained BEFORE events, because a full event ring must cost
        a log line and never an acknowledgement (design §5.4).
        """
        for w in self._server.drain_writes():
            writer = self._writers.get(w.conn)
            if writer is None:  # the client went away between queue and drain
                continue
            # ⚠ ONE write() PER REQUEST.  Concatenating them here would undo
            # what policy.write_spacing_us exists to arrange.
            writer.write(w.data)

        if self._on_event is not None:
            for event in self._server.drain_events():
                self._on_event(event)

        self._arm_timer()

    async def _pump_async(self) -> None:
        self._pump()
        # Flush each connection once the batch is queued.  ⚠ drain() is what
        # gives the kernel the bytes; without it a reply can sit in asyncio's
        # buffer while the client counts to two seconds.
        for writer in list(self._writers.values()):
            try:
                await writer.drain()
            except (ConnectionError, OSError):
                pass

    def _arm_timer(self) -> None:
        """⚠ Re-read after EVERY call, polls included.  The ordinary answer is
        NEVER — the protocol has no deadline of its own — and only the idle
        alarm and write spacing arm anything."""
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        if self._closing:
            return
        due = self._server.next_due_us()
        if due == T.GSP_TIME_NEVER:
            return
        delay = max(0.0, (due - now_us()) / 1_000_000.0)
        loop = asyncio.get_running_loop()
        self._timer = loop.call_later(delay, self._on_timer)

    def _on_timer(self) -> None:
        self._timer = None
        if self._closing:
            return
        self._server.tick(now_us())
        self._pump()
        # A released write still needs flushing; do it off the loop.
        asyncio.get_running_loop().create_task(self._flush())

    async def _flush(self) -> None:
        for writer in list(self._writers.values()):
            try:
                await writer.drain()
            except (ConnectionError, OSError):
                pass

    # --- host → clients ---------------------------------------------------
    async def set_player(self, info: T.gsp_player_info) -> None:
        """The application's club or distance changed; tell every client."""
        self._server.set_player(info, now_us())
        await self._pump_async()

    async def set_session_state(self, state: T.SessionState) -> None:
        self._server.set_session_state(state, now_us())
        await self._pump_async()
