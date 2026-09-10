<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# libgspro — library design

This document is the design of `libgspro`, a C11 library that **plays the GSPro Connect role**:
it listens for launch monitors speaking the GSPro Open Connect v1 protocol and surfaces what
they send. It describes the library as it is to be built, and is the working resource a
developer implements against.

It depends on one document and nothing else: [`protocol.md`](protocol.md) — what the protocol
**is**, with the provenance of every claim. [`conformance.md`](conformance.md) is derived from
both: the client behaviour matrix and the test cases, each tied to the clients that demand it. Section references written bare, such as §2 and
§5.2, are sections of *that* document; references written `design §5.3` are sections of this
one.

⚠ **Nothing here has met a real launch monitor yet.** The protocol document is built from the
vendor's page and from clients known to work against GSPro. Every policy in this design that
answers one of protocol §11's unknowns is exposed as a switch rather than baked in, and the
first session against real hardware is expected to flip some of them. §11 says how.

---

## Contents

1. [Scope, and the line it does not cross](#1-scope-and-the-line-it-does-not-cross)
2. [Why C, and why sans-I/O](#2-why-c-and-why-sans-io)
3. [Architecture](#3-architecture)
4. [The type system, and why it is shaped this way](#4-the-type-system-and-why-it-is-shaped-this-way)
5. [The server](#5-the-server)
6. [PinPoint Studio integration](#6-pinpoint-studio-integration)
7. [The wire log](#7-the-wire-log)
8. [The Python exemplar](#8-the-python-exemplar)
9. [Safety, privacy and logging](#9-safety-privacy-and-logging)
10. [Build, test and toolchain](#10-build-test-and-toolchain)
11. [Implementation plan and status](#11-implementation-plan-and-status)
12. [Appendix A — API index](#appendix-a--api-index)

---

## 1. Scope, and the line it does not cross

**In scope: everything between a byte stream and a decoded, validated, timestamped message.**
Framing (§2), decoding with the tolerances real clients need (§9), classification into shot /
heartbeat / status (§3.4), the one-reply-per-message rule (§4.4), the unsolicited player
information message (§5.2), per-connection state, and a byte-level wire log so a session
against a real launch monitor becomes a replayable fixture.

**Out of scope, permanently:**

- **Sockets, threads, timers, clocks, files.** The library owns none of them. §2 of this
  document is why, and `tests/purity.cmake` is how.
- **Unit conversion and sign-convention changes.** §3.5 and §3.6: nobody has stated what the
  conventions are. The library passes every number through unchanged and surfaces the `Units`
  string. Converting into an application's frame is the application's job, in one place it
  controls. A library that converted would encode a guess and hide it.
- **Ball flight, physics, scoring.** The library is not a simulator. It receives shots.
- **Being a client.** The library never connects *to* GSPro. Sending shots to a real GSPro is a
  different program with a different threat model (it drives someone else's game). A test
  client that sends fixtures to *this* library is tooling (§8) and speaks only to it.
  ⚠ That includes **relaying**: a "tee" that listens on 921 and forwards every shot upstream to
  a real GSPro is a client *and* a server, must forward GSPro's 201/202/203 back down to the
  launch monitor to keep its putting mode right, and inherits GSPro's port conflict on the same
  host (§6.4). If it is ever wanted it is a separate, opt-in tool built on this library, not a
  mode of it.
- **Any other simulator protocol.** An E6 Connect emulator, the other protocol most of these
  devices speak, would be a sibling library with the same shape and a second `Kind` in
  PinPoint's connector factory (§6.5) — not a second dialect inside this one.
- **Any other launch-monitor protocol.** Garmin's R10 protocol, Rapsodo's BLE protocol,
  Foresight's CSV are bridged *to* Open Connect by their connectors; that is the whole reason a
  GSPro-role listener receives from everything at once.

### 1.1 What the library is for

PinPoint Studio today reads a GCQuad's `LastShot.CSV`. Every other launch monitor on the market
already ships, or has a community bridge that ships, a GSPro Open Connect client: Garmin R10,
Rapsodo MLM2PRO, Square, SkyTrak, Uneekor, Bushnell, FlightScope, PiTrac. **Speaking the server
side of one protocol reaches all of them**, with no per-device work, on any machine that can
open a TCP port. That is why it is worth a library rather than a class inside one application:
the same property is valuable to anyone building golf software that is not GSPro.

---

## 2. Why C, and why sans-I/O

**C11 with a flat ABI**, exactly as `libwrist`: opaque handles, POD structs, no callbacks, no
allocation the caller did not ask for. It binds from C++, Python, Rust, C#, Swift with no
wrapper generation. §4.6.

**Sans-I/O**, exactly as `libwrist`, and here the reason is stronger than contention. The
consuming application is Qt and will use `QTcpServer`. A Python user will use `asyncio`. A
PiTrac-style embedded user will use POSIX sockets. A Windows service will use Winsock or IOCP.
**The choice of socket API is a platform and framework decision that belongs to the host**, and
a library that made it would be un-embeddable everywhere it was not chosen. So the host owns
the listening socket, the accepted connections, the read loop and the writes, and the library
sees only:

- *a connection opened*, with an id the host chose,
- *bytes arrived on a connection*, with the host's clock reading,
- *a connection closed*,
- *time passed*.

and hands back *bytes to write*, *events to handle*, and *the time it next needs to be told
about*. That is the whole contract (§3.2). It also means the library is tested without a
socket, deterministically, from byte-string fixtures (§10).

**The purity rule is a test, not a promise.** `tests/purity.cmake` inspects the built object's
undefined symbols and fails on `socket`, `bind`, `listen`, `accept`, `recv`, `send`, `select`,
`poll`, `pthread_create`, `clock_gettime`, `fopen`, `getenv` and their Windows equivalents. The
core's entire undefined-symbol set is expected to be `memcpy`, `memmove`, `memset`, `memcmp`,
`strlen`, `snprintf`, `strtod` and the compiler's own hooks.

---

## 3. Architecture

### 3.1 Three layers, and the consumer links one

```
   ┌────────────────────────────────────────────────────────────────┐
   │ 3.  Reference transports — separate targets, nobody must link  │
   │     gspro_net: POSIX / Winsock select loop (C, for the tools)  │
   │     python/gspro/asyncio_transport.py (Python exemplar)        │
   │     PinPoint Studio's QTcpServer adapter (lives in PinPoint)   │
   └────────────────────────────────────────────────────────────────┘
                                  ▲
   ┌────────────────────────────────────────────────────────────────┐
   │ 2.  The transport contract — four calls IN, three drains OUT.  │
   │     design §3.2                                                │
   └────────────────────────────────────────────────────────────────┘
                                  ▲
   ┌────────────────────────────────────────────────────────────────┐
   │ 1.  gspro — the core.  No sockets, no threads, no timers, no   │
   │     clock, no files, no logging it chose itself.               │
   │                                                                │
   │     server ── connections ── framer ── decoder ── validator    │
   │        │                                   │                   │
   │        └── replies (200/201/5XX) ── player info ── wire log    │
   └────────────────────────────────────────────────────────────────┘
```

**Layer 1 is the whole library as far as a consumer is concerned.** PinPoint links it and
implements layer 2 over `QTcpServer`. The reference transports of layer 3 exist so that a
non-Qt user has something that runs out of the box and so that the command line tools and the
Python exemplar have a socket; they are separate targets and linking none of them is the
expected case.

### 3.2 The transport contract

There is no abstract transport class. The host calls in; the library queues out.

```c
/* IN — from the host's socket code, on the server thread */
gsp_status gsp_server_on_connection_opened(gsp_server*, gsp_conn_id, const char *peer, gsp_time_us now_us);
gsp_status gsp_server_on_bytes(gsp_server*, gsp_conn_id, const uint8_t*, size_t, gsp_time_us now_us);
gsp_status gsp_server_on_connection_closed(gsp_server*, gsp_conn_id, gsp_close_cause, gsp_time_us now_us);

/* CLOCK — the host owns the timer */
gsp_time_us gsp_server_next_due_us(const gsp_server*);
void        gsp_server_tick(gsp_server*, gsp_time_us now_us);

/* HOST → CLIENTS — the only thing the application ever originates */
gsp_status gsp_server_set_player(gsp_server*, const gsp_player_info*, gsp_time_us now_us);

/* OUT — drained by the host, never pushed into it */
size_t gsp_server_poll_writes(gsp_server*, gsp_write_request*, size_t);
size_t gsp_server_poll_events(gsp_server*, gsp_event*,         size_t);
size_t gsp_server_poll_wire  (gsp_server*, gsp_wire_chunk*,    size_t);
```

**The connection id is the host's.** The host already has a handle per socket — a
`QTcpSocket*`, a file descriptor, a Python object — and mapping it to a library-issued id would
be a table the host has to keep. So the host passes any non-zero `uint32_t` it can map back to
its socket (a descriptor, an index into its own array, a counter), and every write request and
event carries it. The library refuses an id that is already open (`GSP_ERR_INVALID_STATE`) and
an id it does not know (`GSP_ERR_UNKNOWN_CONNECTION`).

**`peer` is optional and is personal data.** A textual peer address ("192.168.1.40:51022") is
useful in a log and identifies a household. It is stored for `gsp_connection_info`, redacted
by `gsp_event_format()` unless asked for, and never enters the wire log unless
`policy.record_identifiers` is set. §9.

A complete integration, in the shape PinPoint will use:

```cpp
// on QTcpServer::newConnection
auto *sock = server->nextPendingConnection();
uint32_t id = nextId++;                       // any non-zero value the host can map back
sockets[id] = sock;
gsp_server_on_connection_opened(s, id, peerString(sock).toUtf8().constData(), now_us());
connect(sock, &QTcpSocket::readyRead, [=]{
    QByteArray b = sock->readAll();
    gsp_server_on_bytes(s, id, (const uint8_t*)b.constData(), (size_t)b.size(), now_us());
    pump();
});
connect(sock, &QTcpSocket::disconnected, [=]{
    gsp_server_on_connection_closed(s, id, GSP_CLOSE_REMOTE_CLOSED, now_us());
    sockets.remove(id);
    pump();
});
pump();

// on the QTimer
gsp_server_tick(s, now_us());
pump();

static void pump() {
    gsp_write_request w[8];
    size_t n;
    while ((n = gsp_server_poll_writes(s, w, 8)) > 0)
        for (size_t i = 0; i < n; ++i)
            if (auto *sock = sockets.value(w[i].conn)) sock->write((const char*)w[i].data, w[i].length);

    gsp_event ev[16];
    while ((n = gsp_server_poll_events(s, ev, 16)) > 0)
        for (size_t i = 0; i < n; ++i) handle(ev[i]);

    timer->start(msUntil(gsp_server_next_due_us(s)));
}
```

`gsp_server_next_due_us()` must be re-read after every call into the server. It returns
`GSP_TIME_NEVER` when nothing is pending — which is the ordinary case, since the protocol has
no deadline of its own (§4.2, §9.6); only the optional idle alarm (§5.7) arms it.

### 3.2.1 A byte stream in, and why that is the right unit

⚠ **This is the opposite of `libwrist`'s "one call, one notification" rule, and for the
opposite reason.** BLE preserves message boundaries and has no way to resynchronise a stream,
so there the library refuses to reassemble one. TCP preserves *nothing* — §2: a read may hold
a fraction of a message, one, or several — and JSON is self-delimiting, so here the library
**must** reassemble, and can. `gsp_server_on_bytes()` therefore takes whatever the host read,
of any length including one byte, and the framer of §5.3 finds the messages.

The host must not try to help: not split on newlines (a [TNB] message contains them), not
parse first, not wait for "a whole message" (it cannot know). Hand over the bytes as they came.

### 3.3 Poll, don't call back — and the stop barrier

Identical to `libwrist` design §3.3, restated because it is the property that makes the
library safe to embed in a Qt object with a lifetime:

- The library has no thread and no callback, so there is no producer to stop.
- `gsp_server_close()` marks the server closed, seals the three queues, and returns. Nothing
  can be produced afterwards because nothing produces anything except a call the host makes.
- A consumer's barrier is "stop draining", which it implements itself and cannot get wrong.

### 3.4 Memory ownership

1. **`gsp_server_create()` makes exactly one allocation** — the server object with its
   connection table, per-connection receive buffers, and the three rings, sized from the
   config — and `gsp_server_destroy()` makes exactly one free. Nothing allocates after create.
   Supply `gsp_allocator` to route that one allocation through a pool; leave it zero to use
   `malloc`.
2. **Everything else is by value.** Messages, events, write requests and wire chunks are POD
   with no pointers. An event carrying a shot carries the whole `gsp_message` inline (296 bytes;
   the event is 320) rather than a handle, so it can be copied to another thread, logged or queued
   without a lifetime question.
3. **Rings are drop-oldest with a counter**: `gsp_server_dropped_events()`,
   `gsp_server_dropped_wire()`. ⚠ **The write ring is not drop-oldest**: a dropped reply
   re-sends a shot from [MLM] (§4.4), so when the write ring is full the message that would
   have needed a reply is *not consumed* from the receive buffer and `GSP_ERR_QUEUE_FULL` is
   returned to the host, which must drain writes and call `on_bytes` again with zero bytes to
   resume. A host that drains after every call, as the pump above does, never sees this.

**Sizing.** Messages are ~300–900 bytes and arrive seconds apart. The defaults — 4
connections, a 64-event ring, a 32-write ring, 16 KiB receive buffer per connection — are
generous by an order of magnitude and cost ~100 KB. The wire ring defaults to off.

---

## 4. The type system, and why it is shaped this way

### 4.1 Time

Every `..._us` in the API is **microseconds on a clock the caller chooses and the library
never reads**, as in `libwrist`. Monotonic, arbitrary epoch. It enters only through `now_us`
arguments and appears in events as `host_time_us`, and in messages as `host_recv_us` — the
instant the *last byte* of the message arrived, which is the only timestamp the protocol
affords (§8).

⚠ **That timestamp is the shot's only clock, and it is late.** A launch monitor measures a
strike, computes, and sends; the connector may add its own delay. Several hundred milliseconds
between impact and `host_recv_us` is ordinary and unmeasurable from here. PinPoint's shot
pairing (§6) already handles exactly this for the GCQuad, whose CSV is written after the fact,
and treats the arrival time as an upper bound on the impact time, not an estimate of it.

### 4.2 One message type

Every client→server message has one shape (§3), so there is one type, `gsp_message`, and the
*kind* — shot, heartbeat, status — is a field derived from `ShotDataOptions` (§3.4). Three
types would imply a distinction the wire does not make, and a consumer would write the same
provenance-copying code three times.

```c
typedef struct gsp_message {
    gsp_conn_id conn;               /* which connection                        */
    uint32_t    sequence;           /* server-wide, gaps mean drops            */
    gsp_time_us host_recv_us;       /* arrival of the message's last byte      */
    uint8_t     kind;               /* gsp_message_kind                        */
    uint8_t     units;              /* gsp_units, from the string below        */
    uint8_t     reserved0[2];
    uint32_t    flags;              /* gsp_message_flag — validation findings  */
    int64_t     shot_number;        /* the client's counter; provenance only   */
    char        device_id[GSP_DEVICE_ID_MAX];
    char        units_text[GSP_UNITS_TEXT_MAX];
    char        api_version[GSP_API_VERSION_MAX];
    gsp_shot_options options;
    gsp_ball_data    ball;
    gsp_club_data    club;
    uint16_t    wire_length;        /* bytes of JSON this came from            */
    uint16_t    unknown_keys;       /* count of keys nothing here recognised   */
    uint8_t     reserved1[4];
} gsp_message;
```

### 4.3 Presence is a bitmask, and zero is not absence

§3.2 and §3.3: some clients omit a key they did not measure, others send `0.0`, and one sends a
misspelled key that a strict server would drop. So every numeric section carries a `present`
mask alongside its values:

```c
typedef struct gsp_ball_data {
    double speed, spin_axis, total_spin, back_spin, side_spin, hla, vla, carry_distance;
    uint32_t present;               /* gsp_ball_field bits */
} gsp_ball_data;
```

A field whose bit is clear was **not on the wire**; its value is `0.0` and must not be read.
A field whose bit is set and whose value is `0.0` **was on the wire as zero**, and only the
application can decide, from what it knows about the sending device, whether that is a
measurement. The library never promotes a zero to "absent" and never fills an absent field
*on its own*. This is the same rule PinPoint's `LaunchMonitorReading` applies with
`std::optional`, and the mapping in §6 is one `if (present & bit)` per field.

**The one derivation, and it is explicit.** §3.2: `TotalSpin`/`SpinAxis` and
`BackSpin`/`SideSpin` are two representations of one vector, the vendor requires only one, and
two clients agree on the arithmetic between them. `gsp_ball_data_derive()` fills whichever pair
is absent from the one present and sets the same bits in `present` **and** in a second mask,
`derived`, so a consumer reading `present` sees a value and a consumer that cares sees where it
came from. ⚠ It also treats a present-but-**exactly-zero** `TotalSpin` beside a non-zero pair as
a placeholder and recomputes it, because PiTrac — a real device with a large user base — sends
exactly that (§3.2, conformance CT-D13); that is the one place the library reads a zero as
"unmeasured", and it says so in the `derived` mask. The host calls it; the decoder does not — a shot event reports exactly what was on
the wire, and PinPoint calls the helper in its mapping (§6). It is the only arithmetic the
library will ever do on a measurement, because it is an identity, not a convention, and the
sign question (§3.5) is carried unchanged through it.

### 4.4 Numbers pass through, and the conventions travel as documentation

§3.5 and §3.6: no source states the sign conventions or what `Units` governs. The library
stores what arrived, as `double`, and converts nothing. `units` is decoded to an enum for the
two known strings and `units_text` keeps the original for the third one nobody has seen.

**The header states the inferred conventions next to each field, marked unverified**, so a
reader of `message.h` sees "probable: negative = left" beside `spin_axis` rather than having
to find §3.5 — and sees that it is probable rather than known. When U7 closes, the comment
changes and no code does.

### 4.5 Errors, and validation findings

`gsp_status`: non-negative is success, negative is failure, test `st < GSP_OK`. The library
has no error-string channel; anything with detail is an event.

**A message that parsed is delivered, whatever it is missing.** §3.1's "required" fields are
what the vendor asks a client to send, not what a server can insist on without losing data
from a client that does not. So a message with no `DeviceID`, a float `ShotNumber`, an
`APIversion` of `"2"`, or `ContainsBallData: true` beside no `BallData` object is decoded,
delivered, and **flagged** in `gsp_message.flags`:

| Flag | Meaning |
|---|---|
| `GSP_MSGF_MISSING_DEVICE_ID` | no `DeviceID` |
| `GSP_MSGF_MISSING_SHOT_NUMBER` | no `ShotNumber` |
| `GSP_MSGF_SHOT_NUMBER_NOT_INTEGER` | e.g. `13.5`; `shot_number` holds the truncation |
| `GSP_MSGF_MISSING_API_VERSION` / `GSP_MSGF_UNKNOWN_API_VERSION` | absent, or not `"1"` |
| `GSP_MSGF_MISSING_OPTIONS` | no `ShotDataOptions` — kind is then STATUS |
| `GSP_MSGF_BALL_FLAG_WITHOUT_OBJECT` / `GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG` | §3.4's mismatch, both ways |
| `GSP_MSGF_CLUB_FLAG_WITHOUT_OBJECT` / `GSP_MSGF_CLUB_OBJECT_WITHOUT_FLAG` | likewise |
| `GSP_MSGF_BALL_INCOMPLETE` | a shot with ball data lacking one of `Speed, HLA, VLA`, or lacking `TotalSpin` and lacking the `BackSpin`+`SideSpin` pair, or lacking `SpinAxis` with no pair to derive it from |
| `GSP_MSGF_UNKNOWN_UNITS` | `Units` present and neither known string |
| `GSP_MSGF_KEY_CASE_MISMATCH` | a known key matched only case-insensitively (§9.1) — cheap evidence about which connector this is |
| `GSP_MSGF_UNKNOWN_KEYS` | `unknown_keys > 0` |
| `GSP_MSGF_SHOT_NUMBER_REPEATED` | same `ShotNumber` as this connection's previous shot — [MLM]'s re-send after a slow reply looks exactly like this (§4.4) |
| `GSP_MSGF_STRING_TRUNCATED` | a string exceeded its bound (§5.4); the value is the truncation |
| `GSP_MSGF_TYPE_COERCED` | a number or boolean arrived as a JSON string (`"147.5"`, `"true"`) and was converted — one client does this ([SB], not known to work); cheap to accept, worth knowing about |

The application decides what to do with a flagged shot. PinPoint will record the flags in the
reading's provenance and attribute the shot anyway; a stricter host can drop it.
`policy.reject_incomplete_shots` makes the server itself answer `GSP_MSGF_BALL_INCOMPLETE`
with 501 instead of 200 for hosts that want the client told — off by default, because
protocol U2/U8 have not established what GSPro does and a client told 501 may retry forever.

### 4.6 ABI and bindings

As `libwrist` §4.6: opaque handle, POD structs, `gsp_abi_sizes_get()` / `gsp_abi_check()` so a
binding fails at load rather than at random, `reserved` fields in every public struct so the
first additions move nothing, no global state. The Python binding (§8) declares every struct a
second time and `tests/test_python_abi.py` compares sizes, offsets and enumerators both ways,
exactly as `libwrist` learned to.

---

## 5. The server

### 5.1 Many connections, one server

`libgspro` accepts up to `config.max_connections` simultaneous connections (default 4), each
with its own receive buffer, framer state, identity and counters. §1 could not establish what
GSPro does, and the safe choice is the permissive one: a putting camera beside a full-swing
device (an ordinary [MLM] setup), or a connector that reconnects before its old socket has
finished closing, must not be refused. A fifth connection gets `GSP_ERR_TOO_MANY_CONNECTIONS`
from `on_connection_opened` and the host closes it; nothing the library sends tells the client
why, because the protocol has no message for it.

### 5.2 Connection state

```
   OPEN ──first message──▶ IDENTIFIED ──close──▶ (gone)
     └──────────close──────────────────────────▶ (gone)
```

Two states, and the transition exists only to carry `device_id`: §4.1, a client says nothing
about itself until its first message. `GSP_EV_CONNECTION_OPENED` fires with the peer string;
`GSP_EV_CLIENT_IDENTIFIED` fires once with the first `DeviceID` seen; a later message with a
*different* `DeviceID` on the same connection raises `GSP_WARN_DEVICE_ID_CHANGED` rather than
re-identifying, because that is not something a well-behaved client does and a log should say
so.

Per-connection counters — messages, shots, protocol errors, last-message time, current
ready/ball-detected flags — are readable through `gsp_connection_info()` at any time and are
snapshotted into `GSP_EV_CONNECTION_CLOSED`, so a UI's "GSPRO ● 14 shots" pill needs no
bookkeeping of its own.

### 5.3 Framing

The framer turns the byte stream into complete JSON objects. Per connection:

```
state: depth = 0, in_string = false, escape = false, start = -1, buffer[max_message_bytes]
for each byte b:
    if start < 0:                       # between messages
        if b is whitespace: continue
        if b == '{': start = here; depth = 1; continue
        else: protocol error (leading garbage) → resync
    append b
    if in_string:
        if escape: escape = false
        elif b == '\\': escape = true
        elif b == '"': in_string = false
    else:
        if b == '"': in_string = true
        elif b == '{': depth++
        elif b == '}': depth--; if depth == 0: emit buffer[start..here]; start = -1
    if length > max_message_bytes: protocol error (oversize) → resync
```

**String-aware depth counting**, not [KJD]'s bare count, because a `DeviceID` containing a
brace is legal and a client author choosing `"PiTrac {v2}"` would otherwise wedge the framer
forever with no error. **No newline splitting** ([TNB] sends indented JSON). **Whitespace between
objects is skipped** ([MLM] allows spaces). **A `[` at top level is garbage**: the protocol has no
arrays at top level and treating one as a message would be inventing a protocol.

**Resynchronisation.** On either error the buffer is discarded, `GSP_EV_PROTOCOL_ERROR` is
emitted with the reason, the byte count discarded and the first 32 bytes, a 501 is queued
(§5.5), the connection's error counter increments, and scanning resumes at the next `{`.
`policy.protocol_error_close_threshold` (default 0, never) asks the host to close a connection
after that many *consecutive* errors via `GSP_EV_CLOSE_REQUESTED`; the library cannot close
anything itself and a well-formed message resets the count.

**The bound.** `policy.max_message_bytes` defaults to 16 KiB — twenty times the vendor's full
example — and sizes the per-connection buffer at create. [OSG] uses 64 KiB; nothing legitimate
approaches either.

### 5.4 Decoding and validation

The decoder is a purpose-built JSON reader, not a general library: it knows the six object
names and their members, matches keys **case-insensitively** (§9.1), reads every number as
`double` via `strtod` on a bounded copy (§9.3), reads `true`/`false`, reads strings with escape
handling into bounded buffers (truncating with `GSP_WARN_STRING_TRUNCATED`), skips any value it
does not recognise — nested objects and arrays included — and counts it in `unknown_keys`.
Duplicate keys: last wins (§9.1).

It does not need to be a full JSON parser, and deliberately is not: no Unicode validation
beyond passing bytes through, no number grammar beyond what `strtod` accepts, no depth beyond
two. Anything it cannot skip cleanly is a protocol error and resyncs. ⚠ **It must never read
past the buffer**, and it is the one component fuzzing is aimed at (§10.2).

After decoding, classification (§3.4) and the flag pass (§4.5) run, then the message goes to
the event ring and the reply to the write ring — **reply first**, so that a full event ring
drops the event and not the acknowledgement.

**Strings are bounded, not unbounded.** `GSP_DEVICE_ID_MAX` is 64: the longest observed is 15
characters and a `DeviceID` is a label, not a payload. Truncation is flagged, never silent.

### 5.5 Replies: exactly one, promptly, whatever it was

§4.2 and §4.4 fix this: every complete, parseable object receives **one** reply, queued
**before** `on_bytes` returns, so a host that drains writes after every call has it on the
socket within its own event-loop latency — far inside [MLM]'s 2 s re-send.

| Received | Reply |
|---|---|
| Any well-formed object (shot, heartbeat, status, anything with braces that parsed) | `{"Code":200,"Message":"<policy.ack_message>"}` |
| Shot with `GSP_MSGF_BALL_INCOMPLETE` **and** `policy.reject_incomplete_shots` | `{"Code":501,"Message":"Incomplete ball data"}` |
| Unparseable object, leading garbage, top-level array | `{"Code":501,"Message":"Bad JSON data"}` |
| Oversize | `{"Code":501,"Message":"Payload too large"}` — 501 not [OSG]'s 413, because [GSP] defines only the 5XX space and [TNB] tests `>= 500` |

`policy.ack_message` defaults to `"Shot received successfully"`, the string [GSP] lists.
⚠ [TNB] says the real GSPro string is misspelled (U4). No client keys on the text, so the
default stays the documented spelling; a host that captures the real one can set it.

**There is no other reply.** Nothing else in the protocol is a request. The two remaining
messages the server sends — 201 and the 202/203 pair — are unsolicited and are the host's to
originate (§5.6).

**Spacing writes for fragile readers.** §9.8: six clients parse one read as one message, and
PiTrac's receive thread *dies* when a 200 and a 201 share a segment. The host issues one socket
write per `gsp_write_request` (server.h says so) with `TCP_NODELAY`, which on loopback is
usually enough; `policy.write_spacing_us` (default 0) additionally holds a write to a
connection back until that long after the previous write to it was polled, using the same
`next_due_us` / `tick` mechanism as the idle alarm. 20 ms costs nothing a golfer can notice and
gives a blocked `read()` on the far side time to return. It cannot make coalescing impossible;
nothing can, and the doc says so.

### 5.6 Player information

The application tells the server what the "game" state is — which hand, which club, how far
to the target — and the server tells the clients:

```c
typedef struct gsp_player_info {
    uint8_t handed;             /* gsp_handed: RH / LH / UNKNOWN */
    uint8_t club;               /* gsp_club:   DR … PT / UNKNOWN */
    uint8_t has_distance;       /* DistanceToTarget is meaningful  */
    uint8_t reserved;
    double  distance_to_target; /* in the units the host chose; not converted */
} gsp_player_info;

gsp_status gsp_server_set_player(gsp_server*, const gsp_player_info*, gsp_time_us now_us);
```

`set_player` stores the value and, **if it differs from the stored one**, queues a 201 to every
open connection. If `policy.announce_player_on_connect` is set (default: true) the stored value
is also queued to each new connection immediately after `on_connection_opened`. An `UNKNOWN`
handed or club is omitted from the JSON rather than sent as a string GSPro never sends; a
player with both unknown and no distance sends nothing at all.

`gsp_server_send_player_info(s, conn, info, now)` sends one unconditionally to one connection
(or `GSP_CONN_ALL`) — for a host that wants to re-announce, or to test a client.

`surface` is the `Surface` string [OSP] declares (§5.2): sent when non-empty, vocabulary
unknown (U12), and PinPoint leaves it empty.

### 5.6.1 Session state: the 202/203 pair

§5.1: GSPro sends `{"Code":202,"Message":"GSPro ready"}` when a match or hole starts and
`{"Code":203,"Message":"GSPro round ended"}` when it ends, and ⚠ **[OSP]-style clients do not
arm their launch monitor until they have seen a 202**. A server that emulates GSPro without
them leaves such a client waiting for a game that never starts. So:

```c
typedef enum gsp_session_state { GSP_SESSION_NONE, GSP_SESSION_ACTIVE, GSP_SESSION_ENDED } gsp_session_state;
gsp_status gsp_server_set_session_state(gsp_server*, gsp_session_state, gsp_time_us now_us);
```

`ACTIVE` queues a 202 to every connection; `ENDED` queues a 203; a repeated value queues
nothing. With `policy.announce_ready_on_connect` (default: true) a connection opened while the
state is `ACTIVE` receives a 202 immediately after its 201. The strings are fixed and are the
ones [OSP] matches exactly. PinPoint maps session start and stop onto this (§6).

⚠ **The 201 rides the same write ring as the acknowledgements, in order.** After a shot the
application typically changes club or distance, so the client sees `{200}{201}` in one read —
which is what [MLM]'s splitter exists for (§5.2) and is exactly the concatenation a client is
known to handle. A client that cannot split will drop the 201, which is its bug and was
already its bug against GSPro.

### 5.7 Timers

The protocol has no deadline (§9.6), so **the default server never arms a timer** and
`next_due_us()` returns `GSP_TIME_NEVER`. One optional alarm exists:

- `policy.idle_alarm_us` (default 0, off): emit `GSP_EV_CLIENT_IDLE` once when a connection
  has sent nothing for this long, and `GSP_EV_CLIENT_ACTIVE` when it next does. For a UI that
  wants to grey out a launch monitor that has gone quiet. It **never** closes anything.

`gsp_server_tick()` with no alarm armed is a no-op and may be called at any rate.

---

## 6. PinPoint Studio integration

PinPoint already has the shape this drops into: `LaunchMonitorBase` (a connector notices a
reading and says so), `LaunchMonitorReading` (every field optional, `lm.`-prefixed metrics),
`ShotPairing` (which swing a reading belongs to), and a factory with `Kind::None` and
`Kind::GcQuad`. The integration is **one new `Kind::GsPro`** and one class.

**`GsProMonitor : LaunchMonitorBase`** owns a `QTcpServer`, a `gsp_server*`, a map from
connection id to `QTcpSocket*`, and one `QTimer`; it is the layer-3 transport of §3.1 and the
code in §3.2 is most of it. `setSourcePath()` is repurposed to carry `"0.0.0.0:921"` or a bare
port, as the base's comment already anticipates ("every connector we can foresee is either a
watched path or a polled endpoint"). `state()` maps: no listener → `Error` with the bind
failure text; listening, no client → `Waiting`; a client identified → `Ready`.

**Mapping `gsp_message` → `LaunchMonitorReading`**, one line per field, gated on the presence
bit (§4.3), and converted from the wire's units to the reading's (mph, yards, degrees, rpm) in
this one place — under `Units: "Yards"` that is the identity; under `"Meters"` the carry and
distance convert and, until U3 closes, the speeds are *assumed* mph and the assumption is
written in the code:

| `gsp_message` | `LaunchMonitorReading` | Note |
|---|---|---|
| `shot_number` | `deviceShotId` | provenance only, as the GCQuad's |
| `device_id` | `sourcePath` / log | the connector's name |
| `"gspro"` | `deviceKind` | |
| `host_recv_us` → wall clock | `readAtMs` | the transport converts, as `GcQuadMonitor` does |
| `ball.speed` | `ballSpeed` | |
| `ball.spin_axis` | `spinAxis` | ⚠ sign convention §3.5 — verify before grading anything on it |
| `ball.total_spin` | `spinRate` | |
| `ball.back_spin`, `ball.side_spin` | `backSpin`, `sideSpin` | |
| `ball.hla`, `ball.vla` | `launchDirection`, `launchAngle` | |
| `ball.carry_distance` | `carryDistance` | |
| `club.speed` | `clubheadSpeed` | |
| `club.angle_of_attack` | `attackAngle` | |
| `club.face_to_target` | `faceAngle` | |
| `club.path` | `clubPath` | |
| `club.lie` | `lieAngle` | |
| `club.loft` | `dynamicLoft` | |
| `club.vertical_face_impact` | `strikeHeight` | unit unstated on the wire; assume mm |
| `club.horizontal_face_impact` | `strikeLocation` | ⚠ [MLM] puts face-to-path here (§3.3) |
| `club.closure_rate` | `closureRate` | |
| *(nothing)* | `deviceClub` | ⚠ the wire carries no club; leave empty |

`faceToPath` is derivable as `face_to_target − path` when both are present; that is a choice
for the reading, made where the other derived fields (`spinLoft`, `smashFactor`) are.

**Club selection flows the other way.** PinPoint's own club selection, the thing that resolves
a normative corridor, becomes `gsp_server_set_player()` — so a connector that switches to
putting mode on `"PT"` does so when the coach selects the putter in PinPoint, exactly as it
would in GSPro. Handedness comes from the athlete profile. `DistanceToTarget` has no PinPoint
meaning today and is left unset.

**Two things the base class rules already settle.** A connector must not emit for data that
existed before it started: not a concern here, since nothing exists before a client connects.
And `readingAvailable` is emitted on the GUI thread: the `QTcpServer` lives there, so it is.

**Session state flows the same way.** Starting a PinPoint session calls
`gsp_server_set_session_state(ACTIVE)` and stopping it `ENDED`, so a connector that waits for
"GSPro ready" before arming its device (§5.6.1) arms when the coach presses start.

**Derivation happens in the mapping.** `GsProMonitor` calls `gsp_ball_data_derive()` on a copy
of the ball data before filling the reading, so a device that sent only `BackSpin`/`SideSpin`
still yields `spinRate` and `spinAxis`, and the reading's provenance records the `derived` mask.

**Settings.** The GSPRO link panel the UX design already lists (Settings → Devices → GSPRO
link, with an on/off badge) gets a port, an interface (all / loopback), and the connection pill
text from `gsp_connection_info()`.

### 6.1 Bind address and the firewall

**Bind all interfaces by default, not loopback.** [GSP] documents 127.0.0.1 because it assumes
the connector runs on the GSPro PC, but the useful case for a coaching studio is the opposite: a
Rapsodo or Garmin bridge on a phone or a second laptop, a PiTrac on the network. `QTcpServer`
binds `QHostAddress::Any` and the settings panel offers loopback as the restrictive option.
⚠ **On Windows an inbound rule is needed** for the PinPoint executable on TCP 921, or every
connector on another machine fails silently with "connection refused"; the installer adds it
and the settings panel says whether it is present. The port is open and unauthenticated
(§9.3), which is a reason to make the loopback option easy to find, not a reason to default to it.

### 6.4 Coexistence with GSPro

The goal is for launch monitors to talk to PinPoint **instead of** GSPro, and that case is
simple: PinPoint listens on 921, the user points the connector at PinPoint's address, done —
every connector examined lets the user set the address and port. Two other cases exist:

| Case | What happens | Answer |
|---|---|---|
| GSPro and PinPoint on **different** machines | No conflict. The connector is pointed at one or the other | Supported; the ordinary studio layout |
| GSPro and PinPoint on the **same** machine, both wanting 921 | A bind conflict. GSPro Connect can be moved to **922 and nowhere else**, by setting `<OpenAPIUseAltPort>true</OpenAPIUseAltPort>` in `C:\GSPro\GSPC\GSPconnect.exe.config` ([SLX]) | Either side moves: GSPro to 922 with that setting, or PinPoint to any port. Every connector examined has a port setting. `GSP_ALT_PORT` names 922 so a UI can offer it |
| Capture in PinPoint **while playing** GSPro | Needs a relay: PinPoint receives and forwards up, forwards 201/202/203 back down | Out of scope for the library (§1). If wanted, a separate opt-in tool. Not designed here |

So the library needs nothing for coexistence beyond a configurable port, which the host already
owns; the design decision is that PinPoint's default is 921, its settings panel makes changing
it obvious, and its help text names GSPro's own 922 switch for the shared-machine case.

### 6.5 The connector interface

`LaunchMonitorBase` is already the interface the note asks for: `GsProMonitor` is one `Kind`
beside `GcQuad`, and a future E6 Connect emulator (§1) is a third. Nothing about the shot
pipeline knows which protocol produced a reading, and that stays true.

**Build.** Embedded exactly as `libwrist`: `FetchContent` with a sibling `../libgspro` checkout
winning over GitHub, `GS_BUILD_*` options defaulting off when not top-level, static, provenance
string in the About box. The `CMakeLists.txt` block is a copy of the `libwrist` one with the
names changed, and that is deliberate.

---

## 7. The wire log

**Record the bytes, not the decoded messages**, for the same reason `libwrist` does: protocol
§11 has ten open questions, and when U2, U4 or U7 is answered a byte-level capture of the
session that answered it re-decodes with the fix applied, while a decoded log has already
thrown away what the fix would have read differently. The first capture against a real
launch monitor is the fixture that pins the decoder.

`gsp_server_poll_wire()` drains `gsp_wire_chunk` values: direction (client→server,
server→client, meta for open/close), connection id, host time, sequence, and up to
`GSP_WIRE_CHUNK_MAX` bytes with a `CONTINUES` flag for longer messages. The core buffers into
the ring the config sized (default off); writing chunks anywhere is the host's business, or the
optional `gspro_record` module's, which defines a `.gswire` container and a replay that drives
a recorded session back through a server. The container format is the `libwrist` `.wrwire`
shape with a different magic and a connection-id field; §11 sequences it after the core.

⚠ **Peer addresses and `DeviceID` strings are redacted from the wire log by default**
(`policy.record_identifiers`), and a chunk that was redacted says so with `GSP_WIRE_REDACTED`.
§9.

---

## 8. The Python exemplar

`python/gspro/` is a ctypes binding over `libgspro_ffi`, laid out exactly as `python/wrist/`:
`_library.py` (find and load, every prototype declared), `_types.py` (every struct and enum a
second time, pinned by `tests/test_python_abi.py`), `server.py` (the `Server` class: the
contract of §3.2 as methods returning lists), and an **optional** `asyncio_transport.py` that
runs a real listener with `asyncio.start_server` and pumps a `Server` — not imported by
`import gspro`, because the binding must not choose a socket API either.

Two tools ship with it and are meant to be copied:

- `tools/gsp_listen.py` — a complete listener: binds, prints every event, sets a player from
  the command line. What a PinPoint developer runs to check a launch monitor before touching
  the app.
- `tools/gsp_shoot.py` — a **launch monitor simulator**: connects to a listener and sends
  shots from JSON fixtures, optionally as the byte patterns of specific connectors (indented
  like [TNB], `Backspin` like [MLM], one byte at a time, two shots in one write). It speaks
  only to `libgspro` listeners and refuses port 921 on a non-loopback address by default, so
  nobody points it at a real GSPro by accident (§1: the library is not a client).

The `.gswire` reader is in Python too, so a capture from a real device replays through the
binding and the C tests alike.

---

## 9. Safety, privacy and logging

### 9.1 What the library can send

Five message shapes, composed by the library, from a fixed set of codes: 200, 201, 202, 203,
501. There is no `send_raw()`. A host that wants to send something else to a client is asking
for a different library. The test suite asserts that every byte sequence the write ring can
produce parses as one of the five.

### 9.2 Identifiers

Two things in this protocol identify a person or a household: the **peer address** the host
supplied, and the **`DeviceID`**, which for a user-configured connector ([MLM], [TL]) may be
whatever the user typed. Neither is written to the wire log or to `gsp_event_format()` output
unless asked (`policy.record_identifiers`, `include_identifiers`), and `gsp_event_is_sensitive()`
is true for exactly the events carrying them. A capture taken to answer §11's questions must be
redacted before it becomes a fixture; the `.gitignore` refuses `*.gswire` outside
`tests/fixtures/` for that reason.

### 9.3 Untrusted input

Every byte on the socket is untrusted: the port is open, unauthenticated, possibly on a LAN.
The framer bounds its buffer, the decoder bounds every string and never reads past the message,
and the server bounds connections. A hostile client can at worst occupy its own connection
with garbage, be answered 501 each time, and — if the host sets the threshold — be asked to be
closed. It cannot affect another connection, allocate memory, or make the library call
anything.

### 9.4 Unknown fields and unknown clients

Unknown keys are counted and skipped, never fatal (§9.7). An unknown `APIversion` is flagged
and delivered. A `DeviceID` nobody has seen is just a string. The library does not gate on
who is talking; it gates on whether what they sent parses.

---

## 10. Build, test and toolchain

### 10.1 Toolchain

As `libwrist` §10.1, verbatim in intent: C11, no extensions, CMake ≥ 3.16, no dependencies,
the same warning set with `-Werror` when top-level, ASan/UBSan and coverage options, presets
`dev`/`san`/`cov`/`rel`/`release`, install rules and a CMake package, CI on Linux gcc and
clang, Linux sanitizers, macOS, Windows MSVC. Options are `GS_BUILD_TESTS`, `GS_BUILD_TOOLS`,
`GS_BUILD_FFI`, `GS_BUILD_NET` (the reference socket transport), `GS_BUILD_RECORD`, `GS_WERROR`,
`GS_INSTALL`, defaulting to the top-level check as `libwrist`'s do.

`-Wswitch-enum` stays deliberate: every switch over `gsp_event_type`, `gsp_message_kind`,
`gsp_club` must name every case.

### 10.2 What the tests are for

Every test traces to a numbered claim in `protocol.md` or here, and is enumerated as a `CT-`
row in [`conformance.md`](conformance.md) §3 with the clients that demand it. The categories:

| Area | Pins | Shape |
|---|---|---|
| Framer | §2, design §5.3 | Every fixture delivered byte-by-byte, whole, two-in-one, with whitespace between, with a brace in a string; a garbage prefix resyncs; an oversize message resyncs; the [TNB] indented form |
| Decoder | §3, §9.1, §9.3, design §5.4 | Each source's exact payload shape ([GSP] full, [TL] minimal, [MLM] `Backspin`, [R10] `APIVersion`, [OSG] float shot number, [OSP] nulls-omitted with club `Speed` only, [OCR] putt placeholders, [GC2] serial in `DeviceID`) decodes to the expected presence mask and flags; every truncation of every fixture under ASan |
| Derivation | §3.2, design §4.3 | Each pair from the other, both masks set, the ±90° edge, and that [GSP]'s own example is reported inconsistent rather than "fixed" |
| Classification | §3.4 | The four rows of the kind table |
| Replies | §4.2, §4.4, design §5.5 | Exactly one write per object, queued before `on_bytes` returns, for every kind; the three 501 cases; the write-ring-full path does not drop |
| Player info | §5.2, design §5.6 | `set_player` sends only on change; announce-on-connect; unknown fields omitted; the `{200}{201}` ordering after a shot |
| Session state | §5.1, design §5.6.1 | 202/203 with the exact strings; only on change; on connect while active, after the 201 |
| Connections | design §5.1, §5.2 | Limit, duplicate id, unknown id, identify-once, `DEVICE_ID_CHANGED`, counters in the close event |
| Host transport | conformance §3.8 | `gsp_shoot.py` against PinPoint's adapter, the asyncio transport and the C reference transport: reply latency, one write per message, spacing, a second machine, two clients |
| Purity | design §2 | `tests/purity.cmake` on the core object |
| ABI | design §4.6 | Sizes, offsets and enumerators against the Python binding, both ways |
| Emitted bytes | design §9.1 | Every write the ring can produce round-trips through the decoder's response reader as 200/201/501 |

**Golden fixtures are hand-written from `protocol.md`**, one per source, named for the source
and the section: `tests/fixtures/gsp_full.json`, `tl_minimal.json`, `mlm_backspin.json`,
`r10_heartbeat.json`, `osg_float_shot_number.json`, `tnb_indented.json`, `osp_connect_heartbeat.json`, `osp_shot.json`,
`ocr_putt.json`, `gc2_shot.json`. They are the byte
patterns the sources emit, reconstructed from their serialisation code — not copied files.

### 10.3 Testing without a socket

The whole suite runs on `gsp_server_on_bytes()` with byte strings and a synthetic clock. A
socket appears only in the tools and the Python transport test, which opens a loopback
listener with `asyncio` and drives `gsp_shoot.py` at it.

---

## 11. Implementation plan and status

⚠ **Status: the core is built and the whole conformance suite is green** — 103 cases, clean
under `--preset dev` and `--preset san`. ⚠ **It has still never met a launch monitor**, which
is package 7 and the only thing that can close protocol §11's unknowns. This document and
`protocol.md` were the first deliverable and the public headers the second, written *from* this
design so that the API was reviewable before any of it ran.

⚠ **The conformance suite came before the library, deliberately** (package 1b below). Every
package after it is finished when its `CT-` rows go green, and `src/gs_unimplemented.c` was
what let the suite build and run in the meantime. That file was deleted and the discovered
source list in `CMakeLists.txt` replaced with a literal one when package 3 landed — a
discovered list silently builds yesterday's library, because CMake does not re-run when a file
appears.

| # | Package | Delivers | Depends on |
|---|---|---|---|
| 1 | Headers | `include/gspro/*.h`, exactly the API of Appendix A, compiling under `-Werror` with an empty `src/` | — |
| 1b | **Conformance suite** ✅ | `tests/` — 103 cases, 23 byte-exact fixtures, the sans-I/O gate, an independent Python fixture cross-check, and the CMake that builds and runs them | 1 |
| 2 | **Framer + decoder** ✅ | `src/gs_frame.c`, `src/gs_decode.c`, `src/gs_encode.c`, `src/gs_misc.c` — turns CT-D, CT-K and the API family green, and CT-F apart from the two rows that drive a server | 1b |
| 3 | **Server** ✅ | `src/gs_server.c`: connections, replies, player info, session state, events, idle alarm — turns CT-R, CT-P, CT-C, CT-X and the rest of CT-F green | 2 |
| 4 | **FFI + Python** ✅ | `gspro_ffi` target, `python/gspro/`, `tools/gs_abi_table.c` + `tests/test_python_abi.py`, `gsp_listen.py`, `gsp_shoot.py`, asyncio transport — and nine of the ten CT-T rows, which had nothing to run against until there was a socket | 3 |
| 5 | Reference net transport + tool | `gspro_net` (POSIX/Winsock), `gsplisten` CLI | 3 |
| 6 | Wire log + record | `poll_wire`, `gspro_record`, `.gswire`, replay | 3 |
| 7 | **First contact** | A session against at least one real connector ([MLM] or [R10] with its device, or PiTrac) captured to `.gswire`; §11 of the protocol document updated with what was learned; fixtures promoted from the capture | 4 or 5, 6 |
| 8 | PinPoint Studio | `Kind::GsPro`, `GsProMonitor`, the mapping of §6, the settings panel, the CMake block | 3, and 7 for confidence |

Package 7 is the one that matters and the one that cannot be done at a desk. Everything before
it is arranged so that when a launch monitor is finally on the mat, the session is spent
answering U1–U10 rather than debugging framing.

---

## Appendix A — API index

| Header | Contents |
|---|---|
| `gspro/gspro.h` | Umbrella |
| `gspro/types.h` | `gsp_status`, `gsp_time_us`, `gsp_conn_id`, `gsp_allocator`, export macro |
| `gspro/version.h` | Version, `gsp_abi_sizes`, `gsp_abi_check()` |
| `gspro/protocol.h` | Port, key names, response codes, `gsp_units`, `gsp_handed`, `gsp_club` and their string forms |
| `gspro/message.h` | `gsp_message`, `gsp_ball_data`, `gsp_club_data`, `gsp_shot_options`, presence bits, validation flags, `gsp_player_info` |
| `gspro/codec.h` | Stateless framing, decoding and encoding — public so tools, tests and bindings can parse without a server |
| `gspro/event.h` | `gsp_event` and its payloads, `gsp_event_format()`, `gsp_event_is_sensitive()` |
| `gspro/server.h` | The server, the threading contract, the transport contract, policy, config, rings, `gsp_wire_chunk` |

Internal, reachable from tests: `src/gs_frame.h` (the incremental framer the server keeps per
connection, and which `gsp_frame_find()` drives from a clean state so the stateless and
incremental halves cannot disagree), `src/gs_json.h` (the bounded reader).
