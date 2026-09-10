<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# Conformance runs

A dated record of what was run, on what, and what came back. One section per run,
newest first. ⚠ **This file is evidence, not a claim**: every number in it is a
number a command printed, and where a run changed the library the change is named.

⚠ **A run is not [design §11](design.md#11-implementation-plan-and-status)'s package 7.**
Package 7 is a session against a real launch monitor, and nothing in this file has met one.
Everything below is the library agreeing with **what sixteen clients' source code says they
send** — transcriptions, checked twice, over loopback. That is worth having and it is not the
same thing, and no amount of green here can become the other thing.

---

## 2026-09-10 — the full suite, after package 6

**Commit** `c5f168d` (package 6: the wire log, the `.gswire` container and the replay), plus
the three fixes this run produced (below).

### What it ran on

| | |
|---|---|
| Machine | macOS 27.0 (build 26A5425a), arm64 |
| Compiler | Apple clang 21.0.0 (clang-2100.3.27.1) |
| CMake | 3.30.5 |
| Python | 3.14.6 |

⚠ **One machine, one compiler, one endianness, one libc.** CI runs the same suite on Linux
gcc, Linux clang, Linux + ASan/UBSan, macOS and Windows MSVC, plus an install-and-consume job;
this record is the local run, and the platform column above is a limit on what it can mean.

### The suite

| Build | Configuration | Tests | Result |
|---|---|---|---|
| clean `Debug` | default options | 18 | **all passed** |
| clean `Debug` + ASan + UBSan | `GS_ENABLE_ASAN=ON GS_ENABLE_UBSAN=ON` | 15 run, 3 disabled | **all passed** |
| clean `Debug` + gcov | `GS_ENABLE_COVERAGE=ON` | 18 | **all passed** |
| clean `Release` | `-O3`, no debug info | 18 | **all passed** |
| `GS_BUILD_NET=OFF` | no reference transport | 16 | **all passed** |
| `GS_BUILD_RECORD=OFF` | no recorder | 15 | **all passed** |
| both off | core + binding only | 14 | **all passed** |
| `GS_BUILD_FFI=OFF` | no shared object | 15 | **all passed** |
| `GS_BUILD_TOOLS=OFF` | no tools | 14 | **all passed** |

⚠ **The three disabled tests are the ctypes ones under ASan on macOS**, and they are disabled
rather than unregistered so that `ctest` lists them every run: dyld refuses to preload the
Command Line Tools' ASan runtime into a ctypes host. The Linux/gcc sanitizer job in CI is what
covers them. Zero compiler warnings in every configuration, `-Werror` on throughout.

### Cases and assertions

| Binary | Cases | Assertions | Failures |
|---|---|---|---|
| `test_frame` | 13 | 95 | 0 |
| `test_decode` | 27 | 195 | 0 |
| `test_kind` | 11 | 61 | 0 |
| `test_reply` | 8 | 77 | 0 |
| `test_player` | 12 | 81 | 0 |
| `test_conn` | 11 | 59 | 0 |
| `test_robust` | 10 | 53 | 0 |
| `test_wire` | 12 | 120 | 0 |
| `test_api` | 11 | 393 | 0 |
| `test_record` | 11 | 361 | 0 |
| `test_net` | 13 | 329 | 0 |
| **total (C)** | **139** | **1824** | **0** |

| Python | Checks | Result |
|---|---|---|
| `test_python_abi.py` — sizes, offsets, enumerators, both directions | 273 | passed |
| `test_python_transport.py` — CT-T against the asyncio adapter | 43 | passed |
| `test_python_wire.py` — the container, Python writes / C reads | 22 | passed |
| `test_gsplisten.py` — the C listener end to end, C records / Python reads | 23 | passed |
| `test_fixtures.py` — the fixtures against the protocol document, no C | 622 | passed |
| `test_coverage.py` — every row has a case, every case has a row | — | 113 ids in 122 runs, 1 deferred |

**The one deferred id is CT-T06** — a client on a second machine — and it is deferred rather
than approximated. It is the only case that can tell a listener bound to `0.0.0.0` from one
bound to loopback (design §6.1), and both transport suites print how to run it by hand.

### The soaks

`GSP_SOAK=1`, which replaces CT-T05's five-second window with the five minutes the conformance
row actually names:

| Adapter | Window | Result |
|---|---|---|
| C reference transport (`test_net`) | 300 s | connection open throughout, still answered afterwards |
| asyncio transport (`test_python_transport`) | 300 s | 43/43 checks, same |

Silence is not a fault (protocol §9.6) and neither suite armed a timer: with no idle alarm
`gsp_server_next_due_us()` is `GSP_TIME_NEVER`, so there is nothing that *could* fire.

### The sans-I/O gate

`purity.cmake` passes, and the core's **entire** undefined-symbol set, read out of the archive
rather than assumed, is:

```
memcpy memmove memset memcmp bzero strcmp strncmp strlen strtod snprintf
malloc free hypot atan2 sin cos            (+ the compiler's hardening hooks)
```

No socket, no clock, no thread, no file, no `getenv`. `malloc`/`free` are §3.4's single
allocation; the four maths functions are §4.3's spin derivation. ⚠ design §2 listed a shorter
set than the object actually has — corrected in this run.

### Coverage (gcov, whole suite)

| Source | Lines | Branches |
|---|---|---|
| `src/gs_frame.c` + `gs_frame.h` | 100% | 100% |
| `src/gs_server.c` | 89.1% of 853 | 95.6% of 496 |
| `src/gs_misc.c` | 88.8% of 482 | 92.6% of 270 |
| `src/gs_decode.c` | 81.5% of 639 | 89.7% of 484 |
| `src/gs_encode.c` | **42.0% of 269** | 63.2% of 114 |
| `record/gs_record.c` | 82.9% of 404 | — |
| `net/gs_net.c` | 75.3% of 421 | — |

⚠ **`gs_encode.c` is the outlier and the reason is structural, not neglect.** Most of it is
`gsp_message_encode()` — the *client* direction, which this library does not speak (design §1).
It exists so tools and tests can play a launch monitor, and the fixtures deliberately **replay
bytes rather than re-serialise them**, so the suite goes around it. The half that matters,
`gsp_response_encode()` — the only path by which bytes leave the library — is exercised by
every reply in every case. ⚠ `net/gs_net.c`'s 75% is mostly error paths that need a failing
socket to reach.

### Every fixture, every style, over a real socket

Not part of the suite: `gsp_shoot.py` was aimed at a running `gsplisten` for **every fixture in
every byte style** — 23 fixtures × 6 styles (`asis`, `compact`, `indent`, `newline`, `mlm`,
`r10`) = **138 sessions**, each a fresh TCP connection through the real tool chain rather than
a direct call into the library.

| | |
|---|---|
| Acknowledged with a 200 | **132 / 138** |
| Not acknowledged | 6 — **all six are `gsp_full_commented.json`** |
| Protocol errors at the listener | exactly 1 |

⚠ **The six are the right answer.** `gsp_full_commented.json` is the vendor's own example
*with JavaScript comments in it*, which is not JSON; it exists to pin CT-X08. Sent as-is it
earned one 501 (`BAD_JSON`, 463 bytes discarded) and the simulator reported that a real client
would re-send; in the other five styles the simulator's own re-serialiser refused it before it
reached the socket. A run in which that fixture *had* been acknowledged would be the failure.

### The capture, and the replay

The sweep was recorded: **589 chunks, 133 connections, 94 KB of payload, 276 s**, identifiers
redacted. Replayed back through a **fresh server** with `gswire replay`:

| | |
|---|---|
| Messages decoded | 132 (96 shots) — the 133rd is the invalid one, still one protocol error |
| Replies produced | 133 |
| **Byte-identical to what was sent at the time** | **133 / 133** |
| Differing / missing / extra | 0 / 0 / 0 |
| Findings across the session | `MISSING_SHOT_NUMBER` `UNKNOWN_API_VERSION` `BALL_OBJECT_WITHOUT_FLAG` `CLUB_OBJECT_WITHOUT_FLAG` `KEY_CASE_MISMATCH` `UNKNOWN_KEYS` `TYPE_COERCED` |

That last row is what a capture is *for*: each flag names something a client's bytes did that
`protocol.md` had to reason about rather than observe.

### Installing and consuming

`cmake --install` into a clean prefix, then a separate project consuming it **six ways** — all
three targets (`gspro`, `gspro_net`, `gspro_record`) through both `find_package(gspro CONFIG)`
and `pkg-config`. All six compiled, linked and ran.

### ⚠ What this run changed

A run that changes nothing is a run that was not looking. Three:

1. **`gsp_replay_into_server()` tracked connections it had *ever seen* rather than the ones
   currently *open*.** The 133-connection capture above is what found it: the table filled at
   sixteen, the replay re-opened ids it had forgotten, skipped their closes, exhausted the
   server's connection table and reported **31 of 133 messages with 101 replies "missing"** — a
   capture reading back as a different session, which is the one thing a capture must never do.
   Fixed to track open connections and give the slot back on close; `test_record.c` now carries
   a case with more sequential connections than the table holds, and it fails at exactly 68 of
   80 with the fix removed.
2. **`gswire stats` reported "connections 16"** for that same capture, because it counted
   distinct ids into a fixed table. It counts the opens themselves now, and says 133.
3. **design §2's stated symbol set was shorter than the object's** — see the gate above.

### ⚠ What green still does not mean

Every fixture is a transcription of a client's serialiser, read from that client's source. The
suite therefore says: *the library agrees with what those sixteen clients are written to send,
and two host adapters carry that agreement over a real socket on this machine*. It does not say
what any device actually puts on a wire. Nothing here has met one; `protocol.md` §11's ten open
questions are all still open, and the switches in `gsp_server_policy` that answer them are all
still guesses. That is package 7, and it needs hardware this desk does not have.
