/* Host tests for sb_mac.c (MAC address from the chip UID). */
#include <stdio.h>
#include <string.h>

#include "sb_mac.h"

static int checks, failures;
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

/* The UID the "chip" reports (stands in for the UID registers). */
static uint32_t uid_regs[3] = {0x00470031u, 0x3233510fu, 0x34343730u};
uint32_t HAL_GetUIDw0(void) { return uid_regs[0]; }
uint32_t HAL_GetUIDw1(void) { return uid_regs[1]; }
uint32_t HAL_GetUIDw2(void) { return uid_regs[2]; }

static inline int is_laa_unicast(const uint8_t m[6]) { return (m[0] & 0x03) == 0x02; }

int main(void) {
    uint8_t a[6], b[6];

#ifdef SB_ETH_MAC_FIXED
    static const uint8_t want[6] = {SB_ETH_MAC_FIXED};
    sb_mac_get(a);
    CHECK(sb_mac_is_fixed() == 1, "fixed build reports fixed");
    CHECK(memcmp(a, want, 6) == 0, "fixed address returned as given");
    uid_regs[0] ^= 1;
    sb_mac_get(b);
    CHECK(memcmp(a, b, 6) == 0, "fixed address ignores the UID");
    printf("mac (fixed): %d checks, %d failures\n", checks, failures);
#else
    CHECK(sb_mac_is_fixed() == 0, "default build reports derived");

    sb_mac_get(a);
    sb_mac_get(b);
    CHECK(memcmp(a, b, 6) == 0, "same UID, same address (stable)");
    CHECK(is_laa_unicast(a), "first octet %02x is unicast + locally admin.", a[0]);

    const uint32_t uid[3] = {uid_regs[0], uid_regs[1], uid_regs[2]};
    sb_mac_from_uid(uid, b);
    CHECK(memcmp(a, b, 6) == 0, "sb_mac_get uses sb_mac_from_uid on the UID");

    /* Known answer, computed independently (Python): FNV-1a 64 of the 12
     * little-endian UID bytes is 0xd5b4a093e3b6752b; bytes 2..7 give
     * b6 e3 93 a0 b4 d5, first octet (b6 & fc) | 02 = b6. */
    static const uint8_t kat[6] = {0xb6, 0xe3, 0x93, 0xa0, 0xb4, 0xd5};
    CHECK(memcmp(a, kat, 6) == 0, "known answer: got %02x:%02x:%02x:%02x:%02x:%02x",
          a[0], a[1], a[2], a[3], a[4], a[5]);

    /* Every single-bit change of the UID gives a different, valid address. */
    int distinct = 1, valid = 1;
    for (int w = 0; w < 3; w++) {
        for (int bit = 0; bit < 32; bit++) {
            uint32_t u[3] = {uid[0], uid[1], uid[2]};
            u[w] ^= 1u << bit;
            sb_mac_from_uid(u, b);
            if (memcmp(a, b, 6) == 0) distinct = 0;
            if (!is_laa_unicast(b)) valid = 0;
        }
    }
    CHECK(distinct, "all 96 one-bit UID changes give a new address");
    CHECK(valid, "all derived addresses are unicast + locally administered");
    printf("mac: %d checks, %d failures\n", checks, failures);
#endif
    return failures ? 1 : 0;
}
