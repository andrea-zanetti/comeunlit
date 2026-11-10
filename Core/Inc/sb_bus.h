/**
 * @file    sb_bus.h
 * @brief   SimpleBus bus access: one user per bus at a time, and
 *          request/reply transactions.
 *
 *          Every user of a bus (blind commands, batches, the raw TX queue)
 *          takes that bus's lock first, so frames from different callers
 *          never interleave and a command's poll, stop, pause and press run
 *          back to back. Buses are addressed by PHYSICAL number.
 *
 *          A transaction sends one 4-byte STD frame (checksum appended) and
 *          waits for the module's reply by reading the frame ring, where
 *          the receiver already publishes every frame. The receiver is not
 *          touched: replies arrive as fast as the receiver task processes
 *          them.
 */

#ifndef SB_BUS_H
#define SB_BUS_H

#include <stdint.h>

#include "sb_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Errors (shared with sb_blind) ─────────────────────────────────────── */

#define SB_OK 0
#define SB_E_NOBUS (-1)    /* bus not fitted on this shield               */
#define SB_E_BUS_BUSY (-2) /* listen-before-talk gave up, nothing sent     */
#define SB_E_NO_REPLY (-3) /* no matching reply within SB_REPLY_TIMEOUT_MS */
#define SB_E_LOCK (-4)     /* bus not available within SB_LOCK_TIMEOUT_MS  */
#define SB_E_ARG (-5)      /* bad argument                                 */
#define SB_E_TX (-6)       /* transmitter (timer/DMA) failed; frame not sent */

/* ─── Timing ────────────────────────────────────────────────────────────── */

/* Reply wait after our frame. Measured: end of our frame to the reply being
 * published is ~28 ms (1.5 ms turnaround + 26 ms reply + receiver). */
#ifndef SB_REPLY_TIMEOUT_MS
#define SB_REPLY_TIMEOUT_MS 150
#endif

/* Quiet time on the bus between the end of one transaction (reply received
 * or timed out) and our next frame; modules may still be finishing their own
 * traffic. 40 ms has proven reliable. */
#ifndef SB_INTERFRAME_MS
#define SB_INTERFRAME_MS 40
#endif

#ifndef SB_LOCK_TIMEOUT_MS
#define SB_LOCK_TIMEOUT_MS 5000
#endif

/* ─── API ───────────────────────────────────────────────────────────────── */

/** Create the per-bus locks. Called by simplebus_tx_init(). */
void sb_bus_init(void);

/** 1 if the bus is fitted on this shield, 0 otherwise. */
int sb_bus_fitted(uint8_t bus);

/** Fitted buses, in table order: count, physical id and terminal name. */
int sb_bus_count(void);
uint8_t sb_bus_id_at(int i);
const char* sb_bus_term_at(int i);

/** Take a bus. Returns SB_OK, SB_E_NOBUS or SB_E_LOCK. */
int sb_bus_lock(uint8_t bus);

/** Release a bus taken with sb_bus_lock(). */
void sb_bus_unlock(uint8_t bus);

/**
 * Send `req` (4 bytes, checksum appended) on a bus the caller holds, then
 * wait for a valid STD frame from the module with type `reply_type`, the
 * same address as req[1] and, if in_match >= 0, in byte == in_match.
 *
 * Returns SB_OK (reply filled in), SB_E_BUS_BUSY, SB_E_NO_REPLY,
 * SB_E_NOBUS or SB_E_ARG.
 */
int sb_bus_transact(uint8_t bus, const uint8_t req[4], uint8_t reply_type,
                    int in_match, sb_frame_t* reply);

/* Raw frames: hold the bus this long after sending, so a module's reply
 * (~61 ms later) is not talked over by the next user of the bus. */
#ifndef SB_RAW_SETTLE_MS
#define SB_RAW_SETTLE_MS 150
#endif

/**
 * Admin raw transmit (API.md 6.2): take the bus, respect the quiet time,
 * send `bits` as given, keep the bus SB_RAW_SETTLE_MS, release.
 * Returns SB_OK (on the wire), SB_E_BUS_BUSY, SB_E_NOBUS, SB_E_LOCK or
 * SB_E_ARG.
 */
int sb_bus_raw(uint8_t bus, const char* bits, uint8_t bit_len);

/** Sleep for ms (task context). */
void sb_bus_sleep_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* SB_BUS_H */
