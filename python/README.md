<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# gspro — Python bindings

ctypes bindings for [libgspro](../), the sans-I/O C library that plays the
**GSPro Connect server role**: it listens for launch monitors speaking GSPro
Open Connect v1 and surfaces what they send.

```sh
cmake --preset dev && cmake --build --preset dev
PYTHONPATH=python python3 -c "import gspro; print(gspro.VERSION, gspro.library_path)"
```

The binding loads `libgspro_ffi`, looking in `GSPRO_LIBRARY`, then the repo's
`build/` directories, then beside the package, then the platform loader.

---

## What this is and is not

**The library owns no socket, no thread, no timer and no clock, and the binding
adds none of them.** You bring the socket, you bring the clock, and you run the
loop:

```python
import time, gspro

def now_us():
    return time.monotonic_ns() // 1000      # ⚠ MONOTONIC, never a wall clock

with gspro.Server() as s:
    s.on_connection_opened(conn_id, peer, now_us())
    s.on_bytes(conn_id, data, now_us())     # ⚠ exactly as read(), unsplit
    for w in s.poll_writes():
        sockets[w.conn].sendall(w.data)     # ⚠ ONE write per request
    for ev in s.poll_events():
        print(ev.text)                      # identifiers redacted
```

`gspro.asyncio_transport` is an **optional** reference listener and is *not*
imported by `import gspro`. That is a constraint rather than tidiness: the
binding must not choose a socket API any more than the C core does, because a
host on Qt, Winsock or IOCP embeds the same `Server`.

## The four traps this API refuses to hide

1. **There is no framing.** JSON objects arrive back to back, split or merged
   by TCP as it likes, sometimes indented across lines. Hand `on_bytes`
   whatever the socket returned. Do not split on newlines — a `[TNB]` message
   contains them — and do not wait for "a whole message", which you cannot
   recognise.
2. **Every message gets exactly one reply, promptly.** A heartbeat is "a valid
   shot message" to GSPro. `[MLM]` waits two seconds and then **re-sends the
   shot**, so a late reply is a duplicated shot rather than an error.
3. **Zero is not absence.** Every numeric field carries a presence bit. A clear
   bit means the key was not on the wire; a set bit with `0.0` means the client
   sent zero, and only you know whether that device means it.
4. **One socket write per write request.** Six clients parse one read as one
   message and `[PIT]`'s receive thread *dies* when a 200 and a 201 share a
   segment. `write_spacing_us` helps; concatenating them yourself defeats it.

## Identifiers

`ev.text` is **redacted**. A peer address identifies a household and a
`DeviceID` can carry a hardware serial (`[GC2]` puts one there, and numbers its
shots from the same counter). `ev.format(True)` gives the unredacted line and
`ev.sensitive` says whether there is anything to redact.

## Tools

- `../tools/gsp_listen.py` — a complete listener. What to run with a real
  device on the mat before touching an application.
- `../tools/gsp_shoot.py` — a launch monitor simulator, which replays the
  conformance fixtures in the byte patterns particular connectors produce.
  ⛔ It refuses port 921 on a non-loopback address: that means a real GSPro,
  and driving somebody else's game is out of scope.

## The ABI is checked, not assumed

`_types.py` declares every struct and enum a second time. `gsp_abi_check()`
runs at import and compares eleven struct **sizes** — which is necessary and
nowhere near sufficient, because a field one slot out passes it and returns
plausible numbers. `tests/test_python_abi.py` pins every field offset, every
enumerator and every bound against the compiler, in both directions.
