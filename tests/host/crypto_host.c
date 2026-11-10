/* Host stand-in for sb_crypto.c: same API, OpenSSL HMAC instead of the
 * HASH peripheral. Test key = bytes 0x00..0x1f (as in sb_crypto's
 * "API-shape" self-test vector). */
#include <openssl/hmac.h>
#include <string.h>

#include "sb_crypto.h"

const uint8_t host_test_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
int host_crypto_ok = 1;

int sb_crypto_init(void) { return 0; }
int sb_crypto_ok(void) { return host_crypto_ok; }

int sb_hmac_key(const uint8_t* key, uint32_t key_len, const uint8_t* msg,
                uint32_t len, uint8_t mac[SB_HMAC_LEN]) {
    if (len > SB_HMAC_MAX_MSG) return -1;
    unsigned int n = 0;
    return HMAC(EVP_sha256(), key, (int)key_len, msg, len, mac, &n) && n == 32
               ? 0 : -1;
}

int sb_hmac(const uint8_t* msg, uint32_t len, uint8_t mac[SB_HMAC_LEN]) {
    if (!host_crypto_ok) return -1;
    return sb_hmac_key(host_test_key, 32, msg, len, mac);
}

int sb_random(void* out, uint32_t len) {
    static uint8_t x = 0x5a;
    for (uint32_t i = 0; i < len; i++) ((uint8_t*)out)[i] = (x = (uint8_t)(x * 33 + 7));
    return 0;
}

int sb_ct_equal(const uint8_t* a, const uint8_t* b, uint32_t len) {
    uint8_t d = 0;
    for (uint32_t i = 0; i < len; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
