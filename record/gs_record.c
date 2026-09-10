/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_record.c — the `.gswire` container and the replay.
 *
 * ⚠ READ include/gspro/record.h FIRST: it carries the layout and the reasons.
 * This file is the how.
 *
 * ⚠ IT OPENS FILES, WHICH IS WHY IT IS NOT IN src/.  tests/purity.cmake fails
 * the build if the CORE ever references fopen; this target is made of it.
 */
#include "gspro/record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Little-endian, by hand                                                    */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ NEVER memcpy OFF THE STRUCT.  gsp_wire_chunk's layout is the compiler's
 * business — padding, alignment, and one day a new field — and a container that
 * inherited it could not be read by python/gspro/wire.py, which is the whole
 * point of having a second implementation of this format (design §8).
 */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_i64(uint8_t *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    int i;
    for (i = 0; i < 8; ++i) {
        p[i] = (uint8_t)((u >> (8 * i)) & 0xffu);
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static int64_t get_i64(const uint8_t *p)
{
    uint64_t u = 0u;
    int i;
    for (i = 7; i >= 0; --i) {
        u = (u << 8) | (uint64_t)p[i];
    }
    return (int64_t)u;
}

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    size_t i;
    if (dst_size == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1u < dst_size && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ------------------------------------------------------------------------ */
/* Recording info                                                            */
/* ------------------------------------------------------------------------ */
gsp_recording_info gsp_recording_info_default(void)
{
    gsp_recording_info info;
    memset(&info, 0, sizeof(info));
    copy_bounded(info.library, sizeof(info.library), gsp_version_string());
    copy_bounded(info.clock, sizeof(info.clock), GSP_RECORD_CLOCK_MONOTONIC);
    /* ⚠ Redacted is the DEFAULT here as it is in the policy (design §9.2): a
     * capture that says "identifiers=recorded" must be one somebody chose. */
    info.identifiers_recorded = false;
    return info;
}

/* ------------------------------------------------------------------------ */
/* Writer                                                                    */
/* ------------------------------------------------------------------------ */
struct gsp_recorder {
    FILE              *f;
    gsp_recording_info info;
    uint64_t           chunks;
    uint64_t           bytes;
    gsp_status         first_error;   /* ⚠ the FIRST, kept for close()         */
    char               error[128];
};

static void recorder_fail(gsp_recorder *r, gsp_status st, const char *what)
{
    if (r->first_error >= GSP_OK) {
        r->first_error = st;
        copy_bounded(r->error, sizeof(r->error), what);
    }
}

/* ⚠ A value that could contain a newline would end the header line and turn the
 * rest of the note into a key nobody wrote.  Newlines and '=' are the only
 * characters that can do that, and both become a space. */
static void write_header_value(FILE *f, const char *key, const char *value)
{
    size_t i;
    (void)fputs(key, f);
    (void)fputc('=', f);
    for (i = 0; value[i] != '\0'; ++i) {
        char c = value[i];
        (void)fputc((c == '\n' || c == '\r') ? ' ' : c, f);
    }
    (void)fputc('\n', f);
}

gsp_status gsp_recorder_open(const char *path, const gsp_recording_info *info,
                             gsp_recorder **out)
{
    gsp_recorder *r;
    char port_text[16];

    if (path == NULL || out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    *out = NULL;
    r = (gsp_recorder *)calloc(1u, sizeof(*r));
    if (r == NULL) {
        return GSP_ERR_NO_MEMORY;
    }
    r->info = (info != NULL) ? *info : gsp_recording_info_default();
    if (r->info.library[0] == '\0') {
        copy_bounded(r->info.library, sizeof(r->info.library), gsp_version_string());
    }
    if (r->info.clock[0] == '\0') {
        copy_bounded(r->info.clock, sizeof(r->info.clock), GSP_RECORD_CLOCK_MONOTONIC);
    }

    r->f = fopen(path, "wb");
    if (r->f == NULL) {
        copy_bounded(r->error, sizeof(r->error), "cannot open the file for writing");
        free(r);
        return GSP_ERR_INVALID_STATE;
    }

    (void)fputs(GSP_RECORD_MAGIC "\n", r->f);
    write_header_value(r->f, "library", r->info.library);
    write_header_value(r->f, "clock", r->info.clock);
    write_header_value(r->f, "byte_order", "little");
    write_header_value(r->f, "identifiers",
                       r->info.identifiers_recorded ? "recorded" : "redacted");
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)r->info.port);
    write_header_value(r->f, "port", port_text);
    if (r->info.note[0] != '\0') {
        write_header_value(r->f, "note", r->info.note);
    }
    (void)fputc('\n', r->f);   /* the blank line that ends the header */
    if (ferror(r->f) != 0) {
        recorder_fail(r, GSP_ERR_INVALID_STATE, "the header could not be written");
    }
    *out = r;
    return GSP_OK;
}

gsp_status gsp_recorder_write(gsp_recorder *r, const gsp_wire_chunk *chunks, size_t count)
{
    size_t i;

    if (r == NULL || (chunks == NULL && count > 0u)) {
        return GSP_ERR_INVALID_ARG;
    }
    /* ⚠ EVERY chunk is checked BEFORE the first is written: a partial batch
     * would leave the file with half a caller's intent in it and no way to say
     * which half. */
    for (i = 0; i < count; ++i) {
        if ((size_t)chunks[i].length > (size_t)GSP_WIRE_CHUNK_MAX) {
            return GSP_ERR_INVALID_ARG;
        }
    }
    for (i = 0; i < count; ++i) {
        uint8_t head[GSP_RECORD_ENTRY_HEADER];
        const gsp_wire_chunk *c = &chunks[i];

        memset(head, 0, sizeof(head));
        put_u32(head + 0, (uint32_t)c->length);
        head[4] = c->direction;
        head[5] = c->flags;
        put_u16(head + 6, 0u);
        put_u32(head + 8, c->sequence);
        put_u32(head + 12, c->conn);
        put_i64(head + 16, c->host_time_us);

        if (fwrite(head, 1u, sizeof(head), r->f) != sizeof(head)) {
            recorder_fail(r, GSP_ERR_INVALID_STATE, "a record header could not be written");
            return r->first_error;
        }
        if (c->length > 0u
            && fwrite(c->data, 1u, (size_t)c->length, r->f) != (size_t)c->length) {
            recorder_fail(r, GSP_ERR_INVALID_STATE, "a record payload could not be written");
            return r->first_error;
        }
        r->chunks++;
        r->bytes += (uint64_t)sizeof(head) + (uint64_t)c->length;
    }
    /*
     * ⚠ FLUSHED AFTER EVERY BATCH, and that is not caution for its own sake.
     * A capture is taken ONCE, beside a launch monitor that is not coming back
     * (design §11 package 7), and a host killed with what is still in stdio's
     * buffer loses the whole session rather than the last few chunks — which is
     * exactly what happened the first time this was tried against a listener
     * shut down with SIGTERM: a zero-byte file.  Messages arrive seconds apart,
     * so the cost is nothing anybody can measure.
     */
    if (count > 0u && fflush(r->f) != 0) {
        recorder_fail(r, GSP_ERR_INVALID_STATE, "the capture could not be flushed");
    }
    return (r->first_error < GSP_OK) ? r->first_error : GSP_OK;
}

uint64_t gsp_recorder_chunks(const gsp_recorder *r)
{
    return (r != NULL) ? r->chunks : 0u;
}

uint64_t gsp_recorder_bytes(const gsp_recorder *r)
{
    return (r != NULL) ? r->bytes : 0u;
}

const char *gsp_recorder_error(const gsp_recorder *r)
{
    return (r != NULL) ? r->error : "";
}

gsp_status gsp_recorder_close(gsp_recorder *r)
{
    gsp_status st;

    if (r == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (fflush(r->f) != 0 || ferror(r->f) != 0) {
        /* ⚠ A FULL DISK MUST NOT END A CAPTURE QUIETLY.  A session against real
         * hardware is not repeatable; the caller has to learn now, while the
         * launch monitor is still on the mat. */
        recorder_fail(r, GSP_ERR_INVALID_STATE, "the capture could not be flushed");
    }
    (void)fclose(r->f);
    st = (r->first_error < GSP_OK) ? r->first_error : GSP_OK;
    free(r);
    return st;
}

/* ------------------------------------------------------------------------ */
/* Reader                                                                    */
/* ------------------------------------------------------------------------ */
struct gsp_replay {
    FILE              *f;
    gsp_recording_info info;
    uint64_t           chunks;
    gsp_status         status;      /* GSP_OK while readable, GSP_PENDING at EOF */
    char               error[128];
};

static void replay_fail(gsp_replay *r, gsp_status st, const char *what)
{
    if (r->status >= GSP_OK) {
        r->status = st;
        copy_bounded(r->error, sizeof(r->error), what);
    }
}

/* One header line, without its newline.  false at end of file. */
static bool read_line(FILE *f, char *out, size_t out_size, size_t *consumed)
{
    size_t n = 0u;
    int c;

    for (;;) {
        c = fgetc(f);
        if (c == EOF) {
            out[(n < out_size) ? n : out_size - 1u] = '\0';
            return n > 0u;
        }
        (*consumed)++;
        if (c == '\n') {
            break;
        }
        if (n + 1u < out_size) {
            out[n] = (char)c;
        }
        n++;
    }
    if (n >= out_size) {
        n = out_size - 1u;
    }
    out[n] = '\0';
    return true;
}

gsp_status gsp_replay_open(const char *path, gsp_replay **out)
{
    gsp_replay *r;
    char line[GSP_RECORD_HEADER_MAX];
    size_t consumed = 0u;

    if (path == NULL || out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    *out = NULL;
    r = (gsp_replay *)calloc(1u, sizeof(*r));
    if (r == NULL) {
        return GSP_ERR_NO_MEMORY;
    }
    r->info = gsp_recording_info_default();
    r->info.library[0] = '\0';   /* what the FILE says, not what we are */

    r->f = fopen(path, "rb");
    if (r->f == NULL) {
        free(r);
        return GSP_ERR_INVALID_STATE;
    }
    if (!read_line(r->f, line, sizeof(line), &consumed) || strcmp(line, GSP_RECORD_MAGIC) != 0) {
        (void)fclose(r->f);
        free(r);
        return GSP_ERR_MALFORMED;
    }

    for (;;) {
        char *eq;
        if (consumed > (size_t)GSP_RECORD_HEADER_MAX) {
            (void)fclose(r->f);
            free(r);
            return GSP_ERR_BUFFER_TOO_SMALL;
        }
        if (!read_line(r->f, line, sizeof(line), &consumed)) {
            (void)fclose(r->f);
            free(r);
            return GSP_ERR_MALFORMED;   /* header never ended */
        }
        if (line[0] == '\0') {
            break;                      /* the blank line: records follow */
        }
        eq = strchr(line, '=');
        if (eq == NULL) {
            continue;                   /* not a key: ignore, as below */
        }
        *eq = '\0';
        {
            const char *key = line;
            const char *value = eq + 1;
            if (strcmp(key, "library") == 0) {
                copy_bounded(r->info.library, sizeof(r->info.library), value);
            } else if (strcmp(key, "clock") == 0) {
                copy_bounded(r->info.clock, sizeof(r->info.clock), value);
            } else if (strcmp(key, "note") == 0) {
                copy_bounded(r->info.note, sizeof(r->info.note), value);
            } else if (strcmp(key, "identifiers") == 0) {
                r->info.identifiers_recorded = (strcmp(value, "recorded") == 0);
            } else if (strcmp(key, "port") == 0) {
                r->info.port = (uint16_t)strtoul(value, NULL, 10);
            } else if (strcmp(key, "byte_order") == 0) {
                if (strcmp(value, "little") != 0) {
                    (void)fclose(r->f);
                    free(r);
                    return GSP_ERR_NOT_SUPPORTED;
                }
            } else {
                /* ⚠ AN UNKNOWN KEY IS IGNORED, not an error: a capture written
                 * by a later version stays readable as far as this version
                 * understands it (protocol §9.7's rule, one register up). */
                continue;
            }
        }
    }
    *out = r;
    return GSP_OK;
}

const gsp_recording_info *gsp_replay_info(const gsp_replay *r)
{
    return (r != NULL) ? &r->info : NULL;
}

size_t gsp_replay_read(gsp_replay *r, gsp_wire_chunk *out, size_t max)
{
    size_t n = 0u;

    if (r == NULL || out == NULL || max == 0u || r->status != GSP_OK) {
        return 0u;
    }
    while (n < max) {
        uint8_t head[GSP_RECORD_ENTRY_HEADER];
        size_t got = fread(head, 1u, sizeof(head), r->f);
        uint32_t length;
        gsp_wire_chunk *c;

        if (got == 0u) {
            r->status = GSP_PENDING;    /* clean end of file */
            break;
        }
        if (got != sizeof(head)) {
            replay_fail(r, GSP_ERR_BUFFER_TOO_SMALL, "the file ends inside a record header");
            break;
        }
        length = get_u32(head + 0);
        /* ⚠ A REFUSAL, NOT A CLAMP.  This is the one place a corrupt or hostile
         * file reaches a fixed-size buffer, and a clamp would put half a
         * message into a capture that looks like evidence. */
        if (length > (uint32_t)GSP_WIRE_CHUNK_MAX) {
            replay_fail(r, GSP_ERR_MALFORMED, "a record claims more than GSP_WIRE_CHUNK_MAX");
            break;
        }
        c = &out[n];
        memset(c, 0, sizeof(*c));
        c->length = (uint16_t)length;
        c->direction = head[4];
        c->flags = head[5];
        (void)get_u16(head + 6);     /* reserved: written zero, ignored here */
        c->sequence = get_u32(head + 8);
        c->conn = get_u32(head + 12);
        c->host_time_us = get_i64(head + 16);
        if (length > 0u && fread(c->data, 1u, (size_t)length, r->f) != (size_t)length) {
            replay_fail(r, GSP_ERR_BUFFER_TOO_SMALL, "the file ends inside a record payload");
            break;
        }
        r->chunks++;
        n++;
    }
    return n;
}

gsp_status gsp_replay_status(const gsp_replay *r)
{
    if (r == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    /* GSP_PENDING is this reader's "clean end of file": non-negative, so a
     * caller testing `< GSP_OK` sees success, which is what it is. */
    return r->status;
}

const char *gsp_replay_error(const gsp_replay *r)
{
    return (r != NULL) ? r->error : "";
}

uint64_t gsp_replay_chunks_read(const gsp_replay *r)
{
    return (r != NULL) ? r->chunks : 0u;
}

void gsp_replay_close(gsp_replay *r)
{
    if (r == NULL) {
        return;
    }
    if (r->f != NULL) {
        (void)fclose(r->f);
    }
    free(r);
}

/* ------------------------------------------------------------------------ */
/* Replay into a server                                                      */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THE COMPARISON IS THE PRODUCT.  Feeding the bytes back is easy; saying what
 * the library makes of them NOW against what it said THEN is the thing that
 * answers "would the fix have helped?" without a launch monitor in the room.
 */
#define GS_REPLAY_CONNS   16u
#define GS_REPLAY_PENDING 32u

typedef struct gs_replay_state {
    gsp_conn_id ids[GS_REPLAY_CONNS];
    size_t      count;
    /* Replies the server produced but no recorded chunk has claimed yet, in
     * order.  ⚠ Bounded: a capture whose replies outrun this many un-matched is
     * one where the comparison has already lost its footing, and `replies_extra`
     * says so rather than a bigger buffer hiding it. */
    gsp_write_request pending[GS_REPLAY_PENDING];
    size_t            pending_count;
} gs_replay_state;

static bool replay_known(gs_replay_state *st, gsp_conn_id conn)
{
    size_t i;
    for (i = 0; i < st->count; ++i) {
        if (st->ids[i] == conn) {
            return true;
        }
    }
    return false;
}

static void replay_remember(gs_replay_state *st, gsp_conn_id conn)
{
    if (st->count < GS_REPLAY_CONNS) {
        st->ids[st->count++] = conn;
    }
}

static void replay_drain(gsp_server *server, gs_replay_state *st, gsp_replay_report *rep)
{
    gsp_write_request w[8];
    gsp_event ev[16];
    size_t n;
    size_t i;

    while ((n = gsp_server_poll_writes(server, w, 8u)) > 0u) {
        for (i = 0; i < n; ++i) {
            rep->replies_produced++;
            if (st->pending_count < GS_REPLAY_PENDING) {
                st->pending[st->pending_count++] = w[i];
            } else {
                rep->replies_extra++;
            }
        }
    }
    while ((n = gsp_server_poll_events(server, ev, 16u)) > 0u) {
        for (i = 0; i < n; ++i) {
            switch ((gsp_event_type)ev[i].type) {
            case GSP_EV_SHOT:
                rep->shots++;
                rep->messages++;
                rep->flags_seen |= ev[i].u.message.flags;
                break;
            case GSP_EV_HEARTBEAT:
            case GSP_EV_STATUS:
                rep->messages++;
                rep->flags_seen |= ev[i].u.message.flags;
                break;
            case GSP_EV_PROTOCOL_ERROR:
                rep->protocol_errors++;
                break;
            case GSP_EV_NONE:
            case GSP_EV_CONNECTION_OPENED:
            case GSP_EV_CONNECTION_CLOSED:
            case GSP_EV_CLIENT_IDENTIFIED:
            case GSP_EV_CLIENT_STATE:
            case GSP_EV_PLAYER_INFO_SENT:
            case GSP_EV_SESSION_STATE_SENT:
            case GSP_EV_CLOSE_REQUESTED:
            case GSP_EV_CLIENT_IDLE:
            case GSP_EV_CLIENT_ACTIVE:
            case GSP_EV_WARNING:
            case GSP_EVENT_TYPE_COUNT:
            default:
                break;
            }
        }
    }
}

/* GSP_WRITE_ACK → 200, and so on: the five shapes bytes can leave by (design §9.1). */
static int32_t kind_code(uint8_t kind)
{
    switch ((gsp_write_kind)kind) {
    case GSP_WRITE_ACK:          return (int32_t)GSP_CODE_SHOT_RECEIVED;
    case GSP_WRITE_PLAYER_INFO:  return (int32_t)GSP_CODE_PLAYER_INFO;
    case GSP_WRITE_READY:        return (int32_t)GSP_CODE_READY;
    case GSP_WRITE_ROUND_ENDED:  return (int32_t)GSP_CODE_ROUND_ENDED;
    case GSP_WRITE_FAILURE:      return (int32_t)GSP_CODE_FAILURE;
    default:                     return 0;
    }
}

static void replay_consume(gs_replay_state *st, size_t i)
{
    memmove(&st->pending[i], &st->pending[i + 1u],
            (st->pending_count - i - 1u) * sizeof(st->pending[0]));
    st->pending_count--;
}

/*
 * One recorded reply, against what the server has produced and nothing has
 * claimed yet.
 *
 * ⚠ MATCHED BY WHAT IT IS, NOT BY ITS TURN IN THE QUEUE.  A recording holds the
 * host's 201s and 202s interleaved with the acknowledgements, and their ORDER
 * depends on when the host happened to poll (design §5.5) — so a comparison
 * that paired them off positionally would report a 201 as "a 200 that changed"
 * and then a 200 as "missing", which is two false findings from one difference
 * in timing.  So: an identical reply first, then one of the same CODE (that is
 * the text or the numbers changing, which is the finding), then neither.
 */
static void replay_match_reply(gs_replay_state *st, const gsp_wire_chunk *c,
                               gsp_replay_report *rep)
{
    gsp_response recorded;
    bool decoded = gsp_response_decode(c->data, (size_t)c->length, &recorded) == GSP_OK;
    size_t i;

    for (i = 0; i < st->pending_count; ++i) {
        if (st->pending[i].conn == c->conn && st->pending[i].length == c->length
            && memcmp(st->pending[i].data, c->data, (size_t)c->length) == 0) {
            rep->replies_matched++;
            replay_consume(st, i);
            return;
        }
    }
    if (decoded) {
        for (i = 0; i < st->pending_count; ++i) {
            if (st->pending[i].conn == c->conn
                && kind_code(st->pending[i].kind) == recorded.code) {
                /* ⚠ Same answer, different bytes.  After a deliberate change —
                 * protocol §11's U4, the acknowledgement text — this is the
                 * number the run was made to produce. */
                rep->replies_differing++;
                replay_consume(st, i);
                return;
            }
        }
    }
    /*
     * Nothing the server produced answers to this.  ⚠ SORT THE TWO REASONS
     * APART, because they mean opposite things.  A 201, 202 or 203 is
     * UNSOLICITED: the host originates it from its own game state — which club
     * the player has, whether a round is running (design §5.6) — and a capture
     * holds the bytes on the wire, not the state behind them.  Replaying into a
     * server that was never told the club cannot produce it, and that is not a
     * finding.
     *
     * An ACKNOWLEDGEMENT that has stopped being produced is a finding, and after
     * a deliberate change it is the number to read.
     */
    if (decoded
        && (recorded.code == (int32_t)GSP_CODE_PLAYER_INFO
            || recorded.code == (int32_t)GSP_CODE_READY
            || recorded.code == (int32_t)GSP_CODE_ROUND_ENDED)) {
        rep->replies_unsolicited++;
        return;
    }
    rep->replies_missing++;
}

gsp_status gsp_replay_into_server(gsp_replay *r, gsp_server *server, gsp_replay_report *out)
{
    gs_replay_state st;
    gsp_replay_report rep;
    gsp_wire_chunk chunks[8];
    size_t n;

    if (r == NULL || server == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    memset(&st, 0, sizeof(st));
    memset(&rep, 0, sizeof(rep));

    while ((n = gsp_replay_read(r, chunks, 8u)) > 0u) {
        size_t i;
        for (i = 0; i < n; ++i) {
            const gsp_wire_chunk *c = &chunks[i];
            rep.chunks++;
            if ((c->flags & (uint8_t)GSP_WIRE_LOST) != 0u) {
                rep.lost_chunks++;
            }
            switch ((gsp_wire_direction)c->direction) {
            case GSP_WIRE_CLIENT_TO_SERVER:
                rep.client_chunks++;
                /* ⚠ OPENED LAZILY rather than from the META line.  A capture
                 * may begin mid-session — the wire ring is drop-oldest, so its
                 * first chunks are exactly what a busy session loses — and a
                 * replay that refused to feed bytes for a connection it never
                 * saw open would discard the part of the capture that survived. */
                if (!replay_known(&st, c->conn)) {
                    replay_remember(&st, c->conn);
                    rep.connections++;
                    (void)gsp_server_on_connection_opened(server, c->conn, NULL,
                                                          c->host_time_us);
                    replay_drain(server, &st, &rep);
                    /* The announce-on-connect writes belong to THIS server's
                     * policy, not to the recording; they are produced, and the
                     * recording will not claim them. */
                }
                {
                    gsp_status fed = gsp_server_on_bytes(server, c->conn, c->data,
                                                         (size_t)c->length, c->host_time_us);
                    while (fed == GSP_ERR_QUEUE_FULL) {
                        replay_drain(server, &st, &rep);
                        fed = gsp_server_on_bytes(server, c->conn, NULL, 0u, c->host_time_us);
                    }
                }
                replay_drain(server, &st, &rep);
                break;
            case GSP_WIRE_SERVER_TO_CLIENT:
                rep.server_chunks++;
                replay_drain(server, &st, &rep);
                replay_match_reply(&st, c, &rep);
                break;
            case GSP_WIRE_META:
                rep.meta_chunks++;
                /* ⚠ The META line is the FORMATTED EVENT (design §7), so a
                 * close is recognised by the one word that cannot appear in a
                 * JSON payload chunk.  A capture with no close line simply
                 * leaves the connection open, which is what it was. */
                if (c->length >= 17u
                    && memchr(c->data, 'C', 1u) != NULL
                    && memcmp(c->data, "CONNECTION_CLOSED", 17u) == 0
                    && replay_known(&st, c->conn)) {
                    (void)gsp_server_on_connection_closed(server, c->conn,
                                                          GSP_CLOSE_REMOTE_CLOSED,
                                                          c->host_time_us);
                    replay_drain(server, &st, &rep);
                }
                break;
            default:
                break;
            }
        }
    }
    replay_drain(server, &st, &rep);
    /* Whatever the server produced that the recording never claimed. */
    rep.replies_extra += (uint64_t)st.pending_count;

    if (out != NULL) {
        *out = rep;
    }
    return (gsp_replay_status(r) < GSP_OK) ? gsp_replay_status(r) : GSP_OK;
}
