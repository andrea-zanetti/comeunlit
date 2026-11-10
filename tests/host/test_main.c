/**
 * Host tests for the bus-independent firmware modules:
 *   sb_frame  codec, against frames captured on the real bus
 *   sb_blind  decide / press / reverse / batch, against a simulated module
 *   api_v1    request parsing, result JSON, full request -> response
 *
 * Build and run:  make -C tests/host
 */

#include <stdio.h>
#include <string.h>

#include "api_v1.h"
#include "fake_bus.h"
#include "sb_auth.h"
#include "sb_blind.h"
#include "sb_crypto.h"
#include "sb_frame.h"
#include "sb_stream.h"

extern int host_crypto_ok;

/* ─── api_v1_stream_open stub (the real one lives in http_sse.c) ─────────── */

static int stream_open_rc;
static int stream_open_calls;
static sb_stream_type_t stream_open_type;
static char stream_open_req_mac[65], stream_open_hdr_mac[65];

int api_v1_stream_open(int sock, sb_stream_type_t type, const char* req_mac,
                       const char* hdr_mac) {
    (void)sock;
    stream_open_calls++;
    stream_open_type = type;
    strcpy(stream_open_req_mac, req_mac);
    strcpy(stream_open_hdr_mac, hdr_mac);
    return stream_open_rc;
}

static int failures, checks;

#define CHECK(cond, ...)                         \
    do {                                         \
        checks++;                                \
        if (!(cond)) {                           \
            failures++;                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

#define CHECK_STR(got, want) \
    CHECK(strcmp((got), (want)) == 0, "\n   got:  %s\n   want: %s", (got), (want))

/* ─── lwip_write capture ────────────────────────────────────────────────── */

static char sock_out[4096];

int lwip_write(int s, const void* data, int size) {
    (void)s;
    strncat(sock_out, (const char*)data, (size_t)size);
    return size;
}

static const char* http_body(void) {
    const char* p = strstr(sock_out, "\r\n\r\n");
    return p ? p + 4 : "";
}

static int http_status(void) {
    int st = 0;
    sscanf(sock_out, "HTTP/1.1 %d", &st);
    return st;
}

/* ─── Signed request helpers (independent of sb_auth's internals) ───────── */

static void hex32(const uint8_t* m, char* out) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", m[i]);
}

static void mac_of(const char* msg, uint32_t len, char* out_hex) {
    uint8_t m[32];
    sb_hmac((const uint8_t*)msg, len, m);
    hex32(m, out_hex);
}

static uint64_t client_counter = 1;
static char last_req_mac[65];
static int last_api_rc;

/* Send one request with explicit session/counter; mac_override replaces
 * the computed MAC (NULL = correct MAC). */
static void api_ex(const char* method, const char* path, const char* body,
                   const char* session, uint64_t counter,
                   const char* mac_override) {
    const char* b = body ? body : "";
    char canon[1024];
    int cn = snprintf(canon, sizeof(canon), "SB1-REQ\n%s\n%s\n%s\n%llu\n%s",
                      method, path, session, (unsigned long long)counter, b);
    mac_of(canon, (uint32_t)cn, last_req_mac);

    char req[512];
    snprintf(req, sizeof(req),
             "%s %s HTTP/1.1\r\nHost: x\r\nx-sb-session: %s\r\n"
             "X-SB-Counter: %llu\r\nX-Sb-Mac:  %s \r\n\r\n",
             method, path, session, (unsigned long long)counter,
             mac_override ? mac_override : last_req_mac);
    sock_out[0] = '\0';
    last_api_rc = api_v1_handle(3, req, b, (int)strlen(b));
}

/* Normal signed request in the current session. */
static void api(const char* method, const char* path, const char* body) {
    api_ex(method, path, body, sb_auth_session_hex(), client_counter++, NULL);
}

/* 1 = response carries a valid X-SB-MAC for the last request, 0 = no
 * header, -1 = header present but wrong. */
static int response_signed(void) {
    const char* h = strstr(sock_out, "X-SB-MAC: ");
    if (h == NULL) return 0;
    char got[65];
    memcpy(got, h + 10, 64);
    got[64] = '\0';
    int st = 0;
    sscanf(sock_out, "HTTP/1.1 %d", &st);
    const char* body = strstr(sock_out, "\r\n\r\n") + 4;
    char canon[4096];
    int cn = snprintf(canon, sizeof(canon), "SB1-RSP\n%s\n%d\n%s",
                      last_req_mac, st, body);
    char want[65];
    mac_of(canon, (uint32_t)cn, want);
    return strcmp(got, want) == 0 ? 1 : -1;
}

/* ─── sb_frame ──────────────────────────────────────────────────────────── */

static void test_frame(void) {
    /* Wall-switch capture, 2026-11-07: UP press, then DOWN (stop). */
    const char* up =
        "01010101010000000001001100000101000000010100000001010111010";
    const char* stop =
        "01010101010000000001001100000100100000010000000001010111010";

    sb_frame_t f;
    CHECK(sb_frame_decode(up, (uint8_t)strlen(up), &f) == 1, "decode up");
    CHECK(sb_frame_is_std(&f), "up is STD with good checksum");
    CHECK(f.bytes[0] == 0x00 && f.bytes[1] == 0x06 && f.bytes[2] == 0x01 &&
              f.bytes[3] == 0x01 && f.bytes[4] == 0x5D,
          "up bytes %02X %02X %02X %02X %02X", f.bytes[0], f.bytes[1],
          f.bytes[2], f.bytes[3], f.bytes[4]);

    CHECK(sb_frame_decode(stop, (uint8_t)strlen(stop), &f) == 1, "decode stop");
    CHECK(sb_frame_is_std(&f) && f.bytes[2] == 0x02 && f.bytes[3] == 0x00,
          "stop bytes");

    /* Encoder round trip: 59 symbols out (no final stop bit), decodes back. */
    const uint8_t press[4] = {0x10, 0x06, 0x01, 0x01};
    char bits[129];
    uint8_t n = sb_frame_encode(press, 4, 1, bits, 128);
    CHECK(n == 59, "encode length %u", n);

    /* The poll the bridge sends to bus 2 addr 6 must be exactly the 59
     * symbols (60 pulses) that got a 1.5 ms turnaround on the hardware
     * (frames capture 2026-11-08: tx->rx 28 ms; the 60-symbol version took
     * 60 ms). */
    {
        const uint8_t poll[4] = {0x21, 0x06, 0x00, 0x00};
        char pb[129];
        uint8_t pn = sb_frame_encode(poll, 4, 1, pb, 128);
        CHECK(pn == 59 &&
                  strcmp(pb, "01010101010100001001001100000100000000010000000001000111110") == 0,
              "poll matches the hardware-verified frame: %s", pb);
    }
    CHECK(sb_frame_decode(bits, n, &f) && sb_frame_is_std(&f) &&
              f.bytes[0] == 0x10 && f.bytes[4] == (uint8_t)(0x55 + 0x18),
          "round trip");

    /* The receiver loses the final stop bit: 59 symbols must decode. */
    CHECK(sb_frame_decode(bits, 59, &f) && sb_frame_is_std(&f),
          "59-symbol frame");

    /* Corrupted checksum bit -> decodes, chk_ok = 0. */
    bits[52] = (bits[52] == '1') ? '0' : '1';
    CHECK(sb_frame_decode(bits, n, &f) && !f.chk_ok, "bad checksum detected");

    /* Garbage preamble -> not decoded. */
    CHECK(sb_frame_decode("11111111111111111111111111111111111111111111111111"
                          "111111111",
                          59, &f) == 0,
          "unknown preamble");
}

/* ─── sb_blind ──────────────────────────────────────────────────────────── */

static void test_command_table(void) {
    struct {
        uint8_t cur;
        sb_cmd_t cmd;
        sb_action_t act;
        uint8_t after;
        const char* log;
    } cases[] = {
        {SB_OUT_IDLE, SB_CMD_UP, SB_ACT_PRESS, SB_OUT_UP,
         "poll 2:6; press 2:6 btn=1; "},
        {SB_OUT_IDLE, SB_CMD_DOWN, SB_ACT_PRESS, SB_OUT_DOWN,
         "poll 2:6; press 2:6 btn=2; "},
        {SB_OUT_IDLE, SB_CMD_STOP, SB_ACT_NONE, SB_OUT_IDLE, "poll 2:6; "},
        {SB_OUT_UP, SB_CMD_UP, SB_ACT_NONE, SB_OUT_UP, "poll 2:6; "},
        {SB_OUT_UP, SB_CMD_STOP, SB_ACT_PRESS, SB_OUT_IDLE,
         "poll 2:6; press 2:6 btn=2; "},
        {SB_OUT_UP, SB_CMD_DOWN, SB_ACT_REVERSE, SB_OUT_DOWN,
         "poll 2:6; press 2:6 btn=2; sleep 500; press 2:6 btn=2; "},
        {SB_OUT_DOWN, SB_CMD_DOWN, SB_ACT_NONE, SB_OUT_DOWN, "poll 2:6; "},
        {SB_OUT_DOWN, SB_CMD_STOP, SB_ACT_PRESS, SB_OUT_IDLE,
         "poll 2:6; press 2:6 btn=1; "},
        {SB_OUT_DOWN, SB_CMD_UP, SB_ACT_REVERSE, SB_OUT_UP,
         "poll 2:6; press 2:6 btn=1; sleep 500; press 2:6 btn=1; "},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_reset();
        fake_module_t* m = fake_add(2, 6, cases[i].cur);
        sb_blind_result_t r;
        sb_blind_command(2, 6, cases[i].cmd, &r);
        CHECK(r.err == SB_OK, "case %u err %d", i, r.err);
        CHECK(r.action == cases[i].act, "case %u action %d", i, r.action);
        CHECK(r.before == cases[i].cur, "case %u before", i);
        CHECK(r.after == cases[i].after && m->out == cases[i].after,
              "case %u after %u module %u", i, r.after, m->out);
        CHECK(r.done == SB_DONE_ALL, "case %u done %d", i, r.done);
        CHECK_STR(fake_log, cases[i].log);
    }
}

static void test_command_errors(void) {
    sb_blind_result_t r;

    /* No module */
    fake_reset();
    sb_blind_command(2, 9, SB_CMD_UP, &r);
    CHECK(r.err == SB_E_NO_REPLY && !r.have_before, "no module");

    /* Unknown relay value: nothing pressed */
    fake_reset();
    fake_add(2, 6, 0x07);
    sb_blind_command(2, 6, SB_CMD_STOP, &r);
    CHECK(r.err == SB_E_STATE && r.have_before && r.before == 7, "state");
    CHECK_STR(fake_log, "poll 2:6; ");

    /* Reverse, stop ack lost: no second press, no retry */
    fake_reset();
    fake_module_t* m = fake_add(2, 6, SB_OUT_UP);
    m->drop_ack_at = 1;
    sb_blind_command(2, 6, SB_CMD_DOWN, &r);
    CHECK(r.err == SB_E_NO_ACK && r.done == SB_DONE_NONE && !r.have_after,
          "reverse, first ack lost: err %d done %d", r.err, r.done);
    CHECK_STR(fake_log, "poll 2:6; press 2:6 btn=2; ");
    CHECK(m->out == SB_OUT_IDLE, "module did stop (ack lost, not the press)");

    /* Reverse, second ack lost: done = stop, after = idle */
    fake_reset();
    m = fake_add(2, 6, SB_OUT_UP);
    m->drop_ack_at = 2;
    sb_blind_command(2, 6, SB_CMD_DOWN, &r);
    CHECK(r.err == SB_E_NO_ACK && r.done == SB_DONE_STOP && r.have_after &&
              r.after == SB_OUT_IDLE,
          "reverse, second ack lost");

    /* Module ignores the press: ack shows unchanged state -> mismatch */
    fake_reset();
    m = fake_add(2, 6, SB_OUT_IDLE);
    m->stuck = 1;
    sb_blind_command(2, 6, SB_CMD_UP, &r);
    CHECK(r.err == SB_E_MISMATCH && r.after == SB_OUT_IDLE, "mismatch");

    /* Bus busy at the poll */
    fake_reset();
    fake_add(2, 6, SB_OUT_IDLE);
    fake_set_busy(2, 1);
    sb_blind_command(2, 6, SB_CMD_UP, &r);
    CHECK(r.err == SB_E_BUS_BUSY && !r.have_before, "bus busy");

    /* Unfitted bus */
    fake_reset();
    sb_blind_command(1, 6, SB_CMD_UP, &r);
    CHECK(r.err == SB_E_NOBUS, "no bus");

    /* Locks balanced in every path above (fake logs violations) */
    CHECK(strstr(fake_log, "LOCK") == NULL, "lock balance: %s", fake_log);
}

static void test_batch(void) {
    sb_blind_result_t r[4];

    /* Mixed: idle, moving down (reverse), already up, missing module. */
    fake_reset();
    fake_add(2, 3, SB_OUT_IDLE);
    fake_add(2, 4, SB_OUT_DOWN);
    fake_add(2, 5, SB_OUT_UP);
    r[0].bus = 2; r[0].addr = 3;
    r[1].bus = 2; r[1].addr = 4;
    r[2].bus = 2; r[2].addr = 5;
    r[3].bus = 2; r[3].addr = 9;
    sb_blind_batch(SB_CMD_UP, r, 4);

    CHECK_STR(fake_log,
              "poll 2:3; poll 2:4; poll 2:5; poll 2:9; "
              "press 2:4 btn=1; "
              "sleep 500; "
              "press 2:3 btn=1; press 2:4 btn=1; ");
    CHECK(r[0].err == SB_OK && r[0].action == SB_ACT_PRESS &&
              r[0].after == SB_OUT_UP,
          "batch idle->up");
    CHECK(r[1].err == SB_OK && r[1].action == SB_ACT_REVERSE &&
              r[1].after == SB_OUT_UP,
          "batch down->up");
    CHECK(r[2].err == SB_OK && r[2].action == SB_ACT_NONE, "batch up->up");
    CHECK(r[3].err == SB_E_NO_REPLY, "batch missing module");

    /* Stop batch: no pause at all */
    fake_reset();
    fake_add(2, 3, SB_OUT_UP);
    fake_add(2, 4, SB_OUT_DOWN);
    r[0].bus = 2; r[0].addr = 3;
    r[1].bus = 2; r[1].addr = 4;
    sb_blind_batch(SB_CMD_STOP, r, 2);
    CHECK_STR(fake_log, "poll 2:3; poll 2:4; press 2:3 btn=2; press 2:4 btn=1; ");
    CHECK(r[0].done == SB_DONE_ALL && r[1].done == SB_DONE_ALL,
          "stop batch done");

    /* Two buses, unfitted target: locks taken once per bus, balanced */
    fake_reset();
    fake_add(3, 1, SB_OUT_IDLE);
    fake_add(2, 1, SB_OUT_IDLE);
    r[0].bus = 3; r[0].addr = 1;
    r[1].bus = 2; r[1].addr = 1;
    r[2].bus = 7; r[2].addr = 1;
    sb_blind_batch(SB_CMD_DOWN, r, 3);
    CHECK(r[0].err == SB_OK && r[1].err == SB_OK && r[2].err == SB_E_NOBUS,
          "two buses");
    CHECK(strstr(fake_log, "LOCK") == NULL, "batch lock balance: %s",
          fake_log);
}

/* ─── sb_auth ───────────────────────────────────────────────────────────── */

static const uint8_t test_session[8] = {0x3f, 0x9a, 0x0c, 0x51,
                                        0xd2, 0xe8, 0x7b, 0x46};

static void test_auth_helpers(void) {
    char b[24];
    uint64_t v;
    sb_u64_to_dec(UINT64_MAX, b, sizeof(b));
    CHECK_STR(b, "18446744073709551615");
    CHECK(sb_dec_to_u64(b, (int)strlen(b), &v) == 0 && v == UINT64_MAX,
          "u64 max round trip");
    CHECK(sb_dec_to_u64("18446744073709551616", 20, &v) < 0, "u64 overflow");
    CHECK(sb_dec_to_u64("12a", 3, &v) < 0, "u64 junk");
    CHECK(sb_dec_to_u64("", 0, &v) < 0, "u64 empty");

    /* Replay window (RFC 4303 style, 64 entries) */
    sb_auth_init(test_session);
    CHECK(sb_auth_next() == 1, "next starts at 1");
    CHECK(!sb_auth_window_accept(0), "counter 0 never valid");
    CHECK(sb_auth_window_accept(1), "1 accepted");
    CHECK(!sb_auth_window_accept(1), "1 replay");
    CHECK(sb_auth_window_accept(5), "5 accepted (jump)");
    CHECK(sb_auth_window_accept(3), "3 accepted (out of order, in window)");
    CHECK(!sb_auth_window_accept(3), "3 replay");
    CHECK(sb_auth_next() == 6, "next = 6");
    CHECK(sb_auth_window_accept(5 + 63 + 1), "jump to 69");
    CHECK(sb_auth_window_accept(69 - 63), "69-63 = 6 still in window");
    CHECK(!sb_auth_window_accept(69 - 64), "69-64 = 5 outside window");
    CHECK(sb_auth_window_accept(1000), "far jump");
    CHECK(!sb_auth_window_accept(69), "old top now outside window");

    /* Cross-check against tools/sbctl.py (vectors computed with it). */
    char mac[65];
    sb_auth_sign_response(
        "98810fe6caefeb1b7fd403f01447425a773b90b5242644ef836e6efd7e7b136e",
        200, "{\"ok\":1}", 8, mac);
    CHECK_STR(mac,
              "71b6b4fd04e92b4f3aa8bba6e2985f996511467d3ea6b77b556b4b5e5986b038");
    uint8_t scratch[512];
    const char* ev = "{\"bus\":2,\"addr\":6,\"state\":\"up\"}";
    sb_auth_sign_event(
        "98810fe6caefeb1b7fd403f01447425a773b90b5242644ef836e6efd7e7b136e", 17,
        "state", ev, (uint32_t)strlen(ev), scratch, sizeof(scratch), mac);
    CHECK_STR(mac,
              "a2b058b5c5f4f5790cf7c52acd76fd9e286a7145beb336347620dd696c644f75");
}

static void test_auth_flow(void) {
    fake_reset();
    fake_add(2, 6, SB_OUT_IDLE);
    sb_auth_init(test_session);
    client_counter = 1;
    const char* sess = sb_auth_session_hex();
    CHECK_STR(sess, "3f9a0c51d2e87b46");

    /* Request MAC canonical matches sbctl's (same vector as the HASH
     * self-test): MAC valid, but wrong session -> signed 401 session. */
    api_ex("GET", "/api/v1/health", NULL, "0123456789abcdef", 7,
           "98810fe6caefeb1b7fd403f01447425a773b90b5242644ef836e6efd7e7b136e");
    CHECK(http_status() == 401, "foreign session status %d", http_status());
    CHECK(response_signed() == 1, "foreign-session 401 is signed");
    CHECK(strstr(http_body(), "\"reason\":\"session\"") != NULL, "reason");

    /* Bootstrap */
    api_ex("GET", "/api/v1/health", NULL, "0000000000000000", 99, NULL);
    CHECK(http_status() == 401 && response_signed() == 1, "bootstrap 401");
    CHECK_STR(http_body(),
              "{\"error\":\"auth\",\"reason\":\"session\","
              "\"session\":\"3f9a0c51d2e87b46\",\"next\":1}");

    /* Valid request */
    api_ex("GET", "/api/v1/health", NULL, sess, 1, NULL);
    CHECK(http_status() == 200, "health status %d", http_status());
    CHECK(response_signed() == 1, "health signed");
    CHECK_STR(http_body(),
              "{\"fw\":\"0.1.0\",\"shield\":\"1\",\"buses\":["
              "{\"bus\":2,\"term\":\"J6\"},{\"bus\":3,\"term\":\"J7\"},"
              "{\"bus\":4,\"term\":\"J8\"}],\"uptime_ms\":123456,"
              "\"heap_free\":74210,\"streams\":0}");

    /* Replay of counter 1 -> signed 401 replay, next = 2 */
    api_ex("GET", "/api/v1/health", NULL, sess, 1, NULL);
    CHECK(http_status() == 401 && response_signed() == 1, "replay 401 signed");
    CHECK(strstr(http_body(), "\"reason\":\"replay\"") &&
              strstr(http_body(), "\"next\":2"),
          "replay body %s", http_body());

    /* Replayed POST must not move the blind */
    api_ex("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"up\"}", sess, 2, NULL);
    CHECK(http_status() == 200 && response_signed() == 1, "POST ok");
    fake_log[0] = '\0';
    api_ex("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"up\"}", sess, 2, NULL);
    CHECK(http_status() == 401, "replayed POST rejected");
    CHECK(fake_log[0] == '\0', "replayed POST touched the bus: %s", fake_log);

    /* Wrong MAC -> unsigned 401 mac, nothing done */
    api_ex("GET", "/api/v1/health", NULL, sess, 3,
           "0000000000000000000000000000000000000000000000000000000000000000");
    CHECK(http_status() == 401 && response_signed() == 0, "bad mac unsigned");
    CHECK(strstr(http_body(), "\"reason\":\"mac\"") != NULL, "reason mac");

    /* Body tampered after signing -> mac */
    {
        char canon[256], mac[65];
        int cn = snprintf(canon, sizeof(canon),
                          "SB1-REQ\nPOST\n/api/v1/blinds/2/6\n%s\n4\n"
                          "{\"cmd\":\"stop\"}", sess);
        mac_of(canon, (uint32_t)cn, mac);
        api_ex("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"down\"}", sess, 4,
               mac);
        CHECK(http_status() == 401 && response_signed() == 0,
              "tampered body rejected");
    }

    /* Path tampered -> mac */
    {
        char canon[256], mac[65];
        int cn = snprintf(canon, sizeof(canon),
                          "SB1-REQ\nGET\n/api/v1/blinds/2/6\n%s\n5\n", sess);
        mac_of(canon, (uint32_t)cn, mac);
        api_ex("GET", "/api/v1/blinds/2/7", NULL, sess, 5, mac);
        CHECK(http_status() == 401 && response_signed() == 0,
              "tampered path rejected");
    }

    /* Uppercase hex MAC is not accepted (API.md: lowercase) */
    {
        char up[65];
        api_ex("GET", "/api/v1/health", NULL, sess, 6, NULL); /* ok, uses 6 */
        strcpy(up, last_req_mac);
        for (char* c = up; *c; c++)
            if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 32);
        api_ex("GET", "/api/v1/health", NULL, sess, 7, up);
        CHECK(http_status() == 401 && response_signed() == 0, "uppercase mac");
    }

    /* Missing headers */
    sock_out[0] = '\0';
    api_v1_handle(3, "GET /api/v1/health HTTP/1.1\r\nHost: x\r\n\r\n", "", 0);
    CHECK(http_status() == 401 && strstr(sock_out, "X-SB-MAC") == NULL,
          "no auth headers");

    /* Body over 512 bytes, correctly signed -> 413, signed */
    {
        static char big[700];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        api_ex("POST", "/api/v1/batch", big, sess, 8, NULL);
        CHECK(http_status() == 413 && response_signed() == 1, "413 signed");
    }

    /* Crypto self-test failed -> 503, unsigned, nothing else */
    host_crypto_ok = 0;
    api_ex("GET", "/api/v1/health", NULL, sess, 9, last_req_mac);
    CHECK(http_status() == 503 && strstr(sock_out, "X-SB-MAC") == NULL,
          "crypto unavailable");
    host_crypto_ok = 1;

    client_counter = 100; /* past everything used above */
}

/* ─── sb_stream ─────────────────────────────────────────────────────────── */

/* Ring frame from 4 bytes (+checksum), as the receiver or the TX echo
 * (both 59 symbols) would publish it. */
static sb_ring_frame_t mkframe(uint8_t bus, uint8_t dir, uint32_t ts,
                               uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    sb_ring_frame_t f;
    memset(&f, 0, sizeof(f));
    const uint8_t d[4] = {b0, b1, b2, b3};
    uint8_t n = sb_frame_encode(d, 4, 1, f.bits, MAX_BIT_STRING);
    f.bits[n] = '\0';
    f.bit_len = n;
    f.trunk = bus;
    f.dir = dir;
    f.timestamp_ms = ts;
    return f;
}

static const char* STREAM_MAC =
    "98810fe6caefeb1b7fd403f01447425a773b90b5242644ef836e6efd7e7b136e";

/* Check one SSE text: event name, id, MAC (recomputed), payload. */
static void check_sse(const char* text, int len, const char* event,
                      unsigned id, const char* payload) {
    char want_head[64];
    snprintf(want_head, sizeof(want_head), "event: %s\nid: %u\ndata: ", event,
             id);
    CHECK(len > 0 && strncmp(text, want_head, strlen(want_head)) == 0,
          "sse head: %.*s", len > 0 ? len : 0, text);
    if (len <= 0) return;
    const char* mac = text + strlen(want_head);
    const char* json = mac + 65;
    CHECK(strncmp(json, payload, strlen(payload)) == 0 &&
              strcmp(json + strlen(payload), "\n\n") == 0,
          "sse payload\n   got:  %s   want: %s", json, payload);
    char canon[1024], want_mac[65];
    int cn = snprintf(canon, sizeof(canon), "SB1-EVT\n%s\n%u\n%s\n%s",
                      STREAM_MAC, id, event, payload);
    mac_of(canon, (uint32_t)cn, want_mac);
    CHECK(strncmp(mac, want_mac, 64) == 0 && mac[64] == ' ', "sse mac");
}

static void test_stream_events(void) {
    sb_stream_t st;
    char out[SB_STREAM_LINE_MAX];
    sb_stream_init(&st, SB_STREAM_EVENTS, STREAM_MAC);
    sb_ring_frame_t f;
    int n;

    /* Bridge press, then the module's ack 61 ms later: source bridge */
    f = mkframe(2, SB_DIR_TX, 1000, 0x10, 0x06, 0x01, 0x01);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "TX press silent");
    f = mkframe(2, SB_DIR_RX, 1061, 0x00, 0x06, 0x01, 0x01);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    check_sse(out, n, "state", 1,
              "{\"bus\":2,\"addr\":6,\"state\":\"up\",\"button\":\"up\","
              "\"source\":\"bridge\"}");

    /* Repeat of the same ack within 500 ms: dropped */
    f = mkframe(2, SB_DIR_RX, 1192, 0x00, 0x06, 0x01, 0x01);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "repeat dropped");

    /* Wall switch on another module, twice (as captured): one event */
    f = mkframe(2, SB_DIR_RX, 5000, 0x00, 0x05, 0x02, 0x00);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    check_sse(out, n, "state", 2,
              "{\"bus\":2,\"addr\":5,\"state\":\"idle\",\"button\":\"down\","
              "\"source\":\"local\"}");
    f = mkframe(2, SB_DIR_RX, 5252, 0x00, 0x05, 0x02, 0x00);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "wall repeat");

    /* Same announcement again after > 500 ms: a new event */
    f = mkframe(2, SB_DIR_RX, 5600, 0x00, 0x05, 0x02, 0x00);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "id: 3\n") != NULL, "after 500 ms: new event");

    /* Ack long after our press: local (someone pressed the wall switch) */
    f = mkframe(2, SB_DIR_RX, 1500, 0x00, 0x06, 0x02, 0x00);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "\"source\":\"local\"") != NULL,
          "late ack is local");

    /* Same addr on another bus is a different module */
    f = mkframe(3, SB_DIR_RX, 1062, 0x00, 0x06, 0x01, 0x01);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "\"bus\":3") && strstr(out, "\"local\""),
          "other bus");

    /* Not announcements: poll, poll reply, drop, bad checksum, garbage */
    f = mkframe(2, SB_DIR_TX, 7000, 0x21, 0x06, 0x00, 0x00);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "poll ignored");
    f = mkframe(2, SB_DIR_RX, 7061, 0x01, 0x06, 0x00, 0x01);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "reply ignored");
    f = mkframe(2, SB_DIR_DROP, 7100, 0x10, 0x06, 0x01, 0x01);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "drop ignored");
    f = mkframe(2, SB_DIR_RX, 7200, 0x00, 0x04, 0x01, 0x01);
    f.bits[52] = (f.bits[52] == '1') ? '0' : '1';
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "bad chk ignored");
    memset(f.bits, '1', 59);
    CHECK(sb_stream_frame(&st, &f, out, sizeof(out)) == 0, "garbage ignored");

    /* Heartbeat continues the sequence */
    n = sb_stream_heartbeat(&st, 81234567, out, sizeof(out));
    check_sse(out, n, "hb", 6, "{\"uptime_ms\":81234567}");
}

static void test_stream_frames(void) {
    sb_stream_t st;
    char out[SB_STREAM_LINE_MAX];
    sb_stream_init(&st, SB_STREAM_FRAMES, STREAM_MAC);
    int n;

    sb_ring_frame_t f = mkframe(2, SB_DIR_RX, 2904247, 0x00, 0x06, 0x01,
                                0x01);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    char want[400];
    snprintf(want, sizeof(want),
             "{\"ts\":2904247,\"bus\":2,\"dir\":\"rx\",\"bits\":\"%s\","
             "\"proto\":\"STD\",\"hex\":\"00 06 01 01 5D\",\"chk_ok\":true}",
             f.bits);
    check_sse(out, n, "frame", 1, want);
    /* The wall-switch capture's exact bits */
    CHECK_STR(f.bits,
              "01010101010000000001001100000101000000010100000001010111010");

    f = mkframe(4, SB_DIR_DROP, 10, 0x10, 0x03, 0x02, 0x02);
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "id: 2\n") && strstr(out, "\"dir\":\"drop\""),
          "drop frame");

    memset(f.bits, '1', 40);
    f.bits[40] = '\0';
    f.bit_len = 40;
    f.dir = SB_DIR_RX;
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    snprintf(want, sizeof(want),
             "{\"ts\":10,\"bus\":4,\"dir\":\"rx\",\"bits\":\"%s\"}", f.bits);
    check_sse(out, n, "frame", 3, want);

    /* Longest frame the ring can hold still fits one SSE text */
    memset(f.bits, '0', MAX_BIT_STRING);
    f.bits[MAX_BIT_STRING] = '\0';
    f.bit_len = MAX_BIT_STRING;
    n = sb_stream_frame(&st, &f, out, sizeof(out));
    CHECK(n > 0 && n < (int)sizeof(out), "128-symbol frame fits (%d)", n);
}

/* ─── api_v1 ────────────────────────────────────────────────────────────── */

static void test_api_streams_and_tx(void) {
    fake_reset();

    /* Stream opened: socket handed over, nothing written by the API */
    stream_open_rc = 0;
    stream_open_calls = 0;
    api("GET", "/api/v1/events", NULL);
    CHECK(last_api_rc == API_V1_TAKEN && sock_out[0] == '\0' &&
              stream_open_calls == 1 && stream_open_type == SB_STREAM_EVENTS,
          "events stream opened");
    CHECK_STR(stream_open_req_mac, last_req_mac);
    {
        char canon[128], want[65];
        int cn = snprintf(canon, sizeof(canon), "SB1-RSP\n%s\n200\n",
                          last_req_mac);
        mac_of(canon, (uint32_t)cn, want);
        CHECK_STR(stream_open_hdr_mac, want);
    }
    api("GET", "/api/v1/admin/frames", NULL);
    CHECK(last_api_rc == API_V1_TAKEN && stream_open_type == SB_STREAM_FRAMES,
          "frames stream opened");

    /* No slot: signed 503, socket stays with the caller */
    stream_open_rc = -1;
    api("GET", "/api/v1/events", NULL);
    CHECK(last_api_rc == 0 && http_status() == 503 && response_signed() == 1,
          "stream busy");
    stream_open_rc = 0;

    api("POST", "/api/v1/events", "{}");
    CHECK(http_status() == 405 && response_signed() == 1, "POST events");

    /* Admin TX */
    api("POST", "/api/v1/admin/tx/2", "{\"hex\":\"10 06 01 01\",\"checksum\":true}");
    CHECK(http_status() == 200 && response_signed() == 1, "tx hex status %d",
          http_status());
    CHECK_STR(http_body(), "{\"sent\":true,\"bits\":59}");
    CHECK(strstr(fake_log, "raw 2 n=59 01010101010000010001") != NULL,
          "tx bits on bus: %s", fake_log);

    api("POST", "/api/v1/admin/tx/3", "{\"hex\":\"10060101\"}");
    CHECK_STR(http_body(), "{\"sent\":true,\"bits\":49}");

    api("POST", "/api/v1/admin/tx/4", "{\"bits\":\"0101\"}");
    CHECK_STR(http_body(), "{\"sent\":true,\"bits\":4}");

    const char* bad[] = {
        "{}",
        "{\"hex\":\"10\",\"bits\":\"01\"}",
        "{\"hex\":\"1g\"}",
        "{\"hex\":\"1\"}",
        "{\"hex\":\"01 02 03 04 05 06 07 08 09\"}",
        "{\"bits\":\"0120\"}",
        "{\"bits\":\"\"}",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        api("POST", "/api/v1/admin/tx/2", bad[i]);
        CHECK(http_status() == 400 && response_signed() == 1, "bad tx %u: %s",
              i, bad[i]);
    }

    api("POST", "/api/v1/admin/tx/1", "{\"hex\":\"10\"}");
    CHECK(http_status() == 404 && strstr(http_body(), "no_bus"), "tx no bus");
    api("POST", "/api/v1/admin/tx/2x", "{\"hex\":\"10\"}");
    CHECK(http_status() == 404, "tx bad path");
    api("GET", "/api/v1/admin/tx/2", NULL);
    CHECK(http_status() == 405, "tx GET");

    fake_set_busy(2, 1);
    api("POST", "/api/v1/admin/tx/2", "{\"hex\":\"10\"}");
    CHECK(http_status() == 503 && strstr(http_body(), "bus_busy"), "tx busy");
    fake_set_busy(2, 0);
}

static void test_api(void) {
    fake_reset();
    fake_add(2, 6, SB_OUT_UP);

    api("GET", "/api/v1/blinds/2/6", NULL);
    CHECK(http_status() == 200, "GET status %d", http_status());
    CHECK(response_signed() == 1, "GET response signed");
    CHECK_STR(http_body(), "{\"bus\":2,\"addr\":6,\"state\":\"up\"}");

    api("GET", "/api/v1/blinds/2/9", NULL);
    CHECK(http_status() == 504, "no reply status");
    CHECK_STR(http_body(), "{\"error\":\"no_reply\",\"bus\":2,\"addr\":9}");

    api("GET", "/api/v1/blinds/1/6", NULL);
    CHECK(http_status() == 404, "no bus status");
    CHECK_STR(http_body(), "{\"error\":\"no_bus\",\"bus\":1}");

    api("GET", "/api/v1/blinds/2/6x", NULL);
    CHECK(http_status() == 404, "malformed path");

    api("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"down\"}");
    CHECK(http_status() == 200, "POST status %d", http_status());
    CHECK_STR(http_body(),
              "{\"bus\":2,\"addr\":6,\"before\":\"up\",\"action\":\"reverse\","
              "\"after\":\"down\"}");

    api("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"down\"}");
    CHECK_STR(http_body(),
              "{\"bus\":2,\"addr\":6,\"before\":\"down\",\"action\":\"none\","
              "\"after\":\"down\"}");

    api("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"sideways\"}");
    CHECK(http_status() == 400, "bad cmd");

    /* no_ack mid-reverse, as in API.md 5.2 */
    fake_reset();
    fake_module_t* m = fake_add(2, 6, SB_OUT_UP);
    m->drop_ack_at = 2;
    api("POST", "/api/v1/blinds/2/6", "{ \"cmd\" : \"down\" }");
    CHECK(http_status() == 504, "no_ack status");
    CHECK_STR(http_body(),
              "{\"bus\":2,\"addr\":6,\"error\":\"no_ack\",\"before\":\"up\","
              "\"action\":\"reverse\",\"done\":\"stop\",\"after\":\"idle\"}");

    /* Unknown state */
    fake_reset();
    fake_add(2, 6, 0x07);
    api("POST", "/api/v1/blinds/2/6", "{\"cmd\":\"up\"}");
    CHECK(http_status() == 409, "state status");
    CHECK_STR(http_body(),
              "{\"bus\":2,\"addr\":6,\"error\":\"state\",\"before\":\"unknown\","
              "\"out\":7}");

    /* Batch */
    fake_reset();
    fake_add(2, 3, SB_OUT_IDLE);
    fake_add(2, 4, SB_OUT_DOWN);
    api("POST", "/api/v1/batch",
        "{\"cmd\":\"up\",\"targets\":[{\"bus\":2,\"addr\":3},"
        " {\"addr\":4, \"bus\":2}, {\"bus\":2,\"addr\":5}]}");
    CHECK(http_status() == 200, "batch status");
    CHECK_STR(http_body(),
              "{\"results\":["
              "{\"bus\":2,\"addr\":3,\"before\":\"idle\",\"action\":\"press\","
              "\"after\":\"up\"},"
              "{\"bus\":2,\"addr\":4,\"before\":\"down\",\"action\":\"reverse\","
              "\"after\":\"up\"},"
              "{\"bus\":2,\"addr\":5,\"error\":\"no_reply\"}]}");

    api("POST", "/api/v1/batch",
        "{\"cmd\":\"up\",\"targets\":[{\"bus\":2,\"addr\":3},{\"bus\":2,"
        "\"addr\":3}]}");
    CHECK(http_status() == 400, "duplicate target");

    api("POST", "/api/v1/batch", "{\"cmd\":\"up\",\"targets\":[]}");
    CHECK(http_status() == 400, "empty targets");

    api("GET", "/api/v1/batch", NULL);
    CHECK(http_status() == 405, "batch GET");

    api("GET", "/api/v1/nope", NULL);
    CHECK(http_status() == 404 && response_signed() == 1, "unknown path");

    /* 16 worst-case results fit the response buffer */
    fake_reset();
    char body[1024];
    int n = snprintf(body, sizeof(body), "{\"cmd\":\"up\",\"targets\":[");
    for (int i = 0; i < 16; i++) {
        fake_module_t* mm = fake_add(4, (uint8_t)(200 + i), SB_OUT_DOWN);
        mm->drop_ack_at = 2;
        n += snprintf(body + n, sizeof(body) - n, "%s{\"bus\":4,\"addr\":%d}",
                      i ? "," : "", 200 + i);
    }
    snprintf(body + n, sizeof(body) - n, "]}");
    api("POST", "/api/v1/batch", body);
    const char* b = http_body();
    int results = 0;
    for (const char* p = b; (p = strstr(p, "\"bus\":4")) != NULL; p++) results++;
    CHECK(http_status() == 200 && results == 16 &&
              strcmp(b + strlen(b) - 2, "]}") == 0,
          "16 results (%d), body ends with ]}: %s", results, b + strlen(b) - 10);
}

int main(void) {
    test_frame();
    test_command_table();
    test_command_errors();
    test_batch();
    test_auth_helpers();
    test_auth_flow();
    test_stream_events();
    test_stream_frames();
    test_api();
    test_api_streams_and_tx();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
