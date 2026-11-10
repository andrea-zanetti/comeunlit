/**
 * @file    sb_blind.h
 * @brief   Blind control on top of sb_bus: read, command, batch.
 *
 *          Stateless: every operation polls the module first and decides
 *          from what it reads. Rules (Comelit 20004606, verified):
 *
 *            current  up         down       stop
 *            idle     press UP   press DOWN nothing
 *            up       nothing    reverse    press DOWN
 *            down     reverse    nothing    press UP
 *
 *          A press opposite to the current motion stops the blind. Reverse =
 *          that stopping press, SB_REVERSE_PAUSE_MS, then the press again.
 *          Each press is checked against the module's ack (type 0x00, in =
 *          button, out = new relay state).
 *
 *          No automatic retries (API.md 5.2): a press is not idempotent.
 */

#ifndef SB_BLIND_H
#define SB_BLIND_H

#include <stdint.h>

#include "sb_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Errors on top of sb_bus.h's SB_E_* */
#define SB_E_NO_ACK (-10)   /* press sent, no ack                         */
#define SB_E_STATE (-11)    /* module reports an out value we don't know   */
#define SB_E_MISMATCH (-12) /* ack reports another state than expected     */

#ifndef SB_REVERSE_PAUSE_MS
#define SB_REVERSE_PAUSE_MS 500
#endif

#define SB_BATCH_MAX 16

typedef enum { SB_CMD_UP = 0, SB_CMD_DOWN, SB_CMD_STOP } sb_cmd_t;
typedef enum { SB_ACT_NONE = 0, SB_ACT_PRESS, SB_ACT_REVERSE } sb_action_t;
typedef enum { SB_DONE_NONE = 0, SB_DONE_STOP, SB_DONE_ALL } sb_done_t;

typedef struct {
    uint8_t bus;          /* in: target                                  */
    uint8_t addr;         /* in: target                                  */
    int err;              /* SB_OK or SB_E_*                             */
    uint8_t have_before;  /* poll succeeded                              */
    uint8_t before;       /* out byte from the poll                      */
    uint8_t have_after;   /* at least one ack received                   */
    uint8_t after;        /* out byte from the last ack (else = before)  */
    sb_action_t action;   /* what was decided                            */
    sb_done_t done;       /* how far it got                              */
} sb_blind_result_t;

/** "idle", "up", "down" or "unknown". */
const char* sb_state_name(uint8_t out);

/** Poll one blind. Returns SB_OK (out filled in) or SB_E_*. */
int sb_blind_read(uint8_t bus, uint8_t addr, uint8_t* out);

/** Poll, decide, act on one blind. r->bus and r->addr are set from args. */
void sb_blind_command(uint8_t bus, uint8_t addr, sb_cmd_t cmd,
                      sb_blind_result_t* r);

/**
 * Synchronized command on n targets (r[i].bus, r[i].addr set by caller,
 * n <= SB_BATCH_MAX, no duplicates): poll all, all stop presses, one shared
 * pause if any reversal is pending, all direction presses.
 * Every involved bus is held for the whole batch.
 */
void sb_blind_batch(sb_cmd_t cmd, sb_blind_result_t* r, uint8_t n);

#ifdef __cplusplus
}
#endif

#endif /* SB_BLIND_H */
