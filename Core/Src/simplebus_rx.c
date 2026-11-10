/**
 * @file    simplebus_rx.c
 * @brief   SimpleBus multi-bus receiver.
 *
 *          ISR path:  HAL_TIM_IC_CaptureCallback → simplebus_rx_edge()
 *                     maps timer channel → bus, collects inter-edge deltas.
 *
 *          Task path: simplebus_rx_process() (receiver task)
 *                     converts deltas → bit string, pushes to ring buffer
 *                     tagged with the physical bus number.
 *
 *          Pins and timer channels come from the generated board_pins.h.
 */

#include "simplebus_rx.h"

#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "sb_ring.h"

/* ─── Timing thresholds (microseconds, matches TIM1 prescaler) ──────────── */

#define RX_FRAME_EDGES 60
#define THRESHOLD_US 420
#define GAP_THRESHOLD_US 1000
#define GLITCH_US 100

/* ─── Per-bus ISR state (index = internal table index) ──────────────────── */

typedef struct {
    volatile uint16_t captures[RX_FRAME_EDGES];
    volatile uint32_t edge_count;
    volatile uint16_t last_capture;
    volatile uint8_t frame_ready;
    volatile uint8_t muted; /* our own transmission in progress */
} rx_bus_t;

static rx_bus_t rx_bus[SB_NUM_BUSES];

/* ─── GPIO + timer init ─────────────────────────────────────────────────── */

void simplebus_rx_init(TIM_HandleTypeDef* htim) {
    memset(rx_bus, 0, sizeof(rx_bus));

    SB_GPIO_CLK_ENABLE_ALL();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = SB_RX_TIM_AF;

    for (int i = 0; i < SB_NUM_BUSES; i++) {
        gpio.Pin = sb_bus_pins[i].rx.pin;
        HAL_GPIO_Init(sb_bus_pins[i].rx.port, &gpio);
    }

    for (int i = 0; i < SB_NUM_BUSES; i++) {
        HAL_TIM_IC_Start_IT(htim, sb_bus_pins[i].rx_tim_channel);
    }
}

/* ─── ISR entry point ───────────────────────────────────────────────────── */

void simplebus_rx_edge(uint8_t tim_ch, uint16_t timestamp) {
    if (tim_ch < 1 || tim_ch > 4) return;
    int idx = sb_timch_to_idx[tim_ch - 1];
    if (idx < 0) return;

    rx_bus_t* b = &rx_bus[idx];

    /* Our own frame: not a reception. Restart from scratch afterwards. */
    if (b->muted) {
        b->edge_count = 0;
        return;
    }

    /* Don't overwrite a frame the task hasn't consumed yet */
    if (b->frame_ready) return;

    if (b->edge_count == 0) {
        b->last_capture = timestamp;
        b->edge_count = 1;
        return;
    }

    uint16_t delta = timestamp - b->last_capture;

    /* Bounce/ringing filter: an edge closer than GLITCH_US to the previous
     * one is not a symbol boundary. Drop it WITHOUT moving last_capture, so
     * the next real edge is still measured from the previous real one (a
     * glitch must not shorten the following symbol). */
    if (delta < GLITCH_US) return;
    b->last_capture = timestamp;

    if (delta > GAP_THRESHOLD_US) {
        /* Gap → new frame starts */
        b->edge_count = 1;
        return;
    }

    uint32_t k = b->edge_count - 1;
    if (k < RX_FRAME_EDGES - 1) {
        b->captures[k] = delta;
    }

    b->edge_count++;

    if (b->edge_count >= RX_FRAME_EDGES) {
        b->frame_ready = 1;
        b->edge_count = 0;
    }
}

/* ─── Mute (transmitter) ────────────────────────────────────────────────── */

void simplebus_rx_mute(int idx, int on) {
    if (idx < 0 || idx >= SB_NUM_BUSES) return;
    rx_bus_t* b = &rx_bus[idx];
    /* Set the flag first: from here on the ISR ignores this bus, so
     * resetting the count cannot race with an edge being stored. */
    b->muted = 1;
    b->edge_count = 0;
    if (!on) b->muted = 0;
}

/* ─── Frame processing (task context) ───────────────────────────────────── */

int simplebus_rx_process(void) {
    int pushed = 0;

    for (int i = 0; i < SB_NUM_BUSES; i++) {
        rx_bus_t* b = &rx_bus[i];

        if (!b->frame_ready) continue;

        /* Snapshot deltas from volatile buffer */
        uint16_t deltas[RX_FRAME_EDGES - 1];
        memcpy(deltas, (const void*)b->captures, sizeof(deltas));

        /* Convert to bit string */
        char bits[RX_FRAME_EDGES];
        uint8_t bit_len = RX_FRAME_EDGES - 1;

        for (int k = 0; k < bit_len; k++) {
            bits[k] = (deltas[k] < THRESHOLD_US) ? '1' : '0';
        }
        bits[bit_len] = '\0';

        /* Push to ring buffer, tagged with the physical bus number */
        sb_ring_push(&g_sb_ring, HAL_GetTick(), sb_bus_pins[i].id,
                          SB_DIR_RX, bits, bit_len);

        /* Release the bus for the next frame */
        b->frame_ready = 0;
        pushed++;
    }

    return pushed;
}
