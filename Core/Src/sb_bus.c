/**
 * @file    sb_bus.c
 * @brief   SimpleBus bus access: per-bus locks and transactions.
 *          See sb_bus.h.
 */

#include "sb_bus.h"

#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "cmsis_os2.h"
#include "simplebus_tx.h"
#include "sb_ring.h"
#include "stm32h5xx_hal.h"

/* Polling period of the reply wait. Replies are published by the receiver
 * task; this only bounds how late we notice one. */
#define REPLY_POLL_MS 2

static osMutexId_t bus_mutex[SB_NUM_BUSES];

/* Tick when the last transaction on each bus ended (reply or timeout). */
static uint32_t quiet_since[SB_NUM_BUSES];

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t ms_to_ticks(uint32_t ms) {
    uint32_t t = (uint32_t)(((uint64_t)ms * osKernelGetTickFreq()) / 1000U);
    return t ? t : 1;
}

void sb_bus_sleep_ms(uint32_t ms) {
    if (ms) osDelay(ms_to_ticks(ms));
}

/* ─── Locks ─────────────────────────────────────────────────────────────── */

void sb_bus_init(void) {
    static const osMutexAttr_t attr = {
        .name = "sb_bus",
        .attr_bits = osMutexPrioInherit,
    };
    for (int i = 0; i < SB_NUM_BUSES; i++) {
        bus_mutex[i] = osMutexNew(&attr);
        quiet_since[i] = 0;
        if (bus_mutex[i] == NULL)
            printf("[BUS] mutex for bus %u failed\r\n", sb_bus_pins[i].id);
    }
}

int sb_bus_fitted(uint8_t bus) { return sb_bus_index(bus) >= 0; }

int sb_bus_count(void) { return SB_NUM_BUSES; }

uint8_t sb_bus_id_at(int i) {
    return (i >= 0 && i < SB_NUM_BUSES) ? sb_bus_pins[i].id : 0;
}

const char* sb_bus_term_at(int i) {
    return (i >= 0 && i < SB_NUM_BUSES) ? sb_bus_pins[i].term : "";
}

int sb_bus_lock(uint8_t bus) {
    int i = sb_bus_index(bus);
    if (i < 0) return SB_E_NOBUS;
    if (bus_mutex[i] == NULL) return SB_E_LOCK;
    if (osMutexAcquire(bus_mutex[i], ms_to_ticks(SB_LOCK_TIMEOUT_MS)) != osOK)
        return SB_E_LOCK;
    return SB_OK;
}

void sb_bus_unlock(uint8_t bus) {
    int i = sb_bus_index(bus);
    if (i < 0 || bus_mutex[i] == NULL) return;
    osMutexRelease(bus_mutex[i]);
}

/* ─── Transaction ───────────────────────────────────────────────────────── */

static int matches(const sb_ring_frame_t* fr, uint8_t bus, uint8_t type,
                   uint8_t addr, int in_match, sb_frame_t* out) {
    if (fr->trunk != bus || fr->dir != SB_DIR_RX) return 0;

    sb_frame_t f;
    if (!sb_frame_decode(fr->bits, fr->bit_len, &f)) return 0;
    if (!sb_frame_is_std(&f)) return 0;
    if (f.bytes[SB_B_TYPE] != type || f.bytes[SB_B_ADDR] != addr) return 0;
    if (in_match >= 0 && f.bytes[SB_B_IN] != (uint8_t)in_match) return 0;

    *out = f;
    return 1;
}

int sb_bus_transact(uint8_t bus, const uint8_t req[4], uint8_t reply_type,
                    int in_match, sb_frame_t* reply) {
    int i = sb_bus_index(bus);
    if (i < 0) return SB_E_NOBUS;
    if (req == NULL || reply == NULL) return SB_E_ARG;

    /* Respect the quiet time after the previous transaction. */
    uint32_t gap = ms_to_ticks(SB_INTERFRAME_MS);
    uint32_t since = osKernelGetTickCount() - quiet_since[i];
    if (since < gap) osDelay(gap - since);

    char bits[TX_MAX_BITS + 1];
    uint8_t bit_len = sb_frame_encode(req, 4, 1, bits, TX_MAX_BITS);

    /* Start reading the ring at the current head: anything received before
     * our frame (earlier replies, repeated announcements) is not ours. */
    sb_ring_reader_t c;
    sb_ring_reader_init(&c, &g_sb_ring);

    int rc = simplebus_tx_bits_locked(bus, bits, bit_len);
    if (rc != 0) {
        quiet_since[i] = osKernelGetTickCount();
        return (rc == -2) ? SB_E_BUS_BUSY : (rc == -3) ? SB_E_TX : SB_E_ARG;
    }

    uint32_t t0 = osKernelGetTickCount();
    uint32_t limit = ms_to_ticks(SB_REPLY_TIMEOUT_MS);
    int found = 0;

    for (;;) {
        const sb_ring_frame_t* fr;
        while ((fr = sb_ring_read(&c, &g_sb_ring)) != NULL) {
            if (matches(fr, bus, reply_type, req[SB_B_ADDR], in_match,
                        reply)) {
                found = 1;
                break;
            }
        }
        if (found) break;
        if (osKernelGetTickCount() - t0 >= limit) break;
        osDelay(ms_to_ticks(REPLY_POLL_MS));
    }

    quiet_since[i] = osKernelGetTickCount();
    return found ? SB_OK : SB_E_NO_REPLY;
}

/* ─── Raw transmit (admin) ──────────────────────────────────────────────── */

int sb_bus_raw(uint8_t bus, const char* bits, uint8_t bit_len) {
    int i = sb_bus_index(bus);
    if (i < 0) return SB_E_NOBUS;
    if (bits == NULL || bit_len == 0 || bit_len > TX_MAX_BITS) return SB_E_ARG;

    int rc = sb_bus_lock(bus);
    if (rc != SB_OK) return rc;

    uint32_t gap = ms_to_ticks(SB_INTERFRAME_MS);
    uint32_t since = osKernelGetTickCount() - quiet_since[i];
    if (since < gap) osDelay(gap - since);

    int tx = simplebus_tx_bits_locked(bus, bits, bit_len);
    if (tx == 0) {
        sb_bus_sleep_ms(SB_RAW_SETTLE_MS);
        rc = SB_OK;
    } else {
        rc = (tx == -2) ? SB_E_BUS_BUSY : (tx == -3) ? SB_E_TX : SB_E_ARG;
    }
    quiet_since[i] = osKernelGetTickCount();
    sb_bus_unlock(bus);
    return rc;
}
