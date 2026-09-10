/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_encode.c — the only path by which bytes leave this library.
 *
 * design §9.1: five message shapes, from a fixed set of codes — 200, 201, 202,
 * 203, 501 — and there is no send_raw().  `code` is validated here, so a host
 * that wants to send something else to a launch monitor is asking for a
 * different library.  The suite round-trips every write the server can produce
 * back through gsp_response_decode() and asserts the code is one of the five
 * (CT-R06).
 *
 * ⚠ COMPACT, NO TRAILING NEWLINE, NO WHITESPACE.  Every examined client's own
 * splitter handles back-to-back objects (protocol §2); several of them parse
 * one read as one message and one of those, [PIT], dies outright when two
 * replies share a segment (§9.8).  Spacing them is the server's job; keeping
 * each one small and self-delimiting is this file's.
 *
 * ⚠ AND EVERY STRING IS ESCAPED.  A DeviceID, an ack text or a Surface is free
 * text somebody else chose; one unescaped quote turns our reply into something
 * that is not JSON, on a socket where the client has no way to tell us.
 */

#include "gspro/codec.h"

#include "gs_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* A counting writer                                                         */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ TWO PASSES, so that a reply which does not fit writes NOTHING (codec.h).
 * Half a JSON object on a socket is worse than none: the client cannot tell it
 * is half, and every client in the survey would then mis-frame everything
 * after it.  The first pass has buf == NULL and only counts.
 */
typedef struct gs_out {
    char  *buf;   /* NULL while measuring */
    size_t size;  /* capacity for content, excluding the NUL */
    size_t used;  /* what a complete write would take, excluding the NUL */
} gs_out;

static void ow_char(gs_out *o, char c)
{
    if (o->buf != NULL && o->used < o->size) {
        o->buf[o->used] = c;
    }
    o->used++;
}

static void ow_str(gs_out *o, const char *s)
{
    while (*s != '\0') {
        ow_char(o, *s);
        s++;
    }
}

/* A JSON string literal, quotes included, from at most `max` bytes of `s`. */
static void ow_json_string(gs_out *o, const char *s, size_t max)
{
    size_t i;

    ow_char(o, '"');
    for (i = 0; i < max && s[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            ow_char(o, '\\');
            ow_char(o, (char)c);
        } else if (c == '\n') {
            ow_str(o, "\\n");
        } else if (c == '\r') {
            ow_str(o, "\\r");
        } else if (c == '\t') {
            ow_str(o, "\\t");
        } else if (c == '\b') {
            ow_str(o, "\\b");
        } else if (c == '\f') {
            ow_str(o, "\\f");
        } else if (c < 0x20u) {
            static const char hex[] = "0123456789abcdef";
            ow_str(o, "\\u00");
            ow_char(o, hex[(c >> 4) & 0xfu]);
            ow_char(o, hex[c & 0xfu]);
        } else {
            /* ⚠ Bytes above ASCII go out as they came in: the library does no
             * Unicode validation in either direction (design §5.4). */
            ow_char(o, (char)c);
        }
    }
    ow_char(o, '"');
}

static void ow_int64(gs_out *o, int64_t v)
{
    char tmp[24];
    (void)snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    ow_str(o, tmp);
}

/*
 * ⚠ SHORTEST FORM THAT READS BACK AS THE SAME DOUBLE.  A distance of 4.2 goes
 * out as `4.2`, not `4.2000000000000002`, and a spin of 10454.780354572425
 * survives to its last digit — [OSG] sends seventeen significant figures and a
 * value this library re-emits must not lose one.
 */
static void ow_double(gs_out *o, double v)
{
    char tmp[40];
    int p;

    if (!isfinite(v)) {
        /* JSON has no infinity or NaN, and no client would know what to do
         * with one.  A zero is at least a number. */
        ow_char(o, '0');
        return;
    }
    for (p = 15; p < 17; ++p) {
        (void)snprintf(tmp, sizeof(tmp), "%.*g", p, v);
        if (strtod(tmp, NULL) == v) {
            ow_str(o, tmp);
            return;
        }
    }
    (void)snprintf(tmp, sizeof(tmp), "%.17g", v);
    ow_str(o, tmp);
}

/* Newline and indentation for the [TNB]-style form; nothing when compact. */
static void ow_break(gs_out *o, bool indent, int depth)
{
    int i;
    if (!indent) {
        return;
    }
    ow_char(o, '\n');
    for (i = 0; i < depth * 2; ++i) {
        ow_char(o, ' ');
    }
}

/* ------------------------------------------------------------------------ */
/* gsp_response_encode                                                       */
/* ------------------------------------------------------------------------ */

static bool code_is_ours(int code)
{
    return code == (int)GSP_CODE_SHOT_RECEIVED || code == (int)GSP_CODE_PLAYER_INFO ||
           code == (int)GSP_CODE_READY || code == (int)GSP_CODE_ROUND_ENDED ||
           code == (int)GSP_CODE_FAILURE;
}

static void emit_player(gs_out *o, const gsp_player_info *p)
{
    const char *handed = gsp_handed_text((gsp_handed)p->handed);
    const char *club = gsp_club_code((gsp_club)p->club);
    bool first = true;

    ow_str(o, "{");
    /* ⚠ An UNKNOWN hand or club is OMITTED, never sent as a string GSPro
     * never sends (design §5.6).  gsp_handed_text/gsp_club_code answer "". */
    if (handed[0] != '\0') {
        ow_str(o, "\"" GSP_KEY_PLAYER_HANDED "\":");
        ow_json_string(o, handed, strlen(handed));
        first = false;
    }
    if (club[0] != '\0') {
        if (!first) {
            ow_char(o, ',');
        }
        ow_str(o, "\"" GSP_KEY_PLAYER_CLUB "\":");
        ow_json_string(o, club, strlen(club));
        first = false;
    }
    if (p->has_distance) {
        if (!first) {
            ow_char(o, ',');
        }
        /* ⚠ [OSP] arms its launch monitor on a NON-ZERO DistanceToTarget, so
         * this member is the difference between a device that reports and one
         * that waits (protocol §5.2). */
        ow_str(o, "\"" GSP_KEY_PLAYER_DISTANCE "\":");
        ow_double(o, p->distance_to_target);
        first = false;
    }
    if (p->surface[0] != '\0') {
        if (!first) {
            ow_char(o, ',');
        }
        ow_str(o, "\"" GSP_KEY_PLAYER_SURFACE "\":");
        ow_json_string(o, p->surface, sizeof(p->surface));
    }
    ow_str(o, "}");
}

static void emit_response(gs_out *o, int code, const char *message,
                          const gsp_player_info *player)
{
    ow_str(o, "{\"" GSP_KEY_RESP_CODE "\":");
    ow_int64(o, (int64_t)code);
    if (message != NULL) {
        ow_str(o, ",\"" GSP_KEY_RESP_MESSAGE "\":");
        ow_json_string(o, message, (size_t)-1);
    }
    if (player != NULL) {
        ow_str(o, ",\"" GSP_KEY_RESP_PLAYER "\":");
        emit_player(o, player);
    }
    ow_char(o, '}');
}

gsp_status gsp_response_encode(int code, const char *message, const gsp_player_info *player,
                               char *out, size_t out_size, size_t *written)
{
    gs_out probe;
    gs_out real;

    if (out == NULL || out_size == 0u) {
        return GSP_ERR_INVALID_ARG;
    }
    /* ⚠ There is no send_raw() (design §9.1): anything but the five is refused
     * here rather than reaching a socket. */
    if (!code_is_ours(code)) {
        return GSP_ERR_INVALID_ARG;
    }

    probe.buf = NULL;
    probe.size = 0u;
    probe.used = 0u;
    emit_response(&probe, code, message, player);

    if (probe.used + 1u > out_size) {
        return GSP_ERR_BUFFER_TOO_SMALL;
    }

    real.buf = out;
    real.size = out_size - 1u;
    real.used = 0u;
    emit_response(&real, code, message, player);
    out[real.used] = '\0';
    if (written != NULL) {
        *written = real.used;
    }
    return GSP_OK;
}

/* ------------------------------------------------------------------------ */
/* gsp_message_encode — the other direction, for tools and tests             */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ FOR TALKING TO A libgspro LISTENER.  This library is not a GSPro client
 * (design §1); this exists so gsp_shoot.py and the conformance suite can play
 * a launch monitor at it, and so a host can round-trip a message it built.
 */

typedef struct gs_member_ctx {
    gs_out *o;
    bool    indent;
    int     depth;
    bool    first;
} gs_member_ctx;

static void member(gs_member_ctx *c, const char *name)
{
    if (!c->first) {
        ow_char(c->o, ',');
    }
    c->first = false;
    ow_break(c->o, c->indent, c->depth);
    ow_json_string(c->o, name, (size_t)-1);
    ow_char(c->o, ':');
    if (c->indent) {
        ow_char(c->o, ' ');
    }
}

typedef struct gs_num_out {
    const char *name;
    uint32_t    bit;
    double      value;
} gs_num_out;

static void emit_numeric_section(gs_out *o, bool indent, int depth, const gs_num_out *tab,
                                 size_t n, uint32_t present)
{
    gs_member_ctx c;
    size_t i;

    c.o = o;
    c.indent = indent;
    c.depth = depth + 1;
    c.first = true;

    ow_char(o, '{');
    for (i = 0; i < n; ++i) {
        /* ⚠ A field whose presence bit is clear was NOT on the wire and is
         * omitted rather than sent as a zero (design §4.3). */
        if ((present & tab[i].bit) == 0u) {
            continue;
        }
        member(&c, tab[i].name);
        ow_double(o, tab[i].value);
    }
    if (!c.first) {
        ow_break(o, indent, depth);
    }
    ow_char(o, '}');
}

static void emit_options(gs_out *o, bool indent, int depth, const gsp_shot_options *opt)
{
    gs_member_ctx c;

    c.o = o;
    c.indent = indent;
    c.depth = depth + 1;
    c.first = true;

    ow_char(o, '{');
    /* The two the vendor marks required always go out; the three optional ones
     * only when they were on the wire. */
    member(&c, GSP_KEY_OPT_CONTAINS_BALL);
    ow_str(o, opt->contains_ball_data ? "true" : "false");
    member(&c, GSP_KEY_OPT_CONTAINS_CLUB);
    ow_str(o, opt->contains_club_data ? "true" : "false");
    if ((opt->present & (uint8_t)GSP_OPT_LAUNCH_MONITOR_IS_READY) != 0u) {
        member(&c, GSP_KEY_OPT_LM_READY);
        ow_str(o, opt->launch_monitor_is_ready ? "true" : "false");
    }
    if ((opt->present & (uint8_t)GSP_OPT_LAUNCH_MONITOR_BALL_DETECTED) != 0u) {
        member(&c, GSP_KEY_OPT_BALL_DETECTED);
        ow_str(o, opt->launch_monitor_ball_detected ? "true" : "false");
    }
    if ((opt->present & (uint8_t)GSP_OPT_IS_HEARTBEAT) != 0u) {
        member(&c, GSP_KEY_OPT_HEARTBEAT);
        ow_str(o, opt->is_heartbeat ? "true" : "false");
    }
    ow_break(o, indent, depth);
    ow_char(o, '}');
}

static void emit_message(gs_out *o, const gsp_message *m, bool indent)
{
    gs_member_ctx c;
    const char *units;

    c.o = o;
    c.indent = indent;
    c.depth = 1;
    c.first = true;

    ow_char(o, '{');

    if (m->device_id[0] != '\0') {
        member(&c, GSP_KEY_DEVICE_ID);
        ow_json_string(o, m->device_id, sizeof(m->device_id));
    }
    units = (m->units_text[0] != '\0') ? m->units_text : gsp_units_text((gsp_units)m->units);
    if (units[0] != '\0') {
        member(&c, GSP_KEY_UNITS);
        ow_json_string(o, units, GSP_UNITS_TEXT_MAX);
    }
    member(&c, GSP_KEY_SHOT_NUMBER);
    ow_int64(o, m->shot_number);
    if (m->api_version[0] != '\0') {
        member(&c, GSP_KEY_API_VERSION);
        ow_json_string(o, m->api_version, sizeof(m->api_version));
    }

    if (m->ball.present != 0u) {
        const gs_num_out ball[] = {
            { GSP_KEY_BALL_SPEED, (uint32_t)GSP_BALL_SPEED, m->ball.speed },
            { GSP_KEY_BALL_SPIN_AXIS, (uint32_t)GSP_BALL_SPIN_AXIS, m->ball.spin_axis },
            { GSP_KEY_BALL_TOTAL_SPIN, (uint32_t)GSP_BALL_TOTAL_SPIN, m->ball.total_spin },
            { GSP_KEY_BALL_BACK_SPIN, (uint32_t)GSP_BALL_BACK_SPIN, m->ball.back_spin },
            { GSP_KEY_BALL_SIDE_SPIN, (uint32_t)GSP_BALL_SIDE_SPIN, m->ball.side_spin },
            { GSP_KEY_BALL_HLA, (uint32_t)GSP_BALL_HLA, m->ball.hla },
            { GSP_KEY_BALL_VLA, (uint32_t)GSP_BALL_VLA, m->ball.vla },
            { GSP_KEY_BALL_CARRY, (uint32_t)GSP_BALL_CARRY_DISTANCE, m->ball.carry_distance }
        };
        member(&c, GSP_KEY_BALL_DATA);
        emit_numeric_section(o, indent, 1, ball, sizeof(ball) / sizeof(ball[0]),
                             m->ball.present);
    }
    if (m->club.present != 0u) {
        const gs_num_out club[] = {
            { GSP_KEY_CLUB_SPEED, (uint32_t)GSP_CLUB_SPEED, m->club.speed },
            { GSP_KEY_CLUB_AOA, (uint32_t)GSP_CLUB_ANGLE_OF_ATTACK, m->club.angle_of_attack },
            { GSP_KEY_CLUB_FACE_TO_TARGET, (uint32_t)GSP_CLUB_FACE_TO_TARGET,
              m->club.face_to_target },
            { GSP_KEY_CLUB_LIE, (uint32_t)GSP_CLUB_LIE, m->club.lie },
            { GSP_KEY_CLUB_LOFT, (uint32_t)GSP_CLUB_LOFT, m->club.loft },
            { GSP_KEY_CLUB_PATH, (uint32_t)GSP_CLUB_PATH, m->club.path },
            { GSP_KEY_CLUB_SPEED_AT_IMPACT, (uint32_t)GSP_CLUB_SPEED_AT_IMPACT,
              m->club.speed_at_impact },
            { GSP_KEY_CLUB_VFI, (uint32_t)GSP_CLUB_VERTICAL_FACE_IMPACT,
              m->club.vertical_face_impact },
            { GSP_KEY_CLUB_HFI, (uint32_t)GSP_CLUB_HORIZONTAL_FACE_IMPACT,
              m->club.horizontal_face_impact },
            { GSP_KEY_CLUB_CLOSURE_RATE, (uint32_t)GSP_CLUB_CLOSURE_RATE,
              m->club.closure_rate }
        };
        member(&c, GSP_KEY_CLUB_DATA);
        emit_numeric_section(o, indent, 1, club, sizeof(club) / sizeof(club[0]),
                             m->club.present);
    }

    member(&c, GSP_KEY_SHOT_DATA_OPTIONS);
    emit_options(o, indent, 1, &m->options);

    ow_break(o, indent, 0);
    ow_char(o, '}');
}

gsp_status gsp_message_encode(const gsp_message *m, bool indent, char *out, size_t out_size,
                              size_t *written)
{
    gs_out probe;
    gs_out real;

    if (m == NULL || out == NULL || out_size == 0u) {
        return GSP_ERR_INVALID_ARG;
    }

    probe.buf = NULL;
    probe.size = 0u;
    probe.used = 0u;
    emit_message(&probe, m, indent);

    if (probe.used + 1u > out_size) {
        return GSP_ERR_BUFFER_TOO_SMALL;
    }

    real.buf = out;
    real.size = out_size - 1u;
    real.used = 0u;
    emit_message(&real, m, indent);
    out[real.used] = '\0';
    if (written != NULL) {
        *written = real.used;
    }
    return GSP_OK;
}
