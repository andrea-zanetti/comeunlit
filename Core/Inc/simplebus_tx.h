/**
 * @file    simplebus_tx.h
 * @brief   SimpleBus PLE transmitter, multi-bus, via SN74HCT125.
 *
 *          Buses are addressed by their PHYSICAL number (silkscreen,
 *          Bus n = terminal J(n+4)). Which buses exist, and on which pins,
 *          comes from the generated board_pins.h (hw/pins/shield_rev*.toml).
 *          Rev 1: buses 2, 3, 4.
 *
 *          The only way to transmit is simplebus_tx_bits_locked(), in the
 *          caller's task, with the bus lock held (sb_bus.h). Users:
 *          sb_bus_transact() (polls, presses) and sb_bus_raw() (admin TX).
 *
 *          Bits are timed by TIM7 + GPDMA1 (see simplebus_tx.c and
 *          sb_tx_table.h); interrupts stay enabled while transmitting.
 */

#ifndef SIMPLEBUS_TX_H
#define SIMPLEBUS_TX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TX_MAX_BITS 128
#define TX_MAX_BYTES 10

/**
 * Init the TX/OE pins (drivers tri-stated), the bus locks and the TIM7 +
 * GPDMA1 engine. Call after osKernelInitialize() (it creates RTOS objects).
 */
void simplebus_tx_init(void);

/**
 * Transmit on a bus the caller holds (sb_bus_lock): listen before talk,
 * send (blocking until the frame is out), echo into the frame ring.
 * Returns 0 sent, -1 unknown bus or bad frame, -2 bus busy (LBT timeout,
 * nothing sent; a "drop" frame is pushed to the frame ring), -3 transmitter
 * failure (timer/DMA did not complete; driver released, pin left idle).
 */
int simplebus_tx_bits_locked(uint8_t bus, const char* bits, uint8_t bit_len);

/** GPDMA1 channel 0 interrupt; called from GPDMA1_Channel0_IRQHandler. */
void simplebus_tx_dma_irq(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLEBUS_TX_H */
