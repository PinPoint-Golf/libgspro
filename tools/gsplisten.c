/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gsplisten — a complete GSPro Connect listener in C, and the C twin of
 * tools/gsp_listen.py.
 *
 * Binds, prints every event, and can tell the connected launch monitors which
 * club the player has and whether a round is running.  ⚠ THIS IS WHAT TO RUN
 * WITH A REAL DEVICE ON THE MAT before touching an application: it answers
 * "does this connector talk to us, and what exactly does it send" with nothing
 * else in the way.  It needs no Python, which is the reason it exists beside
 * gsp_listen.py — a PiTrac, a NAS or a Windows box with a compiler and nothing
 * else can still meet a launch monitor.
 *
 *     gsplisten                                 # 127.0.0.1:921
 *     gsplisten --host 0.0.0.0 --port 9210      # ⚠ a device on another machine
 *     gsplisten --club PT --distance 4.2 --handed RH
 *     gsplisten --identifiers --idle-alarm 30
 *
 * ⚠ IDENTIFIERS ARE REDACTED BY DEFAULT.  A peer address identifies a household
 * and a DeviceID can carry a hardware serial ([GC2]).  --identifiers prints
 * them; design §9.2 is why that is not the default, and why a capture taken to
 * answer protocol §11's questions must be redacted before it becomes a fixture.
 *
 * ⚠ IT IS MEANT TO BE COPIED.  The loop in run() is the whole host contract —
 * poll, drain events, act on the ones that ask for an action — and a Qt or
 * Winsock application writes the same shape against the same server.
 */
#include "gspro/gspro.h"
#include "gspro/net.h"
#include "gspro/record.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ⚠ The ONLY thing the signal handler may touch.  Everything else happens on
 * the one thread the server contract allows (server.h). */
static volatile sig_atomic_t gs_running = 1;

static void gs_on_signal(int sig)
{
    (void)sig;
    gs_running = 0;
}

typedef struct gs_options {
    const char *host;
    long        port;
    const char *club;
    const char *handed;
    const char *surface;
    const char *ack_text;
    double      distance;
    bool        has_distance;
    bool        identifiers;
    bool        session_active;
    bool        reject_incomplete;
    double      idle_alarm_s;
    double      write_spacing_s;
    long        max_connections;
    long        stop_after_shots;   /* 0 → until ctrl-c */
    const char *record_path;        /* ⚠ a .gswire capture; off unless asked   */
    const char *record_note;
} gs_options;

static void gs_usage(const char *argv0)
{
    printf(
"usage: %s [options]\n"
"\n"
"  --host ADDR             interface to bind (default 127.0.0.1)\n"
"                          ⚠ 0.0.0.0 to accept a device on ANOTHER MACHINE\n"
"  --port N                default %d; %d is GSPro's own alternative\n"
"  --identifiers           ⚠ print peer addresses and DeviceIDs unredacted\n"
"  --club CODE             DR, W2-W7, H2-H7, I1-I9, PW, GW, SW, LW, PT\n"
"  --handed RH|LH\n"
"  --distance N            ⚠ [OSP] arms its device on a NON-ZERO value\n"
"  --surface TEXT          e.g. \"Green\" — [OSP] only; vocabulary unknown\n"
"  --session-active        send a 202 — [OSP]-style clients do not arm without one\n"
"  --idle-alarm SECONDS    report a client that has gone quiet.  ⚠ Never closes it\n"
"  --write-spacing SECONDS hold writes apart — ⚠ [PIT]'s thread dies on {200}{201}\n"
"  --reject-incomplete     ⚠ answer an incomplete shot 501; a client may retry forever\n"
"  --ack-text TEXT         ⚠ the real GSPro misspells it (protocol U4)\n"
"  --max-connections N     0 → the library default\n"
"  --shots N               exit after N shots (for scripts); 0 → until ctrl-c\n"
"  --record FILE           ⚠ capture the BYTES to a .gswire file — what package 7\n"
"                          is for.  Identifiers are redacted unless --identifiers\n"
"  --note TEXT             a line in the capture's header: which device, which\n"
"                          question it was taken to answer\n"
"  --help\n",
        argv0, GSP_DEFAULT_PORT, GSP_ALT_PORT);
}

/* ------------------------------------------------------------------------ */
/* Arguments                                                                 */
/* ------------------------------------------------------------------------ */
static bool gs_need_value(int argc, int i, const char *what)
{
    if (i + 1 >= argc) {
        fprintf(stderr, "⛔ %s needs a value\n", what);
        return false;
    }
    return true;
}

static bool gs_parse_args(int argc, char **argv, gs_options *o, bool *done)
{
    int i;
    *done = false;
    memset(o, 0, sizeof(*o));
    o->host = "127.0.0.1";
    o->port = GSP_DEFAULT_PORT;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            gs_usage(argv[0]);
            *done = true;
            return true;
        } else if (strcmp(a, "--host") == 0 && gs_need_value(argc, i, a)) {
            o->host = argv[++i];
        } else if (strcmp(a, "--port") == 0 && gs_need_value(argc, i, a)) {
            o->port = strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--identifiers") == 0) {
            o->identifiers = true;
        } else if (strcmp(a, "--club") == 0 && gs_need_value(argc, i, a)) {
            o->club = argv[++i];
        } else if (strcmp(a, "--handed") == 0 && gs_need_value(argc, i, a)) {
            o->handed = argv[++i];
        } else if (strcmp(a, "--distance") == 0 && gs_need_value(argc, i, a)) {
            o->distance = strtod(argv[++i], NULL);
            o->has_distance = true;
        } else if (strcmp(a, "--surface") == 0 && gs_need_value(argc, i, a)) {
            o->surface = argv[++i];
        } else if (strcmp(a, "--session-active") == 0) {
            o->session_active = true;
        } else if (strcmp(a, "--idle-alarm") == 0 && gs_need_value(argc, i, a)) {
            o->idle_alarm_s = strtod(argv[++i], NULL);
        } else if (strcmp(a, "--write-spacing") == 0 && gs_need_value(argc, i, a)) {
            o->write_spacing_s = strtod(argv[++i], NULL);
        } else if (strcmp(a, "--reject-incomplete") == 0) {
            o->reject_incomplete = true;
        } else if (strcmp(a, "--ack-text") == 0 && gs_need_value(argc, i, a)) {
            o->ack_text = argv[++i];
        } else if (strcmp(a, "--max-connections") == 0 && gs_need_value(argc, i, a)) {
            o->max_connections = strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--shots") == 0 && gs_need_value(argc, i, a)) {
            o->stop_after_shots = strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--record") == 0 && gs_need_value(argc, i, a)) {
            o->record_path = argv[++i];
        } else if (strcmp(a, "--note") == 0 && gs_need_value(argc, i, a)) {
            o->record_note = argv[++i];
        } else {
            fprintf(stderr, "⛔ unknown or incomplete option: %s\n", a);
            gs_usage(argv[0]);
            return false;
        }
    }
    if (o->port < 0 || o->port > 65535) {
        fprintf(stderr, "⛔ --port %ld is not a port\n", o->port);
        return false;
    }
    return true;
}

/*
 * ⚠ UNKNOWN MEMBERS ARE OMITTED FROM THE JSON RATHER THAN INVENTED, and a
 * player with nothing known sends nothing at all (design §5.6).
 */
static bool gs_build_player(const gs_options *o, gsp_player_info *out, bool *have)
{
    gsp_handed handed = (o->handed != NULL) ? gsp_handed_parse(o->handed) : GSP_HANDED_UNKNOWN;
    gsp_club club = (o->club != NULL) ? gsp_club_parse(o->club) : GSP_CLUB_UNKNOWN;

    memset(out, 0, sizeof(*out));
    *have = false;
    if (o->club != NULL && club == GSP_CLUB_UNKNOWN) {
        fprintf(stderr, "⚠ '%s' is not a club code.  The vocabulary is DR, W2-W7, "
                        "H2-H7, I1-I9, PW, GW, SW, LW, PT (protocol §5.2).\n", o->club);
        return false;
    }
    if (o->handed != NULL && handed == GSP_HANDED_UNKNOWN) {
        fprintf(stderr, "⚠ '%s' is not a handedness.  RH or LH.\n", o->handed);
        return false;
    }
    if (handed == GSP_HANDED_UNKNOWN && club == GSP_CLUB_UNKNOWN && !o->has_distance
        && (o->surface == NULL || o->surface[0] == '\0')) {
        return true;                    /* nothing to say; say nothing */
    }
    out->handed = (uint8_t)handed;
    out->club = (uint8_t)club;
    out->has_distance = o->has_distance ? 1u : 0u;
    out->distance_to_target = o->distance;
    if (o->surface != NULL) {
        (void)snprintf(out->surface, sizeof(out->surface), "%s", o->surface);
    }
    *have = true;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */
static void gs_print_findings(const gsp_message *m)
{
    unsigned bit;
    bool any = false;
    for (bit = 0; bit < 32u; ++bit) {
        uint32_t flag = 1u << bit;
        const char *name;
        if ((m->flags & flag) == 0u) {
            continue;
        }
        name = gsp_message_flag_name((gsp_message_flag)flag);
        if (name == NULL) {
            continue;
        }
        if (!any) {
            /* ⚠ THE FLAGS ARE THE POINT OF A FIRST SESSION WITH REAL HARDWARE:
             * they say which of protocol §11's unknowns this device just
             * answered (design §11 package 7). */
            printf("    findings:");
            any = true;
        }
        printf(" %s", name);
    }
    if (any) {
        printf("\n");
    }
}

static void gs_handle_events(gsp_server *server, gsp_net *net, const gs_options *o,
                             unsigned long *shots)
{
    gsp_event ev[16];
    size_t n;
    while ((n = gsp_server_poll_events(server, ev, 16)) > 0u) {
        size_t i;
        for (i = 0; i < n; ++i) {
            char line[512];
            const char *mark = "  ";
            switch ((gsp_event_type)ev[i].type) {
            case GSP_EV_SHOT:
                (*shots)++;
                break;
            case GSP_EV_PROTOCOL_ERROR:
            case GSP_EV_WARNING:
                mark = "⚠ ";
                break;
            case GSP_EV_CLOSE_REQUESTED:
                mark = "⚠ ";
                /* ⚠ THE LIBRARY CANNOT CLOSE ANYTHING.  policy.
                 * protocol_error_close_threshold asks the HOST to, and this one
                 * line is the whole of acting on it (design §5.1). */
                (void)gsp_net_close_connection(net, ev[i].conn);
                break;
            case GSP_EV_NONE:
            case GSP_EV_CONNECTION_OPENED:
            case GSP_EV_CONNECTION_CLOSED:
            case GSP_EV_CLIENT_IDENTIFIED:
            case GSP_EV_HEARTBEAT:
            case GSP_EV_STATUS:
            case GSP_EV_CLIENT_STATE:
            case GSP_EV_PLAYER_INFO_SENT:
            case GSP_EV_SESSION_STATE_SENT:
            case GSP_EV_CLIENT_IDLE:
            case GSP_EV_CLIENT_ACTIVE:
            case GSP_EVENT_TYPE_COUNT:
            default:
                break;
            }
            (void)gsp_event_format(&ev[i], line, sizeof(line), o->identifiers);
            printf("%s%s\n", mark, line);
            if (ev[i].type == (uint8_t)GSP_EV_SHOT || ev[i].type == (uint8_t)GSP_EV_HEARTBEAT
                || ev[i].type == (uint8_t)GSP_EV_STATUS) {
                gs_print_findings(&ev[i].u.message);
            }
            fflush(stdout);
        }
    }
}

/* ------------------------------------------------------------------------ */
/*
 * Drain the wire ring into the capture.  ⚠ EVERY POLL, not just at the end: the
 * ring is drop-oldest (design §3.4), so a host that drains it lazily is a host
 * that loses the beginning of the session it is recording — and the beginning
 * is where the connect handshake and the first shot are.
 */
static void gs_drain_wire(gsp_server *server, gsp_recorder *rec, unsigned long *dropped)
{
    gsp_wire_chunk chunks[16];
    size_t n;

    if (rec == NULL) {
        return;
    }
    while ((n = gsp_server_poll_wire(server, chunks, 16u)) > 0u) {
        if (gsp_recorder_write(rec, chunks, n) < GSP_OK) {
            /* ⚠ Said once, loudly, and the session continues: a launch monitor
             * on a mat is not repeatable, so a full disk must not also cost the
             * shots the operator is still hitting. */
            if (*dropped == 0u) {
                fprintf(stderr, "⚠ the capture cannot be written: %s\n",
                        gsp_recorder_error(rec));
            }
            (*dropped)++;
        }
    }
}

static int gs_run(const gs_options *o)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_net_config net_cfg = gsp_net_config_default();
    gsp_server *server = NULL;
    gsp_net *net = NULL;
    gsp_player_info player;
    bool have_player = false;
    char error[GSP_NET_ERROR_MAX];
    unsigned long shots = 0;
    unsigned long capture_failures = 0;
    gsp_recorder *rec = NULL;
    gsp_net_stats stats;

    if (!gs_build_player(o, &player, &have_player)) {
        return 2;
    }
    if (o->max_connections > 0) {
        cfg.max_connections = (uint32_t)o->max_connections;
    }
    cfg.policy.idle_alarm_us = (gsp_time_us)(o->idle_alarm_s * 1000000.0);
    cfg.policy.write_spacing_us = (gsp_time_us)(o->write_spacing_s * 1000000.0);
    cfg.policy.reject_incomplete_shots = o->reject_incomplete;
    if (o->ack_text != NULL) {
        (void)snprintf(cfg.policy.ack_text, sizeof(cfg.policy.ack_text), "%s", o->ack_text);
    }
    if (o->record_path != NULL) {
        /* ⚠ THE RING IS OFF UNLESS A CAPTURE IS ASKED FOR (design §7), so this
         * is where it is sized.  256 chunks is ~140 KB and holds a burst of
         * shots comfortably; the drain below runs every poll regardless. */
        cfg.wire_ring = GSP_WIRE_RING_RECOMMENDED;
        cfg.policy.record_identifiers = o->identifiers;
    }
    if (gsp_server_create(&cfg, &server) < GSP_OK) {
        fprintf(stderr, "⛔ the library refused this configuration\n");
        return 1;
    }

    net_cfg.host = o->host;
    net_cfg.port = (uint16_t)o->port;
    net_cfg.ephemeral_port = (o->port == 0);
    if (o->max_connections > 0) {
        net_cfg.max_connections = (uint16_t)o->max_connections;
    }
    error[0] = '\0';
    if (gsp_net_open(server, &net_cfg, &net, error, sizeof(error)) < GSP_OK) {
        fprintf(stderr, "⛔ cannot bind %s:%ld: %s\n", o->host, o->port, error);
        /* ⚠ Port 921 is very often ALREADY HELD — by GSPro itself, on the
         * machine this is most likely to run on (design §6.4).  Say so, rather
         * than leaving a user to guess at "Address already in use". */
        if (o->port == GSP_DEFAULT_PORT) {
            fprintf(stderr,
                    "   GSPro Connect itself listens here.  It can be moved to %d with\n"
                    "   <OpenAPIUseAltPort>true</OpenAPIUseAltPort> in "
                    "GSPconnect.exe.config ([SLX]).\n", GSP_ALT_PORT);
        }
        gsp_server_close(server);
        gsp_server_destroy(server);
        return 1;
    }

    if (o->record_path != NULL) {
        gsp_recording_info info = gsp_recording_info_default();
        info.port = gsp_net_port(net);
        info.identifiers_recorded = o->identifiers;
        if (o->record_note != NULL) {
            (void)snprintf(info.note, sizeof(info.note), "%s", o->record_note);
        }
        if (gsp_recorder_open(o->record_path, &info, &rec) < GSP_OK) {
            fprintf(stderr, "⛔ cannot write the capture %s\n", o->record_path);
            gsp_net_close(net);
            gsp_server_close(server);
            gsp_server_destroy(server);
            return 1;
        }
    }

    printf("listening on %s:%u   (identifiers %s)\n", o->host, (unsigned)gsp_net_port(net),
           o->identifiers ? "SHOWN" : "redacted");
    if (rec != NULL) {
        /* ⚠ A capture taken with --identifiers carries a peer address and any
         * DeviceID verbatim; design §9.2 is why that must be a decision. */
        printf("recording to %s   (identifiers %s)\n", o->record_path,
               o->identifiers ? "RECORDED — ⚠ redact before sharing" : "redacted");
    }
    if (strcmp(o->host, "127.0.0.1") == 0 || strcmp(o->host, "localhost") == 0) {
        printf("⚠ loopback only — a launch monitor on ANOTHER MACHINE cannot reach "
               "this.  Use --host 0.0.0.0 (design §6.1).\n");
    }

    if (have_player) {
        (void)gsp_server_set_player(server, &player, gsp_net_now_us());
        printf("player: handed=%s club=%s", gsp_handed_text((gsp_handed)player.handed),
               gsp_club_code((gsp_club)player.club));
        if (player.has_distance != 0u) {
            printf(" distance=%g", player.distance_to_target);
        }
        printf("\n");
    }
    if (o->session_active) {
        (void)gsp_server_set_session_state(server, GSP_SESSION_ACTIVE, gsp_net_now_us());
        printf("session: ACTIVE (202 sent — [OSP]-style clients arm on this)\n");
    }
    fflush(stdout);

    /* ⚠ THE WHOLE HOST CONTRACT, and there is no more to it than this.  Poll,
     * drain, act.  The timeout is a courtesy to ctrl-c: gsp_net_poll() clamps
     * its own wait to whatever the idle alarm or write spacing made due. */
    while (gs_running != 0) {
        if (gsp_net_poll(net, 250) < GSP_OK) {
            break;
        }
        gs_handle_events(server, net, o, &shots);
        gs_drain_wire(server, rec, &capture_failures);
        if (o->stop_after_shots > 0 && shots >= (unsigned long)o->stop_after_shots) {
            break;
        }
    }

    stats = gsp_net_get_stats(net);
    gsp_net_close(net);
    gsp_server_close(server);
    gs_handle_events(server, NULL, o, &shots);   /* ⚠ drain once more: design §3.3 */
    gs_drain_wire(server, rec, &capture_failures);
    if (rec != NULL) {
        uint64_t written = gsp_recorder_chunks(rec);
        gsp_status closed = gsp_recorder_close(rec);
        printf("capture: %llu chunk(s) to %s%s\n", (unsigned long long)written,
               o->record_path,
               (closed < GSP_OK) ? "  ⛔ INCOMPLETE — the file could not be finished" : "");
        if (gsp_server_dropped_wire(server) > 0u) {
            /* ⚠ A capture with holes is still evidence, but not of ABSENCE. */
            printf("⚠ %u chunk(s) were dropped before they could be written; the "
                   "capture has holes\n", gsp_server_dropped_wire(server));
        }
    }
    printf("\n%lu shot(s) received; %llu read(s) in, %llu write(s) out, "
           "%u event(s) dropped\n", shots, (unsigned long long)stats.reads,
           (unsigned long long)stats.writes, gsp_server_dropped_events(server));
    gsp_server_destroy(server);
    return 0;
}

int main(int argc, char **argv)
{
    gs_options o;
    bool done = false;

    if (!gs_parse_args(argc, argv, &o, &done)) {
        return 2;
    }
    if (done) {
        return 0;
    }
    (void)signal(SIGINT, gs_on_signal);
#if defined(SIGTERM)
    (void)signal(SIGTERM, gs_on_signal);
#endif
#if defined(SIGPIPE)
    /* ⚠ Only where the transport cannot do better per socket or per send: it
     * uses SO_NOSIGPIPE on BSD/macOS and MSG_NOSIGNAL on Linux, and a library
     * must not change a process's signal disposition behind its host's back.
     * An APPLICATION may, and here one does. */
    (void)signal(SIGPIPE, SIG_IGN);
#endif
    return gs_run(&o);
}
