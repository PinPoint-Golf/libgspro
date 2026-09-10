/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * gs_net_sys.h — the whole of the platform difference between POSIX sockets and
 * Winsock, in one place.
 *
 * ⚠ INTERNAL, and deliberately reachable from tests/ as well as net/: the
 * CT-T cases need a CLIENT socket (tests/gs_net_client.h) and a second copy of
 * these shims is a second place for "Windows spells it differently" to be got
 * wrong.  Nothing here is part of the public API and none of it is installed.
 *
 * ⚠ NOTHING IN THIS FILE MAY BE REACHED FROM src/.  The core owns no socket,
 * no clock and no thread (design §2), and tests/purity.cmake fails the build on
 * exactly the symbols below.  They live behind GS_BUILD_NET for that reason.
 */
#ifndef GS_NET_SYS_H
#define GS_NET_SYS_H

/* ⚠ FEATURE MACROS BEFORE ANY INCLUDE.  The project builds with -std=c11 and
 * CMAKE_C_EXTENSIONS OFF — that is `-std=c11`, not `-std=gnu11` — so glibc
 * hides getaddrinfo, fcntl's O_NONBLOCK and half of <sys/socket.h> unless
 * asked.  200809L is what getaddrinfo needs; Darwin additionally hides its own
 * extensions (SO_NOSIGPIPE among them) once _POSIX_C_SOURCE is defined. */
#if !defined(_WIN32)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#    define _DARWIN_C_SOURCE 1
#  endif
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET gs_sock;
#  define GS_SOCK_INVALID INVALID_SOCKET
#  define GS_SOCK_ERROR   SOCKET_ERROR
typedef int gs_socklen;
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <time.h>
typedef int gs_sock;
#  define GS_SOCK_INVALID (-1)
#  define GS_SOCK_ERROR   (-1)
typedef socklen_t gs_socklen;
#endif

/* ------------------------------------------------------------------------ */
/* Winsock needs waking up; everyone else does not.                          */
/* ------------------------------------------------------------------------ */
/* ⚠ Winsock refcounts startup/cleanup itself, so a nested listener is safe as
 * long as the calls are PAIRED — which is why gsp_net_close() runs the cleanup
 * even on the failure path out of open(). */
static inline bool gs_sys_startup(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

static inline void gs_sys_cleanup(void)
{
#if defined(_WIN32)
    (void)WSACleanup();
#endif
}

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */
static inline int gs_sys_errno(void)
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

/* True when a non-blocking call simply had nothing to do.  ⚠ EINTR is in here
 * on purpose: a signal (the ctrl-c gsplisten installs a handler for) makes
 * select() and read() return EINTR, and treating that as a socket failure would
 * close a client's connection because the user pressed a key. */
static inline bool gs_sys_would_block(int err)
{
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEINTR;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

/* The platform's own words for `err`, NUL-terminated, never empty.  ⚠ The
 * PLATFORM's words rather than ours: "Address already in use" is the entire
 * content of CT-T09's report, and a message this library invented would be one
 * more thing standing between a user and the fact that GSPro holds port 921. */
static inline void gs_sys_error_text(int err, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
#if defined(_WIN32)
    {
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, (DWORD)err, 0, out, (DWORD)out_size, NULL);
        if (n == 0u) {
            (void)snprintf(out, out_size, "winsock error %d", err);
            return;
        }
        /* FormatMessage ends its lines with ".\r\n"; a log line does not. */
        while (n > 0u && (out[n - 1u] == '\r' || out[n - 1u] == '\n' || out[n - 1u] == ' ')) {
            out[--n] = '\0';
        }
    }
#else
    {
        const char *text = strerror(err);
        (void)snprintf(out, out_size, "%s", (text != NULL) ? text : "unknown error");
    }
#endif
}

/* ------------------------------------------------------------------------ */
/* Socket odds and ends                                                      */
/* ------------------------------------------------------------------------ */
static inline void gs_sys_sock_close(gs_sock s)
{
    if (s == GS_SOCK_INVALID) {
        return;
    }
#if defined(_WIN32)
    (void)closesocket(s);
#else
    (void)close(s);
#endif
}

static inline bool gs_sys_set_nonblocking(gs_sock s)
{
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, (long)FIONBIO, &on) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/* ⚠ TCP_NODELAY OR A REPLY WAITS FOR COMPANY.  Nagle holds a 60-byte 200 back
 * for up to 40 ms hoping for more to send, on the one path where [MLM] is
 * counting to two seconds before it re-sends the shot (design §5.5). */
static inline bool gs_sys_set_nodelay(gs_sock s)
{
    int on = 1;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
                      (gs_socklen)sizeof(on)) == 0;
}

/*
 * ⚠ SIGPIPE KILLS THE PROCESS, and a launch monitor pulling its cable is the
 * ordinary way to earn one: a send() to a socket the peer has reset raises it.
 * A library must not change a process-global signal disposition to fix that, so
 * this is done PER SOCKET (BSD/macOS) or PER SEND (Linux) instead, and a
 * platform with neither is documented rather than patched behind the host's
 * back — an application there installs SIG_IGN itself, as gsplisten does.
 */
static inline void gs_sys_set_nosigpipe(gs_sock s)
{
#if defined(SO_NOSIGPIPE)
    int on = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on,
                     (gs_socklen)sizeof(on));
#else
    (void)s;
#endif
}

#if defined(MSG_NOSIGNAL)
#  define GS_SYS_SEND_FLAGS MSG_NOSIGNAL
#else
#  define GS_SYS_SEND_FLAGS 0
#endif

/* ⚠ struct timeval IS NOT THE SAME STRUCT EVERYWHERE.  Winsock declares both
 * members `long`; Darwin's tv_usec is a 32-bit suseconds_t beside a 64-bit
 * tv_sec.  One cast that fits everywhere does not exist, so the difference is
 * spelled once, here, rather than at each select() call site. */
static inline void gs_sys_timeval_set(struct timeval *tv, int64_t us)
{
#if defined(_WIN32)
    tv->tv_sec  = (long)(us / 1000000);
    tv->tv_usec = (long)(us % 1000000);
#else
    tv->tv_sec  = (time_t)(us / 1000000);
    tv->tv_usec = (suseconds_t)(us % 1000000);
#endif
}

/* ------------------------------------------------------------------------ */
/* The clock                                                                 */
/* ------------------------------------------------------------------------ */
/*
 * ⚠ MONOTONIC, ARBITRARY EPOCH — the contract of design §4.1, supplied here
 * because the core is forbidden to read a clock at all.  Not gettimeofday, not
 * time(): a wall clock stepped by NTP mid-session reorders a shot log.
 */
static inline int64_t gs_sys_now_us(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;   /* one thread by contract; no race to lose     */
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) {
            return (int64_t)GetTickCount64() * 1000;
        }
    }
    (void)QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart / freq.QuadPart) * 1000000
                     + ((now.QuadPart % freq.QuadPart) * 1000000) / freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
#endif
}

#endif /* GS_NET_SYS_H */
