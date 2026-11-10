/* Host stub of the generated pin map: buses 2, 3, 4 on TIM1 CH1, CH2, CH4
 * (shield rev 1). Only what simplebus_rx.c uses. */
#ifndef HOST_STUB_BOARD_PINS_H
#define HOST_STUB_BOARD_PINS_H
#include "stm32h5xx_hal.h"
#define SB_NUM_BUSES 3
#define SB_RX_TIM_AF 1u
#define SB_GPIO_CLK_ENABLE_ALL() do { } while (0)
typedef struct { GPIO_TypeDef* port; uint16_t pin; } sb_gpio_t;
typedef struct {
    uint8_t id; const char* term; sb_gpio_t rx, data, oe; uint32_t rx_tim_channel;
} sb_bus_pins_t;
static const sb_bus_pins_t sb_bus_pins[SB_NUM_BUSES] = {
    {2, "J6", {0, 1}, {0, 2}, {0, 4}, 0},
    {3, "J7", {0, 8}, {0, 16}, {0, 32}, 4},
    {4, "J8", {0, 64}, {0, 128}, {0, 256}, 12},
};
static const int8_t sb_timch_to_idx[4] = {0, 1, -1, 2};
#endif
