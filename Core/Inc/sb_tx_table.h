/**
 * @file    sb_tx_table.h
 * @brief   SimpleBus frame -> GPIO BSRR word table for the DMA transmitter.
 *
 *          The transmitter (simplebus_tx.c) has TIM7 tick every
 *          SB_TX_UNIT_US and GPDMA copy one 32-bit word per tick from this
 *          table into the data pin's GPIOx->BSRR: pin set, pin reset, or 0
 *          (no change). Every edge therefore falls on the SB_TX_UNIT_US grid:
 *
 *            LOW pulse      2 units = 186 us
 *            HIGH, bit '1'  2 units = 186 us
 *            HIGH, bit '0'  3 units = 279 us
 *
 *          A frame is its symbols followed by a closing LOW pulse, then the
 *          pin goes back HIGH (idle): 59 symbols -> 60 LOW pulses, the same
 *          shape as a module's frame (docs/simplebus_protocol_reference.md,
 *          section 2). Pure function, no hardware: host-tested.
 */

#ifndef SB_TX_TABLE_H
#define SB_TX_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_TX_UNIT_US 93U
#define SB_TX_LOW_UNITS 2U
#define SB_TX_HI1_UNITS 2U
#define SB_TX_HI0_UNITS 3U

/** Table words needed for a frame of n symbols (worst case: all '0'). */
#define SB_TX_TABLE_WORDS(n) \
    ((uint32_t)(n) * (SB_TX_LOW_UNITS + SB_TX_HI0_UNITS) + SB_TX_LOW_UNITS + 1U)

/**
 * Build the BSRR word table for `bits` ('0'/'1', bit_len symbols) on the GPIO
 * pin `pin_mask` (one GPIO_PIN_x bit, 1..0x8000).
 * Returns the number of words written to `out`, or 0 if an argument is bad
 * (empty frame, a character other than '0'/'1', a pin mask that is not a
 * single bit, or a table that does not fit in out_max words).
 */
uint32_t sb_tx_build(const char* bits, uint8_t bit_len, uint32_t pin_mask,
                     uint32_t* out, uint32_t out_max);

#ifdef __cplusplus
}
#endif

#endif /* SB_TX_TABLE_H */
