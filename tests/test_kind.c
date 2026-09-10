/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_kind.c — CT-K01 … CT-K08, docs/conformance.md §3.3.
 *
 * ⚠ THERE IS NO TYPE FIELD ON THE WIRE.  What a message IS gets derived from
 * three booleans in ShotDataOptions (protocol.md §3.4), and every one of the
 * four rows below is a shape some client sends.  ⚠ And all of them are answered
 * 200: a heartbeat is "a valid shot message" to GSPro, which is the one thing
 * [TNB] asked the vendor to change and the vendor did not.
 */
#include "gspro/gspro.h"
#include "gs_test.h"
#include "gs_srv.h"

static void decode_text(const char *json, gsp_message *out)
{
    memset(out, 0, sizeof(*out));
    GS_ASSERT_EQ(gsp_message_decode(GS_BYTES(json), out), GSP_OK);
}

static void decode_fixture(const char *name, gsp_message *out)
{
    size_t len = 0;
    unsigned char *fx = gs_fixture(name, &len);
    while (len > 0 && (fx[len - 1] == '\n' || fx[len - 1] == '\r')) {
        len--;
    }
    memset(out, 0, sizeof(*out));
    GS_ASSERT_MSG(gsp_message_decode(fx, len, out) == GSP_OK, name);
    free(fx);
}

/* CT-K01 — ball true, club false: a shot. */
GS_TEST(CT_K01_ball_only_is_a_shot)
{
    gsp_message m;
    decode_fixture("gsp_full.json", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);
    GS_ASSERT(gsp_message_is_shot(&m));
}

/* CT-K02 — club data with no ball data is still a shot.  [TNB] offers
 * SendClubData() as a first-class call; whether GSPro renders anything from it
 * is protocol U8, but the classification is not in doubt. */
GS_TEST(CT_K02_club_only_is_a_shot)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"ShotNumber\":1,\"APIversion\":\"1\","
                "\"ClubData\":{\"Speed\":95.0},"
                "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":true}}",
                &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);
    GS_ASSERT_EQ(m.club.present, (uint32_t)GSP_CLUB_SPEED);
    GS_ASSERT_EQ(m.ball.present, 0);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_BALL_INCOMPLETE) == 0u,
                  "a shot that never claimed ball data is not missing any");
}

/* CT-K03 — neither, heartbeat true.  [R10] [FB] [OB] [OF] [PIT] all send this. */
GS_TEST(CT_K03_heartbeat)
{
    gsp_message m;
    static const char *const fixtures[] = {
        "r10_heartbeat.json", "of_heartbeat.json", "ob_heartbeat.json",
        "fb_heartbeat_newline.json", "pit_keepalive.json",
        "osp_connect_heartbeat.json"
    };
    size_t i;
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        decode_fixture(fixtures[i], &m);
        GS_ASSERT_MSG(m.kind == GSP_MSG_HEARTBEAT, fixtures[i]);
        GS_ASSERT_MSG(!gsp_message_is_shot(&m), fixtures[i]);
    }
}

/* ⚠ [PIT] and [OF] carry FULL BallData and ClubData objects on a heartbeat.
 * The flags are what classify, never the presence of the objects. */
GS_TEST(CT_K03b_heartbeat_carrying_ball_object)
{
    gsp_message m;
    decode_fixture("pit_keepalive.json", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_HEARTBEAT);
    GS_ASSERT_MSG(m.ball.present != 0u, "the object was there");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG) != 0u,
                  "an object without its flag is worth recording");
}

/* CT-K04 — neither flag, heartbeat false or absent: a status message.
 * [TNB]'s SendLaunchMonitorStatus() is exactly this. */
GS_TEST(CT_K04_status_message)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"ShotNumber\":1,\"APIversion\":\"1\","
                "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false,"
                "\"LaunchMonitorIsReady\":true,\"LaunchMonitorBallDetected\":false,"
                "\"IsHeartBeat\":false}}", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_STATUS);
    GS_ASSERT_EQ(m.options.launch_monitor_is_ready, 1);
    GS_ASSERT_EQ(m.options.launch_monitor_ball_detected, 0);
}

/* CT-K05 — heartbeat AND ready in one message.  [OSP] defaults IsHeartBeat to
 * true and sets the ready flag beside it, so a server that read the status
 * flags only off a "status" message would never see [OSP]'s. */
GS_TEST(CT_K05_heartbeat_carries_status_flags)
{
    gsp_message m;
    decode_fixture("osp_connect_heartbeat.json", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_HEARTBEAT);
    GS_ASSERT_EQ(m.options.launch_monitor_is_ready, 1);
    GS_ASSERT_MSG((m.options.present & (uint8_t)GSP_OPT_LAUNCH_MONITOR_IS_READY) != 0u,
                  "the ready flag was on the wire");
}

/* …and the server raises the state change from it. */
GS_TEST(CT_K05b_server_reports_state_change_from_a_heartbeat)
{
    gsp_server *s = gs_srv_open();
    gsp_event evs[16];
    size_t n;

    (void)gs_srv_feed(s, "{\"DeviceID\":\"x\",\"ShotDataOptions\":"
                         "{\"ContainsBallData\":false,\"ContainsClubData\":false,"
                         "\"LaunchMonitorIsReady\":true,\"IsHeartBeat\":true}}");
    n = gs_srv_events(s, evs, 16);
    {
        const gsp_event *e = gs_event_of(evs, n, GSP_EV_CLIENT_STATE);
        GS_ASSERT_MSG(e != NULL, "ready went false -> true and must be reported");
        if (e != NULL) {
            GS_ASSERT_EQ(e->u.client_state.ready, 1);
            GS_ASSERT_EQ(e->u.client_state.previous_ready, 0);
        }
    }

    /* ⚠ Only on CHANGE.  [FB] repeats both flags every five seconds and a UI
     * must not repaint for that. */
    (void)gs_srv_feed(s, "{\"DeviceID\":\"x\",\"ShotDataOptions\":"
                         "{\"ContainsBallData\":false,\"ContainsClubData\":false,"
                         "\"LaunchMonitorIsReady\":true,\"IsHeartBeat\":true}}");
    n = gs_srv_events(s, evs, 16);
    GS_ASSERT_EQ(gs_count_of(evs, n, GSP_EV_CLIENT_STATE), 0);
    gs_srv_free(s);
}

/* CT-K06 — no ShotDataOptions at all: status, and flagged. */
GS_TEST(CT_K06_missing_options_is_status)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"ShotNumber\":1,\"APIversion\":\"1\"}", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_STATUS);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_MISSING_OPTIONS) != 0u, "flagged");
}

/* CT-K07 — the flag promises ball data that is not there. */
GS_TEST(CT_K07_ball_flag_without_object)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"ShotDataOptions\":"
                "{\"ContainsBallData\":true,\"ContainsClubData\":false}}", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_BALL_FLAG_WITHOUT_OBJECT) != 0u, "mismatch");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_BALL_INCOMPLETE) != 0u,
                  "a shot claiming ball data with none is incomplete");
    GS_ASSERT_EQ(m.ball.present, 0);
}

/* CT-K08 — the object is there and the flag is not. */
GS_TEST(CT_K08_ball_object_without_flag)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"BallData\":{\"Speed\":140.0,\"HLA\":1.0,\"VLA\":12.0},"
                "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false}}",
                &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_STATUS);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG) != 0u, "mismatch");
    GS_ASSERT_MSG((m.ball.present & (uint32_t)GSP_BALL_SPEED) != 0u,
                  "the values are decoded either way; the host decides");
    GS_ASSERT_NEAR(m.ball.speed, 140.0, 1e-9);
}

/* ⚠ [MLM] sets ContainsClubData true on every shot whether or not its device
 * measured anything, so a true flag is not evidence the numbers are real. */
GS_TEST(CT_K08b_club_flag_true_is_not_evidence)
{
    gsp_message m;
    decode_fixture("mlm_backspin.json", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);
    GS_ASSERT_EQ(m.options.contains_club_data, 1);
    GS_ASSERT_EQ(m.club.present, 0x3ffu);
    GS_ASSERT_NEAR(m.club.lie, 0.0, 1e-12);
    GS_ASSERT_MSG((m.club.present & (uint32_t)GSP_CLUB_LIE) != 0u,
                  "sent as a literal zero: present, and meaningless");
}

GS_TEST_MAIN()
