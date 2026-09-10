/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_reply.c — CT-R01 … CT-R08, docs/conformance.md §3.4.
 *
 * ⚠ EXACTLY ONE REPLY PER MESSAGE, PROMPTLY, WHATEVER IT WAS.  [MLM] blocks on
 * a read with a two-second timeout and RE-SENDS the shot when it expires, so a
 * missing or late reply produces a duplicate shot rather than an error; [OCR]
 * waits ten seconds and gives up.  And [TNB] pairs replies to sends through a
 * queue, so an extra reply desynchronises it for the rest of the session.
 */
#include "gspro/gspro.h"
#include "gs_test.h"
#include "gs_srv.h"

static const char *const SHOT =
    "{\"DeviceID\":\"x\",\"ShotNumber\":1,\"APIversion\":\"1\","
    "\"BallData\":{\"Speed\":140.0,\"SpinAxis\":-3.0,\"TotalSpin\":2500.0,"
    "\"HLA\":1.0,\"VLA\":12.0},"
    "\"ShotDataOptions\":{\"ContainsBallData\":true,\"ContainsClubData\":false}}";

static const char *const HEARTBEAT =
    "{\"DeviceID\":\"x\",\"ShotNumber\":0,\"APIversion\":\"1\","
    "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false,"
    "\"IsHeartBeat\":true}}";

static const char *const STATUS =
    "{\"DeviceID\":\"x\",\"ShotNumber\":0,\"APIversion\":\"1\","
    "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false,"
    "\"LaunchMonitorIsReady\":true,\"IsHeartBeat\":false}}";

/* CT-R01 — one 200 for every kind, and it is queued BEFORE on_bytes returns so
 * a host that drains after each call has it on the socket inside its own event
 * loop latency. */
GS_TEST(CT_R01_one_ack_per_message_of_every_kind)
{
    static const char *const inputs[3] = { NULL, NULL, NULL };
    const char *msgs[3];
    size_t i;

    msgs[0] = SHOT;
    msgs[1] = HEARTBEAT;
    msgs[2] = STATUS;
    (void)inputs;

    for (i = 0; i < 3; ++i) {
        gsp_server *s = gs_srv_open();
        gsp_write_request w[8];
        size_t n;

        /* Drain whatever opening the connection queued, so this counts only
         * the reply to the message. */
        (void)gs_srv_writes(s, w, 8);
        (void)gs_srv_feed(s, msgs[i]);

        n = gs_srv_writes(s, w, 8);
        GS_ASSERT_MSG(n == 1, "exactly one reply per message, whatever its kind");
        if (n >= 1) {
            gsp_response r;
            memset(&r, 0, sizeof(r));
            GS_ASSERT_EQ(w[0].kind, GSP_WRITE_ACK);
            GS_ASSERT_EQ(w[0].conn, GS_CONN_A);
            GS_ASSERT(w[0].length > 0 && w[0].length <= GSP_WRITE_MAX);
            GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
            GS_ASSERT_EQ(r.code, GSP_CODE_SHOT_RECEIVED);
            GS_ASSERT_EQ(r.has_player, 0);
        }
        gs_srv_free(s);
    }
}

/* CT-R02 — two objects in one read get two acks, in order. */
GS_TEST(CT_R02_two_messages_two_acks_in_order)
{
    gsp_server *s = gs_srv_open();
    gsp_write_request w[8];
    char both[1024];
    size_t n;

    (void)gs_srv_writes(s, w, 8);
    snprintf(both, sizeof(both), "%s%s", HEARTBEAT, SHOT);
    (void)gs_srv_feed(s, both);

    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_EQ(n, 2);
    if (n == 2) {
        GS_ASSERT_EQ(w[0].kind, GSP_WRITE_ACK);
        GS_ASSERT_EQ(w[1].kind, GSP_WRITE_ACK);
    }
    gs_srv_free(s);
}

/* CT-R03 — one 501 per unparseable object, with the documented text. */
GS_TEST(CT_R03_failure_replies)
{
    gsp_server *s = gs_srv_open();
    gsp_write_request w[8];
    gsp_event evs[16];
    size_t n;

    (void)gs_srv_writes(s, w, 8);

    /* Leading garbage. */
    (void)gs_srv_feed(s, "xyz");
    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_MSG(n == 1, "one 501 for the garbage");
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(w[0].kind, GSP_WRITE_FAILURE);
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_FAILURE);
        GS_ASSERT_STR(r.message, GSP_TEXT_BAD_JSON);
    }
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT(gs_event_of(evs, n, GSP_EV_PROTOCOL_ERROR) != NULL);

    /* A top-level array. */
    (void)gs_srv_feed(s, "[{\"a\":1}]");
    GS_ASSERT_EQ(gs_srv_writes(s, w, 8), 1);
    gs_srv_free(s);
}

/* CT-R04 / CT-R05 — an incomplete shot is acknowledged by default, and
 * refused only when the host asks.  ⚠ Default OFF: protocol U2/U8 have not
 * established what GSPro does, and a client told 501 may retry forever. */
GS_TEST(CT_R04_incomplete_shot_is_acked_by_default)
{
    gsp_server *s = gs_srv_open();
    gsp_write_request w[8];
    size_t n;

    (void)gs_srv_writes(s, w, 8);
    (void)gs_srv_feed(s, "{\"DeviceID\":\"x\",\"BallData\":{\"Speed\":140.0},"
                         "\"ShotDataOptions\":{\"ContainsBallData\":true,"
                         "\"ContainsClubData\":false}}");
    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_SHOT_RECEIVED);
    }
    gs_srv_free(s);
}

GS_TEST(CT_R05_incomplete_shot_refused_when_asked)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_write_request w[8];
    size_t n;

    cfg.policy.reject_incomplete_shots = true;
    s = gs_srv_create(&cfg);
    if (s != NULL) {
        (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    }
    (void)gs_srv_writes(s, w, 8);
    (void)gs_srv_feed(s, "{\"DeviceID\":\"x\",\"BallData\":{\"Speed\":140.0},"
                         "\"ShotDataOptions\":{\"ContainsBallData\":true,"
                         "\"ContainsClubData\":false}}");
    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(w[0].kind, GSP_WRITE_FAILURE);
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_FAILURE);
        GS_ASSERT_STR(r.message, GSP_TEXT_INCOMPLETE);
    }

    /* ⚠ And a COMPLETE shot is still acknowledged with the policy on. */
    (void)gs_srv_feed(s, SHOT);
    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        GS_ASSERT_EQ(w[0].kind, GSP_WRITE_ACK);
    }
    gs_srv_free(s);
}

/* CT-R06 — ⚠ THE WHOLE EMITTABLE SURFACE.  Every byte sequence the write ring
 * can produce must decode as one of exactly five codes.  There is no
 * send_raw(), and this is what says so in a way that keeps saying it. */
GS_TEST(CT_R06_every_emitted_write_is_one_of_five_codes)
{
    gsp_server *s = gs_srv_open();
    gsp_player_info p;
    gsp_write_request w[32];
    size_t n, i;
    int seen_ack = 0, seen_player = 0, seen_ready = 0, seen_ended = 0, seen_fail = 0;

    memset(&p, 0, sizeof(p));
    p.handed = (uint8_t)GSP_HANDED_RIGHT;
    p.club = (uint8_t)GSP_CLUB_PT;
    p.has_distance = 1;
    p.distance_to_target = 12.5;

    (void)gs_srv_feed(s, SHOT);
    (void)gsp_server_set_player(s, &p, gs_now);
    (void)gsp_server_set_session_state(s, GSP_SESSION_ACTIVE, gs_now);
    (void)gsp_server_set_session_state(s, GSP_SESSION_ENDED, gs_now);
    (void)gs_srv_feed(s, "!!!not json!!!");

    n = gs_srv_writes(s, w, 32);
    GS_ASSERT_MSG(n >= 5, "the five shapes should all have been produced");

    for (i = 0; i < n; ++i) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_MSG(gs_decode_write(&w[i], &r) == GSP_OK,
                      "every emitted write must be decodable JSON");
        switch (r.code) {
            case GSP_CODE_SHOT_RECEIVED: seen_ack = 1; break;
            case GSP_CODE_PLAYER_INFO:   seen_player = 1; break;
            case GSP_CODE_READY:         seen_ready = 1; break;
            case GSP_CODE_ROUND_ENDED:   seen_ended = 1; break;
            case GSP_CODE_FAILURE:       seen_fail = 1; break;
            default:
                GS_ASSERT_MSG(0, "a code outside {200,201,202,203,501} was emitted");
                break;
        }
        GS_ASSERT(w[i].length > 0 && w[i].length <= GSP_WRITE_MAX);
        /* ⚠ No delimiter of our own: mirror what GSPro does (protocol §2). */
        if (w[i].length > 0) {
            GS_ASSERT_MSG(w[i].data[w[i].length - 1u] == (uint8_t)'}',
                          "a reply ends at its closing brace, with nothing appended");
        }
    }
    GS_ASSERT(seen_ack && seen_player && seen_ready && seen_ended && seen_fail);
    gs_srv_free(s);
}

/* CT-R07 — the write ring is NOT drop-oldest.  A dropped reply re-sends a shot
 * from [MLM], so a full ring refuses the bytes instead and the host resumes. */
GS_TEST(CT_R07_write_ring_full_refuses_rather_than_drops)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_write_request w[64];
    char burst[4096];
    size_t used = 0, n, total;
    int i;
    gsp_status st;

    cfg.write_ring = 4;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    (void)gs_srv_writes(s, w, 64);

    burst[0] = '\0';
    for (i = 0; i < 12; ++i) {
        int wrote = snprintf(burst + used, sizeof(burst) - used, "%s", HEARTBEAT);
        GS_ASSERT(wrote > 0);
        used += (size_t)wrote;
    }
    st = gs_srv_feed(s, burst);
    GS_ASSERT_MSG(st == GSP_ERR_QUEUE_FULL,
                  "a full write ring refuses rather than dropping a reply");

    /* Drain and resume with a zero-length call, as server.h documents. */
    total = gs_srv_writes(s, w, 64);
    while (st == GSP_ERR_QUEUE_FULL) {
        st = gsp_server_on_bytes(s, GS_CONN_A, (const uint8_t *)"", 0, gs_now);
        n = gs_srv_writes(s, w + total, 64 - total);
        total += n;
        if (n == 0 && st == GSP_ERR_QUEUE_FULL) {
            GS_ASSERT_MSG(0, "resume made no progress");
            break;
        }
    }
    GS_ASSERT_MSG(total == 12, "every message is eventually answered exactly once");
    gs_srv_free(s);
}

/* CT-R08 — the acknowledgement text is policy, because [TNB] reports the real
 * GSPro misspells it and nobody keys on the string (protocol U4). */
GS_TEST(CT_R08_ack_text_is_policy)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_write_request w[8];
    size_t n;

    /* The default is the string the vendor's page lists. */
    s = gs_srv_open();
    (void)gs_srv_writes(s, w, 8);
    (void)gs_srv_feed(s, SHOT);
    n = gs_srv_writes(s, w, 8);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_STR(r.message, GSP_TEXT_SHOT_RECEIVED);
    }
    gs_srv_free(s);

    snprintf(cfg.policy.ack_text, sizeof(cfg.policy.ack_text), "%s",
             "Shot send and recieved");
    s = gs_srv_create(&cfg);
    if (s != NULL) {
        (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    }
    (void)gs_srv_writes(s, w, 8);
    (void)gs_srv_feed(s, SHOT);
    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_STR(r.message, "Shot send and recieved");
    }
    gs_srv_free(s);
}

GS_TEST_MAIN()
