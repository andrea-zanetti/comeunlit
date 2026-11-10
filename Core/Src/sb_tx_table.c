/**
 * @file    sb_tx_table.c
 * @brief   SimpleBus frame -> GPIO BSRR word table. See sb_tx_table.h.
 */

#include "sb_tx_table.h"

/* Append `level` held for `units` ticks: the edge word, then holds (0). */
static int put(uint32_t* out, uint32_t* n, uint32_t out_max, uint32_t word,
               uint32_t units) {
    if (*n + units > out_max) return 0;
    out[(*n)++] = word;
    for (uint32_t k = 1; k < units; k++) out[(*n)++] = 0;
    return 1;
}

uint32_t sb_tx_build(const char* bits, uint8_t bit_len, uint32_t pin_mask,
                     uint32_t* out, uint32_t out_max) {
    if (bits == 0 || out == 0 || bit_len == 0) return 0;
    if (pin_mask == 0 || pin_mask > 0xFFFFU || (pin_mask & (pin_mask - 1U)))
        return 0;

    const uint32_t set = pin_mask;          /* BSRR BSx: pin HIGH */
    const uint32_t reset = pin_mask << 16U; /* BSRR BRx: pin LOW  */
    uint32_t n = 0;

    for (uint8_t b = 0; b < bit_len; b++) {
        uint32_t hi;
        if (bits[b] == '1')
            hi = SB_TX_HI1_UNITS;
        else if (bits[b] == '0')
            hi = SB_TX_HI0_UNITS;
        else
            return 0;
        if (!put(out, &n, out_max, reset, SB_TX_LOW_UNITS)) return 0;
        if (!put(out, &n, out_max, set, hi)) return 0;
    }

    /* Closing LOW pulse, then back to idle HIGH. The final set is the last
     * word: the DMA transfer ends exactly when the frame does. */
    if (!put(out, &n, out_max, reset, SB_TX_LOW_UNITS)) return 0;
    if (!put(out, &n, out_max, set, 1)) return 0;
    return n;
}
