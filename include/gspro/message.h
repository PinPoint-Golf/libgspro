/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/message.h — one client→server message, decoded, and the player
 * information the server sends back.
 *
 * ONE MESSAGE TYPE.  design §4.2: every client→server message has one shape
 * (protocol §3) and its KIND — shot, heartbeat, status — is derived from
 * ShotDataOptions (protocol §3.4).  Three types would imply a distinction the
 * wire does not make.
 *
 * PRESENCE IS A BITMASK, AND ZERO IS NOT ABSENCE.  design §4.3: some clients
 * omit an unmeasured key, others send 0.0.  Every numeric section carries a
 * `present` mask; a clear bit means the key was NOT ON THE WIRE and the value
 * must not be read.  A set bit with 0.0 means the client SENT zero, and only
 * the application knows whether that device means it.
 *
 * NUMBERS PASS THROUGH UNCHANGED.  design §4.4: no source states the sign
 * conventions or what Units governs.  Each field below carries the best
 * inference, marked as such.  When protocol U3/U7 close, the comments change
 * and no code does.
 */
#ifndef GSPRO_MESSAGE_H
#define GSPRO_MESSAGE_H

#include "gspro/types.h"
#include "gspro/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever gsp_message's layout changes, so a persisted capture of
 * decoded messages can say which layout it holds.  Sizes are in gsp_abi_sizes. */
#define GSP_MESSAGE_LAYOUT_VERSION 1

/*
 * String bounds.  Every string is NUL-terminated and TRUNCATED on overflow,
 * with GSP_WARN_STRING_TRUNCATED raised — never silent (design §5.4).
 *
 * DeviceID names the sending SOFTWARE, chosen by its author: "GSPro LM 1.1"
 * [GSP], "GSPRO-R10" [R10], "PiTrac LM 1.1" [OSG].  The longest observed is
 * 15 characters; 64 is a label's worth, not a payload's.
 */
#define GSP_DEVICE_ID_MAX    64
#define GSP_UNITS_TEXT_MAX   16
#define GSP_API_VERSION_MAX  8

/* ------------------------------------------------------------------------ */
/* BallData (protocol §3.2)                                                  */
/* ------------------------------------------------------------------------ */
typedef enum gsp_ball_field {
    GSP_BALL_SPEED          = 1u << 0,
    GSP_BALL_SPIN_AXIS      = 1u << 1,
    GSP_BALL_TOTAL_SPIN     = 1u << 2,
    GSP_BALL_BACK_SPIN      = 1u << 3,
    GSP_BALL_SIDE_SPIN      = 1u << 4,
    GSP_BALL_HLA            = 1u << 5,
    GSP_BALL_VLA            = 1u << 6,
    GSP_BALL_CARRY_DISTANCE = 1u << 7
} gsp_ball_field;

/* The five the vendor marks required.  TotalSpin may instead be implied by the
 * BackSpin+SideSpin pair [GSP]; gsp_message_ball_complete() knows that rule. */
#define GSP_BALL_REQUIRED_MASK \
    (GSP_BALL_SPEED | GSP_BALL_SPIN_AXIS | GSP_BALL_TOTAL_SPIN | GSP_BALL_HLA | GSP_BALL_VLA)

typedef struct gsp_ball_data {
    double speed;           /* [GSP] required.  mph under "Yards" (protocol §3.6) */
    double spin_axis;       /* [GSP] required.  degrees.  ⚠ PROBABLE, single-source
                             * [TL]: negative = tilted left (draw for a right-
                             * hander), positive = right.  protocol §3.5        */
    double total_spin;      /* [GSP] required.  rpm                              */
    double back_spin;       /* [GSP] "only required if total spin is not sent".
                             * rpm.  ⚠ [MLM] sends the key as "Backspin"         */
    double side_spin;       /* likewise.  rpm.  ⚠ PROBABLE: negative = left       */
    double hla;             /* [GSP] required.  degrees.  ⚠ PROBABLE: + = right   */
    double vla;             /* [GSP] required.  degrees, + = up                   */
    double carry_distance;  /* [GSP] optional.  in Units.  ⚠ [MLM] sends 0 always */
    uint32_t present;       /* gsp_ball_field bits: has a value                  */
    uint32_t derived;       /* gsp_ball_field bits: that value came from
                             * gsp_ball_data_derive(), not the wire.  Always a
                             * subset of `present`; always 0 from the decoder    */
} gsp_ball_data;

/*
 * THE ONE DERIVATION, AND IT IS EXPLICIT (design §4.3).  TotalSpin/SpinAxis
 * and BackSpin/SideSpin are two representations of one vector; the vendor
 * requires only one, and two clients agree on the arithmetic (protocol §3.2):
 *
 *     total = hypot(back, side)        back = total · cos(axis)
 *     axis  = atan2(side, back) [deg]  side = total · sin(axis)
 *
 * Fills whichever pair is ABSENT from the one that is present, setting the same
 * bits in `present` and in `derived`.
 *
 * ⚠ ONE ZERO IS READ AS "UNMEASURED", AND ONLY THIS ONE: a present TotalSpin of
 * EXACTLY 0.0 beside a non-zero BackSpin/SideSpin pair is treated as absent and
 * recomputed (TOTAL and AXIS marked derived).  PiTrac [PIT] — a real device and
 * one of the two largest user bases in the survey — sends exactly that shape on every shot,
 * and a server that believed it records zero spin for all of them (conformance
 * CT-D13).
 *
 * Otherwise touches nothing that was on the wire — ⚠ including when both pairs
 * are present and disagree, as the vendor's own example does (3250 rpm against
 * a 2625 rpm pair); that is reported by the return value, never "corrected".
 * A no-op if neither pair is complete.
 *
 * Returns GSP_OK if it filled something or nothing needed filling,
 * GSP_PENDING if both pairs were present and inconsistent by more than 1 %
 * (nothing changed), GSP_ERR_INVALID_ARG for NULL.
 *
 * The decoder never calls this; a shot event reports exactly what was on the
 * wire.  The host calls it — PinPoint does, in its mapping.
 */
GSP_API gsp_status gsp_ball_data_derive(gsp_ball_data *ball);

/* ------------------------------------------------------------------------ */
/* ClubData (protocol §3.3)                                                  */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ [GSP] shows all ten as 0.0 and annotates none.  Meanings are by name; no
 * source elaborates.  Clients fill unmeasured members with 0.0 [R10] [TL]
 * [MLM] rather than omitting them, so `present` will usually be all ten and
 * zero will usually mean "not measured".  The application decides.
 */
typedef enum gsp_club_field {
    GSP_CLUB_SPEED                  = 1u << 0,
    GSP_CLUB_ANGLE_OF_ATTACK        = 1u << 1,
    GSP_CLUB_FACE_TO_TARGET         = 1u << 2,
    GSP_CLUB_LIE                    = 1u << 3,
    GSP_CLUB_LOFT                   = 1u << 4,
    GSP_CLUB_PATH                   = 1u << 5,
    GSP_CLUB_SPEED_AT_IMPACT        = 1u << 6,
    GSP_CLUB_VERTICAL_FACE_IMPACT   = 1u << 7,
    GSP_CLUB_HORIZONTAL_FACE_IMPACT = 1u << 8,
    GSP_CLUB_CLOSURE_RATE           = 1u << 9
} gsp_club_field;

typedef struct gsp_club_data {
    double speed;                   /* clubhead speed, as ball speed's unit       */
    double angle_of_attack;         /* degrees, + = up                            */
    double face_to_target;          /* degrees.  ⚠ PROBABLE: + = open / right     */
    double lie;                     /* degrees                                    */
    double loft;                    /* dynamic loft, degrees                      */
    double path;                    /* degrees.  ⚠ PROBABLE: + = in-to-out / right */
    double speed_at_impact;         /* distinct from `speed` by name only         */
    double vertical_face_impact;    /* strike height; unit unstated              */
    double horizontal_face_impact;  /* strike location; unit unstated.
                                     * ⚠ [MLM] puts FACE-TO-PATH here (protocol §3.3) */
    double closure_rate;            /* degrees per second, by name               */
    uint32_t present;               /* gsp_club_field bits                        */
    uint32_t reserved;
} gsp_club_data;

/* ------------------------------------------------------------------------ */
/* ShotDataOptions (protocol §3.4)                                           */
/* ------------------------------------------------------------------------ */
typedef enum gsp_option_field {
    GSP_OPT_CONTAINS_BALL_DATA           = 1u << 0,   /* [GSP] required */
    GSP_OPT_CONTAINS_CLUB_DATA           = 1u << 1,   /* [GSP] required */
    GSP_OPT_LAUNCH_MONITOR_IS_READY      = 1u << 2,   /* [GSP] optional; [CL] 3.0.3.2 */
    GSP_OPT_LAUNCH_MONITOR_BALL_DETECTED = 1u << 3,   /* [GSP] optional */
    GSP_OPT_IS_HEARTBEAT                 = 1u << 4    /* [GSP] optional; [TNB] "retired", [R10] sends it */
} gsp_option_field;

typedef struct gsp_shot_options {
    uint8_t contains_ball_data;
    uint8_t contains_club_data;
    uint8_t launch_monitor_is_ready;
    uint8_t launch_monitor_ball_detected;
    uint8_t is_heartbeat;
    uint8_t present;                /* gsp_option_field bits: which were on the wire */
    uint8_t reserved[2];
} gsp_shot_options;

/* ------------------------------------------------------------------------ */
/* Kind (protocol §3.4's table)                                              */
/* ------------------------------------------------------------------------ */
/*
 * There is no Type field on the wire; the kind is DERIVED from the two
 * Contains flags and IsHeartBeat.  ⚠ Every kind is answered 200 (protocol
 * §4.2): a heartbeat and a status message are "valid shot messages" to GSPro.
 */
typedef enum gsp_message_kind {
    GSP_MSG_NONE      = 0,
    GSP_MSG_SHOT      = 1,   /* ContainsBallData or ContainsClubData true         */
    GSP_MSG_HEARTBEAT = 2,   /* neither, IsHeartBeat true                         */
    GSP_MSG_STATUS    = 3    /* neither, IsHeartBeat false/absent — or no options */
} gsp_message_kind;

/* ------------------------------------------------------------------------ */
/* Validation findings (design §4.5)                                         */
/* ------------------------------------------------------------------------ */
/*
 * A MESSAGE THAT PARSED IS DELIVERED, WHATEVER IT IS MISSING.  "Required" on
 * the vendor's page is what a client should send, not what a server can
 * refuse without losing data from the clients that do not.  So the decoder
 * delivers and FLAGS, and the application decides.
 */
typedef enum gsp_message_flag {
    GSP_MSGF_MISSING_DEVICE_ID        = 1u << 0,
    GSP_MSGF_MISSING_SHOT_NUMBER      = 1u << 1,
    GSP_MSGF_SHOT_NUMBER_NOT_INTEGER  = 1u << 2,  /* 13.5; shot_number is the truncation.
                                                   * ⚠ 13.0 [OSG] is an integer and NOT flagged */
    GSP_MSGF_MISSING_API_VERSION      = 1u << 3,
    GSP_MSGF_UNKNOWN_API_VERSION      = 1u << 4,  /* present and not "1"                       */
    GSP_MSGF_MISSING_OPTIONS          = 1u << 5,  /* no ShotDataOptions; kind is then STATUS    */
    GSP_MSGF_BALL_FLAG_WITHOUT_OBJECT = 1u << 6,  /* ContainsBallData true, no BallData         */
    GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG = 1u << 7,  /* BallData present, flag false/absent        */
    GSP_MSGF_CLUB_FLAG_WITHOUT_OBJECT = 1u << 8,
    GSP_MSGF_CLUB_OBJECT_WITHOUT_FLAG = 1u << 9,
    GSP_MSGF_BALL_INCOMPLETE          = 1u << 10, /* a shot lacking a required ball field —
                                                   * gsp_message_ball_complete() is the rule    */
    GSP_MSGF_UNKNOWN_UNITS            = 1u << 11, /* Units present, neither known string        */
    GSP_MSGF_KEY_CASE_MISMATCH        = 1u << 12, /* a known key matched only case-insensitively:
                                                   * "APIVersion" [R10], "Backspin" [MLM]       */
    GSP_MSGF_UNKNOWN_KEYS             = 1u << 13, /* unknown_keys > 0                           */
    GSP_MSGF_SHOT_NUMBER_REPEATED     = 1u << 14, /* same as this connection's previous shot —
                                                   * [MLM]'s re-send after a slow reply looks
                                                   * exactly like this (protocol §4.4)          */
    GSP_MSGF_STRING_TRUNCATED         = 1u << 15, /* a string exceeded its *_MAX               */
    GSP_MSGF_TYPE_COERCED             = 1u << 16  /* a number or boolean arrived as a JSON
                                                   * STRING ("147.5", "true") and was converted.
                                                   * One client does this [SB]                  */
} gsp_message_flag;

/* ------------------------------------------------------------------------ */
/* The message                                                               */
/* ------------------------------------------------------------------------ */
typedef struct gsp_message {
    gsp_conn_id conn;               /* which connection; GSP_CONN_NONE from the bare decoder */
    uint32_t    sequence;           /* server-wide message counter; a gap means a drop      */
    gsp_time_us host_recv_us;       /* arrival of the message's LAST byte.  ⚠ design §4.1   */

    uint8_t     kind;               /* gsp_message_kind                                     */
    uint8_t     units;              /* gsp_units, decoded from units_text                   */
    uint8_t     reserved0[2];
    uint32_t    flags;              /* gsp_message_flag bits                                */

    /* [GSP] "auto increment from LM".  ⚠ PROVENANCE ONLY, never a key: it
     * restarts when the client restarts, is 0 for [R10] heartbeats, and two
     * clients have independent counters (protocol §4.3). */
    int64_t     shot_number;

    char        device_id[GSP_DEVICE_ID_MAX];
    char        units_text[GSP_UNITS_TEXT_MAX];    /* the string as sent; "" if absent */
    char        api_version[GSP_API_VERSION_MAX];

    gsp_shot_options options;
    gsp_ball_data    ball;          /* meaningful only if options.contains_ball_data
                                     * or GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG            */
    gsp_club_data    club;

    uint16_t    wire_length;        /* bytes of JSON this was decoded from            */
    uint16_t    unknown_keys;       /* keys nothing here recognised — skipped, counted */
    uint8_t     reserved1[4];
} gsp_message;

/* True for GSP_MSG_SHOT. */
GSP_API bool gsp_message_is_shot(const gsp_message *m);

/* True for a SHOT whose ball speed is present and exactly zero.  ⚠ Not a flag,
 * because zero is a value (design §4.3) — but [SLX] shows a commercial connector
 * emitting such "shots", and GSPro reacting to them by resetting the club, so
 * the question is worth a function.  The host decides; PinPoint drops them. */
GSP_API bool gsp_message_ball_zero_speed(const gsp_message *m);

/* The vendor's completeness rule for BallData (protocol §3.2): Speed, HLA and
 * VLA present; TotalSpin present or the BackSpin+SideSpin pair present; and
 * SpinAxis present or derivable from that pair.  What GSP_MSGF_BALL_INCOMPLETE
 * is computed from, published so a host can apply it to a message it built. */
GSP_API bool gsp_message_ball_complete(const gsp_ball_data *ball);

/* Human-readable, for logs: "shot #13 from \"GSPRO-R10\" ball[speed spin_axis …]
 * flags[BALL_INCOMPLETE]".  ⚠ device_id is an identifier (design §9.2);
 * `include_identifiers` false writes "<redacted>" in its place.  `out` is
 * always NUL-terminated; returns the length that would have been written. */
GSP_API size_t gsp_message_format(const gsp_message *m, char *out, size_t out_size,
                                  bool include_identifiers);

/* Names one flag bit, "BALL_INCOMPLETE"; "" for a value that is not one flag. */
GSP_API const char *gsp_message_flag_name(gsp_message_flag flag);

/* ------------------------------------------------------------------------ */
/* Player information — the 201 the SERVER sends (protocol §5.2)             */
/* ------------------------------------------------------------------------ */
/*
 * The application's "game state" as far as a launch monitor cares: which hand,
 * which club, how far to the target.  [GSP]: "typically used for Launch
 * monitors that need to switch between full strike clubs and putting."
 *
 * UNKNOWN handed or club is OMITTED from the JSON rather than sent as a string
 * GSPro never sends.  distance_to_target is sent only when has_distance, in
 * whatever unit the host chose — the library converts nothing (design §5.6).
 */
#define GSP_SURFACE_MAX 16

typedef struct gsp_player_info {
    uint8_t handed;                 /* gsp_handed */
    uint8_t club;                   /* gsp_club   */
    uint8_t has_distance;
    uint8_t reserved[5];
    double  distance_to_target;     /* ⚠ DistanceToTarget is not on [GSP]; [TNB] [R10] [OSP].
                                     * [OSP] arms its device on a NON-ZERO value, so send
                                     * a real distance when one is known */
    char    surface[GSP_SURFACE_MAX]; /* ⚠ Surface: [OSP] only, vocabulary unknown (protocol
                                     * U12).  Sent when non-empty; PinPoint leaves it "" */
} gsp_player_info;

/* True if the two would serialise identically — what gsp_server_set_player()
 * uses to decide whether a 201 goes out. */
GSP_API bool gsp_player_info_equal(const gsp_player_info *a, const gsp_player_info *b);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_MESSAGE_H */
