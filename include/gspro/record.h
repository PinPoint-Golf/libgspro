/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/record.h — the optional `.gswire` container, and the replay that drives
 * a recorded session back through a server.
 *
 * ============================================================================
 * ⚠ THIS IS NOT PART OF THE SANS-I/O CORE.  It is a separate target, it opens
 * FILES, and tests/purity.cmake deliberately does not look at it.  The core
 * copies wire chunks into the ring the config sized and the host drains them
 * with gsp_server_poll_wire(); everything below is one host that does the
 * draining.  A consumer with its own storage — a database, a Qt log, an
 * existing capture format — writes forty lines instead and links none of this.
 * ============================================================================
 *
 * WHY IT EXISTS, and it is not "so there is a log".  protocol §11 lists ten
 * open questions this library cannot close at a desk (design §11 package 7).
 * When U2, U4 or U7 is finally answered by a launch monitor on a mat, the
 * session that answered it has to be RE-DECODABLE with the fix applied — and a
 * decoded log has already thrown away the bytes the fix would have read
 * differently.  So:
 *
 *   gsp_recorder   chunks  →  a versioned file
 *   gsp_replay     a file  →  chunks, byte-exact, in order
 *   gsp_replay_into_server()  a file → a SERVER, with a report of what it made
 *                             of the session THIS time
 *
 * ⚠ The third is the one that matters.  A capture replayed through a library
 * that has since been fixed answers "would we have got it right?", and answers
 * it without the launch monitor, the mat, or the person who owns them.  It is
 * also how a capture becomes a fixture: `gswire extract` writes one message's
 * bytes to a file in tests/fixtures/ shape.
 *
 * ⚠ A CAPTURE IS PERSONAL DATA UNTIL IT IS NOT.  A peer address identifies a
 * household and a DeviceID can carry a hardware serial ([GC2]); the core
 * redacts both unless policy.record_identifiers says otherwise, and this
 * container records WHICH of the two it was handed (`identifiers_recorded`) so
 * that nobody has to guess later.  The .gitignore refuses `*.gswire` outside
 * tests/fixtures/ for the same reason (design §9.2).
 */
#ifndef GSPRO_RECORD_H
#define GSPRO_RECORD_H

#include "gspro/server.h"
#include "gspro/codec.h"     /* the replay decodes recorded replies to classify them */
#include "gspro/version.h"   /* the writer stamps gsp_version_string() into the header */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* The container                                                             */
/* ------------------------------------------------------------------------ */
/*
 * A TEXT HEADER so file(1), `head`, and a human can identify it, then
 * fixed-size length-prefixed records.  It is the `libwrist` `.wrwire` shape
 * with a different magic and one added field — the connection id, because this
 * protocol has several clients at once where that one had a single sensor
 * (design §7).
 *
 *     GSWIRE1\n
 *     library=0.1.0\n
 *     clock=monotonic_us\n
 *     byte_order=little\n
 *     identifiers=redacted\n
 *     port=921\n
 *     note=Rapsodo MLM2PRO on the mat, 2026-09-10\n
 *     \n
 *     <record><record>...
 *
 *     offset  size  field
 *       0       4   u32  length         payload bytes after this 24-byte header
 *       4       1   u8   direction      gsp_wire_direction
 *       5       1   u8   flags          gsp_wire_flag
 *       6       2   u16  reserved       written zero, ignored on read
 *       8       4   u32  sequence       ⚠ the CORE's chunk sequence, not an ordinal
 *      12       4   u32  conn           the connection id the host chose
 *      16       8   i64  host_time_us   monotonic, arbitrary epoch
 *      24     len   payload
 *
 * ⚠ `sequence` is written rather than recomputed on read, and that is the whole
 * reason it is in the chunk at all: GSP_WIRE_LOST says chunks were dropped
 * before this one but not HOW MANY, and a reader that renumbered from its own
 * ordinal would silently turn a lossy recording into a complete-looking one.
 *
 * ⚠ Integers are LITTLE-ENDIAN and written byte by byte, never memcpy'd off the
 * struct: a container whose layout depended on the compiler that wrote it could
 * not be read by the Python half (python/gspro/wire.py), which is the whole
 * point of having a second implementation.  `byte_order=` is in the header so
 * that a reader never has to assume it.
 */
#define GSP_RECORD_MAGIC          "GSWIRE1"
#define GSP_RECORD_HEADER_MAX      1024u  /* text header; refused above this   */
#define GSP_RECORD_ENTRY_HEADER      24u
#define GSP_RECORD_CLOCK_MONOTONIC "monotonic_us"
#define GSP_RECORD_CLOCK_MAX         32
#define GSP_RECORD_NOTE_MAX         160
#define GSP_RECORD_LIBRARY_MAX       32

typedef struct gsp_recording_info {
    /* The library version that wrote it, for a capture that outlives a release. */
    char     library[GSP_RECORD_LIBRARY_MAX];

    /* ⚠ The library reads no clock (design §4.1), so this records which one the
     * HOST used.  A wall clock is a defect rather than a variant, and a capture
     * that names one is a capture whose ordering is a fiction. */
    char     clock[GSP_RECORD_CLOCK_MAX];

    /* Free text: which device was on the mat, which question the session was
     * taken to answer.  ⚠ NOT a place for a name or an address. */
    char     note[GSP_RECORD_NOTE_MAX];

    uint16_t port;                 /* the listener's port, 0 if not stated     */
    bool     identifiers_recorded; /* ⚠ policy.record_identifiers was ON       */
    uint8_t  reserved[5];
} gsp_recording_info;

/* This library's version, a monotonic clock, no note, identifiers REDACTED. */
GSP_API gsp_recording_info gsp_recording_info_default(void);

/* ------------------------------------------------------------------------ */
/* Writer                                                                    */
/* ------------------------------------------------------------------------ */
typedef struct gsp_recorder gsp_recorder;

/*
 * Creates or truncates `path` and writes the header.  GSP_ERR_INVALID_ARG on a
 * NULL argument, GSP_ERR_NO_MEMORY, GSP_ERR_INVALID_STATE if the file cannot be
 * opened — with the reason available from gsp_recorder_error().
 */
GSP_API gsp_status gsp_recorder_open(const char *path, const gsp_recording_info *info,
                                     gsp_recorder **out);

/* Appends `count` chunks in order.  A chunk whose length exceeds
 * GSP_WIRE_CHUNK_MAX is refused with GSP_ERR_INVALID_ARG and nothing is
 * written — ⚠ a refusal rather than a clamp: a truncated payload in a capture
 * is worse than no capture, because it looks like evidence. */
GSP_API gsp_status gsp_recorder_write(gsp_recorder *r, const gsp_wire_chunk *chunks,
                                      size_t count);

GSP_API uint64_t gsp_recorder_chunks(const gsp_recorder *r);
GSP_API uint64_t gsp_recorder_bytes(const gsp_recorder *r);
GSP_API const char *gsp_recorder_error(const gsp_recorder *r);

/* Flushes, closes and frees.  ⚠ Returns the FIRST write error the recorder saw,
 * so a full disk cannot end a capture quietly — a session against real hardware
 * is not repeatable. */
GSP_API gsp_status gsp_recorder_close(gsp_recorder *r);

/* ------------------------------------------------------------------------ */
/* Reader                                                                    */
/* ------------------------------------------------------------------------ */
typedef struct gsp_replay gsp_replay;

/*
 * Opens and parses the header.  GSP_ERR_MALFORMED for a wrong magic,
 * GSP_ERR_BUFFER_TOO_SMALL for a header longer than GSP_RECORD_HEADER_MAX.
 *
 * ⚠ An UNKNOWN KEY in the header is IGNORED, not an error — the same rule
 * protocol §9.7 gives for unknown JSON keys, and for the same reason: a
 * recording written by a later version must stay readable as far as this
 * version understands it.  A key it needs and cannot parse is still an error.
 */
GSP_API gsp_status gsp_replay_open(const char *path, gsp_replay **out);

GSP_API const gsp_recording_info *gsp_replay_info(const gsp_replay *r);

/*
 * Fills up to `max` chunks and returns how many — ⚠ the same drain shape as
 * gsp_server_poll_wire(), so a host that has written one loop has written both.
 * 0 means end of file OR a refusal; gsp_replay_status() tells them apart and
 * gsp_replay_error() says why in words.
 *
 * A record claiming a length above GSP_WIRE_CHUNK_MAX is GSP_ERR_MALFORMED and
 * a file that ends mid-record is GSP_ERR_BUFFER_TOO_SMALL.  ⚠ Both are
 * refusals rather than clamps: this is the one place in the project where a
 * corrupt or hostile file reaches a fixed-size buffer.
 */
GSP_API size_t gsp_replay_read(gsp_replay *r, gsp_wire_chunk *out, size_t max);

GSP_API gsp_status gsp_replay_status(const gsp_replay *r);
GSP_API const char *gsp_replay_error(const gsp_replay *r);
GSP_API uint64_t gsp_replay_chunks_read(const gsp_replay *r);
GSP_API void gsp_replay_close(gsp_replay *r);

/* ------------------------------------------------------------------------ */
/* Replay into a server — ⚠ the reason the container exists                  */
/* ------------------------------------------------------------------------ */
/*
 * Feeds every CLIENT_TO_SERVER chunk back into `server` in recorded order, on
 * the recorded connections, at the recorded times, and compares each reply the
 * server produces NOW with the reply that was recorded THEN.
 *
 * That comparison is the whole point.  When protocol §11's U2 or U7 is settled
 * and the decoder changes, this says what the change would have done to a real
 * session — without the launch monitor, the mat, or the person who owns them.
 * `replies_differing` above zero is not a failure: after a deliberate fix it is
 * the expected outcome and the thing to read.
 */
typedef struct gsp_replay_report {
    uint64_t chunks;             /* read from the file                        */
    uint64_t client_chunks;
    uint64_t server_chunks;
    uint64_t meta_chunks;
    uint64_t lost_chunks;        /* carried GSP_WIRE_LOST: the capture has holes */

    uint64_t messages;           /* the server decoded this many              */
    uint64_t shots;
    uint64_t protocol_errors;

    uint64_t replies_produced;   /* by the server, this time                  */
    uint64_t replies_matched;    /* byte for byte with the recording          */
    uint64_t replies_differing;  /* ⚠ read these; after a fix they are the point */
    uint64_t replies_missing;    /* an ACKNOWLEDGEMENT recorded then and not
                                  * produced now — ⚠ the number that means a
                                  * regression against a real session          */
    uint64_t replies_unsolicited;/* ⚠ NOT a defect: a 201/202/203 in the
                                  * recording that this server did not send
                                  * because the HOST originates those from its
                                  * own game state (design §5.6), and a capture
                                  * holds the bytes, not the state behind them  */
    uint64_t replies_extra;      /* produced now, not recorded then           */

    uint32_t connections;        /* distinct ids seen                         */
    uint32_t flags_seen;         /* OR of every message's gsp_message_flag bits */
} gsp_replay_report;

/*
 * ⚠ IT DRAINS THE SERVER'S EVENTS AND WRITES, because it has to look at them to
 * report.  A caller that wants the events themselves drives the chunks itself —
 * gsp_replay_read() into gsp_server_on_bytes() is four lines, and `gswire
 * replay` prints what this returns.
 *
 * ⚠ It does NOT create or destroy the server: a caller replays into a server
 * configured the way the question needs (a different policy, a bigger ring),
 * which is most of why a replay is useful at all.
 */
GSP_API gsp_status gsp_replay_into_server(gsp_replay *r, gsp_server *server,
                                          gsp_replay_report *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_RECORD_H */
