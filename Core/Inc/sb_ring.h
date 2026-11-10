/**
 * @file    sb_ring.h
 * @brief   Lock-free ring buffer for SimpleBus frames: received, sent and
 *          dropped, on every bus.
 *
 *          Producers: the receiver task (received frames) and whichever
 *          task transmits (echo of every sent or dropped frame, see
 *          simplebus_tx_bits_locked). Pushes are made atomic by masking
 *          interrupts for the few hundred cycles of the copy, so producers
 *          never interleave.
 *          Consumers: reply matching (sb_bus_transact) and the streams, each
 *          with its own read index. Writes always succeed: the oldest frame
 *          is overwritten, and a reader that falls behind skips ahead.
 */

#ifndef SB_RING_H
#define SB_RING_H

#include <stdint.h>
#include <string.h>

#include "cmsis_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ─────────────────────────────────────────────────────── */

#define RING_SIZE 128 /* must be power of 2 */
#define RING_MASK (RING_SIZE - 1)
#define MAX_BIT_STRING 128 /* max symbols per frame (60 typical) */
#define MAX_SSE_CLIENTS 4

/* ─── Frame entry ───────────────────────────────────────────────────────── */

/* Direction of a frame in the stream. */
#define SB_DIR_RX 0         /* received from the bus                     */
#define SB_DIR_TX 1         /* transmitted by the bridge                 */
#define SB_DIR_DROP 2 /* not sent: listen-before-talk timed out    */

typedef struct {
    uint32_t timestamp_ms;         /* HAL_GetTick() at capture time    */
    uint8_t trunk;                 /* physical bus number (silkscreen) */
    uint8_t dir;                   /* SB_DIR_*                    */
    uint8_t bit_len;               /* number of valid symbols in bits  */
    char bits[MAX_BIT_STRING + 1]; /* raw bit string, NUL-terminated   */
} sb_ring_frame_t;

/* ─── Ring buffer (singleton) ───────────────────────────────────────────── */

typedef struct {
    sb_ring_frame_t frames[RING_SIZE];
    volatile uint32_t head; /* next write index (producer only) */
} sb_ring_t;

/* ─── Consumer handle ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t tail; /* private read index               */
} sb_ring_reader_t;

/* ─── Global instance ───────────────────────────────────────────────────── */

extern sb_ring_t g_sb_ring;

/* ─── API ───────────────────────────────────────────────────────────────── */

/**
 * Initialise the ring buffer. Call once at startup.
 */
static inline void sb_ring_init(sb_ring_t* r) {
    memset(r, 0, sizeof(*r));
}

/**
 * Push a frame into the ring. Always succeeds.
 * Safe to call from any task (not from an ISR that may preempt another push
 * of the same ring): interrupts are masked for the duration of the copy.
 */
static inline void sb_ring_push(sb_ring_t* r, uint32_t ts_ms,
                                     uint8_t trunk, uint8_t dir,
                                     const char* bit_str, uint8_t bit_len) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    uint32_t idx = r->head & RING_MASK;
    sb_ring_frame_t* f = &r->frames[idx];

    f->timestamp_ms = ts_ms;
    f->trunk = trunk;
    f->dir = dir;
    f->bit_len = bit_len;

    uint8_t copy_len = (bit_len <= MAX_BIT_STRING) ? bit_len : MAX_BIT_STRING;
    memcpy(f->bits, bit_str, copy_len);
    f->bits[copy_len] = '\0';

    /* Memory barrier before publishing */
    __DMB();
    r->head++;

    __set_PRIMASK(primask);
}

/**
 * Initialise a consumer. Starts at the current head (no backlog).
 */
static inline void sb_ring_reader_init(sb_ring_reader_t* c,
                                         const sb_ring_t* r) {
    c->tail = r->head;
}

/**
 * Initialise a consumer with full backlog (reads from oldest available).
 */
static inline void sb_ring_reader_init_full(sb_ring_reader_t* c,
                                              const sb_ring_t* r) {
    uint32_t head = r->head;
    if (head > RING_SIZE)
        c->tail = head - RING_SIZE;
    else
        c->tail = 0;
}

/**
 * Check if the consumer has unread frames.
 */
static inline int sb_ring_reader_available(const sb_ring_reader_t* c,
                                             const sb_ring_t* r) {
    return (c->tail != r->head);
}

/**
 * Read the next frame. Returns NULL if no frames available.
 * The returned pointer is valid until the ring wraps (RING_SIZE pushes).
 */
static inline const sb_ring_frame_t* sb_ring_read(
    sb_ring_reader_t* c, const sb_ring_t* r) {
    if (c->tail == r->head) return NULL;

    /* Check for overrun: if head has lapped us, fast-forward */
    if ((r->head - c->tail) > RING_SIZE) c->tail = r->head - RING_SIZE;

    uint32_t idx = c->tail & RING_MASK;
    __DMB();
    const sb_ring_frame_t* f = &r->frames[idx];
    c->tail++;
    return f;
}

#ifdef __cplusplus
}
#endif

#endif /* SB_RING_H */