/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/net.h — the REFERENCE socket transport.  Layer 3 of design §3.1, and
 * ⚠ NOBODY HAS TO LINK IT.
 *
 * ============================================================================
 * WHAT THIS IS, AND WHY IT IS A SEPARATE TARGET
 * ============================================================================
 * The core owns no socket by construction (design §2): the choice of socket
 * API is a platform and framework decision that belongs to the host, and a
 * library that made it would be un-embeddable everywhere it was not chosen.
 * PinPoint Studio will drive `gsp_server` from QTcpServer; a Python host does
 * it from asyncio (python/gspro/asyncio_transport.py).  This file is the third
 * such host — a POSIX `select()` / Winsock loop — and it exists for two
 * reasons only:
 *
 *   1. a non-Qt, non-Python user has something that runs out of the box, and
 *   2. `gsplisten` and the CT-T host-transport cases (conformance §3.8) have a
 *      socket in C to run against.
 *
 * It lives in the `gspro_net` target behind `GS_BUILD_NET`.  ⚠ Linking it is
 * the exception; `tests/purity.cmake` runs on `gspro`, which is what every
 * consumer links, and this object is exactly the thing that would fail it.
 *
 * ============================================================================
 * THE SHAPE: STILL POLL, STILL NO CALLBACKS
 * ============================================================================
 * `gsp_net` owns sockets and NOTHING ELSE.  It does not own the server (the
 * caller creates and destroys it, so counters stay readable after the listener
 * stops), it does not drain events, and it does not decide anything about the
 * protocol.  One call drives it:
 *
 *     while (running) {
 *         gsp_net_poll(net, 100);                 // accept, read, tick, write
 *         while ((n = gsp_server_poll_events(s, ev, 16)) > 0)
 *             for (i = 0; i < n; ++i) handle(&ev[i]);
 *     }
 *
 * ⚠ EVENTS ARE STILL THE CALLER'S TO DRAIN.  That is not tidiness: acting on
 * GSP_EV_CLOSE_REQUESTED (policy.protocol_error_close_threshold) means closing
 * a socket, and the decision is the application's — so the application sees the
 * event and calls gsp_net_close_connection().  A transport that swallowed the
 * event would have made that policy choice on the host's behalf.
 *
 * ⚠ WHAT IT PROMISES THE CLIENTS, and what every host owes them (design §5.5,
 * protocol §9.8):
 *   - ONE send() per gsp_write_request, never concatenated.  Six clients parse
 *     one read as one message and [PIT]'s receive thread DIES when a 200 and a
 *     201 share a segment.
 *   - TCP_NODELAY on every accepted socket, or Nagle holds a 60-byte reply for
 *     up to 40 ms on the one path where [MLM] is counting to two seconds before
 *     it re-sends the shot.
 *   - Bytes handed to gsp_server_on_bytes() EXACTLY as read(2) returned them:
 *     not split on newlines, not parsed first, not waited on (design §3.2.1).
 *   - next_due_us() re-read after every call INCLUDING the polls, because a
 *     write held back by policy.write_spacing_us becomes due then.
 *
 * ⚠ WHAT IT CANNOT PROMISE: that two writes stay in two segments.  Nothing can
 * (design §5.5).  policy.write_spacing_us makes coalescing much less likely and
 * this transport honours it by calling tick when it comes due; that is the
 * whole of what is available.
 */
#ifndef GSPRO_NET_H
#define GSPRO_NET_H

#include "gspro/server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gsp_net gsp_net;

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ EVERY BOOLEAN IS PHRASED SO THAT ZERO IS THE RIGHT ANSWER.  A zeroed
 * gsp_net_config is a working listener on all interfaces, on the default port,
 * with TCP_NODELAY on and address reuse off — because a caller who memsets a
 * struct and fills in two fields must not silently get the unsafe half of a
 * choice.  That is why the two switches below are named for what they DISABLE.
 */
typedef struct gsp_net_config {
    /*
     * Interface to bind.  NULL or "" → "0.0.0.0", ALL INTERFACES, which is
     * design §6.1's decision and the opposite of what [GSP]'s page assumes: the
     * useful case is a Rapsodo or Garmin bridge on a phone, or a PiTrac on the
     * network, not a connector on this very machine.  "127.0.0.1" is the
     * restrictive option.  ⚠ On Windows an inbound firewall rule is needed for
     * the *executable* or a device on another machine fails silently with
     * "connection refused" — CT-T06 is the only case that can see this, and it
     * needs a second machine.
     */
    const char *host;

    /* 0 → GSP_DEFAULT_PORT (921).  ⚠ Pass 0 explicitly via `ephemeral_port`
     * below to get a kernel-chosen port; 0 here means "the default", because a
     * tool that silently listened on a random port would look like it worked
     * and be unreachable. */
    uint16_t port;

    /* Ask the kernel for any free port and report it from gsp_net_port().
     * For tests and for a second listener beside a real GSPro. */
    bool ephemeral_port;

    /* ⚠ SO_REUSEADDR, and it is OFF by default ON PURPOSE.  A listener that
     * quietly took a port from something else would defeat CT-T09 — "bind while
     * 921 is held by another process: report the reason, do not appear to be
     * listening" — which matters because on the machine this is most likely to
     * run on, 921 is held by GSPro itself (design §6.4).  On POSIX the flag only
     * skips TIME_WAIT and two live listeners still conflict; ⚠ ON WINDOWS
     * SO_REUSEADDR LETS ONE PROCESS STEAL ANOTHER'S LISTENING PORT, so this
     * transport IGNORES the flag there and sets SO_EXCLUSIVEADDRUSE instead. */
    bool reuse_address;

    /* Leave Nagle enabled.  ⚠ Almost certainly wrong: see the header note. */
    bool allow_nagle;

    uint16_t backlog;          /* listen() backlog.  0 → 8                     */

    /*
     * Sockets this transport will hold at once.  0 → 8.
     *
     * ⚠ THIS IS NOT THE SERVER'S LIMIT AND DOES NOT REPLACE IT.  The server has
     * its own `gsp_server_config.max_connections` and answers
     * GSP_ERR_TOO_MANY_CONNECTIONS when it is reached; a socket refused that way
     * is closed here, because the protocol has NO MESSAGE that could tell the
     * client why (design §5.1).  This number bounds only the transport's own
     * table, and select() bounds it in turn: it must leave room for the listener
     * inside FD_SETSIZE, and open() refuses a value that does not.
     */
    uint16_t max_connections;
    uint32_t read_buffer;      /* bytes per read().  0 → 16 KiB                */

    /*
     * Per-connection queue of writes waiting for a socket that would block.
     * 0 → 4.  ⚠ A reply MUST NOT be dropped ([MLM] re-sends the shot when one
     * goes missing, protocol §4.4), so when a queue is full this transport
     * stops draining the server's write ring rather than discarding anything;
     * the ring then applies its own backpressure through GSP_ERR_QUEUE_FULL.
     */
    uint16_t write_queue;

    /* Zeroed → malloc/free.  gsp_net_open() makes exactly ONE allocation. */
    gsp_allocator allocator;
} gsp_net_config;

GSP_API gsp_net_config gsp_net_config_default(void);

/* ------------------------------------------------------------------------ */
/* Counters — ⚠ the only place "one write per request" is observable         */
/* ------------------------------------------------------------------------ */
/*
 * It cannot be seen from the client side: TCP is free to merge two writes into
 * one segment and to split one into two, so a client counting reads is
 * measuring the kernel.  CT-T10 counts send() calls HERE, which is the same
 * thing tests/test_python_transport.py does by wrapping the asyncio writer.
 */
typedef struct gsp_net_stats {
    uint64_t accepted;        /* sockets accepted                               */
    uint64_t refused;         /* accepted and closed again: at the limit, or the
                               * server refused the id, or select() cannot watch
                               * the descriptor (see FD_SETSIZE below)          */
    uint64_t reads;           /* read()s that returned bytes                    */
    uint64_t read_bytes;
    uint64_t writes;          /* write requests fully written                   */
    uint64_t sends;           /* ⚠ send() CALLS.  writes counts requests, this
                               * counts syscalls, and CT-T10 asserts they are
                               * EQUAL — which is "one send() per request" stated
                               * as something a test can fail.  A transport that
                               * concatenated two replies would show two writes
                               * and one send, and [PIT] would die on it        */
    uint64_t partial_writes;  /* ⚠ a request that took more than one send().  A
                               * 256-byte write on a fresh socket essentially
                               * never does, and the transport still handles it */
    uint64_t write_bytes;
    uint64_t polls;
    uint32_t max_write_bytes; /* largest single request written; ≤ GSP_WRITE_MAX */
    uint32_t reserved;
} gsp_net_stats;

GSP_API gsp_net_stats gsp_net_get_stats(const gsp_net *n);

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */
#define GSP_NET_ERROR_MAX 192

/*
 * Binds and listens.  ⚠ THE SERVER IS THE CALLER'S: this never creates one and
 * never destroys one, because a host that wants to read counters after the
 * listener stops must be able to.
 *
 * `error` (may be NULL) receives the reason a bind or listen failed, with the
 * platform's own text in it — CT-T09 is the case that says a host must report
 * that rather than appear to be listening, and "Address already in use" is the
 * whole content of the report.  On failure *out is NULL and nothing leaks.
 *
 * GSP_ERR_INVALID_ARG for a NULL server or an unresolvable host,
 * GSP_ERR_NO_MEMORY if the allocator refused, GSP_ERR_INVALID_STATE if the
 * socket could not be bound or listened on.
 */
GSP_API gsp_status gsp_net_open(gsp_server *server, const gsp_net_config *config,
                                gsp_net **out, char *error, size_t error_size);

/*
 * Closes the listener and every accepted socket, reports each one to the server
 * as GSP_CLOSE_LOCAL_REQUEST so the session log is complete, and frees the one
 * allocation.  NULL is a no-op.
 *
 * ⚠ Drain the server once more afterwards (design §3.3): the close events are
 * queued, not pushed, and a host that stops draining first loses them.
 *
 * ⚠ THE HANDLE IS GONE WHEN THIS RETURNS.  There is no half-closed state and no
 * flag to test: a call on it afterwards is a use-after-free like any other, so
 * a host that keeps the pointer must clear it.  The SERVER is untouched and its
 * counters stay readable, which is the whole reason it is not owned here.
 */
GSP_API void gsp_net_close(gsp_net *n);

/* The port actually bound — ⚠ ask AFTER open(), because `ephemeral_port` means
 * the kernel chose it. */
GSP_API uint16_t gsp_net_port(const gsp_net *n);

/* The last error this transport recorded, "" if none.  Never NULL.  A failed
 * read or write is NOT an error return from poll: the connection is closed and
 * reported, and the reason lands here. */
GSP_API const char *gsp_net_error(const gsp_net *n);

/* ------------------------------------------------------------------------ */
/* The loop                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * ONE PASS: wait for readability, accept, read, feed the server, run whatever
 * the clock made due, then drain the write ring onto the sockets.
 *
 * `timeout_ms` bounds the wait; a negative value waits indefinitely.  ⚠ The
 * effective wait is always clamped to gsp_server_next_due_us(), so an idle
 * alarm or a write held back by policy.write_spacing_us fires on time no matter
 * what the caller passed.  A poll that does nothing costs one select().
 *
 * Returns GSP_OK.  ⚠ A failing client is NOT a failing poll: it is a closed
 * connection, an event on the server, and a line in gsp_net_error().
 */
GSP_API gsp_status gsp_net_poll(gsp_net *n, int timeout_ms);

/*
 * Close one connection and report it to the server as GSP_CLOSE_LOCAL_REQUEST.
 * ⚠ THIS IS WHAT GSP_EV_CLOSE_REQUESTED IS FOR: the library cannot close
 * anything, so a host that sets policy.protocol_error_close_threshold must act
 * on the event itself.  GSP_ERR_UNKNOWN_CONNECTION if this transport does not
 * hold that id.
 */
GSP_API gsp_status gsp_net_close_connection(gsp_net *n, gsp_conn_id conn);

/* Sockets this transport currently holds.  ⚠ May briefly differ from
 * gsp_server_connection_count() — a socket refused by the server is closed
 * without ever becoming a connection. */
GSP_API size_t gsp_net_connection_count(const gsp_net *n);

/*
 * The monotonic microsecond clock this transport supplies to the server, and
 * the one a caller should use for its own now_us arguments so that one session
 * has one timeline.  ⚠ MONOTONIC: a wall clock stepped by NTP produces a
 * session whose ordering is a fiction (design §4.1).
 *
 * ⚠ It is HERE and not in the core precisely because reading a clock is what
 * tests/purity.cmake forbids the core to do.
 */
GSP_API gsp_time_us gsp_net_now_us(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_NET_H */
