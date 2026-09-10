/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_conn.c — CT-C01 … CT-C11, docs/conformance.md §3.6.
 *
 * ⚠ THE CONNECTION ID IS THE HOST'S (design §3.2).  Every case here passes an
 * id the "host" chose, because that is what removes a mapping table from the
 * host's side — and it is why a duplicate id has to be an error rather than a
 * silent reuse.
 */
#include "gspro/gspro.h"
#include "gs_test.h"
#include "gs_srv.h"

static const char *const HEARTBEAT =
    "{\"DeviceID\":\"connector-one\",\"ShotNumber\":0,\"APIversion\":\"1\","
    "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false,"
    "\"IsHeartBeat\":true}}";

static void shot(gsp_server *s, gsp_conn_id conn, int number, const char *device)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{\"DeviceID\":\"%s\",\"ShotNumber\":%d,\"APIversion\":\"1\","
             "\"BallData\":{\"Speed\":140.0,\"SpinAxis\":0.0,\"TotalSpin\":2500.0,"
             "\"HLA\":1.0,\"VLA\":12.0},"
             "\"ShotDataOptions\":{\"ContainsBallData\":true,\"ContainsClubData\":false}}",
             device, number);
    (void)gsp_server_on_bytes(s, conn, (const uint8_t *)json, strlen(json), gs_now);
}

/* CT-C01 — the same id twice while it is open. */
GS_TEST(CT_C01_duplicate_connection_id_refused)
{
    gsp_server *s = gs_srv_create(NULL);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now), GSP_OK);
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now),
                 GSP_ERR_INVALID_STATE);
    GS_ASSERT_EQ(gsp_server_connection_count(s), 1);

    /* Zero is never a valid id — the host must be able to spell "none". */
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, GSP_CONN_NONE, NULL, gs_now),
                 GSP_ERR_INVALID_ARG);

    /* Closed, then reopened: fine.  A host reuses a descriptor number. */
    GS_ASSERT_EQ(gsp_server_on_connection_closed(s, GS_CONN_A, GSP_CLOSE_REMOTE_CLOSED,
                                                 gs_now), GSP_OK);
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now), GSP_OK);
    gs_srv_free(s);
}

/* CT-C02 — bytes for an id nobody opened. */
GS_TEST(CT_C02_unknown_connection_refused)
{
    gsp_server *s = gs_srv_open();
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    GS_ASSERT_EQ(gsp_server_on_bytes(s, (gsp_conn_id)77, (const uint8_t *)"{}", 2, gs_now),
                 GSP_ERR_UNKNOWN_CONNECTION);
    GS_ASSERT_EQ(gsp_server_on_connection_closed(s, (gsp_conn_id)77,
                                                 GSP_CLOSE_REMOTE_CLOSED, gs_now),
                 GSP_ERR_UNKNOWN_CONNECTION);
    {
        gsp_connection_info info;
        GS_ASSERT_EQ(gsp_server_connection_info(s, (gsp_conn_id)77, &info),
                     GSP_ERR_UNKNOWN_CONNECTION);
    }
    gs_srv_free(s);
}

/* CT-C03 — the limit.  ⚠ Permissive by default (four): a putting device beside
 * a full-swing device is an ordinary setup, and a connector that reconnects
 * before its old socket finishes closing must not be refused. */
GS_TEST(CT_C03_connection_limit)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_event evs[16];
    size_t n;

    GS_ASSERT_EQ(cfg.max_connections, 0);   /* 0 means "the default", which is 4 */
    cfg.max_connections = 2;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, 1, NULL, gs_now), GSP_OK);
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, 2, NULL, gs_now), GSP_OK);
    (void)gs_srv_events(s, evs, 16);

    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, 3, NULL, gs_now),
                 GSP_ERR_TOO_MANY_CONNECTIONS);
    GS_ASSERT_EQ(gsp_server_connection_count(s), 2);

    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_MSG(gs_count_of(evs, n, GSP_EV_CONNECTION_OPENED) == 0,
                  "a refused connection produces no event: it was never open");

    {
        gsp_conn_id ids[4];
        GS_ASSERT_EQ(gsp_server_connection_ids(s, ids, 4), 2);
        GS_ASSERT_EQ(gsp_server_connection_ids(s, NULL, 0), 2);
    }
    gs_srv_free(s);
}

/* CT-C04 — a client is identified by its first message, once. */
GS_TEST(CT_C04_identified_once_then_a_change_is_a_warning)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[16];
    size_t n;

    (void)gs_srv_events(s, evs, 16);
    (void)gs_srv_feed(s, HEARTBEAT);

    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gs_count_of(evs, n, GSP_EV_CLIENT_IDENTIFIED), 1);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_CLIENT_IDENTIFIED);
        if (e != NULL) {
            GS_ASSERT_STR(e->u.connection.info.device_id, "connector-one");
            GS_ASSERT_EQ(e->u.connection.info.identified, 1);
        }
    }

    /* A second message from the same client identifies nothing new. */
    (void)gs_srv_feed(s, HEARTBEAT);
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gs_count_of(evs, n, GSP_EV_CLIENT_IDENTIFIED), 0);

    /* ⚠ A DIFFERENT DeviceID on the same connection is not something a
     * well-behaved client does; the first name stands and the log says so. */
    shot(s, GS_CONN_A, 1, "connector-two");
    n = gs_srv_events(s, evs, 16);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_WARNING);
        GS_ASSERT_MSG(e != NULL, "a changed DeviceID must be reported");
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.warning.code, GSP_WARN_DEVICE_ID_CHANGED);
        }
    }
    {
        gsp_connection_info info;
        memset(&info, 0, sizeof(info));
        GS_ASSERT_EQ(gsp_server_connection_info(s, GS_CONN_A, &info), GSP_OK);
        GS_ASSERT_STR(info.device_id, "connector-one");
    }
    gs_srv_free(s);
}

/* CT-C05 — the closing snapshot, so a UI pill needs no bookkeeping of its own. */
GS_TEST(CT_C05_close_carries_the_counters)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[32];
    size_t n;

    (void)gs_srv_events(s, evs, 32);
    shot(s, GS_CONN_A, 1, "connector-one");
    shot(s, GS_CONN_A, 2, "connector-one");
    shot(s, GS_CONN_A, 3, "connector-one");
    (void)gs_srv_feed(s, HEARTBEAT);
    (void)gs_srv_feed(s, HEARTBEAT);

    {
        gsp_connection_info info;
        memset(&info, 0, sizeof(info));
        GS_ASSERT_EQ(gsp_server_connection_info(s, GS_CONN_A, &info), GSP_OK);
        GS_ASSERT_EQ(info.messages, 5);
        GS_ASSERT_EQ(info.shots, 3);
        GS_ASSERT_EQ(info.protocol_errors, 0);
        GS_ASSERT_STR(info.peer, "203.0.113.7:51022");
    }

    (void)gs_srv_events(s, evs, 32);
    (void)gsp_server_on_connection_closed(s, GS_CONN_A, GSP_CLOSE_TRANSPORT_ERROR, gs_now);
    n = gs_srv_events(s, evs, 32);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_CONNECTION_CLOSED);
        GS_ASSERT(e != NULL);
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.connection.cause, GSP_CLOSE_TRANSPORT_ERROR);
            GS_ASSERT_EQ(e->u.connection.info.messages, 5);
            GS_ASSERT_EQ(e->u.connection.info.shots, 3);
        }
    }
    GS_ASSERT_EQ(gsp_server_connection_count(s), 0);
    gs_srv_free(s);
}

/* CT-C06 — a reconnecting client restarts its counter, and that is not a
 * duplicate: the counter is per connection (protocol §4.3). */
GS_TEST(CT_C06_shot_numbers_are_per_connection)
{
    gsp_server *s = gs_srv_create(NULL);
    gsp_event evs[32];
    size_t n;

    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    shot(s, GS_CONN_A, 1, "one");
    (void)gsp_server_on_connection_closed(s, GS_CONN_A, GSP_CLOSE_REMOTE_CLOSED, gs_now);
    (void)gs_srv_events(s, evs, 32);

    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, gs_now);
    shot(s, GS_CONN_A, 1, "one");
    n = gs_srv_events(s, evs, 32);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_SHOT);
        GS_ASSERT(e != NULL);
        if (e != NULL) {
            GS_ASSERT_MSG((e->u.message.flags & (uint32_t)GSP_MSGF_SHOT_NUMBER_REPEATED) == 0u,
                          "a fresh connection starts a fresh counter");
        }
    }
    gs_srv_free(s);
}

/* CT-C07 — the same number twice on ONE connection is what [MLM]'s re-send
 * after a slow reply looks like. */
GS_TEST(CT_C07_repeated_shot_number_is_flagged)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[32];
    size_t n;

    shot(s, GS_CONN_A, 7, "one");
    (void)gs_srv_events(s, evs, 32);
    shot(s, GS_CONN_A, 7, "one");
    n = gs_srv_events(s, evs, 32);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_SHOT);
        GS_ASSERT(e != NULL);
        if (e != NULL) {
            GS_ASSERT_MSG((e->u.message.flags & (uint32_t)GSP_MSGF_SHOT_NUMBER_REPEATED) != 0u,
                          "a repeat on one connection is worth reporting");
        }
    }
    gs_srv_free(s);
}

/* CT-C08 — ⚠ [FB] increments ShotNumber on every HEARTBEAT, so a counter that
 * only counts shots is not a safe assumption and a heartbeat must not poison
 * the repeat check. */
GS_TEST(CT_C08_heartbeat_numbers_do_not_poison_the_repeat_check)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[32];
    size_t n;
    char hb[512];
    int i;

    for (i = 1; i <= 3; ++i) {
        snprintf(hb, sizeof(hb),
                 "{\"DeviceID\":\"fb\",\"ShotNumber\":%d,\"APIversion\":\"1\","
                 "\"ShotDataOptions\":{\"ContainsBallData\":false,"
                 "\"ContainsClubData\":false,\"IsHeartBeat\":true}}", i);
        (void)gs_srv_feed(s, hb);
    }
    (void)gs_srv_events(s, evs, 32);

    shot(s, GS_CONN_A, 2, "fb");
    n = gs_srv_events(s, evs, 32);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_SHOT);
        GS_ASSERT(e != NULL);
        if (e != NULL) {
            GS_ASSERT_MSG((e->u.message.flags & (uint32_t)GSP_MSGF_SHOT_NUMBER_REPEATED) == 0u,
                          "only a previous SHOT's number can be repeated");
        }
    }
    {
        gsp_connection_info info;
        memset(&info, 0, sizeof(info));
        GS_ASSERT_EQ(gsp_server_connection_info(s, GS_CONN_A, &info), GSP_OK);
        GS_ASSERT_EQ(info.messages, 4);
        GS_ASSERT_EQ(info.shots, 1);
    }
    gs_srv_free(s);
}

/* CT-C09 — close seals everything, and a host that drains once more sees a
 * complete log (design §3.3). */
GS_TEST(CT_C09_close_seals_and_reports)
{
    gsp_server *s = gs_srv_create(NULL);
    gsp_event evs[32];
    size_t n;

    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, 1, NULL, gs_now);
    (void)gsp_server_on_connection_opened(s, 2, NULL, gs_now);
    (void)gs_srv_events(s, evs, 32);

    gsp_server_close(s);

    n = gs_srv_events(s, evs, 32);
    GS_ASSERT_MSG(gs_count_of(evs, n, GSP_EV_CONNECTION_CLOSED) == 2,
                  "every open connection is reported closed");
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_CONNECTION_CLOSED);
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.connection.cause, GSP_CLOSE_SERVER_CLOSED);
        }
    }

    /* Nothing can be produced afterwards. */
    GS_ASSERT_EQ(gsp_server_on_connection_opened(s, 3, NULL, gs_now), GSP_ERR_CLOSED);
    GS_ASSERT_EQ(gsp_server_on_bytes(s, 1, (const uint8_t *)"{}", 2, gs_now), GSP_ERR_CLOSED);
    GS_ASSERT_EQ(gsp_server_set_player(s, NULL, gs_now), GSP_ERR_CLOSED);
    gsp_server_destroy(s);
}

/* CT-C10 — the optional idle alarm, on a synthetic clock. */
GS_TEST(CT_C10_idle_alarm)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *s;
    gsp_event evs[16];
    size_t n;
    gsp_time_us t0 = gs_now;

    cfg.policy.idle_alarm_us = (gsp_time_us)10 * 1000 * 1000;
    s = gs_srv_create(&cfg);
    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    (void)gsp_server_on_connection_opened(s, GS_CONN_A, NULL, t0);
    (void)gs_srv_events(s, evs, 16);

    GS_ASSERT_MSG(gsp_server_next_due_us(s) != GSP_TIME_NEVER,
                  "an armed alarm gives the host a deadline to sleep on");

    gsp_server_tick(s, t0 + 9 * 1000 * 1000);
    GS_ASSERT_EQ(gs_count_of(evs, gs_srv_events(s, evs, 16), GSP_EV_CLIENT_IDLE), 0);

    gsp_server_tick(s, t0 + 11 * 1000 * 1000);
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gs_count_of(evs, n, GSP_EV_CLIENT_IDLE), 1);

    /* Once, not repeatedly. */
    gsp_server_tick(s, t0 + 30 * 1000 * 1000);
    GS_ASSERT_EQ(gs_count_of(evs, gs_srv_events(s, evs, 16), GSP_EV_CLIENT_IDLE), 0);

    /* And it comes back when the client speaks. */
    (void)gsp_server_on_bytes(s, GS_CONN_A, (const uint8_t *)HEARTBEAT, strlen(HEARTBEAT),
                              t0 + 31 * 1000 * 1000);
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gs_count_of(evs, n, GSP_EV_CLIENT_ACTIVE), 1);

    /* ⚠ AND IT NEVER CLOSES ANYTHING.  A client may be silent for a whole
     * warm-up (protocol §9.6); the alarm is for a UI, not a supervisor. */
    GS_ASSERT_EQ(gsp_server_connection_count(s), 1);
    gs_srv_free(s);
}

/* CT-C11 — with no alarm armed the protocol has no deadline at all, so the
 * host may sleep indefinitely and tick is a no-op. */
GS_TEST(CT_C11_no_alarm_means_no_deadline)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[16];

    if (s == NULL) {
        GS_ASSERT_MSG(0, "server not implemented");
        return;
    }
    GS_ASSERT_EQ(gsp_server_next_due_us(s), GSP_TIME_NEVER);
    (void)gs_srv_events(s, evs, 16);

    gsp_server_tick(s, gs_now + (gsp_time_us)3600 * 1000 * 1000);
    GS_ASSERT_EQ(gs_srv_events(s, evs, 16), 0);
    GS_ASSERT_EQ(gsp_server_next_due_us(s), GSP_TIME_NEVER);
    GS_ASSERT_EQ(gsp_server_connection_count(s), 1);
    gs_srv_free(s);
}

GS_TEST_MAIN()
