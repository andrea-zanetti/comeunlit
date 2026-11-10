/**
 * @file    sb_mac.h
 * @brief   Ethernet MAC address of this board.
 *
 *          By default the address is derived from the chip's 96-bit unique
 *          ID, so every board gets its own, stable across reflashes. It is a
 *          locally administered unicast address (first octet x2, x6, xA or
 *          xE), so it cannot clash with any vendor-assigned one.
 *
 *          A build can pin a fixed address instead, e.g. to keep a DHCP
 *          reservation: cmake -DSB_ETH_MAC=aa:bb:cc:dd:ee:ff (CMakeLists.txt
 *          turns it into SB_ETH_MAC_FIXED for sb_mac.c only).
 */

#ifndef SB_MAC_H
#define SB_MAC_H

#include <stdint.h>

/** The address to use (fixed at build time, or derived from the UID). */
void sb_mac_get(uint8_t mac[6]);

/** 1 if the build pinned the address with SB_ETH_MAC, 0 if it is derived. */
int sb_mac_is_fixed(void);

/**
 * Derivation used by sb_mac_get(): 64-bit FNV-1a over the 12 UID bytes,
 * then the unicast + locally-administered bits forced in the first octet.
 * Pure function, exposed for the host tests.
 */
void sb_mac_from_uid(const uint32_t uid[3], uint8_t mac[6]);

#endif /* SB_MAC_H */
