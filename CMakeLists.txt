# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Mark Liversedge
cmake_minimum_required(VERSION 3.16)

project(libgspro
    VERSION 0.1.0
    DESCRIPTION "Sans-I/O C library that plays the GSPro Connect server role"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Are we the top-level project, or embedded in someone else's build?
#
# ⚠ THE CACHE IS NOT DIRECTORY-SCOPED.  There is ONE cache per build tree, so a
# `set(... CACHE ... FORCE)` down here rewrites the *consumer's* settings.
# Everything below that could reach outside this directory is gated on this.
#
# The rule, as in libwrist: adding libgspro to a project must change NOTHING
# about that project.  Not its build type, not its test registry, not its
# install manifest.
#
# ⚠ Spelled as a CMAKE_SOURCE_DIR comparison rather than PROJECT_IS_TOP_LEVEL
# deliberately: that variable is CMake 3.21+ and this project promises 3.16,
# which is the system CMake on Ubuntu 20.04 LTS.
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(GS_IS_TOP_LEVEL ON)
else()
    set(GS_IS_TOP_LEVEL OFF)
endif()

option(GS_BUILD_TESTS     "Build the conformance suite"              ${GS_IS_TOP_LEVEL})
option(GS_BUILD_TOOLS     "Build the command line tools"             ${GS_IS_TOP_LEVEL})
option(GS_BUILD_FFI       "Build the shared object the Python binding loads" ${GS_IS_TOP_LEVEL})
option(GS_ENABLE_ASAN     "Build with AddressSanitizer"              OFF)
option(GS_ENABLE_UBSAN    "Build with UndefinedBehaviorSanitizer"    OFF)
option(GS_ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)
# ⚠ -Werror is a CI gate, not a property of the code.  Imposing it on a consumer
# means their build breaks because THEIR compiler warns where ours does not.
option(GS_WERROR          "Treat warnings as errors"                 ${GS_IS_TOP_LEVEL})
option(GS_INSTALL         "Generate the install rules"               ${GS_IS_TOP_LEVEL})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(GS_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning / instrumentation flags, through an INTERFACE target so the library
# never leaks them to consumers.
# ---------------------------------------------------------------------------
add_library(gs_build_flags INTERFACE)

if(MSVC)
    target_compile_options(gs_build_flags INTERFACE /W4 /permissive-)
    # The core touches none of these; the tests open fixture files and MSVC's
    # C4996 on the standard C11 spellings would fail /WX.
    target_compile_definitions(gs_build_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(GS_WERROR)
        target_compile_options(gs_build_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(gs_build_flags INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wcast-qual -Wcast-align -Wpointer-arith
        -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
        -Wredundant-decls -Wswitch-enum -Wundef
        -Wdouble-promotion -Wformat=2 -Wvla)
    if(GS_WERROR)
        target_compile_options(gs_build_flags INTERFACE -Werror)
    endif()
endif()

set(_gs_san "")
if(GS_ENABLE_ASAN)
    list(APPEND _gs_san -fsanitize=address -fno-omit-frame-pointer)
endif()
if(GS_ENABLE_UBSAN)
    list(APPEND _gs_san -fsanitize=undefined -fno-sanitize-recover=undefined)
endif()
if(_gs_san)
    target_compile_options(gs_build_flags INTERFACE ${_gs_san})
    target_link_options(gs_build_flags INTERFACE ${_gs_san})
    # ⚠ clang links its sanitizer runtime statically, which breaks the one
    # target that is dlopen()ed rather than linked (gspro_ffi, loaded by ctypes
    # from an uninstrumented CPython).  Shared runtimes resolve at load.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(gs_build_flags INTERFACE -shared-libsan)
        target_link_options(gs_build_flags INTERFACE -shared-libsan)
    endif()
endif()

if(GS_ENABLE_COVERAGE)
    target_compile_options(gs_build_flags INTERFACE --coverage -O0 -g)
    target_link_options(gs_build_flags INTERFACE --coverage)
endif()

# ---------------------------------------------------------------------------
# Sources
#
# ⚠ A LITERAL LIST, AND IT BECAME ONE THE DAY THE LAST GROUP LANDED.  While the
# conformance suite ran ahead of the library (design §11's packages 1b to 3)
# this list was DISCOVERED from the files on disk, so a package could land as
# one new file and src/gs_unimplemented.c could stand in for whatever was still
# missing.  That was tolerable only while the file set was the thing being
# tracked: CMake does not re-run when a file appears, so a discovered list
# silently builds yesterday's library — which is exactly what it did on the
# first sanitizer run after gs_server.c landed.
#
# The five groups, and what each answers for:
#   gs_frame.c    the string-aware framer            design §5.3
#   gs_decode.c   the bounded JSON reader            design §5.4
#   gs_encode.c   the only path bytes leave by       design §9.1
#   gs_misc.c     vocabulary, derivation, formatters design §4
#   gs_server.c   connections, replies, events       design §5
# ---------------------------------------------------------------------------
set(GS_CORE_SOURCES
    src/gs_frame.c
    src/gs_decode.c
    src/gs_encode.c
    src/gs_misc.c
    src/gs_server.c)

add_library(gspro ${GS_CORE_SOURCES})
add_library(gspro::gspro ALIAS gspro)

target_include_directories(gspro
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(gspro PRIVATE $<BUILD_INTERFACE:gs_build_flags>)

set_target_properties(gspro PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(gspro PRIVATE GSP_BUILDING_LIBRARY=1)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(gspro PUBLIC GSP_SHARED=1)
endif()

# ⚠ NO libm.  The only floating point the library does is the spin derivation
# (hypot/atan2 in gsp_ball_data_derive), and if that ever needs -lm it is
# linked here — not by the consumer discovering an undefined symbol.
if(NOT WIN32)
    target_link_libraries(gspro PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# gspro_ffi — ONE shared object for a language binding to dlopen.
# ---------------------------------------------------------------------------
if(GS_BUILD_FFI)
    add_library(gspro_ffi SHARED ${GS_CORE_SOURCES})
    target_include_directories(gspro_ffi
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(gspro_ffi PRIVATE gs_build_flags)
    if(NOT WIN32)
        target_link_libraries(gspro_ffi PUBLIC m)
    endif()
    # ⚠ Both defines are required, and on Windows neither is sufficient alone:
    # GSP_API is __declspec(dllexport) only when GSP_SHARED and
    # GSP_BUILDING_LIBRARY are both set (include/gspro/types.h).  Miss one and
    # the object exports nothing, which fails at dlopen rather than at build.
    target_compile_definitions(gspro_ffi PRIVATE
        GSP_BUILDING_LIBRARY=1 GSP_SHARED=1)
    set_target_properties(gspro_ffi PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON)
endif()

# ---------------------------------------------------------------------------
# Tools
#
# ⚠ Before the tests, because tests/CMakeLists.txt registers the ABI
# cross-check against $<TARGET_FILE:gs_abi_table>.
# ---------------------------------------------------------------------------
if(GS_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
if(GS_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS gspro EXPORT gsproTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/gspro DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES include/gspro.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
