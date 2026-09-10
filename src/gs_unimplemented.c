/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_unimplemented.c — scaffolding, and nothing else.
 *
 * ⚠ THIS IS NOT AN IMPLEMENTATION AND MUST NEVER BECOME ONE.  It exists so
 * that the conformance suite (docs/conformance.md) can be written, built and
 * run BEFORE the library is written — every case failing loudly against a
 * symbol that answers GSP_ERR_NOT_SUPPORTED, rather than the suite failing to
 * link and therefore telling nobody anything.
 *
 * Each group below is compiled out the moment its real source file appears:
 * CMake looks for src/gs_frame.c, gs_decode.c, gs_encode.c, gs_misc.c and
 * gs_server.c and defines GS_HAVE_FRAME, GS_HAVE_DECODE, GS_HAVE_ENCODE,
 * GS_HAVE_MISC, GS_HAVE_SERVER accordingly.  So the sequence of design §11 can
 * land one file at a time with no edit here and no duplicate symbol, and the
 * configure line prints which groups are still standing in.
 *
 * ⚠ WHEN THE LAST GROUP LANDS, DELETE THIS FILE.  It compiles to nothing at
 * that point, so nothing breaks if it is left — which is exactly why it needs
 * saying here rather than being noticed later.
 */

#include "gspro/gspro.h"

#include <string.h>

/* ISO C forbids an empty translation unit, which this becomes once every
 * GS_HAVE_* is defined.  A typedef emits no symbol and satisfies the grammar. */
typedef int gs_unimplemented_translation_unit_is_not_empty;

/* ------------------------------------------------------------------------ */
#ifndef GS_HAVE_MISC

const char *gsp_status_str(gsp_status status)
{
    (void)status;
    return "unimplemented";
}

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
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

gsp_status gsp_abi_check(const gsp_abi_sizes *expected)
{
    (void)expected;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_units gsp_units_parse(const char *text)
{
    (void)text;
    return GSP_UNITS_UNKNOWN;
}

const char *gsp_units_text(gsp_units units)
{
    (void)units;
    return "";
}

gsp_handed gsp_handed_parse(const char *text)
{
    (void)text;
    return GSP_HANDED_UNKNOWN;
}

const char *gsp_handed_text(gsp_handed handed)
{
    (void)handed;
    return "";
}

gsp_club gsp_club_parse(const char *code)
{
    (void)code;
    return GSP_CLUB_UNKNOWN;
}

const char *gsp_club_code(gsp_club club)
{
    (void)club;
    return "";
}

const char *gsp_club_name(gsp_club club)
{
    (void)club;
    return "";
}

bool gsp_message_is_shot(const gsp_message *m)
{
    (void)m;
    return false;
}

bool gsp_message_ball_complete(const gsp_ball_data *ball)
{
    (void)ball;
    return false;
}

bool gsp_message_ball_zero_speed(const gsp_message *m)
{
    (void)m;
    return false;
}

gsp_status gsp_ball_data_derive(gsp_ball_data *ball)
{
    (void)ball;
    return GSP_ERR_NOT_SUPPORTED;
}

size_t gsp_message_format(const gsp_message *m, char *out, size_t out_size,
                          bool include_identifiers)
{
    (void)m;
    (void)include_identifiers;
    if (out != NULL && out_size > 0u) {
        out[0] = '\0';
    }
    return 0u;
}

const char *gsp_message_flag_name(gsp_message_flag flag)
{
    (void)flag;
    return "";
}

bool gsp_player_info_equal(const gsp_player_info *a, const gsp_player_info *b)
{
    (void)a;
    (void)b;
    return false;
}

const char *gsp_event_type_name(gsp_event_type type)
{
    (void)type;
    return "";
}

bool gsp_event_is_sensitive(const gsp_event *ev)
{
    (void)ev;
    return false;
}

size_t gsp_event_format(const gsp_event *ev, char *out, size_t out_size,
                        bool include_identifiers)
{
    (void)ev;
    (void)include_identifiers;
    if (out != NULL && out_size > 0u) {
        out[0] = '\0';
    }
    return 0u;
}

#endif /* GS_HAVE_MISC */

/* ------------------------------------------------------------------------ */
#ifndef GS_HAVE_FRAME

gsp_status gsp_frame_find(const uint8_t *buf, size_t len, size_t *start, size_t *end)
{
    (void)buf;
    (void)len;
    (void)start;
    (void)end;
    return GSP_ERR_NOT_SUPPORTED;
}

#endif /* GS_HAVE_FRAME */

/* ------------------------------------------------------------------------ */
#ifndef GS_HAVE_DECODE

gsp_status gsp_message_decode(const uint8_t *json, size_t len, gsp_message *out)
{
    (void)json;
    (void)len;
    (void)out;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_status gsp_response_decode(const uint8_t *json, size_t len, gsp_response *out)
{
    (void)json;
    (void)len;
    (void)out;
    return GSP_ERR_NOT_SUPPORTED;
}

#endif /* GS_HAVE_DECODE */

/* ------------------------------------------------------------------------ */
#ifndef GS_HAVE_ENCODE

gsp_status gsp_response_encode(int code, const char *message, const gsp_player_info *player,
                               char *out, size_t out_size, size_t *written)
{
    (void)code;
    (void)message;
    (void)player;
    (void)out;
    (void)out_size;
    (void)written;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_status gsp_message_encode(const gsp_message *m, bool indent, char *out, size_t out_size,
                              size_t *written)
{
    (void)m;
    (void)indent;
    (void)out;
    (void)out_size;
    (void)written;
    return GSP_ERR_NOT_SUPPORTED;
}

#endif /* GS_HAVE_ENCODE */

/* ------------------------------------------------------------------------ */
#ifndef GS_HAVE_SERVER

gsp_server_policy gsp_server_policy_default(void)
{
    gsp_server_policy p;
    memset(&p, 0, sizeof(p));
    return p;
}

gsp_server_config gsp_server_config_default(void)
{
    gsp_server_config c;
    memset(&c, 0, sizeof(c));
    return c;
}

gsp_status gsp_server_create(const gsp_server_config *config, gsp_server **out)
{
    (void)config;
    if (out != NULL) {
        *out = NULL;
    }
    return GSP_ERR_NOT_SUPPORTED;
}

void gsp_server_close(gsp_server *s)
{
    (void)s;
}

void gsp_server_destroy(gsp_server *s)
{
    (void)s;
}

gsp_status gsp_server_on_connection_opened(gsp_server *s, gsp_conn_id conn, const char *peer,
                                           gsp_time_us now_us)
{
    (void)s;
    (void)conn;
    (void)peer;
    (void)now_us;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_status gsp_server_on_bytes(gsp_server *s, gsp_conn_id conn, const uint8_t *data, size_t len,
                               gsp_time_us now_us)
{
    (void)s;
    (void)conn;
    (void)data;
    (void)len;
    (void)now_us;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_status gsp_server_on_connection_closed(gsp_server *s, gsp_conn_id conn,
                                           gsp_close_cause cause, gsp_time_us now_us)
{
    (void)s;
    (void)conn;
    (void)cause;
    (void)now_us;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_time_us gsp_server_next_due_us(const gsp_server *s)
{
    (void)s;
    return GSP_TIME_NEVER;
}

void gsp_server_tick(gsp_server *s, gsp_time_us now_us)
{
    (void)s;
    (void)now_us;
}

gsp_status gsp_server_set_player(gsp_server *s, const gsp_player_info *info, gsp_time_us now_us)
{
    (void)s;
    (void)info;
    (void)now_us;
    return GSP_ERR_NOT_SUPPORTED;
}

void gsp_server_get_player(const gsp_server *s, gsp_player_info *out)
{
    (void)s;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

gsp_status gsp_server_send_player_info(gsp_server *s, gsp_conn_id conn,
                                       const gsp_player_info *info, gsp_time_us now_us)
{
    (void)s;
    (void)conn;
    (void)info;
    (void)now_us;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_status gsp_server_set_session_state(gsp_server *s, gsp_session_state state,
                                        gsp_time_us now_us)
{
    (void)s;
    (void)state;
    (void)now_us;
    return GSP_ERR_NOT_SUPPORTED;
}

gsp_session_state gsp_server_get_session_state(const gsp_server *s)
{
    (void)s;
    return GSP_SESSION_NONE;
}

size_t gsp_server_poll_writes(gsp_server *s, gsp_write_request *out, size_t max)
{
    (void)s;
    (void)out;
    (void)max;
    return 0u;
}

size_t gsp_server_poll_events(gsp_server *s, gsp_event *out, size_t max)
{
    (void)s;
    (void)out;
    (void)max;
    return 0u;
}

size_t gsp_server_poll_wire(gsp_server *s, gsp_wire_chunk *out, size_t max)
{
    (void)s;
    (void)out;
    (void)max;
    return 0u;
}

uint32_t gsp_server_dropped_events(const gsp_server *s)
{
    (void)s;
    return 0u;
}

uint32_t gsp_server_dropped_wire(const gsp_server *s)
{
    (void)s;
    return 0u;
}

size_t gsp_server_connection_count(const gsp_server *s)
{
    (void)s;
    return 0u;
}

size_t gsp_server_connection_ids(const gsp_server *s, gsp_conn_id *out, size_t max)
{
    (void)s;
    (void)out;
    (void)max;
    return 0u;
}

gsp_status gsp_server_connection_info(const gsp_server *s, gsp_conn_id conn,
                                      gsp_connection_info *out)
{
    (void)s;
    (void)conn;
    (void)out;
    return GSP_ERR_NOT_SUPPORTED;
}

#endif /* GS_HAVE_SERVER */
