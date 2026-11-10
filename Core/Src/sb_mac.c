/**
 * @file    sb_mac.c
 * @brief   Ethernet MAC address. See sb_mac.h.
 */

#include "sb_mac.h"

#include <string.h>

#include "stm32h5xx_hal.h"

void sb_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a offset basis */
    for (int w = 0; w < 3; w++) {
        for (int b = 0; b < 4; b++) {
            h ^= (uint8_t)(uid[w] >> (8 * b));
            h *= 0x00000100000001b3ULL; /* FNV-1a prime */
        }
    }
    /* Upper six bytes: FNV's low bits are the weakest mixed. */
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(h >> (16 + 8 * i));
    mac[0] = (uint8_t)((mac[0] & 0xFC) | 0x02); /* unicast, locally admin. */
}

int sb_mac_is_fixed(void) {
#ifdef SB_ETH_MAC_FIXED
    return 1;
#else
    return 0;
#endif
}

void sb_mac_get(uint8_t mac[6]) {
#ifdef SB_ETH_MAC_FIXED
    static const uint8_t fixed[6] = {SB_ETH_MAC_FIXED};
    memcpy(mac, fixed, sizeof(fixed));
#else
    const uint32_t uid[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    sb_mac_from_uid(uid, mac);
#endif
}
