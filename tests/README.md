<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# The conformance suite

The executable form of [`../docs/conformance.md`](../docs/conformance.md): the CT- cases over
nine C binaries and two Python programs (the ABI pin and the host-transport cases), plus a
smoke test of the `gsplisten` tool, the sans-I/O gate and two cross-checks that need no C at
all.  `ctest --preset dev` runs 15 tests.

⚠ **The host-transport family runs TWICE, against two adapters** (conformance §3.8): once on
the asyncio reference transport and once on the C one, which share no code below the socket.
A CT-T case is therefore expected in two files, and `test_coverage.py` fails a T row that
reaches only one of them.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset san && cmake --build --preset san && ctest --preset san
```

⚠ **It is green now, and that was the whole point of writing it first.** The suite was written
before the library (design §11 package 1b) so that the specification could be run rather than
only read: while a source group was missing a scaffold supplied its symbols and the cases
touching it failed, on purpose, so the suite was a spec you could execute.  Every group has
landed, the scaffold is deleted, and the conformance run is the CI gate — red means a real
regression.

⚠ **Green is not the same as correct.** Every fixture is a transcription of a client's
serialiser, not a capture off a wire, so the suite proves the library agrees with what those
clients are *written* to send.  Whether it agrees with a real device is design §11's package 7,
which has not happened yet.

## What is here

| | |
|---|---|
| `gs_test.h` | The harness. ~200 lines, vendored, no dependencies. A case's NAME is its `CT-` id |
| `gs_srv.h` | The three lines of boilerplate every server case would repeat, and a synthetic clock |
| `test_frame.c` | CT-F — framing. There is none on the wire, so this is where most of the hazard lives |
| `test_decode.c` | CT-D — decode and the tolerances real clients need |
| `test_kind.c` | CT-K — shot / heartbeat / status, derived from three booleans |
| `test_reply.c` | CT-R — exactly one reply per message, promptly |
| `test_player.c` | CT-P — the 201 and the 202/203 pair |
| `test_conn.c` | CT-C — connection lifecycle |
| `test_robust.c` | CT-X — hostile input, ring behaviour, ABI |
| `test_api.c` | The vocabulary tables, the redaction sweep, the documented defaults |
| `test_python_abi.py` | The ctypes binding's structs, offsets, enums and bounds against `tools/gs_abi_table.c` — the compiler's own layout (CT-X07) |
| `test_python_transport.py` | CT-T — a host driving a real socket: reply latency, one write per message, spacing, a client vanishing mid-message. Adapter: the **asyncio** transport |
| `test_net.c` | The same CT-T rows against the **C** reference transport (`gspro_net`, `GS_BUILD_NET`), plus four cases about the transport's own contract. Both ends run on one thread, so a failure is reproducible rather than a race |
| `gs_net_client.h` | The client half of those: a launch monitor simulator in C, framing replies with the library's own `gsp_frame_find()` |
| `test_gsplisten.py` | The `gsplisten` tool end to end, as a user runs it — arguments, the announced port, the 201/202/200 a client sees, redaction, the exit path. ⚠ No CT id: it is our tool, not the protocol |
| `test_fixtures.py` | The fixtures checked against the protocol document **in Python**, so the evidence and the decoder cannot be wrong together |
| `test_coverage.py` | Every `CT-` row in the document has a case and every case has a row. ⚠ A case may be deferred, but only by id and with a reason, in that file |
| `purity.cmake` | The library must not reference `socket`, `bind`, `accept`, a clock, a thread or a file |
| `fixtures/` | 23 byte-exact messages, one per client in the survey. [Provenance](fixtures/README.md) |

## Labels

`ctest -L ready` runs the three that need no library — the purity gate and the fixture and
coverage cross-checks.
`ctest -L conformance` runs the specification, and CI now **blocks** on it: every group has
landed, so a red conformance run is a real regression rather than work not yet done.  The
non-blocking `progress` job still prints the green-case count, where a number that quietly
falls — a retired case — stays visible.

## Adding a case

1. Add the row to [`../docs/conformance.md`](../docs/conformance.md) §3 first, **naming the
   client that demands it**. A case with no client behind it is a preference, and preferences
   do not belong in a conformance suite.
2. Name the function for the row: `GS_TEST(CT_D25_something_specific)`. A failure prints that
   name, and the reader goes to one table row that says why the case exists.  ⚠ A **CT-T** row
   is a host's promise rather than the library's, so it is written once **per adapter** — in
   `test_python_transport.py` and in `test_net.c` — and `test_coverage.py` says so if one is
   missing.
3. If it needs new bytes, add a fixture and a row to [`fixtures/README.md`](fixtures/README.md)
   saying **the one thing it is there to pin**.
4. ⚠ **Guard anything that reads the result of a call that can fail.** `GS_ASSERT` reports and
   *continues*, so a case that indexes a buffer after a failed encode is itself undefined —
   which is exactly how the sanitizer build found that bug in `test_api.c` on the day this was
   written.

## Two things that are easy to get wrong

⚠ **The fixtures are transcriptions, not captures.** They say "this client's serialiser
produces these bytes", which is enough to pin a decoder and not enough to settle
[`../docs/protocol.md`](../docs/protocol.md) §11. When the first real capture exists, fixtures
promoted from it are marked as captured.

⚠ **`gs_test.h` auto-registration is the one non-portable part.** A case registers itself
before `main()`, so adding one needs no edit to a list — a list being exactly the thing that
silently stops covering what it forgot. If that mechanism ever fails, the suite would run zero
cases and exit 0, which reads like a pass; the runner refuses to do that.
