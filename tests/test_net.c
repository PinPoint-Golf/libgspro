/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_net.c — CT-T01 … CT-T10 (docs/conformance.md §3.8) against the C
 * REFERENCE TRANSPORT, `gspro_net`.
 *
 * ============================================================================
 * ⚠ THIS IS THE SAME CHECKLIST AS tests/test_python_transport.py, DELIBERATELY.
 * ============================================================================
 * The CT-T rows are the only family in the suite that is about a HOST rather
 * than about the library: reply latency, one write per message, holding a 200
 * apart from a 201, a client that vanishes mid-message.  There is therefore one
 * run of the checklist PER HOST ADAPTER, and a green run means "this adapter is
 * correct here" — never "the transport contract holds".  Three adapters exist
 * (design §3.1):
 *
 *   python/gspro/asyncio_transport.py   tests/test_python_transport.py  ✅
 *   net/gs_net.c (this one)             THIS FILE                       ✅
 *   PinPoint Studio's QTcpServer        lives in PinPoint (package 8)
 *
 * ⚠ AND THE TWO RUNS ARE NOT REDUNDANT.  They share no code below the socket:
 * asyncio's proactor/selector loop and this select() loop are different
 * platform code making the same promises, and the CT-T rows are exactly the
 * promises a host can break on its own — Nagle left on, two writes
 * concatenated, bytes split on newlines before on_bytes, a timer never re-armed
 * after a poll.  tests/test_coverage.py counts a T case once per adapter for
 * that reason and names them.
 *
 * ⚠ WHAT NEITHER RUN CAN SEE: a LAN.  Loopback does not reorder, rarely drops,
 * and its segmentation is not a network's.  CT-T06 — the row that tells a
 * listener bound to 0.0.0.0 from one bound to loopback — needs a second machine
 * and is not automated and not pretended; the runner prints how to run it.
 */
#include "gs_net_client.h"      /* pulls in gs_net_sys.h, gspro/net.h, gs_test.h */
#include "gs_srv.h"            /* the server-side boilerplate every case shares */

#include <stdlib.h>

/* ------------------------------------------------------------------------ */
/* The harness: a server, a listener on an ephemeral loopback port, and every  */
/* event the pair produced.                                                   */
/* ------------------------------------------------------------------------ */
#define GS_FIX_EVENTS_MAX 128

typedef struct gs_net_fix {
    gsp_server *server;
    gsp_net    *net;
    gsp_event   events[GS_FIX_EVENTS_MAX];
    size_t      event_count;
    char        open_error[GSP_NET_ERROR_MAX];
} gs_net_fix;

static void gs_fix_drain_events(gs_net_fix *f)
{
    gsp_event ev[16];
    size_t n;
    while ((n = gsp_server_poll_events(f->server, ev, 16)) > 0u) {
        size_t i;
        for (i = 0; i < n; ++i) {
            if (f->event_count < GS_FIX_EVENTS_MAX) {
                f->events[f->event_count++] = ev[i];
            }
        }
    }
}

/*
 * ⚠ 127.0.0.1 AND AN EPHEMERAL PORT.  The default bind is 0.0.0.0 (design §6.1)
 * and that is right for a listener a launch monitor must reach; a test that did
 * it would open a port to the LAN on every developer machine and every CI
 * runner, and would collide with whatever else holds 921 there.
 */
static bool gs_fix_open(gs_net_fix *f, const gsp_server_config *server_cfg)
{
    gsp_net_config cfg = gsp_net_config_default();
    memset(f, 0, sizeof(*f));
    f->server = gs_srv_create(server_cfg);
    if (f->server == NULL) {
        return false;
    }
    cfg.host = "127.0.0.1";
    cfg.ephemeral_port = true;
    if (gsp_net_open(f->server, &cfg, &f->net, f->open_error, sizeof(f->open_error))
        < GSP_OK) {
        gs_srv_free(f->server);
        f->server = NULL;
        return false;
    }
    return true;
}

static void gs_fix_close(gs_net_fix *f)
{
    if (f->net != NULL) {
        gsp_net_close(f->net);
        f->net = NULL;
    }
    if (f->server != NULL) {
        gs_fix_drain_events(f);     /* ⚠ once more after the close: design §3.3 */
        gs_srv_free(f->server);
        f->server = NULL;
    }
}

/*
 * ADVANCE BOTH ENDS.  One pass of the listener, then one read attempt per
 * client, until `want` replies have arrived across all of them or the budget is
 * spent.  `want` == 0 runs the whole budget, which is how a case asserts that
 * something did NOT happen.
 *
 * ⚠ The listener's own poll timeout is small rather than the whole budget: the
 * client sockets are read by this same thread, so a poll that slept until its
 * own timeout would be the thread that was supposed to be reading the reply it
 * is waiting for.
 */
static size_t gs_run(gs_net_fix *f, gs_client **clients, size_t client_count, size_t want,
                     gsp_time_us budget_us)
{
    gsp_time_us deadline = gsp_net_now_us() + budget_us;
    for (;;) {
        size_t got = 0;
        size_t i;
        (void)gsp_net_poll(f->net, 2);
        gs_fix_drain_events(f);
        for (i = 0; i < client_count; ++i) {
            (void)gs_client_pump(clients[i]);
            got += clients[i]->resp_count;
        }
        if (want != 0u && got >= want) {
            return got;
        }
        if (gsp_net_now_us() >= deadline) {
            return got;
        }
    }
}

static size_t gs_count_events(const gs_net_fix *f, gsp_event_type type)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < f->event_count; ++i) {
        if (f->events[i].type == (uint8_t)type) {
            n++;
        }
    }
    return n;
}

static const gsp_event *gs_first_event(const gs_net_fix *f, gsp_event_type type)
{
    size_t i;
    for (i = 0; i < f->event_count; ++i) {
        if (f->events[i].type == (uint8_t)type) {
            return &f->events[i];
        }
    }
    return NULL;
}

/* A shot as a real client would send it, straight from the byte-exact fixture. */
static unsigned char *gs_shot(const char *fixture, size_t *len)
{
    return gs_fixture(fixture, len);
}

#define GS_MS(x) ((gsp_time_us)(x) * 1000)

/* ------------------------------------------------------------------------ */
/* CT-T01 — a reply, promptly.  [MLM] blocks for 2 s and then RE-SENDS the    */
/* shot, so a late reply is a DUPLICATED SHOT rather than an error.  500 ms   */
/* is that budget with a wide margin.                                        */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T01_one_ack_within_500ms)
{
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("gsp_full.json", &len);
    gsp_time_us t0;
    gsp_time_us elapsed;

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));       /* let the accept happen */

    t0 = gsp_net_now_us();
    gs_client_send(&c, shot, len);
    (void)gs_run(&f, &cl, 1u, 1u, GS_MS(2000));
    elapsed = gsp_net_now_us() - t0;

    GS_ASSERT_EQ(c.resp_count, 1u);
    if (c.resp_count > 0u) {
        GS_ASSERT_EQ(c.resp[0].code, 200);
    }
    GS_ASSERT_MSG(elapsed < GS_MS(500), "a 200 within 500 ms of the shot");
    printf("    note: %.2f ms round trip on loopback\n", (double)elapsed / 1000.0);

    /* And exactly one: §4.4 is one reply per message, not at least one. */
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(100));
    GS_ASSERT_EQ(c.resp_count, 1u);
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_SHOT), 1u);

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* CT-T02 — two shots back to back: two 200s, in order.                      */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T02_two_shots_two_acks_in_order)
{
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("gsp_full.json", &len);
    gsp_net_stats st;

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));

    gs_client_send(&c, shot, len);
    gs_client_send(&c, shot, len);
    (void)gs_run(&f, &cl, 1u, 2u, GS_MS(2000));

    GS_ASSERT_EQ(c.resp_count, 2u);
    if (c.resp_count == 2u) {
        GS_ASSERT_EQ(c.resp[0].code, 200);
        GS_ASSERT_EQ(c.resp[1].code, 200);
    }
    /* ⚠ REPORTED, NOT ASSERTED (conformance §3.8).  Nothing can make TCP
     * coalescing impossible (design §5.5) and a case that demanded two segments
     * would be testing the kernel's scheduler.  What CAN be asserted is on the
     * SENDING side, and it is: two writes, below and in CT-T10. */
    st = gsp_net_get_stats(f.net);
    printf("    note: %u read(s) for 2 replies — separate segments: %s; "
           "%llu send() call(s) on this side\n",
           c.reads, (c.reads >= 2u) ? "yes" : "no", (unsigned long long)st.sends);
    GS_ASSERT_EQ(st.writes, 2u);
    GS_ASSERT_EQ(st.sends, 2u);

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* CT-T03 — ⚠ THE ONE THAT KILLS A CLIENT.  [PIT]'s receive thread DIES when  */
/* a 200 and a 201 share a segment (protocol §9.8).  policy.write_spacing_us  */
/* holds the second write back; without it they are two writes that may still */
/* coalesce.                                                                 */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T03_ack_and_player_info_are_spaced)
{
    const gsp_time_us spacing = GS_MS(20);
    gsp_server_config cfg = gsp_server_config_default();
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("gsp_full.json", &len);
    gsp_player_info player;

    cfg.policy.write_spacing_us = spacing;
    GS_ASSERT(gs_fix_open(&f, &cfg));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));

    gs_client_send(&c, shot, len);
    /* The club changes 10 ms after the shot, as a game would. */
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(10));
    memset(&player, 0, sizeof(player));
    player.handed = (uint8_t)GSP_HANDED_RIGHT;
    player.club = (uint8_t)GSP_CLUB_PT;
    player.has_distance = 1u;
    player.distance_to_target = 4.2;
    GS_ASSERT_EQ(gsp_server_set_player(f.server, &player, gsp_net_now_us()), GSP_OK);

    (void)gs_run(&f, &cl, 1u, 2u, GS_MS(2000));

    GS_ASSERT_EQ(c.resp_count, 2u);
    if (c.resp_count == 2u) {
        gsp_time_us gap = c.resp_us[1] - c.resp_us[0];
        GS_ASSERT_EQ(c.resp[0].code, 200);      /* the acknowledgement first    */
        GS_ASSERT_EQ(c.resp[1].code, 201);      /* the player information second */
        /* ⚠ Half the spacing, as the Python run allows: the 201 is queued about
         * 10 ms after the 200 was polled, so what spacing adds is the remainder
         * — and a loaded CI runner's scheduler owns the rest. */
        GS_ASSERT_MSG(gap >= spacing / 2, "the two writes were held apart");
        printf("    note: gap %.1f ms with spacing %.0f ms, %u read(s)\n",
               (double)gap / 1000.0, (double)spacing / 1000.0, c.reads);
    }
    /* ⚠ AND THE SPACING WAS REAL WORK: with it set, next_due_us() is armed —
     * which is the mechanism, not a side effect (design §5.5). */
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_PLAYER_INFO_SENT), 1u);

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* CT-T04 — one byte at a time.  CT-F02 end to end: every prefix must be      */
/* PENDING and only the last byte may complete the message.                   */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T04_one_byte_at_a_time)
{
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("tl_minimal.json", &len);
    size_t i;

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));

    for (i = 0; i < len; ++i) {
        gs_client_send(&c, shot + i, 1u);
        /* ⚠ The row says 10 ms a byte; 216 bytes of that is two seconds of wall
         * clock, which is [MLM]'s whole patience.  One pass per byte keeps the
         * shape — every prefix is delivered and seen alone — without the wait,
         * because the property under test is per-byte delivery, not the delay. */
        (void)gs_run(&f, &cl, 1u, 0u, GS_MS(1));
        if (i + 1u < len) {
            GS_ASSERT_MSG(c.resp_count == 0u, "no reply until the last byte");
        }
    }
    (void)gs_run(&f, &cl, 1u, 1u, GS_MS(2000));

    GS_ASSERT_EQ(c.resp_count, 1u);
    if (c.resp_count > 0u) {
        GS_ASSERT_EQ(c.resp[0].code, 200);
    }
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_SHOT), 1u);
    printf("    note: %zu single-byte writes, %u read(s) back\n", len, c.reads);

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* CT-T05 — ⚠ SILENCE IS NOT A FAULT.  The protocol has no deadline (§9.6)    */
/* and a client may connect and say nothing for a whole warm-up.  The library */
/* must never close for it, and neither must the host.                        */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T05_silence_does_not_close)
{
    const char *soak = getenv("GSP_SOAK");
    gsp_time_us window = (soak != NULL && soak[0] == '1') ? GS_MS(300000) : GS_MS(5000);
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("gsp_full.json", &len);

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(200));

    /* The STRUCTURAL reason it is safe, checked as well as observed: with no
     * idle alarm armed the server schedules nothing at all, so there is no timer
     * that could fire and no code path that could close anything. */
    GS_ASSERT(gsp_server_next_due_us(f.server) == GSP_TIME_NEVER);
    GS_ASSERT_EQ(gsp_server_connection_count(f.server), 1u);
    GS_ASSERT_EQ(gsp_net_connection_count(f.net), 1u);

    (void)gs_run(&f, &cl, 1u, 0u, window);

    GS_ASSERT_EQ(gsp_server_connection_count(f.server), 1u);
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_CONNECTION_CLOSED), 0u);
    GS_ASSERT(!c.eof);

    /* And it is still a working connection, not merely an open socket. */
    gs_client_send(&c, shot, len);
    (void)gs_run(&f, &cl, 1u, 1u, GS_MS(2000));
    GS_ASSERT_EQ(c.resp_count, 1u);
    if (c.resp_count > 0u) {
        GS_ASSERT_EQ(c.resp[0].code, 200);
    }
    printf("    note: observed %.0f s of silence%s\n", (double)window / 1000000.0,
           (soak != NULL && soak[0] == '1')
               ? "" : " — set GSP_SOAK=1 for the full 5 minutes the row names");

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* CT-T06 — a second machine on the LAN.  ⚠ NOT AUTOMATED AND NOT PRETENDED;  */
/* the note at the end of the run says how to do it by hand.                  */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* CT-T07 — two clients at once, each acknowledged independently.  An         */
/* ordinary setup: a putting camera beside a full-swing device (design §5.1). */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T07_two_simultaneous_clients)
{
    gs_net_fix f;
    gs_client a;
    gs_client b;
    gs_client *cl[2];
    size_t len_a = 0;
    size_t len_b = 0;
    unsigned char *shot_a = gs_shot("gsp_full.json", &len_a);
    unsigned char *shot_b = gs_shot("tl_minimal.json", &len_b);

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot_a);
        free(shot_b);
        return;
    }
    cl[0] = &a;
    cl[1] = &b;
    gs_client_open(&a, gsp_net_port(f.net));
    gs_client_open(&b, gsp_net_port(f.net));
    (void)gs_run(&f, cl, 2u, 0u, GS_MS(50));

    gs_client_send(&a, shot_a, len_a);
    gs_client_send(&b, shot_b, len_b);
    (void)gs_run(&f, cl, 2u, 2u, GS_MS(2000));

    GS_ASSERT_EQ(a.resp_count, 1u);
    GS_ASSERT_EQ(b.resp_count, 1u);
    if (a.resp_count > 0u && b.resp_count > 0u) {
        GS_ASSERT_EQ(a.resp[0].code, 200);
        GS_ASSERT_EQ(b.resp[0].code, 200);
    }
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_SHOT), 2u);
    /* ⚠ On two DIFFERENT ids: one shared id would acknowledge one device's shot
     * on the other's socket, which is the failure this row exists to catch. */
    {
        size_t i;
        gsp_conn_id first = GSP_CONN_NONE;
        bool differ = false;
        for (i = 0; i < f.event_count; ++i) {
            if (f.events[i].type != (uint8_t)GSP_EV_SHOT) {
                continue;
            }
            if (first == GSP_CONN_NONE) {
                first = f.events[i].conn;
            } else if (f.events[i].conn != first) {
                differ = true;
            }
        }
        GS_ASSERT(differ);
    }
    GS_ASSERT_EQ(gsp_net_connection_count(f.net), 2u);

    gs_client_close(&a);
    gs_client_close(&b);
    gs_fix_close(&f);
    free(shot_a);
    free(shot_b);
}

/* ------------------------------------------------------------------------ */
/* CT-T08 — a client that vanishes mid-message.  The partial object must      */
/* produce no message event, and the close must still be reported with its    */
/* counters.                                                                  */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T08_disconnect_mid_message)
{
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("gsp_full.json", &len);
    const gsp_event *closed;

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));

    gs_client_send(&c, shot, len / 2u);     /* half an object, then gone */
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));
    gs_client_close(&c);
    (void)gs_run(&f, &cl, 0u, 0u, GS_MS(200));

    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_SHOT), 0u);
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_HEARTBEAT), 0u);
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_STATUS), 0u);
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_CONNECTION_CLOSED), 1u);
    closed = gs_first_event(&f, GSP_EV_CONNECTION_CLOSED);
    if (closed != NULL) {
        GS_ASSERT_EQ(closed->u.connection.info.messages, 0u);
        GS_ASSERT_EQ(closed->u.connection.cause, (uint8_t)GSP_CLOSE_REMOTE_CLOSED);
    }
    /* ⚠ And the slot is free again on BOTH sides.  A socket closed without
     * telling the server would leave the id occupying the server's table for the
     * life of the process — a leak that reads as a protocol fault later. */
    GS_ASSERT_EQ(gsp_server_connection_count(f.server), 0u);
    GS_ASSERT_EQ(gsp_net_connection_count(f.net), 0u);

    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* CT-T09 — the port is already held.  ⚠ On the machine this library is most  */
/* likely to run on, port 921 is held by GSPro itself (design §6.4).  The host */
/* must REPORT it, not appear to be listening.                                */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T09_bind_conflict_is_reported)
{
    gs_net_fix first;
    gsp_server *second_server;
    gsp_net *second = NULL;
    gsp_net_config cfg = gsp_net_config_default();
    char error[GSP_NET_ERROR_MAX];
    gsp_status st;
    gs_client c;
    gs_client *cl = &c;

    GS_ASSERT(gs_fix_open(&first, NULL));
    if (first.net == NULL) {
        return;
    }
    second_server = gs_srv_create(NULL);
    GS_ASSERT(second_server != NULL);
    if (second_server == NULL) {
        gs_fix_close(&first);
        return;
    }

    cfg.host = "127.0.0.1";
    cfg.port = gsp_net_port(first.net);
    error[0] = '\0';
    st = gsp_net_open(second_server, &cfg, &second, error, sizeof(error));

    GS_ASSERT_MSG(st < GSP_OK, "binding a held port fails rather than succeeding");
    GS_ASSERT_MSG(second == NULL, "and hands back no listener to mistake for one");
    GS_ASSERT_MSG(error[0] != '\0', "with the platform's own reason in the report");
    printf("    note: %s\n", error);
    if (second != NULL) {
        gsp_net_close(second);
    }
    gs_srv_free(second_server);

    /* ⚠ And the listener that OWNS the port is unharmed — a failed bind that
     * quietly took the port, which is what Winsock's SO_REUSEADDR would do, is
     * the failure mode this row is really about. */
    gs_client_open(&c, gsp_net_port(first.net));
    (void)gs_run(&first, &cl, 1u, 0u, GS_MS(100));
    GS_ASSERT_EQ(gsp_server_connection_count(first.server), 1u);
    gs_client_close(&c);
    gs_fix_close(&first);
}

/* ------------------------------------------------------------------------ */
/* CT-T10 — ⚠ EVERY WRITE FITS GSP_WRITE_MAX, and each is ONE send() call.    */
/* The bound is what lets a host put a write request on the stack.            */
/* ------------------------------------------------------------------------ */
GS_TEST(CT_T10_write_size_and_count)
{
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("gsp_full.json", &len);
    gsp_player_info player;
    gsp_net_stats st;

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        free(shot);
        return;
    }

    /* ⚠ TWO REQUESTS QUEUED BEFORE ANYTHING IS POLLED, which is the case a
     * concatenating host gets wrong.  A player and an active session mean a new
     * connection is owed a 201 AND a 202 (policy.announce_*_on_connect), both
     * queued by the same on_connection_opened() call — so the ring holds two
     * for one connection at once and the transport has to issue two sends. */
    memset(&player, 0, sizeof(player));
    player.handed = (uint8_t)GSP_HANDED_LEFT;
    player.club = (uint8_t)GSP_CLUB_PT;
    player.has_distance = 1u;
    player.distance_to_target = 12.5;
    (void)snprintf(player.surface, sizeof(player.surface), "%s", "Green");
    GS_ASSERT_EQ(gsp_server_set_player(f.server, &player, gsp_net_now_us()), GSP_OK);
    GS_ASSERT_EQ(gsp_server_set_session_state(f.server, GSP_SESSION_ACTIVE,
                                              gsp_net_now_us()), GSP_OK);

    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 2u, GS_MS(2000));

    GS_ASSERT_EQ(c.resp_count, 2u);
    if (c.resp_count == 2u) {
        GS_ASSERT_EQ(c.resp[0].code, 201);   /* player information first        */
        GS_ASSERT_EQ(c.resp[1].code, 202);   /* then GSPro ready ([OSP] arms on it) */
    }
    st = gsp_net_get_stats(f.net);
    GS_ASSERT_EQ(st.writes, 2u);
    GS_ASSERT_EQ(st.sends, 2u);

    /* Then a shot, then a club change: a 200 and a 201, which is the pair [PIT]
     * cannot survive in one segment. */
    gs_client_send(&c, shot, len);
    (void)gs_run(&f, &cl, 1u, 3u, GS_MS(2000));
    player.distance_to_target = 3.0;
    GS_ASSERT_EQ(gsp_server_set_player(f.server, &player, gsp_net_now_us()), GSP_OK);
    (void)gs_run(&f, &cl, 1u, 4u, GS_MS(2000));

    st = gsp_net_get_stats(f.net);
    /* ⚠ COUNTED HERE, BECAUSE IT CANNOT BE SEEN FROM THE CLIENT.  TCP may merge
     * two writes into one segment and split one into two, so a client counting
     * reads is measuring the kernel; the transport counts its own send()s, as
     * the Python run does by wrapping the asyncio writer. */
    GS_ASSERT_EQ(st.writes, 4u);
    /* ⚠ ONE SYSCALL PER REQUEST — this is the assertion that fails if a host
     * ever "optimises" the pump by concatenating what the ring handed it, which
     * is the exact byte pattern [PIT]'s receive thread dies on. */
    GS_ASSERT_EQ(st.sends, 4u);
    GS_ASSERT_EQ(st.partial_writes, 0u);
    GS_ASSERT_MSG(st.max_write_bytes <= GSP_WRITE_MAX, "every write fits GSP_WRITE_MAX");
    GS_ASSERT_EQ(c.resp_count, 4u);
    if (c.resp_count == 4u) {
        GS_ASSERT_EQ(c.resp[2].code, 200);
        GS_ASSERT_EQ(c.resp[3].code, 201);
    }
    printf("    note: %llu request(s) in %llu send() call(s), largest %u of %d bytes\n",
           (unsigned long long)st.writes, (unsigned long long)st.sends,
           st.max_write_bytes, GSP_WRITE_MAX);

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

/* ------------------------------------------------------------------------ */
/* The transport's own contract, beyond the CT-T rows                        */
/*                                                                           */
/* ⚠ NOT NAMED FOR A CT ID, ON PURPOSE.  The conformance document numbers what */
/* the PROTOCOL and its clients demand; these are properties of this optional  */
/* target — a reference host — and inventing CT rows for them would put a      */
/* claim about libgspro's own tooling in the middle of a table about launch    */
/* monitors.  They still have to hold, so they are still tested.               */
/* ------------------------------------------------------------------------ */
GS_TEST(net_refuses_beyond_its_table)
{
    gsp_server_config scfg = gsp_server_config_default();
    gsp_net_config cfg = gsp_net_config_default();
    gsp_server *server;
    gsp_net *net = NULL;
    gs_client a;
    gs_client b;
    gsp_net_stats st;
    char error[GSP_NET_ERROR_MAX];
    gsp_time_us deadline;

    scfg.max_connections = 4u;          /* the server is not the binding limit here */
    server = gs_srv_create(&scfg);
    GS_ASSERT(server != NULL);
    if (server == NULL) {
        return;
    }
    cfg.host = "127.0.0.1";
    cfg.ephemeral_port = true;
    cfg.max_connections = 1u;           /* ⚠ one socket, and one only              */
    GS_ASSERT_EQ(gsp_net_open(server, &cfg, &net, error, sizeof(error)), GSP_OK);
    if (net == NULL) {
        gs_srv_free(server);
        return;
    }

    /* ⚠ Pumped directly rather than through gs_run(): this case wants BOTH
     * clients advanced whatever the listener does with them, and one of them is
     * about to be refused. */
    gs_client_open(&a, gsp_net_port(net));
    gs_client_open(&b, gsp_net_port(net));
    deadline = gsp_net_now_us() + GS_MS(300);
    while (gsp_net_now_us() < deadline) {
        (void)gsp_net_poll(net, 2);
        (void)gs_client_pump(&a);
        (void)gs_client_pump(&b);
    }

    GS_ASSERT_EQ(gsp_net_connection_count(net), 1u);
    GS_ASSERT_EQ(gsp_server_connection_count(server), 1u);
    st = gsp_net_get_stats(net);
    GS_ASSERT_EQ(st.accepted, 1u);
    GS_ASSERT_EQ(st.refused, 1u);
    /* ⚠ The refused client is told NOTHING, because the protocol has no message
     * for it (design §5.1): the socket simply closes under it. */
    GS_ASSERT_MSG(b.eof || a.eof, "the refused socket was closed, not left hanging");

    gs_client_close(&a);
    gs_client_close(&b);
    gsp_net_close(net);
    gs_srv_free(server);
}

GS_TEST(net_write_ring_backpressure_never_drops_a_reply)
{
    /* ⚠ THE WRITE RING IS NOT DROP-OLDEST, AND THIS IS WHY IT MATTERS: a reply
     * that goes missing makes [MLM] re-send the shot two seconds later, so the
     * same strike arrives twice (design §3.4).  When the ring is full,
     * on_bytes() returns GSP_ERR_QUEUE_FULL WITHOUT consuming the message and
     * the host must drain and offer zero bytes again.
     *
     * ⚠ AND IT MUST COME BACK ON ITS OWN.  A host that only resumes when MORE
     * BYTES ARRIVE deadlocks against a client waiting for its acknowledgement —
     * which is exactly what this transport did until the retry after the pump
     * was added.  A ring of one and a burst of twenty forces the path many times
     * over in one connection. */
    const size_t burst = 20u;
    gsp_server_config cfg = gsp_server_config_default();
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    size_t len = 0;
    unsigned char *shot = gs_shot("tl_minimal.json", &len);
    size_t i;

    cfg.write_ring = 1u;
    GS_ASSERT(gs_fix_open(&f, &cfg));
    if (f.net == NULL) {
        free(shot);
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(50));

    for (i = 0; i < burst; ++i) {
        gs_client_send(&c, shot, len);
    }
    (void)gs_run(&f, &cl, 1u, burst, GS_MS(4000));

    GS_ASSERT_EQ(c.resp_count, burst);
    GS_ASSERT_EQ(c.dropped, 0u);
    for (i = 0; i < c.resp_count; ++i) {
        GS_ASSERT_EQ(c.resp[i].code, 200);
    }
    GS_ASSERT_EQ(gs_count_events(&f, GSP_EV_SHOT), burst);
    printf("    note: %zu shot(s) through a one-slot write ring, %zu reply(s) back\n",
           burst, c.resp_count);

    gs_client_close(&c);
    gs_fix_close(&f);
    free(shot);
}

GS_TEST(net_close_connection_reports_the_close)
{
    gs_net_fix f;
    gs_client c;
    gs_client *cl = &c;
    gsp_conn_id ids[4];
    size_t n;
    const gsp_event *closed;

    GS_ASSERT(gs_fix_open(&f, NULL));
    if (f.net == NULL) {
        return;
    }
    gs_client_open(&c, gsp_net_port(f.net));
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(100));

    n = gsp_server_connection_ids(f.server, ids, 4u);
    GS_ASSERT_EQ(n, 1u);
    if (n != 1u) {
        gs_client_close(&c);
        gs_fix_close(&f);
        return;
    }
    /* ⚠ THIS IS WHAT GSP_EV_CLOSE_REQUESTED IS FOR.  The library cannot close a
     * socket; a host that sets policy.protocol_error_close_threshold has to act
     * on the event, and this is the call it makes. */
    GS_ASSERT_EQ(gsp_net_close_connection(f.net, ids[0]), GSP_OK);
    GS_ASSERT_EQ(gsp_net_close_connection(f.net, ids[0]), GSP_ERR_UNKNOWN_CONNECTION);
    gs_fix_drain_events(&f);

    closed = gs_first_event(&f, GSP_EV_CONNECTION_CLOSED);
    GS_ASSERT(closed != NULL);
    if (closed != NULL) {
        GS_ASSERT_EQ(closed->u.connection.cause, (uint8_t)GSP_CLOSE_LOCAL_REQUEST);
    }
    GS_ASSERT_EQ(gsp_server_connection_count(f.server), 0u);
    (void)gs_run(&f, &cl, 1u, 0u, GS_MS(100));
    GS_ASSERT_MSG(c.eof, "the client sees the socket close");

    gs_client_close(&c);
    gs_fix_close(&f);
}

GS_TEST(net_bind_reports_an_unusable_host)
{
    gsp_server *server = gs_srv_create(NULL);
    gsp_net_config cfg = gsp_net_config_default();
    gsp_net *net = NULL;
    char error[GSP_NET_ERROR_MAX];

    GS_ASSERT(server != NULL);
    if (server == NULL) {
        return;
    }
    /* An address that resolves to nothing this machine owns: the same report
     * path as CT-T09, reached by the other route a bind can fail. */
    cfg.host = "203.0.113.7";       /* TEST-NET-3, RFC 5737 */
    cfg.ephemeral_port = true;
    error[0] = '\0';
    GS_ASSERT(gsp_net_open(server, &cfg, &net, error, sizeof(error)) < GSP_OK);
    GS_ASSERT(net == NULL);
    GS_ASSERT_MSG(error[0] != '\0', "the reason is reported, not swallowed");
    printf("    note: %s\n", error);

    gs_srv_free(server);
}

/* ------------------------------------------------------------------------ */
/* ⚠ CT-T06 IS NOT RUN HERE AND IS NOT PRETENDED.  Printed after the run,     */
/* through the harness's own pre-main hook, so that a green suite still says   */
/* out loud which row nothing on this machine can reach.                      */
/* ------------------------------------------------------------------------ */
static void gs_net_t06_note(void)
{
    printf("\n"
           "⚠ CT-T06 (a client on a SECOND MACHINE) is not run here and is not\n"
           "  pretended: it needs a real LAN and a second host.  It is the one case\n"
           "  that can tell a listener bound to 0.0.0.0 from one bound to loopback,\n"
           "  which is the difference between a launch monitor working and not\n"
           "  (design §6.1).  Run   gsplisten --host 0.0.0.0   and aim\n"
           "  tools/gsp_shoot.py at it from another machine.\n");
}

GS_TEST_CTOR_(gs_net_t06)
{
    (void)atexit(gs_net_t06_note);
}

GS_TEST_MAIN()
