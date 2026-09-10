# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
"""The ``.gswire`` container, in Python — a SECOND implementation on purpose.

⚠ NOT IMPORTED BY ``import gspro``, exactly as ``asyncio_transport`` is not,
and for the matching reason: the binding's core must choose neither a socket API
nor a storage format.  These two modules mirror the two optional C targets a
consumer does not have to link — ``gspro_net`` and ``gspro_record`` — and a host
that keeps its captures in a database imports neither::

    from gspro.wire import Reader, Writer

⚠ THE POINT OF WRITING IT TWICE.  ``record/gs_record.c`` is the format's other
implementation, and a container defined by exactly one program is a container
defined by that program's bugs.  The two are checked against each other in both
directions: ``tests/test_python_wire.py`` writes here and reads with the C tool,
``tests/test_gsplisten.py`` records with C and reads here.  That is the same
arrangement as ``tests/test_fixtures.py``, which checks the fixtures against the
protocol document without any C at all.

⚠ A CAPTURE IS PERSONAL DATA UNTIL IT IS NOT.  ``identifiers`` in the header
says which kind this file is: ``redacted`` (the default the core writes) or
``recorded``, meaning a peer address and any DeviceID are in it verbatim
(design §9.2).  Check :attr:`Recording.identifiers_recorded` before sharing one.

The layout is in ``include/gspro/record.h`` and is normative there; this file
must follow it rather than the other way round.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator

__all__ = [
    "MAGIC",
    "ENTRY_HEADER",
    "Recording",
    "Chunk",
    "Reader",
    "Writer",
    "read_gswire",
]

MAGIC = b"GSWIRE1"
HEADER_MAX = 1024
ENTRY_HEADER = 24

# ⚠ LITTLE-ENDIAN, EXPLICITLY, and never a native-order struct: the C writer
# spells every integer out byte by byte for the same reason (record.h).  A
# container whose layout depended on the machine that wrote it could not be the
# thing two implementations agree on.
_ENTRY = struct.Struct("<IBBHIIq")


@dataclass
class Recording:
    """The text header, as it was written."""

    library: str = ""
    clock: str = "monotonic_us"
    note: str = ""
    port: int = 0
    identifiers_recorded: bool = False
    #: Keys this version does not know.  ⚠ Kept rather than refused: a capture
    #: written by a LATER version stays readable as far as this one understands
    #: it (protocol §9.7's rule, one register up).
    extra: dict[str, str] = field(default_factory=dict)


@dataclass
class Chunk:
    """One record: the bytes, and everything the core knew about them."""

    sequence: int
    conn: int
    direction: int          # gspro.WireDirection
    flags: int              # gspro.WireFlag
    host_time_us: int
    data: bytes

    @property
    def redacted(self) -> bool:
        """Something was taken OUT of this chunk — not "the file is redacted"."""
        return bool(self.flags & 0x01)

    @property
    def lost(self) -> bool:
        """⚠ Chunks were dropped before this one.  How many is the gap in
        ``sequence``; the flag alone does not say."""
        return bool(self.flags & 0x02)

    @property
    def continues(self) -> bool:
        """The next chunk continues this message."""
        return bool(self.flags & 0x04)

    @classmethod
    def from_struct(cls, raw) -> "Chunk":
        """From a ``gsp_wire_chunk`` as ``Server.poll_wire()`` returns it."""
        return cls(
            sequence=raw.sequence,
            conn=raw.conn,
            direction=raw.direction,
            flags=raw.flags,
            host_time_us=raw.host_time_us,
            data=bytes(bytearray(raw.data[: raw.length])),
        )


def _as_chunk(item) -> Chunk:
    return item if isinstance(item, Chunk) else Chunk.from_struct(item)


class Reader:
    """Reads a capture, header first::

        with Reader("session.gswire") as r:
            print(r.info.note)
            for chunk in r:
                ...

    ⚠ Every refusal below is a REFUSAL rather than a clamp, matching the C
    reader: this is where a corrupt or hostile file meets a buffer, and half a
    message in a capture is worse than none because it looks like evidence.
    """

    def __init__(self, path: str | Path):
        self.path = Path(path)
        self._f = open(self.path, "rb")
        try:
            self.info = self._read_header()
        except Exception:
            self._f.close()
            raise
        self.chunks_read = 0

    # --- header -----------------------------------------------------------
    def _read_header(self) -> Recording:
        magic = self._f.readline()
        if magic.rstrip(b"\r\n") != MAGIC:
            raise ValueError(f"{self.path} is not a .gswire capture")
        info = Recording()
        consumed = len(magic)
        while True:
            line = self._f.readline()
            if not line:
                raise ValueError(f"{self.path}: the header never ended")
            consumed += len(line)
            if consumed > HEADER_MAX:
                raise ValueError(f"{self.path}: the header is longer than {HEADER_MAX} bytes")
            text = line.rstrip(b"\r\n").decode("utf-8", "replace")
            if not text:
                break
            key, _, value = text.partition("=")
            if key == "library":
                info.library = value
            elif key == "clock":
                info.clock = value
            elif key == "note":
                info.note = value
            elif key == "port":
                info.port = int(value or 0)
            elif key == "identifiers":
                info.identifiers_recorded = value == "recorded"
            elif key == "byte_order":
                if value != "little":
                    raise ValueError(f"{self.path}: byte_order={value} is not supported")
            else:
                info.extra[key] = value
        return info

    # --- records ----------------------------------------------------------
    def __iter__(self) -> Iterator[Chunk]:
        while True:
            head = self._f.read(ENTRY_HEADER)
            if not head:
                return
            if len(head) != ENTRY_HEADER:
                raise ValueError(f"{self.path}: the file ends inside a record header")
            length, direction, flags, _reserved, sequence, conn, host_time_us = \
                _ENTRY.unpack(head)
            if length > 512:  # GSP_WIRE_CHUNK_MAX
                raise ValueError(
                    f"{self.path}: a record claims {length} bytes, "
                    "more than GSP_WIRE_CHUNK_MAX"
                )
            data = self._f.read(length)
            if len(data) != length:
                raise ValueError(f"{self.path}: the file ends inside a record payload")
            self.chunks_read += 1
            yield Chunk(sequence, conn, direction, flags, host_time_us, data)

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> "Reader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


class Writer:
    """Writes a capture::

        with Writer("session.gswire", Recording(port=921)) as w:
            w.write(server.poll_wire())

    Accepts :class:`Chunk` values or the ``gsp_wire_chunk`` structures
    ``Server.poll_wire()`` returns, so a Python host records with the same two
    lines the C one does.
    """

    def __init__(self, path: str | Path, info: Recording | None = None):
        self.path = Path(path)
        self.info = info if info is not None else Recording()
        if not self.info.library:
            from . import VERSION  # local: the module must not import the world

            self.info.library = VERSION
        self.chunks = 0
        self.bytes = 0
        self._f = open(self.path, "wb")
        self._write_header()

    @staticmethod
    def _value(text: str) -> str:
        # ⚠ A newline in a value would end the line and turn the rest into a key
        # nobody wrote.  The C writer does the same substitution.
        return text.replace("\n", " ").replace("\r", " ")

    def _write_header(self) -> None:
        out = [MAGIC + b"\n"]
        for key, value in (
            ("library", self.info.library),
            ("clock", self.info.clock),
            ("byte_order", "little"),
            ("identifiers", "recorded" if self.info.identifiers_recorded else "redacted"),
            ("port", str(self.info.port)),
        ):
            out.append(f"{key}={self._value(str(value))}\n".encode("utf-8"))
        if self.info.note:
            out.append(f"note={self._value(self.info.note)}\n".encode("utf-8"))
        out.append(b"\n")
        self._f.write(b"".join(out))
        self._f.flush()

    def write(self, chunks: Iterable) -> None:
        for item in chunks:
            chunk = _as_chunk(item)
            if len(chunk.data) > 512:  # GSP_WIRE_CHUNK_MAX
                # ⚠ Refused before anything is written, as the C writer does.
                raise ValueError("a chunk longer than GSP_WIRE_CHUNK_MAX")
            self._f.write(
                _ENTRY.pack(
                    len(chunk.data),
                    chunk.direction,
                    chunk.flags,
                    0,
                    chunk.sequence,
                    chunk.conn,
                    chunk.host_time_us,
                )
            )
            self._f.write(chunk.data)
            self.chunks += 1
            self.bytes += ENTRY_HEADER + len(chunk.data)
        # ⚠ FLUSHED AFTER EVERY BATCH, as the C recorder does, and for the same
        # reason: a capture is taken ONCE beside hardware that is not coming
        # back, and a process killed with the session still in a buffer loses
        # all of it rather than the last few chunks.  Shots arrive seconds
        # apart; the cost is nothing.
        self._f.flush()

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> "Writer":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def read_gswire(path: str | Path) -> Iterator[Chunk]:
    """Every chunk in ``path``, in order.  The header is on the Reader if it
    matters; this is the two-line form for when it does not."""
    with Reader(path) as reader:
        yield from reader
