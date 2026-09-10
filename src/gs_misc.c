/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_misc.c — the vocabulary tables, the message helpers, the one derivation,
 * the log formatters and the documented defaults.
 *
 * Everything here is a place where a table and an enum have to agree and a
 * disagreement produces a PLAUSIBLE WRONG ANSWER rather than a failure: a club
 * code that maps to its neighbour puts a launch monitor in the wrong mode with
 * nothing anywhere reporting a fault, and a formatter that forgot to redact
 * leaks an identifier into a log during the one incident somebody keeps.  That
 * is why test_api.c exists and why it sweeps every enumerator.
 */

#include "gspro/gspro.h"

#include "gs_json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ⚠ Not M_PI: that is POSIX, and this translation unit is strict ISO C11. */
#define GS_RAD_TO_DEG 57.295779513082320876798
#define GS_DEG_TO_RAD 0.017453292519943295769237

#define GS_REDACTED "<redacted>"

/* ------------------------------------------------------------------------ */
/* Status                                                                    */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ Every one distinct, because a log line is often the only evidence left
 * after a fault and two statuses that print the same are two faults that read
 * as one.
 */
const char *gsp_status_str(gsp_status status)
{
    switch (status) {
    case GSP_OK:
        return "ok";
    case GSP_PENDING:
        return "pending";
    case GSP_ERR_INVALID_ARG:
        return "invalid argument";
    case GSP_ERR_INVALID_STATE:
        return "invalid state";
    case GSP_ERR_NO_MEMORY:
        return "out of memory";
    case GSP_ERR_BUFFER_TOO_SMALL:
        return "buffer too small";
    case GSP_ERR_NOT_SUPPORTED:
        return "not supported";
    case GSP_ERR_TOO_MANY_CONNECTIONS:
        return "too many connections";
    case GSP_ERR_UNKNOWN_CONNECTION:
        return "unknown connection";
    case GSP_ERR_MALFORMED:
        return "malformed message";
    case GSP_ERR_MESSAGE_TOO_LARGE:
        return "message too large";
    case GSP_ERR_QUEUE_FULL:
        return "write queue full";
    case GSP_ERR_CLOSED:
        return "server closed";
    default:
        break;
    }
    return "unknown status";
}

/* ------------------------------------------------------------------------ */
/* Version and ABI                                                           */
/* ------------------------------------------------------------------------ */

const char *gsp_version_string(void)
{
    return GSP_VERSION_STRING;
}

uint32_t gsp_abi_version(void)
{
    return GSP_ABI_VERSION;
}

void gsp_abi_sizes_get(gsp_abi_sizes *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->abi_version = GSP_ABI_VERSION;
    out->message = (uint32_t)sizeof(gsp_message);
    out->ball_data = (uint32_t)sizeof(gsp_ball_data);
    out->club_data = (uint32_t)sizeof(gsp_club_data);
    out->shot_options = (uint32_t)sizeof(gsp_shot_options);
    out->player_info = (uint32_t)sizeof(gsp_player_info);
    out->event = (uint32_t)sizeof(gsp_event);
    out->write_request = (uint32_t)sizeof(gsp_write_request);
    out->wire_chunk = (uint32_t)sizeof(gsp_wire_chunk);
    out->connection_info = (uint32_t)sizeof(gsp_connection_info);
    out->server_config = (uint32_t)sizeof(gsp_server_config);
    out->message_layout_version = GSP_MESSAGE_LAYOUT_VERSION;
}

gsp_status gsp_abi_check(const gsp_abi_sizes *expected)
{
    gsp_abi_sizes mine;

    if (expected == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    gsp_abi_sizes_get(&mine);
    /* ⚠ Sizes are necessary and nowhere near sufficient: a binding whose field
     * is one slot out passes this and returns plausible numbers.  The Python
     * binding pins every offset and enumerator as well (design §4.6). */
    return (memcmp(&mine, expected, sizeof(mine)) == 0) ? GSP_OK : GSP_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------------ */
/* Units                                                                     */
/* ------------------------------------------------------------------------ */

gsp_units gsp_units_parse(const char *text)
{
    if (text == NULL) {
        return GSP_UNITS_UNKNOWN;
    }
    if (gs_ci_equal(text, "Yards")) {
        return GSP_UNITS_YARDS;
    }
    if (gs_ci_equal(text, "Meters")) {
        return GSP_UNITS_METERS;
    }
    /* ⚠ "Metres" is NOT Meters.  Two known strings, and a third nobody has
     * seen stays UNKNOWN with its text kept verbatim (protocol §3.6). */
    return GSP_UNITS_UNKNOWN;
}

const char *gsp_units_text(gsp_units units)
{
    if (units == GSP_UNITS_YARDS) {
        return "Yards";
    }
    if (units == GSP_UNITS_METERS) {
        return "Meters";
    }
    return "";
}

/* ------------------------------------------------------------------------ */
/* Handedness                                                                */
/* ------------------------------------------------------------------------ */

gsp_handed gsp_handed_parse(const char *text)
{
    if (text == NULL) {
        return GSP_HANDED_UNKNOWN;
    }
    if (gs_ci_equal(text, "RH")) {
        return GSP_HANDED_RIGHT;
    }
    if (gs_ci_equal(text, "LH")) {
        return GSP_HANDED_LEFT;
    }
    return GSP_HANDED_UNKNOWN;
}

const char *gsp_handed_text(gsp_handed handed)
{
    if (handed == GSP_HANDED_RIGHT) {
        return "RH";
    }
    if (handed == GSP_HANDED_LEFT) {
        return "LH";
    }
    return "";
}

/* ------------------------------------------------------------------------ */
/* Clubs                                                                     */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ INDEXED BY THE ENUMERATOR, so the table cannot drift out of step with it:
 * a club that parsed to its neighbour would put [MLM] into putting mode for a
 * sand wedge and nothing would report a fault.  The wire codes are [TNB] and
 * [R10]'s, which agree exactly (protocol §5.2); the names are for a UI and
 * never go on the wire.
 */
static const struct {
    const char *code;
    const char *name;
} CLUB_TABLE[GSP_CLUB_COUNT] = {
    { "", "" }, /* GSP_CLUB_UNKNOWN */
    { "DR", "Driver" },
    { "W2", "2 wood" },    { "W3", "3 wood" },    { "W4", "4 wood" },
    { "W5", "5 wood" },    { "W6", "6 wood" },    { "W7", "7 wood" },
    { "H2", "2 hybrid" },  { "H3", "3 hybrid" },  { "H4", "4 hybrid" },
    { "H5", "5 hybrid" },  { "H6", "6 hybrid" },  { "H7", "7 hybrid" },
    { "I1", "1 iron" },    { "I2", "2 iron" },    { "I3", "3 iron" },
    { "I4", "4 iron" },    { "I5", "5 iron" },    { "I6", "6 iron" },
    { "I7", "7 iron" },    { "I8", "8 iron" },    { "I9", "9 iron" },
    { "PW", "Pitching wedge" },
    { "GW", "Gap wedge" },
    { "SW", "Sand wedge" },
    { "LW", "Lob wedge" },
    { "PT", "Putter" }
};

gsp_club gsp_club_parse(const char *code)
{
    int i;

    if (code == NULL || code[0] == '\0') {
        return GSP_CLUB_UNKNOWN;
    }
    for (i = 1; i < (int)GSP_CLUB_COUNT; ++i) {
        if (gs_ci_equal(CLUB_TABLE[i].code, code)) {
            return (gsp_club)i;
        }
    }
    /* ⚠ [PIT] defaults an unrecognised club to DRIVER.  That is a client-side
     * choice; a library that made it would hide the fact from every consumer. */
    return GSP_CLUB_UNKNOWN;
}

const char *gsp_club_code(gsp_club club)
{
    if ((int)club <= 0 || (int)club >= (int)GSP_CLUB_COUNT) {
        return "";
    }
    return CLUB_TABLE[(int)club].code;
}

const char *gsp_club_name(gsp_club club)
{
    if ((int)club <= 0 || (int)club >= (int)GSP_CLUB_COUNT) {
        return "";
    }
    return CLUB_TABLE[(int)club].name;
}

/* ------------------------------------------------------------------------ */
/* Message helpers                                                           */
/* ------------------------------------------------------------------------ */

bool gsp_message_is_shot(const gsp_message *m)
{
    return m != NULL && m->kind == (uint8_t)GSP_MSG_SHOT;
}

bool gsp_message_ball_zero_speed(const gsp_message *m)
{
    if (!gsp_message_is_shot(m)) {
        return false;
    }
    /* ⚠ Not a flag, because zero is a value (design §4.3) — but [SLX] shows a
     * commercial connector emitting such "shots" often enough that a proxy was
     * written to filter them, and GSPro reacting by resetting the club.  The
     * host needs to be able to ask. */
    return (m->ball.present & (uint32_t)GSP_BALL_SPEED) != 0u && m->ball.speed == 0.0;
}

bool gsp_message_ball_complete(const gsp_ball_data *ball)
{
    uint32_t p;
    bool pair;

    if (ball == NULL) {
        return false;
    }
    p = ball->present;
    if ((p & (uint32_t)(GSP_BALL_SPEED | GSP_BALL_HLA | GSP_BALL_VLA)) !=
        (uint32_t)(GSP_BALL_SPEED | GSP_BALL_HLA | GSP_BALL_VLA)) {
        return false;
    }
    /* [GSP]: BackSpin and SideSpin are "only required if total spin is not
     * sent" — so either representation satisfies the rule, and half a pair is
     * not a pair. */
    pair = (p & (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN)) ==
           (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN);
    if ((p & (uint32_t)GSP_BALL_TOTAL_SPIN) == 0u && !pair) {
        return false;
    }
    if ((p & (uint32_t)GSP_BALL_SPIN_AXIS) == 0u && !pair) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* The one derivation (design §4.3, protocol §3.2)                           */
/* ------------------------------------------------------------------------ */

gsp_status gsp_ball_data_derive(gsp_ball_data *b)
{
    bool have_pair;
    bool have_total;
    bool have_axis;

    if (b == NULL) {
        return GSP_ERR_INVALID_ARG;
    }

    have_pair = (b->present & (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN)) ==
                (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN);
    have_total = (b->present & (uint32_t)GSP_BALL_TOTAL_SPIN) != 0u;
    have_axis = (b->present & (uint32_t)GSP_BALL_SPIN_AXIS) != 0u;

    /*
     * ⚠ THE ONE ZERO THIS LIBRARY READS AS "UNMEASURED", AND ONLY THIS ONE.
     * [PIT] — a real camera device with one of the two largest user bases in
     * the survey — sends TotalSpin as a literal 0.0 placeholder beside a real
     * BackSpin/SideSpin pair on every shot.  A server that believed it records
     * zero spin for all of them (protocol §3.2, conformance CT-D13).  The
     * computed SpinAxis beside it is part of the same placeholder shape, so
     * both are recomputed and both are marked derived.
     */
    if (have_total && b->total_spin == 0.0 && have_pair &&
        (b->back_spin != 0.0 || b->side_spin != 0.0)) {
        have_total = false;
        have_axis = false;
    }

    if (have_pair && have_total && have_axis) {
        /*
         * Both representations arrived.  ⚠ Report a disagreement; never
         * "correct" it — the vendor's own example disagrees (3250 rpm against
         * a 2625 rpm pair, protocol §3.2) and its numbers are illustrative,
         * not a measurement.  Compared as vectors, so an axis that is wrong
         * beside a plausible magnitude is caught too.
         */
        double rad = b->spin_axis * GS_DEG_TO_RAD;
        double implied_back = b->total_spin * cos(rad);
        double implied_side = b->total_spin * sin(rad);
        double error = hypot(implied_back - b->back_spin, implied_side - b->side_spin);
        double scale = fabs(b->total_spin);

        if (error > 0.01 * scale) {
            return GSP_PENDING;
        }
        return GSP_OK;
    }

    if (have_pair) {
        if (!have_total) {
            b->total_spin = hypot(b->back_spin, b->side_spin);
            b->present |= (uint32_t)GSP_BALL_TOTAL_SPIN;
            b->derived |= (uint32_t)GSP_BALL_TOTAL_SPIN;
        }
        if (!have_axis) {
            /* ⚠ atan2, not atan: it carries the ±90° case [GC2] special-cases
             * to avoid a divide by a zero backspin. */
            b->spin_axis = atan2(b->side_spin, b->back_spin) * GS_RAD_TO_DEG;
            b->present |= (uint32_t)GSP_BALL_SPIN_AXIS;
            b->derived |= (uint32_t)GSP_BALL_SPIN_AXIS;
        }
        return GSP_OK;
    }

    if (have_total && have_axis) {
        double rad = b->spin_axis * GS_DEG_TO_RAD;
        /* Only what was absent: half a pair on the wire stays as it arrived. */
        if ((b->present & (uint32_t)GSP_BALL_BACK_SPIN) == 0u) {
            b->back_spin = b->total_spin * cos(rad);
            b->present |= (uint32_t)GSP_BALL_BACK_SPIN;
            b->derived |= (uint32_t)GSP_BALL_BACK_SPIN;
        }
        if ((b->present & (uint32_t)GSP_BALL_SIDE_SPIN) == 0u) {
            b->side_spin = b->total_spin * sin(rad);
            b->present |= (uint32_t)GSP_BALL_SIDE_SPIN;
            b->derived |= (uint32_t)GSP_BALL_SIDE_SPIN;
        }
        return GSP_OK;
    }

    /* Neither pair is complete: nothing to derive, and that is not an error. */
    return GSP_OK;
}

/* ------------------------------------------------------------------------ */
/* Names                                                                     */
/* ------------------------------------------------------------------------ */

static const char *const FLAG_NAMES[] = {
    "MISSING_DEVICE_ID",        "MISSING_SHOT_NUMBER",      "SHOT_NUMBER_NOT_INTEGER",
    "MISSING_API_VERSION",      "UNKNOWN_API_VERSION",      "MISSING_OPTIONS",
    "BALL_FLAG_WITHOUT_OBJECT", "BALL_OBJECT_WITHOUT_FLAG", "CLUB_FLAG_WITHOUT_OBJECT",
    "CLUB_OBJECT_WITHOUT_FLAG", "BALL_INCOMPLETE",          "UNKNOWN_UNITS",
    "KEY_CASE_MISMATCH",        "UNKNOWN_KEYS",             "SHOT_NUMBER_REPEATED",
    "STRING_TRUNCATED",         "TYPE_COERCED"
};

const char *gsp_message_flag_name(gsp_message_flag flag)
{
    uint32_t v = (uint32_t)flag;
    unsigned bit = 0u;

    /* ⚠ ONE BIT ONLY.  A caller iterating a mask must not get a plausible
     * answer for a pair of flags. */
    if (v == 0u || (v & (v - 1u)) != 0u) {
        return "";
    }
    while ((v >> bit) != 1u) {
        bit++;
    }
    if (bit >= sizeof(FLAG_NAMES) / sizeof(FLAG_NAMES[0])) {
        return "";
    }
    return FLAG_NAMES[bit];
}

static const char *const EVENT_NAMES[GSP_EVENT_TYPE_COUNT] = {
    "NONE",         "CONNECTION_OPENED", "CONNECTION_CLOSED", "CLIENT_IDENTIFIED",
    "SHOT",         "HEARTBEAT",         "STATUS",            "CLIENT_STATE",
    "PLAYER_INFO_SENT", "SESSION_STATE_SENT", "PROTOCOL_ERROR", "CLOSE_REQUESTED",
    "CLIENT_IDLE",  "CLIENT_ACTIVE",     "WARNING"
};

const char *gsp_event_type_name(gsp_event_type type)
{
    if ((int)type < 0 || (int)type >= (int)GSP_EVENT_TYPE_COUNT) {
        return "";
    }
    return EVENT_NAMES[(int)type];
}

static const char *close_cause_name(uint8_t cause)
{
    static const char *const names[] = { "UNKNOWN", "REMOTE_CLOSED", "LOCAL_REQUEST",
                                         "TRANSPORT_ERROR", "SERVER_CLOSED" };
    if (cause >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[cause];
}

static const char *warning_name(uint16_t code)
{
    static const char *const names[GSP_WARNING_CODE_COUNT] = {
        "NONE", "DEVICE_ID_CHANGED", "STRING_TRUNCATED", "EVENTS_DROPPED", "WIRE_DROPPED",
        "WRITE_RING_FULL"
    };
    if (code >= (uint16_t)GSP_WARNING_CODE_COUNT) {
        return "UNKNOWN";
    }
    return names[code];
}

static const char *protocol_error_name(uint8_t reason)
{
    static const char *const names[] = { "LEADING_GARBAGE", "BAD_JSON", "TOO_LARGE" };
    if (reason >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[reason];
}

static const char *session_state_name(uint8_t state)
{
    static const char *const names[] = { "NONE", "ACTIVE", "ENDED" };
    if (state >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[state];
}

static const char *player_reason_name(uint8_t reason)
{
    static const char *const names[] = { "CHANGED", "ON_CONNECT", "EXPLICIT" };
    if (reason >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[reason];
}

/* ------------------------------------------------------------------------ */
/* A bounded line writer                                                     */
/* ------------------------------------------------------------------------ */
/*
 * snprintf semantics: never overruns, always terminates, and returns what a
 * complete line would have taken so a caller can grow its buffer.
 */
typedef struct gs_line {
    char  *buf;
    size_t size;
    size_t used;
} gs_line;

static void ln_char(gs_line *l, char c)
{
    if (l->buf != NULL && l->used + 1u < l->size) {
        l->buf[l->used] = c;
    }
    l->used++;
}

static void ln_str(gs_line *l, const char *s)
{
    while (*s != '\0') {
        ln_char(l, *s);
        s++;
    }
}

/* At most `max` bytes of a field that need not be NUL-terminated. */
static void ln_str_n(gs_line *l, const char *s, size_t max)
{
    size_t i;
    for (i = 0; i < max && s[i] != '\0'; ++i) {
        ln_char(l, s[i]);
    }
}

static void ln_i64(gs_line *l, int64_t v)
{
    char tmp[24];
    (void)snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    ln_str(l, tmp);
}

static void ln_u32(gs_line *l, uint32_t v)
{
    char tmp[16];
    (void)snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v);
    ln_str(l, tmp);
}

static void ln_double(gs_line *l, double v)
{
    char tmp[40];
    if (!isfinite(v)) {
        ln_str(l, "nan");
        return;
    }
    (void)snprintf(tmp, sizeof(tmp), "%.10g", v);
    ln_str(l, tmp);
}

static size_t ln_finish(gs_line *l)
{
    if (l->buf != NULL && l->size > 0u) {
        l->buf[(l->used < l->size) ? l->used : (l->size - 1u)] = '\0';
    }
    return l->used;
}

/* ------------------------------------------------------------------------ */
/* gsp_message_format                                                        */
/* ------------------------------------------------------------------------ */

static const char *kind_name(uint8_t kind)
{
    if (kind == (uint8_t)GSP_MSG_SHOT) {
        return "shot";
    }
    if (kind == (uint8_t)GSP_MSG_HEARTBEAT) {
        return "heartbeat";
    }
    if (kind == (uint8_t)GSP_MSG_STATUS) {
        return "status";
    }
    return "message";
}

static const char *const BALL_NAMES[8] = { "speed", "spin_axis", "total_spin", "back_spin",
                                           "side_spin", "hla",    "vla",       "carry" };

static const char *const CLUB_NAMES[10] = { "speed", "aoa",  "face_to_target", "lie",
                                            "loft",  "path", "speed_at_impact", "vfi",
                                            "hfi",   "closure_rate" };

static void ln_present(gs_line *l, const char *label, uint32_t present, uint32_t derived,
                       const char *const *names, unsigned count)
{
    unsigned i;
    bool first = true;

    if (present == 0u) {
        return;
    }
    ln_char(l, ' ');
    ln_str(l, label);
    ln_char(l, '[');
    for (i = 0; i < count; ++i) {
        if ((present & (1u << i)) == 0u) {
            continue;
        }
        if (!first) {
            ln_char(l, ' ');
        }
        first = false;
        ln_str(l, names[i]);
        /* A consumer that cares where a value came from can see it here too. */
        if ((derived & (1u << i)) != 0u) {
            ln_char(l, '*');
        }
    }
    ln_char(l, ']');
}

static void ln_flags(gs_line *l, uint32_t flags)
{
    unsigned i;
    bool first = true;

    if (flags == 0u) {
        return;
    }
    ln_str(l, " flags[");
    for (i = 0; i < 32u; ++i) {
        const char *name;
        if ((flags & (1u << i)) == 0u) {
            continue;
        }
        name = gsp_message_flag_name((gsp_message_flag)(1u << i));
        if (!first) {
            ln_char(l, ' ');
        }
        first = false;
        if (name[0] != '\0') {
            ln_str(l, name);
        } else {
            ln_str(l, "BIT");
            ln_u32(l, i);
        }
    }
    ln_char(l, ']');
}

static void ln_message(gs_line *l, const gsp_message *m, bool include_identifiers)
{
    ln_str(l, kind_name(m->kind));

    /*
     * ⚠ THE SHOT NUMBER GOES WITH THE IDENTIFIERS, and that is not
     * over-caution: [GC2] puts the unit's hardware serial inside its DeviceID
     * and numbers its shots from the same counter, so the redacted line for a
     * GC2 shot must carry neither (conformance CT-D18).  A number that can be
     * a serial is a serial.
     */
    if (include_identifiers) {
        ln_str(l, " #");
        ln_i64(l, m->shot_number);
        ln_str(l, " from \"");
        ln_str_n(l, m->device_id, GSP_DEVICE_ID_MAX);
        ln_char(l, '"');
    } else {
        ln_str(l, " from " GS_REDACTED);
    }

    ln_present(l, "ball", m->ball.present, m->ball.derived, BALL_NAMES, 8u);
    ln_present(l, "club", m->club.present, 0u, CLUB_NAMES, 10u);
    ln_flags(l, m->flags);
}

size_t gsp_message_format(const gsp_message *m, char *out, size_t out_size,
                          bool include_identifiers)
{
    gs_line l;

    l.buf = out;
    l.size = out_size;
    l.used = 0u;
    if (m == NULL) {
        ln_str(&l, "(null message)");
        return ln_finish(&l);
    }
    ln_message(&l, m, include_identifiers);
    return ln_finish(&l);
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */

bool gsp_event_is_sensitive(const gsp_event *ev)
{
    if (ev == NULL) {
        return false;
    }
    /* Exactly the events that can carry a peer address or a DeviceID
     * (design §9.2). */
    if (ev->type == (uint8_t)GSP_EV_CONNECTION_OPENED ||
        ev->type == (uint8_t)GSP_EV_CONNECTION_CLOSED ||
        ev->type == (uint8_t)GSP_EV_CLIENT_IDENTIFIED ||
        ev->type == (uint8_t)GSP_EV_SHOT || ev->type == (uint8_t)GSP_EV_HEARTBEAT ||
        ev->type == (uint8_t)GSP_EV_STATUS) {
        return true;
    }
    if (ev->type == (uint8_t)GSP_EV_WARNING) {
        /* ⚠ A warning's text names one only when it is about one. */
        return ev->u.warning.code == (uint16_t)GSP_WARN_DEVICE_ID_CHANGED;
    }
    return false;
}

static void ln_identifier(gs_line *l, const char *label, const char *value, size_t max,
                          bool include_identifiers)
{
    ln_char(l, ' ');
    ln_str(l, label);
    ln_char(l, '=');
    if (!include_identifiers) {
        ln_str(l, GS_REDACTED);
        return;
    }
    if (value[0] == '\0') {
        ln_str(l, "\"\"");
        return;
    }
    ln_char(l, '"');
    ln_str_n(l, value, max);
    ln_char(l, '"');
}

size_t gsp_event_format(const gsp_event *ev, char *out, size_t out_size,
                        bool include_identifiers)
{
    gs_line l;
    const char *name;

    l.buf = out;
    l.size = out_size;
    l.used = 0u;

    if (ev == NULL) {
        ln_str(&l, "(null event)");
        return ln_finish(&l);
    }

    name = gsp_event_type_name((gsp_event_type)ev->type);
    if (name[0] == '\0') {
        ln_str(&l, "EVENT#");
        ln_u32(&l, (uint32_t)ev->type);
    } else {
        ln_str(&l, name);
    }
    ln_str(&l, " seq=");
    ln_u32(&l, ev->sequence);
    if (ev->conn != GSP_CONN_NONE) {
        ln_str(&l, " conn=");
        ln_u32(&l, ev->conn);
    }
    ln_str(&l, " t=");
    ln_i64(&l, ev->host_time_us);

    if (ev->type == (uint8_t)GSP_EV_CONNECTION_OPENED ||
        ev->type == (uint8_t)GSP_EV_CONNECTION_CLOSED ||
        ev->type == (uint8_t)GSP_EV_CLIENT_IDENTIFIED) {
        const gsp_connection_event *c = &ev->u.connection;
        ln_identifier(&l, "peer", c->info.peer, GSP_PEER_MAX, include_identifiers);
        ln_identifier(&l, "device", c->info.device_id, GSP_DEVICE_ID_MAX,
                      include_identifiers);
        if (ev->type == (uint8_t)GSP_EV_CONNECTION_CLOSED) {
            ln_str(&l, " cause=");
            ln_str(&l, close_cause_name(c->cause));
            ln_str(&l, " messages=");
            ln_u32(&l, c->info.messages);
            ln_str(&l, " shots=");
            ln_u32(&l, c->info.shots);
            ln_str(&l, " errors=");
            ln_u32(&l, c->info.protocol_errors);
        }
    } else if (ev->type == (uint8_t)GSP_EV_SHOT || ev->type == (uint8_t)GSP_EV_HEARTBEAT ||
               ev->type == (uint8_t)GSP_EV_STATUS) {
        ln_char(&l, ' ');
        ln_message(&l, &ev->u.message, include_identifiers);
    } else if (ev->type == (uint8_t)GSP_EV_CLIENT_STATE) {
        const gsp_client_state_event *s = &ev->u.client_state;
        ln_str(&l, " ready=");
        ln_u32(&l, s->ready ? 1u : 0u);
        ln_str(&l, " ball_detected=");
        ln_u32(&l, s->ball_detected ? 1u : 0u);
        ln_str(&l, " was=");
        ln_u32(&l, s->previous_ready ? 1u : 0u);
        ln_char(&l, '/');
        ln_u32(&l, s->previous_ball_detected ? 1u : 0u);
    } else if (ev->type == (uint8_t)GSP_EV_PLAYER_INFO_SENT) {
        const gsp_player_info_event *p = &ev->u.player_info;
        ln_str(&l, " handed=");
        ln_str(&l, gsp_handed_text((gsp_handed)p->player.handed));
        ln_str(&l, " club=");
        ln_str(&l, gsp_club_code((gsp_club)p->player.club));
        if (p->player.has_distance) {
            ln_str(&l, " distance=");
            ln_double(&l, p->player.distance_to_target);
        }
        ln_str(&l, " reason=");
        ln_str(&l, player_reason_name(p->reason));
    } else if (ev->type == (uint8_t)GSP_EV_SESSION_STATE_SENT) {
        const gsp_session_state_event *s = &ev->u.session_state;
        ln_str(&l, " state=");
        ln_str(&l, session_state_name(s->state));
        ln_str(&l, " reason=");
        ln_str(&l, player_reason_name(s->reason));
    } else if (ev->type == (uint8_t)GSP_EV_PROTOCOL_ERROR ||
               ev->type == (uint8_t)GSP_EV_CLOSE_REQUESTED) {
        const gsp_protocol_error_event *p = &ev->u.protocol_error;
        ln_str(&l, " reason=");
        ln_str(&l, protocol_error_name(p->reason));
        ln_str(&l, " discarded=");
        ln_u32(&l, p->discarded_bytes);
        ln_str(&l, " consecutive=");
        ln_u32(&l, p->consecutive);
        /* ⚠ The snippet is raw client bytes, and the first thing in a GSPro
         * message is usually its DeviceID — so it is an identifier whatever
         * else it is, and it goes out only when identifiers do. */
        if (include_identifiers && p->snippet_length > 0u) {
            unsigned i;
            unsigned n = p->snippet_length;
            if (n > (unsigned)GSP_ERROR_SNIPPET_MAX) {
                n = (unsigned)GSP_ERROR_SNIPPET_MAX;
            }
            ln_str(&l, " snippet=\"");
            for (i = 0; i < n; ++i) {
                uint8_t c = p->snippet[i];
                if (c >= 0x20u && c < 0x7fu && c != '"' && c != '\\') {
                    ln_char(&l, (char)c);
                } else {
                    ln_char(&l, '.');
                }
            }
            ln_char(&l, '"');
        }
    } else if (ev->type == (uint8_t)GSP_EV_WARNING) {
        const gsp_warning_event *w = &ev->u.warning;
        ln_str(&l, " code=");
        ln_str(&l, warning_name(w->code));
        ln_str(&l, " value=");
        ln_u32(&l, w->value);
        if (include_identifiers || !gsp_event_is_sensitive(ev)) {
            ln_str(&l, " \"");
            ln_str_n(&l, w->text, GSP_WARNING_TEXT_MAX);
            ln_char(&l, '"');
        } else {
            ln_str(&l, " " GS_REDACTED);
        }
    }

    return ln_finish(&l);
}

/* ------------------------------------------------------------------------ */
/* Player information                                                        */
/* ------------------------------------------------------------------------ */

bool gsp_player_info_equal(const gsp_player_info *a, const gsp_player_info *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    /* ⚠ "Would serialise identically", not "has identical bytes": a distance
     * that is not being sent is not part of the message, so two values that
     * differ only there are the same 201 and must not queue a second one
     * (design §5.6). */
    if (a->handed != b->handed || a->club != b->club || a->has_distance != b->has_distance) {
        return false;
    }
    if (a->has_distance && a->distance_to_target != b->distance_to_target) {
        return false;
    }
    return strncmp(a->surface, b->surface, sizeof(a->surface)) == 0;
}

/* ------------------------------------------------------------------------ */
/* The documented defaults                                                   */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THESE LIVE WITH THE VALUE HELPERS, NOT WITH THE SERVER, because they are
 * pure values with no server state behind them and because design §11 makes
 * them part of what package 2 delivers ("the API family").  gs_server.c must
 * not define them a second time.
 *
 * Each is a decision with a reason recorded beside it in server.h; a silent
 * change to one is a change of behaviour for every consumer that never touched
 * the field, which is why test_api.c pins them.
 */

gsp_server_policy gsp_server_policy_default(void)
{
    gsp_server_policy p;

    memset(&p, 0, sizeof(p));
    /* 0 means "take the default" for every size, so the struct stays
     * zero-initialisable and a binding need not know the numbers. */
    p.max_message_bytes = 0u;
    p.idle_alarm_us = 0;                  /* the protocol has no deadline of its own */
    p.protocol_error_close_threshold = 0u; /* never ask for a close by default        */
    p.write_spacing_us = 0;                /* spacing is opt-in                       */
    p.reject_incomplete_shots = false;     /* U2/U8 unsettled; a 501 may loop a client */
    p.announce_player_on_connect = true;   /* U1: better served than not              */
    p.announce_ready_on_connect = true;    /* [OSP] does not arm until it sees a 202  */
    p.record_identifiers = false;          /* identifiers are redacted unless asked   */
    p.ack_text[0] = '\0';                  /* "" selects GSP_TEXT_SHOT_RECEIVED       */
    return p;
}

gsp_server_config gsp_server_config_default(void)
{
    gsp_server_config c;

    memset(&c, 0, sizeof(c));
    c.max_connections = 0u; /* → GSP_MAX_CONNECTIONS_DEFAULT */
    c.event_ring = 0u;      /* → GSP_EVENT_RING_DEFAULT      */
    c.write_ring = 0u;      /* → GSP_WRITE_RING_DEFAULT      */
    c.wire_ring = 0u;       /* ⚠ the wire log is OFF unless asked for */
    c.policy = gsp_server_policy_default();
    return c;
}
