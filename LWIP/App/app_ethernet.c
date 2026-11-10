/**
 ******************************************************************************
 * @file    LwIP/LwIP_HTTP_Server_Socket_RTOS/LWIP/App/app_ethernet.c
 * @author  MCD Application Team
 * @brief   Ethernet specific module
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2024 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* Includes ------------------------------------------------------------------*/
#include "app_ethernet.h"

#include "lwip/opt.h"
#include "main.h"

/**
 * @brief  Link-change callback (netif_set_link_callback in app_freertos.c).
 * @param  netif: the network interface
 * @retval None
 */
void ethernet_link_status_updated(struct netif* netif) {
#if LWIP_DHCP
    /* Nothing to do: lwIP's DHCP client restarts on its own when the link
     * comes back up (netif_set_link_up). */
    (void)netif;
#else
    if (netif_is_up(netif)) {
        ip_addr_t ipaddr;
        char ip_str[16];
        ip4_addr_set_u32(&ipaddr, netif_ip4_addr(netif)->addr);
        ip4addr_ntoa_r(&ipaddr, ip_str, sizeof(ip_str));
        printf("Static IPv4 address: %s\r\n", ip_str);
        BSP_LED_On(LED_YELLOW);
        BSP_LED_Off(LED_RED);
    } else {
        printf("The network cable is not connected \r\n");
        BSP_LED_Off(LED_YELLOW);
        BSP_LED_On(LED_RED);
    }
#endif /* LWIP_DHCP */
}
