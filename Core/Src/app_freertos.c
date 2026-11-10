/**
 * @file    app_freertos.c
 * @brief   FreeRTOS application — SimpleBus receiver task + LwIP network
 *          stack + HTTP API server
 */

#include "app_freertos.h"

#include <stdio.h>
#include <string.h>

#include "app_ethernet.h"
#include "cmsis_os2.h"
#include "ethernetif.h"
#include "http_sse.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/tcpip.h"
#include "main.h"
#include "simplebus_rx.h"
#include "simplebus_tx.h"
#include "sb_mac.h"
#include "sb_ring.h"

/* ─── Globals ────────────────────────────────────────────────────────────── */

struct netif gnetif;
sb_ring_t g_sb_ring;

/* ─── Network threads ────────────────────────────────────────────────────── */

static osThreadId_t EthLinkHandle;
static const osThreadAttr_t EthLinkThread_attributes = {
    .name = "EthLink",
    .priority = osPriorityNormal,
    .stack_size = 1024,
};

/* ─── Forward declarations ───────────────────────────────────────────────── */

static void net_init_task(void* argument);
static void rx_task(void* argument);
static void Netif_Config(void);

/* ─── FreeRTOS init (called from main before scheduler starts) ───────────── */

void MX_FREERTOS_Init(void) {
    sb_ring_init(&g_sb_ring);
    simplebus_tx_init();

    const osThreadAttr_t net_init_attr = {
        .name = "net_init",
        .stack_size = 2048,
        .priority = osPriorityNormal,
    };
    osThreadNew(net_init_task, NULL, &net_init_attr);

    const osThreadAttr_t rx_attr = {
        .name = "sb_rx",
        .stack_size = 2048,
        .priority = osPriorityNormal,
    };
    osThreadNew(rx_task, NULL, &rx_attr);
}

/* ─── Network initialisation (runs once, then self-terminates) ───────────── */

static void Netif_Config(void) {
    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gw;

#if LWIP_DHCP
    ip_addr_set_zero_ip4(&ipaddr);
    ip_addr_set_zero_ip4(&netmask);
    ip_addr_set_zero_ip4(&gw);
#else
    IP_ADDR4(&ipaddr, IP_ADDR0, IP_ADDR1, IP_ADDR2, IP_ADDR3);
    IP_ADDR4(&netmask, NETMASK_ADDR0, NETMASK_ADDR1, NETMASK_ADDR2,
             NETMASK_ADDR3);
    IP_ADDR4(&gw, GW_ADDR0, GW_ADDR1, GW_ADDR2, GW_ADDR3);
#endif

    netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init,
              &tcpip_input);
    netif_set_default(&gnetif);
    netif_set_up(&gnetif);

#if LWIP_NETIF_LINK_CALLBACK
    netif_set_link_callback(&gnetif, ethernet_link_status_updated);
    EthLinkHandle =
        osThreadNew(ethernet_link_thread, &gnetif, &EthLinkThread_attributes);
#endif
}

static void net_init_task(void* argument) {
    (void)argument;
    printf("[NET] Initializing...\r\n");

    tcpip_init(NULL, NULL);
    Netif_Config();

    const uint8_t* m = gnetif.hwaddr;
    printf("[NET] MAC %02x:%02x:%02x:%02x:%02x:%02x (%s)\r\n", m[0], m[1],
           m[2], m[3], m[4], m[5],
           sb_mac_is_fixed() ? "fixed by SB_ETH_MAC" : "from chip UID");

    /* Wait for link to come up */
    printf("[NET] Waiting for link...\r\n");
    while (!netif_is_link_up(&gnetif)) {
        osDelay(100);
    }

#if LWIP_DHCP
    printf("[NET] Link up, starting DHCP...\r\n");
    osDelay(2000);
    netifapi_dhcp_start(&gnetif);

    /* Wait up to 60 s for a lease. Without one the server still starts:
     * lwIP keeps retrying DHCP, and the listener (INADDR_ANY) works as soon
     * as an address arrives. There is no static fallback. */
    int bound = 0;
    for (int i = 0; i < 120 && !bound; i++) {
        osDelay(500);
        bound = dhcp_supplied_address(&gnetif);
    }
    if (bound) {
        char ip_str[16];
        ip_addr_t ipaddr;
        ip4_addr_set_u32(&ipaddr, netif_ip4_addr(&gnetif)->addr);
        ip4addr_ntoa_r(&ipaddr, ip_str, sizeof(ip_str));
        printf("[NET] DHCP address: %s\r\n", ip_str);
    } else {
        printf("[NET] No DHCP lease yet (still trying)\r\n");
    }
#else
    {
        char ip_str[16];
        ip_addr_t ipaddr;
        ip4_addr_set_u32(&ipaddr, netif_ip4_addr(&gnetif)->addr);
        ip4addr_ntoa_r(&ipaddr, ip_str, sizeof(ip_str));
        printf("[NET] Static IP: %s\r\n", ip_str);
    }
#endif

    printf("[NET] Starting HTTP/SSE server...\r\n");
    http_sse_init();
    printf("[NET] Ready\r\n");

    osThreadTerminate(osThreadGetId());
}

/* ─── SimpleBus receiver task ────────────────────────────────────────────── */

static void rx_task(void* argument) {
    (void)argument;

    /* Let network init print first */
    osDelay(2000);
    printf("[RX] SimpleBus receiver running\r\n");

    for (;;) {
        simplebus_rx_process();
        osDelay(1);
    }
}