/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/codec.h — stateless framing, decoding and encoding.
 *
 * Public so that a tool, a test or a binding can frame and parse Open Connect
 * bytes WITHOUT a server: gsp_shoot.py builds messages with the encoder half,
 * the fuzz target aims at the decoder, and PinPoint's unit tests can decode a
 * string literal.  The server (server.h) is built on exactly these functions
 * and adds connection state, replies and events.
 *
 * ⚠ EVERY FUNCTION HERE MUST NEVER READ PAST `len`.  The socket is open and
 * unauthenticated (design §9.3); this is the component fuzzing is aimed at.
 */
#ifndef GSPRO_CODEC_H
#define GSPRO_CODEC_H

#include "gspro/types.h"
#include "gspro/message.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Framing (protocol §2, design §5.3)                                        */
/* ------------------------------------------------------------------------ */
/*
 * THERE IS NO FRAMING ON THE WIRE.  Messages are JSON objects written back to
 * back with nothing between them, sometimes with whitespace, sometimes
 * indented with newlines INSIDE them [TNB].  A read may hold part of one, one,
 * or several.  The only delimiter is the JSON itself.
 *
 * gsp_frame_find() scans `buf[0..len)` for the first complete top-level object,
 * counting brace depth OUTSIDE string literals (a brace inside "DeviceID" must
 * not count) and skipping whitespace between objects.
 *
 *   GSP_OK              a complete object occupies [*start, *end)
 *   GSP_PENDING         the buffer ends inside an object (or is all whitespace);
 *                       *start is where it began, or len if nothing began
 *   GSP_ERR_MALFORMED   a non-whitespace, non-'{' byte precedes any object:
 *                       *start is its offset; discard through it and rescan.
 *                       ⚠ A top-level '[' is malformed: the protocol has no
 *                       arrays at top level and treating one as a message
 *                       would be inventing a protocol
 *
 * Stateless, so O(n) per call over the pending buffer; the server keeps an
 * incremental scanner internally and does not rescan.
 */
GSP_API gsp_status gsp_frame_find(const uint8_t *buf, size_t len, size_t *start, size_t *end);

/* ------------------------------------------------------------------------ */
/* Decoding (protocol §3, §9; design §5.4)                                   */
/* ------------------------------------------------------------------------ */
/*
 * Decodes ONE complete object — exactly the bytes gsp_frame_find() delimited —
 * into `out`, which is fully written (zeroed first).  conn, sequence and
 * host_recv_us are left at GSP_CONN_NONE / 0 / GSP_TIME_UNKNOWN for the server
 * to fill.
 *
 * Tolerances, each with a protocol section that demands it:
 *   keys match case-insensitively            §9.1  ("APIVersion", "Backspin")
 *   every number is read as double           §9.3  (ShotNumber 13.0)
 *   unknown keys and nested values skipped   §9.7  (counted in unknown_keys)
 *   duplicate keys: last wins                §9.1
 *   strings truncate to *_MAX and are flagged design §5.4
 *
 * Returns GSP_OK for anything that is a JSON object, however incomplete — the
 * findings are in out->flags (design §4.5).  GSP_ERR_MALFORMED only for bytes
 * that are not a JSON object at all.
 */
GSP_API gsp_status gsp_message_decode(const uint8_t *json, size_t len, gsp_message *out);

/* ------------------------------------------------------------------------ */
/* Encoding (protocol §5)                                                    */
/* ------------------------------------------------------------------------ */
/*
 * Composes a server→client response.  `message` may be NULL to omit the
 * Message member ([OSG] does; clients accept it) — the server never does.
 * `player` non-NULL adds the Player object with UNKNOWN members omitted.
 *
 * Compact, no trailing newline, no whitespace: the form every client's own
 * splitter is known to handle (protocol §2).  Writes at most `out_size` bytes
 * including the NUL; *written is the length excluding it.  Returns
 * GSP_ERR_BUFFER_TOO_SMALL and writes nothing if it does not fit.
 *
 * ⚠ This is the only path by which bytes leave the library, and `code` is
 * validated against gsp_response_code: anything else is GSP_ERR_INVALID_ARG.
 * There is no send_raw() (design §9.1).
 */
GSP_API gsp_status gsp_response_encode(int code, const char *message,
                                       const gsp_player_info *player,
                                       char *out, size_t out_size, size_t *written);

/*
 * Composes a CLIENT→server message from a gsp_message — the other direction,
 * for tools and tests that play a launch monitor at THIS library.  Fields whose
 * presence bit is clear are omitted; `indent` writes the [TNB] indented form.
 *
 * ⚠ For talking to a libgspro listener in a test.  This library is not a GSPro
 * client (design §1) and the tools that use this refuse to aim it at port 921
 * on a non-loopback address.
 */
GSP_API gsp_status gsp_message_encode(const gsp_message *m, bool indent,
                                      char *out, size_t out_size, size_t *written);

/* ------------------------------------------------------------------------ */
/* Response decoding — for a test client                                     */
/* ------------------------------------------------------------------------ */
#define GSP_RESPONSE_TEXT_MAX 96

typedef struct gsp_response {
    int32_t  code;                  /* 200, 201, 5XX, or whatever arrived  */
    uint8_t  has_message;
    uint8_t  has_player;
    uint8_t  reserved[2];
    char     message[GSP_RESPONSE_TEXT_MAX];
    gsp_player_info player;         /* meaningful if has_player            */
} gsp_response;

/* Decodes one server→client object.  Same tolerances as gsp_message_decode().
 * The emitted-bytes test (design §10.2) round-trips every write the server
 * can produce through this. */
GSP_API gsp_status gsp_response_decode(const uint8_t *json, size_t len, gsp_response *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_CODEC_H */
