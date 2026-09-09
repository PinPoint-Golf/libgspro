/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/server.h — the sans-I/O GSPro Connect listener.
 *
 * ============================================================================
 * THE THREADING CONTRACT.  Three sentences.
 * ============================================================================
 * 1. Every gsp_server_* function must be called from ONE thread — the same
 *    thread for the whole life of the server.  The library contains no locks,
 *    no atomics and no threads, so this is not advice.
 * 2. Nothing here is any-thread; the only objects that leave the server thread
 *    are gsp_event / gsp_write_request / gsp_wire_chunk COPIES, which are POD.
 * 3. After gsp_server_close() returns, no further event or write can be
 *    produced by anything the library owns — because the library never pushes;
 *    the host drains.  Destroying a socket, a buffer or a closure the consumer
 *    passed in is safe from that instant onwards.
 * ============================================================================
 *
 * ⚠ THE LIBRARY DOES NOT OWN A SOCKET.  design §2.  There is no listen(), no
 * accept(), no thread, no timer, no sleep and no clock read.  The host — Qt's
 * QTcpServer, asyncio, POSIX select, Winsock — owns all of that and tells the
 * library what happened.  The host's loop is:
 *
 *     on accept ................. gsp_server_on_connection_opened(s, id, peer, now)
 *     on bytes read ............. gsp_server_on_bytes(s, id, p, n, now)
 *     on socket closed .......... gsp_server_on_connection_closed(s, id, cause, now)
 *     when the app's club/hand
 *       changes ................. gsp_server_set_player(s, &info, now)
 *     arm one timer to .......... gsp_server_next_due_us(s)   (usually NEVER)
 *     when it fires ............. gsp_server_tick(s, now)
 *     then always ............... drain poll_writes / poll_events (/ poll_wire)
 *                                 and write each request to ITS connection
 *
 * That is the whole integration.  design §3.2 has it in Qt.
 */
#ifndef GSPRO_SERVER_H
#define GSPRO_SERVER_H

#include "gspro/types.h"
#include "gspro/protocol.h"
#include "gspro/message.h"
#include "gspro/event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gsp_server gsp_server;

/* ------------------------------------------------------------------------ */
/* Writes the host must perform                                              */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ EVERY BYTE THIS LIBRARY CAN EMIT IS ONE OF FIVE MESSAGE SHAPES — a 200
 * acknowledgement, a 201 player information, a 202 ready, a 203 round ended,
 * a 501 failure — composed by gsp_response_encode() (design §9.1).  There is
 * no send_raw().  The test suite round-trips every write the ring can produce
 * through gsp_response_decode() and asserts the code is one of those five.
 *
 * A 201 with every member present is ~110 bytes; 256 is the by-value bound.
 * Write `data[0..length)` to the connection `conn` names.  ⚠ ONE WRITE PER
 * REQUEST, IN RING ORDER: a shot's 200 followed by a club change's 201 must
 * reach the client in that order (design §5.6).
 */
#define GSP_WRITE_MAX 256

typedef enum gsp_write_kind {
    GSP_WRITE_ACK = 0,          /* 200 — one per well-formed object received  */
    GSP_WRITE_PLAYER_INFO,      /* 201                                        */
    GSP_WRITE_READY,            /* 202                                        */
    GSP_WRITE_ROUND_ENDED,      /* 203                                        */
    GSP_WRITE_FAILURE           /* 501                                        */
} gsp_write_kind;

typedef struct gsp_write_request {
    gsp_conn_id conn;
    uint16_t    length;
    uint8_t     kind;           /* gsp_write_kind, for the log                */
    uint8_t     reserved;
    uint8_t     data[GSP_WRITE_MAX];
} gsp_write_request;

/* ------------------------------------------------------------------------ */
/* Wire log — byte-level record (design §7)                                  */
/* ------------------------------------------------------------------------ */
/*
 * RECORD THE BYTES, NOT THE DECODED MESSAGES.  protocol §11 lists ten open
 * questions; when one closes, a byte-level capture re-decodes with the fix
 * applied and a decoded log cannot.  The first capture against a real launch
 * monitor is the fixture that pins the decoder.
 *
 * The core does no file I/O.  It buffers chunks into the ring the config
 * sized (default: off) and the host drains them with gsp_server_poll_wire().
 * A message longer than a chunk spans several with GSP_WIRE_CONTINUES set.
 */
#define GSP_WIRE_CHUNK_MAX 512

typedef enum gsp_wire_direction {
    GSP_WIRE_CLIENT_TO_SERVER = 0,
    GSP_WIRE_SERVER_TO_CLIENT = 1,
    GSP_WIRE_META             = 2   /* open / close, with the cause in data     */
} gsp_wire_direction;

typedef enum gsp_wire_flag {
    /* ⚠ Identifiers were removed before this chunk was queued: the peer
     * address in a META chunk, and — because it sits inside the JSON — the
     * DeviceID value, replaced by "<redacted>" of equal length so offsets
     * hold.  Off unless policy.record_identifiers (design §9.2). */
    GSP_WIRE_REDACTED  = 1u << 0,
    GSP_WIRE_LOST      = 1u << 1,   /* chunks were dropped before this one      */
    GSP_WIRE_CONTINUES = 1u << 2    /* the next chunk continues this message    */
} gsp_wire_flag;

typedef struct gsp_wire_chunk {
    gsp_time_us host_time_us;
    gsp_conn_id conn;
    uint16_t    length;
    uint8_t     direction;      /* gsp_wire_direction */
    uint8_t     flags;          /* gsp_wire_flag      */
    uint32_t    sequence;
    uint32_t    reserved;
    uint8_t     data[GSP_WIRE_CHUNK_MAX];
} gsp_wire_chunk;

/* ------------------------------------------------------------------------ */
/* Policy                                                                    */
/* ------------------------------------------------------------------------ */
/*
 * Every field answers one of protocol §11's unknowns, or chooses between two
 * behaviours real clients tolerate.  Each is a switch rather than a constant
 * because the first session against real hardware is expected to flip some.
 */
#define GSP_ACK_TEXT_MAX 64

typedef struct gsp_server_policy {
    /*
     * The framer's bound, and the size of each connection's receive buffer.
     * 0 → 16 KiB: twenty times the vendor's full example, a quarter of [OSG]'s
     * bound, and nothing legitimate approaches it.  An object that grows past
     * this is discarded with GSP_PE_TOO_LARGE and a 501 (design §5.3).
     */
    uint32_t max_message_bytes;

    /*
     * ⚠ THE LIBRARY NEVER DROPS A CONNECTION FOR SILENCE (protocol §9.6): a
     * client may connect and say nothing for a whole warm-up, and no client
     * sends periodic heartbeats.  This alarm is for a UI that wants to grey out
     * a launch monitor that has gone quiet: GSP_EV_CLIENT_IDLE once after this
     * long with no message, GSP_EV_CLIENT_ACTIVE when it next speaks.
     * 0 → off, and then gsp_server_next_due_us() is always GSP_TIME_NEVER.
     */
    gsp_time_us idle_alarm_us;

    /*
     * After this many CONSECUTIVE protocol errors on one connection, emit
     * GSP_EV_CLOSE_REQUESTED so the host can close it.  The library cannot
     * close anything and a well-formed object resets the count.  0 → never;
     * a hostile client then merely occupies its own connection being told 501.
     */
    uint32_t protocol_error_close_threshold;

    /*
     * Hold a write to a connection back until this long after the PREVIOUS write
     * to that connection was polled, so that a 200 and a 201 do not share a TCP
     * segment.  ⚠ Six clients parse one read as one message, and PiTrac's
     * receive thread DIES when they share one (protocol §9.8, conformance
     * CT-T03).  Uses the tick/next_due mechanism; 0 → off.  20 ms is plenty.
     * It cannot make coalescing impossible — nothing can — and the host must
     * still issue one socket write per request with TCP_NODELAY.
     */
    gsp_time_us write_spacing_us;

    /*
     * Answer a shot flagged GSP_MSGF_BALL_INCOMPLETE with 501 instead of 200.
     * ⚠ OFF BY DEFAULT: protocol U2/U8 have not established what GSPro does,
     * a client told 501 may retry the same shot forever, and the shot is
     * delivered with its flags either way — the host can drop it itself.
     */
    bool reject_incomplete_shots;

    /*
     * Send the stored player information (gsp_server_set_player) to every new
     * connection immediately.  ⚠ Whether GSPro does this is protocol U1 —
     * weak evidence says it does not.  ON by default because a connector that
     * needs the club before the first strike is better served, and no client
     * examined is harmed by an extra 201.  Sends nothing if no player has been
     * set, or if handed and club are both UNKNOWN and there is no distance.
     */
    bool announce_player_on_connect;

    /*
     * Send a 202 "GSPro ready" to every new connection while the session state
     * is GSP_SESSION_ACTIVE, after the 201.  ⚠ [OSP]-style clients arm their
     * launch monitor only after a 202 (protocol §5.1), so a connector that
     * arrives mid-session would otherwise wait forever.  ON by default.
     */
    bool announce_ready_on_connect;

    /*
     * ⚠ Copy peer addresses and DeviceID values into the wire log rather than
     * redacting them.  Off by default; a consumer that turns on verbose logging
     * should not have to think about it (design §9.2).
     */
    bool record_identifiers;

    uint8_t reserved[4];

    /*
     * The Message text of the 200.  "" → GSP_TEXT_SHOT_RECEIVED, the string
     * [GSP] lists.  ⚠ [TNB] says the real GSPro misspells it (protocol U4); no
     * client keys on the text, so the documented spelling stays until a
     * capture says otherwise.  A host that has captured the real one sets it.
     */
    char ack_text[GSP_ACK_TEXT_MAX];
} gsp_server_policy;

GSP_API gsp_server_policy gsp_server_policy_default(void);

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */
/*
 * Sizes are in ITEMS.  0 takes the default.  gsp_server_create() sizes its
 * single allocation from these and nothing allocates afterwards (design §3.4).
 *
 * Messages are ~300-900 bytes and arrive seconds apart, so the defaults are
 * generous by an order of magnitude and cost ~100 KB together.
 */
#define GSP_MAX_CONNECTIONS_DEFAULT 4
#define GSP_EVENT_RING_DEFAULT      64
#define GSP_WRITE_RING_DEFAULT      32
#define GSP_WIRE_RING_RECOMMENDED   256    /* the wire ring is OFF unless asked */

typedef struct gsp_server_config {
    /* Simultaneous connections.  ⚠ Permissive on purpose (design §5.1): a
     * putting device beside a full-swing device, or a connector reconnecting
     * before its old socket has closed, must not be refused.  0 → 4. */
    uint32_t max_connections;

    /* Drop-oldest, counted by gsp_server_dropped_events().  0 → 64. */
    uint32_t event_ring;

    /* ⚠ NOT drop-oldest: a dropped reply re-sends a shot from [MLM] (protocol
     * §4.4).  When full, on_bytes returns GSP_ERR_QUEUE_FULL WITHOUT consuming
     * the message; drain writes and call on_bytes again with zero bytes.  A
     * host that drains after every call never sees it.  0 → 32. */
    uint32_t write_ring;

    /* 0 → the wire log is OFF.  GSP_WIRE_RING_RECOMMENDED for a capture. */
    uint32_t wire_ring;

    gsp_server_policy policy;
    gsp_allocator     allocator;   /* zeroed → malloc/free */
} gsp_server_config;

GSP_API gsp_server_config gsp_server_config_default(void);

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

/* ONE allocation.  GSP_ERR_INVALID_ARG for an unusable config (a policy
 * bound below 256 bytes, say); GSP_ERR_NO_MEMORY if the allocator refused. */
GSP_API gsp_status gsp_server_create(const gsp_server_config *config, gsp_server **out);

/* Seals every queue.  After this returns nothing can be produced; every
 * further call returns GSP_ERR_CLOSED except the polls, which drain what is
 * already queued, and destroy.  Every open connection is reported closed
 * with GSP_CLOSE_SERVER_CLOSED first, so a host that drains once more after
 * closing sees a complete log. */
GSP_API void gsp_server_close(gsp_server *s);

/* ONE free.  NULL is a no-op. */
GSP_API void gsp_server_destroy(gsp_server *s);

/* ------------------------------------------------------------------------ */
/* IN — from the host's socket code                                          */
/* ------------------------------------------------------------------------ */

/*
 * The host accepted a connection and chose `conn` (non-zero) for it.  `peer`
 * is an optional textual address for logs and gsp_connection_info — ⚠ it is
 * personal data (design §9.2) and may be NULL.
 *
 * GSP_ERR_INVALID_STATE if `conn` is already open; GSP_ERR_TOO_MANY_CONNECTIONS
 * at the limit, and the host should close the socket — the protocol has no
 * message that could tell the client why.  Queues a 201 immediately if
 * policy.announce_player_on_connect and a player is set.
 */
GSP_API gsp_status gsp_server_on_connection_opened(gsp_server *s, gsp_conn_id conn,
                                                   const char *peer, gsp_time_us now_us);

/*
 * Bytes arrived on `conn`.  ANY LENGTH — one byte, half a message, three
 * messages: TCP preserves nothing and the framer reassembles (design §3.2.1).
 * Hand the bytes over exactly as read; do not split on newlines, do not parse
 * first, do not wait for "a whole message".
 *
 * On return every complete object in the buffer has been decoded, its reply
 * queued FIRST and its event second (so a full event ring drops the event and
 * never the acknowledgement), and the wire log updated.  `len` may be 0 to
 * resume after GSP_ERR_QUEUE_FULL.
 *
 * GSP_ERR_UNKNOWN_CONNECTION if `conn` is not open.  GSP_ERR_QUEUE_FULL if the
 * write ring filled — the unconsumed bytes are kept; drain and call again.
 * A protocol error on the wire is NOT an error return: it is a
 * GSP_EV_PROTOCOL_ERROR event and a queued 501, and processing continues at
 * the next '{'.
 */
GSP_API gsp_status gsp_server_on_bytes(gsp_server *s, gsp_conn_id conn,
                                       const uint8_t *data, size_t len, gsp_time_us now_us);

/* The socket is gone, for the reason the host knows.  Emits
 * GSP_EV_CONNECTION_CLOSED with the final counters and frees the slot.
 * Pending write requests for this connection are discarded. */
GSP_API gsp_status gsp_server_on_connection_closed(gsp_server *s, gsp_conn_id conn,
                                                   gsp_close_cause cause, gsp_time_us now_us);

/* ------------------------------------------------------------------------ */
/* CLOCK — the host owns the timer                                           */
/* ------------------------------------------------------------------------ */

/* When the server next needs gsp_server_tick().  GSP_TIME_NEVER unless
 * policy.idle_alarm_us or policy.write_spacing_us is set — the protocol has no
 * deadline of its own.  Re-read after EVERY call into the server, and after
 * every poll_writes(): a held-back write becomes due then. */
GSP_API gsp_time_us gsp_server_next_due_us(const gsp_server *s);

/* Runs whatever is due.  A no-op when nothing is; may be called at any rate. */
GSP_API void gsp_server_tick(gsp_server *s, gsp_time_us now_us);

/* ------------------------------------------------------------------------ */
/* HOST → CLIENTS — the only thing the application originates               */
/* ------------------------------------------------------------------------ */

/*
 * The application's game state changed: hand, club, distance to target.
 * Stores it and, IF IT DIFFERS from the stored value (gsp_player_info_equal),
 * queues a 201 to every open connection and a GSP_EV_PLAYER_INFO_SENT per
 * connection.  Identical values queue nothing — a host may call this on every
 * UI change without spamming clients.  design §5.6.
 *
 * ⚠ The 201 rides the same write ring as the acknowledgements, in order: after
 * a shot the client typically sees {200}{201} in one read, which is exactly
 * the concatenation every examined client's splitter handles.
 */
GSP_API gsp_status gsp_server_set_player(gsp_server *s, const gsp_player_info *info,
                                         gsp_time_us now_us);

/* The stored value; zeroed (all UNKNOWN) if never set. */
GSP_API void gsp_server_get_player(const gsp_server *s, gsp_player_info *out);

/* Sends one 201 unconditionally to `conn`, or to every connection with
 * GSP_CONN_ALL, without changing the stored value.  For re-announcing, or for
 * exercising a client. */
GSP_API gsp_status gsp_server_send_player_info(gsp_server *s, gsp_conn_id conn,
                                               const gsp_player_info *info, gsp_time_us now_us);

/*
 * The 202 / 203 pair (design §5.6.1).  GSPro sends {"Code":202,"Message":
 * "GSPro ready"} when a match or hole starts and {"Code":203,"Message":
 * "GSPro round ended"} when it ends — ⚠ and [OSP]-style clients do not arm
 * their launch monitor until they have seen the 202.  A server emulating GSPro
 * without them leaves such a client waiting.
 *
 * ACTIVE queues a 202 to every open connection; ENDED queues a 203; NONE
 * queues nothing and clears the state.  A repeated value queues nothing.  The
 * strings are fixed (GSP_TEXT_READY / GSP_TEXT_ROUND_ENDED): [OSP] matches
 * them exactly.  PinPoint maps session start and stop onto this.
 */
typedef enum gsp_session_state {
    GSP_SESSION_NONE = 0,       /* nothing has been said; nothing is sent on connect */
    GSP_SESSION_ACTIVE,         /* → 202 now, and on connect if policy says so  */
    GSP_SESSION_ENDED           /* → 203 now                                    */
} gsp_session_state;

GSP_API gsp_status gsp_server_set_session_state(gsp_server *s, gsp_session_state state,
                                                gsp_time_us now_us);
GSP_API gsp_session_state gsp_server_get_session_state(const gsp_server *s);

/* ------------------------------------------------------------------------ */
/* OUT — drained by the host, never pushed into it                          */
/* ------------------------------------------------------------------------ */

/* Each fills up to `max` items and returns how many.  Call until 0. */
GSP_API size_t gsp_server_poll_writes(gsp_server *s, gsp_write_request *out, size_t max);
GSP_API size_t gsp_server_poll_events(gsp_server *s, gsp_event *out, size_t max);
GSP_API size_t gsp_server_poll_wire(gsp_server *s, gsp_wire_chunk *out, size_t max);

/* Lifetime drop counts for the two drop-oldest rings.  A host that is not
 * keeping up must be able to see it. */
GSP_API uint32_t gsp_server_dropped_events(const gsp_server *s);
GSP_API uint32_t gsp_server_dropped_wire(const gsp_server *s);

/* ------------------------------------------------------------------------ */
/* Introspection                                                             */
/* ------------------------------------------------------------------------ */

GSP_API size_t gsp_server_connection_count(const gsp_server *s);

/* The open connection ids, up to `max`; returns the count open (which may
 * exceed `max`).  `out` may be NULL to count. */
GSP_API size_t gsp_server_connection_ids(const gsp_server *s, gsp_conn_id *out, size_t max);

/* Current state of one connection.  GSP_ERR_UNKNOWN_CONNECTION if not open. */
GSP_API gsp_status gsp_server_connection_info(const gsp_server *s, gsp_conn_id conn,
                                              gsp_connection_info *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_SERVER_H */
