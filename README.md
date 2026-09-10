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

⚠ **Status: the core, the Python binding, the reference socket transport and the wire log are
built, and every conformance case that can run without a launch monitor passes — but it has
never met one.** All **115 socket-free C cases** are green, and **nine of the ten
host-transport cases** are green **twice over** — once against the asyncio reference transport
and once against the C one — clean under AddressSanitizer and UndefinedBehaviorSanitizer.
Those cases and the 23 byte-exact fixtures behind them are read from the source of sixteen real
launch-monitor clients, not from a wire, so green means the library agrees with what those
clients are *written* to send:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset san && cmake --build --preset san && ctest --preset san
```

Try it against a real device without writing any code — in Python, or in C with no Python on
the machine at all:

```sh
tools/gsp_listen.py --host 0.0.0.0 --port 921 --club PT --distance 4.2
build/dev/tools/gsplisten --host 0.0.0.0 --port 921 --club PT --distance 4.2
```

Capture a session for later, and replay it through the library afterwards:

```sh
build/dev/tools/gsplisten --host 0.0.0.0 --record session.gswire --note "R10 on the mat"
build/dev/tools/gswire replay session.gswire     # what the library makes of it NOW
build/dev/tools/gswire extract session.gswire 3 tests/fixtures/r10_capture.json
```

⚠ **Captures record bytes, not decoded messages**, so when one of the protocol's open questions
is finally settled the session that settled it re-decodes with the fix applied. Peer addresses
and `DeviceID`s are redacted in place unless you ask for them.

Still to come: the first session against real hardware — the one that can close the open
questions in [`docs/protocol.md` §11](docs/protocol.md). See
[`docs/design.md` §11](docs/design.md#11-implementation-plan-and-status) for the sequence.

## Documentation

- [`docs/protocol.md`](docs/protocol.md) — the GSPro Open Connect v1 protocol as it is,
  **with the provenance of every claim**. The vendor's page is one short paragraph and one
  JSON example; the rest was established by reading client implementations that are known to
  work against GSPro, and one independent server. Section §11 lists what remains unknown and
  how each unknown will be closed.
- [`docs/design.md`](docs/design.md) — how the library answers that protocol, and why.
- [`docs/conformance.md`](docs/conformance.md) — what every known client puts on the wire and
  expects back, and the numbered cases that follow from it.
- [`include/gspro/`](include/gspro/) — the public API, written from the design. Start with
  [`gspro.h`](include/gspro/gspro.h), then [`server.h`](include/gspro/server.h).
- [`tests/`](tests/) — the conformance suite, and [`tests/fixtures/`](tests/fixtures/) — one
  byte-exact message per client, with provenance.

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

A C11 compiler and CMake ≥ 3.16. No dependencies.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets `dev`, `san`, `cov`, `rel` and `release` wrap the usual configurations. Embedded with
`add_subdirectory` or `FetchContent` exactly as [libwrist](../libwrist) is, and consumed as
`#include <gspro/gspro.h>` linking the `gspro` target. Adding it to a project changes nothing
about that project: the tests, the FFI object, the reference transport, the recorder, `-Werror`
and the install rules all default to ON when this is the top-level project and OFF when it is
not (`GS_BUILD_TESTS`, `GS_BUILD_FFI`, `GS_BUILD_NET`, `GS_BUILD_RECORD`, `GS_WERROR`,
`GS_INSTALL`).

**If you would rather not write the socket loop**, `GS_BUILD_NET` builds one:
`gspro_net` — `#include <gspro/net.h>`, link `gspro::gspro_net` — is a POSIX/Winsock
`select()` reference transport, and `gsplisten` is a complete listener built on it. ⚠ It is a
*separate* target because linking a socket is the exception: a Qt host drives the same server
from `QTcpServer`, and the purity gate below runs on `gspro`, which this deliberately is not.

```c
gsp_net *net;
gsp_net_config ncfg = gsp_net_config_default();   /* 0.0.0.0:921, TCP_NODELAY on */
char why[GSP_NET_ERROR_MAX];
if (gsp_net_open(s, &ncfg, &net, why, sizeof(why)) < GSP_OK) return complain(why);
for (;;) {
    gsp_net_poll(net, 250);                       /* accept, read, tick, write */
    while ((n = gsp_server_poll_events(s, ev, 16)) > 0)
        for (i = 0; i < n; ++i) handle(&ev[i]);   /* events stay yours to drain */
}
```

`GS_BUILD_RECORD` adds `gspro_record` — `#include <gspro/record.h>`, link
`gspro::gspro_record` — the `.gswire` container, the replay, and the `gswire` tool. ⚠ Also a
separate target: it opens files, and the core may not.

`tests/purity.cmake` fails the build if the core ever references a socket, a thread, a timer,
a clock or a file — the property that makes the library embeddable at all. Both optional
targets are made of exactly what it forbids, which is why they are separate.

## Python

`python/gspro/` is a ctypes binding over `libgspro_ffi`, laid out as `python/wrist/` is,
with an optional `asyncio` transport that is **not** imported by `import gspro`, and two tools:
`gsp_listen.py` (a complete listener, with `--record`) and `gsp_shoot.py` (a launch monitor
simulator that sends fixtures to a libgspro listener — and refuses to aim at a real GSPro).
`gspro.wire` is a second implementation of the `.gswire` container, checked against the C one
in both directions.

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
