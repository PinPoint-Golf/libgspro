/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/protocol.h — the wire vocabulary of GSPro Open Connect v1, as data.
 *
 * Every constant here is a fact about the protocol with a provenance tag in
 * docs/protocol.md §0.  They are published so that a host, a tool or a binding
 * never holds a second copy of the port, a key name or the club vocabulary:
 * a second copy is a second thing that can drift.
 *
 *   [GSP]  the vendor's page          [TNB] [R10] [MLM] ...  client implementations
 */
#ifndef GSPRO_PROTOCOL_H
#define GSPRO_PROTOCOL_H

#include "gspro/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Transport (protocol §1)                                                   */
/* ------------------------------------------------------------------------ */

/* [GSP] writes it "0921".  Decimal 921; every client defaults to it. */
#define GSP_DEFAULT_PORT 921

/* The ONLY other port GSPro Connect itself can use: [SLX] documents
 * <OpenAPIUseAltPort>true</OpenAPIUseAltPort> in GSPconnect.exe.config moving
 * it to 922.  A host sharing a machine with GSPro offers this (design §6.4). */
#define GSP_ALT_PORT 922

/* [GSP]: "It is assumed that the launch monitor software is being ran on the
 * same PC".  ⚠ The library does not bind anything (design §3.2); this is the
 * documented default for a host that offers one. */
#define GSP_DEFAULT_ADDRESS "127.0.0.1"

/* [GSP]: `"1" is current version`.  ⚠ A string, not a number. */
#define GSP_API_VERSION_TEXT "1"

/* ------------------------------------------------------------------------ */
/* Key names (protocol §3, Appendix A)                                       */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ MATCHED CASE-INSENSITIVELY ON RECEIPT (protocol §9.1): a widely used client
 * sends "APIVersion" [R10] and another sends "Backspin" [MLM], and both work
 * against GSPro.  These are the vendor's spellings, used when the library
 * EMITS and as the canonical form in every table.
 */
#define GSP_KEY_DEVICE_ID          "DeviceID"
#define GSP_KEY_UNITS              "Units"
#define GSP_KEY_SHOT_NUMBER        "ShotNumber"
#define GSP_KEY_API_VERSION        "APIversion"     /* ⚠ lower-case v [GSP] */
#define GSP_KEY_BALL_DATA          "BallData"
#define GSP_KEY_CLUB_DATA          "ClubData"
#define GSP_KEY_SHOT_DATA_OPTIONS  "ShotDataOptions"

#define GSP_KEY_BALL_SPEED         "Speed"
#define GSP_KEY_BALL_SPIN_AXIS     "SpinAxis"
#define GSP_KEY_BALL_TOTAL_SPIN    "TotalSpin"
#define GSP_KEY_BALL_BACK_SPIN     "BackSpin"
#define GSP_KEY_BALL_SIDE_SPIN     "SideSpin"
#define GSP_KEY_BALL_HLA           "HLA"
#define GSP_KEY_BALL_VLA           "VLA"
#define GSP_KEY_BALL_CARRY         "CarryDistance"

#define GSP_KEY_CLUB_SPEED         "Speed"
#define GSP_KEY_CLUB_AOA           "AngleOfAttack"
#define GSP_KEY_CLUB_FACE_TO_TARGET "FaceToTarget"
#define GSP_KEY_CLUB_LIE           "Lie"
#define GSP_KEY_CLUB_LOFT          "Loft"
#define GSP_KEY_CLUB_PATH          "Path"
#define GSP_KEY_CLUB_SPEED_AT_IMPACT "SpeedAtImpact"
#define GSP_KEY_CLUB_VFI           "VerticalFaceImpact"
#define GSP_KEY_CLUB_HFI           "HorizontalFaceImpact"
#define GSP_KEY_CLUB_CLOSURE_RATE  "ClosureRate"

#define GSP_KEY_OPT_CONTAINS_BALL  "ContainsBallData"
#define GSP_KEY_OPT_CONTAINS_CLUB  "ContainsClubData"
#define GSP_KEY_OPT_LM_READY       "LaunchMonitorIsReady"
#define GSP_KEY_OPT_BALL_DETECTED  "LaunchMonitorBallDetected"
#define GSP_KEY_OPT_HEARTBEAT      "IsHeartBeat"

#define GSP_KEY_RESP_CODE          "Code"
#define GSP_KEY_RESP_MESSAGE       "Message"
#define GSP_KEY_RESP_PLAYER        "Player"
#define GSP_KEY_PLAYER_HANDED      "Handed"
#define GSP_KEY_PLAYER_CLUB        "Club"
#define GSP_KEY_PLAYER_DISTANCE    "DistanceToTarget"  /* ⚠ not on [GSP]; [TNB] [R10] [OSP] */
#define GSP_KEY_PLAYER_SURFACE     "Surface"           /* ⚠ [OSP] only; vocabulary unknown */

/* ------------------------------------------------------------------------ */
/* Response codes (protocol §5.1)                                            */
/* ------------------------------------------------------------------------ */
/*
 * The five codes this library can emit, and the only five it will.  There is
 * no send_raw() (design §9.1).
 *
 * ⚠ 202 and 203 are on no vendor page.  They are enumerated by [TNB] and
 * handled, with the exact Message strings below, by [OSP] — which ARMS ITS
 * LAUNCH MONITOR ONLY AFTER A 202, so a server that never sends one leaves
 * that client waiting for a game that never starts (protocol §5.1).
 */
typedef enum gsp_response_code {
    GSP_CODE_SHOT_RECEIVED = 200,  /* [GSP] "Shot received successfully"     */
    GSP_CODE_PLAYER_INFO   = 201,  /* [GSP] "Player information"             */
    GSP_CODE_READY         = 202,  /* [TNB] [OSP] "GSPro ready"              */
    GSP_CODE_ROUND_ENDED   = 203,  /* [TNB] [OSP] "GSPro round ended"        */
    GSP_CODE_FAILURE       = 501   /* [GSP] "501/5XX: Failure occurred"      */
} gsp_response_code;

/* The Message strings the library sends by default.  ⚠ [TNB] reports that the
 * real GSPro misspells its 200 text (protocol U4); no client keys on the text,
 * so the documented spelling stays until a capture says otherwise. */
#define GSP_TEXT_SHOT_RECEIVED  "Shot received successfully"
#define GSP_TEXT_PLAYER_INFO    "GSPro Player Information"   /* verbatim [GSP] */
/* ⚠ FIXED, NOT POLICY: [OSP] compares the 203 text exactly. */
#define GSP_TEXT_READY          "GSPro ready"                /* verbatim [OSP] */
#define GSP_TEXT_ROUND_ENDED    "GSPro round ended"          /* verbatim [OSP] */
#define GSP_TEXT_BAD_JSON       "Bad JSON data"
#define GSP_TEXT_TOO_LARGE      "Payload too large"
#define GSP_TEXT_INCOMPLETE     "Incomplete ball data"

/* ------------------------------------------------------------------------ */
/* Units (protocol §3.6)                                                     */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ WHAT THE FIELD GOVERNS IS NOT STATED BY ANY SOURCE.  Distances, probably;
 * speeds, unknown (protocol U3).  Every client that can be checked sends
 * "Yards" with mph speeds and yard distances.  The library converts NOTHING;
 * gsp_message keeps both this enum and the original text.
 */
typedef enum gsp_units {
    GSP_UNITS_UNKNOWN = 0,   /* absent (⚠ [GSP]: "default yards") or unrecognised */
    GSP_UNITS_YARDS   = 1,   /* "Yards"  [GSP] [TNB] */
    GSP_UNITS_METERS  = 2    /* "Meters" [TNB]; a user setting in [OCR] — never observed on a wire */
} gsp_units;

/* Case-insensitive.  NULL or unknown → GSP_UNITS_UNKNOWN. */
GSP_API gsp_units   gsp_units_parse(const char *text);
/* The wire spelling; "" for UNKNOWN. */
GSP_API const char *gsp_units_text(gsp_units units);

/* ------------------------------------------------------------------------ */
/* Handedness (protocol §5.2)                                                */
/* ------------------------------------------------------------------------ */
typedef enum gsp_handed {
    GSP_HANDED_UNKNOWN = 0,  /* omitted from the 201 JSON when sending */
    GSP_HANDED_RIGHT   = 1,  /* "RH" */
    GSP_HANDED_LEFT    = 2   /* "LH" */
} gsp_handed;

GSP_API gsp_handed  gsp_handed_parse(const char *text);
GSP_API const char *gsp_handed_text(gsp_handed handed);

/* ------------------------------------------------------------------------ */
/* Clubs (protocol §5.2)                                                     */
/* ------------------------------------------------------------------------ */
/*
 * The vocabulary two independent clients agree on exactly [TNB] [R10].  The
 * numbering is this library's; the wire carries the two-letter code.
 *
 * ⚠ THE SHOT MESSAGE CARRIES NO CLUB.  This enum exists for the 201 message the
 * SERVER sends (design §5.6); nothing a launch monitor sends names a club.
 */
typedef enum gsp_club {
    GSP_CLUB_UNKNOWN = 0,    /* omitted from the 201 JSON when sending */
    GSP_CLUB_DR = 1,         /* Driver */
    GSP_CLUB_W2 = 2, GSP_CLUB_W3, GSP_CLUB_W4, GSP_CLUB_W5, GSP_CLUB_W6, GSP_CLUB_W7,
    GSP_CLUB_H2 = 8, GSP_CLUB_H3, GSP_CLUB_H4, GSP_CLUB_H5, GSP_CLUB_H6, GSP_CLUB_H7,
    GSP_CLUB_I1 = 14, GSP_CLUB_I2, GSP_CLUB_I3, GSP_CLUB_I4, GSP_CLUB_I5,
    GSP_CLUB_I6, GSP_CLUB_I7, GSP_CLUB_I8, GSP_CLUB_I9,
    GSP_CLUB_PW = 23,        /* Pitching wedge */
    GSP_CLUB_GW = 24,        /* Gap wedge      */
    GSP_CLUB_SW = 25,        /* Sand wedge     */
    GSP_CLUB_LW = 26,        /* Lob wedge      */
    GSP_CLUB_PT = 27,        /* Putter — the one clients act on: [MLM] switches
                              * its device to putting mode on "PT" */
    GSP_CLUB_COUNT = 28
} gsp_club;

/* Case-insensitive.  NULL or unknown → GSP_CLUB_UNKNOWN. */
GSP_API gsp_club    gsp_club_parse(const char *code);
/* The two-letter wire code, "DR" … "PT"; "" for UNKNOWN. */
GSP_API const char *gsp_club_code(gsp_club club);
/* A human name, "Driver" … "Putter"; "" for UNKNOWN.  For a UI, never the wire. */
GSP_API const char *gsp_club_name(gsp_club club);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_PROTOCOL_H */
