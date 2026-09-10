/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gswire — read, replay and mine a `.gswire` capture.
 *
 *     gswire stats   session.gswire
 *     gswire dump    session.gswire [--bytes] [--identifiers]
 *     gswire replay  session.gswire [--ack-text TEXT] [--reject-incomplete]
 *     gswire extract session.gswire 3 [out.json]
 *
 * ⚠ `replay` IS THE ONE THAT EARNS THE FORMAT.  It feeds a recorded session
 * back through a server built by THIS library and reports what it made of it
 * this time — every message, every finding, and every reply compared byte for
 * byte with the reply that was actually sent.  When protocol §11's U2, U4 or U7
 * is finally settled by a launch monitor on a mat, that answers "would the fix
 * have helped?" without the launch monitor, the mat, or the person who owns
 * them (design §7, §11 package 7).
 *
 * ⚠ `extract` IS HOW A CAPTURE BECOMES A FIXTURE.  tests/fixtures/ holds one
 * byte-exact message per client, hand-written from each client's serialiser
 * because nothing here has met a wire; the first real capture is where those
 * stop being transcriptions.  A fixture promoted this way must be REDACTED
 * first — check the header line `identifiers=` that `stats` prints.
 */
#include "gspro/gspro.h"
#include "gspro/record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *direction_name(uint8_t d)
{
    switch ((gsp_wire_direction)d) {
    case GSP_WIRE_CLIENT_TO_SERVER: return "C->S";
    case GSP_WIRE_SERVER_TO_CLIENT: return "S->C";
    case GSP_WIRE_META:             return "meta";
    default:                        return "????";
    }
}

static void print_flags(uint8_t flags)
{
    printf("%s%s%s",
           ((flags & (uint8_t)GSP_WIRE_REDACTED) != 0u) ? "R" : "-",
           ((flags & (uint8_t)GSP_WIRE_LOST) != 0u) ? "L" : "-",
           ((flags & (uint8_t)GSP_WIRE_CONTINUES) != 0u) ? "+" : "-");
}

/* ⚠ Payloads are UNTRUSTED BYTES from a socket (design §9.3), and a terminal
 * takes escape sequences as instructions.  Everything outside printable ASCII
 * becomes \xHH before it reaches a screen. */
static void print_payload(const uint8_t *data, size_t len, size_t limit)
{
    size_t i;
    for (i = 0; i < len && i < limit; ++i) {
        uint8_t b = data[i];
        if (b >= 0x20u && b < 0x7fu && b != '\\') {
            (void)putchar((int)b);
        } else if (b == '\\') {
            (void)fputs("\\\\", stdout);
        } else if (b == '\n') {
            (void)fputs("\\n", stdout);
        } else if (b == '\r') {
            (void)fputs("\\r", stdout);
        } else if (b == '\t') {
            (void)fputs("\\t", stdout);
        } else {
            printf("\\x%02x", (unsigned)b);
        }
    }
    if (len > limit) {
        printf(" … (%zu more)", len - limit);
    }
}

static void print_header(const gsp_replay *rp, const char *path)
{
    const gsp_recording_info *info = gsp_replay_info(rp);
    printf("%s\n", path);
    printf("  library     %s\n", info->library);
    printf("  clock       %s\n", info->clock);
    printf("  port        %u\n", (unsigned)info->port);
    /* ⚠ THE LINE THAT DECIDES WHETHER THIS FILE CAN BE SHARED.  A capture taken
     * with identifiers recorded carries a peer address and possibly a hardware
     * serial (design §9.2). */
    printf("  identifiers %s%s\n", info->identifiers_recorded ? "RECORDED" : "redacted",
           info->identifiers_recorded
               ? "  ⚠ this capture carries a peer address and any DeviceID verbatim"
               : "");
    if (info->note[0] != '\0') {
        printf("  note        %s\n", info->note);
    }
}

static int fail(const char *what, const char *detail)
{
    fprintf(stderr, "⛔ %s%s%s\n", what, (detail != NULL && detail[0] != '\0') ? ": " : "",
            (detail != NULL) ? detail : "");
    return 1;
}

/* ------------------------------------------------------------------------ */
static int cmd_dump(const char *path, bool whole, bool identifiers)
{
    gsp_replay *rp = NULL;
    gsp_wire_chunk chunks[16];
    size_t n;
    uint64_t index = 0u;
    gsp_status st = gsp_replay_open(path, &rp);

    if (st < GSP_OK) {
        return fail("cannot read the capture", gsp_status_str(st));
    }
    print_header(rp, path);
    if (gsp_replay_info(rp)->identifiers_recorded && !identifiers) {
        printf("  ⚠ printing anyway is what --identifiers is for; the file is not "
               "redacted\n");
    }
    printf("\n  #     time_us  conn  dir   flg  len  payload\n");
    while ((n = gsp_replay_read(rp, chunks, 16u)) > 0u) {
        size_t i;
        for (i = 0; i < n; ++i) {
            const gsp_wire_chunk *c = &chunks[i];
            printf("%4llu %11lld %5u  %s  ", (unsigned long long)index++,
                   (long long)c->host_time_us, (unsigned)c->conn,
                   direction_name(c->direction));
            print_flags(c->flags);
            printf("  %3u  ", (unsigned)c->length);
            print_payload(c->data, (size_t)c->length, whole ? (size_t)GSP_WIRE_CHUNK_MAX : 96u);
            printf("\n");
        }
    }
    st = gsp_replay_status(rp);
    printf("\n%llu chunk(s)\n", (unsigned long long)gsp_replay_chunks_read(rp));
    if (st < GSP_OK) {
        (void)fail("the capture ends badly", gsp_replay_error(rp));
    }
    gsp_replay_close(rp);
    return (st < GSP_OK) ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
static int cmd_stats(const char *path)
{
    gsp_replay *rp = NULL;
    gsp_wire_chunk chunks[16];
    size_t n;
    uint64_t client = 0u;
    uint64_t server = 0u;
    uint64_t meta = 0u;
    uint64_t lost = 0u;
    uint64_t redacted = 0u;
    uint64_t bytes = 0u;
    gsp_time_us first = 0;
    gsp_time_us last = 0;
    bool have_time = false;
    gsp_conn_id conns[16];
    size_t conn_count = 0u;
    gsp_status st = gsp_replay_open(path, &rp);

    if (st < GSP_OK) {
        return fail("cannot read the capture", gsp_status_str(st));
    }
    print_header(rp, path);
    while ((n = gsp_replay_read(rp, chunks, 16u)) > 0u) {
        size_t i;
        for (i = 0; i < n; ++i) {
            const gsp_wire_chunk *c = &chunks[i];
            size_t k;
            bool known = false;
            bytes += (uint64_t)c->length;
            if (!have_time) {
                first = c->host_time_us;
                have_time = true;
            }
            last = c->host_time_us;
            if ((c->flags & (uint8_t)GSP_WIRE_LOST) != 0u) {
                lost++;
            }
            if ((c->flags & (uint8_t)GSP_WIRE_REDACTED) != 0u) {
                redacted++;
            }
            switch ((gsp_wire_direction)c->direction) {
            case GSP_WIRE_CLIENT_TO_SERVER: client++; break;
            case GSP_WIRE_SERVER_TO_CLIENT: server++; break;
            case GSP_WIRE_META:             meta++;   break;
            default: break;
            }
            for (k = 0; k < conn_count; ++k) {
                known = known || (conns[k] == c->conn);
            }
            if (!known && conn_count < 16u && c->conn != GSP_CONN_NONE) {
                conns[conn_count++] = c->conn;
            }
        }
    }
    printf("\n  chunks      %llu  (%llu client, %llu server, %llu meta)\n",
           (unsigned long long)gsp_replay_chunks_read(rp), (unsigned long long)client,
           (unsigned long long)server, (unsigned long long)meta);
    printf("  payload     %llu byte(s)\n", (unsigned long long)bytes);
    printf("  connections %zu\n", conn_count);
    printf("  redacted    %llu chunk(s)\n", (unsigned long long)redacted);
    /* ⚠ A capture with holes is still evidence, but not of ABSENCE: what is not
     * in it may have been dropped rather than never sent (design §3.4). */
    printf("  lost        %llu chunk(s)%s\n", (unsigned long long)lost,
           (lost > 0u) ? "  ⚠ the wire ring overflowed; this capture has holes" : "");
    if (have_time) {
        printf("  span        %.3f s\n", (double)(last - first) / 1000000.0);
    }
    st = gsp_replay_status(rp);
    if (st < GSP_OK) {
        (void)fail("the capture ends badly", gsp_replay_error(rp));
    }
    gsp_replay_close(rp);
    return (st < GSP_OK) ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
static int cmd_replay(const char *path, const char *ack_text, bool reject_incomplete)
{
    gsp_server_config cfg = gsp_server_config_default();
    gsp_server *server = NULL;
    gsp_replay *rp = NULL;
    gsp_replay_report report;
    gsp_status st = gsp_replay_open(path, &rp);
    unsigned bit;

    if (st < GSP_OK) {
        return fail("cannot read the capture", gsp_status_str(st));
    }
    if (ack_text != NULL) {
        (void)snprintf(cfg.policy.ack_text, sizeof(cfg.policy.ack_text), "%s", ack_text);
    }
    cfg.policy.reject_incomplete_shots = reject_incomplete;
    /* ⚠ Rings big enough that the REPORT is about the capture rather than about
     * this program failing to keep up. */
    cfg.event_ring = 4096u;
    cfg.write_ring = 256u;
    cfg.max_connections = 16u;
    if (gsp_server_create(&cfg, &server) < GSP_OK) {
        gsp_replay_close(rp);
        return fail("the library refused this configuration", NULL);
    }
    print_header(rp, path);

    memset(&report, 0, sizeof(report));
    st = gsp_replay_into_server(rp, server, &report);

    printf("\n  chunks      %llu  (%llu client, %llu server, %llu meta)\n",
           (unsigned long long)report.chunks, (unsigned long long)report.client_chunks,
           (unsigned long long)report.server_chunks, (unsigned long long)report.meta_chunks);
    printf("  connections %u\n", report.connections);
    printf("  messages    %llu  (%llu shot(s))\n", (unsigned long long)report.messages,
           (unsigned long long)report.shots);
    printf("  errors      %llu protocol error(s)\n",
           (unsigned long long)report.protocol_errors);
    printf("  replies     %llu produced, %llu matched, %llu DIFFERING, %llu missing, "
           "%llu extra\n",
           (unsigned long long)report.replies_produced,
           (unsigned long long)report.replies_matched,
           (unsigned long long)report.replies_differing,
           (unsigned long long)report.replies_missing,
           (unsigned long long)report.replies_extra);
    if (report.replies_unsolicited > 0u) {
        /* ⚠ Not a finding: the host originates a 201/202/203 from its own game
         * state, and a capture holds the bytes rather than the state. */
        printf("              %llu unsolicited (201/202/203) in the recording that this\n"
               "              server was never given the game state to produce\n",
               (unsigned long long)report.replies_unsolicited);
    }
    if (report.lost_chunks > 0u) {
        printf("  ⚠ %llu chunk(s) carry LOST: this capture has holes\n",
               (unsigned long long)report.lost_chunks);
    }
    /* ⚠ THE FINDINGS ARE WHAT A CAPTURE IS FOR.  Each flag names one thing this
     * device's messages did that protocol.md had to guess about (design §11). */
    if (report.flags_seen != 0u) {
        printf("  findings   ");
        for (bit = 0; bit < 32u; ++bit) {
            uint32_t flag = 1u << bit;
            const char *name;
            if ((report.flags_seen & flag) == 0u) {
                continue;
            }
            name = gsp_message_flag_name((gsp_message_flag)flag);
            if (name != NULL) {
                printf(" %s", name);
            }
        }
        printf("\n");
    }
    if (report.replies_differing > 0u) {
        printf("\n⚠ %llu reply(s) differ from what was sent at the time.  After a\n"
               "  deliberate change that is the POINT of this run; otherwise it is a\n"
               "  regression against a real session.\n",
               (unsigned long long)report.replies_differing);
    }

    gsp_server_close(server);
    gsp_server_destroy(server);
    gsp_replay_close(rp);
    if (st < GSP_OK) {
        return fail("the capture ends badly", gsp_status_str(st));
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
static int cmd_extract(const char *path, uint64_t want, const char *out_path)
{
    gsp_replay *rp = NULL;
    gsp_wire_chunk chunk;
    uint64_t index = 0u;
    gsp_status st = gsp_replay_open(path, &rp);

    if (st < GSP_OK) {
        return fail("cannot read the capture", gsp_status_str(st));
    }
    while (gsp_replay_read(rp, &chunk, 1u) == 1u) {
        if (index++ != want) {
            continue;
        }
        if ((chunk.flags & (uint8_t)GSP_WIRE_CONTINUES) != 0u) {
            /* ⚠ Said out loud rather than silently writing half a message: a
             * fixture that is most of an object is worse than none, because it
             * looks like evidence about a client that never sent it. */
            fprintf(stderr, "⚠ chunk %llu CONTINUES into the next one; this writes only "
                            "the first %u bytes\n",
                    (unsigned long long)want, (unsigned)chunk.length);
        }
        if (out_path != NULL) {
            FILE *f = fopen(out_path, "wb");
            if (f == NULL) {
                gsp_replay_close(rp);
                return fail("cannot write", out_path);
            }
            (void)fwrite(chunk.data, 1u, (size_t)chunk.length, f);
            if (fclose(f) != 0) {
                gsp_replay_close(rp);
                return fail("cannot write", out_path);
            }
            fprintf(stderr, "wrote %u byte(s) to %s\n", (unsigned)chunk.length, out_path);
        } else {
            (void)fwrite(chunk.data, 1u, (size_t)chunk.length, stdout);
        }
        gsp_replay_close(rp);
        return 0;
    }
    gsp_replay_close(rp);
    return fail("no such chunk", path);
}

/* ------------------------------------------------------------------------ */
static void usage(const char *argv0)
{
    printf(
"usage: %s <command> FILE [options]\n"
"\n"
"  stats   FILE                       what the capture holds\n"
"  dump    FILE [--bytes]             every chunk, one per line\n"
"  replay  FILE [--ack-text TEXT]     ⚠ drive it back through THIS library and\n"
"                [--reject-incomplete]  report what it makes of it now\n"
"  extract FILE N [OUT]               one chunk's payload, raw — how a capture\n"
"                                     becomes a fixture.  ⚠ Redact first\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *cmd;
    const char *path;
    bool whole = false;
    bool identifiers = false;
    bool reject_incomplete = false;
    const char *ack_text = NULL;
    int i;

    if (argc < 3) {
        usage(argv[0]);
        return (argc > 1 && strcmp(argv[1], "--help") == 0) ? 0 : 2;
    }
    cmd = argv[1];
    path = argv[2];

    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--bytes") == 0) {
            whole = true;
        } else if (strcmp(argv[i], "--identifiers") == 0) {
            identifiers = true;
        } else if (strcmp(argv[i], "--reject-incomplete") == 0) {
            reject_incomplete = true;
        } else if (strcmp(argv[i], "--ack-text") == 0 && i + 1 < argc) {
            ack_text = argv[++i];
        } else if (strcmp(cmd, "extract") != 0) {
            fprintf(stderr, "⛔ unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    if (strcmp(cmd, "dump") == 0) {
        return cmd_dump(path, whole, identifiers);
    }
    if (strcmp(cmd, "stats") == 0) {
        return cmd_stats(path);
    }
    if (strcmp(cmd, "replay") == 0) {
        return cmd_replay(path, ack_text, reject_incomplete);
    }
    if (strcmp(cmd, "extract") == 0) {
        if (argc < 4) {
            usage(argv[0]);
            return 2;
        }
        return cmd_extract(path, strtoull(argv[3], NULL, 10), (argc > 4) ? argv[4] : NULL);
    }
    usage(argv[0]);
    return 2;
}
