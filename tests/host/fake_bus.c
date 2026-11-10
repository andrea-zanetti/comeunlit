#include "fake_bus.h"

#include <stdio.h>
#include <string.h>

#include "sb_bus.h"

static fake_module_t mods[FAKE_MAX_MODULES];
static int nmods;
static int busy[100];
static int locked[100];
char fake_log[4096];

static void logf_(const char* s) {
    strncat(fake_log, s, sizeof(fake_log) - strlen(fake_log) - 1);
    strncat(fake_log, "; ", sizeof(fake_log) - strlen(fake_log) - 1);
}

void fake_reset(void) {
    memset(mods, 0, sizeof(mods));
    nmods = 0;
    memset(busy, 0, sizeof(busy));
    memset(locked, 0, sizeof(locked));
    fake_log[0] = '\0';
}

fake_module_t* fake_add(uint8_t bus, uint8_t addr, uint8_t out) {
    fake_module_t* m = &mods[nmods++];
    m->bus = bus;
    m->addr = addr;
    m->out = out;
    m->present = 1;
    return m;
}

void fake_set_busy(uint8_t bus, int b) { busy[bus] = b; }

static fake_module_t* find(uint8_t bus, uint8_t addr) {
    for (int i = 0; i < nmods; i++)
        if (mods[i].bus == bus && mods[i].addr == addr && mods[i].present)
            return &mods[i];
    return NULL;
}

/* sb_bus.h ---------------------------------------------------------------- */

void sb_bus_init(void) {}
int sb_bus_fitted(uint8_t bus) { return bus == 2 || bus == 3 || bus == 4; }
int sb_bus_count(void) { return 3; }
uint8_t sb_bus_id_at(int i) { return (uint8_t)(2 + i); }
const char* sb_bus_term_at(int i) {
    static const char* t[] = {"J6", "J7", "J8"};
    return (i >= 0 && i < 3) ? t[i] : "";
}

int sb_bus_lock(uint8_t bus) {
    if (!sb_bus_fitted(bus)) return SB_E_NOBUS;
    if (locked[bus]) {
        logf_("DOUBLE-LOCK");
        return SB_E_LOCK;
    }
    locked[bus] = 1;
    return SB_OK;
}

void sb_bus_unlock(uint8_t bus) {
    if (!locked[bus]) logf_("UNLOCK-NOT-HELD");
    locked[bus] = 0;
}

void sb_bus_sleep_ms(uint32_t ms) {
    char b[32];
    snprintf(b, sizeof(b), "sleep %u", ms);
    logf_(b);
}

static void make_reply(sb_frame_t* f, uint8_t type, uint8_t addr, uint8_t in,
                       uint8_t out) {
    memset(f, 0, sizeof(*f));
    f->proto = SB_PROTO_STD;
    f->n = 5;
    f->bytes[0] = type;
    f->bytes[1] = addr;
    f->bytes[2] = in;
    f->bytes[3] = out;
    f->bytes[4] = (uint8_t)(0x55 + type + addr + in + out);
    f->chk_ok = 1;
}

int sb_bus_transact(uint8_t bus, const uint8_t req[4], uint8_t reply_type,
                    int in_match, sb_frame_t* reply) {
    char b[64];
    if (!locked[bus]) logf_("TX-WITHOUT-LOCK");
    if (busy[bus]) {
        snprintf(b, sizeof(b), "busy %u:%u", bus, req[1]);
        logf_(b);
        return SB_E_BUS_BUSY;
    }
    fake_module_t* m = find(bus, req[1]);

    if (req[0] == SB_TYPE_POLL) {
        snprintf(b, sizeof(b), "poll %u:%u", bus, req[1]);
        logf_(b);
        if (!m) return SB_E_NO_REPLY;
        make_reply(reply, SB_TYPE_POLL_REPLY, req[1], 0, m->out);
        return (reply_type == SB_TYPE_POLL_REPLY) ? SB_OK : SB_E_NO_REPLY;
    }

    if (req[0] == SB_TYPE_PRESS) {
        uint8_t btn = req[2];
        snprintf(b, sizeof(b), "press %u:%u btn=%u", bus, req[1], btn);
        logf_(b);
        if (!m) return SB_E_NO_REPLY;
        m->presses++;
        /* Module behaviour as verified: idle -> start; opposite of the
         * motion -> stop; same direction -> nothing changes. */
        if (!m->stuck) {
            if (m->out == SB_OUT_IDLE)
                m->out = (btn == SB_BTN_UP) ? SB_OUT_UP : SB_OUT_DOWN;
            else if ((m->out == SB_OUT_UP && btn == SB_BTN_DOWN) ||
                     (m->out == SB_OUT_DOWN && btn == SB_BTN_UP))
                m->out = SB_OUT_IDLE;
        }
        if (m->drop_ack_at == m->presses) return SB_E_NO_REPLY;
        make_reply(reply, SB_TYPE_ACK, req[1], btn, m->out);
        if (reply_type != SB_TYPE_ACK) return SB_E_NO_REPLY;
        if (in_match >= 0 && in_match != btn) return SB_E_NO_REPLY;
        return SB_OK;
    }
    return SB_E_ARG;
}

int sb_bus_raw(uint8_t bus, const char* bits, uint8_t bit_len) {
    char b[96];
    if (!sb_bus_fitted(bus)) return SB_E_NOBUS;
    if (busy[bus]) {
        snprintf(b, sizeof(b), "raw-busy %u", bus);
        logf_(b);
        return SB_E_BUS_BUSY;
    }
    snprintf(b, sizeof(b), "raw %u n=%u %.20s", bus, bit_len, bits);
    logf_(b);
    return SB_OK;
}
