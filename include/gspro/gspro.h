/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro.h — umbrella header for libgspro.
 *
 * A sans-I/O C11 library that plays the GSPro Connect role: it listens for
 * launch monitors speaking GSPro Open Connect v1 and surfaces what they send.
 *
 *   docs/protocol.md   what the protocol is, with the provenance of every claim
 *   docs/design.md     how this library answers it, and why
 *
 * The five facts about this protocol that the API deliberately makes hard to
 * ignore, because smoothing any of them away loses shots with no error
 * anywhere:
 *
 *   1. There is NO FRAMING.  JSON objects arrive back to back, split or merged
 *      by TCP as it likes, sometimes indented across lines.  The library
 *      reassembles from any byte stream and the host must not pre-split.
 *   2. Every message gets EXACTLY ONE reply, promptly, whatever it was — a
 *      heartbeat is "a valid shot message" to GSPro, and a connector that
 *      waits 2 s for a reply re-sends the shot.
 *   3. Zero is not absence.  Some clients omit what they did not measure and
 *      others send 0.0; every field carries a presence bit and no value is
 *      converted, inferred or filled in.
 *   4. Clients misspell keys and still work against GSPro.  Keys match
 *      case-insensitively, and the mismatch is flagged rather than hidden.
 *   5. It should not own the socket.  Qt, asyncio, POSIX and Winsock hosts
 *      all embed the same core.
 */
#ifndef GSPRO_H
#define GSPRO_H

#include "gspro/version.h"
#include "gspro/types.h"
#include "gspro/protocol.h"
#include "gspro/message.h"
#include "gspro/codec.h"
#include "gspro/event.h"
#include "gspro/server.h"

#endif /* GSPRO_H */
