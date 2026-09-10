/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_net.c — the reference socket transport: one select() loop, no threads.
 *
 * ⚠ READ include/gspro/net.h FIRST.  It says what this is for and what it
 * promises the clients; this file is how, and the comments here are the how's
 * reasons rather than a second copy of the contract.
 *
 * THE WHOLE OF ONE PASS, in the order it must happen:
 *
 *   select ── accept ── read ── on_bytes ── tick ── drain writes ── send
 *
 * Writes drain LAST because everything before them can produce one, and they
 * drain into one send() per request because six clients parse one read as one
 * message (protocol §9.8).  The tick is between because policy.write_spacing_us
 * releases a held write on the clock rather than on an event.
 */
#include "gs_net_sys.h"

#include "gspro/net.h"

#include <stdlib.h>

/* ------------------------------------------------------------------------ */
/* Defaults                                                                  */
/* ------------------------------------------------------------------------ */
#define GS_NET_BACKLOG_DEFAULT      8
#define GS_NET_MAX_CONNS_DEFAULT    8
#define GS_NET_READ_DEFAULT         (16u * 1024u)
#define GS_NET_WRITE_QUEUE_DEFAULT  4
#define GS_NET_PEER_TEXT_MAX        96

/* How many times one poll will re-offer bytes the write ring refused.  ⚠ NOT a
 * `while`: if a client has stopped reading, its queue stays full and its own
 * backpressure is the correct outcome — the framer keeps the unconsumed bytes
 * and the next poll tries again (design §3.4). */
#define GS_NET_QUEUE_FULL_RETRIES   4

typedef struct gs_net_conn {
    gs_sock            sock;
    gsp_conn_id        conn;
    bool               in_use;
    bool               want_write;   /* the socket blocked with data queued     */
    bool               resume;       /* ⚠ bytes are held in the FRAMER, not here:
                                      * the write ring was full and the message
                                      * was not consumed.  Offer zero bytes again
                                      * once the ring has drained               */
    uint16_t           q_head;       /* index of the request being sent         */
    uint16_t           q_count;
    uint32_t           q_offset;     /* bytes of that request already gone      */
    gsp_write_request *queue;        /* [n->write_queue], into the one block    */
} gs_net_conn;

struct gsp_net {
    gsp_server        *server;
    gsp_allocator      alloc;
    gs_sock            listener;
    uint16_t           port;
    uint16_t           max_conns;
    uint16_t           write_queue;
    uint16_t           conn_count;
    uint32_t           read_size;
    bool               allow_nagle;
    gsp_conn_id        next_conn;
    gsp_net_stats      stats;
    char               error[GSP_NET_ERROR_MAX];
    gs_net_conn       *conns;      /* [max_conns]                              */
    uint8_t           *read_buf;   /* [read_size]                              */
};

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void *gs_net_alloc(const gsp_allocator *a, size_t size)
{
    if (a != NULL && a->alloc != NULL) {
        return a->alloc(a->ctx, size);
    }
    return malloc(size);
}

static void gs_net_free(const gsp_allocator *a, void *p)
{
    if (a != NULL && a->free != NULL) {
        a->free(a->ctx, p);
        return;
    }
    free(p);
}

/* "bind 0.0.0.0:921: Address already in use" — the platform's words, with what
 * we were doing in front of them.  CT-T09 is this string reaching a user. */
static void gs_net_note(char *out, size_t out_size, const char *what, int err)
{
    char text[GSP_NET_ERROR_MAX];
    if (out == NULL || out_size == 0u) {
        return;
    }
    gs_sys_error_text(err, text, sizeof(text));
    (void)snprintf(out, out_size, "%s: %s", what, text);
}

static void gs_net_record(gsp_net *n, const char *what, int err)
{
    gs_net_note(n->error, sizeof(n->error), what, err);
}

static size_t gs_net_round_up(size_t v, size_t align)
{
    return (v + (align - 1u)) & ~(align - 1u);
}

static gs_net_conn *gs_net_find(gsp_net *n, gsp_conn_id conn)
{
    uint16_t i;
    for (i = 0; i < n->max_conns; ++i) {
        if (n->conns[i].in_use && n->conns[i].conn == conn) {
            return &n->conns[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */
gsp_net_config gsp_net_config_default(void)
{
    gsp_net_config c;
    memset(&c, 0, sizeof(c));
    /* ⚠ EVERY FIELD'S DEFAULT IS ZERO, and that is the point of how they are
     * named (net.h): a zeroed config is a working, non-Nagling, non-stealing
     * listener on all interfaces.  This function exists so a caller can say so
     * out loud, not because it fills anything in. */
    return c;
}

/* ------------------------------------------------------------------------ */
/* Connection teardown                                                       */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THE SERVER IS TOLD FIRST, ALWAYS.  A socket closed without an
 * on_connection_closed() leaves the id occupying a slot in the server's table
 * for the life of the process, and the next connection to be handed that id is
 * refused with GSP_ERR_INVALID_STATE — a leak that looks like a protocol fault.
 */
static void gs_net_drop(gsp_net *n, gs_net_conn *c, gsp_close_cause cause)
{
    if (!c->in_use) {
        return;
    }
    (void)gsp_server_on_connection_closed(n->server, c->conn, cause, gsp_net_now_us());
    gs_sys_sock_close(c->sock);
    c->sock = GS_SOCK_INVALID;
    c->in_use = false;
    c->want_write = false;
    c->resume = false;
    c->q_head = 0;
    c->q_count = 0;
    c->q_offset = 0;
    if (n->conn_count > 0u) {
        n->conn_count--;
    }
}

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */
/* ------------------------------------------------------------------------ */
/*
 * ONE send() PER REQUEST.  ⚠ The exception the kernel forces on us is a short
 * send: the request is then finished by further sends and counted in
 * stats.partial_writes, which is the honest reading of "one write per message"
 * — a 256-byte write on a fresh socket essentially never splits, and pretending
 * the case cannot happen would drop bytes when it does.
 */
static void gs_net_flush(gsp_net *n, gs_net_conn *c)
{
    while (c->q_count > 0u) {
        gsp_write_request *w = &c->queue[c->q_head];
        size_t total = (size_t)w->length;
        size_t left = total - (size_t)c->q_offset;
#if defined(_WIN32)
        int sent = send(c->sock, (const char *)w->data + c->q_offset, (int)left,
                        GS_SYS_SEND_FLAGS);
#else
        ssize_t sent = send(c->sock, (const char *)w->data + c->q_offset, left,
                            GS_SYS_SEND_FLAGS);
#endif
        if (sent == GS_SOCK_ERROR) {
            int err = gs_sys_errno();
            if (gs_sys_would_block(err)) {
                c->want_write = true;   /* select() will say when it drains      */
                return;
            }
            gs_net_record(n, "send", err);
            gs_net_drop(n, c, GSP_CLOSE_TRANSPORT_ERROR);
            return;
        }
        n->stats.sends++;
        n->stats.write_bytes += (uint64_t)sent;
        if ((size_t)sent < left) {
            c->q_offset += (uint32_t)sent;
            c->want_write = true;
            return;
        }
        if (c->q_offset != 0u) {
            n->stats.partial_writes++;
        }
        n->stats.writes++;
        if ((uint32_t)w->length > n->stats.max_write_bytes) {
            n->stats.max_write_bytes = (uint32_t)w->length;
        }
        c->q_offset = 0;
        c->q_head = (uint16_t)((c->q_head + 1u) % n->write_queue);
        c->q_count--;
    }
    c->want_write = false;
}

/*
 * Drain the server's write ring onto the connections.
 *
 * ⚠ ONE REQUEST AT A TIME, AND ONLY WHEN THERE IS ROOM FOR IT ANYWHERE.  The
 * write ring is NOT drop-oldest — a dropped reply re-sends a shot from [MLM]
 * (design §3.4) — so this must never take a request it cannot store.  There is
 * no peek, so the test is made before the take: while every open connection has
 * a free queue slot, whatever comes out can be put somewhere.
 */
static void gs_net_pump(gsp_net *n)
{
    for (;;) {
        gsp_write_request w;
        gs_net_conn *c;
        uint16_t i;
        bool room = true;

        for (i = 0; i < n->max_conns; ++i) {
            if (n->conns[i].in_use && n->conns[i].q_count >= n->write_queue) {
                room = false;
                break;
            }
        }
        if (!room) {
            break;
        }
        if (gsp_server_poll_writes(n->server, &w, 1) == 0u) {
            break;
        }
        c = gs_net_find(n, w.conn);
        if (c == NULL) {
            /* The client went away between the queue and the drain.  Nothing to
             * report: its close event is already on the event ring. */
            continue;
        }
        c->queue[(c->q_head + c->q_count) % n->write_queue] = w;
        c->q_count++;
        gs_net_flush(n, c);
    }
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                   */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THE BYTES GO IN EXACTLY AS THEY CAME.  No splitting on newlines (a [TNB]
 * message contains them), no parsing first, no waiting for "a whole message"
 * — which a host cannot recognise (design §3.2.1).  Length may be one byte.
 */
static void gs_net_feed(gsp_net *n, gs_net_conn *c, const uint8_t *data, size_t len)
{
    gsp_status st = gsp_server_on_bytes(n->server, c->conn, data, len, gsp_net_now_us());
    int attempt;

    for (attempt = 0; st == GSP_ERR_QUEUE_FULL && attempt < GS_NET_QUEUE_FULL_RETRIES;
         ++attempt) {
        /* The write ring filled and the message was NOT consumed; the framer is
         * holding it.  Drain what we can and offer zero bytes to resume, as
         * server.h documents. */
        gs_net_pump(n);
        if (!c->in_use) {
            return;             /* the flush found the socket gone              */
        }
        st = gsp_server_on_bytes(n->server, c->conn, NULL, 0, gsp_net_now_us());
    }
    /*
     * ⚠ AND IF IT IS STILL FULL, REMEMBER TO COME BACK.  This is a liveness bug
     * waiting to happen: the unconsumed message sits in the framer, and nothing
     * offers it again until the client sends MORE bytes — which a client waiting
     * for its acknowledgement will never do.  The next poll retries with zero
     * bytes as soon as the ring has drained, whether or not anything arrives.
     */
    c->resume = (st == GSP_ERR_QUEUE_FULL);
}

static void gs_net_read(gsp_net *n, gs_net_conn *c)
{
#if defined(_WIN32)
    int got = recv(c->sock, (char *)n->read_buf, (int)n->read_size, 0);
#else
    ssize_t got = recv(c->sock, n->read_buf, n->read_size, 0);
#endif
    if (got == 0) {
        gs_net_drop(n, c, GSP_CLOSE_REMOTE_CLOSED);
        return;
    }
    if (got == GS_SOCK_ERROR) {
        int err = gs_sys_errno();
        if (gs_sys_would_block(err)) {
            return;
        }
        gs_net_record(n, "recv", err);
        gs_net_drop(n, c, GSP_CLOSE_TRANSPORT_ERROR);
        return;
    }
    n->stats.reads++;
    n->stats.read_bytes += (uint64_t)got;
    gs_net_feed(n, c, n->read_buf, (size_t)got);
}

/* ------------------------------------------------------------------------ */
/* Accepting                                                                 */
/* ------------------------------------------------------------------------ */
static void gs_net_peer_text(const struct sockaddr *addr, gs_socklen len, char *out,
                             size_t out_size)
{
    char host[64];
    char serv[16];
    out[0] = '\0';
    if (getnameinfo(addr, len, host, (unsigned)sizeof(host), serv, (unsigned)sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return;
    }
    /* ⚠ PERSONAL DATA from here on (design §9.2): a peer address identifies a
     * household.  The library redacts it out of gsp_event_format() and the wire
     * log unless asked; the transport's job is only to hand it over. */
    (void)snprintf(out, out_size, "%s:%s", host, serv);
}

static void gs_net_accept(gsp_net *n)
{
    for (;;) {
        struct sockaddr_storage addr;
        gs_socklen addr_len = (gs_socklen)sizeof(addr);
        char peer[GS_NET_PEER_TEXT_MAX];
        gs_net_conn *slot = NULL;
        uint16_t i;
        gsp_conn_id id;
        gs_sock s = accept(n->listener, (struct sockaddr *)&addr, &addr_len);

        if (s == GS_SOCK_INVALID) {
            int err = gs_sys_errno();
            if (!gs_sys_would_block(err)) {
                gs_net_record(n, "accept", err);
            }
            return;
        }

#if !defined(_WIN32)
        /* ⚠ select() CANNOT WATCH A DESCRIPTOR ≥ FD_SETSIZE, and setting one in
         * an fd_set is undefined behaviour rather than an error — a process
         * that already holds 1024 files would corrupt its own stack here.  A
         * transport built on select() has to refuse it; a host that needs
         * thousands of connections wants poll()/epoll/IOCP and is writing its
         * own adapter anyway (design §3.1). */
        if (s >= (gs_sock)FD_SETSIZE) {
            gs_sys_sock_close(s);
            n->stats.refused++;
            (void)snprintf(n->error, sizeof(n->error),
                           "accept: descriptor %d is beyond FD_SETSIZE (%d); "
                           "this select() transport cannot watch it", (int)s, FD_SETSIZE);
            continue;
        }
#endif
        for (i = 0; i < n->max_conns; ++i) {
            if (!n->conns[i].in_use) {
                slot = &n->conns[i];
                break;
            }
        }
        if (slot == NULL) {
            /* ⚠ The transport's table is full.  Closing the socket is the whole
             * vocabulary the protocol offers for a refusal (design §5.1) — and
             * this is the same answer the server gives at ITS limit, below. */
            gs_sys_sock_close(s);
            n->stats.refused++;
            (void)snprintf(n->error, sizeof(n->error),
                           "accept: this transport already holds %u socket(s)",
                           (unsigned)n->max_conns);
            continue;
        }
        /* ⚠ ANY NON-ZERO uint32_t THE HOST CAN MAP BACK (design §3.2).  A
         * counter is the simplest such thing; it must skip 0 (GSP_CONN_NONE)
         * and UINT32_MAX (GSP_CONN_ALL, the broadcast id). */
        do {
            id = n->next_conn++;
        } while (id == GSP_CONN_NONE || id == GSP_CONN_ALL);

        gs_net_peer_text((const struct sockaddr *)&addr, addr_len, peer, sizeof(peer));

        if (gsp_server_on_connection_opened(n->server, id, peer[0] != '\0' ? peer : NULL,
                                            gsp_net_now_us()) < GSP_OK) {
            /* ⚠ GSP_ERR_TOO_MANY_CONNECTIONS lands here, and NOTHING THE HOST
             * SENDS CAN TELL THE CLIENT WHY — the protocol has no message for
             * it (design §5.1).  Closing the socket is the whole vocabulary. */
            gs_sys_sock_close(s);
            n->stats.refused++;
            continue;
        }

        if (!gs_sys_set_nonblocking(s)) {
            gs_net_record(n, "set nonblocking", gs_sys_errno());
        }
        if (!n->allow_nagle && !gs_sys_set_nodelay(s)) {
            gs_net_record(n, "TCP_NODELAY", gs_sys_errno());
        }
        gs_sys_set_nosigpipe(s);

        slot->sock = s;
        slot->conn = id;
        slot->in_use = true;
        slot->want_write = false;
        slot->q_head = 0;
        slot->q_count = 0;
        slot->q_offset = 0;
        n->conn_count++;
        n->stats.accepted++;

        /* policy.announce_player_on_connect and announce_ready_on_connect may
         * have queued a 201 and a 202 already; they go out with the next pump. */
    }
}

/* ------------------------------------------------------------------------ */
/* Open                                                                      */
/* ------------------------------------------------------------------------ */
static gs_sock gs_net_bind(const char *host, uint16_t port, bool reuse, int backlog,
                           uint16_t *bound_port, char *error, size_t error_size)
{
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    struct addrinfo *ai;
    char port_text[8];
    gs_sock s = GS_SOCK_INVALID;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

    rc = getaddrinfo(host, port_text, &hints, &list);
    if (rc != 0 || list == NULL) {
#if defined(_WIN32)
        gs_net_note(error, error_size, "resolve", rc);
#else
        (void)snprintf(error, error_size, "resolve %s: %s", host, gai_strerror(rc));
#endif
        return GS_SOCK_INVALID;
    }

    for (ai = list; ai != NULL; ai = ai->ai_next) {
        struct sockaddr_storage actual;
        gs_socklen actual_len = (gs_socklen)sizeof(actual);
        char what[96];

        /* ⚠ WHICH ADDRESS, in the message itself.  "Address already in use" on
         * its own leaves a user guessing; "bind 0.0.0.0:921: Address already in
         * use" tells them GSPro has the port (design §6.4). */
        (void)snprintf(what, sizeof(what), "bind %s:%u", host, (unsigned)port);

        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == GS_SOCK_INVALID) {
            gs_net_note(error, error_size, "socket", gs_sys_errno());
            continue;
        }
#if defined(_WIN32)
        /* ⚠ WINSOCK'S SO_REUSEADDR IS NOT POSIX'S: it lets one process BIND A
         * PORT ANOTHER IS ALREADY LISTENING ON, silently stealing connections.
         * The flag is honoured only on POSIX, and here the opposite is asked
         * for — so that a second listener FAILS, loudly, which is CT-T09. */
        {
            int excl = 1;
            (void)reuse;
            (void)setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl,
                             (gs_socklen)sizeof(excl));
        }
#else
        if (reuse) {
            int on = 1;
            (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
                             (gs_socklen)sizeof(on));
        }
#endif
        if (bind(s, ai->ai_addr, (gs_socklen)ai->ai_addrlen) != 0) {
            gs_net_note(error, error_size, what, gs_sys_errno());
            gs_sys_sock_close(s);
            s = GS_SOCK_INVALID;
            continue;
        }
        if (listen(s, backlog) != 0) {
            (void)snprintf(what, sizeof(what), "listen %s:%u", host, (unsigned)port);
            gs_net_note(error, error_size, what, gs_sys_errno());
            gs_sys_sock_close(s);
            s = GS_SOCK_INVALID;
            continue;
        }
        /* ⚠ The REQUESTED port is the fallback, so gsp_net_port() never says 0
         * while the socket is listening.  It is only ever wrong for an
         * ephemeral bind whose getsockname() failed, and then 0 is the honest
         * answer — "the kernel chose one and would not tell us". */
        *bound_port = port;
        if (getsockname(s, (struct sockaddr *)&actual, &actual_len) == 0) {
            if (actual.ss_family == AF_INET) {
                *bound_port = ntohs(((struct sockaddr_in *)(void *)&actual)->sin_port);
            } else if (actual.ss_family == AF_INET6) {
                *bound_port = ntohs(((struct sockaddr_in6 *)(void *)&actual)->sin6_port);
            }
        }
        break;
    }

    freeaddrinfo(list);
    return s;
}

gsp_status gsp_net_open(gsp_server *server, const gsp_net_config *config, gsp_net **out,
                        char *error, size_t error_size)
{
    gsp_net_config cfg;
    gsp_net *n;
    uint8_t *block;
    size_t conns_off;
    size_t queues_off;
    size_t read_off;
    size_t total;
    uint16_t max_conns;
    uint16_t write_queue;
    uint32_t read_size;
    uint16_t i;
    uint16_t port;
    char local_error[GSP_NET_ERROR_MAX];

    if (error != NULL && error_size > 0u) {
        error[0] = '\0';
    }
    if (out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (server == NULL) {
        return GSP_ERR_INVALID_ARG;
    }

    cfg = (config != NULL) ? *config : gsp_net_config_default();
    max_conns = (cfg.max_connections != 0u) ? cfg.max_connections : GS_NET_MAX_CONNS_DEFAULT;
    write_queue = (cfg.write_queue != 0u) ? cfg.write_queue : GS_NET_WRITE_QUEUE_DEFAULT;
    read_size = (cfg.read_buffer != 0u) ? cfg.read_buffer : GS_NET_READ_DEFAULT;
    port = cfg.ephemeral_port ? 0u : ((cfg.port != 0u) ? cfg.port : (uint16_t)GSP_DEFAULT_PORT);

    /* ⚠ The listener needs a slot in the read set too, so the bound is
     * FD_SETSIZE - 1.  Refusing here beats corrupting an fd_set later. */
    if ((size_t)max_conns + 1u > (size_t)FD_SETSIZE) {
        if (error != NULL) {
            (void)snprintf(error, error_size,
                           "max_connections %u does not fit select()'s FD_SETSIZE (%d)",
                           (unsigned)max_conns, FD_SETSIZE);
        }
        return GSP_ERR_INVALID_ARG;
    }

    if (!gs_sys_startup()) {
        if (error != NULL) {
            gs_net_note(error, error_size, "winsock startup", gs_sys_errno());
        }
        return GSP_ERR_NOT_SUPPORTED;
    }

    /* ONE ALLOCATION, as the core makes one (design §3.4): the object, the
     * connection table, every connection's write queue and the read buffer. */
    conns_off  = gs_net_round_up(sizeof(gsp_net), 16u);
    queues_off = gs_net_round_up(conns_off + (size_t)max_conns * sizeof(gs_net_conn), 16u);
    read_off   = gs_net_round_up(queues_off + (size_t)max_conns * (size_t)write_queue
                                     * sizeof(gsp_write_request), 16u);
    total      = read_off + read_size;

    block = (uint8_t *)gs_net_alloc(&cfg.allocator, total);
    if (block == NULL) {
        gs_sys_cleanup();
        if (error != NULL) {
            (void)snprintf(error, error_size, "out of memory (%zu bytes)", total);
        }
        return GSP_ERR_NO_MEMORY;
    }
    memset(block, 0, total);

    n = (gsp_net *)(void *)block;
    n->server = server;
    n->alloc = cfg.allocator;
    n->listener = GS_SOCK_INVALID;
    n->max_conns = max_conns;
    n->write_queue = write_queue;
    n->read_size = read_size;
    n->allow_nagle = cfg.allow_nagle;
    n->next_conn = 1u;
    n->conns = (gs_net_conn *)(void *)(block + conns_off);
    n->read_buf = block + read_off;
    for (i = 0; i < max_conns; ++i) {
        n->conns[i].sock = GS_SOCK_INVALID;
        n->conns[i].queue = (gsp_write_request *)(void *)(block + queues_off
                            + (size_t)i * (size_t)write_queue * sizeof(gsp_write_request));
    }

    local_error[0] = '\0';
    n->listener = gs_net_bind((cfg.host != NULL && cfg.host[0] != '\0') ? cfg.host : "0.0.0.0",
                              port, cfg.reuse_address,
                              (cfg.backlog != 0u) ? (int)cfg.backlog : GS_NET_BACKLOG_DEFAULT,
                              &n->port, local_error, sizeof(local_error));
    if (n->listener == GS_SOCK_INVALID) {
        (void)snprintf(n->error, sizeof(n->error), "%s", local_error);
        if (error != NULL) {
            (void)snprintf(error, error_size, "%s", local_error);
        }
        gs_net_free(&cfg.allocator, block);
        gs_sys_cleanup();
        return GSP_ERR_INVALID_STATE;
    }
    if (!gs_sys_set_nonblocking(n->listener)) {
        /* ⚠ A blocking listener would make accept() hang after select() said it
         * was readable and the connection was then reset — the classic
         * select-accept race, and a hang is worse than a refusal. */
        gs_net_note(local_error, sizeof(local_error), "set nonblocking", gs_sys_errno());
        (void)snprintf(n->error, sizeof(n->error), "%s", local_error);
        if (error != NULL) {
            (void)snprintf(error, error_size, "%s", local_error);
        }
        gs_sys_sock_close(n->listener);
        gs_net_free(&cfg.allocator, block);
        gs_sys_cleanup();
        return GSP_ERR_INVALID_STATE;
    }

    *out = n;
    return GSP_OK;
}

void gsp_net_close(gsp_net *n)
{
    gsp_allocator alloc;
    uint16_t i;

    if (n == NULL) {
        return;
    }
    for (i = 0; i < n->max_conns; ++i) {
        /* ⚠ Reported to the server, not just closed: see gs_net_drop().  The
         * events are queued — drain once more after this returns. */
        gs_net_drop(n, &n->conns[i], GSP_CLOSE_LOCAL_REQUEST);
    }
    if (n->listener != GS_SOCK_INVALID) {
        gs_sys_sock_close(n->listener);
        n->listener = GS_SOCK_INVALID;
    }
    alloc = n->alloc;
    gs_net_free(&alloc, n);
    gs_sys_cleanup();
}

/* ------------------------------------------------------------------------ */
/* The pass                                                                  */
/* ------------------------------------------------------------------------ */
gsp_status gsp_net_poll(gsp_net *n, int timeout_ms)
{
    fd_set readers;
    fd_set writers;
    struct timeval tv;
    struct timeval *tvp = &tv;
    gs_sock max_fd;
    gsp_time_us now;
    gsp_time_us due;
    int64_t wait_us;
    int ready;
    uint16_t i;

    if (n == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    n->stats.polls++;

    FD_ZERO(&readers);
    FD_ZERO(&writers);
    FD_SET(n->listener, &readers);
    max_fd = n->listener;
    for (i = 0; i < n->max_conns; ++i) {
        gs_net_conn *c = &n->conns[i];
        if (!c->in_use) {
            continue;
        }
        FD_SET(c->sock, &readers);
        if (c->want_write) {
            FD_SET(c->sock, &writers);
        }
        if (c->sock > max_fd) {
            max_fd = c->sock;
        }
    }

    /*
     * ⚠ THE WAIT IS CLAMPED BY next_due_us(), ALWAYS.  The ordinary answer is
     * NEVER — the protocol has no deadline of its own (protocol §9.6) — and only
     * policy.idle_alarm_us and policy.write_spacing_us arm anything.  A caller
     * that passed a 10-second timeout must still not hold a 201 back by ten
     * seconds because spacing said 20 ms.
     */
    now = gsp_net_now_us();
    due = gsp_server_next_due_us(n->server);
    wait_us = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms * 1000;
    if (due != GSP_TIME_NEVER) {
        int64_t until = (due > now) ? (due - now) : 0;
        if (wait_us < 0 || until < wait_us) {
            wait_us = until;
        }
    }
    if (wait_us < 0) {
        tvp = NULL;
    } else {
        gs_sys_timeval_set(&tv, wait_us);
    }

#if defined(_WIN32)
    ready = select(0, &readers, &writers, NULL, tvp);
#else
    ready = select((int)max_fd + 1, &readers, &writers, NULL, tvp);
#endif
    if (ready == GS_SOCK_ERROR) {
        int err = gs_sys_errno();
        if (!gs_sys_would_block(err)) {
            gs_net_record(n, "select", err);
        }
        /* ⚠ NOT an error return.  EINTR is what ctrl-c looks like from in here,
         * and a caller's loop must be free to check its own flag and come back. */
        FD_ZERO(&readers);
        FD_ZERO(&writers);
    }

    if (ready > 0 && FD_ISSET(n->listener, &readers)) {
        gs_net_accept(n);
    }

    if (ready > 0) {
        for (i = 0; i < n->max_conns; ++i) {
            gs_net_conn *c = &n->conns[i];
            gs_sock s = c->sock;
            if (!c->in_use) {
                continue;
            }
            if (FD_ISSET(s, &writers)) {
                gs_net_flush(n, c);
            }
            if (c->in_use && FD_ISSET(s, &readers)) {
                /* ⚠ ONE read() PER PASS PER CONNECTION, on purpose: a client
                 * that never stops talking must not starve the others or the
                 * clock.  select() says so again immediately if more is there. */
                gs_net_read(n, c);
            }
        }
    }

    /* Whatever the clock made due: the idle alarm, and a write that spacing has
     * been holding.  ⚠ Re-read `now` — accept and read took time. */
    now = gsp_net_now_us();
    due = gsp_server_next_due_us(n->server);
    if (due != GSP_TIME_NEVER && due <= now) {
        gsp_server_tick(n->server, now);
    }

    gs_net_pump(n);

    /* ⚠ Anything the write ring refused earlier, offered again now that the
     * pump has run.  See the note at the end of gs_net_feed(). */
    for (i = 0; i < n->max_conns; ++i) {
        gs_net_conn *c = &n->conns[i];
        if (c->in_use && c->resume) {
            gs_net_feed(n, c, NULL, 0);
        }
    }
    return GSP_OK;
}

gsp_status gsp_net_close_connection(gsp_net *n, gsp_conn_id conn)
{
    gs_net_conn *c;
    if (n == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    c = gs_net_find(n, conn);
    if (c == NULL) {
        return GSP_ERR_UNKNOWN_CONNECTION;
    }
    gs_net_drop(n, c, GSP_CLOSE_LOCAL_REQUEST);
    return GSP_OK;
}

/* ------------------------------------------------------------------------ */
/* Introspection                                                             */
/* ------------------------------------------------------------------------ */
uint16_t gsp_net_port(const gsp_net *n)
{
    return (n != NULL) ? n->port : 0u;
}

const char *gsp_net_error(const gsp_net *n)
{
    return (n != NULL) ? n->error : "";
}

size_t gsp_net_connection_count(const gsp_net *n)
{
    return (n != NULL) ? (size_t)n->conn_count : 0u;
}

gsp_net_stats gsp_net_get_stats(const gsp_net *n)
{
    gsp_net_stats s;
    if (n == NULL) {
        memset(&s, 0, sizeof(s));
        return s;
    }
    return n->stats;
}

gsp_time_us gsp_net_now_us(void)
{
    return (gsp_time_us)gs_sys_now_us();
}
