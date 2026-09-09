/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
#ifndef GSPRO_VERSION_H
#define GSPRO_VERSION_H

#include "gspro/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GSP_VERSION_MAJOR 0
#define GSP_VERSION_MINOR 1
#define GSP_VERSION_PATCH 0
#define GSP_VERSION_STRING "0.1.0"

/*
 * The ABI version changes whenever a public struct's layout changes.  A binding
 * generated against one ABI must refuse to load a library reporting another —
 * gsp_abi_check() does that comparison for it, including the struct sizes,
 * which is what actually goes wrong when a header and a shared object drift.
 *
 * ⚠ SIZES ARE NECESSARY AND NOWHERE NEAR SUFFICIENT (libwrist design §4.6.1).
 * A binding whose field is one slot out passes this check and returns
 * plausible numbers.  The Python binding therefore also pins every field
 * offset and every enumerator through tools/gs_abi_table.c, both ways.
 */
#define GSP_ABI_VERSION 1

GSP_API const char *gsp_version_string(void);
GSP_API uint32_t gsp_abi_version(void);

/* Size of each public POD struct, as the LIBRARY was built.  A binding compares
 * these against its own sizeof() and fails at load rather than at random. */
typedef struct gsp_abi_sizes {
    uint32_t abi_version;
    uint32_t message;
    uint32_t ball_data;
    uint32_t club_data;
    uint32_t shot_options;
    uint32_t player_info;
    uint32_t event;
    uint32_t write_request;
    uint32_t wire_chunk;
    uint32_t connection_info;
    uint32_t server_config;
    uint32_t message_layout_version;
} gsp_abi_sizes;

GSP_API void gsp_abi_sizes_get(gsp_abi_sizes *out);

/* GSP_OK if `expected` (filled in by the caller from its own headers) matches
 * this build; GSP_ERR_NOT_SUPPORTED otherwise. */
GSP_API gsp_status gsp_abi_check(const gsp_abi_sizes *expected);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GSPRO_VERSION_H */
