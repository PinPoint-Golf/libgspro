/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_player.c — CT-P01 … CT-P12, docs/conformance.md §3.5.
 *
 * The two things the server ORIGINATES: the 201 player message and the 202/203
 * session pair.  Both are unsolicited, and both are how a launch monitor learns
 * to switch between full-swing and putting mode — [MLM] and [OCR] switch on
 * Club == "PT", [OSP] applies a distance threshold, and ⚠ [OSP] does not arm
 * its device at all until it has seen a 202.
 */
#include "gspro/gspro.h"
#include "gs_test.h"
#include "gs_srv.h"

static gsp_player_info player(gsp_handed h, gsp_club c, int has_dist, double dist)
{
    gsp_player_info p;
    memset(&p, 0, sizeof(p));
    p.handed = (uint8_t)h;
    p.club = (uint8_t)c;
    p.has_distance = (uint8_t)(has_dist ? 1 : 0);
    p.distance_to_target = dist;
    return p;
}

/* Collect writes of one kind. */
static size_t writes_of(gsp_server *s, gsp_write_kind kind, gsp_write_request *out, size_t max)
{
    gsp_write_request all[32];
    size_t n = gs_srv_writes(s, all, 32);
    size_t i, c = 0;
    for (i = 0; i < n && c < max; ++i) {
        if (all[i].kind == (uint8_t)kind) {
            out[c++] = all[i];
        }
    }
    return c;
}

/* CT-P01 — one 201 per open connection, and an event per connection. */
GS_TEST(CT_P01_set_player_broadcasts)
{
    gsp_server *s = gs_srv_create(NULL);
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_DR, 0, 0.0);
    gsp_write_request w[16];
    gsp_event evs[16];
    size_t n;

    if (s != NULL) {
        (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
        (void)gsp_server_on_connection_opened(s, GS_CONN_B, NULL, gs_now);
    }
    (void)gs_srv_writes(s, w, 16);
    (void)gs_srv_events(s, evs, 16);

    GS_ASSERT_EQ(gsp_server_set_player(s, &p, gs_now), GSP_OK);
    n = writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16);
    GS_ASSERT_EQ(n, 2);
    if (n == 2) {
        GS_ASSERT(w[0].conn != w[1].conn);
        GS_ASSERT(w[0].conn == GS_CONN_A || w[0].conn == GS_CONN_B);
    }

    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gs_count_of(evs, n, GSP_EV_PLAYER_INFO_SENT), 2);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_PLAYER_INFO_SENT);
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.player_info.reason, GSP_PI_CHANGED);
        }
    }
    gs_srv_free(s);
}

/* CT-P02 — an unchanged value sends nothing, so a host may call this on every
 * UI change without spamming the client. */
GS_TEST(CT_P02_unchanged_player_sends_nothing)
{
    gsp_server *s = gs_srv_open();
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_I7, 1, 150.0);
    gsp_write_request w[16];

    (void)gs_srv_writes(s, w, 16);
    GS_ASSERT_EQ(gsp_server_set_player(s, &p, gs_now), GSP_OK);
    GS_ASSERT_EQ(writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16), 1);

    GS_ASSERT_EQ(gsp_server_set_player(s, &p, gs_now), GSP_OK);
    GS_ASSERT_MSG(writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16) == 0,
                  "an identical value is not a change");

    /* Any one field differing IS a change. */
    p.distance_to_target = 149.0;
    GS_ASSERT_EQ(gsp_server_set_player(s, &p, gs_now), GSP_OK);
    GS_ASSERT_EQ(writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16), 1);

    GS_ASSERT(gsp_player_info_equal(&p, &p));
    gs_srv_free(s);
}

/* CT-P03 — the putting case: Club "PT" with a real distance.  ⚠ [OSP] treats a
 * NON-ZERO DistanceToTarget as the signal to arm, so the value must actually
 * reach the wire. */
GS_TEST(CT_P03_putter_and_distance_reach_the_wire)
{
    gsp_server *s = gs_srv_open();
    gsp_player_info p = player(GSP_HANDED_LEFT, GSP_CLUB_PT, 1, 4.2);
    gsp_write_request w[16];
    size_t n;

    (void)gs_srv_writes(s, w, 16);
    (void)gsp_server_set_player(s, &p, gs_now);
    n = writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_PLAYER_INFO);
        GS_ASSERT_EQ(r.has_player, 1);
        GS_ASSERT_EQ(r.player.club, GSP_CLUB_PT);
        GS_ASSERT_EQ(r.player.handed, GSP_HANDED_LEFT);
        GS_ASSERT_EQ(r.player.has_distance, 1);
        GS_ASSERT_NEAR(r.player.distance_to_target, 4.2, 1e-9);
        GS_ASSERT_STR(r.message, GSP_TEXT_PLAYER_INFO);
    }
    gs_srv_free(s);
}

/* CT-P04 — nothing known: nothing sent.  An UNKNOWN member is omitted rather
 * than sent as a string GSPro never sends. */
GS_TEST(CT_P04_nothing_known_sends_nothing)
{
    gsp_server *s = gs_srv_open();
    gsp_player_info p = player(GSP_HANDED_UNKNOWN, GSP_CLUB_UNKNOWN, 0, 0.0);
    gsp_write_request w[16];

    (void)gs_srv_writes(s, w, 16);
    (void)gsp_server_set_player(s, &p, gs_now);
    GS_ASSERT_EQ(writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16), 0);

    /* Half-known: sent, with only the member that is known. */
    p.club = (uint8_t)GSP_CLUB_SW;
    (void)gsp_server_set_player(s, &p, gs_now);
    {
        size_t n = writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16);
        GS_ASSERT_EQ(n, 1);
        if (n == 1) {
            gsp_response r;
            memset(&r, 0, sizeof(r));
            GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
            GS_ASSERT_EQ(r.player.club, GSP_CLUB_SW);
            GS_ASSERT_MSG(r.player.handed == GSP_HANDED_UNKNOWN,
                          "an unknown hand is omitted, not invented");
        }
    }
    gs_srv_free(s);
}

/* CT-P05 — the Surface member [OSP] and [FB] both declare. */
GS_TEST(CT_P05_surface_is_carried_when_set)
{
    gsp_server *s = gs_srv_open();
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_PW, 0, 0.0);
    gsp_write_request w[16];
    size_t n;

    snprintf(p.surface, sizeof(p.surface), "%s", "Green");
    (void)gs_srv_writes(s, w, 16);
    (void)gsp_server_set_player(s, &p, gs_now);
    n = writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        GS_ASSERT_MSG(gs_memfind(w[0].data, w[0].length, "Green") != NULL,
                      "Surface should reach the wire when non-empty");
    }
    gs_srv_free(s);
}

/* CT-P06 / CT-P07 — announcing on connect is a policy, because whether GSPro
 * does it is protocol U1 and the evidence is weak both ways. */
GS_TEST(CT_P06_announce_on_connect_when_enabled)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_DR, 0, 0.0);
    gsp_write_request w[16];
    gsp_event evs[16];
    size_t n;

    GS_ASSERT_MSG(cfg.policy.announce_player_on_connect,
                  "the default is ON: a connector that needs the club before "
                  "the first strike is better served, and no client is harmed");
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_set_player(s, &p, gs_now);
    (void)gs_srv_writes(s, w, 16);
    (void)gs_srv_events(s, evs, 16);

    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    n = writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        GS_ASSERT_EQ(w[0].conn, GS_CONN_A);
    }
    n = gs_srv_events(s, evs, 16);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_PLAYER_INFO_SENT);
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.player_info.reason, GSP_PI_ON_CONNECT);
        }
    }
    gs_srv_free(s);
}

GS_TEST(CT_P07_no_announce_when_disabled)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_DR, 0, 0.0);
    gsp_write_request w[16];

    cfg.policy.announce_player_on_connect = false;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_set_player(s, &p, gs_now);
    (void)gs_srv_writes(s, w, 16);

    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    GS_ASSERT_EQ(writes_of(s, GSP_WRITE_PLAYER_INFO, w, 16), 0);
    gs_srv_free(s);
}

/* CT-P08 — ⚠ after a shot the client typically sees {200}{201} in one read.
 * The ORDER is the contract: the acknowledgement its request/response loop is
 * waiting on must come first. */
GS_TEST(CT_P08_ack_precedes_player_info)
{
    gsp_server *s = gs_srv_open();
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_I7, 1, 150.0);
    gsp_write_request w[16];
    size_t n;

    (void)gs_srv_writes(s, w, 16);
    (void)gs_srv_feed(s, "{\"DeviceID\":\"x\",\"ShotNumber\":1,"
                         "\"BallData\":{\"Speed\":140.0,\"SpinAxis\":0.0,"
                         "\"TotalSpin\":2500.0,\"HLA\":1.0,\"VLA\":12.0},"
                         "\"ShotDataOptions\":{\"ContainsBallData\":true,"
                         "\"ContainsClubData\":false}}");
    (void)gsp_server_set_player(s, &p, gs_now);

    n = gs_srv_writes(s, w, 16);
    GS_ASSERT_EQ(n, 2);
    if (n == 2) {
        GS_ASSERT_EQ(w[0].kind, GSP_WRITE_ACK);
        GS_ASSERT_EQ(w[1].kind, GSP_WRITE_PLAYER_INFO);
    }
    gs_srv_free(s);
}

/* CT-P09 / CT-P10 — the 202/203 pair, with the exact strings [OSP] matches. */
GS_TEST(CT_P09_session_active_sends_202)
{
    gsp_server *s = gs_srv_open();
    gsp_write_request w[16];
    gsp_event evs[16];
    size_t n;

    (void)gs_srv_writes(s, w, 16);
    (void)gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gsp_server_get_session_state(s), GSP_SESSION_NONE);

    GS_ASSERT_EQ(gsp_server_set_session_state(s, GSP_SESSION_ACTIVE, gs_now), GSP_OK);
    GS_ASSERT_EQ(gsp_server_get_session_state(s), GSP_SESSION_ACTIVE);
    n = writes_of(s, GSP_WRITE_READY, w, 16);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_READY);
        GS_ASSERT_STR(r.message, GSP_TEXT_READY);
    }
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT(gs_event_of(evs, n, GSP_EV_SESSION_STATE_SENT) != NULL);

    /* Repeating it sends nothing. */
    (void)gsp_server_set_session_state(s, GSP_SESSION_ACTIVE, gs_now);
    GS_ASSERT_EQ(writes_of(s, GSP_WRITE_READY, w, 16), 0);
    gs_srv_free(s);
}

GS_TEST(CT_P10_session_ended_sends_203_with_the_exact_string)
{
    gsp_server *s = gs_srv_open();
    gsp_write_request w[16];
    size_t n;

    (void)gs_srv_writes(s, w, 16);
    (void)gsp_server_set_session_state(s, GSP_SESSION_ACTIVE, gs_now);
    (void)gs_srv_writes(s, w, 16);

    (void)gsp_server_set_session_state(s, GSP_SESSION_ENDED, gs_now);
    n = writes_of(s, GSP_WRITE_ROUND_ENDED, w, 16);
    GS_ASSERT_EQ(n, 1);
    if (n == 1) {
        gsp_response r;
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gs_decode_write(&w[0], &r), GSP_OK);
        GS_ASSERT_EQ(r.code, GSP_CODE_ROUND_ENDED);
        /* ⚠ [OSP] compares this text EXACTLY before it clears its match state. */
        GS_ASSERT_STR(r.message, "GSPro round ended");
    }
    gs_srv_free(s);
}

/* CT-P11 — a connector arriving mid-session gets the 201 and then the 202,
 * because [OSP]-style clients do not arm until they have seen the 202. */
GS_TEST(CT_P11_connect_while_active_gets_201_then_202)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_player_info p = player(GSP_HANDED_RIGHT, GSP_CLUB_DR, 0, 0.0);
    gsp_write_request w[16];
    size_t n;

    GS_ASSERT_MSG(cfg.policy.announce_ready_on_connect, "default is ON");
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_set_player(s, &p, gs_now);
    (void)gsp_server_set_session_state(s, GSP_SESSION_ACTIVE, gs_now);
    (void)gs_srv_writes(s, w, 16);

    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    n = gs_srv_writes(s, w, 16);
    GS_ASSERT_EQ(n, 2);
    if (n == 2) {
        GS_ASSERT_EQ(w[0].kind, GSP_WRITE_PLAYER_INFO);
        GS_ASSERT_EQ(w[1].kind, GSP_WRITE_READY);
    }
    gs_srv_free(s);
}

/* CT-P12 — the 202 round-trips through the response decoder a client uses. */
GS_TEST(CT_P12_response_decoder_reads_all_five_codes)
{
    gsp_response r;
    static const struct { const char *json; int code; } cases[] = {
        { "{\"Code\":200,\"Message\":\"Shot received successfully\"}", 200 },
        { "{\"Code\":201,\"Message\":\"GSPro Player Information\","
          "\"Player\":{\"Handed\":\"RH\",\"Club\":\"DR\"}}", 201 },
        { "{\"Code\":202,\"Message\":\"GSPro ready\"}", 202 },
        { "{\"Code\":203,\"Message\":\"GSPro round ended\"}", 203 },
        { "{\"Code\":501,\"Message\":\"Failure occurred\"}", 501 }
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gsp_response_decode(GS_BYTES(cases[i].json), &r), GSP_OK);
        GS_ASSERT_EQ(r.code, cases[i].code);
        GS_ASSERT_EQ(r.has_message, 1);
    }

    /* The 201 carries a player; the rest do not. */
    memset(&r, 0, sizeof(r));
    GS_ASSERT_EQ(gsp_response_decode(GS_BYTES(cases[1].json), &r), GSP_OK);
    GS_ASSERT_EQ(r.has_player, 1);
    GS_ASSERT_EQ(r.player.handed, GSP_HANDED_RIGHT);
    GS_ASSERT_EQ(r.player.club, GSP_CLUB_DR);

    /* ⚠ [OSG] replies with no Message at all and clients accept it. */
    memset(&r, 0, sizeof(r));
    GS_ASSERT_EQ(gsp_response_decode(GS_BYTES("{\"Code\":200}"), &r), GSP_OK);
    GS_ASSERT_EQ(r.code, 200);
    GS_ASSERT_EQ(r.has_message, 0);

    /* And DistanceToTarget, which no vendor page documents. */
    memset(&r, 0, sizeof(r));
    GS_ASSERT_EQ(gsp_response_decode(
        GS_BYTES("{\"Code\":201,\"Player\":{\"Handed\":\"LH\",\"Club\":\"PT\","
                 "\"DistanceToTarget\":4.5,\"Surface\":\"Green\"}}"), &r), GSP_OK);
    GS_ASSERT_EQ(r.player.club, GSP_CLUB_PT);
    GS_ASSERT_EQ(r.player.has_distance, 1);
    GS_ASSERT_NEAR(r.player.distance_to_target, 4.5, 1e-9);
    GS_ASSERT_STR(r.player.surface, "Green");
}

GS_TEST_MAIN()
