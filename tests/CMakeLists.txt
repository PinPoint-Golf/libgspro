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
    if(GS_SCAFFOLDED)
        string(REPLACE ";" " " _gs_txt "${GS_SCAFFOLDED}")
        target_compile_definitions(${name} PRIVATE GS_SCAFFOLDED_GROUPS="${_gs_txt}")
    endif()
    # ⚠ NO -Wno-unused-function HERE, DELIBERATELY.  The harness leaves helpers
    # unused per translation unit, but a suppression added to the target lands
    # BEFORE gs_build_flags' -Wall in the command line and is undone by it.  The
    # helpers are `static inline` instead, which no compiler warns about.
    add_test(NAME ${name} COMMAND ${name})
    # ⚠ LABELS EXIST SO CI CAN BLOCK ON WHAT SHOULD PASS TODAY.  While a source
    # group is scaffolded the conformance cases are EXPECTED to fail, and a job
    # that is always red teaches everyone to ignore it.  `ctest -L ready` runs
    # only what does not need the library; the full run is a progress report
    # until the last group lands, and becomes the gate the moment it does.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 LABELS "conformance")
endforeach()

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
