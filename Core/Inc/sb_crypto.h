/**
 * @file    sb_crypto.h
 * @brief   HMAC-SHA256 on the hardware HASH unit, random numbers from the
 *          hardware RNG, and boot self-tests.
 *
 *          The HASH unit is shared (HTTP task, stream tasks): every call is
 *          serialized by a mutex. Messages are hashed in one piece, at most
 *          SB_HMAC_MAX_MSG bytes.
 *
 *          sb_crypto_init() must run in task context (it creates a mutex),
 *          before any other call. It runs the RFC 4231 known-answer tests on
 *          the peripheral; if they fail, sb_crypto_ok() stays 0 and every
 *          sb_hmac*() call fails, so nothing is ever signed with a
 *          misconfigured unit.
 */

#ifndef SB_CRYPTO_H
#define SB_CRYPTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_HMAC_LEN 32u
#define SB_HMAC_MAX_MSG 2560u /* request header part + body, or a full batch response */

/** Init HASH + RNG, run self-tests. Returns 0 if all passed. */
int sb_crypto_init(void);

/** 1 once sb_crypto_init() has passed its self-tests. */
int sb_crypto_ok(void);

/** HMAC-SHA256 with the build-time API key (sb_secret.h). 0 on success. */
int sb_hmac(const uint8_t* msg, uint32_t len, uint8_t mac[SB_HMAC_LEN]);

/** HMAC-SHA256 with an explicit key (self-tests). 0 on success. */
int sb_hmac_key(const uint8_t* key, uint32_t key_len, const uint8_t* msg,
                uint32_t len, uint8_t mac[SB_HMAC_LEN]);

/** Fill out with len random bytes from the hardware RNG. 0 on success. */
int sb_random(void* out, uint32_t len);

/** Constant-time comparison: 1 if equal. */
int sb_ct_equal(const uint8_t* a, const uint8_t* b, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* SB_CRYPTO_H */
