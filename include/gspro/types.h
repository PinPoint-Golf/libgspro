/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gspro/types.h — scalar types, status codes, the time contract and the
 * connection id.
 *
 * Everything in libgspro is C11, POD and free of hidden allocation.  See
 * docs/design.md §4 for the type-system rules these headers follow.
 */
#ifndef GSPRO_TYPES_H
#define GSPRO_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(_WIN32) && defined(GSP_SHARED)
#  if defined(GSP_BUILDING_LIBRARY)
#    define GSP_API __declspec(dllexport)
#  else
#    define GSP_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define GSP_API __attribute__((visibility("default")))
#else
#  define GSP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */
/*
 * THE HOST CLOCK IS THE CALLER'S.  design §4.1.
 *
 * Every `int64_t ... _us` in this API is microseconds on a clock the caller
 * chooses and the library never reads.  The library calls no clock function,
 * on any platform, ever — host time only ever enters through the `now_us`
 * arguments the caller supplies.
 *
 * The clock MUST be monotonic.  PinPoint Studio uses std::chrono::steady_clock;
 * anything with the same properties works, including a synthetic clock in a
 * test.  The epoch is arbitrary and never interpreted.
 *
 * ⚠ The arrival stamp is the ONLY clock a shot has (protocol §8): the wire
 * carries no timestamp, and a launch monitor measures, computes and sends
 * some hundreds of milliseconds after impact.  Treat host_recv_us as an upper
 * bound on the impact time, never as an estimate of it.
 */
typedef int64_t gsp_time_us;

/* A timestamp that is structurally unavailable. */
#define GSP_TIME_UNKNOWN  INT64_MIN

/* gsp_server_next_due_us() returns this when nothing is pending — which is
 * the ordinary case: the protocol has no deadline of its own (protocol §9.6). */
#define GSP_TIME_NEVER    INT64_MAX

/* ------------------------------------------------------------------------ */
/* Status                                                                    */
/* ------------------------------------------------------------------------ */
/*
 * Non-negative values are success.  Negative values are failure.  Test with
 * `if (st < GSP_OK)`, never `if (st != GSP_OK)` — GSP_PENDING is not an error.
 */
typedef enum gsp_status {
    GSP_OK                       = 0,
    GSP_PENDING                  = 1,  /* the framer needs more bytes           */

    GSP_ERR_INVALID_ARG          = -1,
    GSP_ERR_INVALID_STATE        = -2, /* e.g. a connection id already open     */
    GSP_ERR_NO_MEMORY            = -3,
    GSP_ERR_BUFFER_TOO_SMALL     = -4,
    GSP_ERR_NOT_SUPPORTED        = -5,
    GSP_ERR_TOO_MANY_CONNECTIONS = -6, /* config.max_connections reached         */
    GSP_ERR_UNKNOWN_CONNECTION   = -7, /* id was never opened, or is closed     */
    GSP_ERR_MALFORMED            = -8, /* not JSON, not an object, bad grammar  */
    GSP_ERR_MESSAGE_TOO_LARGE    = -9, /* exceeds policy.max_message_bytes      */
    GSP_ERR_QUEUE_FULL           = -10,/* ⚠ the WRITE ring — see design §3.4    */
    GSP_ERR_CLOSED               = -11 /* gsp_server_close() has been called    */
} gsp_status;

/* Stable, allocation-free, never NULL.  Safe for logs. */
GSP_API const char *gsp_status_str(gsp_status status);

/* ------------------------------------------------------------------------ */
/* Connection identity                                                       */
/* ------------------------------------------------------------------------ */
/*
 * THE CONNECTION ID IS THE HOST'S.  design §3.2.  The host already holds a
 * handle per socket — a QTcpSocket*, a descriptor, a Python object — and a
 * library-issued id would be a table it has to keep.  So the host passes any
 * non-zero value it can map back to its socket, and every write request and
 * event carries it.  The library refuses a duplicate while that id is open.
 */
typedef uint32_t gsp_conn_id;

#define GSP_CONN_NONE  ((gsp_conn_id)0)
#define GSP_CONN_ALL   ((gsp_conn_id)UINT32_MAX)  /* broadcast, where accepted */

/* ------------------------------------------------------------------------ */
/* Allocator (optional)                                                      */
/* ------------------------------------------------------------------------ */
/*
 * gsp_server_create() makes exactly ONE allocation and gsp_server_destroy()
 * exactly one free (design §3.4).  Leave this zeroed to use malloc/free, or
 * supply both functions to route that allocation through a pool.
 */
typedef struct gsp_allocator {
    void *(*alloc)(void *ctx, size_t size);
    void  (*free)(void *ctx, void *ptr);
    void  *ctx;
} gsp_allocator;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_TYPES_H */
