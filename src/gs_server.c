/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_server.c — the sans-I/O GSPro Connect listener that never listens.
 *
 * design §5.  It owns no socket, no thread, no timer and no clock: the host
 * calls in with bytes and a `now`, and drains three rings on the way out.
 * Everything below is a function of those calls, which is what makes a whole
 * session reproducible under a synthetic counter (CT-X06) and what lets
 * PinPoint embed it in a QObject with a lifetime.
 *
 * Four properties are load-bearing, and each has a conformance case standing
 * over it:
 *
 *   1. EXACTLY ONE REPLY PER MESSAGE, QUEUED BEFORE on_bytes RETURNS.  [MLM]
 *      re-sends a shot after two seconds of silence and [OCR] gives up after
 *      ten, so a missing reply is a duplicated shot rather than an error, and
 *      [TNB] pairs replies to sends through a queue, so an extra one
 *      desynchronises the rest of the session (CT-R01, CT-R02).
 *   2. THE WRITE RING IS NOT DROP-OLDEST.  A dropped event costs a log line; a
 *      dropped reply costs a duplicated shot.  So when it is full on_bytes
 *      refuses the message rather than dropping its acknowledgement, keeps the
 *      bytes, and the host drains and resumes (CT-R07, CT-X05).
 *   3. THE REPLY IS QUEUED BEFORE THE EVENT, so a full EVENT ring drops the
 *      event and never the acknowledgement (design §5.4).
 *   4. THE LIBRARY NEVER CLOSES A CONNECTION.  It can ask (CT-X03); it cannot
 *      act, and it never drops a client for silence (protocol §9.6, CT-C10).
 */

#include "gspro/gspro.h"

#include "gs_frame.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Per-connection state                                                      */
/* ------------------------------------------------------------------------ */

typedef struct gs_conn {
    uint8_t             used;
    uint8_t             idle_reported;   /* CLIENT_IDLE fired; CLIENT_ACTIVE owed */
    uint8_t             reserved[2];
    gsp_connection_info info;

    /* The framer (gs_frame.h) and its pending bytes.  ⚠ `rx` is the whole
     * PENDING INPUT, not just the object being assembled: it has to survive a
     * GSP_ERR_QUEUE_FULL so the host can drain and resume without re-supplying
     * bytes it already handed over (server.h). */
    gs_framer framer;
    uint8_t  *rx;
    size_t    rx_len;   /* bytes pending                                     */
    size_t    scan;     /* how many of them the framer has already seen      */

    /* policy.write_spacing_us: the earliest time a write to this connection
     * may be polled (design §5.5).  Equal to `now` when spacing is off. */
    gsp_time_us next_write_us;
} gs_conn;

struct gsp_server {
    gsp_server_policy policy;
    gsp_allocator     allocator;
    uint8_t           has_allocator;
    uint8_t           closed;
    uint8_t           reserved[2];

    uint32_t max_connections;
    uint32_t event_ring;
    uint32_t write_ring;
    uint32_t wire_ring;
    uint32_t max_message_bytes;
    size_t   rx_capacity;      /* max_message_bytes + 1: see note in create() */

    /* ⚠ The library reads no clock (design §4.1).  This is the last `now` a
     * host supplied, and it is what poll_writes — which takes no now_us —
     * compares write spacing against. */
    gsp_time_us now_us;

    uint32_t event_sequence;
    uint32_t message_sequence;
    uint32_t wire_sequence;    /* per CHUNK, so a reader sees a gap where one was */
    uint32_t dropped_events;
    uint32_t dropped_wire;
    uint8_t  drop_warned;      /* one WARN_EVENTS_DROPPED per overflow run */
    uint8_t  wire_drop_warned; /* the same, for the wire ring                  */
    uint8_t  wire_lost_pending;/* the ring emptied while a drop was owed       */
    uint8_t  reserved2[1];

    gsp_player_info player;
    uint8_t         player_set;
    uint8_t         session_state;   /* gsp_session_state */
    uint8_t         reserved3[6];

    gs_conn           *conns;
    gsp_event         *events;
    gsp_write_request *writes;
    gsp_wire_chunk    *wire;

    uint32_t ev_head, ev_count;
    uint32_t w_head, w_count;
    uint32_t wire_head, wire_count;
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

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

static gs_conn *find_conn(gsp_server *s, gsp_conn_id conn)
{
    uint32_t i;
    for (i = 0; i < s->max_connections; ++i) {
        if (s->conns[i].used && s->conns[i].info.conn == conn) {
            return &s->conns[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* The event ring — drop-OLDEST, and counted                                 */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THE ASYMMETRY WITH THE WRITE RING IS THE POINT (design §3.4).  An event is
 * a log line and a consumer that fell behind can afford to lose the oldest
 * ones; a reply is an acknowledgement a launch monitor is blocking on.  So
 * this ring drops and counts, and the write ring refuses.
 */
static void ring_push_event(gsp_server *s, const gsp_event *ev)
{
    if (s->event_ring == 0u) {
        return;
    }
    if (s->ev_count == s->event_ring) {
        s->ev_head = (s->ev_head + 1u) % s->event_ring;
        s->ev_count--;
        s->dropped_events++;
        s->drop_warned = s->drop_warned ? 1u : 2u; /* 2 = owed, not yet said */
    }
    s->events[(s->ev_head + s->ev_count) % s->event_ring] = *ev;
    s->ev_count++;
}

static void emit(gsp_server *s, gsp_event *ev)
{
    ev->sequence = s->event_sequence++;
    ring_push_event(s, ev);

    /* ⚠ Say it once per overflow run, not once per dropped event: a warning
     * per drop would itself fill the ring it is complaining about.  The count
     * is always exact in gsp_server_dropped_events(). */
    if (s->drop_warned == 2u) {
        gsp_event w;
        s->drop_warned = 1u;
        memset(&w, 0, sizeof(w));
        w.type = (uint8_t)GSP_EV_WARNING;
        w.host_time_us = s->now_us;
        w.conn = GSP_CONN_NONE;
        w.u.warning.code = (uint16_t)GSP_WARN_EVENTS_DROPPED;
        w.u.warning.value = s->dropped_events;
        copy_bounded(w.u.warning.text, sizeof(w.u.warning.text),
                     "event ring overflowed; oldest events were dropped");
        w.sequence = s->event_sequence++;
        ring_push_event(s, &w);
    }
}

static void emit_simple(gsp_server *s, gsp_event_type type, gsp_conn_id conn, gsp_time_us now)
{
    gsp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)type;
    ev.conn = conn;
    ev.host_time_us = now;
    emit(s, &ev);
}

static void emit_warning(gsp_server *s, gsp_conn_id conn, gsp_warning_code code,
                         uint32_t value, const char *text, gsp_time_us now)
{
    gsp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)GSP_EV_WARNING;
    ev.conn = conn;
    ev.host_time_us = now;
    ev.u.warning.code = (uint16_t)code;
    ev.u.warning.value = value;
    copy_bounded(ev.u.warning.text, sizeof(ev.u.warning.text), text);
    emit(s, &ev);
}

/*
 * ⚠ THE WIRE LOG SITS HERE, ABOVE ITS CALLERS, AND NOT WHERE IT READS BEST.
 * emit_connection() and protocol_error() both feed it, so with the section
 * further down each needed a forward declaration — and gcc's -Wredundant-decls,
 * which this project builds with, rejects a declaration that the definition
 * repeats.  ⚠ clang ACCEPTS -Wredundant-decls AND IMPLEMENTS NOTHING, so a
 * macOS build is silent about it and CI is where it surfaces.  Ordering the
 * file so nothing needs declaring twice is the fix that holds for both.
 */
/* ------------------------------------------------------------------------ */
/* The wire log — design §7                                                  */
/* ------------------------------------------------------------------------ */
/*
 * RECORD THE BYTES, NOT THE DECODED MESSAGES.  protocol §11 lists ten open
 * questions, and when U2, U4 or U7 is answered a byte-level capture of the
 * session that answered it RE-DECODES with the fix applied; a decoded log has
 * already thrown away what the fix would have read differently.  The first
 * capture against a real launch monitor is the fixture that pins the decoder,
 * which is why this is a precondition for design §11's package 7 rather than a
 * convenience.
 *
 * ⚠ A CHUNK IS ONE MESSAGE, ONE DISCARDED RUN, OR ONE REPLY — NOT ONE read().
 * Read boundaries are the kernel's rather than the client's (design §3.2.1), so
 * recording them would preserve an artefact; and redaction has to know where the
 * DeviceID value SITS, which is knowable only once an object has been framed.
 *
 * ⚠ OFF UNLESS config.wire_ring SAYS OTHERWISE.  s->wire is NULL then and every
 * function here returns before it copies a byte: a library that recorded a
 * household's traffic because somebody turned on verbose logging would be a
 * different kind of library (design §9.2).
 */

static bool wire_on(const gsp_server *s)
{
    return s->wire != NULL;
}

/* ⚠ The value is overwritten with THIS, repeated and cut to the value's own
 * length, so the JSON still parses and every offset after it still holds — a
 * capture whose lengths moved could not be compared with the event log or with
 * a fixture promoted from it.  It contains no quote and no backslash, so it
 * cannot end the string it replaces (design §9.2). */
static const char GS_WIRE_MASK[] = "<redacted>";
#define GS_WIRE_MASK_LEN 10u

static bool wire_is_space(uint8_t b)
{
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

static bool wire_ci_equal(const uint8_t *p, const char *lit, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        uint8_t a = p[i];
        uint8_t b = (uint8_t)lit[i];
        if (a >= 'A' && a <= 'Z') {
            a = (uint8_t)(a + 32);
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/*
 * Finds every DeviceID VALUE in a buffer, in order, without decoding it.
 *
 * ⚠ IT WORKS ON RAW BYTES BECAUSE IT MUST ALSO WORK ON BYTES THAT DID NOT
 * PARSE.  A capture that omitted what the decoder rejected would omit the whole
 * subject of the session it was taken to explain (conformance CT-W06) — so the
 * discarded runs are recorded too, and a best-effort scan is what can redact
 * them.  The key match is case-insensitive because the decoder's is (§9.1):
 * [MLM] and [R10] each misspell a key's case and a redactor that were stricter
 * than the parser would leak exactly the identifiers those clients send.
 */
typedef struct gs_redactor {
    const uint8_t *buf;
    size_t         len;
    size_t         next;       /* where the next search starts               */
    size_t         start, end; /* the current value's span                   */
    bool           have;
} gs_redactor;

static void redactor_init(gs_redactor *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->next = 0u;
    r->start = 0u;
    r->end = 0u;
    r->have = false;
}

static bool redactor_next(gs_redactor *r)
{
    size_t i = r->next;

    /* `"DeviceID"` is ten bytes; anything shorter cannot hold the key. */
    while (r->len >= 10u && i + 10u <= r->len) {
        size_t j;
        size_t v;
        if (r->buf[i] != '"' || !wire_ci_equal(r->buf + i + 1u, "deviceid", 8u)
            || r->buf[i + 9u] != '"') {
            i++;
            continue;
        }
        j = i + 10u;
        while (j < r->len && wire_is_space(r->buf[j])) {
            j++;
        }
        if (j >= r->len || r->buf[j] != ':') {
            i++;
            continue;
        }
        j++;
        while (j < r->len && wire_is_space(r->buf[j])) {
            j++;
        }
        if (j >= r->len || r->buf[j] != '"') {
            /* null, a number, anything that is not a string: nothing that could
             * carry a serial, and not ours to rewrite. */
            i++;
            continue;
        }
        j++;
        v = j;
        while (v < r->len && r->buf[v] != '"') {
            if (r->buf[v] == '\\' && v + 1u < r->len) {
                v++;                      /* \" does not end the string        */
            }
            v++;
        }
        /* ⚠ A value with no closing quote — a truncated or hostile object —
         * redacts to the end of the buffer.  OVER-redaction is the safe
         * direction; under-redaction is a leak. */
        r->start = j;
        r->end = v;
        r->next = (v < r->len) ? (v + 1u) : r->len;
        r->have = true;
        return true;
    }
    r->have = false;
    r->next = r->len;
    return false;
}

/*
 * Masks whatever falls inside `dst`, which holds `buf[off .. off+n)`.  Returns
 * true if anything was removed, which is what sets GSP_WIRE_REDACTED on that
 * chunk — the flag says "something was taken OUT of this chunk", not "the
 * session was redacted".
 *
 * ⚠ A value that straddles a chunk boundary is kept for the next chunk rather
 * than consumed, and the mask is indexed from the value's own start, so the two
 * halves of a long id still read as one repeated "<redacted>".
 */
static bool redactor_apply(gs_redactor *r, uint8_t *dst, size_t off, size_t n)
{
    const size_t end = off + n;
    bool any = false;

    for (;;) {
        size_t a;
        size_t b;
        size_t k;

        if (!r->have && !redactor_next(r)) {
            return any;
        }
        if (r->end <= off) {
            r->have = false;      /* entirely behind this chunk */
            continue;
        }
        if (r->start >= end) {
            return any;           /* not reached yet; keep it for a later chunk */
        }
        a = (r->start > off) ? r->start : off;
        b = (r->end < end) ? r->end : end;
        for (k = a; k < b; ++k) {
            dst[k - off] = (uint8_t)GS_WIRE_MASK[(k - r->start) % GS_WIRE_MASK_LEN];
        }
        any = any || (b > a);
        if (r->end > end) {
            return any;           /* straddles: the next chunk finishes it */
        }
        r->have = false;
    }
}

/*
 * ⚠ DROP-OLDEST, AND THE SURVIVOR SAYS SO.  The wire ring is a log, not an
 * acknowledgement path — unlike the write ring, which refuses (design §3.4).
 * GSP_WIRE_LOST goes on the OLDEST SURVIVING chunk rather than the newcomer,
 * because that is where the reader meets the gap; `sequence` says how big it
 * was, which is why chunks carry one at all.
 */
static void wire_ring_push(gsp_server *s, const gsp_wire_chunk *chunk)
{
    gsp_wire_chunk *slot;

    if (s->wire_count == s->wire_ring) {
        s->wire_head = (s->wire_head + 1u) % s->wire_ring;
        s->wire_count--;
        s->dropped_wire++;
        if (s->wire_count > 0u) {
            s->wire[s->wire_head].flags |= (uint8_t)GSP_WIRE_LOST;
        } else {
            /* A ring of one: there is no survivor to mark, so the flag is owed
             * to whatever arrives next. */
            s->wire_lost_pending = 1u;
        }
        s->wire_drop_warned = s->wire_drop_warned ? 1u : 2u;
    }
    slot = &s->wire[(s->wire_head + s->wire_count) % s->wire_ring];
    *slot = *chunk;
    if (s->wire_lost_pending != 0u) {
        slot->flags |= (uint8_t)GSP_WIRE_LOST;
        s->wire_lost_pending = 0u;
    }
    s->wire_count++;

    /* ⚠ Once per overflow run, exactly as the event ring does it: a warning per
     * dropped chunk would fill the event ring with complaints about the wire
     * ring.  gsp_server_dropped_wire() is always exact. */
    if (s->wire_drop_warned == 2u) {
        s->wire_drop_warned = 1u;
        emit_warning(s, GSP_CONN_NONE, GSP_WARN_WIRE_DROPPED, s->dropped_wire,
                     "wire ring overflowed; oldest chunks were dropped", s->now_us);
    }
}

static void wire_push_bytes(gsp_server *s, gsp_conn_id conn, gsp_wire_direction direction,
                            const uint8_t *data, size_t len, gsp_time_us now, bool redact,
                            uint8_t extra_flags)
{
    gs_redactor r;
    size_t off = 0u;

    if (!wire_on(s)) {
        return;
    }
    redactor_init(&r, data, len);
    do {
        gsp_wire_chunk chunk;
        size_t n = len - off;

        if (n > (size_t)GSP_WIRE_CHUNK_MAX) {
            n = (size_t)GSP_WIRE_CHUNK_MAX;
        }
        memset(&chunk, 0, sizeof(chunk));
        chunk.host_time_us = now;
        chunk.conn = conn;
        chunk.sequence = s->wire_sequence++;
        chunk.direction = (uint8_t)direction;
        chunk.flags = extra_flags;
        chunk.length = (uint16_t)n;
        if (n > 0u) {
            memcpy(chunk.data, data + off, n);
            if (redact && redactor_apply(&r, chunk.data, off, n)) {
                chunk.flags |= (uint8_t)GSP_WIRE_REDACTED;
            }
        }
        off += n;
        if (off < len) {
            chunk.flags |= (uint8_t)GSP_WIRE_CONTINUES;
        }
        wire_ring_push(s, &chunk);
    } while (off < len);
}

/* Bytes a client sent: one framed object, or one run the framer threw away. */
static void wire_client(gsp_server *s, gsp_conn_id conn, const uint8_t *data, size_t len,
                        gsp_time_us now)
{
    if (!wire_on(s) || len == 0u) {
        return;
    }
    wire_push_bytes(s, conn, GSP_WIRE_CLIENT_TO_SERVER, data, len, now,
                    !s->policy.record_identifiers, 0u);
}

/*
 * An open or a close, as one line of text.
 *
 * ⚠ IT IS gsp_event_format()'s OWN OUTPUT, and that is the point: the peer
 * address is personal data (design §9.2) and the formatter is the code that
 * already knows how to withhold it, is already tested for withholding it
 * (test_api.c's redaction sweep), and cannot drift from what the event log says
 * because it IS what the event log says.
 */
static void wire_meta(gsp_server *s, const gsp_event *ev)
{
    char line[256];
    size_t n;
    bool withheld;

    if (!wire_on(s)) {
        return;
    }
    /* ⚠ THE CONNECTION'S LIFE, NOT EVERY EVENT THAT CARRIES ONE.
     * emit_connection() also builds GSP_EV_CLIENT_IDENTIFIED, and that one is a
     * CONCLUSION rather than a fact on the wire: the DeviceID it reports is
     * already in the recorded message bytes, three lines above it in the same
     * capture.  A log that repeated the library's readings back to itself would
     * be the decoded log design §7 exists to avoid. */
    if (ev->type != (uint8_t)GSP_EV_CONNECTION_OPENED
        && ev->type != (uint8_t)GSP_EV_CONNECTION_CLOSED) {
        return;
    }
    n = gsp_event_format(ev, line, sizeof(line), s->policy.record_identifiers);
    if (n >= sizeof(line)) {
        n = sizeof(line) - 1u;
    }
    withheld = !s->policy.record_identifiers && gsp_event_is_sensitive(ev);
    wire_push_bytes(s, ev->conn, GSP_WIRE_META, (const uint8_t *)line, n, ev->host_time_us,
                    false, withheld ? (uint8_t)GSP_WIRE_REDACTED : 0u);
}

static void emit_connection(gsp_server *s, gsp_event_type type, const gs_conn *c,
                            gsp_close_cause cause, gsp_time_us now)
{
    gsp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)type;
    ev.conn = c->info.conn;
    ev.host_time_us = now;
    ev.u.connection.info = c->info;
    ev.u.connection.cause = (uint8_t)cause;
    emit(s, &ev);
    /* ⚠ ONE HOOK COVERS ALL THREE: an open, a close the host reported, and the
     * close gsp_server_close() reports for every connection still up.  A capture
     * whose connections just stop, with no line saying why, cannot be read back
     * as a session (design §7). */
    wire_meta(s, &ev);
}

/* ------------------------------------------------------------------------ */
/* The write ring — REFUSES rather than drops                                */
/* ------------------------------------------------------------------------ */

static bool write_ring_full(const gsp_server *s)
{
    return s->w_count >= s->write_ring;
}

static bool queue_write(gsp_server *s, gsp_conn_id conn, gsp_write_kind kind, int code,
                        const char *message, const gsp_player_info *player)
{
    gsp_write_request *w;
    size_t written = 0u;

    if (write_ring_full(s)) {
        return false;
    }
    w = &s->writes[(s->w_head + s->w_count) % s->write_ring];
    /* ⚠ The ONLY path by which bytes leave this library, and it validates the
     * code against the five (design §9.1).  There is no send_raw(). */
    if (gsp_response_encode(code, message, player, (char *)w->data, sizeof(w->data),
                            &written) < GSP_OK) {
        return false;
    }
    w->conn = conn;
    w->kind = (uint8_t)kind;
    w->reserved = 0u;
    w->length = (uint16_t)written;
    s->w_count++;
    return true;
}

/* A connection that has gone away must not leave writes addressed to it in the
 * ring: the host has nothing left to write them to (server.h). */
static void drop_writes_for(gsp_server *s, gsp_conn_id conn)
{
    uint32_t i;
    uint32_t keep = 0u;

    for (i = 0; i < s->w_count; ++i) {
        gsp_write_request *src = &s->writes[(s->w_head + i) % s->write_ring];
        if (src->conn == conn) {
            continue;
        }
        if (keep != i) {
            s->writes[(s->w_head + keep) % s->write_ring] = *src;
        }
        keep++;
    }
    s->w_count = keep;
}

/* ------------------------------------------------------------------------ */
/* Player information and session state                                      */
/* ------------------------------------------------------------------------ */

/*
 * ⚠ A player with nothing known says nothing.  An UNKNOWN hand or club is
 * omitted from the JSON rather than sent as a string GSPro never sends
 * (design §5.6), so a 201 with all of them unknown would be an empty Player
 * object — noise a client has to parse and learn nothing from.
 */

static bool player_has_anything(const gsp_player_info *p)
{
    return p->handed != (uint8_t)GSP_HANDED_UNKNOWN ||
           p->club != (uint8_t)GSP_CLUB_UNKNOWN || p->has_distance != 0u ||
           p->surface[0] != '\0';
}

static void send_player_to(gsp_server *s, gs_conn *c, const gsp_player_info *p,
                           gsp_player_info_reason reason, gsp_time_us now)
{
    gsp_event ev;

    if (!queue_write(s, c->info.conn, GSP_WRITE_PLAYER_INFO, (int)GSP_CODE_PLAYER_INFO,
                     GSP_TEXT_PLAYER_INFO, p)) {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)GSP_EV_PLAYER_INFO_SENT;
    ev.conn = c->info.conn;
    ev.host_time_us = now;
    ev.u.player_info.player = *p;
    ev.u.player_info.reason = (uint8_t)reason;
    emit(s, &ev);
}

static void send_session_to(gsp_server *s, gs_conn *c, gsp_session_state state,
                            gsp_player_info_reason reason, gsp_time_us now)
{
    gsp_event ev;
    gsp_write_kind kind;
    int code;
    const char *text;

    if (state == GSP_SESSION_ACTIVE) {
        kind = GSP_WRITE_READY;
        code = (int)GSP_CODE_READY;
        text = GSP_TEXT_READY;
    } else if (state == GSP_SESSION_ENDED) {
        kind = GSP_WRITE_ROUND_ENDED;
        code = (int)GSP_CODE_ROUND_ENDED;
        text = GSP_TEXT_ROUND_ENDED;
    } else {
        return;
    }
    /* ⚠ The strings are FIXED, not policy: [OSP] compares them exactly before
     * it arms or clears its match state (protocol §5.1). */
    if (!queue_write(s, c->info.conn, kind, code, text, NULL)) {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)GSP_EV_SESSION_STATE_SENT;
    ev.conn = c->info.conn;
    ev.host_time_us = now;
    ev.u.session_state.state = (uint8_t)state;
    ev.u.session_state.reason = (uint8_t)reason;
    emit(s, &ev);
}

/* ------------------------------------------------------------------------ */
/* Protocol errors                                                           */
/* ------------------------------------------------------------------------ */

static void protocol_error(gsp_server *s, gs_conn *c, gsp_protocol_error_reason reason,
                           const uint8_t *snippet, size_t snippet_len, size_t discarded,
                           gsp_time_us now)
{
    gsp_event ev;
    const char *text;
    size_t n;

    /* ⚠ WHAT FAILED IS RECORDED TOO (conformance CT-W06).  A capture taken to
     * explain a client this library rejected, which left out the bytes it
     * rejected, would leave out the entire subject.  Redaction still applies:
     * gs_redactor scans raw bytes precisely so a malformed object cannot leak
     * what a well-formed one would not. */
    wire_client(s, c->info.conn, snippet, snippet_len, now);

    c->info.protocol_errors++;
    c->info.consecutive_errors++;

    /* ⚠ 501, not [OSG]'s 413 for oversize: [GSP] defines only the 5XX space
     * and [TNB] treats anything >= 500 as failure (design §5.5). */
    text = (reason == GSP_PE_TOO_LARGE) ? GSP_TEXT_TOO_LARGE : GSP_TEXT_BAD_JSON;
    (void)queue_write(s, c->info.conn, GSP_WRITE_FAILURE, (int)GSP_CODE_FAILURE, text, NULL);

    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)GSP_EV_PROTOCOL_ERROR;
    ev.conn = c->info.conn;
    ev.host_time_us = now;
    ev.u.protocol_error.reason = (uint8_t)reason;
    ev.u.protocol_error.discarded_bytes =
        (uint32_t)((discarded > 0xffffffffu) ? 0xffffffffu : discarded);
    ev.u.protocol_error.consecutive = c->info.consecutive_errors;
    n = (snippet_len > (size_t)GSP_ERROR_SNIPPET_MAX) ? (size_t)GSP_ERROR_SNIPPET_MAX
                                                      : snippet_len;
    if (n > 0u && snippet != NULL) {
        memcpy(ev.u.protocol_error.snippet, snippet, n);
    }
    ev.u.protocol_error.snippet_length = (uint8_t)n;
    emit(s, &ev);

    /*
     * ⚠ ASKED ONCE, AT THE THRESHOLD, and only asked: the library cannot close
     * anything (design §5.3).  A well-formed message resets the run, so a
     * client having a bad minute is not treated like one that has gone mad.
     */
    if (s->policy.protocol_error_close_threshold > 0u &&
        c->info.consecutive_errors == s->policy.protocol_error_close_threshold) {
        gsp_event req;
        memset(&req, 0, sizeof(req));
        req.type = (uint8_t)GSP_EV_CLOSE_REQUESTED;
        req.conn = c->info.conn;
        req.host_time_us = now;
        req.u.protocol_error = ev.u.protocol_error;
        emit(s, &req);
    }
}

/* ------------------------------------------------------------------------ */
/* One decoded message                                                       */
/* ------------------------------------------------------------------------ */

static void note_identity(gsp_server *s, gs_conn *c, const gsp_message *m, gsp_time_us now)
{
    if (m->device_id[0] == '\0') {
        return;
    }
    if (!c->info.identified) {
        c->info.identified = 1u;
        copy_bounded(c->info.device_id, sizeof(c->info.device_id), m->device_id);
        emit_connection(s, GSP_EV_CLIENT_IDENTIFIED, c, GSP_CLOSE_UNKNOWN, now);
        return;
    }
    if (strcmp(c->info.device_id, m->device_id) != 0) {
        /* ⚠ Not something a well-behaved client does.  The first name stands
         * — re-identifying would rewrite the history of the connection — and
         * the log says what happened (design §5.2). */
        emit_warning(s, c->info.conn, GSP_WARN_DEVICE_ID_CHANGED, 0,
                     "a later message named a different DeviceID", now);
    }
}

static void note_client_state(gsp_server *s, gs_conn *c, const gsp_message *m,
                              gsp_time_us now)
{
    uint8_t ready = c->info.ready;
    uint8_t detected = c->info.ball_detected;

    /* ⚠ Read off ANY message, heartbeat included: [OSP]'s connect-time message
     * carries IsHeartBeat true AND the ready flag, and [R10] does the same, so
     * a server that only read them off a "status" message would never see
     * either (protocol §3.4, CT-K05). */
    if ((m->options.present & (uint8_t)GSP_OPT_LAUNCH_MONITOR_IS_READY) != 0u) {
        ready = m->options.launch_monitor_is_ready ? 1u : 0u;
    }
    if ((m->options.present & (uint8_t)GSP_OPT_LAUNCH_MONITOR_BALL_DETECTED) != 0u) {
        detected = m->options.launch_monitor_ball_detected ? 1u : 0u;
    }
    if (ready == c->info.ready && detected == c->info.ball_detected) {
        /* ⚠ Only on CHANGE.  [FB] repeats both flags every five seconds and a
         * UI must not repaint for that. */
        return;
    }
    {
        gsp_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = (uint8_t)GSP_EV_CLIENT_STATE;
        ev.conn = c->info.conn;
        ev.host_time_us = now;
        ev.u.client_state.ready = ready;
        ev.u.client_state.ball_detected = detected;
        ev.u.client_state.previous_ready = c->info.ready;
        ev.u.client_state.previous_ball_detected = c->info.ball_detected;
        c->info.ready = ready;
        c->info.ball_detected = detected;
        emit(s, &ev);
    }
}

static gsp_event_type event_for_kind(uint8_t kind)
{
    if (kind == (uint8_t)GSP_MSG_SHOT) {
        return GSP_EV_SHOT;
    }
    if (kind == (uint8_t)GSP_MSG_HEARTBEAT) {
        return GSP_EV_HEARTBEAT;
    }
    return GSP_EV_STATUS;
}

/* The object occupies `json[0..len)` and a write slot is already known free.
 * False when the bytes framed as an object but are not one. */
static bool deliver_message(gsp_server *s, gs_conn *c, const uint8_t *json, size_t len,
                            gsp_time_us now)
{
    gsp_message m;
    gsp_event ev;
    const char *ack;

    if (gsp_message_decode(json, len, &m) < GSP_OK) {
        /* Braces balanced, contents not a JSON object — the vendor's own
         * commented example is exactly this (CT-X08). */
        protocol_error(s, c, GSP_PE_BAD_JSON, json, len, len, now);
        return false;
    }

    /* ⚠ THE BYTES AS THEY ARRIVED, before anything this library concluded about
     * them.  That is the whole value of the log: when protocol §11's U2 or U7 is
     * answered, this re-decodes with the fix applied (design §7). */
    wire_client(s, c->info.conn, json, len, now);

    m.conn = c->info.conn;
    m.sequence = s->message_sequence++;
    m.host_recv_us = now;

    c->info.messages++;
    c->info.last_message_us = now;
    c->info.consecutive_errors = 0u;

    if (m.kind == (uint8_t)GSP_MSG_SHOT) {
        /* ⚠ Per CONNECTION, and only against a previous SHOT's number: a
         * reconnecting client restarts its counter (protocol §4.3, CT-C06) and
         * [FB] increments the same field on every heartbeat (CT-C08). */
        if (c->info.last_shot_number != INT64_MIN && m.shot_number == c->info.last_shot_number) {
            m.flags |= (uint32_t)GSP_MSGF_SHOT_NUMBER_REPEATED;
        }
        c->info.last_shot_number = m.shot_number;
        c->info.shots++;
    }

    /*
     * ⚠ REPLY FIRST, EVENT SECOND (design §5.4), so that a full event ring
     * drops the event and never the acknowledgement.  And every kind is
     * answered 200: a heartbeat is "a valid shot message" to GSPro, which is
     * the one thing [TNB] asked the vendor to change and the vendor did not
     * (protocol §4.2).
     */
    if (s->policy.reject_incomplete_shots && (m.flags & (uint32_t)GSP_MSGF_BALL_INCOMPLETE) != 0u) {
        (void)queue_write(s, c->info.conn, GSP_WRITE_FAILURE, (int)GSP_CODE_FAILURE,
                          GSP_TEXT_INCOMPLETE, NULL);
    } else {
        ack = (s->policy.ack_text[0] != '\0') ? s->policy.ack_text : GSP_TEXT_SHOT_RECEIVED;
        (void)queue_write(s, c->info.conn, GSP_WRITE_ACK, (int)GSP_CODE_SHOT_RECEIVED, ack,
                          NULL);
    }

    if (c->idle_reported) {
        c->idle_reported = 0u;
        emit_simple(s, GSP_EV_CLIENT_ACTIVE, c->info.conn, now);
    }
    note_identity(s, c, &m, now);
    note_client_state(s, c, &m, now);

    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)event_for_kind(m.kind);
    ev.conn = c->info.conn;
    ev.host_time_us = now;
    ev.u.message = m;
    emit(s, &ev);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Framing the pending buffer                                                */
/* ------------------------------------------------------------------------ */

/* Drop the bytes the framer has already dealt with, so an object always starts
 * at offset 0 and its length is simply `scan`. */
static void compact(gs_conn *c)
{
    if (c->scan == 0u) {
        return;
    }
    if (c->scan < c->rx_len) {
        memmove(c->rx, c->rx + c->scan, c->rx_len - c->scan);
    }
    c->rx_len -= c->scan;
    c->scan = 0u;
}

static void reset_rx(gs_conn *c)
{
    c->rx_len = 0u;
    c->scan = 0u;
    gs_framer_reset(&c->framer);
}

/*
 * Runs the framer over the pending buffer, delivering every complete object.
 *
 * GSP_OK          the buffer is drained to what is still incomplete
 * GSP_ERR_QUEUE_FULL  stopped before a message because the write ring is full;
 *                     everything still pending is retained
 *
 * *discarded is set when a protocol error threw the buffer away, which tells
 * the caller to drop the rest of this call's input too — see the note there.
 */
static gsp_status frame_pending(gsp_server *s, gs_conn *c, gsp_time_us now, bool *discarded)
{
    while (c->scan < c->rx_len) {
        gs_frame_step step;

        if (!gs_framer_in_message(&c->framer)) {
            uint8_t b = c->rx[c->scan];
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
                /* ⚠ Whitespace between objects is nothing, not a delimiter and
                 * not garbage: [MLM] allows spaces and [FB], [GG] and [PIT]
                 * append a newline (protocol §2). */
                c->scan++;
                continue;
            }
            /*
             * ⚠ THE WRITE SLOT IS CHECKED HERE, BEFORE A SINGLE BYTE OF THE
             * NEXT MESSAGE IS CONSUMED.  One message produces exactly one
             * reply and nothing else queues in between, so a slot free now is
             * still free when the object closes — which is what lets the
             * refusal keep the bytes instead of dropping an acknowledgement
             * (design §3.4, CT-R07).
             */
            if (write_ring_full(s)) {
                compact(c);
                return GSP_ERR_QUEUE_FULL;
            }
            compact(c);
        }

        step = gs_framer_feed(&c->framer, c->rx[c->scan]);

        if (step == GS_FRAME_GARBAGE) {
            /*
             * ⚠ THE WHOLE PENDING BUFFER GOES, not just the offending byte.
             * A stream that has desynchronised cannot be trusted to the next
             * '{' — that brace may be inside a truncated string — and a server
             * that spliced the two halves together would invent a message.
             * [FB] resets its buffer on any decode error for the same reason,
             * and CT-R03 pins it: `[{"a":1}]` earns ONE 501, not a 501, a 200
             * and another 501.  Scanning resumes at the next '{' in whatever
             * arrives afterwards.
             */
            size_t n = c->rx_len - c->scan;
            protocol_error(s, c, GSP_PE_LEADING_GARBAGE, c->rx + c->scan, n, n, now);
            reset_rx(c);
            *discarded = true;
            return GSP_OK;
        }

        c->scan++;

        if (step == GS_FRAME_CLOSE) {
            if (!deliver_message(s, c, c->rx, c->scan, now)) {
                /* The object was framed but is not JSON; the rest of the
                 * stream is suspect for the same reason garbage is. */
                reset_rx(c);
                *discarded = true;
                return GSP_OK;
            }
            compact(c);
            continue;
        }

        /*
         * ⚠ The bound is on ONE OBJECT, and the object starts at 0 (compact()
         * guarantees it), so its length is `scan`.  16 KiB by default: twenty
         * times the vendor's full example and a quarter of [OSG]'s own bound.
         */
        if (c->scan > (size_t)s->max_message_bytes) {
            protocol_error(s, c, GSP_PE_TOO_LARGE, c->rx, c->scan, c->rx_len, now);
            reset_rx(c);
            *discarded = true;
            return GSP_OK;
        }
    }
    /* ⚠ Drop consumed whitespace before returning, or a read that is ALL
     * whitespace fills the buffer with bytes the framer has already dealt with
     * and the next call finds no room to append into. */
    if (!gs_framer_in_message(&c->framer)) {
        compact(c);
    }
    return GSP_OK;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

static size_t align_up(size_t v)
{
    const size_t a = sizeof(void *) * 2u; /* generous, and a power of two */
    return (v + (a - 1u)) & ~(a - 1u);
}

gsp_status gsp_server_create(const gsp_server_config *config, gsp_server **out)
{
    gsp_server_config cfg;
    gsp_server *s;
    unsigned char *base;
    size_t off_conns, off_events, off_writes, off_wire, off_rx, total;
    uint32_t max_conn, ev_ring, w_ring, wire_ring, max_msg;
    size_t rx_cap;
    uint32_t i;

    if (out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    *out = NULL;
    cfg = (config != NULL) ? *config : gsp_server_config_default();

    max_conn = (cfg.max_connections != 0u) ? cfg.max_connections
                                           : (uint32_t)GSP_MAX_CONNECTIONS_DEFAULT;
    ev_ring = (cfg.event_ring != 0u) ? cfg.event_ring : (uint32_t)GSP_EVENT_RING_DEFAULT;
    w_ring = (cfg.write_ring != 0u) ? cfg.write_ring : (uint32_t)GSP_WRITE_RING_DEFAULT;
    wire_ring = cfg.wire_ring; /* 0 → the wire log is off (design §7) */
    max_msg = (cfg.policy.max_message_bytes != 0u) ? cfg.policy.max_message_bytes
                                                   : (uint32_t)(16u * 1024u);

    /* A bound too small to hold a reply is not a configuration, it is a typo. */
    if (max_msg < 256u) {
        return GSP_ERR_INVALID_ARG;
    }
    if (max_conn > 4096u || ev_ring > 1000000u || w_ring > 1000000u) {
        return GSP_ERR_INVALID_ARG;
    }
    /*
     * ⚠ One byte more than the bound, so that an object which EXCEEDS the
     * bound can be recognised as exceeding it rather than merely filling the
     * buffer — otherwise the framer would stall with nothing to report.
     */
    rx_cap = (size_t)max_msg + 1u;

    /* ONE allocation (design §3.4), and nothing allocates afterwards. */
    total = align_up(sizeof(gsp_server));
    off_conns = total;
    total = align_up(total + (size_t)max_conn * sizeof(gs_conn));
    off_events = total;
    total = align_up(total + (size_t)ev_ring * sizeof(gsp_event));
    off_writes = total;
    total = align_up(total + (size_t)w_ring * sizeof(gsp_write_request));
    off_wire = total;
    total = align_up(total + (size_t)wire_ring * sizeof(gsp_wire_chunk));
    off_rx = total;
    total = total + (size_t)max_conn * rx_cap;

    if (cfg.allocator.alloc != NULL && cfg.allocator.free != NULL) {
        base = (unsigned char *)cfg.allocator.alloc(cfg.allocator.ctx, total);
    } else {
        base = (unsigned char *)malloc(total);
    }
    if (base == NULL) {
        return GSP_ERR_NO_MEMORY;
    }
    memset(base, 0, total);

    s = (gsp_server *)(void *)base;
    s->policy = cfg.policy;
    s->allocator = cfg.allocator;
    s->has_allocator = (cfg.allocator.alloc != NULL && cfg.allocator.free != NULL) ? 1u : 0u;
    s->max_connections = max_conn;
    s->event_ring = ev_ring;
    s->write_ring = w_ring;
    s->wire_ring = wire_ring;
    s->max_message_bytes = max_msg;
    s->rx_capacity = rx_cap;
    s->now_us = 0;
    s->session_state = (uint8_t)GSP_SESSION_NONE;

    s->conns = (gs_conn *)(void *)(base + off_conns);
    s->events = (gsp_event *)(void *)(base + off_events);
    s->writes = (gsp_write_request *)(void *)(base + off_writes);
    s->wire = (wire_ring > 0u) ? (gsp_wire_chunk *)(void *)(base + off_wire) : NULL;

    for (i = 0; i < max_conn; ++i) {
        s->conns[i].rx = base + off_rx + (size_t)i * rx_cap;
    }

    *out = s;
    return GSP_OK;
}

void gsp_server_close(gsp_server *s)
{
    uint32_t i;

    if (s == NULL || s->closed) {
        return;
    }
    /* ⚠ Every open connection is reported closed FIRST, so a host that drains
     * once more after closing sees a complete log (design §3.3). */
    for (i = 0; i < s->max_connections; ++i) {
        gs_conn *c = &s->conns[i];
        if (!c->used) {
            continue;
        }
        emit_connection(s, GSP_EV_CONNECTION_CLOSED, c, GSP_CLOSE_SERVER_CLOSED, s->now_us);
        drop_writes_for(s, c->info.conn);
        c->used = 0u;
    }
    s->closed = 1u;
}

void gsp_server_destroy(gsp_server *s)
{
    if (s == NULL) {
        return;
    }
    if (s->has_allocator) {
        void (*fn)(void *, void *) = s->allocator.free;
        void *ctx = s->allocator.ctx;
        fn(ctx, s);
    } else {
        free(s);
    }
}

/* ------------------------------------------------------------------------ */
/* IN — from the host's socket code                                          */
/* ------------------------------------------------------------------------ */

gsp_status gsp_server_on_connection_opened(gsp_server *s, gsp_conn_id conn, const char *peer,
                                           gsp_time_us now_us)
{
    gs_conn *c = NULL;
    uint32_t i;

    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (s->closed) {
        return GSP_ERR_CLOSED;
    }
    /* ⚠ The id is the HOST's (design §3.2), but zero means "none" and
     * UINT32_MAX means "all", so neither can name a socket. */
    if (conn == GSP_CONN_NONE || conn == GSP_CONN_ALL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (find_conn(s, conn) != NULL) {
        return GSP_ERR_INVALID_STATE;
    }
    for (i = 0; i < s->max_connections; ++i) {
        if (!s->conns[i].used) {
            c = &s->conns[i];
            break;
        }
    }
    if (c == NULL) {
        /* ⚠ No event: it was never open.  The host closes the socket, and
         * nothing we send can tell the client why — the protocol has no
         * message for it (design §5.1). */
        return GSP_ERR_TOO_MANY_CONNECTIONS;
    }

    s->now_us = now_us;
    {
        uint8_t *rx = c->rx;
        memset(c, 0, sizeof(*c));
        c->rx = rx;
    }
    c->used = 1u;
    c->info.conn = conn;
    c->info.opened_us = now_us;
    c->info.last_message_us = GSP_TIME_UNKNOWN;
    c->info.last_shot_number = INT64_MIN;
    c->next_write_us = now_us;
    gs_framer_reset(&c->framer);
    /* ⚠ Personal data (design §9.2): stored for gsp_connection_info, redacted
     * by gsp_event_format() unless asked. */
    copy_bounded(c->info.peer, sizeof(c->info.peer), peer);

    emit_connection(s, GSP_EV_CONNECTION_OPENED, c, GSP_CLOSE_UNKNOWN, now_us);

    if (s->policy.announce_player_on_connect && s->player_set &&
        player_has_anything(&s->player)) {
        send_player_to(s, c, &s->player, GSP_PI_ON_CONNECT, now_us);
    }
    /* ⚠ AFTER the 201, because [OSP] arms only once it has seen the 202 and a
     * connector arriving mid-session should learn the club first (CT-P11). */
    if (s->policy.announce_ready_on_connect &&
        s->session_state == (uint8_t)GSP_SESSION_ACTIVE) {
        send_session_to(s, c, GSP_SESSION_ACTIVE, GSP_PI_ON_CONNECT, now_us);
    }
    return GSP_OK;
}

gsp_status gsp_server_on_bytes(gsp_server *s, gsp_conn_id conn, const uint8_t *data,
                               size_t len, gsp_time_us now_us)
{
    gs_conn *c;
    size_t pos = 0u;
    gsp_status st = GSP_OK;

    if (s == NULL || (data == NULL && len > 0u)) {
        return GSP_ERR_INVALID_ARG;
    }
    if (s->closed) {
        return GSP_ERR_CLOSED;
    }
    c = find_conn(s, conn);
    if (c == NULL) {
        return GSP_ERR_UNKNOWN_CONNECTION;
    }
    s->now_us = now_us;

    for (;;) {
        bool discarded = false;
        size_t take = s->rx_capacity - c->rx_len;

        if (take > len - pos) {
            take = len - pos;
        }
        if (take > 0u) {
            memcpy(c->rx + c->rx_len, data + pos, take);
            c->rx_len += take;
            pos += take;
        }

        st = frame_pending(s, c, now_us, &discarded);

        if (discarded) {
            /* The pending buffer was thrown away; the rest of this read is
             * part of the same desynchronised stream and goes with it. */
            pos = len;
            continue;
        }
        if (st == GSP_ERR_QUEUE_FULL) {
            break;
        }
        if (pos >= len) {
            break;
        }
        if (take == 0u) {
            /* No room and nothing consumed.  frame_pending() cannot leave this
             * state — an object filling the buffer exceeds the bound and is
             * reported — so treat it as a bug rather than spinning. */
            break;
        }
    }

    if (st == GSP_ERR_QUEUE_FULL) {
        size_t left = len - pos;
        size_t room = s->rx_capacity - c->rx_len;

        if (left > 0u && left <= room) {
            memcpy(c->rx + c->rx_len, data + pos, left);
            c->rx_len += left;
            left = 0u;
        }
        if (left > 0u) {
            /*
             * ⚠ THE ONE PLACE BYTES CAN BE LOST, and it says so rather than
             * splicing the stream: the write ring is full AND this read holds
             * more than the receive buffer can retain.  Reachable only by a
             * host that never drains writes (design §3.2's pump never sees
             * GSP_ERR_QUEUE_FULL at all) while handing over more than
             * policy.max_message_bytes in one call.  With the defaults the
             * write ring — 32 replies — fills long before 16 KiB of real
             * messages, which are 300-900 bytes each, so this is a bound on
             * fuzz, not on a launch monitor.
             */
            reset_rx(c);
            emit_warning(s, conn, GSP_WARN_WRITE_RING_FULL, (uint32_t)left,
                         "write ring full; unretainable bytes discarded", now_us);
        } else {
            emit_warning(s, conn, GSP_WARN_WRITE_RING_FULL, (uint32_t)c->rx_len,
                         "write ring full; drain writes and resume", now_us);
        }
        return GSP_ERR_QUEUE_FULL;
    }
    return GSP_OK;
}

gsp_status gsp_server_on_connection_closed(gsp_server *s, gsp_conn_id conn,
                                           gsp_close_cause cause, gsp_time_us now_us)
{
    gs_conn *c;

    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (s->closed) {
        return GSP_ERR_CLOSED;
    }
    c = find_conn(s, conn);
    if (c == NULL) {
        return GSP_ERR_UNKNOWN_CONNECTION;
    }
    s->now_us = now_us;
    emit_connection(s, GSP_EV_CONNECTION_CLOSED, c, cause, now_us);
    drop_writes_for(s, conn);
    c->used = 0u;
    return GSP_OK;
}

/* ------------------------------------------------------------------------ */
/* CLOCK — the host owns the timer                                           */
/* ------------------------------------------------------------------------ */

/* When this connection would go idle, or NEVER. */
static gsp_time_us idle_deadline(const gsp_server *s, const gs_conn *c)
{
    gsp_time_us base;

    if (s->policy.idle_alarm_us <= 0 || c->idle_reported) {
        return GSP_TIME_NEVER;
    }
    base = (c->info.last_message_us != GSP_TIME_UNKNOWN) ? c->info.last_message_us
                                                         : c->info.opened_us;
    return base + s->policy.idle_alarm_us;
}

gsp_time_us gsp_server_next_due_us(const gsp_server *s)
{
    gsp_time_us due = GSP_TIME_NEVER;
    uint32_t i;

    if (s == NULL || s->closed) {
        return GSP_TIME_NEVER;
    }
    /* ⚠ The ordinary answer is NEVER: the protocol has no deadline of its own
     * (protocol §9.6) and only the two optional policies arm anything. */
    for (i = 0; i < s->max_connections; ++i) {
        const gs_conn *c = &s->conns[i];
        gsp_time_us cand;
        if (!c->used) {
            continue;
        }
        cand = idle_deadline(s, c);
        if (cand < due) {
            due = cand;
        }
        if (s->policy.write_spacing_us > 0 && c->next_write_us > s->now_us) {
            uint32_t k;
            for (k = 0; k < s->w_count; ++k) {
                if (s->writes[(s->w_head + k) % s->write_ring].conn == c->info.conn) {
                    if (c->next_write_us < due) {
                        due = c->next_write_us;
                    }
                    break;
                }
            }
        }
    }
    return due;
}

void gsp_server_tick(gsp_server *s, gsp_time_us now_us)
{
    uint32_t i;

    if (s == NULL || s->closed) {
        return;
    }
    s->now_us = now_us;
    for (i = 0; i < s->max_connections; ++i) {
        gs_conn *c = &s->conns[i];
        if (!c->used) {
            continue;
        }
        if (idle_deadline(s, c) <= now_us) {
            /* ⚠ ONCE, and it NEVER closes anything: a client may be silent for
             * a whole warm-up (protocol §9.6).  This is for a UI that wants to
             * grey out a launch monitor that has gone quiet (design §5.7). */
            c->idle_reported = 1u;
            emit_simple(s, GSP_EV_CLIENT_IDLE, c->info.conn, now_us);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* HOST → CLIENTS                                                            */
/* ------------------------------------------------------------------------ */

gsp_status gsp_server_set_player(gsp_server *s, const gsp_player_info *info,
                                 gsp_time_us now_us)
{
    uint32_t i;
    bool changed;

    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (s->closed) {
        return GSP_ERR_CLOSED;
    }
    if (info == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    s->now_us = now_us;

    changed = !s->player_set || !gsp_player_info_equal(&s->player, info);
    s->player = *info;
    s->player_set = 1u;
    if (!changed) {
        /* ⚠ A host may call this on every UI change; an identical value is not
         * a change and must not queue a second 201 (design §5.6). */
        return GSP_OK;
    }
    if (!player_has_anything(&s->player)) {
        return GSP_OK;
    }
    for (i = 0; i < s->max_connections; ++i) {
        if (s->conns[i].used) {
            send_player_to(s, &s->conns[i], &s->player, GSP_PI_CHANGED, now_us);
        }
    }
    return GSP_OK;
}

void gsp_server_get_player(const gsp_server *s, gsp_player_info *out)
{
    if (out == NULL) {
        return;
    }
    if (s == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s->player;
}

gsp_status gsp_server_send_player_info(gsp_server *s, gsp_conn_id conn,
                                       const gsp_player_info *info, gsp_time_us now_us)
{
    const gsp_player_info *p;
    uint32_t i;

    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (s->closed) {
        return GSP_ERR_CLOSED;
    }
    p = (info != NULL) ? info : &s->player;
    s->now_us = now_us;

    if (conn == GSP_CONN_ALL) {
        for (i = 0; i < s->max_connections; ++i) {
            if (s->conns[i].used) {
                send_player_to(s, &s->conns[i], p, GSP_PI_EXPLICIT, now_us);
            }
        }
        return GSP_OK;
    }
    {
        gs_conn *c = find_conn(s, conn);
        if (c == NULL) {
            return GSP_ERR_UNKNOWN_CONNECTION;
        }
        send_player_to(s, c, p, GSP_PI_EXPLICIT, now_us);
    }
    return GSP_OK;
}

gsp_status gsp_server_set_session_state(gsp_server *s, gsp_session_state state,
                                        gsp_time_us now_us)
{
    uint32_t i;

    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    if (s->closed) {
        return GSP_ERR_CLOSED;
    }
    if (state != GSP_SESSION_NONE && state != GSP_SESSION_ACTIVE &&
        state != GSP_SESSION_ENDED) {
        return GSP_ERR_INVALID_ARG;
    }
    s->now_us = now_us;
    if (s->session_state == (uint8_t)state) {
        return GSP_OK; /* a repeated value queues nothing */
    }
    s->session_state = (uint8_t)state;
    if (state == GSP_SESSION_NONE) {
        return GSP_OK;
    }
    for (i = 0; i < s->max_connections; ++i) {
        if (s->conns[i].used) {
            send_session_to(s, &s->conns[i], state, GSP_PI_CHANGED, now_us);
        }
    }
    return GSP_OK;
}

gsp_session_state gsp_server_get_session_state(const gsp_server *s)
{
    if (s == NULL) {
        return GSP_SESSION_NONE;
    }
    return (gsp_session_state)s->session_state;
}

/* ------------------------------------------------------------------------ */
/* OUT — drained by the host, never pushed into it                           */
/* ------------------------------------------------------------------------ */

size_t gsp_server_poll_writes(gsp_server *s, gsp_write_request *out, size_t max)
{
    size_t n = 0u;
    uint32_t i = 0u;

    if (s == NULL || out == NULL || max == 0u) {
        return 0u;
    }
    while (i < s->w_count && n < max) {
        gsp_write_request *w = &s->writes[(s->w_head + i) % s->write_ring];
        gs_conn *c = find_conn(s, w->conn);

        /*
         * ⚠ policy.write_spacing_us holds a write back so a 200 and a 201 do
         * not share a TCP segment: six clients parse one read as one message
         * and PiTrac's receive thread DIES when they share one (protocol §9.8).
         * Held-back entries are skipped, never reordered within a connection.
         */
        if (c != NULL && s->now_us < c->next_write_us) {
            i++;
            continue;
        }
        out[n++] = *w;
        /* ⚠ RECORDED HERE RATHER THAN AT QUEUE TIME, because this is the moment
         * the bytes become the host's to write.  A reply held back by
         * write_spacing_us therefore appears in the capture where it appears on
         * the wire, and one addressed to a connection that went away before it
         * was polled is discarded by drop_writes_for() and never recorded — it
         * never went out (design §5.5). */
        wire_push_bytes(s, w->conn, GSP_WIRE_SERVER_TO_CLIENT, w->data, (size_t)w->length,
                        s->now_us, false, 0u);
        if (c != NULL && s->policy.write_spacing_us > 0) {
            c->next_write_us = s->now_us + s->policy.write_spacing_us;
        }
        /* Remove entry i, keeping everything before it in order. */
        {
            uint32_t k = i;
            while (k > 0u) {
                s->writes[(s->w_head + k) % s->write_ring] =
                    s->writes[(s->w_head + k - 1u) % s->write_ring];
                k--;
            }
        }
        s->w_head = (s->w_head + 1u) % s->write_ring;
        s->w_count--;
    }
    return n;
}

size_t gsp_server_poll_events(gsp_server *s, gsp_event *out, size_t max)
{
    size_t n = 0u;

    if (s == NULL || out == NULL || max == 0u) {
        return 0u;
    }
    while (s->ev_count > 0u && n < max) {
        out[n++] = s->events[s->ev_head];
        s->ev_head = (s->ev_head + 1u) % s->event_ring;
        s->ev_count--;
    }
    if (s->ev_count == 0u) {
        s->drop_warned = 0u; /* a fresh overflow run earns a fresh warning */
    }
    return n;
}

size_t gsp_server_poll_wire(gsp_server *s, gsp_wire_chunk *out, size_t max)
{
    size_t n = 0u;

    if (s == NULL || out == NULL || max == 0u || s->wire == NULL) {
        return 0u;
    }
    /* Chunks are pushed by the wire-log section above: one per framed object,
     * one per discarded run, one per reply as it is polled, and one META line
     * per connection event.  ⚠ Drop-oldest and counted — see wire_ring_push(). */
    while (s->wire_count > 0u && n < max) {
        out[n++] = s->wire[s->wire_head];
        s->wire_head = (s->wire_head + 1u) % s->wire_ring;
        s->wire_count--;
    }
    if (s->wire_count == 0u) {
        s->wire_drop_warned = 0u; /* a fresh overflow run earns a fresh warning */
    }
    return n;
}

uint32_t gsp_server_dropped_events(const gsp_server *s)
{
    return (s == NULL) ? 0u : s->dropped_events;
}

uint32_t gsp_server_dropped_wire(const gsp_server *s)
{
    return (s == NULL) ? 0u : s->dropped_wire;
}

/* ------------------------------------------------------------------------ */
/* Introspection                                                             */
/* ------------------------------------------------------------------------ */

size_t gsp_server_connection_count(const gsp_server *s)
{
    size_t n = 0u;
    uint32_t i;

    if (s == NULL) {
        return 0u;
    }
    for (i = 0; i < s->max_connections; ++i) {
        if (s->conns[i].used) {
            n++;
        }
    }
    return n;
}

size_t gsp_server_connection_ids(const gsp_server *s, gsp_conn_id *out, size_t max)
{
    size_t n = 0u;
    uint32_t i;

    if (s == NULL) {
        return 0u;
    }
    for (i = 0; i < s->max_connections; ++i) {
        if (!s->conns[i].used) {
            continue;
        }
        if (out != NULL && n < max) {
            out[n] = s->conns[i].info.conn;
        }
        n++;
    }
    return n;
}

gsp_status gsp_server_connection_info(const gsp_server *s, gsp_conn_id conn,
                                      gsp_connection_info *out)
{
    uint32_t i;

    if (s == NULL || out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    for (i = 0; i < s->max_connections; ++i) {
        if (s->conns[i].used && s->conns[i].info.conn == conn) {
            *out = s->conns[i].info;
            return GSP_OK;
        }
    }
    return GSP_ERR_UNKNOWN_CONNECTION;
}
