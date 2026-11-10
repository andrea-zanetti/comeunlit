cmake_minimum_required(VERSION 3.20)

# Toolchain must be set before project()
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake)

project(simplebus_bridge C ASM)

set(CMAKE_C_STANDARD 11)

# ─── Target MCU ──────────────────────────────────────────────────────────────
set(MCU_FAMILY STM32H5xx)
set(MCU_DEVICE STM32H563xx)
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/STM32H563ZITX_FLASH.ld)

# ─── Compile definitions ────────────────────────────────────────────────────
add_compile_definitions(
    USE_HAL_DRIVER
    ${MCU_DEVICE}
)

# ─── Compile options ────────────────────────────────────────────────────────
add_compile_options(
    -Wall
    -ffunction-sections
    -fdata-sections
    -fstack-usage
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-Og -g3)
else()
    add_compile_options(-Os)
endif()

# ─── Pin map (generated) ───────────────────────────────────────────────────
# Board + mating + shield TOML -> validated C header (board_pins.h).
# Select the shield with: cmake -DSHIELD_REV=<n> ..
# Rev 1 = buses 2-4 (no bodge); rev 2 = buses 1-4.
set(SHIELD_REV "1" CACHE STRING "Shield revision (selects hw/pins/shield_rev<N>.toml)")

set(PINS_DIR      ${CMAKE_CURRENT_SOURCE_DIR}/hw/pins)
set(PINS_BOARD    ${PINS_DIR}/board_nucleo_h563zi.toml)
set(PINS_MATING   ${PINS_DIR}/mating_zio.toml)
set(PINS_SHIELD   ${PINS_DIR}/shield_rev${SHIELD_REV}.toml)
set(PINS_GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PINS_HEADER   ${PINS_GEN_DIR}/board_pins.h)

if(NOT EXISTS ${PINS_SHIELD})
    message(FATAL_ERROR "No shield map for SHIELD_REV=${SHIELD_REV}: ${PINS_SHIELD}")
endif()

file(MAKE_DIRECTORY ${PINS_GEN_DIR})

add_custom_command(
    OUTPUT  ${PINS_HEADER}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_pins.py
            --board  ${PINS_BOARD}
            --mating ${PINS_MATING}
            --shield ${PINS_SHIELD}
            --out    ${PINS_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_pins.py
            ${PINS_BOARD} ${PINS_MATING} ${PINS_SHIELD}
    COMMENT "Generating board_pins.h (shield rev ${SHIELD_REV})"
    VERBATIM
)

# ─── API secret (generated) ─────────────────────────────────────────────────
# secrets.toml (gitignored) -> sb_secret.h with the API HMAC key.
# See docs/API.md section 8 and secrets.example.toml.
set(SECRETS_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/secrets.toml
    CACHE FILEPATH "TOML file with the API HMAC key ([auth] key)")
set(SECRET_HEADER  ${PINS_GEN_DIR}/sb_secret.h)

if(NOT EXISTS ${SECRETS_FILE})
    message(FATAL_ERROR
        "No API secret: ${SECRETS_FILE} not found.\n"
        "Copy secrets.example.toml to secrets.toml and set a fresh key:\n"
        "  python3 -c \"import secrets; print(secrets.token_hex(32))\"\n"
        "See docs/API.md section 8.")
endif()

add_custom_command(
    OUTPUT  ${SECRET_HEADER}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_secrets.py
            --secrets ${SECRETS_FILE}
            --out     ${SECRET_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_secrets.py ${SECRETS_FILE}
    COMMENT "Generating sb_secret.h"
    VERBATIM
)

# ─── Ethernet MAC address ──────────────────────────────────────────────────
# Default: derived from the chip's unique ID (locally administered, unique
# per board; see Core/Inc/sb_mac.h). Pin a fixed one, e.g. to keep a DHCP
# reservation, with: cmake -DSB_ETH_MAC=aa:bb:cc:dd:ee:ff ..
set(SB_ETH_MAC "" CACHE STRING
    "Fixed Ethernet MAC (aa:bb:cc:dd:ee:ff); empty = derive from the chip UID")

if(SB_ETH_MAC)
    if(NOT SB_ETH_MAC MATCHES "^[0-9A-Fa-f][0-9A-Fa-f](:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])(:[0-9A-Fa-f][0-9A-Fa-f])$")
        message(FATAL_ERROR "SB_ETH_MAC='${SB_ETH_MAC}' is not aa:bb:cc:dd:ee:ff")
    endif()
    string(REPLACE ":" ";" SB_MAC_OCTETS "${SB_ETH_MAC}")
    list(GET SB_MAC_OCTETS 0 SB_MAC_FIRST)
    math(EXPR SB_MAC_MCAST "0x${SB_MAC_FIRST} % 2")
    if(SB_MAC_MCAST)
        message(FATAL_ERROR "SB_ETH_MAC='${SB_ETH_MAC}' is a multicast address")
    endif()
    list(TRANSFORM SB_MAC_OCTETS PREPEND "0x")
    list(JOIN SB_MAC_OCTETS "," SB_MAC_BYTES)
    set_source_files_properties(Core/Src/sb_mac.c PROPERTIES
        COMPILE_DEFINITIONS "SB_ETH_MAC_FIXED=${SB_MAC_BYTES}")
    message(STATUS "Ethernet MAC fixed at ${SB_ETH_MAC}")
else()
    message(STATUS "Ethernet MAC derived from the chip UID")
endif()

# ─── Include paths ──────────────────────────────────────────────────────────
set(INCLUDE_DIRS
    # Application
    Core/Inc
    ${PINS_GEN_DIR}
    LWIP/App
    LWIP/Target

    # HAL & BSP
    Drivers/STM32H5xx_HAL_Driver/Inc
    Drivers/STM32H5xx_HAL_Driver/Inc/Legacy
    Drivers/BSP/STM32H5xx_Nucleo
    Drivers/BSP/Components/lan8742
    Drivers/CMSIS/Device/ST/STM32H5xx/Include
    Drivers/CMSIS/Include

    # FreeRTOS
    Middlewares/Third_Party/FreeRTOS/Source/include
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    Middlewares/Third_Party/CMSIS/RTOS2/Include

    # LwIP
    Middlewares/Third_Party/LwIP/src/include
    Middlewares/Third_Party/LwIP/system
)

# ─── Application sources ───────────────────────────────────────────────────
file(GLOB APP_SOURCES CONFIGURE_DEPENDS Core/Src/*.c)

# ─── Startup (assembly) ────────────────────────────────────────────────────
set(STARTUP_SOURCE Core/Startup/startup_stm32h563zitx.s)

# ─── LwIP application glue ─────────────────────────────────────────────────
set(LWIP_APP_SOURCES
    LWIP/App/app_ethernet.c
    LWIP/App/http_sse.c
    LWIP/Target/ethernetif.c
)

# ─── HAL driver sources ────────────────────────────────────────────────────
set(HAL_SOURCES
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_cortex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_dma.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_dma_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_eth.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_eth_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_exti.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_flash.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_flash_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_gpio.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_hash.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_icache.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_pwr.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_pwr_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rcc.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rcc_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rng.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rng_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_tim.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_tim_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_uart.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_uart_ex.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_usart.c
    Drivers/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_usart_ex.c
)

# ─── BSP sources ────────────────────────────────────────────────────────────
set(BSP_SOURCES
    Drivers/BSP/STM32H5xx_Nucleo/stm32h5xx_nucleo.c
    Drivers/BSP/Components/lan8742/lan8742.c
)

# ─── FreeRTOS sources ───────────────────────────────────────────────────────
set(FREERTOS_SOURCES
    Middlewares/Third_Party/FreeRTOS/Source/croutine.c
    Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
    Middlewares/Third_Party/FreeRTOS/Source/list.c
    Middlewares/Third_Party/FreeRTOS/Source/queue.c
    Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
    Middlewares/Third_Party/FreeRTOS/Source/tasks.c
    Middlewares/Third_Party/FreeRTOS/Source/timers.c
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/port.c
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c
    Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c
)

# ─── LwIP core sources ─────────────────────────────────────────────────────
set(LWIP_SOURCES
    # System / OS port
    Middlewares/Third_Party/LwIP/system/OS/sys_arch.c

    # API
    Middlewares/Third_Party/LwIP/src/api/api_lib.c
    Middlewares/Third_Party/LwIP/src/api/api_msg.c
    Middlewares/Third_Party/LwIP/src/api/err.c
    Middlewares/Third_Party/LwIP/src/api/if_api.c
    Middlewares/Third_Party/LwIP/src/api/netbuf.c
    Middlewares/Third_Party/LwIP/src/api/netdb.c
    Middlewares/Third_Party/LwIP/src/api/netifapi.c
    Middlewares/Third_Party/LwIP/src/api/sockets.c
    Middlewares/Third_Party/LwIP/src/api/tcpip.c

    # Core
    Middlewares/Third_Party/LwIP/src/core/altcp.c
    Middlewares/Third_Party/LwIP/src/core/altcp_alloc.c
    Middlewares/Third_Party/LwIP/src/core/altcp_tcp.c
    Middlewares/Third_Party/LwIP/src/core/def.c
    Middlewares/Third_Party/LwIP/src/core/dns.c
    Middlewares/Third_Party/LwIP/src/core/inet_chksum.c
    Middlewares/Third_Party/LwIP/src/core/init.c
    Middlewares/Third_Party/LwIP/src/core/ip.c
    Middlewares/Third_Party/LwIP/src/core/mem.c
    Middlewares/Third_Party/LwIP/src/core/memp.c
    Middlewares/Third_Party/LwIP/src/core/netif.c
    Middlewares/Third_Party/LwIP/src/core/pbuf.c
    Middlewares/Third_Party/LwIP/src/core/raw.c
    Middlewares/Third_Party/LwIP/src/core/stats.c
    Middlewares/Third_Party/LwIP/src/core/sys.c
    Middlewares/Third_Party/LwIP/src/core/tcp.c
    Middlewares/Third_Party/LwIP/src/core/tcp_in.c
    Middlewares/Third_Party/LwIP/src/core/tcp_out.c
    Middlewares/Third_Party/LwIP/src/core/timeouts.c
    Middlewares/Third_Party/LwIP/src/core/udp.c

    # IPv4
    Middlewares/Third_Party/LwIP/src/core/ipv4/acd.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/autoip.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/dhcp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/etharp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/icmp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/igmp.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/ip4.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_addr.c
    Middlewares/Third_Party/LwIP/src/core/ipv4/ip4_frag.c

    # Netif: Ethernet only (IPv4 only: no IPv6, 6LoWPAN, bridge, SLIP, ZEP)
    Middlewares/Third_Party/LwIP/src/netif/ethernet.c
)

# ─── Executable ─────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME}.elf
    ${PINS_HEADER}
    ${SECRET_HEADER}
    ${APP_SOURCES}
    ${STARTUP_SOURCE}
    ${LWIP_APP_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${FREERTOS_SOURCES}
    ${LWIP_SOURCES}
)

target_include_directories(${PROJECT_NAME}.elf PRIVATE ${INCLUDE_DIRS})

# ─── Linker configuration ──────────────────────────────────────────────────
target_link_options(${PROJECT_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${PROJECT_NAME}.map
    -Wl,--gc-sections
    -static
    -Wl,--start-group -lc -lm -Wl,--end-group
)

# Ensure relink on linker script change
set_target_properties(${PROJECT_NAME}.elf PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT}
)

# ─── Post-build: generate .bin and .hex, print size ────────────────────────
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Generating .bin and .hex, printing size"
)

# ─── Flash target ───────────────────────────────────────────────────────────
find_program(STM32_PROGRAMMER STM32_Programmer_CLI
    PATHS /opt/stm32cubeide/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_2.2.400.202601091506/tools/bin
)

if(STM32_PROGRAMMER)
    add_custom_target(flash
        COMMAND ${STM32_PROGRAMMER} -c port=SWD -w $<TARGET_FILE:${PROJECT_NAME}.elf> -rst
        DEPENDS ${PROJECT_NAME}.elf
        COMMENT "Flashing via ST-Link SWD"
    )
endif()
