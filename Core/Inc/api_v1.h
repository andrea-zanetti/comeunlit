/**
 * @file    api_v1.h
 * @brief   HTTP API v1 handlers (docs/API.md).
 *
 *          All requests and responses are signed (API.md section 3); use
 *          tools/sbctl.py instead of curl.
 *
 *            GET  /api/v1/health                version, buses, uptime, heap
 *            GET  /api/v1/blinds/{bus}/{addr}   live poll
 *            POST /api/v1/blinds/{bus}/{addr}   {"cmd":"up|down|stop"}
 *            POST /api/v1/batch                 {"cmd":..,"targets":[..]}
 *            GET  /api/v1/events                stream: module announcements
 *            GET  /api/v1/admin/frames          stream: every frame
 *            POST /api/v1/admin/tx/{bus}        {"hex":..,"checksum":..} or
 *                                               {"bits":..}
 */

#ifndef API_V1_H
#define API_V1_H

#include <stdint.h>

#include "sb_blind.h"
#include "sb_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Path prefix routed to api_v1_handle(). */
#define API_V1_PREFIX "/api/v1/"

/** api_v1_handle() result: the socket now belongs to a stream task. */
#define API_V1_TAKEN 1

/**
 * Handle one request whose path starts with API_V1_PREFIX.
 * req: request buffer (request line + headers), NUL-terminated.
 * body/body_len: request body, NUL-terminated.
 * Returns 0 when the response is complete (the caller closes sock), or
 * API_V1_TAKEN when a stream task has taken the socket (do not close it).
 */
int api_v1_handle(int sock, const char* req, const char* body, int body_len);

/**
 * Provided by the HTTP server (LWIP/App/http_sse.c; stubbed in tests):
 * start a stream task that owns `sock`, first writes the 200 header with
 * X-SB-MAC = hdr_mac, then signed events. Returns 0 if started, <0 if no
 * stream slot or memory is available (the caller still owns sock).
 */
int api_v1_stream_open(int sock, sb_stream_type_t type, const char* req_mac,
                       const char* hdr_mac);

/* ─── Pieces exposed for host tests (tests/host) ────────────────────────── */

/** Parse {"cmd":"up|down|stop"}. Returns 0 and sets *cmd, or -1. */
int api_v1_parse_cmd(const char* body, sb_cmd_t* cmd);

/**
 * Parse "targets":[{"bus":N,"addr":N},...] into r[i].bus / r[i].addr.
 * Returns the count, -1 if malformed, -2 if more than max.
 */
int api_v1_parse_targets(const char* body, sb_blind_result_t* r, int max);

/** JSON object for one command result (API.md 5.2). Returns its length. */
int api_v1_format_result(char* out, int out_sz, const sb_blind_result_t* r);

/** "10 06 01 01" (spaces optional) -> bytes. Count, or -1 if malformed. */
int api_v1_parse_hex(const char* s, int len, uint8_t* out, int max);

/** HTTP status for an SB_OK / SB_E_* code. */
int api_v1_status_of(int err);

/** API error code string for an SB_E_* code ("no_reply", ...). */
const char* api_v1_error_of(int err);

#ifdef __cplusplus
}
#endif

#endif /* API_V1_H */
