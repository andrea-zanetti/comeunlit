/**
 ******************************************************************************
 * @file    stm32h5xx_it.c
 * @brief   Interrupt Service Routines.
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "stm32h5xx_it.h"

#include "main.h"
#include "simplebus_tx.h"

/* External variables --------------------------------------------------------*/
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim6;
extern ETH_HandleTypeDef EthHandle;

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
 * @brief This function handles Non maskable interrupt.
 */
void NMI_Handler(void) {
    while (1) {
    }
}

/**
 * @brief This function handles Hard fault interrupt.
 */
void HardFault_Handler(void) {
    while (1) {
    }
}

/**
 * @brief This function handles Memory management fault.
 */
void MemManage_Handler(void) {
    while (1) {
    }
}

/**
 * @brief This function handles Pre-fetch fault, memory access fault.
 */
void BusFault_Handler(void) {
    while (1) {
    }
}

/**
 * @brief This function handles Undefined instruction or illegal state.
 */
void UsageFault_Handler(void) {
    while (1) {
    }
}

/**
 * @brief This function handles Debug monitor.
 */
void DebugMon_Handler(void) {
}

/******************************************************************************/
/* STM32H5xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h5xx.s).                    */
/******************************************************************************/

/**
 * @brief This function handles EXTI Line13 interrupt.
 */
void EXTI13_IRQHandler(void) {
    BSP_PB_IRQHandler(BUTTON_USER);
}

/**
 * @brief This function handles TIM1 Capture Compare interrupt.
 */
void TIM1_CC_IRQHandler(void) {
    HAL_TIM_IRQHandler(&htim1);
}

/**
 * @brief This function handles TIM6 global interrupt.
 */
void TIM6_IRQHandler(void) {
    HAL_TIM_IRQHandler(&htim6);
}

/**
 * @brief This function handles GPDMA1 channel 0 (SimpleBus transmitter).
 */
void GPDMA1_Channel0_IRQHandler(void) { simplebus_tx_dma_irq(); }

/**
 * @brief This function handles Ethernet global interrupt.
 */
void ETH_IRQHandler(void) { HAL_ETH_IRQHandler(&EthHandle); }
