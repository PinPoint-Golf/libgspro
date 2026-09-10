/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_frame.c — CT-F01 … CT-F12, docs/conformance.md §3.1.
 *
 * ⚠ THERE IS NO FRAMING ON THIS WIRE (protocol.md §2).  Messages are JSON
 * objects written back to back with nothing between them; three clients of
 * sixteen append a newline and thirteen do not; two send objects containing
 * newlines and tabs of their own.  Every case here is one shape a real client
 * puts on a socket.
 */
#include "gspro/gspro.h"
#include "gs_test.h"
#include "gs_srv.h"

/* Frame the whole of `text`, appending each object's span to `spans`.
 * Returns the number of complete objects found, and leaves the terminal status
 * in *tail. */
static size_t frame_all(const char *text, size_t (*spans)[2], size_t max, gsp_status *tail)
{
    const uint8_t *buf = (const uint8_t *)text;
    size_t len = strlen(text);
    size_t off = 0, found = 0;

    for (;;) {
        size_t start = 0, end = 0;
        gsp_status st = gsp_frame_find(buf + off, len - off, &start, &end);
        if (st != GSP_OK) {
            *tail = st;
            return found;
        }
        if (found < max) {
            spans[found][0] = off + start;
            spans[found][1] = off + end;
        }
        found++;
        off += end;
        if (off >= len) {
            *tail = GSP_PENDING;
            return found;
        }
    }
}

/* CT-F01 — one compact object in one call. [MLM] [TL] [R10] [OF] [OB] [GC2] */
GS_TEST(CT_F01_single_compact_object)
{
    const char *msg = "{\"DeviceID\":\"x\",\"ShotNumber\":1}";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(msg), &start, &end), GSP_OK);
    GS_ASSERT_EQ(start, 0);
    GS_ASSERT_EQ(end, strlen(msg));
}

/* CT-F02 — the same object delivered one byte at a time.  TCP guarantees
 * nothing about read boundaries, so every prefix must be PENDING and only the
 * last byte may complete the message. */
GS_TEST(CT_F02_byte_at_a_time)
{
    const char *msg = "{\"DeviceID\":\"x\",\"BallData\":{\"Speed\":1.5}}";
    size_t full = strlen(msg);
    size_t n;

    for (n = 0; n < full; ++n) {
        size_t start = 999, end = 999;
        gsp_status st = gsp_frame_find((const uint8_t *)msg, n, &start, &end);
        GS_ASSERT_MSG(st == GSP_PENDING, "a prefix must never complete a message");
    }
    {
        size_t start = 999, end = 999;
        GS_ASSERT_EQ(gsp_frame_find((const uint8_t *)msg, full, &start, &end), GSP_OK);
        GS_ASSERT_EQ(end, full);
    }
}

/* CT-F03 — two objects in one call.  GSPro itself does this ([OSP], [SLX],
 * [MLM] all grew splitters for it), so a client will too. */
GS_TEST(CT_F03_two_objects_one_read)
{
    const char *text = "{\"a\":1}{\"b\":2}";
    size_t spans[4][2];
    gsp_status tail = GSP_OK;
    GS_ASSERT_EQ(frame_all(text, spans, 4, &tail), 2);
    GS_ASSERT_EQ(spans[0][0], 0);
    GS_ASSERT_EQ(spans[0][1], 7);
    GS_ASSERT_EQ(spans[1][0], 7);
    GS_ASSERT_EQ(spans[1][1], 14);
}

/* CT-F04 — object followed by a newline.  [FB], [GG] and [PIT] delimit; the
 * newline is trailing whitespace and must not become part of the object nor an
 * error. */
GS_TEST(CT_F04_trailing_newline)
{
    const char *text = "{\"a\":1}\n";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(text), &start, &end), GSP_OK);
    GS_ASSERT_EQ(start, 0);
    GS_ASSERT_MSG(end == 7, "the object ends at its brace, not at the delimiter");
    /* What is left is whitespace: pending, not garbage. */
    GS_ASSERT_EQ(gsp_frame_find((const uint8_t *)text + end, strlen(text) - end, &start, &end),
                 GSP_PENDING);
}

/* CT-F05 — CRLF between two objects. */
GS_TEST(CT_F05_crlf_between_objects)
{
    const char *text = "{\"a\":1}\r\n{\"b\":2}";
    size_t spans[4][2];
    gsp_status tail = GSP_OK;
    GS_ASSERT_EQ(frame_all(text, spans, 4, &tail), 2);
    GS_ASSERT_EQ(spans[1][0], 9);
    GS_ASSERT_EQ(spans[1][1], 16);
}

/* CT-F06 — an indented object, newlines and tabs INSIDE one message.  [TNB]
 * and [OSP] send Newtonsoft's indented form and [PIT] sends Boost's, so a
 * framer that split on newlines would break on three clients. */
GS_TEST(CT_F06_indented_object_is_one_message)
{
    size_t len = 0;
    unsigned char *fx = gs_fixture("tnb_indented.json", &len);
    size_t start = 999, end = 999;
    GS_ASSERT_MSG(memchr(fx, '\n', len) != NULL, "fixture should contain newlines");
    GS_ASSERT_EQ(gsp_frame_find(fx, len, &start, &end), GSP_OK);
    GS_ASSERT_EQ(start, 0);
    GS_ASSERT_EQ(end, len);
    free(fx);
}

/* CT-F07 — braces and an escaped quote inside a string.  A bare depth counter
 * ([KJD], [SLX] both use one) wedges here forever; ours counts outside strings
 * only.  A DeviceID is free text chosen by a client author. */
GS_TEST(CT_F07_braces_inside_a_string)
{
    const char *text = "{\"DeviceID\":\"PiTrac {v2} \\\"beta\\\" }}}\",\"ShotNumber\":1}";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(text), &start, &end), GSP_OK);
    GS_ASSERT_EQ(start, 0);
    GS_ASSERT_MSG(end == strlen(text), "a brace inside a string must not close the object");
}

/* CT-F08 — the read boundary falls inside a string literal. */
GS_TEST(CT_F08_split_inside_string)
{
    const char *text = "{\"DeviceID\":\"half";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(text), &start, &end), GSP_PENDING);

    {
        const char *whole = "{\"DeviceID\":\"half here\"}";
        GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(whole), &start, &end), GSP_OK);
        GS_ASSERT_EQ(end, strlen(whole));
    }
}

/* CT-F09 — a non-whitespace byte before any object.  Report where, so the host
 * can discard through it and rescan (design §5.3). */
GS_TEST(CT_F09_leading_garbage)
{
    const char *text = "xyz{\"a\":1}";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(text), &start, &end), GSP_ERR_MALFORMED);
    GS_ASSERT_MSG(start == 0, "start names the offending byte");

    /* Having discarded it, the object behind it decodes. */
    GS_ASSERT_EQ(gsp_frame_find((const uint8_t *)text + 3, strlen(text) - 3, &start, &end),
                 GSP_OK);
    GS_ASSERT_EQ(end, 7);
}

/* CT-F10 — a top-level array.  The protocol has no arrays at top level and
 * treating one as a message would be inventing a protocol (design §5.3). */
GS_TEST(CT_F10_top_level_array_is_garbage)
{
    const char *text = "[{\"a\":1}]";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(text), &start, &end), GSP_ERR_MALFORMED);
    GS_ASSERT_EQ(start, 0);
}

/* Leading whitespace alone is pending, never garbage — [MLM]'s splitter allows
 * spaces between objects, so they arrive. */
GS_TEST(CT_F10b_whitespace_only_is_pending)
{
    const char *text = "  \r\n\t ";
    size_t start = 999, end = 999;
    GS_ASSERT_EQ(gsp_frame_find(GS_BYTES(text), &start, &end), GSP_PENDING);
    GS_ASSERT_MSG(start == strlen(text), "nothing began, so start is the length");
}

/* CT-F11 — an object larger than policy.max_message_bytes.  Server-level: the
 * framer itself is stateless and unbounded; the bound lives with the buffer. */
GS_TEST(CT_F11_oversize_is_rejected_and_resyncs)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_event evs[16];
    gsp_write_request w[8];
    size_t n, nw;
    char *big;
    size_t big_len = 4096;

    cfg.policy.max_message_bytes = 1024;
    s = gs_srv_create(&cfg);
    if (s != NULL) {
        (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    }

    big = (char *)malloc(big_len + 1);
    GS_ASSERT(big != NULL);
    if (big == NULL) {
        gs_srv_free(s);
        return;
    }
    memset(big, 'a', big_len);
    big[0] = '{';
    big[1] = '"';
    big[big_len] = '\0';
    (void)gs_srv_feed_n(s, big, big_len);
    free(big);

    n = gs_srv_events(s, evs, 16);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_PROTOCOL_ERROR);
        GS_ASSERT_MSG(e != NULL, "an oversize message must be reported");
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.protocol_error.reason, GSP_PE_TOO_LARGE);
        }
    }
    nw = gs_srv_writes(s, w, 8);
    GS_ASSERT_MSG(nw == 1, "exactly one 501 for the oversize message");
    if (nw == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_FAILURE);
    }

    /* ⚠ And the connection recovers: a well-formed message after the flood is
     * decoded, because the buffer was discarded rather than the client dropped. */
    (void)gs_srv_feed(s, "{\"DeviceID\":\"after\",\"ShotDataOptions\":"
                         "{\"ContainsBallData\":false,\"ContainsClubData\":false}}");
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_MSG(gs_event_of(evs, n, GSP_EV_STATUS) != NULL,
                  "the framer must resynchronise after an oversize message");
    gs_srv_free(s);
}

/* CT-F12 — a burst of ten objects in one call: ten messages, ten replies. */
GS_TEST(CT_F12_ten_objects_one_read)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[64];
    gsp_write_request w[32];
    char burst[2048];
    size_t used = 0;
    int i;

    burst[0] = '\0';
    for (i = 0; i < 10; ++i) {
        int wrote = snprintf(burst + used, sizeof(burst) - used,
                             "{\"DeviceID\":\"burst\",\"ShotNumber\":%d,"
                             "\"ShotDataOptions\":{\"ContainsBallData\":false,"
                             "\"ContainsClubData\":false,\"IsHeartBeat\":true}}",
                             i);
        GS_ASSERT(wrote > 0);
        used += (size_t)wrote;
    }
    (void)gs_srv_feed(s, burst);

    GS_ASSERT_EQ(gs_count_of(evs, gs_srv_events(s, evs, 64), GSP_EV_HEARTBEAT), 10);
    GS_ASSERT_EQ(gs_srv_writes(s, w, 32), 10);
    gs_srv_free(s);
}

GS_TEST_MAIN()
