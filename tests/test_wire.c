/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_wire.c — CT-W01 … CT-W12, docs/conformance.md §3.9.
 *
 * ⚠ THESE CASES ARE ABOUT EVIDENCE, not about behaviour a client can see.  The
 * wire log exists because protocol §11 lists ten open questions and a
 * BYTE-LEVEL capture of the session that answers one re-decodes with the fix
 * applied, while a decoded log has already thrown away what the fix would have
 * read differently (design §7).  Every row below is a property the first
 * capture off real hardware (design §11 package 7) has to have, checked now,
 * because a capture taken without them cannot be retaken.
 *
 * ⚠ AND HALF OF THEM ARE PRIVACY.  A peer address identifies a household and a
 * DeviceID can carry a hardware serial ([GC2]); CT-W09 and CT-W10 are the two
 * halves of design §9.2's promise that neither is written unless asked.
 */
#include "gs_srv.h"

#define GS_WIRE_MAX 64

/* Drain the whole ring, however many polls that takes. */
static size_t gs_wire_all(gsp_server *s, gsp_wire_chunk *out, size_t max)
{
    size_t total = 0u;
    size_t n;
    if (s == NULL) {
        return 0u;
    }
    while (total < max && (n = gsp_server_poll_wire(s, out + total, max - total)) > 0u) {
        total += n;
    }
    return total;
}

/* A server with the log ON.  ⚠ Nothing else in the suite does this, which is
 * itself CT-W01: the log is off by default and every other case runs without
 * paying for it. */
static gsp_server *gs_wire_server(uint32_t ring, bool identifiers)
{
    gsp_server_config cfg = gsp_server_config_default();
    cfg.wire_ring = ring;
    cfg.policy.record_identifiers = identifiers;
    return gs_srv_create(&cfg);
}

static gsp_server *gs_wire_open(uint32_t ring, bool identifiers)
{
    gsp_server *s = gs_wire_server(ring, identifiers);
    if (s != NULL) {
        (void)gsp_server_on_connection_opened(s, GS_CONN_A, "203.0.113.7:51022", gs_now);
    }
    return s;
}

/* The chunks of one direction, in order. */
static size_t gs_wire_of(const gsp_wire_chunk *all, size_t n, gsp_wire_direction dir,
                         const gsp_wire_chunk **out, size_t max)
{
    size_t i;
    size_t k = 0u;
    for (i = 0; i < n && k < max; ++i) {
        if (all[i].direction == (uint8_t)dir) {
            out[k++] = &all[i];
        }
    }
    return k;
}

/* ------------------------------------------------------------------------ */
/* CT-W01 — OFF unless asked.  ⚠ The default a case has to state rather than  */
/* assume: a library that recorded a household's traffic because somebody     */
/* turned on verbose logging would be a different library (design §9.2).      */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W01_the_log_is_off_by_default)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_wire_chunk chunks[4];

    GS_ASSERT_EQ(cfg.wire_ring, 0u);
    s = gs_srv_open();
    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed(s, "{\"DeviceID\":\"x\",\"ShotNumber\":1}"), GSP_OK);
    GS_ASSERT_EQ(gsp_server_poll_wire(s, chunks, 4u), 0u);
    GS_ASSERT_EQ(gsp_server_dropped_wire(s), 0u);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W02 — one message, its exact bytes.                                    */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W02_a_message_is_recorded_byte_for_byte)
{
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[8];
    size_t n;
    size_t len = 0u;
    unsigned char *shot;

    if (s == NULL) {
        return;
    }
    /* ⚠ A fixture that FITS one chunk: [GSP]'s full example is 642 bytes and
     * spans two, which is CT-W05's subject rather than this one. */
    shot = gs_fixture("tl_minimal.json", &len);
    GS_ASSERT(len <= (size_t)GSP_WIRE_CHUNK_MAX);
    GS_ASSERT_EQ(gs_srv_feed_n(s, shot, len), GSP_OK);

    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u), 1u);
    GS_ASSERT_EQ(client[0]->length, (uint16_t)len);
    GS_ASSERT_MEM(client[0]->data, shot, len);
    GS_ASSERT_EQ(client[0]->conn, GS_CONN_A);
    GS_ASSERT_EQ(client[0]->host_time_us, gs_now);
    GS_ASSERT_EQ(client[0]->flags & (uint8_t)GSP_WIRE_CONTINUES, 0u);
    free(shot);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W03 — two objects in one read are TWO chunks.  ⚠ A chunk is a message,  */
/* not a read: read boundaries are the kernel's, not the client's.            */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W03_two_objects_in_one_read_are_two_chunks)
{
    const char *a = "{\"DeviceID\":\"A\",\"ShotNumber\":1}";
    const char *b = "{\"DeviceID\":\"A\",\"ShotNumber\":2}";
    char both[128];
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[8];
    size_t n;

    if (s == NULL) {
        return;
    }
    (void)snprintf(both, sizeof(both), "%s%s", a, b);
    GS_ASSERT_EQ(gs_srv_feed(s, both), GSP_OK);

    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u), 2u);
    GS_ASSERT_EQ(client[0]->length, (uint16_t)strlen(a));
    GS_ASSERT_MEM(client[0]->data, a, strlen(a));
    GS_ASSERT_EQ(client[1]->length, (uint16_t)strlen(b));
    GS_ASSERT_MEM(client[1]->data, b, strlen(b));
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W04 — one object split across three reads is ONE chunk, when it closes. */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W04_an_object_split_across_reads_is_one_chunk)
{
    const char *json = "{\"DeviceID\":\"A\",\"ShotNumber\":7}";
    size_t len = strlen(json);
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[8];
    size_t n;

    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed_n(s, json, 10u), GSP_OK);
    GS_ASSERT_EQ(gs_wire_all(s, all, GS_WIRE_MAX), 1u);   /* the META open only */
    GS_ASSERT_EQ(all[0].direction, (uint8_t)GSP_WIRE_META);

    GS_ASSERT_EQ(gs_srv_feed_n(s, json + 10, 10u), GSP_OK);
    GS_ASSERT_EQ(gs_wire_all(s, all, GS_WIRE_MAX), 0u);   /* still incomplete   */

    GS_ASSERT_EQ(gs_srv_feed_n(s, json + 20, len - 20u), GSP_OK);
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u), 1u);
    GS_ASSERT_EQ(client[0]->length, (uint16_t)len);
    GS_ASSERT_MEM(client[0]->data, json, len);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W05 — longer than GSP_WIRE_CHUNK_MAX: consecutive chunks, CONTINUES on  */
/* all but the last, concatenating to the original bytes.                     */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W05_a_long_object_spans_chunks_that_reassemble)
{
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[16];
    char big[1400];
    char rebuilt[1400];
    size_t at = 0u;
    size_t n;
    size_t parts;
    size_t i;
    size_t total = 0u;

    if (s == NULL) {
        return;
    }
    /* One legitimate object, comfortably over two chunks: a long unknown key's
     * value, which §9.7 says is skipped and counted rather than fatal. */
    at = (size_t)snprintf(big, sizeof(big), "{\"DeviceID\":\"A\",\"Note\":\"");
    while (at < sizeof(big) - 8u) {
        big[at] = (char)('a' + (int)(at % 26u));
        at++;
    }
    at += (size_t)snprintf(big + at, sizeof(big) - at, "\"}");
    GS_ASSERT(at > (size_t)GSP_WIRE_CHUNK_MAX * 2u);
    GS_ASSERT_EQ(gs_srv_feed_n(s, big, at), GSP_OK);

    n = gs_wire_all(s, all, GS_WIRE_MAX);
    parts = gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 16u);
    GS_ASSERT_EQ(parts, (at + (size_t)GSP_WIRE_CHUNK_MAX - 1u) / (size_t)GSP_WIRE_CHUNK_MAX);
    for (i = 0; i < parts; ++i) {
        bool last = (i + 1u == parts);
        GS_ASSERT_EQ((client[i]->flags & (uint8_t)GSP_WIRE_CONTINUES) != 0u, !last);
        /* ⚠ Consecutive sequence numbers, or a reader cannot tell a continued
         * message from two that happen to be adjacent. */
        if (i > 0u) {
            GS_ASSERT_EQ(client[i]->sequence, client[i - 1u]->sequence + 1u);
        }
        if (total + client[i]->length <= sizeof(rebuilt)) {
            memcpy(rebuilt + total, client[i]->data, client[i]->length);
        }
        total += client[i]->length;
    }
    GS_ASSERT_EQ(total, at);
    GS_ASSERT_MEM(rebuilt, big, at);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W06 — ⚠ WHAT FAILED IS RECORDED TOO.  A capture taken to explain a      */
/* client this library rejected, which omitted the bytes it rejected, would   */
/* omit the whole subject.                                                    */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W06_discarded_bytes_are_recorded)
{
    const char *garbage = "not json at all";
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[8];
    size_t n;
    gsp_server_config cfg;
    gsp_server *small;
    char oversize[600];

    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed(s, garbage), GSP_OK);
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u), 1u);
    GS_ASSERT_EQ(client[0]->length, (uint16_t)strlen(garbage));
    GS_ASSERT_MEM(client[0]->data, garbage, strlen(garbage));
    gs_srv_free(s);

    /* And the oversize path, which discards for a different reason. */
    cfg = gsp_server_config_default();
    cfg.wire_ring = 32u;
    cfg.policy.record_identifiers = true;
    cfg.policy.max_message_bytes = 256u;
    small = gs_srv_create(&cfg);
    if (small == NULL) {
        return;
    }
    (void)gsp_server_on_connection_opened(small, GS_CONN_A, NULL, gs_now);
    memset(oversize, 'x', sizeof(oversize));
    oversize[0] = '{';
    GS_ASSERT_EQ(gsp_server_on_bytes(small, GS_CONN_A, (const uint8_t *)oversize,
                                     sizeof(oversize), gs_now), GSP_OK);
    n = gs_wire_all(small, all, GS_WIRE_MAX);
    GS_ASSERT(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u) > 0u);
    GS_ASSERT_EQ(client[0]->data[0], (uint8_t)'{');
    gs_srv_free(small);
}

/* ------------------------------------------------------------------------ */
/* CT-W07 — every reply, its exact bytes, recorded when the host polls it.    */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W07_replies_are_recorded_as_they_are_polled)
{
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *server[8];
    gsp_write_request writes[GS_DRAIN_MAX];
    size_t n;
    size_t w;

    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed(s, "{\"DeviceID\":\"A\",\"ShotNumber\":1}"), GSP_OK);

    /* ⚠ Not yet: the reply is queued, and a queued reply has not gone out.  A
     * capture that recorded it here would show bytes on the wire that a
     * connection dropped before the poll never sent (design §5.5). */
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_SERVER_TO_CLIENT, server, 8u), 0u);

    w = gs_srv_writes(s, writes, GS_DRAIN_MAX);
    GS_ASSERT_EQ(w, 1u);
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_SERVER_TO_CLIENT, server, 8u), 1u);
    if (w == 1u) {
        GS_ASSERT_EQ(server[0]->length, writes[0].length);
        GS_ASSERT_MEM(server[0]->data, writes[0].data, writes[0].length);
        GS_ASSERT_EQ(server[0]->conn, writes[0].conn);
    }
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W08 — the connection's own life, so a capture reads back as a session.  */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W08_open_and_close_are_recorded)
{
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *meta[8];
    size_t n;
    size_t count;

    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed(s, "{\"DeviceID\":\"A\",\"ShotNumber\":1}"), GSP_OK);
    GS_ASSERT_EQ(gsp_server_on_connection_closed(s, GS_CONN_A, GSP_CLOSE_REMOTE_CLOSED,
                                                 gs_now), GSP_OK);
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    count = gs_wire_of(all, n, GSP_WIRE_META, meta, 8u);
    GS_ASSERT_EQ(count, 2u);
    if (count == 2u) {
        GS_ASSERT(gs_memfind(meta[0]->data, meta[0]->length, "CONNECTION_OPENED") != NULL);
        GS_ASSERT(gs_memfind(meta[1]->data, meta[1]->length, "CONNECTION_CLOSED") != NULL);
        /* The counters are in the line, so a reader knows what the session saw
         * without re-deriving it from the chunks. */
        GS_ASSERT(gs_memfind(meta[1]->data, meta[1]->length, "REMOTE_CLOSED") != NULL);
        GS_ASSERT(gs_memfind(meta[1]->data, meta[1]->length, "messages=1") != NULL);
    }
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W09 — ⚠ THE PRIVACY PROMISE (design §9.2).  Identifiers are redacted    */
/* unless asked for, in place, so the JSON still parses and offsets hold.     */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W09_identifiers_are_redacted_by_default)
{
    const char *json = "{\"DeviceID\":\"GC2-SERIAL-12345\",\"ShotNumber\":1}";
    gsp_server *s = gs_wire_open(32u, false);   /* the DEFAULT */
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[8];
    const gsp_wire_chunk *meta[8];
    gsp_message decoded;
    size_t n;

    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed(s, json), GSP_OK);
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u), 1u);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_META, meta, 8u), 1u);

    /* The serial is GONE, and says so. */
    GS_ASSERT(gs_memfind(client[0]->data, client[0]->length, "GC2-SERIAL-12345") == NULL);
    GS_ASSERT(gs_memfind(client[0]->data, client[0]->length, "<redacted>") != NULL);
    GS_ASSERT((client[0]->flags & (uint8_t)GSP_WIRE_REDACTED) != 0u);
    /* ⚠ THE SAME LENGTH, so every offset after it still holds and the chunk is
     * still a JSON object a reader can decode. */
    GS_ASSERT_EQ(client[0]->length, (uint16_t)strlen(json));
    GS_ASSERT_EQ(gsp_message_decode(client[0]->data, client[0]->length, &decoded), GSP_OK);
    GS_ASSERT_EQ(decoded.shot_number, 1);
    GS_ASSERT(strstr(decoded.device_id, "SERIAL") == NULL);
    /* And the peer address is not in the META line either. */
    GS_ASSERT(gs_memfind(meta[0]->data, meta[0]->length, "203.0.113.7") == NULL);
    GS_ASSERT((meta[0]->flags & (uint8_t)GSP_WIRE_REDACTED) != 0u);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W10 — and verbatim when a capture is deliberately taken with them.      */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W10_identifiers_are_kept_when_asked_for)
{
    const char *json = "{\"DeviceID\":\"GC2-SERIAL-12345\",\"ShotNumber\":1}";
    gsp_server *s = gs_wire_open(32u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    const gsp_wire_chunk *client[8];
    const gsp_wire_chunk *meta[8];
    size_t n;

    if (s == NULL) {
        return;
    }
    GS_ASSERT_EQ(gs_srv_feed(s, json), GSP_OK);
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_CLIENT_TO_SERVER, client, 8u), 1u);
    GS_ASSERT_EQ(gs_wire_of(all, n, GSP_WIRE_META, meta, 8u), 1u);
    GS_ASSERT(gs_memfind(client[0]->data, client[0]->length, "GC2-SERIAL-12345") != NULL);
    GS_ASSERT_EQ(client[0]->flags & (uint8_t)GSP_WIRE_REDACTED, 0u);
    GS_ASSERT(gs_memfind(meta[0]->data, meta[0]->length, "203.0.113.7") != NULL);
    GS_ASSERT_EQ(meta[0]->flags & (uint8_t)GSP_WIRE_REDACTED, 0u);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W11 — a ring smaller than the traffic: drop-OLDEST, counted, and the    */
/* survivor says so.  ⚠ The opposite of the write ring, which refuses.        */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W11_a_full_ring_drops_oldest_and_says_so)
{
    gsp_server *s = gs_wire_open(4u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    gsp_event events[GS_DRAIN_MAX];
    size_t n;
    size_t e;
    size_t i;
    size_t warnings = 0u;
    char json[64];

    if (s == NULL) {
        return;
    }
    for (i = 0; i < 10u; ++i) {
        (void)snprintf(json, sizeof(json), "{\"DeviceID\":\"A\",\"ShotNumber\":%u}",
                       (unsigned)i);
        GS_ASSERT_EQ(gs_srv_feed(s, json), GSP_OK);
    }
    GS_ASSERT(gsp_server_dropped_wire(s) > 0u);

    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(n, 4u);                       /* the ring, and no more         */
    GS_ASSERT((all[0].flags & (uint8_t)GSP_WIRE_LOST) != 0u);
    for (i = 1; i < n; ++i) {
        GS_ASSERT_EQ(all[i].flags & (uint8_t)GSP_WIRE_LOST, 0u);
    }

    /* ⚠ One warning per overflow RUN, not one per dropped chunk — a warning per
     * drop would fill the event ring with complaints about the wire ring. */
    e = gs_srv_events(s, events, GS_DRAIN_MAX);
    for (i = 0; i < e; ++i) {
        if (events[i].type == (uint8_t)GSP_EV_WARNING
            && events[i].u.warning.code == (uint16_t)GSP_WARN_WIRE_DROPPED) {
            warnings++;
        }
    }
    GS_ASSERT_EQ(warnings, 1u);
    gs_srv_free(s);
}

/* ------------------------------------------------------------------------ */
/* CT-W12 — sequence numbers are the reader's only way to see a gap.  ⚠ A     */
/* reader that renumbered from its own ordinal would turn a lossy capture     */
/* into a complete-looking one.                                              */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_W12_sequence_increases_and_gaps_are_visible)
{
    gsp_server *s = gs_wire_open(64u, true);
    gsp_wire_chunk all[GS_WIRE_MAX];
    size_t n;
    size_t i;
    char json[64];

    if (s == NULL) {
        return;
    }
    for (i = 0; i < 5u; ++i) {
        (void)snprintf(json, sizeof(json), "{\"DeviceID\":\"A\",\"ShotNumber\":%u}",
                       (unsigned)i);
        GS_ASSERT_EQ(gs_srv_feed(s, json), GSP_OK);
    }
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT(n >= 6u);
    for (i = 1; i < n; ++i) {
        GS_ASSERT_EQ(all[i].sequence, all[i - 1u].sequence + 1u);   /* no gap here */
    }
    gs_srv_free(s);

    /* And with a ring that overflowed, the gap is exactly what was dropped. */
    s = gs_wire_open(4u, true);
    if (s == NULL) {
        return;
    }
    for (i = 0; i < 10u; ++i) {
        (void)snprintf(json, sizeof(json), "{\"DeviceID\":\"A\",\"ShotNumber\":%u}",
                       (unsigned)i);
        GS_ASSERT_EQ(gs_srv_feed(s, json), GSP_OK);
    }
    n = gs_wire_all(s, all, GS_WIRE_MAX);
    GS_ASSERT_EQ(n, 4u);
    GS_ASSERT_EQ(all[0].sequence, gsp_server_dropped_wire(s));
    for (i = 1; i < n; ++i) {
        GS_ASSERT_EQ(all[i].sequence, all[i - 1u].sequence + 1u);
    }
    gs_srv_free(s);
}

GS_TEST_MAIN()
