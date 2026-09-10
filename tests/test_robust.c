/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_robust.c — CT-X01 … CT-X08, docs/conformance.md §3.7.
 *
 * ⚠ EVERY BYTE ON THIS SOCKET IS UNTRUSTED.  The port is open, unauthenticated
 * and, in the setup this library is built for, reachable from the LAN (design
 * §6.1, §9.3).  A hostile client must at worst occupy its own connection being
 * told 501; it must never reach another connection, allocate without bound, or
 * make the library call anything.
 *
 * ⚠ Worth running under the `san` preset, which is what it is there for.
 */
#include "gspro/gspro.h"
#include "gs_test.h"
#include "gs_srv.h"

/* A tiny deterministic PRNG.  ⚠ NOT rand(): the purity gate forbids the
 * library touching it, and a fuzz case that is not reproducible from its seed
 * is a case that cannot be re-run against a fix. */
static uint32_t rng_state = 0x1234567u;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* CT-X01 — a flood of open braces: bounded memory, an error per bound, and the
 * connection survives. */
GS_TEST(CT_X01_brace_flood_is_bounded)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_event evs[64];
    gsp_write_request w[64];
    char chunk[4096];
    int round;

    cfg.policy.max_message_bytes = 2048;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    (void)gs_srv_writes(s, w, 64);

    memset(chunk, '{', sizeof(chunk));
    for (round = 0; round < 64; ++round) {          /* 256 KiB of it */
        (void)gs_srv_feed_n(s, chunk, sizeof(chunk));
        (void)gs_srv_events(s, evs, 64);
        (void)gs_srv_writes(s, w, 64);
    }

    GS_ASSERT_MSG(gsp_server_connection_count(s) == 1,
                  "the library never closes a connection itself");

    /* ⚠ And it still works: a good message after the flood decodes. */
    (void)gs_srv_feed(s, "{\"DeviceID\":\"survivor\",\"ShotDataOptions\":"
                         "{\"ContainsBallData\":false,\"ContainsClubData\":false}}");
    {
        size_t n = gs_srv_events(s, evs, 64);
        GS_ASSERT_MSG(gs_event_of(evs, n, GSP_EV_STATUS) != NULL,
                      "the framer resynchronises after any amount of garbage");
    }
    gs_srv_free(s);
}

/* CT-X02 — random bytes at the decoder and the framer.  The requirement is
 * that they RETURN, and never claim success on something that is not an object. */
GS_TEST(CT_X02_random_bytes_at_the_codec)
{
    uint8_t buf[512];
    int iter;

    rng_state = 0x1234567u;
    for (iter = 0; iter < 10000; ++iter) {
        size_t len = (size_t)(rng_next() % sizeof(buf));
        size_t i;
        gsp_message m;
        gsp_response r;
        size_t start = 0, end = 0;

        for (i = 0; i < len; ++i) {
            uint32_t x = rng_next();
            /* Weighted towards structural characters, which is where a decoder
             * actually breaks — pure noise mostly fails at byte one. */
            uint8_t structural = (uint8_t)"{}[]\":,\\ \n\t0123456789"[(x >> 8) % 21u];
            uint8_t noise = (uint8_t)((x >> 16) & 0xffu);
            buf[i] = ((x & 3u) == 0u) ? structural : noise;
        }
        (void)gsp_frame_find(buf, len, &start, &end);
        memset(&m, 0, sizeof(m));
        (void)gsp_message_decode(buf, len, &m);
        memset(&r, 0, sizeof(r));
        (void)gsp_response_decode(buf, len, &r);
    }
    GS_ASSERT_MSG(1, "reaching here without a crash or a sanitizer report is the case");
}

/* CT-X02b — the same noise through a server, which adds the framer's state. */
GS_TEST(CT_X02b_random_bytes_at_the_server)
{
    gsp_server *s = gs_srv_open();
    uint8_t buf[256];
    gsp_event evs[32];
    gsp_write_request w[32];
    int iter;

    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    rng_state = 0x89abcdefu;
    for (iter = 0; iter < 2000; ++iter) {
        size_t len = (size_t)(rng_next() % sizeof(buf));
        size_t i;
        for (i = 0; i < len; ++i) {
            uint32_t x = rng_next();
            uint8_t structural = (uint8_t)"{}[]\":,\\ \n\t0123456789"[(x >> 8) % 21u];
            uint8_t noise = (uint8_t)((x >> 16) & 0xffu);
            buf[i] = ((x & 3u) == 0u) ? structural : noise;
        }
        (void)gs_srv_feed_n(s, buf, len);
        (void)gs_srv_events(s, evs, 32);
        (void)gs_srv_writes(s, w, 32);
    }
    GS_ASSERT_EQ(gsp_server_connection_count(s), 1);
    gs_srv_free(s);
}

/* CT-X03 / CT-X04 — the close-request threshold, which asks the HOST to close
 * because the library cannot. */
GS_TEST(CT_X03_close_requested_at_the_threshold)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_event evs[64];
    gsp_write_request w[64];
    size_t total_requests = 0;
    int i;

    GS_ASSERT_MSG(cfg.policy.protocol_error_close_threshold == 0,
                  "the default is never: a hostile client merely occupies its own socket");
    cfg.policy.protocol_error_close_threshold = 5;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    (void)gs_srv_writes(s, w, 64);

    for (i = 0; i < 12; ++i) {
        (void)gs_srv_feed(s, "garbage");
        total_requests += gs_count_of(evs, gs_srv_events(s, evs, 64),
                                      GSP_EV_CLOSE_REQUESTED);
        (void)gs_srv_writes(s, w, 64);
    }
    GS_ASSERT_MSG(total_requests == 1, "asked once, not once per error");
    GS_ASSERT_MSG(gsp_server_connection_count(s) == 1,
                  "asking is all the library can do");
    gs_srv_free(s);
}

GS_TEST(CT_X04_a_good_message_resets_the_run)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_event evs[64];
    gsp_write_request w[64];
    size_t requests = 0;
    int i;

    cfg.policy.protocol_error_close_threshold = 5;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    (void)gs_srv_writes(s, w, 64);

    for (i = 0; i < 4; ++i) {
        (void)gs_srv_feed(s, "garbage");
    }
    (void)gs_srv_feed(s, "{\"DeviceID\":\"ok\",\"ShotDataOptions\":"
                         "{\"ContainsBallData\":false,\"ContainsClubData\":false}}");
    for (i = 0; i < 4; ++i) {
        (void)gs_srv_feed(s, "garbage");
    }
    requests = gs_count_of(evs, gs_srv_events(s, evs, 64), GSP_EV_CLOSE_REQUESTED);
    GS_ASSERT_MSG(requests == 0, "a well-formed message resets the consecutive count");

    {
        gsp_connection_info info;
        memset(&info, 0, sizeof(info));
        GS_ASSERT_EQ(gsp_server_connection_info(s, GS_CONN_A, &info), GSP_OK);
        GS_ASSERT_EQ(info.protocol_errors, 8);
        GS_ASSERT_EQ(info.consecutive_errors, 4);
    }
    gs_srv_free(s);
}

/* CT-X05 — ⚠ THE ASYMMETRY THAT MATTERS.  The event ring is drop-oldest and
 * counted; the WRITE ring is not, because a dropped event costs a log line and
 * a dropped reply costs a duplicated shot ([MLM] re-sends after 2 s). */
GS_TEST(CT_X05_events_may_drop_but_acks_never_do)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_write_request w[64];
    gsp_event evs[64];
    size_t acks = 0, n;
    int i;
    gsp_status st = GSP_OK;

    cfg.event_ring = 4;
    cfg.write_ring = 32;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    (void)gs_srv_writes(s, w, 64);
    (void)gs_srv_events(s, evs, 64);

    for (i = 0; i < 10; ++i) {
        st = gs_srv_feed(s, "{\"DeviceID\":\"x\",\"ShotDataOptions\":"
                            "{\"ContainsBallData\":false,\"ContainsClubData\":false,"
                            "\"IsHeartBeat\":true}}");
        GS_ASSERT_MSG(st == GSP_OK, "the write ring is big enough here");
    }

    acks = gs_srv_writes(s, w, 64);
    GS_ASSERT_MSG(acks == 10, "every message is answered, whatever the event ring did");

    n = gs_srv_events(s, evs, 64);
    GS_ASSERT_MSG(n <= 4, "the event ring holds what it was sized for");
    GS_ASSERT_MSG(gsp_server_dropped_events(s) >= 6, "and counts what it dropped");

    /* ⚠ A drop is also visible in the sequence numbers, so a consumer that
     * ignores the counter still has evidence. */
    if (n >= 2) {
        GS_ASSERT_MSG(evs[n - 1].sequence > evs[0].sequence, "sequence is monotonic");
    }
    gs_srv_free(s);
}

/* CT-X06 — the sans-I/O gate is a ctest of its own (tests/CMakeLists.txt); this
 * asserts the API-level half: no call reads a clock, so identical inputs at
 * identical synthetic times give identical output. */
GS_TEST(CT_X06_behaviour_is_a_function_of_the_supplied_clock)
{
    gsp_write_request w1[8], w2[8];
    size_t n1, n2;
    gsp_server *a = gs_srv_create(NULL);
    gsp_server *b = gs_srv_create(NULL);
    const char *msg = "{\"DeviceID\":\"x\",\"ShotNumber\":1,\"APIversion\":\"1\","
                      "\"ShotDataOptions\":{\"ContainsBallData\":false,"
                      "\"ContainsClubData\":false,\"IsHeartBeat\":true}}";

    if (a == NULL || b == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        gs_srv_free(a);
        gs_srv_free(b);
        return;
    }
    (void)gsp_server_on_connection_opened(a, GS_CONN_A, NULL, 1000);
    (void)gsp_server_on_connection_opened(b, GS_CONN_A, NULL, 1000);
    (void)gsp_server_on_bytes(a, GS_CONN_A, (const uint8_t *)msg, strlen(msg), 2000);
    (void)gsp_server_on_bytes(b, GS_CONN_A, (const uint8_t *)msg, strlen(msg), 2000);

    n1 = gs_srv_writes(a, w1, 8);
    n2 = gs_srv_writes(b, w2, 8);
    GS_ASSERT_EQ(n1, n2);
    if (n1 == n2 && n1 > 0) {
        GS_ASSERT_EQ(w1[0].length, w2[0].length);
        GS_ASSERT_MEM(w1[0].data, w2[0].data, w1[0].length);
    }
    gs_srv_free(a);
    gs_srv_free(b);
}

/* CT-X07 — the ABI surface a binding checks at load. */
GS_TEST(CT_X07_abi_sizes_match_this_build)
{
    gsp_abi_sizes mine;
    gsp_abi_sizes lib;

    memset(&mine, 0, sizeof(mine));
    mine.abi_version = GSP_ABI_VERSION;
    mine.message = (uint32_t)sizeof(gsp_message);
    mine.ball_data = (uint32_t)sizeof(gsp_ball_data);
    mine.club_data = (uint32_t)sizeof(gsp_club_data);
    mine.shot_options = (uint32_t)sizeof(gsp_shot_options);
    mine.player_info = (uint32_t)sizeof(gsp_player_info);
    mine.event = (uint32_t)sizeof(gsp_event);
    mine.write_request = (uint32_t)sizeof(gsp_write_request);
    mine.wire_chunk = (uint32_t)sizeof(gsp_wire_chunk);
    mine.connection_info = (uint32_t)sizeof(gsp_connection_info);
    mine.server_config = (uint32_t)sizeof(gsp_server_config);
    mine.message_layout_version = GSP_MESSAGE_LAYOUT_VERSION;

    memset(&lib, 0, sizeof(lib));
    gsp_abi_sizes_get(&lib);
    GS_ASSERT_EQ(lib.abi_version, GSP_ABI_VERSION);
    GS_ASSERT_EQ(lib.message, mine.message);
    GS_ASSERT_EQ(lib.event, mine.event);
    GS_ASSERT_EQ(lib.write_request, mine.write_request);
    GS_ASSERT_EQ(lib.server_config, mine.server_config);

    GS_ASSERT_EQ(gsp_abi_check(&mine), GSP_OK);

    /* ⚠ And it must REFUSE a mismatch, which is the only thing it is for. */
    mine.message += 8u;
    GS_ASSERT_EQ(gsp_abi_check(&mine), GSP_ERR_NOT_SUPPORTED);
    GS_ASSERT_EQ(gsp_abi_check(NULL), GSP_ERR_INVALID_ARG);
}

/* CT-X08 — the vendor's own example carries `//` comments and is therefore not
 * JSON.  Somebody WILL paste it: it must be refused cleanly, not crash and not
 * half-decode. */
GS_TEST(CT_X08_the_vendors_commented_example_is_refused)
{
    size_t len = 0;
    unsigned char *fx = gs_fixture("gsp_full_commented.json", &len);
    gsp_message m;
    gsp_server *s;
    gsp_write_request w[8];
    size_t n;

    memset(&m, 0, sizeof(m));
    GS_ASSERT_MSG(gsp_message_decode(fx, len, &m) < GSP_OK,
                  "a comment is not JSON and must not decode");

    s = gs_srv_open();
    (void)gs_srv_writes(s, w, 8);
    (void)gs_srv_feed_n(s, fx, len);
    n = gs_srv_writes(s, w, 8);
    GS_ASSERT_MSG(n >= 1, "and the client is told, rather than left waiting");
    if (n >= 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_FAILURE);
    }
    gs_srv_free(s);
    free(fx);
}

/* NULL and zero-length everywhere a caller can reach.  Not a CT row: it is the
 * floor beneath all of them. */
GS_TEST(CT_X09_null_and_empty_arguments)
{
    gsp_message m;
    gsp_response r;
    size_t start = 0, end = 0;
    char buf[64];
    size_t written = 0;

    GS_ASSERT_EQ(gsp_frame_find(NULL, 0, &start, &end), GSP_ERR_INVALID_ARG);
    GS_ASSERT_EQ(gsp_message_decode(NULL, 0, &m), GSP_ERR_INVALID_ARG);
    GS_ASSERT_EQ(gsp_message_decode((const uint8_t *)"{}", 2, NULL), GSP_ERR_INVALID_ARG);
    GS_ASSERT_EQ(gsp_response_decode(NULL, 0, &r), GSP_ERR_INVALID_ARG);
    GS_ASSERT_EQ(gsp_response_encode(200, NULL, NULL, NULL, 0, &written),
                 GSP_ERR_INVALID_ARG);
    /* An out buffer that cannot hold the reply refuses rather than truncating:
     * half a JSON object on a socket is worse than none. */
    GS_ASSERT_EQ(gsp_response_encode(200, GSP_TEXT_SHOT_RECEIVED, NULL, buf, 4, &written),
                 GSP_ERR_BUFFER_TOO_SMALL);
    /* A code outside the five is refused: there is no send_raw(). */
    GS_ASSERT_EQ(gsp_response_encode(418, "teapot", NULL, buf, sizeof(buf), &written),
                 GSP_ERR_INVALID_ARG);

    GS_ASSERT_EQ(gsp_server_create(NULL, NULL), GSP_ERR_INVALID_ARG);
    gsp_server_close(NULL);
    gsp_server_destroy(NULL);
    GS_ASSERT_EQ(gsp_server_next_due_us(NULL), GSP_TIME_NEVER);
    gsp_server_tick(NULL, 0);
    GS_ASSERT_EQ(gsp_server_connection_count(NULL), 0);
    GS_ASSERT_EQ(gsp_server_poll_writes(NULL, NULL, 0), 0);
    GS_ASSERT_EQ(gsp_server_poll_events(NULL, NULL, 0), 0);
    GS_ASSERT_EQ(gsp_server_poll_wire(NULL, NULL, 0), 0);
}

GS_TEST_MAIN()
