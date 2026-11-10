/**
 * @file    sb_crypto.c
 * @brief   Hardware HMAC-SHA256 (HASH) and RNG. See sb_crypto.h.
 *
 *          Not managed by CubeMX: clocks are enabled in the MSP callbacks
 *          below. The RNG runs from HSI48, which this file switches on;
 *          HAL_RNG_Init applies ST's NIST-compliant configuration itself.
 */

#include "sb_crypto.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "sb_secret.h"
#include "stm32h5xx_hal.h"

#define HASH_TIMEOUT_MS 10

static HASH_HandleTypeDef hhash;
static RNG_HandleTypeDef hrng;
static osMutexId_t hash_mutex;
static volatile int crypto_ok;

/* ─── MSP: clocks (HAL weak callbacks) ──────────────────────────────────── */

void HAL_HASH_MspInit(HASH_HandleTypeDef* h) {
    (void)h;
    __HAL_RCC_HASH_CLK_ENABLE();
}

void HAL_HASH_MspDeInit(HASH_HandleTypeDef* h) {
    (void)h;
    __HAL_RCC_HASH_CLK_DISABLE();
}

void HAL_RNG_MspInit(RNG_HandleTypeDef* h) {
    (void)h;

    /* HSI48 on, without touching the other oscillators or the PLL. */
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    osc.HSI48State = RCC_HSI48_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
        printf("[CRYPTO] HSI48 start failed\r\n");

    RCC_PeriphCLKInitTypeDef clk = {0};
    clk.PeriphClockSelection = RCC_PERIPHCLK_RNG;
    clk.RngClockSelection = RCC_RNGCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
        printf("[CRYPTO] RNG clock select failed\r\n");

    __HAL_RCC_RNG_CLK_ENABLE();
}

void HAL_RNG_MspDeInit(RNG_HandleTypeDef* h) {
    (void)h;
    __HAL_RCC_RNG_CLK_DISABLE();
}

/* ─── Helpers ───────────────────────────────────────────────────────────── */

int sb_ct_equal(const uint8_t* a, const uint8_t* b, uint32_t len) {
    uint8_t d = 0;
    for (uint32_t i = 0; i < len; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static int hash_init(void) {
    memset(&hhash, 0, sizeof(hhash));
    hhash.Instance = HASH;
    hhash.Init.DataType = HASH_BYTE_SWAP; /* byte stream input */
    hhash.Init.Algorithm = HASH_ALGOSELECTION_SHA256;
    return (HAL_HASH_Init(&hhash) == HAL_OK) ? 0 : -1;
}

/* Caller holds hash_mutex. */
static int hmac_locked(const uint8_t* key, uint32_t key_len,
                       const uint8_t* msg, uint32_t len, uint8_t* mac) {
    hhash.Init.pKey = (uint8_t*)key; /* HAL field is non-const, only read */
    hhash.Init.KeySize = key_len;

    if (HAL_HASH_HMAC_Start(&hhash, msg, len, mac, HASH_TIMEOUT_MS) == HAL_OK)
        return 0;

    /* Leave the unit in a known state for the next caller. */
    HAL_HASH_DeInit(&hhash);
    hash_init();
    return -1;
}

static int hmac_any(const uint8_t* key, uint32_t key_len, const uint8_t* msg,
                    uint32_t len, uint8_t* mac, int require_ok) {
    if (hash_mutex == NULL) return -1;
    if (require_ok && !crypto_ok) return -1;
    if (len > SB_HMAC_MAX_MSG) return -1;

    if (osMutexAcquire(hash_mutex, osWaitForever) != osOK) return -1;
    int rc = hmac_locked(key, key_len, msg, len, mac);
    osMutexRelease(hash_mutex);
    return rc;
}

int sb_hmac_key(const uint8_t* key, uint32_t key_len, const uint8_t* msg,
                uint32_t len, uint8_t mac[SB_HMAC_LEN]) {
    return hmac_any(key, key_len, msg, len, mac, 0);
}

int sb_hmac(const uint8_t* msg, uint32_t len, uint8_t mac[SB_HMAC_LEN]) {
    return hmac_any(sb_secret_key, SB_SECRET_KEY_LEN, msg, len, mac, 1);
}

int sb_random(void* out, uint32_t len) {
    uint8_t* p = (uint8_t*)out;
    while (len > 0) {
        uint32_t r;
        if (HAL_RNG_GenerateRandomNumber(&hrng, &r) != HAL_OK) return -1;
        uint32_t n = (len < 4) ? len : 4;
        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
    return 0;
}

/* ─── Self-tests ────────────────────────────────────────────────────────── */

typedef struct {
    const char* name;
    const uint8_t* key;
    uint32_t key_len;
    const char* msg;
    const char* mac_hex;
} kat_t;

static const uint8_t key_tc1[20] = {
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
};
static const uint8_t key_tc2[4] = {'J', 'e', 'f', 'e'};
static const uint8_t key_api[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static const kat_t kats[] = {
    /* RFC 4231 4.2, test case 1 */
    {"RFC4231-1", key_tc1, sizeof(key_tc1), "Hi There",
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
    /* RFC 4231 4.3, test case 2: 4-byte key */
    {"RFC4231-2", key_tc2, sizeof(key_tc2), "what do ya want for nothing?",
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
    /* API shape: 32-byte key, 46-byte message (not a multiple of 4).
     * Reference: Python hmac.new(bytes(range(32)), msg, sha256). */
    {"API-shape", key_api, sizeof(key_api),
     "SB1-REQ\nGET\n/api/v1/health\n0123456789abcdef\n7\n",
     "98810fe6caefeb1b7fd403f01447425a773b90b5242644ef836e6efd7e7b136e"},
    /* Repeat of case 1: the unit must be reusable back to back. */
    {"RFC4231-1 again", key_tc1, sizeof(key_tc1), "Hi There",
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
};

static void hex_to_bytes(const char* hex, uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static int self_test(void) {
    int failed = 0;
    for (unsigned i = 0; i < sizeof(kats) / sizeof(kats[0]); i++) {
        const kat_t* k = &kats[i];
        uint8_t want[SB_HMAC_LEN], got[SB_HMAC_LEN];
        hex_to_bytes(k->mac_hex, want, SB_HMAC_LEN);
        memset(got, 0, sizeof(got));

        int rc = sb_hmac_key(k->key, k->key_len, (const uint8_t*)k->msg,
                             (uint32_t)strlen(k->msg), got);
        if (rc != 0 || !sb_ct_equal(got, want, SB_HMAC_LEN)) {
            printf("[CRYPTO] self-test %s FAILED (rc %d, got %02x%02x%02x%02x...)\r\n",
                   k->name, rc, got[0], got[1], got[2], got[3]);
            failed++;
        }
    }

    /* RNG: two draws must differ and not be stuck at 0 / all ones. */
    uint32_t a = 0, b = 0;
    if (sb_random(&a, 4) != 0 || sb_random(&b, 4) != 0 || a == b ||
        a == 0 || a == 0xFFFFFFFFu) {
        printf("[CRYPTO] RNG self-test FAILED (%08lx %08lx)\r\n",
               (unsigned long)a, (unsigned long)b);
        failed++;
    }
    return failed;
}

/* ─── Init ──────────────────────────────────────────────────────────────── */

int sb_crypto_init(void) {
    static const osMutexAttr_t attr = {
        .name = "sb_hash",
        .attr_bits = osMutexPrioInherit,
    };
    crypto_ok = 0;

    hash_mutex = osMutexNew(&attr);
    if (hash_mutex == NULL) {
        printf("[CRYPTO] mutex failed\r\n");
        return -1;
    }
    if (hash_init() != 0) {
        printf("[CRYPTO] HASH init failed\r\n");
        return -1;
    }

    memset(&hrng, 0, sizeof(hrng));
    hrng.Instance = RNG;
    hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
    if (HAL_RNG_Init(&hrng) != HAL_OK) {
        printf("[CRYPTO] RNG init failed (error 0x%lx)\r\n",
               (unsigned long)hrng.ErrorCode);
        return -1;
    }

    int failed = self_test();
    if (failed) {
        printf("[CRYPTO] %d self-test(s) failed: API disabled\r\n", failed);
        return -1;
    }

    crypto_ok = 1;
    uint32_t r[2] = {0, 0};
    sb_random(r, sizeof(r));
    printf("[CRYPTO] HASH HMAC-SHA256 + RNG OK (4 HMAC vectors, RNG); "
           "boot random %08lx%08lx\r\n",
           (unsigned long)r[0], (unsigned long)r[1]);
    return 0;
}

int sb_crypto_ok(void) { return crypto_ok; }
