/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_srv.h — the three lines of boilerplate every server-side conformance case
 * would otherwise repeat: make a server, open a connection, feed it bytes,
 * drain what came out.
 *
 * ⚠ THE CLOCK IS SYNTHETIC AND MONOTONIC, and every case that cares passes an
 * explicit `now`.  design §4.1: the library never reads a clock, so a whole
 * session is deterministic under a counter — which is what makes the idle-alarm
 * and write-spacing cases (CT-C10, CT-T03) testable at all rather than timed.
 */
#ifndef GS_SRV_H
#define GS_SRV_H

#include "gspro/gspro.h"
#include "gs_test.h"

#define GS_CONN_A ((gsp_conn_id)1)
#define GS_CONN_B ((gsp_conn_id)2)

/* A monotonic microsecond counter with an arbitrary epoch, as the contract
 * asks for.  Starts away from zero so a case cannot pass by accident on a
 * field that was never set. */
static gsp_time_us gs_now = (gsp_time_us)1000000000;

static inline gsp_time_us gs_advance(gsp_time_us by_us)
{
    gs_now += by_us;
    return gs_now;
}

/* Create with the defaults, or with a config the caller has adjusted.  Returns
 * NULL if the library refuses, which every caller must tolerate: while
 * src/gs_server.c does not exist the scaffold refuses everything, and a case
 * that dereferenced NULL would abort the whole binary and hide the rest. */
static inline gsp_server *gs_srv_create(const gsp_server_config *cfg)
{
    gsp_server_config local;
    gsp_server *s = NULL;
    if (cfg == NULL) {
        local = gsp_server_config_default();
        cfg = &local;
    }
    if (gsp_server_create(cfg, &s) < GSP_OK) {
        return NULL;
    }
    return s;
}

/* Create and open one connection, id GS_CONN_A. */
static inline gsp_server *gs_srv_open(void)
{
    gsp_server *s = gs_srv_create(NULL);
    if (s != NULL) {
        (void)gsp_server_on_connection_opened(s, GS_CONN_A, "203.0.113.7:51022", gs_now);
    }
    return s;
}

static inline void gs_srv_free(gsp_server *s)
{
    if (s != NULL) {
        gsp_server_close(s);
        gsp_server_destroy(s);
    }
}

/* Feed a C string as one read on connection A. */
static inline gsp_status gs_srv_feed(gsp_server *s, const char *json)
{
    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    return gsp_server_on_bytes(s, GS_CONN_A, (const uint8_t *)json, strlen(json), gs_now);
}

static inline gsp_status gs_srv_feed_n(gsp_server *s, const void *data, size_t len)
{
    if (s == NULL) {
        return GSP_ERR_INVALID_ARG;
    }
    return gsp_server_on_bytes(s, GS_CONN_A, (const uint8_t *)data, len, gs_now);
}

/* Drain every pending write; returns how many were collected. */
#define GS_DRAIN_MAX 64

static inline size_t gs_srv_writes(gsp_server *s, gsp_write_request *out, size_t max)
{
    size_t total = 0;
    size_t n;
    if (s == NULL) {
        return 0;
    }
    while (total < max && (n = gsp_server_poll_writes(s, out + total, max - total)) > 0) {
        total += n;
    }
    return total;
}

static inline size_t gs_srv_events(gsp_server *s, gsp_event *out, size_t max)
{
    size_t total = 0;
    size_t n;
    if (s == NULL) {
        return 0;
    }
    while (total < max && (n = gsp_server_poll_events(s, out + total, max - total)) > 0) {
        total += n;
    }
    return total;
}

/* The first event of a type, or NULL. */
static inline const gsp_event *gs_event_of(const gsp_event *evs, size_t n, gsp_event_type type)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (evs[i].type == (uint8_t)type) {
            return &evs[i];
        }
    }
    return NULL;
}

static inline size_t gs_count_of(const gsp_event *evs, size_t n, gsp_event_type type)
{
    size_t i, c = 0;
    for (i = 0; i < n; ++i) {
        if (evs[i].type == (uint8_t)type) {
            c++;
        }
    }
    return c;
}

/* Decode a write request back into a response, so a case asserts on the
 * MEANING of the bytes rather than on a string this library happens to emit. */
static inline gsp_status gs_decode_write(const gsp_write_request *w, gsp_response *out)
{
    return gsp_response_decode(w->data, (size_t)w->length, out);
}

#endif /* GS_SRV_H */
