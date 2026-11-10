/**
 * @file    sb_stream.c
 * @brief   Signed SSE streams. See sb_stream.h.
 */

#include "sb_stream.h"

#include <stdio.h>
#include <string.h>

#include "sb_blind.h"
#include "sb_frame.h"

void sb_stream_init(sb_stream_t* s, sb_stream_type_t type,
                    const char* req_mac_hex) {
    memset(s, 0, sizeof(*s));
    s->type = type;
    memcpy(s->req_mac, req_mac_hex, SB_AUTH_MAC_HEX);
    s->req_mac[SB_AUTH_MAC_HEX] = '\0';
}

/* ─── Formatting ────────────────────────────────────────────────────────── */

static const char* dir_name(uint8_t dir) {
    switch (dir) {
        case SB_DIR_TX: return "tx";
        case SB_DIR_DROP: return "drop";
        default: return "rx";
    }
}

static const char* button_name(uint8_t in) {
    switch (in) {
        case SB_BTN_UP: return "up";
        case SB_BTN_DOWN: return "down";
        default: return "none";
    }
}

int sb_stream_frame_json(const sb_ring_frame_t* f, char* out, int out_sz) {
    int n = snprintf(out, out_sz,
                     "{\"ts\":%lu,\"bus\":%u,\"dir\":\"%s\",\"bits\":\"%.*s\"",
                     (unsigned long)f->timestamp_ms, f->trunk,
                     dir_name(f->dir), f->bit_len, f->bits);

    sb_frame_t d;
    if (n < out_sz && sb_frame_decode(f->bits, f->bit_len, &d)) {
        n += snprintf(out + n, out_sz - n, ",\"proto\":\"%s\",\"hex\":\"",
                      sb_proto_label(d.proto));
        for (int i = 0; i < d.n && n < out_sz; i++)
            n += snprintf(out + n, out_sz - n, "%s%02X", i ? " " : "",
                          d.bytes[i]);
        if (n < out_sz)
            n += snprintf(out + n, out_sz - n, "\",\"chk_ok\":%s",
                          d.chk_ok ? "true" : "false");
    }
    if (n < out_sz) n += snprintf(out + n, out_sz - n, "}");
    if (n >= out_sz) return -1;
    return n;
}

/* Assemble "event/id/data" with the MAC. */
static int sse(sb_stream_t* s, const char* event, const char* json, int jlen,
               char* out, int out_sz) {
    uint32_t seq = s->seq + 1;
    char mac[SB_AUTH_MAC_HEX + 1];
    if (sb_auth_sign_event(s->req_mac, seq, event, json, (uint32_t)jlen,
                           s->scratch, sizeof(s->scratch), mac) != 0)
        return -1;
    int n = snprintf(out, out_sz, "event: %s\nid: %lu\ndata: %s %.*s\n\n",
                     event, (unsigned long)seq, mac, jlen, json);
    if (n >= out_sz) return -1;
    s->seq = seq;
    return n;
}

/* ─── Recent-frame memory ───────────────────────────────────────────────── */

static void remember(sb_stream_recent_t* tab, uint8_t bus, uint8_t addr,
                     uint8_t in, uint8_t out, uint32_t t) {
    /* Replace the same key, else the oldest (or a free) slot. */
    int victim = 0;
    for (int i = 0; i < SB_STREAM_RECENT; i++) {
        if (tab[i].used && tab[i].bus == bus && tab[i].addr == addr) {
            victim = i;
            break;
        }
        if (!tab[i].used) {
            victim = i;
            break;
        }
        if (tab[i].t < tab[victim].t) victim = i;
    }
    tab[victim].used = 1;
    tab[victim].bus = bus;
    tab[victim].addr = addr;
    tab[victim].in = in;
    tab[victim].out = out;
    tab[victim].t = t;
}

static const sb_stream_recent_t* lookup(const sb_stream_recent_t* tab,
                                        uint8_t bus, uint8_t addr) {
    for (int i = 0; i < SB_STREAM_RECENT; i++)
        if (tab[i].used && tab[i].bus == bus && tab[i].addr == addr)
            return &tab[i];
    return NULL;
}

/* ─── Feed ──────────────────────────────────────────────────────────────── */

int sb_stream_frame(sb_stream_t* s, const sb_ring_frame_t* f, char* out,
                    int out_sz) {
    if (s->type == SB_STREAM_FRAMES) {
        char json[SB_STREAM_LINE_MAX - 128];
        int j = sb_stream_frame_json(f, json, sizeof(json));
        if (j < 0) return 0; /* cannot happen with MAX_BIT_STRING; skip */
        return sse(s, "frame", json, j, out, out_sz);
    }

    /* Events stream */
    sb_frame_t d;
    if (!sb_frame_decode(f->bits, f->bit_len, &d) || !sb_frame_is_std(&d))
        return 0;

    uint8_t type = d.bytes[SB_B_TYPE], addr = d.bytes[SB_B_ADDR];
    uint8_t in = d.bytes[SB_B_IN], o = d.bytes[SB_B_OUT];
    uint32_t t = f->timestamp_ms;

    if (f->dir == SB_DIR_TX) {
        /* Our own press: remember it to attribute the ack. */
        if (type == SB_TYPE_PRESS) remember(s->presses, f->trunk, addr, in, o, t);
        return 0;
    }
    if (f->dir != SB_DIR_RX || type != SB_TYPE_ACK) return 0;

    /* Repeat filter: identical announcement within SB_STREAM_DEDUP_MS. */
    const sb_stream_recent_t* prev = lookup(s->seen, f->trunk, addr);
    if (prev && prev->in == in && prev->out == o &&
        (uint32_t)(t - prev->t) < SB_STREAM_DEDUP_MS)
        return 0;
    remember(s->seen, f->trunk, addr, in, o, t);

    const sb_stream_recent_t* p = lookup(s->presses, f->trunk, addr);
    int ours = p && p->in == in && (uint32_t)(t - p->t) <= SB_STREAM_SOURCE_MS;

    char json[160];
    int j = snprintf(json, sizeof(json),
                     "{\"bus\":%u,\"addr\":%u,\"state\":\"%s\",\"button\":\"%s\","
                     "\"source\":\"%s\"}",
                     f->trunk, addr, sb_state_name(o), button_name(in),
                     ours ? "bridge" : "local");
    return sse(s, "state", json, j, out, out_sz);
}

int sb_stream_heartbeat(sb_stream_t* s, uint32_t uptime_ms, char* out,
                        int out_sz) {
    char json[40];
    int j = snprintf(json, sizeof(json), "{\"uptime_ms\":%lu}",
                     (unsigned long)uptime_ms);
    return sse(s, "hb", json, j, out, out_sz);
}
