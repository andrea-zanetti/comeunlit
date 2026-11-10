/**
 * @file    sb_sys.h
 * @brief   System facts reported by GET /api/v1/health.
 *
 *          Kept behind functions so api_v1.c builds on the host
 *          (tests/host stubs them).
 */

#ifndef SB_SYS_H
#define SB_SYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware version reported by /health. Bump on releases. */
#define SB_FW_VERSION "0.1.0"

uint32_t sb_sys_uptime_ms(void);
uint32_t sb_sys_heap_free(void);
int sb_sys_streams(void);         /* open SSE streams */
const char* sb_sys_shield(void);  /* shield revision, e.g. "1" */

#ifdef __cplusplus
}
#endif

#endif /* SB_SYS_H */
