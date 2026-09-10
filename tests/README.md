<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# The conformance suite

The executable form of [`../docs/conformance.md`](../docs/conformance.md): **103 cases** over
eight binaries, plus the sans-I/O gate and a fixture cross-check that needs no C at all.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset san && cmake --build --preset san && ctest --preset san
```

⚠ **Most of it is red right now, and that is the design.** The suite was written before the
library (design §11 package 1b) so that the specification could be run rather than only read.
`src/gs_unimplemented.c` supplies whatever `src/` does not yet define, so everything links and
every case reports a real failure instead of the whole build failing to link and telling
nobody anything. The configure line and each binary's first line of output name the groups
still standing in:

```
-- libgspro: NOT IMPLEMENTED YET — FRAME DECODE ENCODE MISC SERVER
```

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
| `test_fixtures.py` | The fixtures checked against the protocol document **in Python**, so the evidence and the decoder cannot be wrong together |
| `purity.cmake` | The library must not reference `socket`, `bind`, `accept`, a clock, a thread or a file |
| `fixtures/` | 23 byte-exact messages, one per client in the survey. [Provenance](fixtures/README.md) |

## Labels

`ctest -L ready` runs what must pass today — the purity gate and the fixture cross-check.
`ctest -L conformance` runs the specification. CI blocks on the first and reports the second
as a count, because a job that is always red teaches everyone to ignore it.

## Adding a case

1. Add the row to [`../docs/conformance.md`](../docs/conformance.md) §3 first, **naming the
   client that demands it**. A case with no client behind it is a preference, and preferences
   do not belong in a conformance suite.
2. Name the function for the row: `GS_TEST(CT_D25_something_specific)`. A failure prints that
   name, and the reader goes to one table row that says why the case exists.
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
