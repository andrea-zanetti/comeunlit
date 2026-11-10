/**
 * @file    simplebus_tx.c
 * @brief   SimpleBus PLE transmitter, multi-bus, via SN74HCT125.
 *
 *          Pins come from the generated board_pins.h; nothing is hardcoded
 *          here. Callers address buses by physical number.
 *
 *          Every transmission happens in the caller's task with the bus
 *          lock held (sb_bus.h): request/reply transactions and admin raw
 *          frames both call simplebus_tx_bits_locked().
 *
 *          Timing is done by hardware, with interrupts left on: TIM7 ticks
 *          every SB_TX_UNIT_US and requests a GPDMA1 transfer on each
 *          update; the DMA copies one word per tick from a table
 *          (sb_tx_table.c) into the data pin's GPIOx->BSRR. The calling task
 *          blocks on a semaphore until the DMA completes. One engine serves
 *          all buses: transmissions take turns (one 26 ms frame at a time).
 *
 *          While we transmit on a bus, that bus's receiver is muted so our
 *          own frame is not reported as received; it is echoed into the
 *          frame ring as "tx" instead. The other buses keep receiving.
 *
 *          ~OE is active LOW with external 10k pullup (boot-safe tri-state).
 *          Buses not present in the shield map are never touched, so their
 *          drivers stay tri-stated by the pullup.
 *
 *  NOTE:   PB6/PB7 are configured as LPUART1 AF by MX_GPIO_Init().
 *          tx_gpio_init() runs later and reconfigures them as outputs.
 */

#include "simplebus_tx.h"

#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "cmsis_os2.h"
#include "sb_bus.h"
#include "sb_ring.h"
#include "sb_tx_table.h"
#include "simplebus_rx.h"
#include "stm32h5xx_hal.h"

/* ─── Build options ─────────────────────────────────────────────────────── */

/* Listen-before-talk: before transmitting, wait until the bus has been idle
 * (RX line HIGH) for LBT_IDLE_US, giving up after LBT_TIMEOUT_US. Without it
 * the bridge talks over modules that are already transmitting, and with
 * several modules on a line the resulting collisions garble both frames.
 * Only disable (-DSB_TX_USE_LBT=0) for bench tests where RX is not working:
 * a dead receiver never reads idle, so every transmission would time out. */
#ifndef SB_TX_USE_LBT
#define SB_TX_USE_LBT 1
#endif

/* ─── Pin macros (i = internal table index) ─────────────────────────────── */

#define PIN_HIGH(g) ((g).port->BSRR = (g).pin)
#define PIN_LOW(g) ((g).port->BSRR = (uint32_t)(g).pin << 16U)
#define PIN_READ(g) (((g).port->IDR & (g).pin) != 0)

#define BUS_OE_ENABLE(i) PIN_LOW(sb_bus_pins[i].oe)
#define BUS_OE_DISABLE(i) PIN_HIGH(sb_bus_pins[i].oe)
#define BUS_DATA_HIGH(i) PIN_HIGH(sb_bus_pins[i].data)
#define BUS_RX_READ(i) PIN_READ(sb_bus_pins[i].rx)

/* ─── Timing ────────────────────────────────────────────────────────────── */

/* Listen-before-talk */
#define LBT_IDLE_US 2000
#define LBT_TIMEOUT_US 50000

/* Engine wait: the frame itself plus this margin before giving up. */
#define TX_DONE_MARGIN_MS 20
/* Longest wait for the engine if another bus is transmitting. */
#define TX_ENGINE_WAIT_MS 500

/* DMA interrupt priority: numerically >= configLIBRARY_MAX_SYSCALL_
 * INTERRUPT_PRIORITY (5), because its callback releases a semaphore. */
#define TX_DMA_IRQ_PRIO 6

/* ─── Engine state ──────────────────────────────────────────────────────── */

static TIM_HandleTypeDef htim7;
static DMA_HandleTypeDef hdma_tx;
static osSemaphoreId_t tx_done;
static osMutexId_t tx_engine;
static volatile int tx_status; /* 0 complete, -1 DMA error */
static uint32_t tx_table[SB_TX_TABLE_WORDS(TX_MAX_BITS)];

static uint32_t us_to_cycles;

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t ms_to_ticks(uint32_t ms) {
    uint32_t t = (uint32_t)(((uint64_t)ms * osKernelGetTickFreq()) / 1000U);
    return t ? t : 1;
}

static void dwt_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    us_to_cycles = HAL_RCC_GetHCLKFreq() / 1000000U;
}

/* ─── GPIO init ─────────────────────────────────────────────────────────── */

static void tx_gpio_init(void) {
    SB_GPIO_CLK_ENABLE_ALL();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    for (int i = 0; i < SB_NUM_BUSES; i++) {
        /* Preload output levels before switching to output mode */
        BUS_DATA_HIGH(i);
        BUS_OE_DISABLE(i);

        /* DATA pin: push-pull, idle HIGH */
        gpio.Pin = sb_bus_pins[i].data.pin;
        HAL_GPIO_Init(sb_bus_pins[i].data.port, &gpio);

        /* OE pin: push-pull, HIGH = driver tri-stated */
        gpio.Pin = sb_bus_pins[i].oe.pin;
        HAL_GPIO_Init(sb_bus_pins[i].oe.port, &gpio);
    }

    /* RX pins are configured as timer inputs by simplebus_rx_init(); IDR
     * reads correctly in AF mode, so LBT needs nothing extra here. */
}

/* ─── Timer + DMA engine ────────────────────────────────────────────────── */

static void dma_complete(DMA_HandleTypeDef* h) {
    (void)h;
    tx_status = 0;
    osSemaphoreRelease(tx_done);
}

static void dma_error(DMA_HandleTypeDef* h) {
    (void)h;
    tx_status = -1;
    osSemaphoreRelease(tx_done);
}

void simplebus_tx_dma_irq(void) { HAL_DMA_IRQHandler(&hdma_tx); }

static int engine_init(void) {
    tx_done = osSemaphoreNew(1, 0, NULL);
    static const osMutexAttr_t mattr = {.name = "sb_tx",
                                        .attr_bits = osMutexPrioInherit};
    tx_engine = osMutexNew(&mattr);
    if (tx_done == NULL || tx_engine == NULL) return -1;

    /* TIM7: free-running up-counter, one update (= one DMA request) every
     * SB_TX_UNIT_US. Its kernel clock is PCLK1, doubled when APB1 is
     * divided (same rule as the HAL time base on TIM6). */
    __HAL_RCC_TIM7_CLK_ENABLE();
    RCC_ClkInitTypeDef clk;
    uint32_t latency;
    HAL_RCC_GetClockConfig(&clk, &latency);
    uint32_t timclk = HAL_RCC_GetPCLK1Freq();
    if (clk.APB1CLKDivider != RCC_HCLK_DIV1) timclk *= 2U;

    htim7.Instance = TIM7;
    htim7.Init.Prescaler = 0;
    htim7.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim7.Init.Period = (timclk / 1000000U) * SB_TX_UNIT_US - 1U;
    htim7.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim7.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (htim7.Init.Period > 0xFFFFU) return -1; /* 16-bit timer */
    if (HAL_TIM_Base_Init(&htim7) != HAL_OK) return -1;

    /* GPDMA1 channel 0: memory (table) -> GPIOx->BSRR, one word per TIM7
     * update request. */
    __HAL_RCC_GPDMA1_CLK_ENABLE();
    hdma_tx.Instance = GPDMA1_Channel0;
    hdma_tx.Init.Request = GPDMA1_REQUEST_TIM7_UP;
    hdma_tx.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
    hdma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_tx.Init.SrcInc = DMA_SINC_INCREMENTED;
    hdma_tx.Init.DestInc = DMA_DINC_FIXED;
    hdma_tx.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_WORD;
    hdma_tx.Init.DestDataWidth = DMA_DEST_DATAWIDTH_WORD;
    hdma_tx.Init.Priority = DMA_HIGH_PRIORITY;
    hdma_tx.Init.SrcBurstLength = 1;
    hdma_tx.Init.DestBurstLength = 1;
    hdma_tx.Init.TransferAllocatedPort =
        DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1;
    hdma_tx.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
    hdma_tx.Init.Mode = DMA_NORMAL;
    if (HAL_DMA_Init(&hdma_tx) != HAL_OK) return -1;
    hdma_tx.XferCpltCallback = dma_complete;
    hdma_tx.XferErrorCallback = dma_error;

    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, TX_DMA_IRQ_PRIO, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    return 0;
}

static void engine_stop(void) {
    __HAL_TIM_DISABLE(&htim7);
    __HAL_TIM_DISABLE_DMA(&htim7, TIM_DMA_UPDATE);
}

/* Send a prepared table of n words on bus index i. Blocks until done.
 * Returns 0 sent, -3 engine failure (pin left HIGH, nothing pending). */
static int engine_send(uint8_t i, uint32_t n) {
    /* Drop a stale completion, if any. */
    while (osSemaphoreAcquire(tx_done, 0) == osOK) {
    }
    tx_status = -1;

    volatile uint32_t* bsrr = &sb_bus_pins[i].data.port->BSRR;
    if (HAL_DMA_Start_IT(&hdma_tx, (uint32_t)(uintptr_t)tx_table,
                         (uint32_t)(uintptr_t)bsrr,
                         n * sizeof(uint32_t)) != HAL_OK)
        return -3;

    __HAL_TIM_SET_COUNTER(&htim7, 0);
    __HAL_TIM_CLEAR_FLAG(&htim7, TIM_FLAG_UPDATE);
    __HAL_TIM_ENABLE_DMA(&htim7, TIM_DMA_UPDATE);
    __HAL_TIM_ENABLE(&htim7);

    uint32_t frame_ms = (n * SB_TX_UNIT_US + 999U) / 1000U;
    osStatus_t st =
        osSemaphoreAcquire(tx_done, ms_to_ticks(frame_ms + TX_DONE_MARGIN_MS));
    engine_stop();

    if (st != osOK || tx_status != 0) {
        HAL_DMA_Abort(&hdma_tx);
        BUS_DATA_HIGH(i);
        return -3;
    }
    return 0;
}

/* ─── Bus monitoring ────────────────────────────────────────────────────── */

#if SB_TX_USE_LBT
static int listen_before_talk(uint8_t i) {
    uint32_t t0 = DWT->CYCCNT;
    uint32_t idle_start = t0;

    while ((DWT->CYCCNT - idle_start) < (LBT_IDLE_US * us_to_cycles)) {
        if (!BUS_RX_READ(i)) idle_start = DWT->CYCCNT;
        if ((DWT->CYCCNT - t0) > (LBT_TIMEOUT_US * us_to_cycles)) return -1;
    }
    return 0;
}
#endif

/* ─── Transmit (caller holds the bus lock) ──────────────────────────────── */

int simplebus_tx_bits_locked(uint8_t bus, const char* bits, uint8_t bit_len) {
    int idx = sb_bus_index(bus);
    if (idx < 0 || bit_len == 0 || bit_len > TX_MAX_BITS) return -1;
    uint8_t i = (uint8_t)idx;

    if (tx_engine == NULL) return -3;
    if (osMutexAcquire(tx_engine, ms_to_ticks(TX_ENGINE_WAIT_MS)) != osOK)
        return -3;

    int rc = 0;
    uint32_t n = sb_tx_build(bits, bit_len, sb_bus_pins[i].data.pin, tx_table,
                             SB_TX_TABLE_WORDS(TX_MAX_BITS));
    if (n == 0) {
        rc = -1;
        goto out;
    }

#if SB_TX_USE_LBT
    if (listen_before_talk(i) < 0) {
        printf("[TX] Bus %u LBT timeout\r\n", bus);
        /* Show the drop in the frame stream, so a missing reply is explained */
        sb_ring_push(&g_sb_ring, HAL_GetTick(), bus, SB_DIR_DROP, bits,
                     bit_len);
        rc = -2;
        goto out;
    }
#endif

    /* Mute our own receiver, enable driver, transmit, disable driver. */
    simplebus_rx_mute(i, 1);
    BUS_OE_ENABLE(i);
    rc = engine_send(i, n);
    BUS_OE_DISABLE(i);
    simplebus_rx_mute(i, 0);

    if (rc == 0) {
        /* Echo into the frame ring: the receiver was muted for our own
         * frame, so without this the frame stream would only show the
         * modules' side of the conversation. */
        sb_ring_push(&g_sb_ring, HAL_GetTick(), bus, SB_DIR_TX, bits,
                     bit_len);
    } else {
        printf("[TX] Bus %u transmit failed (timer/DMA)\r\n", bus);
    }

out:
    osMutexRelease(tx_engine);
    return rc;
}

/* ─── Public API ────────────────────────────────────────────────────────── */

void simplebus_tx_init(void) {
    dwt_init();
    tx_gpio_init();
    sb_bus_init();
    int eng = engine_init();

    printf("[TX] shield rev %s, buses:", SB_SHIELD_REV);
    for (int i = 0; i < SB_NUM_BUSES; i++)
        printf(" %u(%s)", sb_bus_pins[i].id, sb_bus_pins[i].term);
    printf(", LBT %s, %s\r\n", SB_TX_USE_LBT ? "on" : "off",
           eng == 0 ? "TIM7+GPDMA1" : "ENGINE INIT FAILED");
}
