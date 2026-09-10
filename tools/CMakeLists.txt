# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
#
# Development tools.  ⚠ Nothing here is part of the library and nothing the
# library does depends on any of it; GS_BUILD_TOOLS defaults OFF for a consumer
# who has embedded libgspro in their own build.

# ---------------------------------------------------------------------------
# gs_abi_table — every public struct's layout, as the compiler built it.
#
# ⚠ THE ONLY THING THAT CAN TELL python/gspro/_types.py THE TRUTH.
# gsp_abi_check() compares eleven struct SIZES at binding load and says nothing
# about where the fields inside them sit or about any enum at all, so a ctypes
# declaration one slot out passes it and returns plausible numbers forever.
# tests/test_python_abi.py compares the binding against this.
#
# It links the STATIC library rather than gspro_ffi: it reports on the layout
# the headers describe, which is a property of the compiler, not of a
# particular shared object.
# ---------------------------------------------------------------------------
add_executable(gs_abi_table gs_abi_table.c)
target_link_libraries(gs_abi_table PRIVATE gspro gs_build_flags)

# ---------------------------------------------------------------------------
# gsplisten — a complete listener, in C, on the reference transport.
#
# ⚠ THE C TWIN OF tools/gsp_listen.py, and it exists because the Python one
# needs Python.  A PiTrac, a NAS or a Windows box with a compiler and nothing
# else can still meet a launch monitor with this, which is the situation
# design §11's package 7 (FIRST CONTACT) is most likely to be run in.
#
# Needs GS_BUILD_NET: it is a host, and the socket lives there.
# ---------------------------------------------------------------------------
if(GS_BUILD_NET AND GS_BUILD_RECORD)
    add_executable(gsplisten gsplisten.c)
    # ⚠ It links the recorder too, because --record is the whole reason to run
    # it beside a real launch monitor (design §11 package 7).
    target_link_libraries(gsplisten PRIVATE gspro_net gspro_record gspro gs_build_flags)
elseif(GS_BUILD_NET)
    message(STATUS "gsplisten needs GS_BUILD_RECORD as well; not building it")
else()
    message(STATUS "gsplisten needs GS_BUILD_NET; not building it")
endif()

# ---------------------------------------------------------------------------
# gswire — read, replay and mine a .gswire capture.
#
# ⚠ `gswire replay` is the tool design §7 exists for: it drives a recorded
# session back through THIS library and reports what it makes of it now, which
# is how a fix to the decoder is checked against a real session without the
# launch monitor that produced it.  `gswire extract` is how one of that
# session's messages becomes a fixture in tests/fixtures/.
# ---------------------------------------------------------------------------
if(GS_BUILD_RECORD)
    add_executable(gswire gswire.c)
    target_link_libraries(gswire PRIVATE gspro_record gspro gs_build_flags)
else()
    message(STATUS "gswire needs GS_BUILD_RECORD; not building it")
endif()

# ⚠ The Python tools are not built, installed or byte-compiled by CMake — they
# are scripts that find the shared object at run time (python/gspro/_library.py)
# and are meant to be copied out of this repository:
#
#   tools/gsp_listen.py   a complete listener; what to run with a real device
#                         on the mat before touching an application
#   tools/gsp_shoot.py    a launch monitor simulator, and the client half of
#                         the CT-T host-transport cases
#
# ⛔ gsp_shoot.py refuses port 921 on a non-loopback address: that combination
# means a real GSPro, and driving somebody else's game is permanently out of
# scope (design §1).
