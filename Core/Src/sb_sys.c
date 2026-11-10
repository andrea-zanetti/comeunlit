/**
 * @file    sb_sys.c
 * @brief   System facts for GET /api/v1/health. See sb_sys.h.
 */

#include "sb_sys.h"

#include "FreeRTOS.h"
#include "board_pins.h"
#include "http_sse.h"
#include "stm32h5xx_hal.h"

uint32_t sb_sys_uptime_ms(void) { return HAL_GetTick(); }

uint32_t sb_sys_heap_free(void) { return (uint32_t)xPortGetFreeHeapSize(); }

int sb_sys_streams(void) { return http_sse_stream_count(); }

const char* sb_sys_shield(void) { return SB_SHIELD_REV; }
