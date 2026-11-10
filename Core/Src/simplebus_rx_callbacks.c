/**
 * @file    simplebus_rx_callbacks.c
 * @brief   TIM1 input capture callback: forwards each edge to the receiver.
 *
 *          Passes the timer channel number (1..4); simplebus_rx_edge() maps
 *          it to a bus using the generated table, so this file never needs
 *          to change when the shield wiring does.
 */

#include <stdint.h>

#include "board_pins.h"
#include "simplebus_rx.h"
#include "stm32h5xx_hal.h"

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef* htim) {
    if (htim->Instance != SB_RX_TIM) return;

    switch (htim->Channel) {
        case HAL_TIM_ACTIVE_CHANNEL_1:
            simplebus_rx_edge(1,
                              HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1));
            break;
        case HAL_TIM_ACTIVE_CHANNEL_2:
            simplebus_rx_edge(2,
                              HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_2));
            break;
        case HAL_TIM_ACTIVE_CHANNEL_3:
            simplebus_rx_edge(3,
                              HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_3));
            break;
        case HAL_TIM_ACTIVE_CHANNEL_4:
            simplebus_rx_edge(4,
                              HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_4));
            break;
        default:
            break;
    }
}