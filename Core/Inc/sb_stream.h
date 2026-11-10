/**
 * @file    sb_stream.h
 * @brief   Signed SSE streams (docs/API.md 3.6, 5.4, 6.1).
 *
 *          One sb_stream_t per open stream. The stream task feeds it every
 *          frame read from the frame ring; it returns the complete SSE text
 *          to send (or nothing), already signed and numbered:
 *
 *              event: <name>
 *              id: <seq>
 *              data: <64 hex MAC> <json>
 *
 *          SB_STREAM_FRAMES: every frame (rx / tx / drop)  -> event "frame"
 *          SB_STREAM_EVENTS: module announcements (type 0x00) only, repeats
 *                            within 500 ms dropped          -> event "state"
 *          Both:             heartbeat every 15 s           -> event "hb"
 *
 *          The only memory is a few hundred ms of recent frames (for the
 *          repeat filter and to tell bridge-caused acks from wall switches).
 *
 *          No RTOS or HAL dependencies (host-testable). Uses the caller's
 *          scratch for signing, so streams can run in parallel tasks.
 */

#ifndef SB_STREAM_H
#define SB_STREAM_H

#include <stdint.h>

#include "sb_auth.h"
#include "sb_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SB_STREAM_HB_MS 15000u      /* heartbeat period                    */
#define SB_STREAM_DEDUP_MS 500u     /* repeated announcement filter        */
#define SB_STREAM_SOURCE_MS 200u    /* ack within this of our press = ours */
#define SB_STREAM_LINE_MAX 640      /* longest SSE text produced           */

typedef enum {
    SB_STREAM_EVENTS = 1, /* GET /api/v1/events        */
    SB_STREAM_FRAMES = 2, /* GET /api/v1/admin/frames  */
} sb_stream_type_t;

#define SB_STREAM_RECENT 8

typedef struct {
    uint8_t used;
    uint8_t bus, addr, in, out;
    uint32_t t;
} sb_stream_recent_t;

typedef struct {
    sb_stream_type_t type;
    char req_mac[SB_AUTH_MAC_HEX + 1];
    uint32_t seq;                                  /* last id sent       */
    sb_stream_recent_t seen[SB_STREAM_RECENT];     /* announcements      */
    sb_stream_recent_t presses[SB_STREAM_RECENT];  /* our 0x10 frames    */
    uint8_t scratch[SB_STREAM_LINE_MAX];           /* signing buffer     */
} sb_stream_t;

void sb_stream_init(sb_stream_t* s, sb_stream_type_t type,
                    const char* req_mac_hex);

/**
 * Feed one frame from the ring. Returns the length of the SSE text written
 * to out (>0), 0 if this frame produces nothing for this stream, <0 on
 * error (signing failed: the stream must be closed).
 */
int sb_stream_frame(sb_stream_t* s, const sb_ring_frame_t* f, char* out,
                    int out_sz);

/** Signed heartbeat. Same return convention. */
int sb_stream_heartbeat(sb_stream_t* s, uint32_t uptime_ms, char* out,
                        int out_sz);

/** JSON of one frame as sent on /admin/frames (exposed for tests). */
int sb_stream_frame_json(const sb_ring_frame_t* f, char* out, int out_sz);

#ifdef __cplusplus
}
#endif

#endif /* SB_STREAM_H */
