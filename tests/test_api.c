/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_api.c — the vocabulary tables and the API surface itself.
 *
 * These are not protocol cases; they are the contract the headers state.  Every
 * one of them is a place where a table and an enum have to agree, and where a
 * disagreement produces a plausible wrong answer rather than a failure — a
 * club code that maps to the wrong club, a status that prints as another one, a
 * sensitive event whose formatter forgot to redact.
 */
#include "gspro/gspro.h"
#include "gs_test.h"

/* The two-letter code of every club, in the order [TNB] and [R10] agree on. */
static const struct { gsp_club club; const char *code; } CLUBS[] = {
    { GSP_CLUB_DR, "DR" },
    { GSP_CLUB_W2, "W2" }, { GSP_CLUB_W3, "W3" }, { GSP_CLUB_W4, "W4" },
    { GSP_CLUB_W5, "W5" }, { GSP_CLUB_W6, "W6" }, { GSP_CLUB_W7, "W7" },
    { GSP_CLUB_H2, "H2" }, { GSP_CLUB_H3, "H3" }, { GSP_CLUB_H4, "H4" },
    { GSP_CLUB_H5, "H5" }, { GSP_CLUB_H6, "H6" }, { GSP_CLUB_H7, "H7" },
    { GSP_CLUB_I1, "I1" }, { GSP_CLUB_I2, "I2" }, { GSP_CLUB_I3, "I3" },
    { GSP_CLUB_I4, "I4" }, { GSP_CLUB_I5, "I5" }, { GSP_CLUB_I6, "I6" },
    { GSP_CLUB_I7, "I7" }, { GSP_CLUB_I8, "I8" }, { GSP_CLUB_I9, "I9" },
    { GSP_CLUB_PW, "PW" }, { GSP_CLUB_GW, "GW" }, { GSP_CLUB_SW, "SW" },
    { GSP_CLUB_LW, "LW" }, { GSP_CLUB_PT, "PT" }
};

/* ⚠ Every code round-trips, and every enumerator is covered.  A club that
 * parsed to the wrong neighbour would put a launch monitor in the wrong mode
 * with nothing anywhere reporting a fault. */
GS_TEST(API_club_codes_round_trip)
{
    size_t i;
    GS_ASSERT_EQ(sizeof(CLUBS) / sizeof(CLUBS[0]), GSP_CLUB_COUNT - 1);

    for (i = 0; i < sizeof(CLUBS) / sizeof(CLUBS[0]); ++i) {
        GS_ASSERT_STR(gsp_club_code(CLUBS[i].club), CLUBS[i].code);
        GS_ASSERT_EQ(gsp_club_parse(CLUBS[i].code), CLUBS[i].club);
        GS_ASSERT_MSG(gsp_club_name(CLUBS[i].club)[0] != '\0', CLUBS[i].code);
    }

    /* Case-insensitive, as every other value match is. */
    GS_ASSERT_EQ(gsp_club_parse("pt"), GSP_CLUB_PT);
    GS_ASSERT_EQ(gsp_club_parse("Dr"), GSP_CLUB_DR);

    /* Unknown is unknown, never a default club. */
    GS_ASSERT_EQ(gsp_club_parse("XX"), GSP_CLUB_UNKNOWN);
    GS_ASSERT_EQ(gsp_club_parse(""), GSP_CLUB_UNKNOWN);
    GS_ASSERT_EQ(gsp_club_parse(NULL), GSP_CLUB_UNKNOWN);
    GS_ASSERT_STR(gsp_club_code(GSP_CLUB_UNKNOWN), "");
    /* ⚠ [PIT] defaults an unrecognised club to DRIVER, which is a client-side
     * choice.  A LIBRARY that did that would hide the fact from every consumer. */
    GS_ASSERT_EQ(gsp_club_parse("W8"), GSP_CLUB_UNKNOWN);
}

GS_TEST(API_handed_and_units_round_trip)
{
    GS_ASSERT_EQ(gsp_handed_parse("RH"), GSP_HANDED_RIGHT);
    GS_ASSERT_EQ(gsp_handed_parse("LH"), GSP_HANDED_LEFT);
    GS_ASSERT_EQ(gsp_handed_parse("rh"), GSP_HANDED_RIGHT);
    GS_ASSERT_EQ(gsp_handed_parse("R"), GSP_HANDED_UNKNOWN);
    GS_ASSERT_EQ(gsp_handed_parse(NULL), GSP_HANDED_UNKNOWN);
    GS_ASSERT_STR(gsp_handed_text(GSP_HANDED_RIGHT), "RH");
    GS_ASSERT_STR(gsp_handed_text(GSP_HANDED_LEFT), "LH");
    GS_ASSERT_STR(gsp_handed_text(GSP_HANDED_UNKNOWN), "");

    GS_ASSERT_EQ(gsp_units_parse("Yards"), GSP_UNITS_YARDS);
    GS_ASSERT_EQ(gsp_units_parse("yards"), GSP_UNITS_YARDS);
    GS_ASSERT_EQ(gsp_units_parse("Meters"), GSP_UNITS_METERS);
    GS_ASSERT_EQ(gsp_units_parse("Metres"), GSP_UNITS_UNKNOWN);
    GS_ASSERT_EQ(gsp_units_parse(NULL), GSP_UNITS_UNKNOWN);
    GS_ASSERT_STR(gsp_units_text(GSP_UNITS_YARDS), "Yards");
    GS_ASSERT_STR(gsp_units_text(GSP_UNITS_METERS), "Meters");
    GS_ASSERT_STR(gsp_units_text(GSP_UNITS_UNKNOWN), "");
}

/* Every status has a distinct, non-empty string.  A log line is often the only
 * evidence left after a fault. */
GS_TEST(API_every_status_has_a_distinct_string)
{
    static const gsp_status all[] = {
        GSP_OK, GSP_PENDING, GSP_ERR_INVALID_ARG, GSP_ERR_INVALID_STATE,
        GSP_ERR_NO_MEMORY, GSP_ERR_BUFFER_TOO_SMALL, GSP_ERR_NOT_SUPPORTED,
        GSP_ERR_TOO_MANY_CONNECTIONS, GSP_ERR_UNKNOWN_CONNECTION,
        GSP_ERR_MALFORMED, GSP_ERR_MESSAGE_TOO_LARGE, GSP_ERR_QUEUE_FULL,
        GSP_ERR_CLOSED
    };
    size_t i, j;

    for (i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const char *si = gsp_status_str(all[i]);
        GS_ASSERT(si != NULL && si[0] != '\0');
        for (j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
            GS_ASSERT_MSG(strcmp(si, gsp_status_str(all[j])) != 0,
                          "two statuses share a string");
        }
    }
    /* Never NULL, whatever it is handed. */
    GS_ASSERT(gsp_status_str((gsp_status)12345) != NULL);
}

GS_TEST(API_every_event_type_has_a_name)
{
    int t;
    for (t = 0; t < (int)GSP_EVENT_TYPE_COUNT; ++t) {
        const char *n = gsp_event_type_name((gsp_event_type)t);
        GS_ASSERT(n != NULL);
        if (t != (int)GSP_EV_NONE) {
            GS_ASSERT_MSG(n[0] != '\0', "an unnamed event type prints as nothing in a log");
        }
    }
    GS_ASSERT(gsp_event_type_name((gsp_event_type)9999) != NULL);
}

/* ⚠ THE REDACTION SWEEP.  design §9.2: a logging path that only redacts the
 * events a developer happened to hit is one that leaks during an incident.
 * Every event type is formatted with an identifier in every field that can
 * hold one, and the redacted form must contain none of them. */
GS_TEST(API_no_event_leaks_an_identifier_when_redacted)
{
    int t;
    for (t = 1; t < (int)GSP_EVENT_TYPE_COUNT; ++t) {
        gsp_event ev;
        char line[1024];

        memset(&ev, 0, sizeof(ev));
        ev.type = (uint8_t)t;
        ev.conn = 3;
        ev.host_time_us = 1234567;
        snprintf(ev.u.connection.info.peer, sizeof(ev.u.connection.info.peer),
                 "%s", "198.51.100.23:40001");
        snprintf(ev.u.connection.info.device_id, sizeof(ev.u.connection.info.device_id),
                 "%s", "Foresight GC2 (SECRET99)");

        GS_ASSERT(gsp_event_format(&ev, line, sizeof(line), false) > 0);
        GS_ASSERT_MSG(strstr(line, "198.51.100.23") == NULL,
                      gsp_event_type_name((gsp_event_type)t));
        GS_ASSERT_MSG(strstr(line, "SECRET99") == NULL,
                      gsp_event_type_name((gsp_event_type)t));
        GS_ASSERT_MSG(line[0] != '\0', "a redacted line is still a line");
    }

    /* And the same sweep with the message payload, which carries a DeviceID. */
    for (t = 1; t < (int)GSP_EVENT_TYPE_COUNT; ++t) {
        gsp_event ev;
        char line[1024];
        memset(&ev, 0, sizeof(ev));
        ev.type = (uint8_t)t;
        snprintf(ev.u.message.device_id, sizeof(ev.u.message.device_id),
                 "%s", "Foresight GC2 (SECRET99)");
        GS_ASSERT(gsp_event_format(&ev, line, sizeof(line), false) > 0);
        GS_ASSERT_MSG(strstr(line, "SECRET99") == NULL, "message payload leaked");
    }
}

GS_TEST(API_sensitive_events_are_marked)
{
    gsp_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = (uint8_t)GSP_EV_SHOT;
    snprintf(ev.u.message.device_id, sizeof(ev.u.message.device_id), "%s", "dev");
    GS_ASSERT_MSG(gsp_event_is_sensitive(&ev), "a shot names the sending software");

    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)GSP_EV_CONNECTION_OPENED;
    snprintf(ev.u.connection.info.peer, sizeof(ev.u.connection.info.peer),
             "%s", "198.51.100.23:1");
    GS_ASSERT_MSG(gsp_event_is_sensitive(&ev), "a peer address identifies a household");

    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)GSP_EV_SESSION_STATE_SENT;
    GS_ASSERT_MSG(!gsp_event_is_sensitive(&ev), "a 202 carries nothing personal");

    GS_ASSERT(!gsp_event_is_sensitive(NULL));
}

/* Every flag bit has a name, and the function refuses a value that is not one
 * bit — a caller iterating a mask must not get a plausible answer for a pair. */
GS_TEST(API_every_message_flag_has_a_name)
{
    unsigned bit;
    for (bit = 0; bit < 17u; ++bit) {
        const char *n = gsp_message_flag_name((gsp_message_flag)(1u << bit));
        GS_ASSERT(n != NULL);
        GS_ASSERT_MSG(n[0] != '\0', "a flag with no name is a flag nobody can log");
    }
    GS_ASSERT_STR(gsp_message_flag_name((gsp_message_flag)0), "");
    GS_ASSERT_STR(gsp_message_flag_name((gsp_message_flag)3u), "");   /* two bits */
    GS_ASSERT_STR(gsp_message_flag_name((gsp_message_flag)(1u << 30)), "");
}

/* The defaults the headers document.  ⚠ Each of these is a decision with a
 * reason recorded beside it; a silent change to one is a change of behaviour
 * for every consumer that never touched the field. */
GS_TEST(API_documented_defaults)
{
    gsp_server_config c = gsp_server_config_default();
    gsp_server_policy p = gsp_server_policy_default();

    /* 0 means "take the default" for every size, so the struct stays
     * zero-initialisable and a binding need not know the numbers. */
    GS_ASSERT_EQ(c.max_connections, 0);
    GS_ASSERT_EQ(c.event_ring, 0);
    GS_ASSERT_EQ(c.write_ring, 0);
    GS_ASSERT_MSG(c.wire_ring == 0, "the wire log is OFF unless asked for");

    GS_ASSERT_MSG(p.announce_player_on_connect, "protocol U1: better served than not");
    GS_ASSERT_MSG(p.announce_ready_on_connect, "[OSP] does not arm until it sees a 202");
    GS_ASSERT_MSG(!p.reject_incomplete_shots, "U2/U8 unsettled; a 501 may loop a client");
    GS_ASSERT_MSG(!p.record_identifiers, "identifiers are redacted unless asked");
    GS_ASSERT_MSG(p.idle_alarm_us == 0, "the protocol has no deadline of its own");
    GS_ASSERT_MSG(p.write_spacing_us == 0, "spacing is opt-in");
    GS_ASSERT_MSG(p.protocol_error_close_threshold == 0, "never ask for a close by default");
    GS_ASSERT_MSG(p.max_message_bytes == 0, "0 selects the 16 KiB bound");

    /* And the config carries the same policy the policy function returns. */
    GS_ASSERT_EQ(c.policy.announce_player_on_connect, p.announce_player_on_connect);
    GS_ASSERT_EQ(c.policy.announce_ready_on_connect, p.announce_ready_on_connect);
}

GS_TEST(API_version_and_constants)
{
    GS_ASSERT_STR(gsp_version_string(), GSP_VERSION_STRING);
    GS_ASSERT_EQ(gsp_abi_version(), GSP_ABI_VERSION);

    /* The two ports the protocol knows about, and nothing else. */
    GS_ASSERT_EQ(GSP_DEFAULT_PORT, 921);
    GS_ASSERT_EQ(GSP_ALT_PORT, 922);
    GS_ASSERT_STR(GSP_API_VERSION_TEXT, "1");

    /* ⚠ The two strings a client compares EXACTLY ([OSP]). */
    GS_ASSERT_STR(GSP_TEXT_READY, "GSPro ready");
    GS_ASSERT_STR(GSP_TEXT_ROUND_ENDED, "GSPro round ended");
    GS_ASSERT_STR(GSP_TEXT_PLAYER_INFO, "GSPro Player Information");

    /* A write must fit a 201 with every member present. */
    GS_ASSERT(GSP_WRITE_MAX >= 200);
}

/* gsp_message_ball_complete() is the vendor's rule, published so a host can
 * apply it to a message it built itself. */
GS_TEST(API_ball_complete_is_the_vendors_rule)
{
    gsp_ball_data b;

    /* Speed, HLA, VLA, and total+axis: complete. */
    memset(&b, 0, sizeof(b));
    b.present = (uint32_t)(GSP_BALL_SPEED | GSP_BALL_HLA | GSP_BALL_VLA |
                           GSP_BALL_TOTAL_SPIN | GSP_BALL_SPIN_AXIS);
    GS_ASSERT(gsp_message_ball_complete(&b));

    /* The pair instead of the total: also complete ([GSP] says either). */
    memset(&b, 0, sizeof(b));
    b.present = (uint32_t)(GSP_BALL_SPEED | GSP_BALL_HLA | GSP_BALL_VLA |
                           GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN);
    GS_ASSERT(gsp_message_ball_complete(&b));

    /* Neither spin representation: not complete. */
    memset(&b, 0, sizeof(b));
    b.present = (uint32_t)(GSP_BALL_SPEED | GSP_BALL_HLA | GSP_BALL_VLA);
    GS_ASSERT(!gsp_message_ball_complete(&b));

    /* Half a pair is not a pair. */
    memset(&b, 0, sizeof(b));
    b.present = (uint32_t)(GSP_BALL_SPEED | GSP_BALL_HLA | GSP_BALL_VLA |
                           GSP_BALL_BACK_SPIN);
    GS_ASSERT(!gsp_message_ball_complete(&b));

    /* No launch angles: not complete, whatever the spin says. */
    memset(&b, 0, sizeof(b));
    b.present = (uint32_t)(GSP_BALL_SPEED | GSP_BALL_TOTAL_SPIN | GSP_BALL_SPIN_AXIS);
    GS_ASSERT(!gsp_message_ball_complete(&b));

    GS_ASSERT(!gsp_message_ball_complete(NULL));
}

/* The response encoder, on its own: it is the only path bytes leave by. */
GS_TEST(API_response_encoder)
{
    char buf[512];
    size_t written = 0;
    gsp_response r;
    gsp_player_info p;

    /* ⚠ Guarded on the status, and every case below is too.  GS_ASSERT reports
     * and CONTINUES, so a case that read `buf[written - 1]` after a failed
     * encode would index buf[-1] — which is how the sanitizer build found this
     * very line.  A conformance case must not itself be undefined when the
     * thing it is testing is absent. */
    if (gsp_response_encode(GSP_CODE_SHOT_RECEIVED, GSP_TEXT_SHOT_RECEIVED, NULL,
                            buf, sizeof(buf), &written) == GSP_OK) {
        GS_ASSERT(written > 0 && written < sizeof(buf));
        GS_ASSERT_EQ(buf[written], '\0');
        GS_ASSERT_MSG(written > 0 && buf[written - 1] == '}',
                      "nothing is appended after the object");
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gsp_response_decode((const uint8_t *)buf, written, &r), GSP_OK);
        GS_ASSERT_EQ(r.code, 200);
        GS_ASSERT_EQ(r.has_player, 0);
    } else {
        GS_ASSERT_MSG(0, "gsp_response_encode is not implemented");
    }

    /* A Message may be omitted, as [OSG] does. */
    if (gsp_response_encode(GSP_CODE_SHOT_RECEIVED, NULL, NULL, buf, sizeof(buf),
                            &written) == GSP_OK) {
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gsp_response_decode((const uint8_t *)buf, written, &r), GSP_OK);
        GS_ASSERT_EQ(r.has_message, 0);
    }

    /* A player with UNKNOWN members omits them rather than inventing a value. */
    memset(&p, 0, sizeof(p));
    p.handed = (uint8_t)GSP_HANDED_UNKNOWN;
    p.club = (uint8_t)GSP_CLUB_I7;
    if (gsp_response_encode(GSP_CODE_PLAYER_INFO, GSP_TEXT_PLAYER_INFO, &p, buf,
                            sizeof(buf), &written) == GSP_OK) {
        GS_ASSERT_MSG(gs_memfind(buf, written, "\"Handed\"") == NULL,
                      "an unknown hand is omitted");
        GS_ASSERT_MSG(gs_memfind(buf, written, "I7") != NULL, "a known club is sent");
    }

    /* ⚠ A DeviceID with a quote or a backslash in it must be escaped, or the
     * reply we build from it is not JSON.  Client authors choose these freely. */
    memset(&p, 0, sizeof(p));
    p.club = (uint8_t)GSP_CLUB_DR;
    if (gsp_response_encode(GSP_CODE_FAILURE, "bad \"input\" \\ here", NULL, buf,
                            sizeof(buf), &written) == GSP_OK) {
        memset(&r, 0, sizeof(r));
        GS_ASSERT_EQ(gsp_response_decode((const uint8_t *)buf, written, &r), GSP_OK);
        GS_ASSERT_STR(r.message, "bad \"input\" \\ here");
    }
}

GS_TEST_MAIN()
