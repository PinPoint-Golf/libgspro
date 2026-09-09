/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/event.h — everything the server has to say, as drained POD.
 *
 * ⚠ THE LIBRARY NEVER INVOKES A CONSUMER CALLBACK.  design §3.3.  Events are
 * queued and the host drains them, which makes the teardown barrier "stop
 * draining" — a guarantee the consumer implements itself and cannot get wrong.
 *
 * Every event is a fixed-size POD with no pointers, so an event survives being
 * copied, queued, logged or sent across a thread boundary.  An event carrying
 * a message carries the WHOLE gsp_message inline, by value.
 */
#ifndef GSPRO_EVENT_H
#define GSPRO_EVENT_H

#include "gspro/types.h"
#include "gspro/message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gsp_event_type {
    GSP_EV_NONE = 0,

    /* Connections (design §5.1, §5.2) ---------------------------------- */
    GSP_EV_CONNECTION_OPENED,   /* the host reported one; peer in .connection  */
    GSP_EV_CONNECTION_CLOSED,   /* with cause and final counters               */
    GSP_EV_CLIENT_IDENTIFIED,   /* first message seen: device_id is now known  */

    /* Messages (protocol §3.4) — all three carry .message ---------------- */
    GSP_EV_SHOT,                /* kind == GSP_MSG_SHOT                        */
    GSP_EV_HEARTBEAT,           /* kind == GSP_MSG_HEARTBEAT                   */
    GSP_EV_STATUS,              /* kind == GSP_MSG_STATUS                      */

    /* Derived state ----------------------------------------------------- */
    GSP_EV_CLIENT_STATE,        /* ready / ball-detected changed; for a UI pill */
    GSP_EV_PLAYER_INFO_SENT,    /* a 201 was queued, and why                   */
    GSP_EV_SESSION_STATE_SENT,  /* a 202 or 203 was queued, and why            */

    /* Trouble ----------------------------------------------------------- */
    GSP_EV_PROTOCOL_ERROR,      /* garbage, bad JSON, oversize; a 501 went out */
    GSP_EV_CLOSE_REQUESTED,     /* ⚠ policy threshold hit: the HOST should close
                                 * this connection.  The library cannot        */
    GSP_EV_CLIENT_IDLE,         /* policy.idle_alarm_us elapsed with no message */
    GSP_EV_CLIENT_ACTIVE,       /* …and it spoke again                          */
    GSP_EV_WARNING,

    GSP_EVENT_TYPE_COUNT
} gsp_event_type;

/* ------------------------------------------------------------------------ */
/* Connections                                                               */
/* ------------------------------------------------------------------------ */
/*
 * Why a connection went away, AS THE HOST REPORTED IT in
 * gsp_server_on_connection_closed().  The library sees no socket and cannot
 * classify on its own; it records what it was told.
 */
typedef enum gsp_close_cause {
    GSP_CLOSE_UNKNOWN = 0,
    GSP_CLOSE_REMOTE_CLOSED,    /* the client hung up — the ordinary case      */
    GSP_CLOSE_LOCAL_REQUEST,    /* the host closed it, e.g. after CLOSE_REQUESTED */
    GSP_CLOSE_TRANSPORT_ERROR,
    GSP_CLOSE_SERVER_CLOSED     /* gsp_server_close() sealed everything         */
} gsp_close_cause;

/* ⚠ The peer address identifies a household (design §9.2).  Redacted from
 * gsp_event_format() and the wire log unless asked. */
#define GSP_PEER_MAX 64

/*
 * A connection's state, readable at any time via gsp_server_connection_info()
 * and snapshotted into CONNECTION_OPENED (peer only) and CONNECTION_CLOSED
 * (everything).  A UI's "GSPRO ● 14 shots" pill needs no bookkeeping of its
 * own.
 */
typedef struct gsp_connection_info {
    gsp_conn_id conn;
    uint8_t     identified;         /* device_id has been seen                  */
    uint8_t     ready;              /* last LaunchMonitorIsReady seen, else 0   */
    uint8_t     ball_detected;      /* last LaunchMonitorBallDetected, else 0   */
    uint8_t     reserved0;
    gsp_time_us opened_us;
    gsp_time_us last_message_us;    /* GSP_TIME_UNKNOWN until the first          */
    uint32_t    messages;           /* every well-formed object                  */
    uint32_t    shots;              /* of which kind == SHOT                     */
    uint32_t    protocol_errors;    /* lifetime                                  */
    uint32_t    consecutive_errors; /* since the last well-formed object         */
    int64_t     last_shot_number;   /* for GSP_MSGF_SHOT_NUMBER_REPEATED; INT64_MIN if none */
    char        peer[GSP_PEER_MAX];         /* as the host supplied it; "" if not */
    char        device_id[GSP_DEVICE_ID_MAX]; /* "" until identified              */
} gsp_connection_info;

typedef struct gsp_connection_event {
    gsp_connection_info info;
    uint8_t  cause;                 /* gsp_close_cause; UNKNOWN on OPENED       */
    uint8_t  reserved[7];
} gsp_connection_event;

/* ------------------------------------------------------------------------ */
/* Client state                                                              */
/* ------------------------------------------------------------------------ */
/* Emitted only on CHANGE of either flag, never per message — [R10] sends both
 * on every heartbeat and a UI does not want to repaint for that. */
typedef struct gsp_client_state_event {
    uint8_t ready;
    uint8_t ball_detected;
    uint8_t previous_ready;
    uint8_t previous_ball_detected;
    uint8_t reserved[4];
} gsp_client_state_event;

/* ------------------------------------------------------------------------ */
/* Player information sent                                                   */
/* ------------------------------------------------------------------------ */
typedef enum gsp_player_info_reason {
    GSP_PI_CHANGED = 0,         /* gsp_server_set_player() with a new value    */
    GSP_PI_ON_CONNECT,          /* policy.announce_player_on_connect            */
    GSP_PI_EXPLICIT             /* gsp_server_send_player_info()                */
} gsp_player_info_reason;

typedef struct gsp_player_info_event {
    gsp_player_info player;
    uint8_t reason;             /* gsp_player_info_reason                       */
    uint8_t reserved[7];
} gsp_player_info_event;

/* ------------------------------------------------------------------------ */
/* Session state sent (design §5.6.1)                                        */
/* ------------------------------------------------------------------------ */
typedef struct gsp_session_state_event {
    uint8_t state;              /* gsp_session_state (server.h): ACTIVE → 202, ENDED → 203 */
    uint8_t reason;             /* gsp_player_info_reason: CHANGED or ON_CONNECT */
    uint8_t reserved[6];
} gsp_session_state_event;

/* ------------------------------------------------------------------------ */
/* Protocol error                                                            */
/* ------------------------------------------------------------------------ */
typedef enum gsp_protocol_error_reason {
    GSP_PE_LEADING_GARBAGE = 0, /* a byte before '{' that is not whitespace    */
    GSP_PE_BAD_JSON,            /* braces balanced, content not an object      */
    GSP_PE_TOO_LARGE            /* policy.max_message_bytes exceeded            */
} gsp_protocol_error_reason;

#define GSP_ERROR_SNIPPET_MAX 32

typedef struct gsp_protocol_error_event {
    uint8_t  reason;            /* gsp_protocol_error_reason                    */
    uint8_t  snippet_length;
    uint8_t  reserved[2];
    uint32_t discarded_bytes;   /* how much of the buffer was thrown away       */
    uint32_t consecutive;       /* this connection's run of errors, this one included */
    uint8_t  snippet[GSP_ERROR_SNIPPET_MAX]; /* the first bytes, raw, for the log */
} gsp_protocol_error_event;

/* ------------------------------------------------------------------------ */
/* Warnings                                                                  */
/* ------------------------------------------------------------------------ */
typedef enum gsp_warning_code {
    GSP_WARN_NONE = 0,
    GSP_WARN_DEVICE_ID_CHANGED,     /* a later message named a different DeviceID;
                                     * the connection keeps its first (design §5.2) */
    GSP_WARN_STRING_TRUNCATED,      /* a wire string exceeded its *_MAX            */
    GSP_WARN_EVENTS_DROPPED,        /* the event ring overflowed; value = count    */
    GSP_WARN_WIRE_DROPPED,          /* the wire ring overflowed; value = count     */
    GSP_WARN_WRITE_RING_FULL,       /* on_bytes returned GSP_ERR_QUEUE_FULL        */
    GSP_WARNING_CODE_COUNT
} gsp_warning_code;

#define GSP_WARNING_TEXT_MAX 64

typedef struct gsp_warning_event {
    uint16_t code;              /* gsp_warning_code */
    uint16_t reserved;
    uint32_t value;
    char     text[GSP_WARNING_TEXT_MAX];  /* human-readable; may carry an identifier
                                           * — check gsp_event_is_sensitive()  */
} gsp_warning_event;

/* ------------------------------------------------------------------------ */
/* The event                                                                 */
/* ------------------------------------------------------------------------ */
typedef struct gsp_event {
    uint8_t     type;           /* gsp_event_type                               */
    uint8_t     reserved0[3];
    uint32_t    sequence;       /* server-wide; a gap means the ring dropped    */
    gsp_time_us host_time_us;   /* the now_us of the call that produced it      */
    gsp_conn_id conn;           /* GSP_CONN_NONE for server-wide events         */
    uint32_t    reserved1;
    union {
        gsp_connection_event     connection;     /* OPENED, CLOSED, IDENTIFIED */
        gsp_message              message;        /* SHOT, HEARTBEAT, STATUS    */
        gsp_client_state_event   client_state;   /* CLIENT_STATE               */
        gsp_player_info_event    player_info;    /* PLAYER_INFO_SENT           */
        gsp_session_state_event  session_state;  /* SESSION_STATE_SENT         */
        gsp_protocol_error_event protocol_error; /* PROTOCOL_ERROR, CLOSE_REQUESTED */
        gsp_warning_event        warning;        /* WARNING                    */
    } u;
} gsp_event;

/* "SHOT", "PROTOCOL_ERROR", …  Stable, never NULL. */
GSP_API const char *gsp_event_type_name(gsp_event_type type);

/* True for exactly the events that can carry a peer address or a DeviceID:
 * CONNECTION_*, CLIENT_IDENTIFIED, SHOT/HEARTBEAT/STATUS, and a WARNING whose
 * text names one.  design §9.2. */
GSP_API bool gsp_event_is_sensitive(const gsp_event *ev);

/* One line, for a log.  `include_identifiers` false redacts peer and
 * device_id.  A test formats EVERY event type with identifiers populated and
 * asserts none leaks through the redacted form.  `out` is always
 * NUL-terminated; returns the length that would have been written. */
GSP_API size_t gsp_event_format(const gsp_event *ev, char *out, size_t out_size,
                                bool include_identifiers);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_EVENT_H */
