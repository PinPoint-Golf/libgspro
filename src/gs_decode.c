/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_decode.c — the bounded JSON reader (gs_json.h) and the two decoders.
 *
 * design §5.4.  ⚠ EVERY TOLERANCE HERE IS A REAL CLIENT, not defensive
 * programming.  Keys match case-insensitively because [MLM] spells it
 * "Backspin" and [R10] "APIVersion" and both work against GSPro; numbers are
 * read as double because [OSG] sends `13.0` where an integer belongs; a null
 * object is absence because [OB] sends `"ClubData": null`; a JSON string
 * holding a number is coerced because [SB] sends every value that way.  A
 * decoder that refused any of them would lose shots from a device somebody
 * owns (conformance §1.1).
 *
 * ⚠ AND A MESSAGE THAT PARSED IS DELIVERED, WHATEVER IT IS MISSING (design
 * §4.5).  "Required" on the vendor's page is what a client should send, not
 * what a server can insist on.  So the findings go in out->flags and the
 * application decides; GSP_ERR_MALFORMED is only for bytes that are not a JSON
 * object at all.
 */

#include "gspro/codec.h"

#include "gs_json.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Longest key this reader will hold.  The longest it knows is
 * "LaunchMonitorBallDetected" at 25; anything longer cannot match one and is
 * therefore an unknown key however it is truncated. */
#define GS_KEY_MAX 64

/* ⚠ A hostile client must not be able to recurse the stack away (design §9.3).
 * The protocol nests two deep; 32 is past anything legitimate. */
#define GS_JSON_DEPTH_MAX 32u

/* ------------------------------------------------------------------------ */
/* The reader                                                                */
/* ------------------------------------------------------------------------ */

void gs_json_skip_ws(gs_json *j)
{
    while (j->pos < j->len && gs_is_ws(j->buf[j->pos])) {
        j->pos++;
    }
}

bool gs_json_at_end(gs_json *j)
{
    gs_json_skip_ws(j);
    return j->pos >= j->len;
}

gs_json_type gs_json_peek(gs_json *j)
{
    uint8_t c;

    gs_json_skip_ws(j);
    if (j->pos >= j->len) {
        return GS_JSON_NONE;
    }
    c = j->buf[j->pos];
    if (c == '{') {
        return GS_JSON_OBJECT;
    }
    if (c == '[') {
        return GS_JSON_ARRAY;
    }
    if (c == '"') {
        return GS_JSON_STRING;
    }
    if (c == 't') {
        return GS_JSON_TRUE;
    }
    if (c == 'f') {
        return GS_JSON_FALSE;
    }
    if (c == 'n') {
        return GS_JSON_NULL;
    }
    /* ⚠ '+' and a bare leading '.' are not JSON, but strtod takes them and a
     * client that emits one is telling us a number.  The grammar this reader
     * enforces is strtod's (design §5.4). */
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
        return GS_JSON_NUMBER;
    }
    return GS_JSON_NONE;
}

bool gs_json_accept(gs_json *j, char c)
{
    gs_json_skip_ws(j);
    if (j->pos < j->len && j->buf[j->pos] == (uint8_t)c) {
        j->pos++;
        return true;
    }
    return false;
}

/* One decoded byte into a bounded field, or a note that it did not fit. */
static void put(char *out, size_t out_size, size_t *used, bool *truncated, char c)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    if (*used + 1u < out_size) {
        out[*used] = c;
        (*used)++;
        return;
    }
    if (truncated != NULL) {
        *truncated = true;
    }
}

static void put_utf8(char *out, size_t out_size, size_t *used, bool *truncated, uint32_t cp)
{
    if (cp < 0x80u) {
        put(out, out_size, used, truncated, (char)cp);
    } else if (cp < 0x800u) {
        put(out, out_size, used, truncated, (char)(0xc0u | (cp >> 6)));
        put(out, out_size, used, truncated, (char)(0x80u | (cp & 0x3fu)));
    } else if (cp < 0x10000u) {
        put(out, out_size, used, truncated, (char)(0xe0u | (cp >> 12)));
        put(out, out_size, used, truncated, (char)(0x80u | ((cp >> 6) & 0x3fu)));
        put(out, out_size, used, truncated, (char)(0x80u | (cp & 0x3fu)));
    } else {
        put(out, out_size, used, truncated, (char)(0xf0u | (cp >> 18)));
        put(out, out_size, used, truncated, (char)(0x80u | ((cp >> 12) & 0x3fu)));
        put(out, out_size, used, truncated, (char)(0x80u | ((cp >> 6) & 0x3fu)));
        put(out, out_size, used, truncated, (char)(0x80u | (cp & 0x3fu)));
    }
}

/* Four hex digits at `j->pos`, or false.  Never reads past len. */
static bool read_hex4(gs_json *j, uint32_t *out)
{
    uint32_t v = 0u;
    int i;

    if (j->pos + 4u > j->len) {
        return false;
    }
    for (i = 0; i < 4; ++i) {
        uint8_t c = j->buf[j->pos + (size_t)i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A') + 10u;
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    j->pos += 4u;
    *out = v;
    return true;
}

bool gs_json_read_string(gs_json *j, char *out, size_t out_size, bool *truncated)
{
    size_t used = 0u;

    gs_json_skip_ws(j);
    if (j->pos >= j->len || j->buf[j->pos] != (uint8_t)'"') {
        return false;
    }
    j->pos++;
    if (out != NULL && out_size > 0u) {
        out[0] = '\0';
    }

    while (j->pos < j->len) {
        uint8_t c = j->buf[j->pos++];

        if (c == '"') {
            if (out != NULL && out_size > 0u) {
                out[used] = '\0';
            }
            return true;
        }
        if (c != '\\') {
            /* ⚠ Bytes above ASCII pass through unexamined: design §5.4 does no
             * Unicode validation, and a DeviceID is whatever its author typed. */
            put(out, out_size, &used, truncated, (char)c);
            continue;
        }
        if (j->pos >= j->len) {
            return false;
        }
        c = j->buf[j->pos++];
        if (c == '"' || c == '\\' || c == '/') {
            put(out, out_size, &used, truncated, (char)c);
        } else if (c == 'b') {
            put(out, out_size, &used, truncated, '\b');
        } else if (c == 'f') {
            put(out, out_size, &used, truncated, '\f');
        } else if (c == 'n') {
            put(out, out_size, &used, truncated, '\n');
        } else if (c == 'r') {
            put(out, out_size, &used, truncated, '\r');
        } else if (c == 't') {
            put(out, out_size, &used, truncated, '\t');
        } else if (c == 'u') {
            uint32_t cp = 0u;
            if (!read_hex4(j, &cp)) {
                return false;
            }
            if (cp >= 0xd800u && cp <= 0xdbffu && j->pos + 6u <= j->len &&
                j->buf[j->pos] == (uint8_t)'\\' && j->buf[j->pos + 1u] == (uint8_t)'u') {
                size_t save = j->pos;
                uint32_t low = 0u;
                j->pos += 2u;
                if (read_hex4(j, &low) && low >= 0xdc00u && low <= 0xdfffu) {
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
                } else {
                    j->pos = save;
                }
            }
            put_utf8(out, out_size, &used, truncated, cp);
        } else {
            return false;
        }
    }
    return false; /* unterminated */
}

bool gs_json_read_number(gs_json *j, double *out)
{
    char tmp[GS_JSON_NUMBER_MAX];
    size_t n = 0u;
    char *endp = NULL;
    double v;

    gs_json_skip_ws(j);
    while (j->pos < j->len) {
        uint8_t c = j->buf[j->pos];
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' ||
              c == 'E')) {
            break;
        }
        if (n + 1u >= sizeof(tmp)) {
            return false; /* nothing legitimate is this long */
        }
        tmp[n++] = (char)c;
        j->pos++;
    }
    if (n == 0u) {
        return false;
    }
    tmp[n] = '\0';
    /* ⚠ strtod on a BOUNDED COPY (design §5.4): the source buffer is not
     * NUL-terminated and strtod would read past it. */
    v = strtod(tmp, &endp);
    if (endp != tmp + n) {
        return false;
    }
    *out = v;
    return true;
}

bool gs_json_read_literal(gs_json *j, gs_json_type type, int *value)
{
    const char *word;
    size_t n;

    if (type == GS_JSON_TRUE) {
        word = "true";
        *value = 1;
    } else if (type == GS_JSON_FALSE) {
        word = "false";
        *value = 0;
    } else if (type == GS_JSON_NULL) {
        word = "null";
        *value = 0;
    } else {
        return false;
    }
    n = strlen(word);
    gs_json_skip_ws(j);
    if (j->pos + n > j->len || memcmp(j->buf + j->pos, word, n) != 0) {
        return false;
    }
    j->pos += n;
    return true;
}

/*
 * A nested object or array, consumed whole.  ⚠ Whole is the point: a reader
 * that resumed inside one would read its members as members of the enclosing
 * object (CT-D08b).  String-aware, so a brace in a value cannot unbalance it.
 */
static bool skip_structure(gs_json *j)
{
    unsigned depth = 0u;
    bool in_string = false;
    bool escape = false;

    while (j->pos < j->len) {
        uint8_t c = j->buf[j->pos++];

        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            depth++;
            if (depth > GS_JSON_DEPTH_MAX) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (depth == 0u) {
                return false;
            }
            depth--;
            if (depth == 0u) {
                return true;
            }
        }
    }
    return false;
}

bool gs_json_skip_value(gs_json *j)
{
    gs_json_type t = gs_json_peek(j);
    double d = 0.0;
    int v = 0;

    if (t == GS_JSON_OBJECT || t == GS_JSON_ARRAY) {
        return skip_structure(j);
    }
    if (t == GS_JSON_STRING) {
        return gs_json_read_string(j, NULL, 0u, NULL);
    }
    if (t == GS_JSON_NUMBER) {
        return gs_json_read_number(j, &d);
    }
    if (t == GS_JSON_TRUE || t == GS_JSON_FALSE || t == GS_JSON_NULL) {
        return gs_json_read_literal(j, t, &v);
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Key tables                                                                */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ MATCHED EXACTLY FIRST, THEN IGNORING CASE (protocol §9.1).  The two-pass
 * order is what makes GSP_MSGF_KEY_CASE_MISMATCH mean something: an exact hit
 * on any key wins over a case-insensitive hit on another, so the flag says
 * "this client spells a key differently from the vendor's page" — cheap
 * evidence about which connector is on the other end.
 */
typedef struct gs_key {
    const char *name;
    int         id;
} gs_key;

/* A double member of a section, by presence bit and offset. */
typedef struct gs_num_key {
    const char *name;
    uint32_t    bit;
    size_t      offset;
} gs_num_key;

/* A uint8_t flag member of ShotDataOptions. */
typedef struct gs_bool_key {
    const char *name;
    uint8_t     bit;
    size_t      offset;
} gs_bool_key;

static const gs_num_key BALL_KEYS[] = {
    { GSP_KEY_BALL_SPEED,      (uint32_t)GSP_BALL_SPEED,      offsetof(gsp_ball_data, speed) },
    { GSP_KEY_BALL_SPIN_AXIS,  (uint32_t)GSP_BALL_SPIN_AXIS,  offsetof(gsp_ball_data, spin_axis) },
    { GSP_KEY_BALL_TOTAL_SPIN, (uint32_t)GSP_BALL_TOTAL_SPIN, offsetof(gsp_ball_data, total_spin) },
    /* ⚠ [MLM] sends this key as "Backspin"; the second pass catches it. */
    { GSP_KEY_BALL_BACK_SPIN,  (uint32_t)GSP_BALL_BACK_SPIN,  offsetof(gsp_ball_data, back_spin) },
    { GSP_KEY_BALL_SIDE_SPIN,  (uint32_t)GSP_BALL_SIDE_SPIN,  offsetof(gsp_ball_data, side_spin) },
    { GSP_KEY_BALL_HLA,        (uint32_t)GSP_BALL_HLA,        offsetof(gsp_ball_data, hla) },
    { GSP_KEY_BALL_VLA,        (uint32_t)GSP_BALL_VLA,        offsetof(gsp_ball_data, vla) },
    { GSP_KEY_BALL_CARRY,      (uint32_t)GSP_BALL_CARRY_DISTANCE,
      offsetof(gsp_ball_data, carry_distance) }
};

static const gs_num_key CLUB_KEYS[] = {
    { GSP_KEY_CLUB_SPEED, (uint32_t)GSP_CLUB_SPEED, offsetof(gsp_club_data, speed) },
    { GSP_KEY_CLUB_AOA, (uint32_t)GSP_CLUB_ANGLE_OF_ATTACK,
      offsetof(gsp_club_data, angle_of_attack) },
    { GSP_KEY_CLUB_FACE_TO_TARGET, (uint32_t)GSP_CLUB_FACE_TO_TARGET,
      offsetof(gsp_club_data, face_to_target) },
    { GSP_KEY_CLUB_LIE, (uint32_t)GSP_CLUB_LIE, offsetof(gsp_club_data, lie) },
    { GSP_KEY_CLUB_LOFT, (uint32_t)GSP_CLUB_LOFT, offsetof(gsp_club_data, loft) },
    { GSP_KEY_CLUB_PATH, (uint32_t)GSP_CLUB_PATH, offsetof(gsp_club_data, path) },
    { GSP_KEY_CLUB_SPEED_AT_IMPACT, (uint32_t)GSP_CLUB_SPEED_AT_IMPACT,
      offsetof(gsp_club_data, speed_at_impact) },
    { GSP_KEY_CLUB_VFI, (uint32_t)GSP_CLUB_VERTICAL_FACE_IMPACT,
      offsetof(gsp_club_data, vertical_face_impact) },
    /* ⚠ [MLM] puts FACE-TO-PATH here — a quantity that is not what the key
     * names (protocol §3.3).  Passed through unchanged; the host decides. */
    { GSP_KEY_CLUB_HFI, (uint32_t)GSP_CLUB_HORIZONTAL_FACE_IMPACT,
      offsetof(gsp_club_data, horizontal_face_impact) },
    { GSP_KEY_CLUB_CLOSURE_RATE, (uint32_t)GSP_CLUB_CLOSURE_RATE,
      offsetof(gsp_club_data, closure_rate) }
};

static const gs_bool_key OPTION_KEYS[] = {
    { GSP_KEY_OPT_CONTAINS_BALL, (uint8_t)GSP_OPT_CONTAINS_BALL_DATA,
      offsetof(gsp_shot_options, contains_ball_data) },
    { GSP_KEY_OPT_CONTAINS_CLUB, (uint8_t)GSP_OPT_CONTAINS_CLUB_DATA,
      offsetof(gsp_shot_options, contains_club_data) },
    { GSP_KEY_OPT_LM_READY, (uint8_t)GSP_OPT_LAUNCH_MONITOR_IS_READY,
      offsetof(gsp_shot_options, launch_monitor_is_ready) },
    { GSP_KEY_OPT_BALL_DETECTED, (uint8_t)GSP_OPT_LAUNCH_MONITOR_BALL_DETECTED,
      offsetof(gsp_shot_options, launch_monitor_ball_detected) },
    { GSP_KEY_OPT_HEARTBEAT, (uint8_t)GSP_OPT_IS_HEARTBEAT,
      offsetof(gsp_shot_options, is_heartbeat) }
};

enum {
    ROOT_DEVICE_ID = 1,
    ROOT_UNITS,
    ROOT_SHOT_NUMBER,
    ROOT_API_VERSION,
    ROOT_BALL_DATA,
    ROOT_CLUB_DATA,
    ROOT_OPTIONS
};

static const gs_key ROOT_KEYS[] = {
    { GSP_KEY_DEVICE_ID, ROOT_DEVICE_ID },
    { GSP_KEY_UNITS, ROOT_UNITS },
    { GSP_KEY_SHOT_NUMBER, ROOT_SHOT_NUMBER },
    /* ⚠ Spelled with a lower-case v on [GSP]; [R10] sends "APIVersion" and
     * [SB] "Apiversion", and both reach here through the second pass. */
    { GSP_KEY_API_VERSION, ROOT_API_VERSION },
    { GSP_KEY_BALL_DATA, ROOT_BALL_DATA },
    { GSP_KEY_CLUB_DATA, ROOT_CLUB_DATA },
    { GSP_KEY_SHOT_DATA_OPTIONS, ROOT_OPTIONS }
};

enum { RESP_CODE = 1, RESP_MESSAGE, RESP_PLAYER };

static const gs_key RESPONSE_KEYS[] = {
    { GSP_KEY_RESP_CODE, RESP_CODE },
    { GSP_KEY_RESP_MESSAGE, RESP_MESSAGE },
    { GSP_KEY_RESP_PLAYER, RESP_PLAYER }
};

enum { PLAYER_HANDED = 1, PLAYER_CLUB, PLAYER_DISTANCE, PLAYER_SURFACE };

static const gs_key PLAYER_KEYS[] = {
    { GSP_KEY_PLAYER_HANDED, PLAYER_HANDED },
    { GSP_KEY_PLAYER_CLUB, PLAYER_CLUB },
    { GSP_KEY_PLAYER_DISTANCE, PLAYER_DISTANCE },
    { GSP_KEY_PLAYER_SURFACE, PLAYER_SURFACE }
};

/* 0 for a key nothing here recognises.  *mismatch is set when only the
 * case-insensitive pass matched. */
static int lookup(const gs_key *tab, size_t n, const char *key, bool *mismatch)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (strcmp(tab[i].name, key) == 0) {
            return tab[i].id;
        }
    }
    for (i = 0; i < n; ++i) {
        if (gs_ci_equal(tab[i].name, key)) {
            *mismatch = true;
            return tab[i].id;
        }
    }
    return 0;
}

static const gs_num_key *lookup_num(const gs_num_key *tab, size_t n, const char *key,
                                    bool *mismatch)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (strcmp(tab[i].name, key) == 0) {
            return &tab[i];
        }
    }
    for (i = 0; i < n; ++i) {
        if (gs_ci_equal(tab[i].name, key)) {
            *mismatch = true;
            return &tab[i];
        }
    }
    return NULL;
}

static const gs_bool_key *lookup_bool(const gs_bool_key *tab, size_t n, const char *key,
                                      bool *mismatch)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (strcmp(tab[i].name, key) == 0) {
            return &tab[i];
        }
    }
    for (i = 0; i < n; ++i) {
        if (gs_ci_equal(tab[i].name, key)) {
            *mismatch = true;
            return &tab[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Member walking                                                            */
/* ------------------------------------------------------------------------ */

/* 1: `key` holds the next member's name and the ':' is consumed.
 * 0: the object ended.  -1: malformed. */
static int next_member(gs_json *j, bool *first, char *key, size_t key_size)
{
    if (gs_json_accept(j, '}')) {
        return 0;
    }
    if (!*first && !gs_json_accept(j, ',')) {
        return -1;
    }
    *first = false;
    /* ⚠ A key longer than GS_KEY_MAX truncates, and a truncated key is
     * GS_KEY_MAX-1 characters long — longer than every key this reader knows,
     * so it can never collide with one. */
    if (!gs_json_read_string(j, key, key_size, NULL)) {
        return -1;
    }
    if (!gs_json_accept(j, ':')) {
        return -1;
    }
    return 1;
}

static void count_unknown(uint16_t *unknown)
{
    if (*unknown < 0xffffu) {
        (*unknown)++;
    }
}

/* 1: read.  0: absent (null, or a type this member cannot be).  -1: malformed. */
static int read_number_member(gs_json *j, double *out, uint32_t *flags)
{
    gs_json_type t = gs_json_peek(j);
    int v = 0;

    if (t == GS_JSON_NUMBER) {
        return gs_json_read_number(j, out) ? 1 : -1;
    }
    if (t == GS_JSON_STRING) {
        /* ⚠ [SB] sends every number as a JSON string.  Cheap to accept, and
         * worth knowing about: GSP_MSGF_TYPE_COERCED (design §4.5). */
        char tmp[GS_JSON_NUMBER_MAX];
        char *endp = NULL;
        double d;
        if (!gs_json_read_string(j, tmp, sizeof(tmp), NULL)) {
            return -1;
        }
        if (tmp[0] == '\0') {
            return 0;
        }
        d = strtod(tmp, &endp);
        if (endp == tmp || *endp != '\0') {
            return 0;
        }
        *out = d;
        *flags |= (uint32_t)GSP_MSGF_TYPE_COERCED;
        return 1;
    }
    if (t == GS_JSON_NULL) {
        /* ⚠ [R10] and [OSP] write nulls where a value is absent. */
        return gs_json_read_literal(j, t, &v) ? 0 : -1;
    }
    return gs_json_skip_value(j) ? 0 : -1;
}

static int read_bool_member(gs_json *j, uint8_t *out, uint32_t *flags)
{
    gs_json_type t = gs_json_peek(j);
    int v = 0;

    if (t == GS_JSON_TRUE || t == GS_JSON_FALSE) {
        if (!gs_json_read_literal(j, t, &v)) {
            return -1;
        }
        *out = (uint8_t)v;
        return 1;
    }
    if (t == GS_JSON_STRING) {
        char tmp[16];
        if (!gs_json_read_string(j, tmp, sizeof(tmp), NULL)) {
            return -1;
        }
        if (gs_ci_equal(tmp, "true")) {
            *out = 1u;
        } else if (gs_ci_equal(tmp, "false")) {
            *out = 0u;
        } else {
            return 0;
        }
        *flags |= (uint32_t)GSP_MSGF_TYPE_COERCED;
        return 1;
    }
    if (t == GS_JSON_NULL) {
        /* ⚠ [R10] writes "IsHeartBeat": null.  A null boolean is absence, not
         * false — the presence bit stays clear (CT-D07b). */
        return gs_json_read_literal(j, t, &v) ? 0 : -1;
    }
    if (t == GS_JSON_NUMBER) {
        double d = 0.0;
        if (!gs_json_read_number(j, &d)) {
            return -1;
        }
        *out = (d != 0.0) ? 1u : 0u;
        *flags |= (uint32_t)GSP_MSGF_TYPE_COERCED;
        return 1;
    }
    return gs_json_skip_value(j) ? 0 : -1;
}

static int read_string_member(gs_json *j, char *out, size_t out_size, uint32_t *flags)
{
    gs_json_type t = gs_json_peek(j);
    bool truncated = false;
    int v = 0;

    if (t == GS_JSON_STRING) {
        if (!gs_json_read_string(j, out, out_size, &truncated)) {
            return -1;
        }
        if (truncated) {
            /* ⚠ Never silent (design §5.4); the value is the truncation. */
            *flags |= (uint32_t)GSP_MSGF_STRING_TRUNCATED;
        }
        return 1;
    }
    if (t == GS_JSON_NULL) {
        return gs_json_read_literal(j, t, &v) ? 0 : -1;
    }
    return gs_json_skip_value(j) ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Sections                                                                  */
/* ------------------------------------------------------------------------ */

static bool decode_numeric_section(gs_json *j, void *section, uint32_t *present,
                                   const gs_num_key *tab, size_t n, uint32_t *flags,
                                   uint16_t *unknown)
{
    char key[GS_KEY_MAX];
    bool first = true;

    if (!gs_json_accept(j, '{')) {
        return false;
    }
    for (;;) {
        const gs_num_key *k;
        bool mismatch = false;
        double v = 0.0;
        int got;
        int r = next_member(j, &first, key, sizeof(key));

        if (r == 0) {
            return true;
        }
        if (r < 0) {
            return false;
        }
        k = lookup_num(tab, n, key, &mismatch);
        if (k == NULL) {
            count_unknown(unknown);
            if (!gs_json_skip_value(j)) {
                return false;
            }
            continue;
        }
        if (mismatch) {
            *flags |= (uint32_t)GSP_MSGF_KEY_CASE_MISMATCH;
        }
        got = read_number_member(j, &v, flags);
        if (got < 0) {
            return false;
        }
        if (got == 1) {
            /* memcpy rather than a cast: the section is not necessarily
             * double-aligned as far as the compiler is concerned. */
            memcpy((char *)section + k->offset, &v, sizeof(v));
            *present |= k->bit;
        }
    }
}

static bool decode_options(gs_json *j, gsp_shot_options *opt, uint32_t *flags,
                           uint16_t *unknown)
{
    char key[GS_KEY_MAX];
    bool first = true;

    if (!gs_json_accept(j, '{')) {
        return false;
    }
    for (;;) {
        const gs_bool_key *k;
        bool mismatch = false;
        uint8_t v = 0u;
        int got;
        int r = next_member(j, &first, key, sizeof(key));

        if (r == 0) {
            return true;
        }
        if (r < 0) {
            return false;
        }
        k = lookup_bool(OPTION_KEYS, sizeof(OPTION_KEYS) / sizeof(OPTION_KEYS[0]), key,
                        &mismatch);
        if (k == NULL) {
            count_unknown(unknown);
            if (!gs_json_skip_value(j)) {
                return false;
            }
            continue;
        }
        if (mismatch) {
            *flags |= (uint32_t)GSP_MSGF_KEY_CASE_MISMATCH;
        }
        got = read_bool_member(j, &v, flags);
        if (got < 0) {
            return false;
        }
        if (got == 1) {
            *((uint8_t *)opt + k->offset) = v;
            opt->present |= k->bit;
        }
    }
}

/* ⚠ A double straight to int64_t is undefined when it does not fit, and every
 * byte on this socket is untrusted: `{"ShotNumber":1e999}` must not be UB. */
static int64_t clamp_int64(double d)
{
    if (d != d) {
        return 0;
    }
    if (d <= -9223372036854775808.0) {
        return INT64_MIN;
    }
    if (d >= 9223372036854775808.0) {
        return INT64_MAX;
    }
    return (int64_t)d;
}

/* ------------------------------------------------------------------------ */
/* gsp_message_decode                                                        */
/* ------------------------------------------------------------------------ */

gsp_status gsp_message_decode(const uint8_t *json, size_t len, gsp_message *out)
{
    gs_json j;
    char key[GS_KEY_MAX];
    bool first = true;
    bool have_device_id = false;
    bool have_shot_number = false;
    bool have_api_version = false;
    bool have_units = false;
    bool have_ball_object = false;
    bool have_club_object = false;
    bool have_options = false;
    uint32_t flags = 0u;
    uint16_t unknown = 0u;

    if (json == NULL || out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->conn = GSP_CONN_NONE;
    out->host_recv_us = GSP_TIME_UNKNOWN;
    out->wire_length = (uint16_t)((len > 0xffffu) ? 0xffffu : len);

    j.buf = json;
    j.len = len;
    j.pos = 0u;

    if (!gs_json_accept(&j, '{')) {
        return GSP_ERR_MALFORMED;
    }

    for (;;) {
        bool mismatch = false;
        int id;
        int r = next_member(&j, &first, key, sizeof(key));

        if (r == 0) {
            break;
        }
        if (r < 0) {
            return GSP_ERR_MALFORMED;
        }
        id = lookup(ROOT_KEYS, sizeof(ROOT_KEYS) / sizeof(ROOT_KEYS[0]), key, &mismatch);
        if (id == 0) {
            /* ⚠ Skipped WHOLE, nested value and all, and counted — [OB] adds a
             * top-level "ClubName" (protocol §9.7, CT-D08 / CT-D08b). */
            count_unknown(&unknown);
            if (!gs_json_skip_value(&j)) {
                return GSP_ERR_MALFORMED;
            }
            continue;
        }
        if (mismatch) {
            flags |= (uint32_t)GSP_MSGF_KEY_CASE_MISMATCH;
        }

        if (id == ROOT_DEVICE_ID) {
            int got = read_string_member(&j, out->device_id, sizeof(out->device_id), &flags);
            if (got < 0) {
                return GSP_ERR_MALFORMED;
            }
            have_device_id = have_device_id || (got == 1);
        } else if (id == ROOT_UNITS) {
            int got = read_string_member(&j, out->units_text, sizeof(out->units_text), &flags);
            if (got < 0) {
                return GSP_ERR_MALFORMED;
            }
            have_units = have_units || (got == 1);
        } else if (id == ROOT_API_VERSION) {
            int got = read_string_member(&j, out->api_version, sizeof(out->api_version), &flags);
            if (got < 0) {
                return GSP_ERR_MALFORMED;
            }
            have_api_version = have_api_version || (got == 1);
        } else if (id == ROOT_SHOT_NUMBER) {
            double v = 0.0;
            int got = read_number_member(&j, &v, &flags);
            if (got < 0) {
                return GSP_ERR_MALFORMED;
            }
            if (got == 1) {
                have_shot_number = true;
                out->shot_number = clamp_int64(v);
                /* ⚠ 13.0 IS an integer: [OSG] serialises its counter as a
                 * float and must not be flagged for it (CT-D05).  13.5 is
                 * flagged and shot_number holds the truncation (CT-D06). */
                if (v != trunc(v)) {
                    flags |= (uint32_t)GSP_MSGF_SHOT_NUMBER_NOT_INTEGER;
                }
            }
        } else if (id == ROOT_BALL_DATA) {
            gs_json_type t = gs_json_peek(&j);
            if (t == GS_JSON_OBJECT) {
                if (!decode_numeric_section(&j, &out->ball, &out->ball.present, BALL_KEYS,
                                            sizeof(BALL_KEYS) / sizeof(BALL_KEYS[0]), &flags,
                                            &unknown)) {
                    return GSP_ERR_MALFORMED;
                }
                have_ball_object = true;
            } else if (!gs_json_skip_value(&j)) {
                return GSP_ERR_MALFORMED;
            }
        } else if (id == ROOT_CLUB_DATA) {
            gs_json_type t = gs_json_peek(&j);
            if (t == GS_JSON_OBJECT) {
                if (!decode_numeric_section(&j, &out->club, &out->club.present, CLUB_KEYS,
                                            sizeof(CLUB_KEYS) / sizeof(CLUB_KEYS[0]), &flags,
                                            &unknown)) {
                    return GSP_ERR_MALFORMED;
                }
                have_club_object = true;
            } else if (!gs_json_skip_value(&j)) {
                /* ⚠ [OB] sends "ClubData": null.  A null object is absence,
                 * not a malformed message (CT-D07). */
                return GSP_ERR_MALFORMED;
            }
        } else { /* ROOT_OPTIONS */
            gs_json_type t = gs_json_peek(&j);
            if (t == GS_JSON_OBJECT) {
                if (!decode_options(&j, &out->options, &flags, &unknown)) {
                    return GSP_ERR_MALFORMED;
                }
                have_options = true;
            } else if (!gs_json_skip_value(&j)) {
                return GSP_ERR_MALFORMED;
            }
        }
    }

    /* ⚠ Trailing bytes after the object mean the caller handed over something
     * other than one message, and the truncation sweep (CT-D24) asserts that
     * no partial buffer ever decodes as success.  Whitespace only. */
    if (!gs_json_at_end(&j)) {
        return GSP_ERR_MALFORMED;
    }

    /* ------------------------------------------------------------------ */
    /* Classification and the flag pass (design §4.5, protocol §3.4)       */
    /* ------------------------------------------------------------------ */
    if (have_units) {
        out->units = (uint8_t)gsp_units_parse(out->units_text);
        if (out->units == (uint8_t)GSP_UNITS_UNKNOWN) {
            flags |= (uint32_t)GSP_MSGF_UNKNOWN_UNITS;
        }
    }

    if (!have_device_id) {
        flags |= (uint32_t)GSP_MSGF_MISSING_DEVICE_ID;
    }
    if (!have_shot_number) {
        flags |= (uint32_t)GSP_MSGF_MISSING_SHOT_NUMBER;
    }
    if (!have_api_version) {
        flags |= (uint32_t)GSP_MSGF_MISSING_API_VERSION;
    } else if (strcmp(out->api_version, GSP_API_VERSION_TEXT) != 0) {
        flags |= (uint32_t)GSP_MSGF_UNKNOWN_API_VERSION;
    }
    if (!have_options) {
        flags |= (uint32_t)GSP_MSGF_MISSING_OPTIONS;
    }

    if (out->options.contains_ball_data || out->options.contains_club_data) {
        out->kind = (uint8_t)GSP_MSG_SHOT;
    } else if (out->options.is_heartbeat) {
        out->kind = (uint8_t)GSP_MSG_HEARTBEAT;
    } else {
        out->kind = (uint8_t)GSP_MSG_STATUS;
    }

    /* ⚠ Gate on the FLAG, then check the object, and report the mismatch
     * (protocol §3.4). */
    if (out->options.contains_ball_data && !have_ball_object) {
        flags |= (uint32_t)GSP_MSGF_BALL_FLAG_WITHOUT_OBJECT;
    }
    if (have_ball_object && !out->options.contains_ball_data) {
        /* ⚠ Always a finding: a BallData object is the strike itself, and a
         * server that gated on the flag alone would discard it.  [PIT] and
         * [OF] both ride a full one on a heartbeat (CT-K03b). */
        flags |= (uint32_t)GSP_MSGF_BALL_OBJECT_WITHOUT_FLAG;
    }
    if (out->options.contains_club_data && !have_club_object) {
        flags |= (uint32_t)GSP_MSGF_CLUB_FLAG_WITHOUT_OBJECT;
    }
    if (have_club_object && !out->options.contains_club_data &&
        out->kind != (uint8_t)GSP_MSG_SHOT) {
        /* ⚠ NOT a finding on a shot, and that asymmetry is deliberate:
         * [GSP]'s own example, [GC2] and [PIT] all pad every shot with a
         * ten-member all-zero ClubData beside "ContainsClubData": false
         * (protocol §3.3), so on a shot it is the documented shape and
         * flagging it would flag the vendor's own example (CT-D01).  On a
         * heartbeat or a status message it is worth recording. */
        flags |= (uint32_t)GSP_MSGF_CLUB_OBJECT_WITHOUT_FLAG;
    }

    /* ⚠ Only a shot that CLAIMED ball data can be missing any: a club-only
     * shot never promised a ball (CT-K02). */
    if (out->kind == (uint8_t)GSP_MSG_SHOT && out->options.contains_ball_data &&
        !gsp_message_ball_complete(&out->ball)) {
        flags |= (uint32_t)GSP_MSGF_BALL_INCOMPLETE;
    }

    out->unknown_keys = unknown;
    if (unknown > 0u) {
        flags |= (uint32_t)GSP_MSGF_UNKNOWN_KEYS;
    }
    out->flags = flags;
    return GSP_OK;
}

/* ------------------------------------------------------------------------ */
/* gsp_response_decode — for a test client, and for CT-R06                   */
/* ------------------------------------------------------------------------ */

static bool decode_player(gs_json *j, gsp_player_info *p, uint8_t *has_player)
{
    char key[GS_KEY_MAX];
    bool first = true;
    uint32_t ignored = 0u;

    if (!gs_json_accept(j, '{')) {
        return false;
    }
    *has_player = 1u;
    for (;;) {
        bool mismatch = false;
        int id;
        int r = next_member(j, &first, key, sizeof(key));

        if (r == 0) {
            return true;
        }
        if (r < 0) {
            return false;
        }
        id = lookup(PLAYER_KEYS, sizeof(PLAYER_KEYS) / sizeof(PLAYER_KEYS[0]), key, &mismatch);
        if (id == PLAYER_HANDED || id == PLAYER_CLUB) {
            char text[16];
            int got = read_string_member(j, text, sizeof(text), &ignored);
            if (got < 0) {
                return false;
            }
            if (got == 1) {
                if (id == PLAYER_HANDED) {
                    p->handed = (uint8_t)gsp_handed_parse(text);
                } else {
                    p->club = (uint8_t)gsp_club_parse(text);
                }
            }
        } else if (id == PLAYER_DISTANCE) {
            double v = 0.0;
            int got = read_number_member(j, &v, &ignored);
            if (got < 0) {
                return false;
            }
            if (got == 1) {
                p->distance_to_target = v;
                p->has_distance = 1u;
            }
        } else if (id == PLAYER_SURFACE) {
            if (read_string_member(j, p->surface, sizeof(p->surface), &ignored) < 0) {
                return false;
            }
        } else if (!gs_json_skip_value(j)) {
            return false;
        }
    }
}

gsp_status gsp_response_decode(const uint8_t *json, size_t len, gsp_response *out)
{
    gs_json j;
    char key[GS_KEY_MAX];
    bool first = true;
    uint32_t ignored = 0u;

    if (json == NULL || out == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    j.buf = json;
    j.len = len;
    j.pos = 0u;

    if (!gs_json_accept(&j, '{')) {
        return GSP_ERR_MALFORMED;
    }
    for (;;) {
        bool mismatch = false;
        int id;
        int r = next_member(&j, &first, key, sizeof(key));

        if (r == 0) {
            break;
        }
        if (r < 0) {
            return GSP_ERR_MALFORMED;
        }
        id = lookup(RESPONSE_KEYS, sizeof(RESPONSE_KEYS) / sizeof(RESPONSE_KEYS[0]), key,
                    &mismatch);
        if (id == RESP_CODE) {
            double v = 0.0;
            int got = read_number_member(&j, &v, &ignored);
            if (got < 0) {
                return GSP_ERR_MALFORMED;
            }
            if (got == 1) {
                int64_t c = clamp_int64(v);
                if (c > INT32_MAX) {
                    c = INT32_MAX;
                }
                if (c < INT32_MIN) {
                    c = INT32_MIN;
                }
                out->code = (int32_t)c;
            }
        } else if (id == RESP_MESSAGE) {
            int got = read_string_member(&j, out->message, sizeof(out->message), &ignored);
            if (got < 0) {
                return GSP_ERR_MALFORMED;
            }
            /* ⚠ [OSG] replies {"Code":200} with no Message at all and every
             * client accepts it, so absence is normal and must be visible. */
            if (got == 1) {
                out->has_message = 1u;
            }
        } else if (id == RESP_PLAYER) {
            if (gs_json_peek(&j) == GS_JSON_OBJECT) {
                if (!decode_player(&j, &out->player, &out->has_player)) {
                    return GSP_ERR_MALFORMED;
                }
            } else if (!gs_json_skip_value(&j)) {
                return GSP_ERR_MALFORMED;
            }
        } else if (!gs_json_skip_value(&j)) {
            return GSP_ERR_MALFORMED;
        }
    }
    if (!gs_json_at_end(&j)) {
        return GSP_ERR_MALFORMED;
    }
    return GSP_OK;
}
