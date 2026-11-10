/**
 * @file    sb_auth.c
 * @brief   API v1 request authentication and response signing.
 *          See sb_auth.h and docs/API.md section 3.
 */

#include "sb_auth.h"

#include <stdio.h>
#include <string.h>

#include "sb_crypto.h"

/* ─── State (RAM only) ──────────────────────────────────────────────────── */

static int initialised;
static char session_hex[SB_AUTH_SESSION_HEX + 1];

/* Window: bit i set = counter (top - i) already accepted. Counter 0 is
 * marked used at init, so the first valid counter is 1. */
static uint64_t win_top;
static uint64_t win_bits;

/* HTTP-task scratch for request / response MACs (body up to ~2.4 KB). */
static uint8_t scratch[SB_HMAC_MAX_MSG];

/* ─── Small helpers ─────────────────────────────────────────────────────── */

static const char HEX[] = "0123456789abcdef";

static void to_hex(const uint8_t* in, uint32_t n, char* out) {
    for (uint32_t i = 0; i < n; i++) {
        out[2 * i] = HEX[in[i] >> 4];
        out[2 * i + 1] = HEX[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1; /* lowercase only (API.md 3.3) */
}

static int from_hex(const char* s, int len, uint8_t* out) {
    if (len % 2) return -1;
    for (int i = 0; i < len / 2; i++) {
        int h = hexval(s[2 * i]), l = hexval(s[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return 0;
}

int sb_u64_to_dec(uint64_t v, char* out, int out_sz) {
    char tmp[21];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v && n < 20);
    if (n + 1 > out_sz) return -1;
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

int sb_dec_to_u64(const char* s, int len, uint64_t* v) {
    if (len < 1 || len > 20) return -1;
    uint64_t n = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (n > (UINT64_MAX - d) / 10) return -1; /* overflow */
        n = n * 10 + d;
    }
    *v = n;
    return 0;
}

/* Concatenate parts into the scratch buffer and MAC it. */
typedef struct {
    const void* p;
    uint32_t n;
} part_t;

static int mac_parts(const part_t* parts, int count, uint8_t* buf,
                     uint32_t cap, uint8_t mac[SB_HMAC_LEN]) {
    uint32_t len = 0;
    for (int i = 0; i < count; i++) {
        if (parts[i].n > cap - len) return -1;
        memcpy(buf + len, parts[i].p, parts[i].n);
        len += parts[i].n;
    }
    return sb_hmac(buf, len, mac);
}

#define PART_STR(s) {(s), (uint32_t)strlen(s)}
#define NL {"\n", 1}

/* ─── Session and window ────────────────────────────────────────────────── */

void sb_auth_init(const uint8_t session[8]) {
    to_hex(session, 8, session_hex);
    win_top = 0;
    win_bits = 1; /* counter 0 = used */
    initialised = 1;
}

const char* sb_auth_session_hex(void) { return session_hex; }

uint64_t sb_auth_next(void) { return win_top + 1; }

int sb_auth_window_accept(uint64_t c) {
    if (c > win_top) {
        uint64_t shift = c - win_top;
        win_bits = (shift >= 64) ? 0 : (win_bits << shift);
        win_bits |= 1;
        win_top = c;
        return 1;
    }
    uint64_t d = win_top - c;
    if (d >= 64) return 0;
    if ((win_bits >> d) & 1u) return 0;
    win_bits |= (uint64_t)1 << d;
    return 1;
}

/* ─── Request parsing ───────────────────────────────────────────────────── */

static int ci_eq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return 1;
}

typedef struct {
    const char* method;
    int method_len;
    const char* path;
    int path_len;
    const char* session;
    int session_len;
    const char* counter;
    int counter_len;
    const char* mac;
    int mac_len;
} parsed_t;

/* Value of header `name` (lowercase, with ':'), trimmed. */
static int find_header(const char* hdr, const char* end, const char* name,
                       const char** val, int* val_len) {
    int nlen = (int)strlen(name);
    const char* line = hdr;
    while (line < end) {
        const char* eol = line;
        while (eol + 1 < end && !(eol[0] == '\r' && eol[1] == '\n')) eol++;
        if (eol + 1 >= end) eol = end;

        if (eol - line > nlen && ci_eq(line, name, nlen)) {
            const char* v = line + nlen;
            const char* ve = eol;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            *val = v;
            *val_len = (int)(ve - v);
            return 0;
        }
        line = eol + 2;
    }
    return -1;
}

static int parse(const char* req, parsed_t* p) {
    const char* end = strstr(req, "\r\n\r\n");
    if (end == NULL) return -1;

    const char* sp1 = memchr(req, ' ', (size_t)(end - req));
    if (sp1 == NULL) return -1;
    const char* sp2 = memchr(sp1 + 1, ' ', (size_t)(end - sp1 - 1));
    if (sp2 == NULL) return -1;
    p->method = req;
    p->method_len = (int)(sp1 - req);
    p->path = sp1 + 1;
    p->path_len = (int)(sp2 - sp1 - 1);
    if (p->method_len < 1 || p->path_len < 1) return -1;

    const char* hdr = strstr(req, "\r\n");
    if (hdr == NULL || hdr >= end) return -1;
    hdr += 2;

    if (find_header(hdr, end, "x-sb-session:", &p->session, &p->session_len) ||
        find_header(hdr, end, "x-sb-counter:", &p->counter, &p->counter_len) ||
        find_header(hdr, end, "x-sb-mac:", &p->mac, &p->mac_len))
        return -1;
    return 0;
}

/* ─── Check ─────────────────────────────────────────────────────────────── */

sb_auth_result_t sb_auth_check(const char* req, const char* body,
                               uint32_t body_len, sb_auth_ctx_t* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    if (!initialised || !sb_crypto_ok()) return SB_AUTH_UNAVAILABLE;

    parsed_t p;
    if (parse(req, &p) < 0) return SB_AUTH_BAD_MAC;

    uint8_t sess[8], given[SB_HMAC_LEN];
    uint64_t counter;
    if (p.session_len != SB_AUTH_SESSION_HEX ||
        from_hex(p.session, p.session_len, sess) < 0)
        return SB_AUTH_BAD_MAC;
    if (sb_dec_to_u64(p.counter, p.counter_len, &counter) < 0)
        return SB_AUTH_BAD_MAC;
    if (p.mac_len != SB_AUTH_MAC_HEX || from_hex(p.mac, p.mac_len, given) < 0)
        return SB_AUTH_BAD_MAC;

    const part_t parts[] = {
        PART_STR("SB1-REQ\n"),
        {p.method, (uint32_t)p.method_len}, NL,
        {p.path, (uint32_t)p.path_len}, NL,
        {p.session, (uint32_t)p.session_len}, NL,
        {p.counter, (uint32_t)p.counter_len}, NL,
        {body, body_len},
    };
    uint8_t mac[SB_HMAC_LEN];
    if (mac_parts(parts, sizeof(parts) / sizeof(parts[0]), scratch,
                  sizeof(scratch), mac) != 0)
        return SB_AUTH_BAD_MAC;
    if (!sb_ct_equal(mac, given, SB_HMAC_LEN)) return SB_AUTH_BAD_MAC;

    /* The request is genuine: from here on, answers are signed. */
    ctx->mac_valid = 1;
    memcpy(ctx->req_mac, p.mac, SB_AUTH_MAC_HEX);
    ctx->req_mac[SB_AUTH_MAC_HEX] = '\0';

    if (memcmp(p.session, session_hex, SB_AUTH_SESSION_HEX) != 0)
        return SB_AUTH_BAD_SESSION;
    if (!sb_auth_window_accept(counter)) return SB_AUTH_REPLAY;
    return SB_AUTH_OK;
}

const char* sb_auth_reason(sb_auth_result_t r) {
    switch (r) {
        case SB_AUTH_BAD_SESSION: return "session";
        case SB_AUTH_REPLAY: return "replay";
        case SB_AUTH_UNAVAILABLE: return "unavailable";
        default: return "mac";
    }
}

int sb_auth_401_body(char* out, int out_sz, sb_auth_result_t r) {
    char next[21];
    sb_u64_to_dec(sb_auth_next(), next, sizeof(next));
    int n = snprintf(out, out_sz,
                     "{\"error\":\"auth\",\"reason\":\"%s\",\"session\":\"%s\","
                     "\"next\":%s}",
                     sb_auth_reason(r), session_hex, next);
    if (n >= out_sz) n = out_sz - 1;
    return n;
}

/* ─── Signing ───────────────────────────────────────────────────────────── */

int sb_auth_sign_response(const char* req_mac_hex, int status,
                          const char* body, uint32_t len,
                          char out_hex[SB_AUTH_MAC_HEX + 1]) {
    char st[12];
    snprintf(st, sizeof(st), "%d", status);
    const part_t parts[] = {
        PART_STR("SB1-RSP\n"),
        {req_mac_hex, SB_AUTH_MAC_HEX}, NL,
        PART_STR(st), NL,
        {body, len},
    };
    uint8_t mac[SB_HMAC_LEN];
    if (mac_parts(parts, sizeof(parts) / sizeof(parts[0]), scratch,
                  sizeof(scratch), mac) != 0)
        return -1;
    to_hex(mac, SB_HMAC_LEN, out_hex);
    return 0;
}

int sb_auth_sign_event(const char* req_mac_hex, uint32_t seq,
                       const char* event, const char* json, uint32_t len,
                       uint8_t* buf, uint32_t cap,
                       char out_hex[SB_AUTH_MAC_HEX + 1]) {
    char sq[12];
    snprintf(sq, sizeof(sq), "%lu", (unsigned long)seq);
    const part_t parts[] = {
        PART_STR("SB1-EVT\n"),
        {req_mac_hex, SB_AUTH_MAC_HEX}, NL,
        PART_STR(sq), NL,
        PART_STR(event), NL,
        {json, len},
    };
    uint8_t mac[SB_HMAC_LEN];
    if (mac_parts(parts, sizeof(parts) / sizeof(parts[0]), buf, cap, mac) != 0)
        return -1;
    to_hex(mac, SB_HMAC_LEN, out_hex);
    return 0;
}
