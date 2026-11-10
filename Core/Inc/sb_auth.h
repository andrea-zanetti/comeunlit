/**
 * @file    sb_auth.h
 * @brief   API v1 request authentication and response signing
 *          (docs/API.md section 3).
 *
 *          Request:  X-SB-Session, X-SB-Counter, X-SB-MAC headers;
 *                    MAC = HMAC(key, "SB1-REQ\n" METHOD \n PATH \n SESSION \n
 *                                    COUNTER \n BODY)
 *          Response: X-SB-MAC = HMAC(key, "SB1-RSP\n" REQUEST_MAC \n STATUS \n
 *                                    BODY)
 *          Event:    MAC = HMAC(key, "SB1-EVT\n" REQUEST_MAC \n SEQ \n EVENT \n
 *                               JSON)
 *
 *          Session: 64-bit random per boot. Counter: client-chosen, accepted
 *          once, with a 64-entry sliding window (RFC 4303 3.4.3). State is
 *          RAM only.
 *
 *          Not thread-safe: sb_auth_check() and sb_auth_sign_response() are
 *          called from the HTTP task only. sb_auth_sign_event() takes the
 *          caller's scratch buffer and may be called from any task.
 *
 *          Uses only sb_crypto.h, so it builds on the host (tests/host).
 */

#ifndef SB_AUTH_H
#define SB_AUTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_AUTH_SESSION_HEX 16 /* chars */
#define SB_AUTH_MAC_HEX 64     /* chars */

typedef enum {
    SB_AUTH_OK = 0,
    SB_AUTH_BAD_SESSION, /* MAC valid, session is not the current one     */
    SB_AUTH_REPLAY,      /* MAC valid, counter already used / too old     */
    SB_AUTH_BAD_MAC,     /* headers missing/malformed or MAC invalid      */
    SB_AUTH_UNAVAILABLE, /* crypto self-test failed or auth not initialised */
} sb_auth_result_t;

typedef struct {
    int mac_valid;                   /* request MAC verified with the key */
    char req_mac[SB_AUTH_MAC_HEX + 1]; /* request MAC (hex) if mac_valid  */
} sb_auth_ctx_t;

/** Set the session for this boot (8 random bytes) and reset the window. */
void sb_auth_init(const uint8_t session[8]);

/** Current session as 16 lowercase hex digits ("" before init). */
const char* sb_auth_session_hex(void);

/** One above the highest accepted counter (what a client should send). */
uint64_t sb_auth_next(void);

/**
 * Check one request. `req` is the request buffer starting at the request
 * line and containing the full header block ("\r\n\r\n"); body/body_len is
 * the body. The window advances only on SB_AUTH_OK.
 */
sb_auth_result_t sb_auth_check(const char* req, const char* body,
                               uint32_t body_len, sb_auth_ctx_t* ctx);

/** "session", "replay", "mac" or "unavailable". */
const char* sb_auth_reason(sb_auth_result_t r);

/** JSON body of the 401 for result r (API.md 3.4). Returns its length. */
int sb_auth_401_body(char* out, int out_sz, sb_auth_result_t r);

/** X-SB-MAC for a response. Returns 0 on success. */
int sb_auth_sign_response(const char* req_mac_hex, int status,
                          const char* body, uint32_t len,
                          char out_hex[SB_AUTH_MAC_HEX + 1]);

/** MAC for one stream event, using the caller's scratch buffer. */
int sb_auth_sign_event(const char* req_mac_hex, uint32_t seq,
                       const char* event, const char* json, uint32_t len,
                       uint8_t* scratch, uint32_t cap,
                       char out_hex[SB_AUTH_MAC_HEX + 1]);

/* ─── Exposed for tests ─────────────────────────────────────────────────── */

/** Replay window: 1 if counter c is accepted (and recorded), 0 if not. */
int sb_auth_window_accept(uint64_t c);

/** Decimal u64 helpers (no printf %llu dependency). */
int sb_u64_to_dec(uint64_t v, char* out, int out_sz);
int sb_dec_to_u64(const char* s, int len, uint64_t* v);

#ifdef __cplusplus
}
#endif

#endif /* SB_AUTH_H */
