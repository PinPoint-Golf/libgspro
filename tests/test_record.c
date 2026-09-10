/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_record.c — the `.gswire` container and the replay (design §7).
 *
 * ⚠ NO CT IDS HERE, DELIBERATELY, and the same rule as tests/test_net.c's four
 * transport cases: docs/conformance.md numbers what the PROTOCOL and its
 * clients demand, and this is a container of ours in an optional module.  What
 * the CORE must record is CT-W01…W12 in test_wire.c; what a FILE of it must
 * survive is here.
 *
 * ⚠ THE ONE THING TO BREAK IS SILENCE.  A capture is taken once, beside a
 * launch monitor that is not coming back (design §11 package 7), so every
 * failure mode below is about a file that lies rather than one that errors: a
 * clamped payload that looks like a message, a renumbered sequence that hides a
 * gap, a full disk that ends a capture quietly.
 */
#include "gs_srv.h"

#include "gspro/record.h"

#include <stdio.h>

/* A path in the build tree's temp space.  ⚠ Named per case so a failing run
 * leaves the file that failed rather than the last one written. */
static const char *gs_tmp(const char *name)
{
    static char path[512];
    (void)snprintf(path, sizeof(path), "%s.gswire", name);
    return path;
}

static gsp_wire_chunk gs_make_chunk(uint32_t seq, gsp_conn_id conn, gsp_wire_direction dir,
                                    const char *text, uint8_t flags)
{
    gsp_wire_chunk c;
    size_t n = (text != NULL) ? strlen(text) : 0u;
    memset(&c, 0, sizeof(c));
    if (n > (size_t)GSP_WIRE_CHUNK_MAX) {
        n = (size_t)GSP_WIRE_CHUNK_MAX;
    }
    c.sequence = seq;
    c.conn = conn;
    c.direction = (uint8_t)dir;
    c.flags = flags;
    c.length = (uint16_t)n;
    c.host_time_us = (gsp_time_us)1000000000 + (gsp_time_us)seq * 1000;
    if (n > 0u) {
        memcpy(c.data, text, n);
    }
    return c;
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_round_trips_every_chunk_field_byte_exactly)
{
    const char *path = gs_tmp("round_trip");
    gsp_recording_info info = gsp_recording_info_default();
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_wire_chunk written[3];
    gsp_wire_chunk read_back[8];
    size_t n;
    size_t i;

    written[0] = gs_make_chunk(7u, 3u, GSP_WIRE_CLIENT_TO_SERVER,
                               "{\"DeviceID\":\"A\",\"ShotNumber\":1}", 0u);
    written[1] = gs_make_chunk(8u, 3u, GSP_WIRE_SERVER_TO_CLIENT,
                               "{\"Code\":200,\"Message\":\"Shot received successfully\"}",
                               (uint8_t)GSP_WIRE_REDACTED);
    written[2] = gs_make_chunk(9u, 3u, GSP_WIRE_META, "CONNECTION_CLOSED conn=3",
                               (uint8_t)GSP_WIRE_LOST);

    GS_ASSERT_EQ(gsp_recorder_open(path, &info, &rec), GSP_OK);
    if (rec == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_write(rec, written, 3u), GSP_OK);
    GS_ASSERT_EQ(gsp_recorder_chunks(rec), 3u);
    GS_ASSERT(gsp_recorder_bytes(rec) > 3u * GSP_RECORD_ENTRY_HEADER);
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        return;
    }
    n = gsp_replay_read(rp, read_back, 8u);
    GS_ASSERT_EQ(n, 3u);
    for (i = 0; i < n && i < 3u; ++i) {
        /* ⚠ EVERY field, not just the payload: a container that dropped the
         * connection id would turn two devices into one, and one that dropped
         * the flags would turn a redacted capture into a verbatim-looking one. */
        GS_ASSERT_EQ(read_back[i].sequence, written[i].sequence);
        GS_ASSERT_EQ(read_back[i].conn, written[i].conn);
        GS_ASSERT_EQ(read_back[i].direction, written[i].direction);
        GS_ASSERT_EQ(read_back[i].flags, written[i].flags);
        GS_ASSERT_EQ(read_back[i].length, written[i].length);
        GS_ASSERT_EQ(read_back[i].host_time_us, written[i].host_time_us);
        GS_ASSERT_MEM(read_back[i].data, written[i].data, written[i].length);
    }
    GS_ASSERT_EQ(gsp_replay_read(rp, read_back, 8u), 0u);
    GS_ASSERT_EQ(gsp_replay_status(rp), GSP_PENDING);   /* a clean end of file */
    GS_ASSERT_EQ(gsp_replay_chunks_read(rp), 3u);
    gsp_replay_close(rp);
    (void)remove(path);
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_keeps_a_sequence_gap_rather_than_renumbering_it)
{
    /* ⚠ THE GAP IS THE EVIDENCE.  GSP_WIRE_LOST says chunks were dropped but
     * not HOW MANY; a reader that renumbered from its own ordinal would turn a
     * lossy capture into a complete-looking one (record.h). */
    const char *path = gs_tmp("sequence_gap");
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_wire_chunk out[2];
    gsp_wire_chunk in[2];

    in[0] = gs_make_chunk(4u, 1u, GSP_WIRE_CLIENT_TO_SERVER, "{}", 0u);
    in[1] = gs_make_chunk(99u, 1u, GSP_WIRE_CLIENT_TO_SERVER, "{}",
                          (uint8_t)GSP_WIRE_LOST);

    GS_ASSERT_EQ(gsp_recorder_open(path, NULL, &rec), GSP_OK);
    if (rec == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_write(rec, in, 2u), GSP_OK);
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_replay_read(rp, out, 2u), 2u);
    GS_ASSERT_EQ(out[0].sequence, 4u);
    GS_ASSERT_EQ(out[1].sequence, 99u);
    GS_ASSERT((out[1].flags & (uint8_t)GSP_WIRE_LOST) != 0u);
    gsp_replay_close(rp);
    (void)remove(path);
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_header_says_which_clock_and_whether_identifiers_were_kept)
{
    const char *path = gs_tmp("header");
    gsp_recording_info info = gsp_recording_info_default();
    const gsp_recording_info *back;
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;

    /* ⚠ The DEFAULT is redacted, here as in the policy (design §9.2). */
    GS_ASSERT(!info.identifiers_recorded);
    GS_ASSERT_STR(info.clock, GSP_RECORD_CLOCK_MONOTONIC);
    GS_ASSERT_STR(info.library, gsp_version_string());

    info.identifiers_recorded = true;
    info.port = 921u;
    (void)snprintf(info.note, sizeof(info.note),
                   "Rapsodo MLM2PRO on the mat, answering protocol U2");

    GS_ASSERT_EQ(gsp_recorder_open(path, &info, &rec), GSP_OK);
    if (rec == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        return;
    }
    back = gsp_replay_info(rp);
    GS_ASSERT(back != NULL);
    if (back != NULL) {
        GS_ASSERT(back->identifiers_recorded);
        GS_ASSERT_EQ(back->port, 921u);
        GS_ASSERT_STR(back->clock, GSP_RECORD_CLOCK_MONOTONIC);
        GS_ASSERT_STR(back->library, gsp_version_string());
        GS_ASSERT(strstr(back->note, "MLM2PRO") != NULL);
    }
    gsp_replay_close(rp);
    (void)remove(path);
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_refuses_a_file_that_is_not_a_recording)
{
    const char *path = gs_tmp("not_a_recording");
    gsp_replay *rp = NULL;
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return;
    }
    (void)fputs("{\"DeviceID\":\"A\"}\n", f);
    (void)fclose(f);

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_ERR_MALFORMED);
    GS_ASSERT(rp == NULL);
    (void)remove(path);

    GS_ASSERT_EQ(gsp_replay_open("this-file-does-not-exist.gswire", &rp),
                 GSP_ERR_INVALID_STATE);
    GS_ASSERT(rp == NULL);
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_ignores_a_header_key_it_does_not_know)
{
    /* ⚠ protocol §9.7's rule, one register up: a capture written by a LATER
     * version must stay readable as far as this version understands it. */
    const char *path = gs_tmp("future_header");
    gsp_replay *rp = NULL;
    gsp_wire_chunk chunk;
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return;
    }
    (void)fputs(GSP_RECORD_MAGIC "\n", f);
    (void)fputs("library=9.9.9\n", f);
    (void)fputs("clock=monotonic_us\n", f);
    (void)fputs("byte_order=little\n", f);
    (void)fputs("identifiers=redacted\n", f);
    (void)fputs("weather=a key from a version that does not exist yet\n", f);
    (void)fputs("\n", f);
    (void)fclose(f);

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        return;
    }
    GS_ASSERT_STR(gsp_replay_info(rp)->library, "9.9.9");
    GS_ASSERT_EQ(gsp_replay_read(rp, &chunk, 1u), 0u);
    GS_ASSERT_EQ(gsp_replay_status(rp), GSP_PENDING);
    gsp_replay_close(rp);
    (void)remove(path);
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_refuses_a_length_above_the_chunk_maximum_rather_than_clamping)
{
    /* ⚠ THE ONE PLACE A HOSTILE FILE MEETS A FIXED BUFFER.  A clamp would put
     * half a message into something that looks like evidence. */
    const char *path = gs_tmp("oversize_record");
    gsp_replay *rp = NULL;
    gsp_wire_chunk chunk;
    uint8_t head[GSP_RECORD_ENTRY_HEADER];
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return;
    }
    (void)fputs(GSP_RECORD_MAGIC "\nbyte_order=little\n\n", f);
    memset(head, 0, sizeof(head));
    head[0] = 0xffu;                 /* length = 0x0000ffff, far above the max */
    head[1] = 0xffu;
    (void)fwrite(head, 1u, sizeof(head), f);
    (void)fclose(f);

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_replay_read(rp, &chunk, 1u), 0u);
    GS_ASSERT_EQ(gsp_replay_status(rp), GSP_ERR_MALFORMED);
    GS_ASSERT(gsp_replay_error(rp)[0] != '\0');
    gsp_replay_close(rp);
    (void)remove(path);

    /* And the writer refuses the same thing before it reaches a file. */
    {
        gsp_recorder *rec = NULL;
        gsp_wire_chunk big;
        const char *wpath = gs_tmp("oversize_write");
        memset(&big, 0, sizeof(big));
        big.length = (uint16_t)(GSP_WIRE_CHUNK_MAX + 1);
        GS_ASSERT_EQ(gsp_recorder_open(wpath, NULL, &rec), GSP_OK);
        if (rec != NULL) {
            GS_ASSERT_EQ(gsp_recorder_write(rec, &big, 1u), GSP_ERR_INVALID_ARG);
            GS_ASSERT_EQ(gsp_recorder_chunks(rec), 0u);
            GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);
        }
        (void)remove(wpath);
    }
}

/* ------------------------------------------------------------------------ */
GS_TEST(record_reports_a_file_that_ends_mid_record)
{
    const char *path = gs_tmp("truncated");
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_wire_chunk chunk = gs_make_chunk(1u, 1u, GSP_WIRE_CLIENT_TO_SERVER,
                                         "{\"DeviceID\":\"A\"}", 0u);
    gsp_wire_chunk out;
    long size;
    FILE *f;

    GS_ASSERT_EQ(gsp_recorder_open(path, NULL, &rec), GSP_OK);
    if (rec == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_write(rec, &chunk, 1u), GSP_OK);
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);

    /* Cut the last four bytes off: a capture interrupted by a pulled cable. */
    f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    (void)fseek(f, 0, SEEK_END);
    size = ftell(f);
    (void)fclose(f);
    GS_ASSERT(size > 4);
    {
        unsigned char *buf = (unsigned char *)malloc((size_t)size);
        size_t got;
        if (buf == NULL) {
            return;
        }
        f = fopen(path, "rb");
        if (f == NULL) {
            free(buf);
            return;
        }
        got = fread(buf, 1u, (size_t)size, f);
        (void)fclose(f);
        f = fopen(path, "wb");
        if (f == NULL) {
            free(buf);
            return;
        }
        (void)fwrite(buf, 1u, got - 4u, f);
        (void)fclose(f);
        free(buf);
    }

    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_replay_read(rp, &out, 1u), 0u);
    /* ⚠ REPORTED, not silently treated as the end of the file: a capture that
     * stopped early and one that finished are different facts. */
    GS_ASSERT_EQ(gsp_replay_status(rp), GSP_ERR_BUFFER_TOO_SMALL);
    gsp_replay_close(rp);
    (void)remove(path);
}

/* ------------------------------------------------------------------------ */
/* The replay, which is the reason the container exists                      */
/* ------------------------------------------------------------------------ */
GS_TEST(replay_drives_a_recorded_session_back_through_a_server)
{
    const char *path = gs_tmp("session");
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *live;
    gsp_server *again;
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_replay_report report;
    gsp_wire_chunk chunks[64];
    gsp_write_request writes[GS_DRAIN_MAX];
    size_t len = 0u;
    unsigned char *shot;
    size_t n;

    /* --- a live session, recorded ------------------------------------- */
    cfg.wire_ring = 64u;
    cfg.policy.record_identifiers = true;
    live = gs_srv_create(&cfg);
    if (live == NULL) {
        return;
    }
    shot = gs_fixture("tl_minimal.json", &len);
    (void)gsp_server_on_connection_opened(live, GS_CONN_A, "203.0.113.7:51022", gs_now);
    GS_ASSERT_EQ(gs_srv_feed_n(live, shot, len), GSP_OK);
    (void)gs_srv_writes(live, writes, GS_DRAIN_MAX);   /* the reply goes to the log */
    GS_ASSERT_EQ(gsp_server_on_connection_closed(live, GS_CONN_A, GSP_CLOSE_REMOTE_CLOSED,
                                                 gs_now), GSP_OK);

    n = 0u;
    {
        size_t got;
        while (n < 64u && (got = gsp_server_poll_wire(live, chunks + n, 64u - n)) > 0u) {
            n += got;
        }
    }
    GS_ASSERT(n >= 4u);   /* open, the shot, the reply, close */

    GS_ASSERT_EQ(gsp_recorder_open(path, NULL, &rec), GSP_OK);
    if (rec == NULL) {
        free(shot);
        gs_srv_free(live);
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_write(rec, chunks, n), GSP_OK);
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);
    gs_srv_free(live);

    /* --- and the same session, through a fresh server ------------------ */
    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (rp == NULL) {
        free(shot);
        return;
    }
    again = gs_srv_create(NULL);
    if (again == NULL) {
        gsp_replay_close(rp);
        free(shot);
        return;
    }
    memset(&report, 0, sizeof(report));
    GS_ASSERT_EQ(gsp_replay_into_server(rp, again, &report), GSP_OK);

    GS_ASSERT_EQ(report.chunks, (uint64_t)n);
    GS_ASSERT_EQ(report.messages, 1u);
    GS_ASSERT_EQ(report.shots, 1u);
    GS_ASSERT_EQ(report.connections, 1u);
    GS_ASSERT_EQ(report.protocol_errors, 0u);
    /* ⚠ THE COMPARISON IS THE PRODUCT: the same bytes in, the same reply out,
     * byte for byte, from a library that was not running at the time. */
    GS_ASSERT_EQ(report.replies_matched, 1u);
    GS_ASSERT_EQ(report.replies_differing, 0u);
    GS_ASSERT_EQ(report.replies_missing, 0u);
    GS_ASSERT_EQ(report.lost_chunks, 0u);

    gsp_replay_close(rp);
    gs_srv_free(again);
    free(shot);
    (void)remove(path);
}

/* ------------------------------------------------------------------------ */
GS_TEST(replay_reports_a_reply_that_has_changed)
{
    /* ⚠ THE CASE THE WHOLE MODULE IS FOR.  When protocol §11's U4 is settled
     * and the acknowledgement text changes, a capture replayed through the new
     * library must SAY that the reply differs — silently matching would make
     * the recording useless for the one question it was taken to answer. */
    const char *path = gs_tmp("changed_reply");
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *live;
    gsp_server *again;
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_replay_report report;
    gsp_wire_chunk chunks[32];
    gsp_write_request writes[GS_DRAIN_MAX];
    size_t n = 0u;
    size_t got;

    cfg.wire_ring = 32u;
    live = gs_srv_create(&cfg);
    if (live == NULL) {
        return;
    }
    (void)gsp_server_on_connection_opened(live, GS_CONN_A, NULL, gs_now);
    GS_ASSERT_EQ(gs_srv_feed(live, "{\"DeviceID\":\"A\",\"ShotNumber\":1}"), GSP_OK);
    (void)gs_srv_writes(live, writes, GS_DRAIN_MAX);
    while (n < 32u && (got = gsp_server_poll_wire(live, chunks + n, 32u - n)) > 0u) {
        n += got;
    }
    GS_ASSERT_EQ(gsp_recorder_open(path, NULL, &rec), GSP_OK);
    if (rec == NULL) {
        gs_srv_free(live);
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_write(rec, chunks, n), GSP_OK);
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);
    gs_srv_free(live);

    /* The same session, into a server whose acknowledgement text is what a
     * capture off the real GSPro might one day say it should be. */
    cfg = gsp_server_config_default();
    (void)snprintf(cfg.policy.ack_text, sizeof(cfg.policy.ack_text),
                   "Shot recieved succesfully");   /* [TNB]: protocol U4 */
    again = gs_srv_create(&cfg);
    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (again == NULL || rp == NULL) {
        if (rp != NULL) {
            gsp_replay_close(rp);
        }
        gs_srv_free(again);
        return;
    }
    memset(&report, 0, sizeof(report));
    GS_ASSERT_EQ(gsp_replay_into_server(rp, again, &report), GSP_OK);
    GS_ASSERT_EQ(report.messages, 1u);
    GS_ASSERT_EQ(report.replies_matched, 0u);
    GS_ASSERT_EQ(report.replies_differing, 1u);

    gsp_replay_close(rp);
    gs_srv_free(again);
    (void)remove(path);
}

GS_TEST(replay_does_not_call_a_host_originated_201_a_missing_reply)
{
    /* ⚠ THE TWO REASONS A RECORDED REPLY IS NOT REPRODUCED MEAN OPPOSITE
     * THINGS.  A 201 comes from the HOST's game state — which club the player
     * has (design §5.6) — and a capture holds the bytes on the wire, not the
     * state behind them, so a replay into a server that was never told the club
     * cannot produce it and must not report a regression.  An acknowledgement
     * that stopped being produced is a regression, and CT-style noise here would
     * bury it. */
    const char *path = gs_tmp("unsolicited");
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *live;
    gsp_server *again;
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_replay_report report;
    gsp_wire_chunk chunks[32];
    gsp_write_request writes[GS_DRAIN_MAX];
    gsp_player_info player;
    size_t n = 0u;
    size_t got;

    cfg.wire_ring = 32u;
    live = gs_srv_create(&cfg);
    if (live == NULL) {
        return;
    }
    memset(&player, 0, sizeof(player));
    player.handed = (uint8_t)GSP_HANDED_RIGHT;
    player.club = (uint8_t)GSP_CLUB_PT;
    GS_ASSERT_EQ(gsp_server_set_player(live, &player, gs_now), GSP_OK);
    (void)gsp_server_on_connection_opened(live, GS_CONN_A, NULL, gs_now);  /* → a 201 */
    GS_ASSERT_EQ(gs_srv_feed(live, "{\"DeviceID\":\"A\",\"ShotNumber\":1}"), GSP_OK);
    (void)gs_srv_writes(live, writes, GS_DRAIN_MAX);
    while (n < 32u && (got = gsp_server_poll_wire(live, chunks + n, 32u - n)) > 0u) {
        n += got;
    }
    GS_ASSERT_EQ(gsp_recorder_open(path, NULL, &rec), GSP_OK);
    if (rec == NULL) {
        gs_srv_free(live);
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_write(rec, chunks, n), GSP_OK);
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);
    gs_srv_free(live);

    /* A fresh server that was never told about a player. */
    again = gs_srv_create(NULL);
    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (again == NULL || rp == NULL) {
        if (rp != NULL) {
            gsp_replay_close(rp);
        }
        gs_srv_free(again);
        return;
    }
    memset(&report, 0, sizeof(report));
    GS_ASSERT_EQ(gsp_replay_into_server(rp, again, &report), GSP_OK);
    GS_ASSERT_EQ(report.replies_matched, 1u);       /* the 200 still matches      */
    GS_ASSERT_EQ(report.replies_unsolicited, 1u);   /* the 201 is explained       */
    GS_ASSERT_EQ(report.replies_missing, 0u);       /* ⚠ and NOT called missing   */
    GS_ASSERT_EQ(report.replies_differing, 0u);

    gsp_replay_close(rp);
    gs_srv_free(again);
    (void)remove(path);
}

GS_TEST(replay_gives_a_connection_slot_back_when_the_capture_closes_it)
{
    /* ⚠ FOUND BY A REAL RUN, NOT BY A HUNCH.  A sweep of 138 sessions against
     * gsplisten produced a capture with 133 connections — one after another,
     * never more than one at a time — and the replay tracked ids EVER SEEN
     * rather than ids currently OPEN.  Its table filled at sixteen, it re-opened
     * ids it had forgotten, it skipped their closes, it exhausted the server's
     * connection table, and it reported 31 of 133 messages with 101 replies
     * "missing": the capture read back as a DIFFERENT SESSION, which is the one
     * thing a capture must never do.
     *
     * So: more sequential connections than the replay's own table holds, into a
     * server that allows four at a time.  ⚠ `sessions` must stay above
     * GS_REPLAY_CONNS in record/gs_record.c — raising that constant without
     * raising this number would retire the case while it still passed. */
    const char *path = gs_tmp("sequential_connections");
    const unsigned sessions = 80u;
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *live;
    gsp_server *again;
    gsp_recorder *rec = NULL;
    gsp_replay *rp = NULL;
    gsp_replay_report report;
    gsp_wire_chunk chunks[16];
    gsp_write_request writes[GS_DRAIN_MAX];
    size_t got;
    unsigned i;
    char json[64];

    cfg.wire_ring = 16u;
    cfg.max_connections = 4u;      /* ⚠ FEWER than the sessions below */
    live = gs_srv_create(&cfg);
    if (live == NULL) {
        return;
    }
    GS_ASSERT_EQ(gsp_recorder_open(path, NULL, &rec), GSP_OK);
    if (rec == NULL) {
        gs_srv_free(live);
        return;
    }
    for (i = 0; i < sessions; ++i) {
        gsp_conn_id conn = (gsp_conn_id)(100u + i);
        (void)gsp_server_on_connection_opened(live, conn, NULL, gs_now);
        (void)snprintf(json, sizeof(json), "{\"DeviceID\":\"A\",\"ShotNumber\":%u}", i);
        GS_ASSERT_EQ(gsp_server_on_bytes(live, conn, (const uint8_t *)json, strlen(json),
                                         gs_now), GSP_OK);
        (void)gs_srv_writes(live, writes, GS_DRAIN_MAX);
        GS_ASSERT_EQ(gsp_server_on_connection_closed(live, conn, GSP_CLOSE_REMOTE_CLOSED,
                                                     gs_now), GSP_OK);
        /* ⚠ Drained and written every time round, as a host must: the ring is
         * drop-oldest and holds sixteen (design §3.4). */
        while ((got = gsp_server_poll_wire(live, chunks, 16u)) > 0u) {
            GS_ASSERT_EQ(gsp_recorder_write(rec, chunks, got), GSP_OK);
        }
    }
    GS_ASSERT_EQ(gsp_server_dropped_wire(live), 0u);   /* nothing was lost */
    GS_ASSERT_EQ(gsp_recorder_close(rec), GSP_OK);
    gs_srv_free(live);

    cfg = gsp_server_config_default();
    cfg.max_connections = 4u;
    again = gs_srv_create(&cfg);
    GS_ASSERT_EQ(gsp_replay_open(path, &rp), GSP_OK);
    if (again == NULL || rp == NULL) {
        if (rp != NULL) {
            gsp_replay_close(rp);
        }
        gs_srv_free(again);
        return;
    }
    memset(&report, 0, sizeof(report));
    GS_ASSERT_EQ(gsp_replay_into_server(rp, again, &report), GSP_OK);
    GS_ASSERT_EQ(report.connections, (uint64_t)sessions);
    GS_ASSERT_EQ(report.messages, (uint64_t)sessions);
    GS_ASSERT_EQ(report.replies_matched, (uint64_t)sessions);
    GS_ASSERT_EQ(report.replies_missing, 0u);
    GS_ASSERT_EQ(report.replies_differing, 0u);

    gsp_replay_close(rp);
    gs_srv_free(again);
    (void)remove(path);
}

GS_TEST_MAIN()
