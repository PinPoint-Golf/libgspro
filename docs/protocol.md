<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (C) 2026 Mark Liversedge -->
# GSPro Open Connect v1 — protocol reference

Implementation-facing description of the **GSPro Open Connect v1** protocol as spoken between
a launch monitor (the *client*) and GSPro Connect (the *server*). Written to be implemented
from directly, **from the server side**: `libgspro` plays the GSPro Connect role, so that any
launch monitor or connector that can talk to GSPro can talk to the application embedding this
library instead.

**This document states what the protocol *is*, and says where each statement came from.** The
vendor's own documentation is one short page. Everything beyond it has been established by
reading independent client implementations that are known to work against the real GSPro
Connect, and by reading one independent server implementation. Every claim carries a
provenance tag from §0, and a claim that rests on a single client is marked as such. Nothing
here has yet been verified against a running copy of GSPro Connect or a real launch monitor
— see §11 for what that leaves open and how it will be closed.

**The protocol is small.** One TCP socket, JSON objects in both directions, no framing, no
authentication, no versioning beyond a string that has only ever been `"1"`. Almost all of the
difficulty is in the things the page does not say: how messages are delimited (they are not),
what the server replies to a heartbeat (a shot acknowledgement), and what clients actually send
(not quite what the page shows).

---

## 0. Provenance

Every claim in this document carries one or more of these tags. Where the official page and a
client disagree, both are quoted and the disagreement is stated.

| Tag | Source | What it is | State when read |
|---|---|---|---|
| **[GSP]** | https://gsprogolf.com/GSProConnectV1.html | The vendor's page, the only official specification | Fetched 2026-09-09; reproduced verbatim in Appendix A |
| **[CL]** | https://gsprogolf.com/change-log.html | The vendor's release notes | Fetched 2026-09-09 |
| **[TNB]** | https://github.com/tnbozman/gspro-interface | C# client library (.NET 5), MIT. Includes `OpenAPI-Documentation-Feedback.MD`, an annotated redraft of [GSP] with corrections the author raised with the vendor | `f0048f6`, 2022-01-03 |
| **[R10]** | https://github.com/mholow/gsp-r10-adapter | C# bridge from a Garmin Approach R10 to GSPro, MIT. Widely used | `d337671`, 2024-01-28 |
| **[MLM]** | https://github.com/springbok/MLM2PRO-GSPro-Connector | Python/PySide bridge from a Rapsodo MLM2PRO to GSPro, GPL. Widely used | `6d503e6`, 2026-07-01 |
| **[KJD]** | https://github.com/kenjdavidson/gspro-connector | Java client, MIT | `3bf633e`, 2023-12-11 |
| **[TL]** | https://github.com/travislang/gspro-garmin-connect-v2 | Node/Electron bridge from an R10 to GSPro | `27fc785`, 2023-04-27 |
| **[OSG]** | https://github.com/jhauck2/OpenShotGolf | Godot golf simulator that **implements the GSPro role** — a listener for Open Connect clients, GPL | `b83a3ff`, 2026-06-23 |
| **[PIT]** | https://hackaday.io/project/195042-pitrac-the-diy-golf-launch-monitor/log/228422 | PiTrac DIY launch monitor's build log for its Open Connect client and test server | Fetched 2026-09-09 |
| **[OSP]** | https://github.com/OpenSkyPlus/GSPro4OSP | C# plugin bridging a SkyTrak+ (via OpenSkyPlus) to GSPro, proprietary licence — **read only**. The most complete client found: handles 202/203, `Surface`, putting-mode switching | `6d8ae6d`, 2024-11-23 |
| **[OCR]** | https://github.com/rowengb/GSPro-MLM2PRO-OCR-Connector | Python bridge from a Rapsodo MLM2PRO (screen OCR) to GSPro, MIT. Same lineage as [MLM] | `edd91e2`, 2023-08-12 |
| **[GC2]** | https://github.com/matthew-johnston/gspro-gc2-connector | Rust bridge from a Foresight GC2 serial feed to GSPro, no licence file — read only | `b4eb3fa`, 2024-08-21 |
| **[TG]** | https://gist.github.com/that0n3guy/ebcb574407183964195b26c9508e13a6 | Electron R10 bridge gist | Fetched 2026-09-09 |
| **[PIT]** *(code)* | https://github.com/PiTracLM/PiTrac | The PiTrac DIY camera launch monitor itself, GPL-2.0 — **read only**. C++/Boost client plus a GSPro-role test server. Real hardware, 411 stars, active community | `6ffc0eb`, 2026-08-28 |
| **[OF]** | https://github.com/open-flight/openflight | OpenFlight DIY Doppler-radar launch monitor, AGPL-3.0 — **read only**. Python client with a string-aware framer and a mock GSPro-role server in its tests. Real hardware, **1,043 stars — the largest client in the survey**, active | `40d5fad`, 2026-09-08 |
| **[OB]** | https://github.com/rroojrooj/Open-Birdie | Open-Birdie simulator, MIT. **Implements the GSPro role**, tested against a Uneekor EYEMINI LITE through a bridge that tails Uneekor VIEW's shot folder | `a6cb942`, 2026-07-28 |
| **[FB]** | https://github.com/401Unauthorized/fairway-bridge | Fairway Bridge, Go, Apache-2.0. R10 → GSPro bridge | `6132780`, 2025-11-16 |
| **[GG]** | https://github.com/ShaneBreazeale/garmingolf-connector | Rust bridge from Garmin Golf's E6 stream to GSPro, no licence file — read only. Has a GSPro payload contract test | `0303df3`, 2026-06-14 |
| **[SLX]** | https://github.com/kenjdavidson/gspro-slx-proxy | Electron **proxy between SLX Connect (Swinglogic's commercial connector) and GSPro**, MIT, archived after Swinglogic adopted its fix. Documents GSPro Connect's alternate-port setting | `fd4b55d`, 2024-10-12 |
| **[SB]** | https://github.com/ShaneBreazeale/GSPro-gc2-API-Interface | Python GC2 USB bridge, 2022, unlicensed. ⚠ Not known to work: it expects a reply format no other source has seen and its shot path has a fatal bug. Cited only for the *shape* of what it sends | `6814edf`, 2022-08-29 |

The full behaviour-by-client matrix, with star counts as a rough proxy for user base, is
[`conformance.md`](conformance.md) §1.

**Not a source, and why.** *OpenGolf Connect* (https://opengolfapi.org/connect/) advertises "a
universal `:921` listener" that accepts "a raw GSPro OpenConnect payload" and says its source is
at `github.com/opengolfapi/connect`. That repository returns 404, the `opengolfapi` organisation
publishes no such repository, and the advertised `@opengolf/connect` npm package does not exist
(checked 2026-09-09). It is a second GSPro-role listener in principle and would have been the
best evidence about what clients tolerate; until its code is public it contributes nothing
beyond the page's own claim that a custom port is needed "if 921 is taken by GSPro" (§11 U11).

⚠ **Licensing of the sources.** [MLM] and [OSG] are GPL. They were **read** to establish
behaviour; no code from any source is copied into this MIT-licensed library, and the
implementation is written from this document alone. The same rule as `libwrist`.

**How the tags are used.** A statement tagged **[GSP]** is the vendor's word. A statement
tagged only with client tags is *what working clients do*, which is evidence about the server
they work against but not a specification of it. A statement tagged **[OSG]** alone describes
what one *independent* server does, which is evidence about what clients tolerate rather than
about GSPro. Where a claim matters and has only one source, the text says so.

---

## 1. Roles and transport

**GSPro Connect is a TCP server; the launch monitor is a TCP client.** [GSP]: "GSPro Connect
opens a socket and listens for an incoming client. Once connected is established a constant
2-way communication continues." `libgspro` is therefore a **listener**, and everything that
connects to it is a launch monitor or a bridge acting as one.

| | Value | Source |
|---|---|---|
| Transport | TCP | [GSP] ("socket"), every client opens a stream socket [TNB] [R10] [MLM] [KJD] [TL] |
| Port | **921**, or **922** | [GSP] writes it "0921" — a leading zero, decimal 921. Every client defaults to 921. ⚠ GSPro Connect can be moved to **922** and nowhere else: [SLX] documents `<OpenAPIUseAltPort>true</OpenAPIUseAltPort>` in `C:\GSPro\GSPC\GSPconnect.exe.config`, which "will start GSPConnect on port 922, instead of 921". That is the whole of GSPro's port configurability (was U11) |
| Address | 127.0.0.1 | [GSP]: "It is assumed that the launch monitor software is being ran on the same PC as GSPro connect. If this is not the case, firewall and port forwarding may be required" |
| Authentication | None | [GSP]: "The socket connection is open and does not require authentication" |
| Encoding | UTF-8 text, JSON | Clients encode as UTF-8 [R10] [MLM] [TL] [KJD] or ASCII [TNB] [OSG]. No client sends a byte outside ASCII in practice |
| Concurrent clients | Not stated | [GSP] says "an incoming client", singular. [OSG] accepts one connection at a time; [SLX]'s monitor listener destroys a second connection with "Monitor is already connected"; [OB] accepts many. A second connector (a putting device beside a full-swing device) is an ordinary GSPro setup [MLM], so the *product* supports more than one client, whether through one listener or several is not established. `libgspro` accepts many; design §5.1 |

**Binding address.** ⚠ [GSP] documents the loopback address, but a launch monitor on another
machine or an embedded device (PiTrac, [PIT]) reaches the server over the LAN. `libgspro` does
not own the socket, so the bind address is the host's choice; design §3.2.

**Reconnection.** Clients reconnect on their own initiative after a drop, typically after a
fixed delay: 5 s [R10] [TL] [GC2], 1.5 s [OB], 30 s indefinitely [OSP], exponential 1 → 30 s [OF]. [PIT] reconnects lazily, on the next shot after its receive thread died. [MLM] does not reconnect automatically; it tells the user. A server
must therefore expect the same client to arrive again on a fresh connection at any time, with
its shot counter either continuing or restarting (§4.3).

---

## 2. Framing — there is none

⚠ **This is the most important thing on this page, and the official page does not mention it.**

Each message is one JSON object. **Nothing delimits one object from the next.** There is no
length prefix, no newline, no NUL. A message is written to the socket as the bytes of its JSON
serialisation and nothing else:

- [TNB]: `Encoding.ASCII.GetBytes(JsonConvert.SerializeObject(payload, Formatting.Indented, ...))` — *indented* JSON, so a single message contains newlines and tabs **inside** it. A newline cannot be a delimiter.
- [MLM]: `self._socket.sendall(json.dumps(payload).encode("utf-8"))` — compact JSON, no trailing newline.
- [TL]: `this.socket.write(JSON.stringify(APIData))` — compact, no trailing newline.
- [KJD]: `socket.getOutputStream().write(Json.writeValue(request).getBytes())`.
- [R10]: `SendAsync(JsonSerializer.Serialize(...))`.
- [OSP]: `Formatting.Indented`, UTF-8, `WriteAsync` — a second client sending **indented** JSON.
- [GC2]: `serde_json::to_string` then `write_all` — compact, no delimiter.
- [TG]: `client.write(JSON.stringify(gsprodata))`.

- [PIT]: Boost `write_some` of a pretty-printed object (Boost's JSON writer indents) with numbers un-quoted by a regex afterwards.
- [OF]: `json.dumps(..., separators=(",", ":"))` — compact.
- [OB]'s bridge: `sock.write(JSON.stringify(msg))` — compact.
- ⚠ [FB]: `conn.Write(append(data, '\n'))` — **compact with a trailing newline**.
- ⚠ [GG]: its contract test asserts "forwarded GSPro payload should be newline-delimited".

So **two of fourteen clients append a newline** and twelve do not. A server must accept both,
which the whitespace rule below gives for free, and must never *require* one.

**Consequently a TCP read may contain a fraction of a message, exactly one message, or several
concatenated.** Both are observed in practice on the *client* side reading GSPro's replies, and
there is no reason the server side differs:

- [R10] handles `"{one}{two}"` in one read by rewriting `}{` to `},{` and parsing the result as an array, with the comment "Sometimes multiple responses received in one buffer".
- [MLM] splits a buffer with the regular expression `(\{.*?})(?= *\{)` — one or more objects, possibly separated by spaces.
- [KJD] reads byte by byte and counts `{` against `}`, parsing when the depth returns to zero. Its own comment: "At this point I'm not sure whether there is a delimiter or any way to tell when a message has been completed (hopefully `\n`)."
- [TNB] reads into a 256-byte buffer and parses whatever arrived as one message. ⚠ This breaks on any message over 256 bytes and on two messages in one read; it is what the reference implementation does, and it works only because GSPro's replies are short and infrequent.
- [OSP] reads until `DataAvailable` is false, then parses with `JsonTextReader { SupportMultipleContent = true }` — and its own comment records **GSPro itself sending two objects in one segment**: "GSPro's API returns two json objects back in the same response. This seems to only occur when a match/hole starts and the ready message is sent with an immediate 'match ended' message". So concatenation happens in both directions, unsolicited, from the real server.
- [OCR] uses the same regular expression as [MLM].
- [SLX]'s own comment on reading from GSPro: "GSPro connector sometimes sends double messages with each flush" — a third independent observation of GSPro concatenating, this one from a proxy that sat between a commercial connector and GSPro for months. It counts bare braces (not string-aware).
- [OF] frames with a string- and escape-aware depth counter and a 64 KiB backstop — the same algorithm this library uses (design §5.3), independently arrived at.
- ⚠ [PIT] parses **each `read_some()` as one message and abandons its receive thread when that fails**, reconnecting on the next shot. So a 200 and a 201 landing in one read do not merely lose the 201 — they cost the connection. §9.8.
- [FB] accumulates into a buffer and runs `json.Decoder`, but **resets the buffer on any decode error**, so a partial message followed by more bytes is lost rather than completed.
- [OSG], as a server, parses each read as one message. It works because the clients it was tested with send one shot per write, with seconds between them.

**What a correct server does.** Treat the connection as a byte stream. Accumulate bytes.
Extract complete JSON objects by tracking brace depth **outside of string literals** (a `{`
inside `"DeviceID": "Foo{Bar"` must not count). Parse each complete object independently.
Ignore whitespace between objects. Bound the accumulation (§9.2). Design §5.3 specifies this.

**Whitespace between and inside messages** is legal JSON and does occur ([TNB] sends indented
messages). A server must accept CR, LF, tab and space anywhere JSON allows them and between
objects.

---

## 3. Client → server: the shot message

Everything a client sends is one message shape, with sections switched on and off by
`ShotDataOptions`. [GSP] shows the shape once with every section present; Appendix A has it
verbatim. This section states what each part means and what clients actually put there.

### 3.1 Root properties

| Key | Type | Required | Meaning | Source |
|---|---|---|---|---|
| `DeviceID` | string | **required** | "unqiue per launch monitor / prooject type" [GSP, sic]. A free-text name of the *software* sending, not a serial number: `"GSPro LM 1.1"` [GSP] — **copied verbatim by [FB]**, so the vendor's example string is a real value on the wire — `"GSPRO-R10"` [R10], `"Garmin R10"` [GG], `"GsPro4Osp"` [OSP], `"OpenFlight"` [OF], `"PiTrac LM 0.1"` [PIT], `"Uneekor VIEW (Open-Birdie watch)"` [OB], user-configurable in [MLM] and [TL]. ⚠ [GC2] sends `"Foresight GC2 (<serial number>)"` — a **hardware serial** inside the label, so the field can identify a specific unit and is treated as an identifier (design §9.2) | [GSP] |
| `Units` | string | optional, "default yards" | `"Yards"` or `"Meters"` — the only two values any source knows: [TNB] enumerates exactly these two, and [GSP] shows `"Yards"`. [TNB]'s annotated redraft marks this field "optional - default yards". [OCR] takes it from a user setting named `METRIC` whose default is `"Yards"`, and [OF] has a `units` parameter its tests exercise with `"Meters"`, so a connector *can* send it; none was observed defaulting to it. ⚠ What the field *governs* is not stated by anyone; see §3.6 | [GSP] [TNB] [OCR] |
| `ShotNumber` | number | **required** | "auto increment from LM" [GSP]. ⚠ Observed as a JSON *float* (`13.0`) in [OSG]'s fixtures, as `0` for heartbeats in [R10], and starting from 1 in [MLM] and [TL]. A server must accept an integer-valued float. See §4.3 | [GSP] |
| `APIversion` | string | **required** | `"1" is current version` [GSP]. ⚠ Spelled with a lower-case `v`. [R10]'s C# property is `APIVersion`, serialised with the default (unchanged) casing, so **at least one widely used client sends the key as `APIVersion`**; [SB] sends `Apiversion`. A server must match keys case-insensitively | [GSP] [R10] [SB] |
| `BallData` | object | when `ContainsBallData` | §3.2 | [GSP] |
| `ClubData` | object | when `ContainsClubData` | §3.3 | [GSP] |
| `ShotDataOptions` | object | **required** by its two required members | §3.4 | [GSP] |

### 3.2 `BallData`

Angles in degrees, spin in rpm, speed in the unit implied by `Units` (§3.6). Sign conventions
in §3.5.

| Key | Required | Meaning | Source |
|---|---|---|---|
| `Speed` | **required** | Ball speed | [GSP] |
| `SpinAxis` | **required** | Spin axis tilt, degrees. ⚠ Sign convention §3.5 | [GSP] |
| `TotalSpin` | **required** | Total spin, rpm | [GSP] |
| `BackSpin` | "only required if total spin is not sent" | Backspin component, rpm | [GSP] |
| `SideSpin` | "only required if total spin is not sent" | Sidespin component, rpm | [GSP] |
| `HLA` | **required** | Horizontal launch angle, degrees | [GSP] |
| `VLA` | **required** | Vertical launch angle, degrees | [GSP] |
| `CarryDistance` | optional | Carry, in `Units` | [GSP] |

**What clients actually send.**

- [TL] sends **only the five required keys** — `Speed, SpinAxis, TotalSpin, HLA, VLA` — with no `BackSpin`, `SideSpin` or `CarryDistance`. A server must not require the optional keys.
- [MLM] sends every key, with `CarryDistance: 0` unconditionally, and ⚠ **spells backspin `"Backspin"`** (lower-case *s*), not `"BackSpin"`. This connector is in daily use against GSPro, which means either GSPro matches keys case-insensitively or it ignores that key and uses `TotalSpin`/`SpinAxis`. Either way a server that wants the value must match case-insensitively.
- [R10] sends all eight as non-nullable doubles, so an unmeasured quantity arrives as `0.0`, not as an absent key.
- [OSG]'s fixtures carry `CarryDistance: 0.0` and `BackSpin`/`SideSpin` with more decimal places than a float holds; values should be read as `double`.
- [OSP] declares every member nullable and serialises with nulls ignored, so an unmeasured quantity is **absent**, not zero — the opposite of [R10]. It also converts its device's m/s to mph explicitly before sending under `"Yards"`, which is the clearest evidence that **`Units: "Yards"` means mph speeds**.
- [OCR]'s putting path sends `SpinAxis: 0` and `VLA: 0` as literal placeholders for a putt, with real `Speed`, `TotalSpin` and `HLA`.
- ⚠ [PIT] — a real camera device with the second-largest user base of any source — sends **`TotalSpin: 0.0` as a literal placeholder** with real `BackSpin` and `SideSpin`, and a computed `SpinAxis`. A server that trusts a present `TotalSpin` records zero spin for every PiTrac shot.
- [FB] marks `BackSpin`, `SideSpin` and `CarryDistance` `omitempty`, so a zero there is **absent** rather than sent — the third convention, after "send zero" and "send null-as-absent".
- [OB]'s bridge sends **`"ClubData": null`** — a JSON null where an object is expected — and an extra top-level key `ClubName` carrying the device's club as text. Both must be tolerated: a null object is "absent", and unknown keys are skipped (§9.7).
- [SB] sends every number and boolean **as a JSON string** (`"Speed": "147.5"`, `"ContainsBallData": "true"`). It is not known to work against GSPro, so this is a tolerance a server may choose, not one it needs; `libgspro` coerces and flags it (design §4.5).
- ⚠ **A `0.0` is therefore ambiguous**: it may be a measurement or a placeholder for "not measured". The protocol has no null and no absent-means-unknown convention that clients follow. `libgspro` surfaces presence *and* value and leaves the interpretation to the application (design §4.3).

**Deriving the spin pair from the total, and back.** Two clients that hold one representation
compute the other before sending, and they agree on the arithmetic:

- [OCR]: `BackSpin = TotalSpin · cos(SpinAxis)`, `SideSpin = TotalSpin · sin(SpinAxis)`.
- [GC2]: `SpinAxis = atan(SideSpin / BackSpin)` in degrees, ±90° when `BackSpin` is zero.

So `TotalSpin = hypot(BackSpin, SideSpin)` and `SpinAxis = atan2(SideSpin, BackSpin)`, with
positive `SideSpin` giving positive `SpinAxis` — consistent with §3.5's inferred sign.
⚠ **The vendor's own example does not satisfy this**: `TotalSpin 3250` against `BackSpin 2500,
SideSpin -800` (hypot 2625) and `SpinAxis -13.2` (atan2 gives -17.7°). The example's numbers
are illustrative, not a measurement. `libgspro` offers the derivation as an explicit helper
that marks what it filled in (design §4.3); it never applies it silently.

### 3.3 `ClubData`

All ten keys are shown in [GSP] with `0.0` and no required/optional annotation. Angles in
degrees, speeds as ball speed, `ClosureRate` in degrees per second (by name; no source states
it). Impact locations are unstated in unit; the names suggest a signed distance from face centre.

| Key | Meaning (by name — no source elaborates) |
|---|---|
| `Speed` | Clubhead speed |
| `AngleOfAttack` | Attack angle |
| `FaceToTarget` | Face angle relative to the target line |
| `Lie` | Lie angle at impact |
| `Loft` | Dynamic loft |
| `Path` | Club path relative to the target line |
| `SpeedAtImpact` | Clubhead speed at impact (distinct from `Speed` — presumably pre-impact vs at-impact) |
| `VerticalFaceImpact` | Strike height on the face |
| `HorizontalFaceImpact` | Strike location across the face |
| `ClosureRate` | Face rotation rate through impact |

**What clients actually send.** [MLM] fills `Lie`, `Loft`, `VerticalFaceImpact`, `ClosureRate`
with literal `0` and ⚠ **puts face-to-path in `HorizontalFaceImpact`** — a quantity that is not
what the key names. [R10] sends the object only when club data exists, with unmeasured members
`0.0`. [TL] fills six of ten with `0.0`. [OSP] and [OCR] send `ClubData` containing **only
`Speed`** with `ContainsClubData: true`; [OF] sends `Speed` and `Path` with the rest `0.0` and
sets `ContainsClubData` only when club speed was measured; [GG] omits every member it did not
measure and omits the object entirely when not sending club data. [GC2] and [PIT] send all ten
as `0.0` with `ContainsClubData: false`. A server should expect anything from one key to all ten and treat `0.0` as "possibly
unmeasured" exactly as for ball data.

### 3.4 `ShotDataOptions`

| Key | Required | Meaning | Source |
|---|---|---|---|
| `ContainsBallData` | **required** | `BallData` is present and is a shot | [GSP] |
| `ContainsClubData` | **required** | `ClubData` is present | [GSP] |
| `LaunchMonitorIsReady` | optional | The device is armed and will report the next strike. [CL] 3.0.3.2: "Added openAPI ball indicator support (developer notes: ShotDataOptions.LaunchMonitorIsReady)" — GSPro shows this as an on-screen indicator | [GSP] [CL] |
| `LaunchMonitorBallDetected` | optional | The device sees a ball on the mat | [GSP] |
| `IsHeartBeat` | optional | This message carries no shot; it exists to keep the connection alive. [TNB]'s redraft annotates it "(retired)" and "Heart beat (retired)" in the summary, but [R10] sends it and GSPro accepts it (§4.2) | [GSP] [TNB] [R10] |

**Message kinds are derived, not declared.** There is no `Type` field. A server classifies a
message by these flags:

| `ContainsBallData` | `ContainsClubData` | `IsHeartBeat` | Kind |
|---|---|---|---|
| true | any | any | **Shot** with ball data (and club data if the second flag is true) |
| false | true | any | **Shot** with club data only — [TNB] offers `SendClubData()` producing exactly this. Whether GSPro renders a shot from club data alone is unknown |
| false | false | true | **Heartbeat** [R10] [TNB] |
| false | false | false / absent | **Status**: a bare ready/ball-detected update. [TNB] `SendLaunchMonitorStatus()` sends this with `IsHeartBeat: false` |

⚠ **Heartbeat and status combine.** [OSP]'s `ShotDataOptions` defaults `IsHeartBeat` to
`true`, so its connect-time message carries `IsHeartBeat: true` *and* `LaunchMonitorIsReady`;
[R10] does the same. A server must read the ready/ball-detected flags off a heartbeat too, not
only off a "status" message.

⚠ **The `Contains*` flags and the presence of the objects can disagree.** [MLM] sends
`ContainsClubData: true` on every shot whether or not the device measured anything. [TNB]
serialises with `NullValueHandling.Ignore`, so a shot with `ContainsBallData: true` always has a
`BallData` object, but nothing stops a client sending the flag without the object. A server must
gate on the **flag**, then check the **object** is there, and report the mismatch rather than
crash or silently invent zeros (design §5.4).

### 3.5 Sign conventions

⚠ **No source states them.** What follows is inferred from what clients do to their devices'
native values before sending, which is the best available evidence for what GSPro expects:

- **`SpinAxis`**: [TL] takes the Garmin R10's spin axis, brings values above 90 into (−180, 180] by subtracting 360, then **negates** it. The R10 reports positive for a draw-side tilt (left-hander's slice), so GSPro's convention is the opposite: **negative tilts left (draw for a right-hander), positive tilts right (fade)**. The [GSP] example has `SpinAxis: -13.2` with `SideSpin: -800.0`, consistent with **negative sidespin = left**. Treat as *probable, single-source*.
- **`HLA`**: [GSP] example `2.3`; no transformation applied by any client to their device's launch direction. Devices in this space report positive = right of target. Treat as *probable*: **positive = right**.
- **`VLA`, `AngleOfAttack`, `Loft`**: positive = upward. No client transforms them.
- **`FaceToTarget`, `Path`**: no client transforms them; devices report positive = open / in-to-out for a right-hander. Treat as *probable*: **positive = right of target**.
- **Handedness** does not flip any of these in any client. Whether GSPro mirrors for a left-handed player is unknown; the `Handed` field in the 201 message exists so the *client* can decide (§5.2).

`libgspro` **passes every value through unchanged** and documents the above as unverified.
Conversion to an application's own conventions is the application's job (design §4.4).

### 3.6 Units

⚠ **What `Units` governs is not stated anywhere.** [GSP] says only `"default yards"`. [TNB]'s
enum offers `Yards` and `Meters`. No client sends `Meters`: [R10] and [TL] hard-code `"Yards"`;
[MLM] exposes it as a setting defaulting to `"Yards"`; [TNB] defaults to `YARDS` in its constants
and `METERS` in its (unused) session singleton.

What the field plausibly controls is `CarryDistance` and the 201 message's `DistanceToTarget`
(§5.2). Whether it also switches `Speed` from mph to m/s is unknown. Every client that can be
checked sends **mph** for speeds and **yards** for distances under `"Yards"`, and that pairing is
what GSPro is known to accept.

`libgspro` surfaces the string, an enum for the two known values, and converts nothing.

---

## 4. Client behaviour over a connection

### 4.1 Connect, then send

There is no handshake, hello or registration message. A client connects and its first message
is whatever it has to say. [R10] sends a heartbeat immediately on connect ("PingTimer … 0, 0",
fired once); [OSP] sends a heartbeat carrying its ready flag on connect and then reads; [MLM],
[TL], [GC2] send nothing until a shot; [TNB] sends nothing until asked.

⚠ **A server therefore learns the client's `DeviceID` only from its first message, and may
learn nothing about a client for minutes.** A connection with no traffic is normal and must not
be dropped for silence.

### 4.2 Heartbeat and status messages

A heartbeat is the shot message with `ContainsBallData: false`, `ContainsClubData: false`,
`IsHeartBeat: true`, and typically `ShotNumber: 0` [R10] or the next counter value [TNB].
[R10] sends `LaunchMonitorIsReady` and `LaunchMonitorBallDetected` in the same message,
combining heartbeat with status.

⚠ **GSPro answers a heartbeat or status message with a 200 "Shot received successfully".**
[TNB]'s "Issues Raised With GSPro": "Launch Monitor Status Response is a valid shot message
(recommend new response code)". So a server that emulates GSPro replies 200 to *every*
well-formed message, whatever it contains, and does not count it as a shot. Design §5.5.

**Cadence.** The older clients send none: [R10]'s timer fires once on connect; [TNB] exposes
`SendHeartBeat()` and never calls it on a timer; [MLM], [TL], [KJD], [GC2] never send one, and
[TNB]'s redraft calls the heartbeat "retired". The newer ones do: every **3 s** [OB], **5 s** when
idle [OF], **5 s** always [FB]. `ShotNumber` on a heartbeat is `0` [R10] [OF], the current shot
count [OB], or **incremented per heartbeat** [FB] — so a shot counter that only counts shots is
not a safe assumption (§4.3). `LaunchMonitorBallDetected` rides on heartbeats as `true` [FB] or
`false` [OF] [OB], carrying real state. A server must not require heartbeats and must read the
status flags off them.

⚠ **Whether GSPro answers a heartbeat at all is contested.** [TNB] says it does, with a 200 ("a
valid shot message"). [PIT]'s code carries the comment "Currently, it doesn't appear we get a
response for a keep-alive ?". Both observed a live GSPro. `libgspro` replies (§9.4): a reply
nobody expects is harmless, and a missing reply to a client that does expect one is not (U13).

### 4.3 Shot numbering

`ShotNumber` is the client's own counter. It starts at 1 [MLM] [TL] [TNB] or wherever the
client likes, increments per *shot* [MLM] [TL] or per *message* including heartbeats [TNB],
restarts when the client restarts, and is `0` for heartbeats in [R10].

⚠ **It is not a reliable de-duplication key** across reconnects, and two clients on one server
have independent counters. It is provenance, in the same sense PinPoint Studio treats a
GCQuad's shot id — recorded, never used to decide anything. [TNB] raised with the vendor the
absence of any request id echoed in responses, which confirms none exists.

### 4.4 What a client expects back, and how quickly

Every client that checks a reply waits for a **single** response after sending a shot:

- [MLM] blocks on `recv(2048)` with a **2 s** socket timeout and retries the send once on timeout, then reports failure. ⚠ **This means a server must reply to a shot within 2 s or the connector will re-send the shot** — a duplicate with the same `ShotNumber`.
- [TNB] blocks up to 10 s on its receive queue.
- [OCR] polls for a 200 for up to **10 s** after each shot, and before sending drains anything unsolicited (a 201) that arrived while idle.
- [R10], [TL], [KJD], [OF], [FB] fire and forget; replies are handled asynchronously if at all. [OB]'s bridge and [GC2] discard replies entirely.
- [PIT] handles replies asynchronously, and (§2) **its receive thread dies on any reply it cannot parse as exactly one object**.

⚠ **A reply must be sent promptly and must be one JSON object per message received.** A server
that replies twice to one message, or replies to nothing, breaks the request/response
assumption [MLM] and [TNB] make. Design §5.5 makes the 1:1 rule structural.

---

## 5. Server → client messages

### 5.1 Response shape

```json
{
  "Code": 201,
  "Message": "GSPro Player Information",
  "Player": {
    "Handed": "RH",
    "Club": "DR"
  }
}
```
[GSP], verbatim. `Code` is an integer, `Message` a string, `Player` an object present on 201.

| Code | Meaning | Source |
|---|---|---|
| **200** | "Shot received successfully" | [GSP]. Text quoted from [GSP]'s list; the actual string GSPro sends is not shown on the page, and [TNB] notes a spelling error in it ("Shot Reponse message spelling error. (Shot send and recieved)"). Clients key on `Code`, never on `Message` [TNB] [R10] [MLM] |
| **201** | "Player information" — `Player` object present | [GSP] |
| **202** | `"GSPro ready"` | Not on [GSP]. Enumerated by [TNB] and **handled, with the exact string, by [OSP]**, whose comment records receiving it "when a match/hole starts". [OSP] arms its launch monitor only once it has seen a 202 (`InMatch`), so a server that never sends one leaves an [OSP]-style client waiting. Sent by `libgspro` on the host's request (design §5.6) |
| **203** | `"GSPro round ended"` | Not on [GSP]. [TNB] and [OSP], the latter matching the exact string. Observed arriving concatenated with a 202 |
| **501 / 5XX** | "Failure occurred" | [GSP]. [TNB] treats any `Code >= 500` as failure. [OSG] sends `501` with a `Message` for bad JSON and `413` for oversize input |

⚠ **`Message` text is free.** [OSG] replies `{"Code":200}` with no `Message` at all and clients
accept it. [OB] replies `"Heartbeat OK"`, `"Shot received successfully"` or `"OK"` depending on
kind. [KJD] fabricates its own `"Data received successfully"`. [PIT]'s test server answers
**every** message with a 201, and PiTrac's client is content with that. A server should send a
`Message` because [GSP] shows one, and a client must not depend on it — ⚠ except that [OSP]
matches the 203 text `"GSPro round ended"` exactly, so a server sending 203 must send that
string.

### 5.2 The 201 player-information message

```json
{ "Code": 201, "Message": "GSPro Player Information", "Player": { "Handed": "RH", "Club": "DR" } }
```

| Key | Values | Source |
|---|---|---|
| `Handed` | `"RH"`, `"LH"` | [GSP] shows `"RH"`; [TNB] and [R10] enumerate both |
| `Club` | See table below | [GSP] shows `"DR"`; [TNB] and [R10] enumerate the set |
| `DistanceToTarget` | number, optional | ⚠ **Not on [GSP].** Present as a nullable float in [TNB], [R10] and [OSP] — three independent clients written against a live GSPro. [OSP] treats a non-zero value as the signal that the game has advanced and *arms its launch monitor on it*, so GSPro evidently sends it with most 201s during play. Unit presumably `Units`; unverified. Absent in [MLM]'s handling |
| `Surface` | string, optional | ⚠ Declared by [OSP] and [FB], read by neither. The lie surface, presumably ("Tee", "Fairway", "Green"…); vocabulary unknown (§11 U12) |

**Club codes.** [TNB] and [R10] agree exactly on the vocabulary; [R10] additionally has an
`unknown` member as its own default, not a wire value.

| Code | Club | | Code | Club | | Code | Club |
|---|---|---|---|---|---|---|---|
| `DR` | Driver | | `H2`–`H7` | 2–7 hybrid | | `PW` | Pitching wedge |
| `W2`–`W7` | 2–7 wood | | `I1`–`I9` | 1–9 iron | | `GW` | Gap wedge |
| | | | | | | `SW` | Sand wedge |
| | | | | | | `LW` | Lob wedge |
| | | | | | | `PT` | Putter |

**Why it exists.** [GSP]: "typically used for Launch monitors that need to switch between full
strike clubs and putting." [MLM] and [OCR] switch their device into putting mode when
`Club == "PT"`. [OSP] implements the fuller rule **client-side**, from its own configuration:
`DistanceToPtMode` 0 → putting mode when `Club` is in a configured list (default `["PT"]`);
-1 → never; > 0 → putting mode when `DistanceToTarget` ≤ that or `Club == "PT"`. So the server's
job is only to send an accurate club and distance; each client decides its own mode. This is
what PinPoint Studio's own club selection will drive (design §6).

**When GSPro sends it.** ⚠ Not stated on [GSP]. Evidence:

- It is **unsolicited**. [R10] handles it in a general receive handler that fires whenever bytes arrive; [MLM] polls the socket every 250 ms specifically to catch it; [TNB] raises it from its receive thread as a separate event from the shot reply. None of them requests it.
- It is sent **when the club changes in the game**. [CL] 2.0.4.1: "FIXED: Club not reporting to launch monitor on practice range after club has been manually changed" — i.e. a club change is supposed to reach the launch monitor, and there is only one message that can carry it.
- It is probably sent **after a shot is processed**, when the game advances and the club or distance changes. [MLM]'s regex splitter exists because a 200 and a 201 arrive **concatenated** in one read after a shot — the 201 following the 200 is the ordinary case.
- Whether it is sent **on connect** is unknown. The only forum evidence found is a user who connected and saw no messages until a shot was hit, which is weak evidence that GSPro does *not* announce on connect. [OB], an independent server, greets every connection with a 201 and comments "like GSPro does" — the author's belief, not a capture.
- ⚠ **A zero-speed shot resets GSPro's club selection.** [SLX] exists because SLX Connect "sometimes" sends a shot with `Speed: 0`, which made GSPro reset the club — "it makes it impossible to change clubs at specific times" — and the proxy's fix is to rewrite any shot whose ball or club speed is zero into a heartbeat before it reaches GSPro. So GSPro treats a zero-speed shot as a shot, and its club logic reacts to shots. A server emulating GSPro should **not** copy that; a server *validating* shots should treat zero ball speed as "not a shot" (design §4.5). `libgspro` makes announcing on connect a policy choice, default ON, because a connector that needs to know the club before the first strike is better served by it and no client is harmed by an extra 201 (design §5.6).

### 5.3 Error responses

[GSP] lists "501/5XX: Failure occurred" and nothing else. When GSPro sends them, and what for,
is not documented. [OSG], as an independent server, sends:

- `{"Code":501,"Message":"Bad JSON data"}` for unparseable input,
- `{"Code":501,"Message":"Expected JSON object"}` for a JSON value that is not an object,
- `{"Code":501,"Message":"Missing or invalid shot data"}` when `ShotDataOptions.ContainsBallData` is not true or `BallData` is missing — ⚠ so [OSG] rejects heartbeats and status messages with 501, which GSPro does not (§4.2). `libgspro` does not copy that,
- `{"Code":413,"Message":"Payload too large"}` for over 64 KiB pending.

`libgspro`'s error policy is design §5.5; it uses 501 for "could not parse" and keeps the 5XX
space for the failures a server can actually detect.

---

## 6. Message catalogue

Every message either side sends, in one place.

| Direction | Kind | Discriminator | Reply |
|---|---|---|---|
| client → server | Shot | `ContainsBallData` or `ContainsClubData` true | one 200 (or 5XX) |
| client → server | Heartbeat | both `Contains*` false, `IsHeartBeat` true | one 200 |
| client → server | Status | both `Contains*` false, `IsHeartBeat` false/absent | one 200 |
| server → client | Acknowledgement | `Code` 200 | — |
| server → client | Player information | `Code` 201, `Player` present | — |
| server → client | Failure | `Code` ≥ 500 | — |
| server → client | Ready | `Code` 202, `Message` `"GSPro ready"` | sent when the host says a session started, and on connect if one is active |
| server → client | Round ended | `Code` 203, `Message` `"GSPro round ended"` | sent when the host says the session ended |

---

## 7. Worked example — one shot

Client connects to 127.0.0.1:921. Client writes (compact, no delimiter):

```json
{"DeviceID":"PinPoint LM 1.0","Units":"Yards","ShotNumber":1,"APIversion":"1","BallData":{"Speed":147.5,"SpinAxis":-13.2,"TotalSpin":3250.0,"HLA":2.3,"VLA":14.3},"ShotDataOptions":{"ContainsBallData":true,"ContainsClubData":false}}
```

Server writes, within 2 s (§4.4):

```json
{"Code":200,"Message":"Shot received successfully"}
```

Later, the game changes club. Server writes, unsolicited, possibly in the same TCP segment as
the acknowledgement above:

```json
{"Code":201,"Message":"GSPro Player Information","Player":{"Handed":"RH","Club":"PT","DistanceToTarget":12.5}}
```

A client reading that segment sees `{"Code":200,...}{"Code":201,...}` and must split it (§2).

---

## 8. What the protocol does not carry

Read this before looking for a field that is not there.

- **No shot identity beyond the client's counter** (§4.3), and nothing echoed in a reply. A client cannot match a 200 to a specific shot except by order.
- **No timestamp.** Neither direction carries one. The time of a shot is the time its bytes arrived, and the arrival stamp is the server's.
- **No device identity beyond `DeviceID`**, which names software, not hardware, and is chosen by the client author.
- **No flight results from the server** — carry, total, offline are computed by the game and never sent back. `DistanceToTarget` is the only game state exposed.
- **No negotiation.** `APIversion` has one value; no capability exchange exists.
- **No message from the server that a client must answer.** Everything from the server is informational.
- **No confirmation of a putting-mode switch**, and no round or hole state beyond the club, the distance, the surface, and the coarse 202/203 "ready" / "round ended" pair.

---

## 9. Robustness rules a server must follow

Each of these is a consequence of a specific observation above.

### 9.1 Key matching is case-insensitive

Because `APIVersion` [R10] and `Backspin` [MLM] are sent by clients that work against GSPro
(§3.1, §3.2). Duplicate keys: last one wins, as in every JSON library the clients use.

### 9.2 Bound the receive buffer and resynchronise

A message is normally 300–900 bytes ([GSP]'s full example is ~850 bytes indented). [OSG] bounds
at 64 KiB. With no framing, a client that sends garbage leaves the server unable to find the
next message boundary; the only recovery is to discard the buffer, reply 5XX, and start again
at the next `{`. Design §5.3 sets the bound and the resynchronisation rule.

### 9.3 Numbers are doubles; integers may arrive as floats

`ShotNumber: 13.0` [OSG]. `BackSpin: 10454.780354572425` [OSG]. Read every number as a double
and convert `ShotNumber` with a range check.

### 9.4 Reply exactly once per message, promptly

§4.4. A missing reply re-sends the shot from [MLM] after 2 s; an extra reply desynchronises
[TNB]'s queue.

### 9.5 Never require optional keys; never trust `0.0`

§3.2, §3.3. Presence is a fact; zero is not a fact about the world.

### 9.6 Never require heartbeats; never drop for silence

§4.1, §4.2. A client may connect and say nothing for the length of a warm-up.

### 9.8 One write per message, and space them for fragile readers

§2: [PIT], [TNB], [TL], [TG], [SB] and [SLX]'s monitor side all parse **one read as one
message**. Two replies landing in one TCP read are lost by all of them, and for [PIT] they
cost the connection. GSPro itself is observed doing this ([OSP], [SLX], [MLM]) — which is why
those clients grew splitters — but a server that wants the widest compatibility issues one
socket write per message, sets `TCP_NODELAY`, and can space consecutive writes to one client
by a few milliseconds (design §5.5). This does not make coalescing impossible; nothing can.

### 9.7 Tolerate the absent and the extra

Unknown keys are ignored (every client's JSON library ignores unknown keys on the response
side, and a future GSPro field must not break a server). Absent `Units` means yards [GSP].
`ShotDataOptions` is sent by every client examined, but a server should treat its absence as both
flags false, classify the message as status, and flag it.

---

## 10. Server checklist

1. Listen on a configurable port, default 921, on an address the host chooses (§1).
2. Accept a connection; expect nothing from it for an arbitrary time (§4.1).
3. Accumulate bytes; extract JSON objects by string-aware brace depth; bound the buffer (§2, §9.2).
4. Parse each object with case-insensitive keys, all numbers as double (§9.1, §9.3).
5. Classify by `ShotDataOptions` (§3.4); check the sections the flags promise are present.
6. Reply **one** `{"Code":200,"Message":"..."}` per well-formed object, whatever its kind; `5XX` per unparseable one (§4.2, §5.3, §9.4). One socket write per message, `TCP_NODELAY`, optionally spaced (§9.8).
7. Surface shots with per-field presence and the raw `Units` string; convert nothing (§3.2, §3.6).
8. Surface ready / ball-detected / heartbeat as status, not shots (§3.4).
9. Send `{"Code":201,...,"Player":{...}}` unsolicited whenever the application's club, handedness or distance changes, and optionally on connect (§5.2).
10. Send `{"Code":202,"Message":"GSPro ready"}` when a session starts and `{"Code":203,"Message":"GSPro round ended"}` when it ends, with those exact strings (§5.1); never require a reply to anything (§8).

---

## 11. Unknowns, and how each will be closed

| # | Unknown | Best current evidence | How to close |
|---|---|---|---|
| U1 | Whether GSPro sends 201 on connect | Weak evidence it does not (§5.2) | Connect a raw socket to a real GSPro Connect and observe |
| U2 | Whether GSPro matches keys case-insensitively | [MLM]'s `Backspin` works, but `TotalSpin` may be what GSPro uses | Send a shot with only `Backspin`/`SideSpin` and no `TotalSpin` to a real GSPro; see if it renders |
| U3 | What `Units: "Meters"` changes | Nothing (§3.6) | Send identical shots under both and compare rendered carry |
| U4 | The exact `Message` string of a 200 | "Shot received successfully" per [GSP]'s list; [TNB] says it is misspelled on the wire | Capture one |
| U5 | Exactly when 202 / 203 are sent | Exist, with exact strings, per [OSP]: "when a match/hole starts", the pair sometimes together | Capture a session across a round start and end |
| U6 | GSPro's reply to a malformed message, if any | None | Send garbage; observe |
| U7 | Sign conventions | Inferred from [TL] and [GSP]'s example (§3.5) | Send a known-draw shot; observe flight |
| U8 | Whether a club-data-only shot renders | [TNB] offers it | Send one |
| U9 | Whether GSPro accepts >1 concurrent client on 921 | Product supports a putting device beside a full-swing device | Connect two |
| U10 | Whether GSPro ever closes the connection itself | [MLM] handles a zero-length read as "GSPro closed the connection" | Observe across a game exit |
| ~~U11~~ | ~~Whether GSPro Connect's listening port is configurable~~ | **Closed by [SLX]**: `<OpenAPIUseAltPort>true</OpenAPIUseAltPort>` in `GSPconnect.exe.config` moves it to 922. Two values only. Design §6.4 | — |
| U12 | The `Surface` vocabulary and when it is sent | [OSP] and [FB] declare the field; neither reads it | Capture 201s across tee, fairway, rough, sand and green |
| U13 | Whether GSPro replies to a heartbeat | [TNB] says 200; [PIT]'s comment says apparently nothing | Send one; capture |
| U14 | What GSPro does with a zero-speed shot | [SLX]: it resets the club selection, and the fix was to suppress such shots | Send one; capture the 201 that presumably follows |

None of U1–U10 blocks a server that follows §9: each is either a policy `libgspro` exposes, or a
behaviour the server tolerates both ways. They are listed so that the first session against a
real launch monitor and a real GSPro is spent closing them, and so that a wire capture of that
session (design §7) becomes the fixture that pins the answers.

---

## Appendix A — the official page, verbatim

Fetched from https://gsprogolf.com/GSProConnectV1.html on 2026-09-09. Reproduced in full because
it is the only primary source and is short. Spelling and comments are the vendor's.

> **GSPro Open Connect v1 Documentation**
>
> The following describes how to connect to the GSPro Connect interface with the "Open Connect" protocol.
>
> **Summary**
>
> The overall process breaks down very simple; GSPro Connect opens a socket and listens for an incoming client. Once connected is established a constant 2-way communication continues:
>
> To GSPro Connect from Launch Monitor client:
> - Shot Data
> - Ready signals
> - Heart beat
>
> From GSPro to Client
> - General response from incoming messages (success/failure)
> - Player information
>
> **Socket information**
>
> The socket connection is open and does not require authentication. Note: It is assumed that the launch monitor software is being ran on the same PC as GSPro connect. If this is not the case, firewall and port forwarding may be required
>
> Port: 0921
> IP Address: 127.0.0.1
>
> **Launch Monitor to GSPro Connect JSON**
>
> The JSON required to send a shot is straight forward and readable. The two more important sections are the root properties and BallData object. The other area of important is the shotData properties to describe what data is being sent in.

```
{
"DeviceID": "GSPro LM 1.1",  			//required - unqiue per launch monitor / prooject type
"Units": "Yards",						//default yards
"ShotNumber": 13,						//required - auto increment from LM
"APIversion": "1",						//required - "1" is current version
"BallData": {
	"Speed": 147.5,						//required
	"SpinAxis": -13.2,					//required
	"TotalSpin": 3250.0,				//required
	"BackSpin": 2500.0,					//only required if total spin is not sent
	"SideSpin": -800.0,					//only required if total spin is not sent
	"HLA": 2.3,							//required
	"VLA": 14.3,						//required
	"CarryDistance": 256.5				//optional
},
"ClubData": {
	"Speed": 0.0,
	"AngleOfAttack": 0.0,
	"FaceToTarget": 0.0,
	"Lie": 0.0,
	"Loft": 0.0,
	"Path": 0.0,
	"SpeedAtImpact": 0.0,
	"VerticalFaceImpact": 0.0,
	"HorizontalFaceImpact": 0.0,
	"ClosureRate": 0.0
},
"ShotDataOptions": {
	"ContainsBallData": true,			//required
	"ContainsClubData": false,			//required
	"LaunchMonitorIsReady": true, 		//not required
	"LaunchMonitorBallDetected": true, 	//not required
	"IsHeartBeat": false 				//not required
}
	}
```

> **GSPro Connect to Launch Monitor JSON**
>
> The response from GSPro Connect is currently very simple. The two common responses you will receive is 1) 200 response confirming that we received and processed your JSON and 2) Player information for handedness and club, The latter is typically used for Launch monitors that need to switch between full strike clubs and putting.

```
{
	"Code": 201,
	"Message": "GSPro Player Information",
	"Player": {
		"Handed": "RH",
		"Club": "DR"
	}
}
```

> Additional documentation will be added to describe additional response codes. Currently:
>
> 200: Shot received successfully
> 201: Player information
> 501/5XX: Failure occurred

## Appendix B — the reference client's corrections to the page

[TNB] ships `OpenAPI-Documentation-Feedback.MD`, a redraft of Appendix A with these
substantive changes (everything else is copy-editing):

- Summary: "Ready signals **(currently not implemented)**", "Heart beat **(retired)**".
- `Units`: "**optional** - default yards".
- `IsHeartBeat`: "optional **(retired)**".
- Adds "implemented using a tcp server" to the first line.

And a list of issues the author raised with the vendor, quoted in full:

> - Passing and Id in the requests which is returned in the responses so that response can be matched to requests
> - Shot Reponse message spelling error. (Shot send and recieved)
> - Launch Monitor Status Response is a valid shot message (recommend new response code)

The third is the source for §4.2's rule that every message is answered 200.
