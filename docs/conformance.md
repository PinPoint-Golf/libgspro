<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# libgspro — client behaviour matrix and conformance suite

Every client of GSPro Open Connect that could be found and read, what each one actually puts
on the wire and expects back, and the conformance cases that follow. The suite is the
acceptance test for `libgspro` **and** the checklist for any host transport (PinPoint's
`QTcpServer` adapter included): a case tagged **[T]** is about the bytes the host writes and
when, not about the library.

Tags are [`protocol.md`](protocol.md) §0's. A case cites the clients that motivate it, so
that when a client is dropped or a new one appears the suite can be re-derived rather than
re-argued. ⚠ **No case here has yet been run against a real launch monitor**; the matrix is
read from source, and design §11's package 7 is where it meets hardware.

**The suite is built, runs, and is green.** `tests/` holds **103 cases** across eight binaries, plus the sans-I/O gate and a Python fixture cross-check that needs no C at
all. It was written before the library, so that the specification was one that could be run
rather than one that could only be read; every case now passes against the core, in both
configurations:

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset san && cmake --build --preset san && ctest --preset san   # CT-D24, CT-X02
```

⚠ **Green here is not the same as correct.** Every case is read from a client's source, not
from a wire, so the suite says the library agrees with what those clients are written to send —
design §11's package 7 is where that meets hardware.

---

## 1. The clients

Stars are GitHub's on 2026-09-09 and are a rough proxy for user base; a DIY launch monitor's
stars measure builders, a bridge's measure owners of that device who play GSPro.

| Tag | Device | Language | Stars | Alive | Licence |
|---|---|---|---|---|---|
| [OF] | OpenFlight DIY radar LM (hardware) | Python | **1043** | 2026-09 | AGPL-3.0 |
| [PIT] | PiTrac DIY camera LM (hardware) | C++ | 411 | 2026-08 | GPL-2.0 |
| [TL] | Garmin R10 | Node/Electron | 138 | 2023-04 | none |
| [MLM] | Rapsodo MLM2PRO, Mevo+, Uneekor, SkyTrak+ (screen OCR) | Python | 96 | 2026-07 | GPL-3.0 |
| [R10] | Garmin R10 | C# | 92 | 2024-01 | MIT |
| [OSG] | *(server role)* PiTrac, Square | C#/Godot | 61 | 2026-08 | GPL-2.0 |
| [OCR] | Rapsodo MLM2PRO (screen OCR) | Python | 23 | 2023-08 | MIT |
| [OSP] | SkyTrak+ via OpenSkyPlus (installer shipped) | C# | 10 / 1 | 2024-11 | proprietary |
| [KJD] | none (library) | Java | 8 | 2023-12 | MIT |
| [TNB] | none (reference library) | C# | 5 | 2023-10 | MIT |
| [FB] | Garmin R10 | Go | 5 | 2025-11 | Apache-2.0 |
| [SLX] | *(proxy)* SLX Micro via SLX Connect | TypeScript | 2 | 2024-10, adopted upstream | MIT |
| [OB] | *(server role)* Uneekor EYEMINI LITE via VIEW | Node | 1 | 2026-07 | MIT |
| [GG] | Garmin R10 via E6 stream | Rust | 1 | 2026-06 | none |
| [GC2] | Foresight GC2 (serial) | Rust | 0 | 2024-08 | none |
| [SB] | Foresight GC2 (USB) — ⚠ not known to work | Python | 2 | 2022-08 | none |

**Commercial connectors** — Garmin's own R10/R50 GSPro mode, Rapsodo's, FlightScope's, Uneekor's,
Square's, Bushnell's, SLX Connect — are closed and were not read. [SLX] is the nearest thing:
its logic was adopted into SLX Connect, and it sat between that connector and GSPro long enough
to document two GSPro behaviours nobody else did (§2).

### 1.1 What each sends

| Tag | Delimiter | Layout | Numbers | Absent = | Ball keys sent | Club object | `Contains*` truth |
|---|---|---|---|---|---|---|---|
| [PIT] | none | indented | plain; `TotalSpin` **0.0 placeholder** with real pair | 0.0 | all 7, no carry | all ten 0.0 | ball yes, club no |
| [TL] | none | compact | plain | omitted | **5 required only** | 6 of 10 zero | honest |
| [MLM] | none | compact | plain | 0 | all 8, key **`Backspin`**, carry 0 | 4 zero, face-to-path in HFI | club **always true** |
| [R10] | none | compact | plain | 0.0 | all 8 | all ten, when present | honest |
| [OCR] | none | compact | plain | 0 | 7 (no carry); putt: axis 0, VLA 0 | **`Speed` only** | club always true |
| [OSP] | none | indented | plain | **null → omitted** | measured only | `Speed` only | honest |
| [KJD] | none | compact | plain | omitted | as built | as built | honest |
| [TNB] | none | **indented** | plain | null → omitted | as built | as built | honest |
| [FB] | **`\n`** | compact | plain | `omitempty` for pair + carry | up to 8 | all ten | honest; **heartbeat every 5 s** |
| [OB] bridge | none | compact | plain | 0 | all 8 | **`null`** + extra `ClubName` | honest; heartbeat every 3 s |
| [GG] | **`\n`** | compact | plain | omitted | as measured | measured members only, or absent | honest |
| [OF] | none | compact | plain, rounded | 0.0 | all 8 | `Speed`, `Path`, rest 0.0 | club iff speed measured; heartbeat 5 s idle |
| [GC2] | none | compact | plain | 0.0 | all 8 (`TotalSpin` real *and* pair) | all ten 0.0 | ball yes, club no |
| [SB] | none | compact | **strings** `"147.5"`, `"true"`; key `Apiversion` | — | 5 | none | strings |

### 1.2 What each expects back

| Tag | Reply parsing | Waits for 200 | Acts on 201 | Handles 202/203 | Reconnect |
|---|---|---|---|---|---|
| [PIT] | **one read = one object; thread dies on failure** | no | DR / PT only, else Driver | no → "unknown" | lazily, next shot |
| [TL] | one read = one object | no | logs only | no | 5 s |
| [MLM] | regex split | **2 s, then re-sends** | PT → putting mode | no | manual |
| [R10] | `}{` → `},{` array | no | Club → device | no | 5 s |
| [OCR] | regex split | **10 s** | PT → putting mode | no | — |
| [OSP] | `SupportMultipleContent` | no | Handed; **arms on non-zero `DistanceToTarget`**; putting by config | **yes, exact strings; arms only after 202** | 30 s forever |
| [KJD] | brace count | no | event | no | — |
| [TNB] | 256-byte read = one object | 10 s queue | event | enum only | — |
| [FB] | decoder, **buffer reset on error** | no | logs only | no | — |
| [OB] bridge | discards | no | — | — | 1.5 s |
| [GG] | — | no | — | — | — |
| [OF] | string-aware framer | no | Handed + Club → state | no (ignored) | 1 → 30 s backoff |
| [GC2] | logs raw | no | — | — | 5 s |
| [SLX] monitor side | one read = one object | — | forwards GSPro's 201 to SLX | — | — |

---

## 2. The servers, and GSPro itself

| Server | Frames | Replies to shot | Replies to heartbeat / status | 201 | Bad input |
|---|---|---|---|---|---|
| **GSPro Connect** (from client evidence) | — | 200 (misspelled text, [TNB]) | 200 [TNB] / apparently nothing [PIT] — U13 | unsolicited on club change [CL]; after shots, concatenated with the 200 [MLM] [SLX]; with `DistanceToTarget` [OSP]; 202/203 at match start, sometimes concatenated [OSP] | unknown (U6); **zero-speed shot resets club** [SLX] |
| [OSG] | one read = one object | `{"Code":200}` | **501** "Missing or invalid shot data" | never | 501 / 413 |
| [OB] | string-aware, 64 KiB | 200 "Shot received successfully" | 200 "Heartbeat OK" / "OK" | **on connect**, and on `setPlayer` | 501 "Bad JSON: …" |
| [PIT] test server | one read = one object | **201** | 201 | every reply | — |
| [OF] mock | records bytes | scripted | scripted | scripted | — |
| **libgspro** (design §5) | string-aware, 16 KiB | 200 | 200 | on change; on connect by policy; 202/203 by session state | 501, resync |

Two facts about GSPro come only from [SLX] and are worth restating because a proxy that sat in
the real path is the strongest evidence in this survey short of a capture:

1. **GSPro concatenates replies.** "GSPro connector sometimes sends double messages with each flush."
2. **A shot with zero ball speed makes GSPro reset the club selection**, and Swinglogic's own
   connector produced such shots often enough that a proxy was written to turn them into
   heartbeats — and the fix was then adopted into SLX Connect. Two consequences for a server:
   a zero-speed "shot" from a commercial connector is a thing that happens (CT-D12), and
   emulating GSPro's reaction to it would be emulating a bug.

---

## 3. Conformance cases

Each case names the input, the required outcome, and the sources that make it necessary.
Prefix: **F** framing, **D** decode, **K** kind, **R** reply, **P** player and session, **C**
connection, **X** robustness, **T** host transport. A case marked **[T]** is verified against
the host's socket adapter with `gsp_shoot.py`, not against the library alone.

### 3.1 Framing

| Case | Input | Required | Because |
|---|---|---|---|
| CT-F01 | One compact object in one call | one message | [MLM] [TL] [R10] [OF] [OB] [GC2] |
| CT-F02 | Same object delivered **one byte per call** | one message, after the last byte | TCP |
| CT-F03 | Two objects in one call | two messages, two replies, in order | [OSP] [SLX] observed GSPro doing it; clients will |
| CT-F04 | Object + `\n` | one message; the newline consumed, not flagged | [FB] [GG] |
| CT-F05 | Object + `\r\n` + object | two messages | legal whitespace |
| CT-F06 | **Indented** object with newlines and tabs inside | one message | [TNB] [OSP] [PIT] |
| CT-F07 | Object whose `DeviceID` contains `{`, `}` and `\"` | one message, string intact | a bare brace counter ([KJD] [SLX]) fails this; ours must not |
| CT-F08 | Split **inside a string literal** across two calls | one message | TCP |
| CT-F09 | Leading garbage `xyz{...}` | `GSP_EV_PROTOCOL_ERROR` LEADING_GARBAGE, one 501, then the object decodes | §5.3 |
| CT-F10 | Top-level `[{...}]` | protocol error, 501, resync at next `{` | §5.3 |
| CT-F11 | 16 KiB + 1 of `{"a":"aaaa…` | protocol error TOO_LARGE, 501, buffer discarded, a following object decodes | [OSG] [OB] bound at 64 KiB; ours 16 |
| CT-F12 | Ten objects in one 8 KiB call | ten messages, ten replies | burst |

### 3.2 Decode

| Case | Input | Required | Because |
|---|---|---|---|
| CT-D01 | [GSP]'s full example verbatim | every field present, no flags except `UNKNOWN_KEYS` = 0 | [GSP] |
| CT-D02 | [TL]'s five-key `BallData` | present = required five; `BALL_INCOMPLETE` **clear** (TotalSpin+SpinAxis suffice) | [TL] |
| CT-D03 | `"Backspin"` lower-case s | back_spin present, `KEY_CASE_MISMATCH` set | [MLM] |
| CT-D04 | `"APIVersion"` / `"Apiversion"` | api_version `"1"`, `KEY_CASE_MISMATCH` | [R10] [SB] |
| CT-D05 | `"ShotNumber": 13.0` | shot_number 13, no `NOT_INTEGER` flag | [OSG] |
| CT-D06 | `"ShotNumber": 13.5` | 13, `NOT_INTEGER` | §9.3 |
| CT-D07 | `"ClubData": null` | club.present = 0, no crash, not `CLUB_OBJECT_WITHOUT_FLAG` | [OB] |
| CT-D08 | Extra top-level `"ClubName": "DRIVER"` | unknown_keys = 1, `UNKNOWN_KEYS` set, decoded normally | [OB] |
| CT-D09 | `BallData` with `"Speed":"147.5"` and options `"ContainsBallData":"true"` | values coerced, `TYPE_COERCED` set | [SB] |
| CT-D10 | `ClubData` with only `Speed` | club.present = SPEED only | [OSP] [OCR] |
| CT-D11 | `ClubData` with `Speed` and `Path` | present = SPEED\|PATH plus whatever else was 0.0 and sent | [OF] |
| CT-D12 | Shot with `"Speed": 0` and `ContainsBallData: true` | delivered as SHOT with speed present and zero; **no** flag — zero is a value; `gsp_message_ball_zero_speed()` true | [SLX]: happens in the wild; the host decides |
| CT-D13 | `TotalSpin: 0.0` with real `BackSpin`/`SideSpin` | decoder: all three present, no flag. `gsp_ball_data_derive()`: total and axis **recomputed**, `derived` = TOTAL\|AXIS | [PIT] |
| CT-D14 | `TotalSpin` and pair both real and consistent | derive: no change, `GSP_OK` | [GC2] [OF] |
| CT-D15 | [GSP]'s example (3250 vs pair 2500/-800) | derive: no change, `GSP_PENDING` | protocol §3.2 |
| CT-D16 | Pair only, `BackSpin: 0, SideSpin: 500` | derive: total 500, axis +90 | [GC2]'s edge |
| CT-D17 | `"DeviceID"` of 100 characters | truncated to 63, `STRING_TRUNCATED` flag and warning | §5.4 |
| CT-D18 | `"DeviceID": "Foresight GC2 (ABC123)"` | stored; `gsp_event_is_sensitive()` true; redacted by format | [GC2] |
| CT-D19 | `"Units": "Meters"` | units METERS, text kept, no conversion | [OCR] [OF] |
| CT-D20 | `"Units": "Metres"` | units UNKNOWN, `UNKNOWN_UNITS`, text kept | §3.6 |
| CT-D21 | No `Units` | UNKNOWN, text `""`, no flag ([GSP]: default yards is the host's inference) | [GSP] |
| CT-D22 | Duplicate key `"Speed"` twice | last wins | §9.1 |
| CT-D23 | Numbers in exponent form `1.5e2`, negative zero, `-0.0` | 150, 0 | `strtod` grammar |
| CT-D24 | Every truncation of every fixture | never reads past `len`; ASan clean | design §5.4 |

### 3.3 Kind

| Case | Options | Required kind | Because |
|---|---|---|---|
| CT-K01 | ball true, club false | SHOT | [GSP] |
| CT-K02 | ball false, club true | SHOT (club-only) | [TNB] |
| CT-K03 | ball false, club false, heartbeat true | HEARTBEAT | [R10] [FB] [OB] [OF] |
| CT-K04 | ball false, club false, heartbeat false | STATUS | [TNB] [OSP] |
| CT-K05 | heartbeat true **and** ready true | HEARTBEAT; ready flag read; `CLIENT_STATE` event if changed | [OSP] [R10] [PIT] |
| CT-K06 | no `ShotDataOptions` | STATUS, `MISSING_OPTIONS` | §9.7 |
| CT-K07 | ball true, no `BallData` | SHOT, `BALL_FLAG_WITHOUT_OBJECT`, `BALL_INCOMPLETE` | §3.4 |
| CT-K08 | ball false, `BallData` present with values | STATUS, `BALL_OBJECT_WITHOUT_FLAG`, values decoded | §3.4 |

### 3.4 Replies

| Case | Input | Required | Because |
|---|---|---|---|
| CT-R01 | Any of CT-K01…K08 | exactly one write, kind ACK, code 200, queued **before `on_bytes` returns** | [MLM] 2 s, [OCR] 10 s |
| CT-R02 | CT-F03's two objects | two ACKs in order | [SLX] |
| CT-R03 | CT-F09/F10/F11 | exactly one write, kind FAILURE, 501, with the documented text | [OSG] [OB] |
| CT-R04 | Incomplete shot, `reject_incomplete_shots` off | 200 | default |
| CT-R05 | Incomplete shot, policy on | 501 "Incomplete ball data" | policy |
| CT-R06 | Every write the ring can produce, fed to `gsp_response_decode()` | code ∈ {200, 201, 202, 203, 501}; never anything else | design §9.1 |
| CT-R07 | Write ring full | `on_bytes` returns `GSP_ERR_QUEUE_FULL`, bytes retained, resumes after drain | design §3.4 |
| CT-R08 | ACK text policy set to the misspelled string | that string, verbatim | U4 |

### 3.5 Player and session

| Case | Action | Required | Because |
|---|---|---|---|
| CT-P01 | `set_player(RH, DR)` with two connections | one 201 per connection, `PLAYER_INFO_SENT` × 2, reason CHANGED | design §5.6 |
| CT-P02 | `set_player` twice with equal values | second sends nothing | design §5.6 |
| CT-P03 | `set_player(RH, PT, distance 4.2)` | JSON has `DistanceToTarget: 4.2`; a client such as [MLM] would switch to putting | [MLM] [OCR] [OSP] |
| CT-P04 | `set_player(UNKNOWN, UNKNOWN, no distance)` | nothing sent; `Player` would be empty | design §5.6 |
| CT-P05 | `surface` set to `"Green"` | `Surface` member present | [OSP] [FB] |
| CT-P06 | Connection opened after `set_player`, policy on | 201 queued immediately, reason ON_CONNECT | [OB]; U1 |
| CT-P07 | Same, policy off | nothing until a change | U1 |
| CT-P08 | Shot then `set_player` change, one drain | ring order is ACK then 201 | [MLM]'s `{200}{201}` |
| CT-P09 | `set_session_state(ACTIVE)` | 202 `"GSPro ready"` to all; repeated call sends nothing | [OSP] |
| CT-P10 | `set_session_state(ENDED)` | 203 `"GSPro round ended"`, exact string | [OSP] matches it |
| CT-P11 | Connect while ACTIVE, `announce_ready_on_connect` | 201 then 202 | [OSP] arms only after 202 |
| CT-P12 | 202 fed to `gsp_response_decode()` | code 202, has_message, text exact | CT-R06 |

### 3.6 Connections

| Case | Action | Required | Because |
|---|---|---|---|
| CT-C01 | Open id 7 twice | second returns `INVALID_STATE` | design §3.2 |
| CT-C02 | `on_bytes` for unknown id | `UNKNOWN_CONNECTION` | |
| CT-C03 | Open 5 with max 4 | fifth `TOO_MANY_CONNECTIONS`; no event for it | design §5.1 |
| CT-C04 | First message names DeviceID | `CLIENT_IDENTIFIED` once; second message with another name → `WARN_DEVICE_ID_CHANGED`, id unchanged | design §5.2 |
| CT-C05 | Close after 3 shots, 2 heartbeats | `CONNECTION_CLOSED` info: messages 5, shots 3, cause as given | design §5.2 |
| CT-C06 | Reconnect with `ShotNumber` restarting at 1 | second connection's first shot is **not** `SHOT_NUMBER_REPEATED` (per-connection) | §4.3 |
| CT-C07 | Same shot number twice on one connection | `SHOT_NUMBER_REPEATED` | [MLM] re-send |
| CT-C08 | Heartbeats with incrementing `ShotNumber` then a shot | no repeated flag; counter is provenance | [FB] |
| CT-C09 | `gsp_server_close()` with 2 open | two `CONNECTION_CLOSED` SERVER_CLOSED; every later call `GSP_ERR_CLOSED`; polls drain | design §3.3 |
| CT-C10 | Idle alarm 10 s, silence 11 s | `CLIENT_IDLE` once; next message → `CLIENT_ACTIVE` | design §5.7 |
| CT-C11 | No idle alarm | `next_due_us` always NEVER; `tick` a no-op | §9.6 |

### 3.7 Robustness

| Case | Input | Required | Because |
|---|---|---|---|
| CT-X01 | 1 MiB of `{` | bounded memory, TOO_LARGE errors, connection survives | §9.3 |
| CT-X02 | Random bytes, 10 000 iterations under ASan/UBSan | no crash, no read past `len` | design §5.4 |
| CT-X03 | 100 consecutive errors, threshold 50 | `CLOSE_REQUESTED` once, at the 50th | design §5.3 |
| CT-X04 | Errors then a good message | consecutive count reset; no close request | design §5.3 |
| CT-X05 | Event ring of 4, 10 shots | 6 dropped, `dropped_events()` 6, `WARN_EVENTS_DROPPED`, **all 10 ACKs written** | design §5.4 |
| CT-X06 | Purity | core object references no socket, thread, clock or file symbol | design §2 |
| CT-X07 | ABI | sizes, offsets, enumerators match the Python binding both ways | design §4.6 |

### 3.8 Host transport [T]

Run with `gsp_shoot.py` against the host's adapter (PinPoint's `GsProMonitor`, the Python
asyncio transport, the C reference transport).

| Case | Drive | Required | Because |
|---|---|---|---|
| CT-T01 | Connect, send CT-D01, read | one 200 within **500 ms** | [MLM] 2 s with margin |
| CT-T02 | Send two shots back to back | two 200s, in order, each in its own segment where the platform allows | §9.8 |
| CT-T03 | Shot, then host changes club within 10 ms | 200 and 201 arrive as **two writes**; with `write_spacing_us` = 20 ms, at least 20 ms apart | [PIT] dies on `{200}{201}` |
| CT-T04 | Send one byte per 10 ms | one 200 after the last | CT-F02 end to end |
| CT-T05 | Connect and send nothing for 5 min | connection still open | §9.6 |
| CT-T06 | Connect from a second machine | accepted (bind is not loopback) | design §6.1 |
| CT-T07 | Two simultaneous clients | both acknowledged independently | design §5.1 |
| CT-T08 | Client disconnects mid-message | `CONNECTION_CLOSED`, no event for the partial | |
| CT-T09 | Bind while 921 is held by another process | host reports `Error` state with the reason; no crash | design §6 |
| CT-T10 | Host reply size | every write ≤ `GSP_WRITE_MAX`, one `write()` call each | design §5.5 |

---

## 4. Fixtures

One file per row, hand-written from the source's serialisation code — not copied files. The
`.json` is the exact bytes a source emits; a `.txt` beside it says what the decode must yield.

| File | Emits | Case |
|---|---|---|
| `gsp_full.json` | [GSP] verbatim, indented, with the vendor's inconsistent spin | D01, D15 |
| `tl_minimal.json` | five ball keys, six club zeros | D02 |
| `mlm_backspin.json` | `Backspin`, carry 0, club always true, face-to-path in HFI | D03 |
| `r10_apiversion.json` | `APIVersion`, all eight ball keys | D04 |
| `r10_heartbeat.json` | ShotNumber 0, IsHeartBeat + ready + detected | K03, K05 |
| `osg_float_shot_number.json` | `13.0`, 15-decimal spins | D05 |
| `tnb_indented.json` | Newtonsoft indented, nulls omitted | F06 |
| `osp_connect_heartbeat.json` | indented, IsHeartBeat true + ready | K05 |
| `osp_shot.json` | nulls omitted, club `Speed` only | D10 |
| `ocr_putt.json` | axis 0, VLA 0, club `Speed` only | D10 |
| `pit_shot.json` | indented, `TotalSpin` 0.0 placeholder, ten club zeros | D13 |
| `pit_keepalive.json` | IsHeartBeat true, ready true | K03 |
| `of_shot.json` | compact rounded, club `Speed`+`Path` | D11 |
| `of_heartbeat.json` | ShotNumber 0, detected false | K03 |
| `ob_shot.json` | `ClubData: null`, `ClubName` | D07, D08 |
| `ob_heartbeat.json` | 3 s cadence shape | K03 |
| `fb_shot_newline.json` | compact + `\n`, omitempty | F04 |
| `fb_heartbeat_newline.json` | incrementing ShotNumber | C08 |
| `gg_shot_newline.json` | `\n`, partial club members | F04, D11 |
| `gc2_shot.json` | serial in DeviceID, total and pair | D18, D14 |
| `sb_strings.json` | string-typed values, `Apiversion` | D09 |
| `slx_zero_speed.json` | a "shot" with Speed 0 | D12 |
| `gsp_full_commented.json` | the vendor's example **with its `//` comments**, which is what somebody pastes | X08 |

Provenance for each, and the byte-level details a re-generation would get wrong, are in
[`../tests/fixtures/README.md`](../tests/fixtures/README.md).

### 4.1 Cases beyond the tables above

Written while building the suite, each covering a hazard the tables imply but do not name.
They carry a letter suffix so the numbered rows keep their meaning.

| Case | What it pins | Because |
|---|---|---|
| CT-F10b | whitespace alone is pending, never garbage | [MLM]'s splitter allows spaces between objects |
| CT-D07b | a null BOOLEAN in `ShotDataOptions` is absence | [R10] writes `"IsHeartBeat": null` |
| CT-D08b | an unknown key whose value is a nested object or array is skipped **whole** | a decoder that resumed inside it would read the nested members as top-level keys |
| CT-D21 | absent `Units` is UNKNOWN and not a finding | [TL] sends no `Units` key at all |
| CT-D23b | missing required root fields are delivered and flagged, never refused | design §4.5 |
| CT-K03b | a heartbeat carrying full `BallData`/`ClubData` objects | [PIT] and [OF] both do |
| CT-K05b | the server raises `CLIENT_STATE` from a heartbeat, and only on change | [OSP] carries ready on a heartbeat; [FB] repeats it every 5 s |
| CT-K08b | `ContainsClubData: true` is not evidence the numbers are real | [MLM] sets it on every shot |
| CT-X02b | the noise sweep through a server, which adds the framer's state | |
| CT-X08 | the vendor's own commented example is refused cleanly | it is not JSON, and it is the first thing anyone copies |
| CT-X09 | NULL and zero-length at every entry point | the floor beneath every other case |
| API-* | the vocabulary tables, the redaction sweep, the documented defaults | a club code that maps to its neighbour, or an event whose formatter forgot to redact, fails silently |

⚠ **The redaction sweep (`API_no_event_leaks_an_identifier_when_redacted`) formats EVERY event
type with an identifier in every field that can hold one.** design §9.2: a logging path that
only redacts the events a developer happened to hit is one that leaks during an incident.

---

## 5. What the matrix says the server must never assume

Read as a list, because each one was true of at least one of the top four clients by user base:

- that a message ends at a newline, or that it does not ([FB] [GG] vs everyone else);
- that a read holds one message ([PIT] [TNB] [TL] believe it and are wrong the other way);
- that `TotalSpin` being present means it is real ([PIT], the second-largest client);
- that `ContainsClubData` means the club numbers are real ([MLM]);
- that a key's case matches the page ([MLM] [R10] [SB]);
- that an object is an object and not `null` ([OB]);
- that heartbeats are absent, periodic, or carry a fixed `ShotNumber` ([FB] [OB] [OF] [R10]);
- that the client will wait more than two seconds for a reply ([MLM]);
- that the client can parse two replies in one read ([PIT]);
- that a zero-speed shot is impossible ([SLX]);
- that the client cares about the `Message` text ([OSG] proves nobody does, [OSP] proves one string matters).
