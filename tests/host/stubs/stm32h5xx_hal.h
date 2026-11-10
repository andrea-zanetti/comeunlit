/* Host stub: just enough HAL for simplebus_rx.c and sb_mac.c (host tests). */
#ifndef HOST_STUB_STM32H5XX_HAL_H
#define HOST_STUB_STM32H5XX_HAL_H
#include <stdint.h>
typedef struct { uint32_t dummy; } GPIO_TypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef struct { void* Instance; } TIM_HandleTypeDef;
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_LOW 0u
static inline void HAL_GPIO_Init(GPIO_TypeDef* p, GPIO_InitTypeDef* g) { (void)p; (void)g; }
static inline int HAL_TIM_IC_Start_IT(TIM_HandleTypeDef* h, uint32_t c) { (void)h; (void)c; return 0; }
uint32_t HAL_GetTick(void);
uint32_t HAL_GetUIDw0(void);
uint32_t HAL_GetUIDw1(void);
uint32_t HAL_GetUIDw2(void);
#endif
