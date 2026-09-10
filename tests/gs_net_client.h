/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_net_client.h — the CLIENT half of the CT-T cases, in C.
 *
 * ⚠ WHY THIS EXISTS AT ALL.  conformance §3.8's ten rows are about what a HOST
 * does with a real socket — reply latency, one write per message, holding a 200
 * apart from a 201, a client that vanishes mid-message.  The library owns no
 * socket by construction, so nothing in the rest of tests/ can reach them; they
 * need a listener and a client.  Package 4 gave the Python side both
 * (asyncio_transport.py and tools/gsp_shoot.py) and nine rows went green
 * against that ONE host adapter.  This is the same checklist pointed at the C
 * reference transport, which is the second adapter and a different machine's
 * worth of platform code (select/Winsock rather than asyncio).
 *
 * ⚠ SINGLE THREADED, AND THAT IS THE POINT.  The Python cases run the client on
 * a worker thread because asyncio's listener owns the main one.  Here both ends
 * are driven from one thread by hand — gs_net_pump() advances the listener and
 * every client in lockstep — so a failure is reproducible rather than a race,
 * there is no pthread dependency (this suite links NOTHING, gs_test.h §1), and
 * the same file compiles on Windows.
 *
 * ⚠ The client socket stays BLOCKING for writes and is polled for reads.  Every
 * payload here is under a kilobyte and fits a fresh socket's buffer, so a write
 * cannot block; a client that sent megabytes would need the other shape.
 */
#ifndef GS_NET_CLIENT_H
#define GS_NET_CLIENT_H

#include "gs_net_sys.h"     /* the platform shim gspro_net itself uses */

#include "gspro/gspro.h"
#include "gspro/net.h"
#include "gs_test.h"

#define GS_CLIENT_BUF_MAX  8192
/* ⚠ Replies beyond this are COUNTED but not stored (gs_client.dropped), so a
 * case that asks for more than it can keep fails on the count rather than
 * silently reporting the first sixteen as if they were all of them. */
#define GS_CLIENT_RESP_MAX 32

typedef struct gs_client {
    gs_sock  sock;
    bool     eof;                       /* the listener closed on us            */
    uint8_t  buf[GS_CLIENT_BUF_MAX];    /* bytes not yet framed                 */
    size_t   len;
    unsigned reads;                     /* recv()s that returned bytes — ⚠ this
                                         * is a KERNEL measurement, see CT-T02  */
    unsigned dropped;                   /* replies past GS_CLIENT_RESP_MAX       */
    gsp_response resp[GS_CLIENT_RESP_MAX];
    gsp_time_us  resp_us[GS_CLIENT_RESP_MAX]; /* when each was READ, monotonic  */
    size_t   resp_count;
} gs_client;

/* Connect to a listener on loopback.  Aborts on failure: a client that could
 * not connect is a broken test binary, not a failing conformance case. */
static inline void gs_client_open(gs_client *c, uint16_t port)
{
    struct sockaddr_in addr;
    memset(c, 0, sizeof(*c));
    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->sock == GS_SOCK_INVALID) {
        fprintf(stderr, "gs_net_client: socket() failed\n");
        abort();
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->sock, (const struct sockaddr *)&addr, (gs_socklen)sizeof(addr)) != 0) {
        char text[128];
        gs_sys_error_text(gs_sys_errno(), text, sizeof(text));
        fprintf(stderr, "gs_net_client: connect to 127.0.0.1:%u failed: %s\n",
                (unsigned)port, text);
        abort();
    }
    gs_sys_set_nodelay(c->sock);
    gs_sys_set_nosigpipe(c->sock);
}

static inline void gs_client_close(gs_client *c)
{
    if (c->sock != GS_SOCK_INVALID) {
        gs_sys_sock_close(c->sock);
        c->sock = GS_SOCK_INVALID;
    }
}

static inline void gs_client_send(gs_client *c, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32)
        int n = send(c->sock, p + sent, (int)(len - sent), GS_SYS_SEND_FLAGS);
#else
        ssize_t n = send(c->sock, p + sent, len - sent, GS_SYS_SEND_FLAGS);
#endif
        if (n <= 0) {
            if (n < 0 && gs_sys_would_block(gs_sys_errno())) {
                continue;
            }
            return;     /* the listener went away; the case will say so */
        }
        sent += (size_t)n;
    }
}

/* ------------------------------------------------------------------------ */
/* Reading replies                                                           */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THE CLIENT FRAMES THE SAME WAY EVERY REAL ONE MUST.  There is no framing on
 * the wire (protocol §2): a read may hold half a reply, one, or a 200 and a 201
 * back to back.  gsp_frame_find() is the library's own scanner, used here from
 * the OUTSIDE — which is also what makes a coalesced pair readable at all, and
 * is precisely what [PIT] does not do (protocol §9.8).
 */
static inline void gs_client_drain_(gs_client *c)
{
    for (;;) {
        size_t start = 0;
        size_t end = 0;
        gsp_status st = gsp_frame_find(c->buf, c->len, &start, &end);
        if (st != GSP_OK) {
            if (st == GSP_ERR_MALFORMED) {
                /* ⛔ Nothing the library can emit gets here: every write is one
                 * of five JSON objects (design §9.1).  Discarding the byte keeps
                 * the loop finite so a case FAILS on its assertion rather than
                 * hanging. */
                memmove(c->buf, c->buf + start + 1u, c->len - start - 1u);
                c->len -= start + 1u;
                continue;
            }
            return;
        }
        {
            gsp_response r;
            if (gsp_response_decode(c->buf + start, end - start, &r) != GSP_OK) {
                c->dropped++;               /* ⛔ not a shape the library emits */
            } else if (c->resp_count < GS_CLIENT_RESP_MAX) {
                c->resp[c->resp_count] = r;
                c->resp_us[c->resp_count] = gsp_net_now_us();
                c->resp_count++;
            } else {
                c->dropped++;
            }
        }
        memmove(c->buf, c->buf + end, c->len - end);
        c->len -= end;
    }
}

/* One non-blocking read.  Returns the bytes taken. */
static inline size_t gs_client_pump(gs_client *c)
{
    fd_set r;
    struct timeval tv;
    size_t space;
#if defined(_WIN32)
    int got;
#else
    ssize_t got;
#endif
    if (c->sock == GS_SOCK_INVALID || c->eof) {
        return 0u;
    }
    FD_ZERO(&r);
    FD_SET(c->sock, &r);
    gs_sys_timeval_set(&tv, 0);
#if defined(_WIN32)
    if (select(0, &r, NULL, NULL, &tv) <= 0) {
        return 0u;
    }
#else
    if (select((int)c->sock + 1, &r, NULL, NULL, &tv) <= 0) {
        return 0u;
    }
#endif
    space = sizeof(c->buf) - c->len;
    if (space == 0u) {
        return 0u;
    }
#if defined(_WIN32)
    got = recv(c->sock, (char *)c->buf + c->len, (int)space, 0);
#else
    got = recv(c->sock, c->buf + c->len, space, 0);
#endif
    if (got == 0) {
        c->eof = true;
        return 0u;
    }
    if (got < 0) {
        if (!gs_sys_would_block(gs_sys_errno())) {
            c->eof = true;
        }
        return 0u;
    }
    c->reads++;
    c->len += (size_t)got;
    gs_client_drain_(c);
    return (size_t)got;
}

#endif /* GS_NET_CLIENT_H */
