<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# libgspro

A **C11 library that plays the GSPro Connect role**: it listens for golf launch monitors
speaking the GSPro Open Connect v1 protocol and surfaces every shot, heartbeat and status
message they send, with **Python bindings** for driving one from Python.

Almost every launch monitor on the market ships, or has a community bridge that ships, a GSPro
Open Connect client — Garmin R10, Rapsodo MLM2PRO, Square, SkyTrak, Uneekor, Bushnell,
FlightScope, PiTrac. Speaking the server side of that one protocol reaches all of them.

It is **sans-I/O** — it owns no socket, thread, timer or clock. Your code accepts connections
and reads bytes; the library returns decoded messages, the replies to write, and events. That
lets it sit inside a Qt application on `QTcpServer`, a Python program on `asyncio`, or a POSIX
`select` loop, and lets it be tested without a socket.

⚠ **Status: designed, not yet built.** The protocol reference, the design and the public
headers are complete and are the first deliverable; nothing under `src/` exists yet. See
[`docs/design.md` §11](docs/design.md#11-implementation-plan-and-status) for the sequence.

## Documentation

- [`docs/protocol.md`](docs/protocol.md) — the GSPro Open Connect v1 protocol as it is,
  **with the provenance of every claim**. The vendor's page is one short paragraph and one
  JSON example; the rest was established by reading client implementations that are known to
  work against GSPro, and one independent server. Section §11 lists what remains unknown and
  how each unknown will be closed.
- [`docs/design.md`](docs/design.md) — how the library answers that protocol, and why.
- [`include/gspro/`](include/gspro/) — the public API, written from the design. Start with
  [`gspro.h`](include/gspro/gspro.h), then [`server.h`](include/gspro/server.h).

## The integration, in one screen

```c
gsp_server *s;
gsp_server_config cfg = gsp_server_config_default();
gsp_server_create(&cfg, &s);

/* on accept */        gsp_server_on_connection_opened(s, id, peer, now_us());
/* on bytes read */    gsp_server_on_bytes(s, id, buf, n, now_us());       /* ANY length */
/* on socket closed */ gsp_server_on_connection_closed(s, id, GSP_CLOSE_REMOTE_CLOSED, now_us());
/* on club change */   gsp_server_set_player(s, &player, now_us());        /* sends a 201 */

/* then, always: */
gsp_write_request w[8]; gsp_event ev[16]; size_t n;
while ((n = gsp_server_poll_writes(s, w, 8)) > 0)
    for (size_t i = 0; i < n; ++i) write_to(w[i].conn, w[i].data, w[i].length);
while ((n = gsp_server_poll_events(s, ev, 16)) > 0)
    for (size_t i = 0; i < n; ++i)
        if (ev[i].type == GSP_EV_SHOT) handle(&ev[i].u.message);
```

The connection id is yours — any non-zero value you can map back to your socket. Every write
request and event carries it.

## Building

Not yet. When it exists: a C11 compiler and CMake ≥ 3.16, no dependencies, embedded with
`add_subdirectory` or `FetchContent` exactly as [libwrist](../libwrist) is, and consumed as
`#include <gspro/gspro.h>` linking the `gspro` target. The same `GS_BUILD_*` options, the same
`dev` / `san` / `cov` / `rel` / `release` presets, the same purity test that fails the build if
the core ever references a socket, a thread, a timer, a clock or a file.

## Python

`python/gspro/` will be a ctypes binding over `libgspro_ffi`, laid out as `python/wrist/` is,
with an optional `asyncio` transport that is **not** imported by `import gspro`, and two tools:
`gsp_listen.py` (a complete listener) and `gsp_shoot.py` (a launch monitor simulator that sends
fixtures to a libgspro listener — and refuses to aim at a real GSPro).

## Disclaimer

This project is an independent, unofficial work. It is not affiliated with, authorised,
endorsed, sponsored or supported by GSPro or any of its subsidiaries or affiliates.

### Scope: the protocol only

This library implements the **server side** of a published, open, unauthenticated protocol —
GSPro Open Connect v1, documented by the vendor at https://gsprogolf.com/GSProConnectV1.html
for exactly the purpose of letting third-party software interoperate — so that launch
monitors which speak it can send shots to an application of the user's choosing. To be
explicit about what it does not do:

- It does **not** connect to, interoperate with, extend, modify, replace, emulate for the
  purpose of deception, or interfere with the GSPro application itself in any way. It never
  sends anything *to* GSPro.
- It does **not** connect to, authenticate against, query, scrape or otherwise interact with
  GSPro's servers, services, APIs or online accounts.
- It does **not** access, transmit, retrieve or store any GSPro account, subscription, licence
  or user data.
- It does **not** contain, reproduce or redistribute any GSPro application code or other
  proprietary material. The protocol was documented from the vendor's public page and from
  independent open-source client implementations, and every claim's source is recorded in
  [`docs/protocol.md`](docs/protocol.md).
- It does **not** unlock, bypass or circumvent any paid feature, subscription tier or access
  control of GSPro or of any launch monitor.

### Trademarks

"GSPro" and "GSPro Connect" are trademarks of their respective owners, as are the names of
the launch monitors mentioned above. They are used here solely to identify the protocol and
the devices with which this software is designed to interoperate, as permitted by nominative
fair use. Their use does not imply any association, sponsorship or endorsement.

### Warranty

This software is provided "as is", without warranty of any kind. Use is entirely at your own
risk. Please do not contact GSPro or any launch monitor vendor for support with this library;
issues should be raised in this repository.

## Licence

**MIT** — full text in [`LICENSE`](LICENSE), and every source file carries an
`SPDX-License-Identifier: MIT` line. The same reasoning as libwrist: the library is embedded,
sometimes statically into a signed application, and MIT asks only that the copyright notice
travels with the code.
