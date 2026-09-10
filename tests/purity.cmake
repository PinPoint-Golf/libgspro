# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
#
# purity.cmake — assert that the core is still sans-I/O.
#
# design §2 is the property that makes this library embeddable at all: it owns
# no socket, thread, timer, clock or file, because the choice of socket API is a
# platform decision belonging to the host.  That property is easy to state and
# easy to erode one convenience at a time, so it is a test.
#
# Every symbol below would mean the library had started doing something the host
# is supposed to do.  ⚠ `socket`, `bind`, `accept` and `listen` above all: this
# is a LISTENER library that must never listen.

execute_process(COMMAND nm -u --defined-only=no "${LIB}"
                OUTPUT_VARIABLE nm_out
                ERROR_VARIABLE nm_err
                RESULT_VARIABLE nm_rc)

if(NOT nm_rc EQUAL 0)
    execute_process(COMMAND nm -u "${LIB}"
                    OUTPUT_VARIABLE nm_out
                    RESULT_VARIABLE nm_rc)
endif()

if(NOT nm_rc EQUAL 0)
    message(STATUS "purity: nm unavailable, skipping")
    return()
endif()

set(forbidden
    # the sockets this library exists NOT to own
    "socket" "socketpair" "bind" "listen" "accept" "accept4" "connect"
    "recv" "recvfrom" "recvmsg" "send" "sendto" "sendmsg" "shutdown"
    "select" "pselect" "poll" "epoll_create" "epoll_wait" "kqueue" "kevent"
    "getaddrinfo" "gethostbyname" "inet_addr" "inet_pton" "setsockopt"
    "WSAStartup" "WSASocketA" "WSASocketW" "closesocket" "ioctlsocket"
    # time
    "clock_gettime" "gettimeofday" "nanosleep" "usleep" "GetTickCount"
    "QueryPerformanceCounter"
    # threads
    "pthread_create" "thrd_create" "CreateThread" "_beginthreadex"
    # files
    "fopen" "freopen" "open64" "creat" "CreateFileA" "CreateFileW"
    # ambient state
    "getenv" "rand" "srand" "system"
)

set(violations "")
foreach(sym ${forbidden})
    if(nm_out MATCHES "U +_?${sym}\n" OR nm_out MATCHES "U +_?${sym}$")
        list(APPEND violations "${sym}")
    endif()
endforeach()

if(violations)
    message(FATAL_ERROR
        "libgspro must stay sans-I/O (design §2) but references: ${violations}")
endif()

message(STATUS "purity: ${LIB} is clean")
