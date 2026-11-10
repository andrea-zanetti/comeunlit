/**
 * @file    api_v1.c
 * @brief   HTTP API v1 handlers (docs/API.md). See api_v1.h.
 *
 *          Every request is authenticated (sb_auth, API.md section 3) before
 *          anything else happens, and every response to a genuine request
 *          carries X-SB-MAC. Requests run synchronously in the HTTP accept
 *          task: a batch blocks other requests until it completes (about
 *          1 s for a few blinds with a reversal), which is fine for a single
 *          automation client.
 */

#include "api_v1.h"

#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"
#include "sb_auth.h"
#include "sb_blind.h"
#include "sb_bus.h"
#include "sb_frame.h"
#include "sb_stream.h"
#include "sb_sys.h"

/* Longest raw frame accepted on /admin/tx (= simplebus_tx TX_MAX_BITS). */
#define TX_MAX_BITS_API 128

/* API.md section 2: request bodies are at most 512 bytes. */
#define API_MAX_BODY 512

/* Authentication of the request being handled (HTTP task only). Responses
 * are signed whenever the request's own MAC was valid. */
static sb_auth_ctx_t g_ctx;

/* ─── Error and status mapping ──────────────────────────────────────────── */

int api_v1_status_of(int err) {
    switch (err) {
        case SB_OK: return 200;
        case SB_E_NOBUS: return 404;
        case SB_E_BUS_BUSY: return 503;
        case SB_E_NO_REPLY: return 504;
        case SB_E_NO_ACK: return 504;
        case SB_E_LOCK: return 504;
        case SB_E_STATE: return 409;
        case SB_E_MISMATCH: return 502;
        case SB_E_TX: return 500;
        default: return 400;
    }
}

const char* api_v1_error_of(int err) {
    switch (err) {
        case SB_E_NOBUS: return "no_bus";
        case SB_E_BUS_BUSY: return "bus_busy";
        case SB_E_NO_REPLY: return "no_reply";
        case SB_E_NO_ACK: return "no_ack";
        case SB_E_LOCK: return "timeout";
        case SB_E_STATE: return "state";
        case SB_E_MISMATCH: return "mismatch";
        case SB_E_TX: return "tx_failed";
        default: return "bad_request";
    }
}

static const char* reason_of(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Error";
    }
}

static const char* action_name(sb_action_t a) {
    switch (a) {
        case SB_ACT_PRESS: return "press";
        case SB_ACT_REVERSE: return "reverse";
        default: return "none";
    }
}

static const char* done_name(sb_done_t d) {
    switch (d) {
        case SB_DONE_STOP: return "stop";
        case SB_DONE_ALL: return "all";
        default: return "none";
    }
}

/* ─── Response writer ───────────────────────────────────────────────────── */

static void reply(int sock, int status, const char* body, int len) {
    char mac_hdr[SB_AUTH_MAC_HEX + 16] = "";
    if (g_ctx.mac_valid) {
        char mac[SB_AUTH_MAC_HEX + 1];
        if (sb_auth_sign_response(g_ctx.req_mac, status, body, (uint32_t)len,
                                  mac) == 0)
            snprintf(mac_hdr, sizeof(mac_hdr), "X-SB-MAC: %s\r\n", mac);
    }

    char hdr[256];
    int h = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %d\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason_of(status), len, mac_hdr);
    lwip_write(sock, hdr, h);
    if (len > 0) lwip_write(sock, body, len);
}

static void reply_error(int sock, int status, const char* code,
                        const char* detail) {
    char body[160];
    int n;
    if (detail)
        n = snprintf(body, sizeof(body),
                     "{\"error\":\"%s\",\"detail\":\"%s\"}", code, detail);
    else
        n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    reply(sock, status, body, n);
}

/* ─── Result formatting (API.md 5.2) ────────────────────────────────────── */

int api_v1_format_result(char* out, int out_sz, const sb_blind_result_t* r) {
    int n = snprintf(out, out_sz, "{\"bus\":%u,\"addr\":%u", r->bus, r->addr);

#define APPEND(...)                                                     \
    do {                                                                \
        if (n < out_sz) n += snprintf(out + n, out_sz - n, __VA_ARGS__); \
    } while (0)

    if (r->err != SB_OK) APPEND(",\"error\":\"%s\"", api_v1_error_of(r->err));

    if (r->have_before) {
        APPEND(",\"before\":\"%s\"", sb_state_name(r->before));
        if (r->err == SB_E_STATE) {
            APPEND(",\"out\":%u", r->before);
        } else {
            APPEND(",\"action\":\"%s\"", action_name(r->action));
            if (r->err != SB_OK)
                APPEND(",\"done\":\"%s\"", done_name(r->done));
            if (r->err == SB_OK || r->have_after)
                APPEND(",\"after\":\"%s\"", sb_state_name(r->after));
        }
    }
    APPEND("}");
#undef APPEND

    if (n >= out_sz) n = out_sz - 1;
    return n;
}

/* ─── Minimal JSON scanning ─────────────────────────────────────────────── */

/* Find "key" within [p, end); returns a pointer just past the ':' that
 * follows it, or NULL. */
static const char* find_key(const char* p, const char* end, const char* key) {
    size_t klen = strlen(key);
    for (; p + klen + 2 <= end; p++) {
        if (*p != '"' || strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"')
            continue;
        const char* q = p + klen + 2;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
            q++;
        if (q < end && *q == ':') {
            q++;
            while (q < end &&
                   (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
                q++;
            return q;
        }
    }
    return NULL;
}

static int parse_uint(const char* p, const char* end, unsigned max,
                      unsigned* v) {
    unsigned n = 0;
    int digits = 0;
    while (p < end && *p >= '0' && *p <= '9' && digits < 4) {
        n = n * 10 + (unsigned)(*p - '0');
        p++;
        digits++;
    }
    if (digits == 0 || n > max) return -1;
    *v = n;
    return 0;
}

int api_v1_parse_cmd(const char* body, sb_cmd_t* cmd) {
    const char* end = body + strlen(body);
    const char* p = find_key(body, end, "cmd");
    if (p == NULL || *p != '"') return -1;
    p++;
    const char* q = memchr(p, '"', (size_t)(end - p));
    if (q == NULL) return -1;

    size_t len = (size_t)(q - p);
    if (len == 2 && strncmp(p, "up", 2) == 0) *cmd = SB_CMD_UP;
    else if (len == 4 && strncmp(p, "down", 4) == 0) *cmd = SB_CMD_DOWN;
    else if (len == 4 && strncmp(p, "stop", 4) == 0) *cmd = SB_CMD_STOP;
    else return -1;
    return 0;
}

int api_v1_parse_targets(const char* body, sb_blind_result_t* r, int max) {
    const char* end = body + strlen(body);
    const char* p = find_key(body, end, "targets");
    if (p == NULL || *p != '[') return -1;
    p++;

    int count = 0;
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' ||
                           *p == '\n' || *p == ','))
            p++;
        if (p >= end) return -1;
        if (*p == ']') break;
        if (*p != '{') return -1;

        const char* obj_end = memchr(p, '}', (size_t)(end - p));
        if (obj_end == NULL) return -1;

        const char* b = find_key(p, obj_end, "bus");
        const char* a = find_key(p, obj_end, "addr");
        unsigned bus, addr;
        if (b == NULL || a == NULL || parse_uint(b, obj_end, 99, &bus) < 0 ||
            parse_uint(a, obj_end, 255, &addr) < 0)
            return -1;

        if (count >= max) return -2;
        memset(&r[count], 0, sizeof(r[count]));
        r[count].bus = (uint8_t)bus;
        r[count].addr = (uint8_t)addr;
        count++;
        p = obj_end + 1;
    }
    return count;
}

/* String value of "key" (between quotes, no escapes): pointer + length. */
static int find_string(const char* body, const char* key, const char** v,
                       int* len) {
    const char* end = body + strlen(body);
    const char* p = find_key(body, end, key);
    if (p == NULL || *p != '"') return -1;
    p++;
    const char* q = memchr(p, '"', (size_t)(end - p));
    if (q == NULL) return -1;
    *v = p;
    *len = (int)(q - p);
    return 0;
}

static int find_bool(const char* body, const char* key) {
    const char* end = body + strlen(body);
    const char* p = find_key(body, end, key);
    return (p && end - p >= 4 && strncmp(p, "true", 4) == 0) ? 1 : 0;
}

static int hexnib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "10 06 01 01" or "10060101" -> bytes. Returns count, -1 if malformed. */
int api_v1_parse_hex(const char* s, int len, uint8_t* out, int max) {
    int n = 0;
    for (int i = 0; i < len;) {
        if (s[i] == ' ') {
            i++;
            continue;
        }
        if (i + 1 >= len) return -1;
        int h = hexnib(s[i]), l = hexnib(s[i + 1]);
        if (h < 0 || l < 0 || n >= max) return -1;
        out[n++] = (uint8_t)((h << 4) | l);
        i += 2;
    }
    return n;
}

/* ─── Request line ──────────────────────────────────────────────────────── */

typedef enum { M_OTHER = 0, M_GET, M_POST } method_t;

#define PATH_MAX_LEN 64

static method_t parse_request_line(const char* req, char* path, int path_sz) {
    method_t m = M_OTHER;
    const char* p = req;
    if (strncmp(p, "GET ", 4) == 0) {
        m = M_GET;
        p += 4;
    } else if (strncmp(p, "POST ", 5) == 0) {
        m = M_POST;
        p += 5;
    } else {
        p = strchr(p, ' ');
        if (p == NULL) {
            path[0] = '\0';
            return M_OTHER;
        }
        p++;
    }

    int n = 0;
    while (*p && *p != ' ' && *p != '?' && *p != '\r' && n < path_sz - 1)
        path[n++] = *p++;
    path[n] = '\0';
    return m;
}

/* "/api/v1/blinds/<bus>/<addr>" -> 0, or -1 if the tail is malformed. */
static int parse_blind_path(const char* tail, unsigned* bus, unsigned* addr) {
    const char* end = tail + strlen(tail);
    const char* slash = strchr(tail, '/');
    if (slash == NULL) return -1;
    if (parse_uint(tail, slash, 99, bus) < 0) return -1;
    /* every char before the slash must be a digit */
    for (const char* c = tail; c < slash; c++)
        if (*c < '0' || *c > '9') return -1;
    const char* a = slash + 1;
    for (const char* c = a; c < end; c++)
        if (*c < '0' || *c > '9') return -1;
    if (parse_uint(a, end, 255, addr) < 0) return -1;
    return 0;
}

/* ─── Handlers ──────────────────────────────────────────────────────────── */

static char resp_buf[2048];
static sb_blind_result_t batch_res[SB_BATCH_MAX];

static void handle_blind_get(int sock, unsigned bus, unsigned addr) {
    uint8_t out = 0;
    int rc = sb_blind_read((uint8_t)bus, (uint8_t)addr, &out);
    int n;
    if (rc == SB_OK) {
        n = snprintf(resp_buf, sizeof(resp_buf),
                     "{\"bus\":%u,\"addr\":%u,\"state\":\"%s\"", bus, addr,
                     sb_state_name(out));
        if (strcmp(sb_state_name(out), "unknown") == 0)
            n += snprintf(resp_buf + n, sizeof(resp_buf) - n, ",\"out\":%u",
                          out);
        n += snprintf(resp_buf + n, sizeof(resp_buf) - n, "}");
    } else {
        n = snprintf(resp_buf, sizeof(resp_buf),
                     "{\"error\":\"%s\",\"bus\":%u,\"addr\":%u}",
                     api_v1_error_of(rc), bus, addr);
    }
    reply(sock, api_v1_status_of(rc), resp_buf, n);
}

static void handle_blind_post(int sock, unsigned bus, unsigned addr,
                              const char* body) {
    sb_cmd_t cmd;
    if (api_v1_parse_cmd(body, &cmd) < 0) {
        reply_error(sock, 400, "bad_request", "cmd must be up, down or stop");
        return;
    }
    sb_blind_result_t r;
    sb_blind_command((uint8_t)bus, (uint8_t)addr, cmd, &r);
    int n = api_v1_format_result(resp_buf, sizeof(resp_buf), &r);
    reply(sock, api_v1_status_of(r.err), resp_buf, n);
}

static void handle_batch(int sock, const char* body) {
    sb_cmd_t cmd;
    if (api_v1_parse_cmd(body, &cmd) < 0) {
        reply_error(sock, 400, "bad_request", "cmd must be up, down or stop");
        return;
    }
    int n = api_v1_parse_targets(body, batch_res, SB_BATCH_MAX);
    if (n == -2) {
        reply_error(sock, 400, "bad_request", "at most 16 targets");
        return;
    }
    if (n <= 0) {
        reply_error(sock, 400, "bad_request",
                    "targets must be [{\\\"bus\\\":N,\\\"addr\\\":N},...]");
        return;
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (batch_res[i].bus == batch_res[j].bus &&
                batch_res[i].addr == batch_res[j].addr) {
                reply_error(sock, 400, "bad_request", "duplicate target");
                return;
            }

    sb_blind_batch(cmd, batch_res, (uint8_t)n);

    int len = snprintf(resp_buf, sizeof(resp_buf), "{\"results\":[");
    /* One result is at most ~120 bytes; stop well before the end so the
     * closing "]}" always fits (16 targets need about 1.9 KB). */
    for (int i = 0; i < n && len < (int)sizeof(resp_buf) - 160; i++) {
        if (i) resp_buf[len++] = ',';
        len += api_v1_format_result(resp_buf + len, sizeof(resp_buf) - len - 2,
                                    &batch_res[i]);
    }
    resp_buf[len++] = ']';
    resp_buf[len++] = '}';
    reply(sock, 200, resp_buf, len);
}

#define API_TX_MAX_HEX 8 /* API.md 6.2 */

static void handle_admin_tx(int sock, unsigned bus, const char* body) {
    const char* hex;
    const char* bits;
    int hex_len, bits_len;
    int has_hex = find_string(body, "hex", &hex, &hex_len) == 0;
    int has_bits = find_string(body, "bits", &bits, &bits_len) == 0;
    if (has_hex == has_bits) {
        reply_error(sock, 400, "bad_request", "exactly one of hex or bits");
        return;
    }

    char frame[TX_MAX_BITS_API + 1];
    int n;
    if (has_hex) {
        uint8_t bytes[API_TX_MAX_HEX];
        int nb = api_v1_parse_hex(hex, hex_len, bytes, API_TX_MAX_HEX);
        if (nb <= 0) {
            reply_error(sock, 400, "bad_request", "hex: 1-8 bytes");
            return;
        }
        n = sb_frame_encode(bytes, (uint8_t)nb, find_bool(body, "checksum"),
                            frame, TX_MAX_BITS_API);
    } else {
        if (bits_len < 1 || bits_len > TX_MAX_BITS_API) {
            reply_error(sock, 400, "bad_request", "bits: 1-128 symbols");
            return;
        }
        for (int i = 0; i < bits_len; i++)
            if (bits[i] != '0' && bits[i] != '1') {
                reply_error(sock, 400, "bad_request", "bits: only 0 and 1");
                return;
            }
        memcpy(frame, bits, (size_t)bits_len);
        n = bits_len;
    }
    frame[n] = '\0';

    int rc = sb_bus_raw((uint8_t)bus, frame, (uint8_t)n);
    if (rc == SB_OK) {
        int len = snprintf(resp_buf, sizeof(resp_buf),
                           "{\"sent\":true,\"bits\":%d}", n);
        reply(sock, 200, resp_buf, len);
    } else {
        reply_error(sock, api_v1_status_of(rc), api_v1_error_of(rc), NULL);
    }
}

static int handle_stream(int sock, sb_stream_type_t type) {
    char hdr_mac[SB_AUTH_MAC_HEX + 1];
    if (sb_auth_sign_response(g_ctx.req_mac, 200, "", 0, hdr_mac) == 0 &&
        api_v1_stream_open(sock, type, g_ctx.req_mac, hdr_mac) == 0)
        return API_V1_TAKEN;
    reply_error(sock, 503, "busy", "all stream slots in use");
    return 0;
}

static void handle_health(int sock) {
    int n = snprintf(resp_buf, sizeof(resp_buf),
                     "{\"fw\":\"%s\",\"shield\":\"%s\",\"buses\":[",
                     SB_FW_VERSION, sb_sys_shield());
    for (int i = 0; i < sb_bus_count(); i++)
        n += snprintf(resp_buf + n, sizeof(resp_buf) - n,
                      "%s{\"bus\":%u,\"term\":\"%s\"}", i ? "," : "",
                      sb_bus_id_at(i), sb_bus_term_at(i));
    n += snprintf(resp_buf + n, sizeof(resp_buf) - n,
                  "],\"uptime_ms\":%lu,\"heap_free\":%lu,\"streams\":%d}",
                  (unsigned long)sb_sys_uptime_ms(),
                  (unsigned long)sb_sys_heap_free(), sb_sys_streams());
    reply(sock, 200, resp_buf, n);
}

int api_v1_handle(int sock, const char* req, const char* body,
                  int body_len) {
    /* 1. Authenticate before looking at anything else. */
    sb_auth_result_t ar =
        sb_auth_check(req, body, (uint32_t)(body_len > 0 ? body_len : 0),
                      &g_ctx);
    if (ar == SB_AUTH_UNAVAILABLE) {
        reply_error(sock, 503, "crypto", "self-test failed, see console");
        return 0;
    }
    if (ar != SB_AUTH_OK) {
        /* Signed (g_ctx.mac_valid) unless the MAC itself was bad. */
        char b[160];
        int n = sb_auth_401_body(b, sizeof(b), ar);
        reply(sock, 401, b, n);
        return 0;
    }
    if (body_len > API_MAX_BODY) {
        reply_error(sock, 413, "too_large", NULL);
        return 0;
    }

    char path[PATH_MAX_LEN];
    method_t m = parse_request_line(req, path, sizeof(path));

    static const char health_path[] = API_V1_PREFIX "health";
    static const char events_path[] = API_V1_PREFIX "events";
    static const char frames_path[] = API_V1_PREFIX "admin/frames";
    static const char tx_prefix[] = API_V1_PREFIX "admin/tx/";

    if (strcmp(path, health_path) == 0) {
        if (m == M_GET)
            handle_health(sock);
        else
            reply_error(sock, 405, "method", NULL);
        return 0;
    }

    if (strcmp(path, events_path) == 0 || strcmp(path, frames_path) == 0) {
        if (m != M_GET) {
            reply_error(sock, 405, "method", NULL);
            return 0;
        }
        return handle_stream(sock, strcmp(path, events_path) == 0
                                       ? SB_STREAM_EVENTS
                                       : SB_STREAM_FRAMES);
    }

    if (strncmp(path, tx_prefix, sizeof(tx_prefix) - 1) == 0) {
        unsigned bus;
        const char* t = path + sizeof(tx_prefix) - 1;
        const char* e = t + strlen(t);
        int digits = 1;
        for (const char* c = t; c < e; c++)
            if (*c < '0' || *c > '9') digits = 0;
        if (!digits || parse_uint(t, e, 99, &bus) < 0) {
            reply_error(sock, 404, "not_found", NULL);
            return 0;
        }
        if (!sb_bus_fitted((uint8_t)bus)) {
            char b[64];
            int n = snprintf(b, sizeof(b), "{\"error\":\"no_bus\",\"bus\":%u}",
                             bus);
            reply(sock, 404, b, n);
            return 0;
        }
        if (m == M_POST)
            handle_admin_tx(sock, bus, body);
        else
            reply_error(sock, 405, "method", NULL);
        return 0;
    }

    static const char blinds_prefix[] = API_V1_PREFIX "blinds/";
    static const char batch_path[] = API_V1_PREFIX "batch";

    if (strncmp(path, blinds_prefix, sizeof(blinds_prefix) - 1) == 0) {
        unsigned bus, addr;
        if (parse_blind_path(path + sizeof(blinds_prefix) - 1, &bus, &addr) <
            0) {
            reply_error(sock, 404, "not_found", NULL);
            return 0;
        }
        if (!sb_bus_fitted((uint8_t)bus)) {
            char b[64];
            int n = snprintf(b, sizeof(b), "{\"error\":\"no_bus\",\"bus\":%u}",
                             bus);
            reply(sock, 404, b, n);
            return 0;
        }
        if (m == M_GET)
            handle_blind_get(sock, bus, addr);
        else if (m == M_POST)
            handle_blind_post(sock, bus, addr, body);
        else
            reply_error(sock, 405, "method", NULL);
        return 0;
    }

    if (strcmp(path, batch_path) == 0) {
        if (m == M_POST)
            handle_batch(sock, body);
        else
            reply_error(sock, 405, "method", NULL);
        return 0;
    }

    reply_error(sock, 404, "not_found", NULL);
    return 0;
}
