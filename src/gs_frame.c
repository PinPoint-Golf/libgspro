/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_frame.c — gsp_frame_find(), the stateless half of the framer.
 *
 * design §5.3, protocol §2.  It drives the same gs_framer the server keeps per
 * connection (gs_frame.h), from a clean state, over a whole buffer — so the
 * stateless and incremental halves cannot disagree about where a message ends.
 */

#include "gspro/codec.h"

#include "gs_frame.h"

gsp_status gsp_frame_find(const uint8_t *buf, size_t len, size_t *start, size_t *end)
{
    gs_framer f;
    size_t i;
    size_t begin = len;

    if (buf == NULL || start == NULL || end == NULL) {
        return GSP_ERR_INVALID_ARG;
    }

    gs_framer_reset(&f);
    *start = len;
    *end = len;

    for (i = 0; i < len; ++i) {
        switch (gs_framer_feed(&f, buf[i])) {
        case GS_FRAME_SKIP:
            break;
        case GS_FRAME_OPEN:
            begin = i;
            break;
        case GS_FRAME_INSIDE:
            break;
        case GS_FRAME_CLOSE:
            *start = begin;
            *end = i + 1u;
            return GSP_OK;
        case GS_FRAME_GARBAGE:
        default:
            /* ⚠ *start names the offending byte so the caller can discard
             * through it and rescan (design §5.3's resynchronisation). */
            *start = i;
            return GSP_ERR_MALFORMED;
        }
    }

    /* Ran out of bytes.  Either an object began and is unfinished, or the whole
     * buffer was whitespace — in which case nothing began and *start is len.
     * ⚠ Whitespace alone is PENDING, never garbage: [MLM]'s splitter allows
     * spaces between objects, so they arrive. */
    *start = begin;
    return GSP_PENDING;
}
