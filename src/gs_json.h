/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_json.h — the bounded JSON reader, internal to the library.
 *
 * design §5.4.  This is a purpose-built reader, not a general JSON library,
 * and deliberately so: it knows the six object names and their members, it
 * skips anything it does not recognise, and it has no depth beyond what those
 * objects need.  It builds no tree and allocates nothing — every value is
 * copied straight into the caller's bounded field.
 *
 * ⚠ IT MUST NEVER READ PAST `len`.  The port is open and unauthenticated
 * (design §9.3) and this is the component fuzzing is aimed at (CT-X02,
 * CT-D24).  Every function below takes its bound from the gs_json cursor and
 * every loop tests `pos < len` before dereferencing.
 *
 * Every entry point returns false on malformed input and leaves the cursor
 * somewhere undefined; the caller abandons the whole message at that point
 * (GSP_ERR_MALFORMED) rather than trying to resynchronise inside it.
 */
#ifndef GS_JSON_H
#define GS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* ASCII helpers                                                             */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ NOT <ctype.h>.  tolower() is locale-dependent — in a Turkish locale it
 * maps 'I' to a dotless 'ı' — and the host chooses the locale, not us.  Key
 * matching is a wire rule (protocol §9.1), so it is spelled out in ASCII.
 */
static inline char gs_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive equality of two NUL-terminated ASCII strings. */
static inline bool gs_ci_equal(const char *a, const char *b)
{
    size_t i = 0;
    if (a == NULL || b == NULL) {
        return false;
    }
    for (i = 0; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (gs_ascii_lower(a[i]) != gs_ascii_lower(b[i])) {
            return false;
        }
    }
    return a[i] == b[i];
}

static inline bool gs_is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ------------------------------------------------------------------------ */
/* The cursor                                                                */
/* ------------------------------------------------------------------------ */

typedef struct gs_json {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} gs_json;

/* What the next value is, without consuming it.  GS_JSON_NONE at end of
 * buffer or on a byte that begins no JSON value (a `/` comment, say). */
typedef enum gs_json_type {
    GS_JSON_NONE = 0,
    GS_JSON_OBJECT,
    GS_JSON_ARRAY,
    GS_JSON_STRING,
    GS_JSON_NUMBER,
    GS_JSON_TRUE,
    GS_JSON_FALSE,
    GS_JSON_NULL
} gs_json_type;

/* Longest number token accepted.  [OSG] sends seventeen significant digits;
 * anything approaching this bound is not a measurement. */
#define GS_JSON_NUMBER_MAX 64

void         gs_json_skip_ws(gs_json *j);
gs_json_type gs_json_peek(gs_json *j);
bool         gs_json_at_end(gs_json *j);

/* Consumes `c` if it is next (after whitespace); false if it is not there. */
bool gs_json_accept(gs_json *j, char c);

/*
 * Reads a string value into `out` (always NUL-terminated when out_size > 0),
 * decoding the JSON escapes including \uXXXX and surrogate pairs.  A value
 * longer than the field is TRUNCATED and *truncated is set — never silent
 * (design §5.4).  `out` may be NULL to consume and discard.
 */
bool gs_json_read_string(gs_json *j, char *out, size_t out_size, bool *truncated);

/* Reads a number as a double, via strtod on a bounded copy (design §5.4). */
bool gs_json_read_number(gs_json *j, double *out);

/* Consumes `true`, `false` or `null`; *value is 1, 0, 0 respectively. */
bool gs_json_read_literal(gs_json *j, gs_json_type type, int *value);

/*
 * Consumes one value of any type, whole.  ⚠ Nested objects and arrays are
 * skipped ENTIRELY: a reader that resumed inside one would read its members as
 * members of the enclosing object (conformance CT-D08b).  Bounded depth, so a
 * hostile client cannot recurse the stack away.
 */
bool gs_json_skip_value(gs_json *j);

#endif /* GS_JSON_H */
