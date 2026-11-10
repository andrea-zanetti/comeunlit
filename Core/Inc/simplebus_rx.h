/**
 * @file    simplebus_rx.h
 * @brief   SimpleBus multi-bus receiver: edge capture + frame assembly.
 *
 *          Receivers and their timer channels come from the generated
 *          board_pins.h. Frames are pushed to the frame ring tagged with
 *          the PHYSICAL bus number (sb_ring_frame_t.trunk).
 */

#ifndef SIMPLEBUS_RX_H
#define SIMPLEBUS_RX_H

#include <stdint.h>

#include "stm32h5xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure every fitted bus's RX pin as a timer input (AF) and start
 * input capture with interrupts on its channel. Call after MX_TIM1_Init().
 */
void simplebus_rx_init(TIM_HandleTypeDef* htim);

/**
 * Called from HAL_TIM_IC_CaptureCallback: one edge on one timer channel.
 * tim_ch is the channel number 1..4. Runs in ISR context; must be fast.
 * Edges on channels with no bus assigned are ignored.
 */
void simplebus_rx_edge(uint8_t tim_ch, uint16_t timestamp);

/**
 * Mute (on = 1) or unmute (on = 0) the receiver of the bus at internal table
 * index idx (0..SB_NUM_BUSES-1). A muted receiver ignores every edge and
 * drops any partly received frame; unmuting starts fresh. The transmitter
 * mutes a bus for the length of its own frame. A completed frame still
 * waiting for simplebus_rx_process() is kept.
 */
void simplebus_rx_mute(int idx, int on);

/**
 * Call from the receiver task. Checks every bus for a completed frame,
 * converts deltas to bits and pushes to the frame ring.
 * Returns the number of frames pushed.
 */
int simplebus_rx_process(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLEBUS_RX_H */
