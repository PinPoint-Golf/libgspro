/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_abi_table.c — every public struct's size, every field's offset, and every
 * enumerator, read straight out of the compiler and printed as JSON.
 *
 * ⚠ WHY THIS EXISTS.  gsp_abi_check() compares ELEVEN STRUCT SIZES
 * (include/gspro/version.h) and says nothing whatever about where the fields
 * inside them sit, or about any enum at all.  version.h says so itself: "a
 * binding whose field is one slot out passes this check and returns plausible
 * numbers".  A `spin_axis` read at `total_spin`'s offset gives every shot a
 * wrong-but-believable spin axis, forever, with no error anywhere.
 *
 * So python/gspro/_types.py — which declares all of this a SECOND time — is not
 * trusted on inspection and is not hand-verified.  It is pinned against this
 * table by tests/test_python_abi.py.
 *
 * ⚠ ADDING A FIELD TO A PUBLIC STRUCT MEANS ADDING A ROW HERE, and three
 * things check that you did.  Each is listed with what it CANNOT see, because
 * an unbounded completeness claim is worse than a bounded one:
 *
 *   1. The tiling self-check below — rows must start at 0, never overlap, and
 *      reach the end, with no gap wider than the struct's own alignment.
 *      ⚠ BLIND to a dropped row whose bytes are indistinguishable from the
 *      padding that would have been there anyway.
 *   2. sizeof here against ctypes.sizeof in tests/test_python_abi.py, for
 *      EVERY struct in this table rather than the eleven gsp_abi_check() knows.
 *      A field added to a header changes the struct's size, so this is what
 *      actually catches a forgotten row.  ⚠ Blind to a field carved out of an
 *      existing `reserved` array, which does not change the size.
 *   3. The field-name sets, compared in BOTH directions by the same test.
 *
 * The one shape all three miss: a field carved out of a `reserved` slot and
 * updated in the header but in neither this table nor the binding.  It is
 * narrow — the reserved arrays are themselves rows here, so shrinking one
 * changes that row's size and trips (2) — and it is written down rather than
 * engineered around.
 *
 * ⚠ This is a DEVELOPMENT tool.  Nothing at runtime reads it; a shipped
 * binding still guards itself with gsp_abi_check() at import.
 */

#include "gspro/gspro.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct abi_field {
    const char *name;
    size_t      offset;
    size_t      size;
} abi_field;

typedef struct abi_struct {
    const char      *name;
    size_t           size;
    size_t           align;
    const abi_field *fields;
    size_t           field_count;
} abi_struct;

typedef struct abi_enumerator {
    const char *name;
    long long   value;
} abi_enumerator;

typedef struct abi_enum {
    const char           *name;
    const abi_enumerator *values;
    size_t                count;
} abi_enum;

/* ⚠ sizeof on a member expression of a null pointer never dereferences it —
 * the operand of sizeof is unevaluated.  This is the standard idiom, and it is
 * what lets one macro cover scalars, arrays, nested structs and the union. */
#define F(type, member) \
    { #member, offsetof(type, member), sizeof(((type *)0)->member) }

#define S(type, fields) \
    { #type, sizeof(type), _Alignof(type), (fields), sizeof(fields) / sizeof((fields)[0]) }

#define E(name) { #name, (long long)(name) }

#define EN(cname, values) { cname, (values), sizeof(values) / sizeof((values)[0]) }

/* ------------------------------------------------------------------------ */
/* Structs                                                                   */
/* ------------------------------------------------------------------------ */

static const abi_field f_allocator[] = {
    F(gsp_allocator, alloc), F(gsp_allocator, free), F(gsp_allocator, ctx),
};

static const abi_field f_ball_data[] = {
    F(gsp_ball_data, speed),     F(gsp_ball_data, spin_axis),
    F(gsp_ball_data, total_spin), F(gsp_ball_data, back_spin),
    F(gsp_ball_data, side_spin), F(gsp_ball_data, hla),
    F(gsp_ball_data, vla),       F(gsp_ball_data, carry_distance),
    F(gsp_ball_data, present),   F(gsp_ball_data, derived),
};

static const abi_field f_club_data[] = {
    F(gsp_club_data, speed),                F(gsp_club_data, angle_of_attack),
    F(gsp_club_data, face_to_target),       F(gsp_club_data, lie),
    F(gsp_club_data, loft),                 F(gsp_club_data, path),
    F(gsp_club_data, speed_at_impact),      F(gsp_club_data, vertical_face_impact),
    F(gsp_club_data, horizontal_face_impact), F(gsp_club_data, closure_rate),
    F(gsp_club_data, present),              F(gsp_club_data, reserved),
};

static const abi_field f_shot_options[] = {
    F(gsp_shot_options, contains_ball_data),
    F(gsp_shot_options, contains_club_data),
    F(gsp_shot_options, launch_monitor_is_ready),
    F(gsp_shot_options, launch_monitor_ball_detected),
    F(gsp_shot_options, is_heartbeat),
    F(gsp_shot_options, present),
    F(gsp_shot_options, reserved),
};

static const abi_field f_message[] = {
    F(gsp_message, conn),        F(gsp_message, sequence),
    F(gsp_message, host_recv_us), F(gsp_message, kind),
    F(gsp_message, units),       F(gsp_message, reserved0),
    F(gsp_message, flags),       F(gsp_message, shot_number),
    F(gsp_message, device_id),   F(gsp_message, units_text),
    F(gsp_message, api_version), F(gsp_message, options),
    F(gsp_message, ball),        F(gsp_message, club),
    F(gsp_message, wire_length), F(gsp_message, unknown_keys),
    F(gsp_message, reserved1),
};

static const abi_field f_player_info[] = {
    F(gsp_player_info, handed),   F(gsp_player_info, club),
    F(gsp_player_info, has_distance), F(gsp_player_info, reserved),
    F(gsp_player_info, distance_to_target), F(gsp_player_info, surface),
};

static const abi_field f_connection_info[] = {
    F(gsp_connection_info, conn),          F(gsp_connection_info, identified),
    F(gsp_connection_info, ready),         F(gsp_connection_info, ball_detected),
    F(gsp_connection_info, reserved0),     F(gsp_connection_info, opened_us),
    F(gsp_connection_info, last_message_us), F(gsp_connection_info, messages),
    F(gsp_connection_info, shots),         F(gsp_connection_info, protocol_errors),
    F(gsp_connection_info, consecutive_errors),
    F(gsp_connection_info, last_shot_number), F(gsp_connection_info, peer),
    F(gsp_connection_info, device_id),
};

static const abi_field f_connection_event[] = {
    F(gsp_connection_event, info), F(gsp_connection_event, cause),
    F(gsp_connection_event, reserved),
};

static const abi_field f_client_state_event[] = {
    F(gsp_client_state_event, ready),
    F(gsp_client_state_event, ball_detected),
    F(gsp_client_state_event, previous_ready),
    F(gsp_client_state_event, previous_ball_detected),
    F(gsp_client_state_event, reserved),
};

static const abi_field f_player_info_event[] = {
    F(gsp_player_info_event, player), F(gsp_player_info_event, reason),
    F(gsp_player_info_event, reserved),
};

static const abi_field f_session_state_event[] = {
    F(gsp_session_state_event, state), F(gsp_session_state_event, reason),
    F(gsp_session_state_event, reserved),
};

static const abi_field f_protocol_error_event[] = {
    F(gsp_protocol_error_event, reason),
    F(gsp_protocol_error_event, snippet_length),
    F(gsp_protocol_error_event, reserved),
    F(gsp_protocol_error_event, discarded_bytes),
    F(gsp_protocol_error_event, consecutive),
    F(gsp_protocol_error_event, snippet),
};

static const abi_field f_warning_event[] = {
    F(gsp_warning_event, code), F(gsp_warning_event, reserved),
    F(gsp_warning_event, value), F(gsp_warning_event, text),
};

static const abi_field f_event[] = {
    F(gsp_event, type),      F(gsp_event, reserved0), F(gsp_event, sequence),
    F(gsp_event, host_time_us), F(gsp_event, conn),   F(gsp_event, reserved1),
    F(gsp_event, u),
};

static const abi_field f_write_request[] = {
    F(gsp_write_request, conn), F(gsp_write_request, length),
    F(gsp_write_request, kind), F(gsp_write_request, reserved),
    F(gsp_write_request, data),
};

static const abi_field f_wire_chunk[] = {
    F(gsp_wire_chunk, host_time_us), F(gsp_wire_chunk, conn),
    F(gsp_wire_chunk, length),       F(gsp_wire_chunk, direction),
    F(gsp_wire_chunk, flags),        F(gsp_wire_chunk, sequence),
    F(gsp_wire_chunk, reserved),     F(gsp_wire_chunk, data),
};

static const abi_field f_server_policy[] = {
    F(gsp_server_policy, max_message_bytes),
    F(gsp_server_policy, idle_alarm_us),
    F(gsp_server_policy, protocol_error_close_threshold),
    F(gsp_server_policy, write_spacing_us),
    F(gsp_server_policy, reject_incomplete_shots),
    F(gsp_server_policy, announce_player_on_connect),
    F(gsp_server_policy, announce_ready_on_connect),
    F(gsp_server_policy, record_identifiers),
    F(gsp_server_policy, reserved),
    F(gsp_server_policy, ack_text),
};

static const abi_field f_server_config[] = {
    F(gsp_server_config, max_connections), F(gsp_server_config, event_ring),
    F(gsp_server_config, write_ring),      F(gsp_server_config, wire_ring),
    F(gsp_server_config, policy),          F(gsp_server_config, allocator),
};

static const abi_field f_abi_sizes[] = {
    F(gsp_abi_sizes, abi_version),   F(gsp_abi_sizes, message),
    F(gsp_abi_sizes, ball_data),     F(gsp_abi_sizes, club_data),
    F(gsp_abi_sizes, shot_options),  F(gsp_abi_sizes, player_info),
    F(gsp_abi_sizes, event),         F(gsp_abi_sizes, write_request),
    F(gsp_abi_sizes, wire_chunk),    F(gsp_abi_sizes, connection_info),
    F(gsp_abi_sizes, server_config), F(gsp_abi_sizes, message_layout_version),
};

static const abi_field f_response[] = {
    F(gsp_response, code),     F(gsp_response, has_message),
    F(gsp_response, has_player), F(gsp_response, reserved),
    F(gsp_response, message),  F(gsp_response, player),
};

static const abi_struct k_structs[] = {
    S(gsp_allocator, f_allocator),
    S(gsp_ball_data, f_ball_data),
    S(gsp_club_data, f_club_data),
    S(gsp_shot_options, f_shot_options),
    S(gsp_message, f_message),
    S(gsp_player_info, f_player_info),
    S(gsp_connection_info, f_connection_info),
    S(gsp_connection_event, f_connection_event),
    S(gsp_client_state_event, f_client_state_event),
    S(gsp_player_info_event, f_player_info_event),
    S(gsp_session_state_event, f_session_state_event),
    S(gsp_protocol_error_event, f_protocol_error_event),
    S(gsp_warning_event, f_warning_event),
    S(gsp_event, f_event),
    S(gsp_write_request, f_write_request),
    S(gsp_wire_chunk, f_wire_chunk),
    S(gsp_server_policy, f_server_policy),
    S(gsp_server_config, f_server_config),
    S(gsp_abi_sizes, f_abi_sizes),
    S(gsp_response, f_response),
};

/* ------------------------------------------------------------------------ */
/* Enums                                                                     */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ THE SAME FAILURE, ONE REGISTER DOWN.  A binding that transcribes an enum by
 * hand and gets one member's number wrong lands every later member on its
 * neighbour: a club that reads as the next club up puts a launch monitor into
 * the wrong mode with nothing reporting a fault.  libwrist's equivalent test
 * caught exactly this on the day it was written.
 */

static const abi_enumerator e_status[] = {
    E(GSP_OK), E(GSP_PENDING), E(GSP_ERR_INVALID_ARG), E(GSP_ERR_INVALID_STATE),
    E(GSP_ERR_NO_MEMORY), E(GSP_ERR_BUFFER_TOO_SMALL), E(GSP_ERR_NOT_SUPPORTED),
    E(GSP_ERR_TOO_MANY_CONNECTIONS), E(GSP_ERR_UNKNOWN_CONNECTION),
    E(GSP_ERR_MALFORMED), E(GSP_ERR_MESSAGE_TOO_LARGE), E(GSP_ERR_QUEUE_FULL),
    E(GSP_ERR_CLOSED),
};

static const abi_enumerator e_units[] = {
    E(GSP_UNITS_UNKNOWN), E(GSP_UNITS_YARDS), E(GSP_UNITS_METERS),
};

static const abi_enumerator e_handed[] = {
    E(GSP_HANDED_UNKNOWN), E(GSP_HANDED_RIGHT), E(GSP_HANDED_LEFT),
};

static const abi_enumerator e_club[] = {
    E(GSP_CLUB_UNKNOWN), E(GSP_CLUB_DR),
    E(GSP_CLUB_W2), E(GSP_CLUB_W3), E(GSP_CLUB_W4), E(GSP_CLUB_W5),
    E(GSP_CLUB_W6), E(GSP_CLUB_W7),
    E(GSP_CLUB_H2), E(GSP_CLUB_H3), E(GSP_CLUB_H4), E(GSP_CLUB_H5),
    E(GSP_CLUB_H6), E(GSP_CLUB_H7),
    E(GSP_CLUB_I1), E(GSP_CLUB_I2), E(GSP_CLUB_I3), E(GSP_CLUB_I4),
    E(GSP_CLUB_I5), E(GSP_CLUB_I6), E(GSP_CLUB_I7), E(GSP_CLUB_I8),
    E(GSP_CLUB_I9),
    E(GSP_CLUB_PW), E(GSP_CLUB_GW), E(GSP_CLUB_SW), E(GSP_CLUB_LW),
    E(GSP_CLUB_PT),
};

static const abi_enumerator e_response_code[] = {
    E(GSP_CODE_SHOT_RECEIVED), E(GSP_CODE_PLAYER_INFO), E(GSP_CODE_READY),
    E(GSP_CODE_ROUND_ENDED), E(GSP_CODE_FAILURE),
};

static const abi_enumerator e_ball_field[] = {
    E(GSP_BALL_SPEED), E(GSP_BALL_SPIN_AXIS), E(GSP_BALL_TOTAL_SPIN),
    E(GSP_BALL_BACK_SPIN), E(GSP_BALL_SIDE_SPIN), E(GSP_BALL_HLA),
    E(GSP_BALL_VLA), E(GSP_BALL_CARRY_DISTANCE),
};

static const abi_enumerator e_club_field[] = {
    E(GSP_CLUB_SPEED), E(GSP_CLUB_ANGLE_OF_ATTACK), E(GSP_CLUB_FACE_TO_TARGET),
    E(GSP_CLUB_LIE), E(GSP_CLUB_LOFT), E(GSP_CLUB_PATH),
    E(GSP_CLUB_SPEED_AT_IMPACT), E(GSP_CLUB_VERTICAL_FACE_IMPACT),
    E(GSP_CLUB_HORIZONTAL_FACE_IMPACT), E(GSP_CLUB_CLOSURE_RATE),
};

static const abi_enumerator e_option_field[] = {
    E(GSP_OPT_CONTAINS_BALL_DATA), E(GSP_OPT_CONTAINS_CLUB_DATA),
    E(GSP_OPT_LAUNCH_MONITOR_IS_READY), E(GSP_OPT_LAUNCH_MONITOR_BALL_DETECTED),
    E(GSP_OPT_IS_HEARTBEAT),
};

static const abi_enumerator e_message_kind[] = {
    E(GSP_MSG_NONE), E(GSP_MSG_SHOT), E(GSP_MSG_HEARTBEAT), E(GSP_MSG_STATUS),
};

static const abi_enumerator e_message_flag[] = {
    E(GSP_MSGF_MISSING_DEVICE_ID), E(GSP_MSGF_MISSING_SHOT_NUMBER),
    E(GSP_MSGF_SHOT_NUMBER_NOT_INTEGER), E(GSP_MSGF_MISSING_API_VERSION),
    E(GSP_MSGF_UNKNOWN_API_VERSION), E(GSP_MSGF_MISSING_OPTIONS),
    E(GSP_MSGF_BALL_FLAG_WITHOUT_OBJECT), E(GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG),
    E(GSP_MSGF_CLUB_FLAG_WITHOUT_OBJECT), E(GSP_MSGF_CLUB_OBJECT_WITHOUT_FLAG),
    E(GSP_MSGF_BALL_INCOMPLETE), E(GSP_MSGF_UNKNOWN_UNITS),
    E(GSP_MSGF_KEY_CASE_MISMATCH), E(GSP_MSGF_UNKNOWN_KEYS),
    E(GSP_MSGF_SHOT_NUMBER_REPEATED), E(GSP_MSGF_STRING_TRUNCATED),
    E(GSP_MSGF_TYPE_COERCED),
};

static const abi_enumerator e_event_type[] = {
    E(GSP_EV_NONE), E(GSP_EV_CONNECTION_OPENED), E(GSP_EV_CONNECTION_CLOSED),
    E(GSP_EV_CLIENT_IDENTIFIED), E(GSP_EV_SHOT), E(GSP_EV_HEARTBEAT),
    E(GSP_EV_STATUS), E(GSP_EV_CLIENT_STATE), E(GSP_EV_PLAYER_INFO_SENT),
    E(GSP_EV_SESSION_STATE_SENT), E(GSP_EV_PROTOCOL_ERROR),
    E(GSP_EV_CLOSE_REQUESTED), E(GSP_EV_CLIENT_IDLE), E(GSP_EV_CLIENT_ACTIVE),
    E(GSP_EV_WARNING),
};

static const abi_enumerator e_close_cause[] = {
    E(GSP_CLOSE_UNKNOWN), E(GSP_CLOSE_REMOTE_CLOSED), E(GSP_CLOSE_LOCAL_REQUEST),
    E(GSP_CLOSE_TRANSPORT_ERROR), E(GSP_CLOSE_SERVER_CLOSED),
};

static const abi_enumerator e_player_info_reason[] = {
    E(GSP_PI_CHANGED), E(GSP_PI_ON_CONNECT), E(GSP_PI_EXPLICIT),
};

static const abi_enumerator e_protocol_error_reason[] = {
    E(GSP_PE_LEADING_GARBAGE), E(GSP_PE_BAD_JSON), E(GSP_PE_TOO_LARGE),
};

static const abi_enumerator e_warning_code[] = {
    E(GSP_WARN_NONE), E(GSP_WARN_DEVICE_ID_CHANGED), E(GSP_WARN_STRING_TRUNCATED),
    E(GSP_WARN_EVENTS_DROPPED), E(GSP_WARN_WIRE_DROPPED),
    E(GSP_WARN_WRITE_RING_FULL),
};

static const abi_enumerator e_write_kind[] = {
    E(GSP_WRITE_ACK), E(GSP_WRITE_PLAYER_INFO), E(GSP_WRITE_READY),
    E(GSP_WRITE_ROUND_ENDED), E(GSP_WRITE_FAILURE),
};

static const abi_enumerator e_wire_direction[] = {
    E(GSP_WIRE_CLIENT_TO_SERVER), E(GSP_WIRE_SERVER_TO_CLIENT), E(GSP_WIRE_META),
};

static const abi_enumerator e_wire_flag[] = {
    E(GSP_WIRE_REDACTED), E(GSP_WIRE_LOST), E(GSP_WIRE_CONTINUES),
};

static const abi_enumerator e_session_state[] = {
    E(GSP_SESSION_NONE), E(GSP_SESSION_ACTIVE), E(GSP_SESSION_ENDED),
};

/* ⚠ The three COUNT sentinels are NOT enumerators here.  GSP_EVENT_TYPE_COUNT
 * does not even share its enum's `GSP_EV_` prefix, and none of the three is a
 * value that can arrive on a wire — they are bounds.  They are pinned below as
 * constants instead, where a binding that loops to one of them still finds it
 * checked. */
static const abi_enum k_enums[] = {
    EN("gsp_status", e_status),
    EN("gsp_units", e_units),
    EN("gsp_handed", e_handed),
    EN("gsp_club", e_club),
    EN("gsp_response_code", e_response_code),
    EN("gsp_ball_field", e_ball_field),
    EN("gsp_club_field", e_club_field),
    EN("gsp_option_field", e_option_field),
    EN("gsp_message_kind", e_message_kind),
    EN("gsp_message_flag", e_message_flag),
    EN("gsp_event_type", e_event_type),
    EN("gsp_close_cause", e_close_cause),
    EN("gsp_player_info_reason", e_player_info_reason),
    EN("gsp_protocol_error_reason", e_protocol_error_reason),
    EN("gsp_warning_code", e_warning_code),
    EN("gsp_write_kind", e_write_kind),
    EN("gsp_wire_direction", e_wire_direction),
    EN("gsp_wire_flag", e_wire_flag),
    EN("gsp_session_state", e_session_state),
};

/* ------------------------------------------------------------------------ */
/* Constants a binding has to hold a second copy of                          */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ A BOUND IS PART OF A LAYOUT.  `char device_id[GSP_DEVICE_ID_MAX]` is a row
 * above and its size is checked — but a binding also uses these numbers on
 * their own, to size a buffer or to truncate a string before handing it over.
 * A stale copy of GSP_WRITE_MAX in Python reads a write request short.
 */
typedef struct abi_constant {
    const char *name;
    long long   value;
} abi_constant;

static const abi_constant k_constants[] = {
    E(GSP_DEVICE_ID_MAX),    E(GSP_UNITS_TEXT_MAX),   E(GSP_API_VERSION_MAX),
    E(GSP_SURFACE_MAX),      E(GSP_PEER_MAX),         E(GSP_ERROR_SNIPPET_MAX),
    E(GSP_WARNING_TEXT_MAX), E(GSP_ACK_TEXT_MAX),     E(GSP_WRITE_MAX),
    E(GSP_WIRE_CHUNK_MAX),   E(GSP_RESPONSE_TEXT_MAX),
    E(GSP_MESSAGE_LAYOUT_VERSION), E(GSP_ABI_VERSION),
    E(GSP_CLUB_COUNT), E(GSP_EVENT_TYPE_COUNT), E(GSP_WARNING_CODE_COUNT),
    E(GSP_DEFAULT_PORT),     E(GSP_ALT_PORT),
    E(GSP_MAX_CONNECTIONS_DEFAULT), E(GSP_EVENT_RING_DEFAULT),
    E(GSP_WRITE_RING_DEFAULT), E(GSP_WIRE_RING_RECOMMENDED),
};

/* ------------------------------------------------------------------------ */
/* The self-check                                                            */
/* ------------------------------------------------------------------------ */
/*
 * The rows must TILE their struct.  Padding is the only thing allowed between
 * two fields, and padding before a field can never exceed the struct's own
 * alignment minus one — so a gap of `align` or more means a field is missing
 * from the row above it.  The same bound applies to the tail.
 *
 * ⚠ THIS IS THE WEAKEST OF THE THREE CHECKS.  It catches an appended field and
 * a mis-sized row outright; it is blind to a dropped row that hides inside
 * padding.  The struct-size comparison in tests/test_python_abi.py is what
 * actually closes that.  Read a clean run here as the table being
 * self-consistent, never as the table being complete.
 */
static int selfcheck_one(const abi_struct *s, FILE *err)
{
    size_t end = 0u;
    size_t i;
    int    problems = 0;

    if (s->field_count == 0u) {
        fprintf(err, "abi: %s has no fields listed\n", s->name);
        return 1;
    }
    for (i = 0u; i < s->field_count; i++) {
        const abi_field *f = &s->fields[i];
        if (f->offset < end) {
            fprintf(err, "abi: %s.%s overlaps the field before it (offset %zu < %zu)\n",
                    s->name, f->name, f->offset, end);
            problems++;
        } else if (f->offset - end >= s->align) {
            fprintf(err,
                    "abi: %s has an unexplained %zu-byte hole before .%s — "
                    "a field is MISSING from tools/gs_abi_table.c\n",
                    s->name, f->offset - end, f->name);
            problems++;
        }
        end = f->offset + f->size;
    }
    if (end > s->size) {
        fprintf(err, "abi: %s runs %zu bytes past sizeof (%zu)\n", s->name, end - s->size,
                s->size);
        problems++;
    } else if (s->size - end >= s->align) {
        fprintf(err,
                "abi: %s has %zu bytes of unexplained tail past .%s — "
                "a field is MISSING from tools/gs_abi_table.c\n",
                s->name, s->size - end, s->fields[s->field_count - 1u].name);
        problems++;
    }
    return problems;
}

/* ------------------------------------------------------------------------ */
int main(void)
{
    gsp_abi_sizes sizes;
    size_t        i;
    size_t        j;
    int           problems = 0;

    gsp_abi_sizes_get(&sizes);

    printf("{\n");
    printf("  \"abi_version\": %u,\n", (unsigned)sizes.abi_version);
    printf("  \"message_layout_version\": %u,\n", (unsigned)sizes.message_layout_version);
    printf("  \"library_version\": \"%s\",\n", gsp_version_string());

    printf("  \"structs\": {\n");
    for (i = 0u; i < sizeof(k_structs) / sizeof(k_structs[0]); i++) {
        const abi_struct *s = &k_structs[i];
        problems += selfcheck_one(s, stderr);
        printf("    \"%s\": {\n", s->name);
        printf("      \"size\": %zu,\n", s->size);
        printf("      \"align\": %zu,\n", s->align);
        printf("      \"fields\": [\n");
        for (j = 0u; j < s->field_count; j++) {
            const abi_field *f = &s->fields[j];
            printf("        {\"name\": \"%s\", \"offset\": %zu, \"size\": %zu}%s\n", f->name,
                   f->offset, f->size, (j + 1u == s->field_count) ? "" : ",");
        }
        printf("      ]\n");
        printf("    }%s\n", (i + 1u == sizeof(k_structs) / sizeof(k_structs[0])) ? "" : ",");
    }
    printf("  },\n");

    printf("  \"enums\": {\n");
    for (i = 0u; i < sizeof(k_enums) / sizeof(k_enums[0]); i++) {
        const abi_enum *e = &k_enums[i];
        printf("    \"%s\": {\n", e->name);
        for (j = 0u; j < e->count; j++) {
            printf("      \"%s\": %lld%s\n", e->values[j].name, e->values[j].value,
                   (j + 1u == e->count) ? "" : ",");
        }
        printf("    }%s\n", (i + 1u == sizeof(k_enums) / sizeof(k_enums[0])) ? "" : ",");
    }
    printf("  },\n");

    printf("  \"constants\": {\n");
    for (i = 0u; i < sizeof(k_constants) / sizeof(k_constants[0]); i++) {
        printf("    \"%s\": %lld%s\n", k_constants[i].name, k_constants[i].value,
               (i + 1u == sizeof(k_constants) / sizeof(k_constants[0])) ? "" : ",");
    }
    printf("  }\n");
    printf("}\n");

    if (problems > 0) {
        fprintf(stderr,
                "\nabi: %d self-check problem(s).  The table is out of step with the "
                "headers; NOTHING it printed should be trusted.\n",
                problems);
    }
    return problems == 0 ? 0 : 1;
}
