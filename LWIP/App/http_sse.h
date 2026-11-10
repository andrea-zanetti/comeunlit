/**
 * @file    http_sse.h
 * @brief   HTTP server for the SimpleBus bridge API v1 (docs/API.md):
 *          signed requests and signed SSE streams. See http_sse.c.
 */

#ifndef HTTP_SSE_H
#define HTTP_SSE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP server. Creates its own FreeRTOS task, which also runs the
 * crypto self-tests and picks the API session for this boot.
 * Call after LwIP is initialised and the interface has an address.
 */
void http_sse_init(void);

/** Number of streams currently open (at most MAX_SSE_CLIENTS). */
int http_sse_stream_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SSE_H */
