/**
 * @file    sb_blind.c
 * @brief   Blind control: read, command, batch. See sb_blind.h.
 *
 *          Uses only sb_bus.h, so it builds on the host against a simulated
 *          bus (tests/host).
 */

#include "sb_blind.h"

#include <string.h>

#include "sb_frame.h"

/* ─── Helpers ───────────────────────────────────────────────────────────── */

const char* sb_state_name(uint8_t out) {
    switch (out) {
        case SB_OUT_IDLE: return "idle";
        case SB_OUT_UP: return "up";
        case SB_OUT_DOWN: return "down";
        default: return "unknown";
    }
}

static int known_state(uint8_t out) {
    return out == SB_OUT_IDLE || out == SB_OUT_UP || out == SB_OUT_DOWN;
}

static uint8_t opposite_btn(uint8_t out) {
    return (out == SB_OUT_UP) ? SB_BTN_DOWN : SB_BTN_UP;
}

/* Bus held by the caller. */
static int poll_locked(uint8_t bus, uint8_t addr, uint8_t* out) {
    const uint8_t req[4] = {SB_TYPE_POLL, addr, 0x00, 0x00};
    sb_frame_t reply;
    int rc = sb_bus_transact(bus, req, SB_TYPE_POLL_REPLY, -1, &reply);
    if (rc == SB_OK) *out = reply.bytes[SB_B_OUT];
    return rc;
}

/* Press `btn` and check the ack reports `expect`. Bus held by the caller.
 * Fills r->after/have_after from the ack when there is one. */
static int press_locked(sb_blind_result_t* r, uint8_t btn, uint8_t expect) {
    const uint8_t req[4] = {SB_TYPE_PRESS, r->addr, btn, btn};
    sb_frame_t ack;
    int rc = sb_bus_transact(r->bus, req, SB_TYPE_ACK, btn, &ack);
    if (rc == SB_E_NO_REPLY) return SB_E_NO_ACK;
    if (rc != SB_OK) return rc;

    r->have_after = 1;
    r->after = ack.bytes[SB_B_OUT];
    return (r->after == expect) ? SB_OK : SB_E_MISMATCH;
}

static void result_init(sb_blind_result_t* r, uint8_t bus, uint8_t addr) {
    memset(r, 0, sizeof(*r));
    r->bus = bus;
    r->addr = addr;
}

/* Decide from the polled state. Sets r->action, and the buttons/expected
 * states of the (up to two) presses. */
typedef struct {
    uint8_t stop_btn;   /* 0 = no stopping press */
    uint8_t dir_btn;    /* 0 = no direction press */
    uint8_t dir_expect;
} plan_t;

static plan_t decide(sb_blind_result_t* r, sb_cmd_t cmd) {
    plan_t p = {0, 0, 0};
    uint8_t cur = r->before;

    if (cmd == SB_CMD_STOP) {
        if (cur == SB_OUT_IDLE) {
            r->action = SB_ACT_NONE;
        } else {
            r->action = SB_ACT_PRESS;
            p.stop_btn = opposite_btn(cur);
        }
        return p;
    }

    uint8_t want = (cmd == SB_CMD_UP) ? SB_OUT_UP : SB_OUT_DOWN;
    uint8_t want_btn = (cmd == SB_CMD_UP) ? SB_BTN_UP : SB_BTN_DOWN;

    if (cur == want) {
        r->action = SB_ACT_NONE;
    } else if (cur == SB_OUT_IDLE) {
        r->action = SB_ACT_PRESS;
        p.dir_btn = want_btn;
        p.dir_expect = want;
    } else {
        /* Moving the other way: pressing want_btn is the opposite press,
         * so the first press stops it. */
        r->action = SB_ACT_REVERSE;
        p.stop_btn = want_btn;
        p.dir_btn = want_btn;
        p.dir_expect = want;
    }
    return p;
}

/* ─── Read ──────────────────────────────────────────────────────────────── */

int sb_blind_read(uint8_t bus, uint8_t addr, uint8_t* out) {
    int rc = sb_bus_lock(bus);
    if (rc != SB_OK) return rc;
    rc = poll_locked(bus, addr, out);
    sb_bus_unlock(bus);
    return rc;
}

/* ─── Single command ────────────────────────────────────────────────────── */

void sb_blind_command(uint8_t bus, uint8_t addr, sb_cmd_t cmd,
                      sb_blind_result_t* r) {
    plan_t p;
    result_init(r, bus, addr);

    int rc = sb_bus_lock(bus);
    if (rc != SB_OK) {
        r->err = rc;
        return;
    }

    rc = poll_locked(bus, addr, &r->before);
    if (rc != SB_OK) {
        r->err = rc;
        goto out;
    }
    r->have_before = 1;
    r->after = r->before;

    if (!known_state(r->before)) {
        r->err = SB_E_STATE;
        goto out;
    }

    p = decide(r, cmd);

    if (p.stop_btn) {
        rc = press_locked(r, p.stop_btn, SB_OUT_IDLE);
        if (rc != SB_OK) {
            r->err = rc;
            goto out;
        }
        r->done = SB_DONE_STOP;
        if (p.dir_btn) sb_bus_sleep_ms(SB_REVERSE_PAUSE_MS);
    }

    if (p.dir_btn) {
        rc = press_locked(r, p.dir_btn, p.dir_expect);
        if (rc != SB_OK) {
            r->err = rc;
            goto out;
        }
    }
    r->done = SB_DONE_ALL;

out:
    sb_bus_unlock(bus);
}

/* ─── Batch ─────────────────────────────────────────────────────────────── */

void sb_blind_batch(sb_cmd_t cmd, sb_blind_result_t* r, uint8_t n) {
    if (n > SB_BATCH_MAX) n = SB_BATCH_MAX;

    plan_t plan[SB_BATCH_MAX];
    uint8_t buses[SB_BATCH_MAX];
    uint8_t nbus = 0;

    for (uint8_t i = 0; i < n; i++) result_init(&r[i], r[i].bus, r[i].addr);

    /* Distinct fitted buses, ascending: lock order is fixed, so two
     * batches can never deadlock on each other. */
    for (uint8_t i = 0; i < n; i++) {
        if (!sb_bus_fitted(r[i].bus)) {
            r[i].err = SB_E_NOBUS;
            continue;
        }
        uint8_t k = 0;
        while (k < nbus && buses[k] < r[i].bus) k++;
        if (k < nbus && buses[k] == r[i].bus) continue;
        memmove(&buses[k + 1], &buses[k], (size_t)(nbus - k));
        buses[k] = r[i].bus;
        nbus++;
    }

    uint8_t locked = 0;
    for (; locked < nbus; locked++) {
        int rc = sb_bus_lock(buses[locked]);
        if (rc != SB_OK) {
            for (uint8_t i = 0; i < n; i++)
                if (r[i].err == SB_OK) r[i].err = rc;
            goto unlock;
        }
    }

    /* 1. Poll every target, decide. */
    for (uint8_t i = 0; i < n; i++) {
        memset(&plan[i], 0, sizeof(plan[i]));
        if (r[i].err != SB_OK) continue;

        int rc = poll_locked(r[i].bus, r[i].addr, &r[i].before);
        if (rc != SB_OK) {
            r[i].err = rc;
            continue;
        }
        r[i].have_before = 1;
        r[i].after = r[i].before;

        if (!known_state(r[i].before)) {
            r[i].err = SB_E_STATE;
            continue;
        }
        plan[i] = decide(&r[i], cmd);
        if (r[i].action == SB_ACT_NONE) r[i].done = SB_DONE_ALL;
    }

    /* 2. Every stopping press. */
    int pause = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (r[i].err != SB_OK || !plan[i].stop_btn) continue;
        int rc = press_locked(&r[i], plan[i].stop_btn, SB_OUT_IDLE);
        if (rc != SB_OK) {
            r[i].err = rc;
            continue;
        }
        r[i].done = SB_DONE_STOP;
        if (plan[i].dir_btn)
            pause = 1;
        else
            r[i].done = SB_DONE_ALL;
    }

    /* 3. One shared pause before any reversal completes. */
    if (pause) sb_bus_sleep_ms(SB_REVERSE_PAUSE_MS);

    /* 4. Every direction press. */
    for (uint8_t i = 0; i < n; i++) {
        if (r[i].err != SB_OK || !plan[i].dir_btn) continue;
        int rc = press_locked(&r[i], plan[i].dir_btn, plan[i].dir_expect);
        if (rc != SB_OK) {
            r[i].err = rc;
            continue;
        }
        r[i].done = SB_DONE_ALL;
    }

unlock:
    while (locked > 0) sb_bus_unlock(buses[--locked]);
}
