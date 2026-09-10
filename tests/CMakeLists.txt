# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
#
# The conformance suite of docs/conformance.md §3.  One binary per case family;
# each case is named for its CT id so a failure line points at one table row
# that says what the case is and which client demands it.

set(GS_TEST_SOURCES
    test_frame.c      # CT-F01 .. CT-F12   framing
    test_decode.c     # CT-D01 .. CT-D24   decode and tolerances
    test_kind.c       # CT-K01 .. CT-K08   shot / heartbeat / status
    test_reply.c      # CT-R01 .. CT-R08   one reply per message
    test_player.c     # CT-P01 .. CT-P12   201 and the 202/203 pair
    test_conn.c       # CT-C01 .. CT-C11   connection lifecycle
    test_robust.c     # CT-X01 .. CT-X07   hostile input, rings, ABI
    test_api.c        # the vocabulary tables and the ABI surface
)

foreach(src ${GS_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE gspro gs_build_flags)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    # ⚠ The fixture directory is compiled in rather than resolved from argv[0]
    # or the working directory: ctest, a debugger and a direct shell invocation
    # all start somewhere different, and a suite that silently found no
    # fixtures would be a suite that skipped rather than passed.
    target_compile_definitions(${name} PRIVATE
        GS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    # ⚠ NO -Wno-unused-function HERE, DELIBERATELY.  The harness leaves helpers
    # unused per translation unit, but a suppression added to the target lands
    # BEFORE gs_build_flags' -Wall in the command line and is undone by it.  The
    # helpers are `static inline` instead, which no compiler warns about.
    add_test(NAME ${name} COMMAND ${name})
    # ⚠ THE FULL RUN IS NOW THE GATE.  While a source group was scaffolded the
    # conformance cases were expected to fail and `ctest -L ready` ran only what
    # did not need the library.  Every group has landed, so `conformance` is
    # green or the library is broken; the `ready` label stays because the purity
    # and cross-check tests need no C at all.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 LABELS "conformance")
endforeach()

# ---------------------------------------------------------------------------
# The host transport cases, against the C reference transport (conformance §3.8)
#
# ⚠ THE SAME TEN ROWS AS tests/test_python_transport.py, AND THAT IS THE DESIGN.
# CT-T is the one family that is about a HOST rather than about the library —
# reply latency, one send() per message, holding a 200 apart from a 201, a
# client that vanishes mid-message — so it is run ONCE PER HOST ADAPTER.  The
# asyncio adapter went green in package 4; this is the same checklist pointed at
# net/gs_net.c, which shares no code with it below the socket.  PinPoint's
# QTcpServer adapter is the third run and lives in PinPoint.
#
# ⚠ It builds ONLY with GS_BUILD_NET, and it is the one binary in this directory
# that links a socket: `purity` below runs on `gspro`, which this deliberately is
# not.
# ---------------------------------------------------------------------------
if(GS_BUILD_NET)
    add_executable(test_net test_net.c)
    target_link_libraries(test_net PRIVATE gspro_net gspro gs_build_flags)
    target_include_directories(test_net PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        # ⚠ The client half (gs_net_client.h) uses the SAME platform shim the
        # transport does.  A second copy of "Windows spells it differently" is a
        # second place to get it wrong.
        ${CMAKE_SOURCE_DIR}/net)
    target_compile_definitions(test_net PRIVATE
        GS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME test_net COMMAND test_net)
    # ⚠ CT-T05 observes a real silence: five seconds by default, and GSP_SOAK=1
    # runs the full five minutes the row names — which is why this is generous
    # rather than the 120 s the socket-free cases get.
    set_tests_properties(test_net PROPERTIES TIMEOUT 420 LABELS "conformance")
endif()

# ---------------------------------------------------------------------------
# The sans-I/O gate (design §2, conformance CT-X06)
#
# ⚠ Runs on `gspro`, the target every C consumer links.  Never point it at
# gspro_ffi if that target ever gains a module that opens files.
# ---------------------------------------------------------------------------
add_test(NAME purity
         COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:gspro>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/purity.cmake)
set_tests_properties(purity PROPERTIES LABELS "ready")

# ---------------------------------------------------------------------------
# The fixtures are a second implementation of the protocol document's claims,
# so they are checked against it independently of any C.  This one runs with no
# library at all, which is why it is worth having while the library is still
# being written.
# ---------------------------------------------------------------------------
if(APPLE AND NOT DEFINED Python3_FIND_FRAMEWORK)
    # Prefer the interpreter on PATH over the Command Line Tools' signed 3.9:
    # a sanitizer runtime dlopen()ed into a platform binary is refused outright.
    set(Python3_FIND_FRAMEWORK LAST)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME fixtures
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fixtures.py
                     ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
    set_tests_properties(fixtures PROPERTIES ENVIRONMENT "PYTHONIOENCODING=utf-8"
                         LABELS "ready")
    # ⚠ THE SUITE'S OWN COMPLETENESS IS A TEST, because nothing else can be: a
    # CT- row added to the document with no case is a claim nobody checks, and
    # a case with no row is a preference somebody encoded without saying which
    # client demands it.  Both read as ordinary work and neither fails anything.
    add_test(NAME coverage
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_coverage.py
                     ${CMAKE_SOURCE_DIR})
    set_tests_properties(coverage PROPERTIES ENVIRONMENT "PYTHONIOENCODING=utf-8"
                         LABELS "ready")
else()
    message(STATUS "no python3: the fixture and coverage cross-checks will not run")
endif()

# ---------------------------------------------------------------------------
# The Python binding
#
# ⚠ SAME SHAPE AGAIN, ONE REGISTER UP.  python/gspro/_types.py declares every
# public struct and enum a SECOND time, in ctypes.  gsp_abi_check() guards
# eleven struct SIZES at load and nothing else — not one field offset, not one
# enumerator — and version.h says so itself: "a binding whose field is one slot
# out passes this check and returns plausible numbers".
#
# `gs_abi_table` reads the real layout out of the compiler and test_python_abi
# compares the two, in both directions, for structs, fields, enums and bounds.
#
# ⚠ AND THE HOST TRANSPORT CASES.  CT-T01..T10 (conformance §3.8) are about
# what a HOST does with a real socket — reply latency, one write per message,
# spacing a 200 from a 201, a client that vanishes mid-message.  The library
# owns no socket by construction, so nothing in the C suite can reach them.
# ---------------------------------------------------------------------------
if(GS_BUILD_TOOLS AND GS_BUILD_FFI AND Python3_Interpreter_FOUND)
    # ⚠ The .so is passed explicitly rather than discovered.  This must pin the
    # layout of the object THIS build produced — a search that found build/dev
    # while testing build/san would report on the wrong artefact and pass.
    add_test(NAME python_abi
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_python_abi.py
                     $<TARGET_FILE:gs_abi_table>
                     $<TARGET_FILE:gspro_ffi>)

    add_test(NAME python_transport
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/test_python_transport.py
                     $<TARGET_FILE:gspro_ffi>)
    # ⚠ CT-T05 observes a real silence.  Five seconds by default; GSP_SOAK=1
    # runs the full five minutes the conformance row names, which is why the
    # timeout is generous rather than tight.
    set_tests_properties(python_transport PROPERTIES TIMEOUT 420)

    set_tests_properties(python_abi python_transport PROPERTIES LABELS "conformance")

    # ⚠ These harnesses report in the house style — ⚠, ⛔, § — and Windows gives
    # CPython a cp1252 stdout, where writing any of them raises
    # UnicodeEncodeError and fails the test on its OUTPUT rather than its
    # subject.  The sanitizer block below REPLACES this ENVIRONMENT wholesale
    # rather than adding to it, so it repeats the variable.
    set_tests_properties(python_abi python_transport
                         PROPERTIES ENVIRONMENT "PYTHONIOENCODING=utf-8")

    # ⚠ ASan lives in the shared object, not in the interpreter, so CPython has
    # to preload the runtime before dlopening it.  If the preload is merely
    # missing the tests are left to FAIL rather than skip: a sanitizer build
    # that quietly stops checking is the thing this project has a rule about.
    #
    # ⚠⚠ AND WHAT IT DOES NOT BUY.  ASan instruments the code it COMPILED.  A
    # ctypes read happens inside CPython, which it did not, so a Python-side
    # mistake returns plausible data here with no report at all.  What this run
    # does catch is a fault the LIBRARY commits at the binding's request — a
    # wrong length into gsp_server_on_bytes, a stale handle handed back in.
    if((GS_ENABLE_ASAN OR GS_ENABLE_UBSAN) AND NOT MSVC)
        # ⛔ macOS + ASan is the one case where the preload CANNOT exist: dyld
        # answers `Sanitizer load violates platform policy` because the Command
        # Line Tools' runtime is a signed platform binary that will only load
        # into an image linked against it, and ctypes dlopen()s ours instead.
        # DISABLED rather than unregistered, so ctest lists them every run and
        # the coverage this platform is missing stays legible.  The Linux/gcc
        # sanitizer job in CI is what covers them.
        if(APPLE AND GS_ENABLE_ASAN)
            set_tests_properties(python_abi python_transport PROPERTIES DISABLED TRUE)
            message(STATUS
                "python binding tests: DISABLED under ASan on macOS — dyld refuses "
                "to preload the Command Line Tools' ASan runtime into a ctypes host. "
                "The Linux/gcc CI sanitizer job is what covers them.")
        else()
            # ⚠ THE RUNTIME IS NAMED DIFFERENTLY BY EACH COMPILER.  gcc ships a
            # shared libasan.so; clang links its runtime STATICALLY into
            # executables, so nothing resolves the __asan_* symbols in
            # libgspro_ffi.so when ctypes dlopen()s it, and its shared runtime
            # lives under -print-runtime-dir with an architecture in the name.
            set(GS_SAN_RUNTIME "")
            execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=libasan.so
                            OUTPUT_VARIABLE GS_SAN_RUNTIME
                            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(NOT EXISTS "${GS_SAN_RUNTIME}")
                execute_process(COMMAND ${CMAKE_C_COMPILER} -print-runtime-dir
                                OUTPUT_VARIABLE GS_SAN_RTDIR
                                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
                file(GLOB GS_SAN_CANDIDATES "${GS_SAN_RTDIR}/libclang_rt.asan*.so")
                if(GS_SAN_CANDIDATES)
                    list(GET GS_SAN_CANDIDATES 0 GS_SAN_RUNTIME)
                endif()
            endif()
            if(EXISTS "${GS_SAN_RUNTIME}")
                set_tests_properties(python_abi python_transport PROPERTIES ENVIRONMENT
                    "LD_PRELOAD=${GS_SAN_RUNTIME};ASAN_OPTIONS=detect_leaks=0;PYTHONIOENCODING=utf-8")
            elseif(GS_ENABLE_ASAN)
                message(STATUS
                    "python binding tests: no sanitizer runtime from ${CMAKE_C_COMPILER}; "
                    "they will run without the preload and are expected to fail")
            endif()
        endif()
    endif()
elseif(Python3_Interpreter_FOUND)
    message(STATUS "python binding tests need GS_BUILD_TOOLS and GS_BUILD_FFI")
endif()

# ---------------------------------------------------------------------------
# gsplisten, end to end, as a user runs it
#
# ⚠ BELOW find_package(Python3), because it needs the interpreter it finds.
#
# ⚠ NOT A CONFORMANCE CASE and carries no CT id: the document numbers what the
# protocol and its clients demand, not what our own tools do.  It is here because
# test_net above exercises the TRANSPORT by linking it, and nothing exercised the
# TOOL — its arguments, its bind-failure report, its event printing, its exit
# path — and a tool that only ever runs by hand breaks between the times somebody
# runs it.  It is the first thing a user meets with a launch monitor on the mat.
# ---------------------------------------------------------------------------
if(GS_BUILD_NET AND GS_BUILD_TOOLS AND Python3_Interpreter_FOUND)
    add_test(NAME gsplisten
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gsplisten.py
                     $<TARGET_FILE:gsplisten> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
    set_tests_properties(gsplisten PROPERTIES TIMEOUT 120 LABELS "conformance"
                         ENVIRONMENT "PYTHONIOENCODING=utf-8")
endif()
