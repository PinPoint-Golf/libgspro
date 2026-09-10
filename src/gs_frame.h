/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_frame.h — the incremental framer, internal to the library.
 *
 * design §5.3.  There is no framing on this wire: JSON objects arrive back to
 * back, split or merged by TCP as it likes, sometimes indented across lines.
 * The only delimiter is the JSON itself, so the framer counts brace depth
 * OUTSIDE string literals and treats whitespace between objects as nothing.
 *
 * ⚠ STRING-AWARE, NOT A BARE BRACE COUNT.  A DeviceID is free text chosen by a
 * client author, so `"PiTrac {v2}"` is a legal value; [KJD] and [SLX] both
 * count braces without regard for strings and would wedge on it forever with
 * no error anywhere.  Every byte goes through gs_framer_feed().
 *
 * The state is one struct so that the server can keep one per connection and
 * feed bytes as they arrive without ever rescanning its buffer, while the
 * public gsp_frame_find() (codec.h) drives the same machine from a clean state
 * over a whole buffer.  One implementation, so the stateless and incremental
 * halves cannot disagree about what a message is.
 */
#ifndef GS_FRAME_H
#define GS_FRAME_H

#include <stdbool.h>
#include <stdint.h>

/* What one byte did.  ⚠ The caller appends the byte to its message buffer for
 * OPEN, INSIDE and CLOSE, and for nothing else. */
typedef enum gs_frame_step {
    GS_FRAME_SKIP = 0,  /* whitespace between objects: consumed, ignored       */
    GS_FRAME_OPEN,      /* '{' at top level: this byte begins a message        */
    GS_FRAME_INSIDE,    /* consumed; the message is still open                 */
    GS_FRAME_CLOSE,     /* '}' closing depth 1: the message ends AT this byte  */
    GS_FRAME_GARBAGE    /* not whitespace, not '{', and no message is open     */
} gs_frame_step;

typedef struct gs_framer {
    uint32_t depth;      /* brace depth; 0 means "between messages"            */
    bool     in_string;  /* inside a "…" literal                               */
    bool     escape;     /* the previous byte was a backslash inside a string  */
} gs_framer;

/* Between messages, with nothing buffered. */
static inline void gs_framer_reset(gs_framer *f)
{
    f->depth = 0u;
    f->in_string = false;
    f->escape = false;
}

static inline bool gs_framer_in_message(const gs_framer *f)
{
    return f->depth > 0u;
}

/*
 * One byte.  ⚠ GS_FRAME_GARBAGE leaves the framer untouched so the caller can
 * report the offending byte and carry on scanning at the next one; every other
 * result has already been folded into the state.
 */
static inline gs_frame_step gs_framer_feed(gs_framer *f, uint8_t b)
{
    if (f->depth == 0u) {
        if (b == ' ' || b == '\t' || b == '\n' || b == '\r') {
            return GS_FRAME_SKIP;
        }
        if (b != '{') {
            /* ⚠ Including '[': the protocol has no arrays at top level, and
             * treating one as a message would be inventing a protocol. */
            return GS_FRAME_GARBAGE;
        }
        f->depth = 1u;
        return GS_FRAME_OPEN;
    }

    if (f->in_string) {
        if (f->escape) {
            f->escape = false;
        } else if (b == '\\') {
            f->escape = true;
        } else if (b == '"') {
            f->in_string = false;
        }
        return GS_FRAME_INSIDE;
    }

    if (b == '"') {
        f->in_string = true;
    } else if (b == '{') {
        f->depth++;
    } else if (b == '}') {
        f->depth--;
        if (f->depth == 0u) {
            return GS_FRAME_CLOSE;
        }
    }
    return GS_FRAME_INSIDE;
}

#endif /* GS_FRAME_H */
