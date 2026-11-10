# Host tests for the bus-independent firmware modules.
#   make -C tests/host          build and run the C tests
#   make -C tests/host e2e      also run tools/sbctl.py against fake_bridge
#   make -C tests/host clean
#
# Needs gcc and OpenSSL's libcrypto (stands in for the HASH peripheral).

ROOT   := ../..
CC     ?= gcc
CFLAGS := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -O0 \
          -fsanitize=address,undefined \
          -I$(ROOT)/Core/Inc -I. -Istubs
LDLIBS := -lcrypto

FW_SRCS := $(ROOT)/Core/Src/sb_frame.c \
           $(ROOT)/Core/Src/sb_blind.c \
           $(ROOT)/Core/Src/sb_auth.c \
           $(ROOT)/Core/Src/sb_stream.c \
           $(ROOT)/Core/Src/api_v1.c
HOST_SRCS := fake_bus.c crypto_host.c sys_host.c
HDRS := fake_bus.h $(wildcard $(ROOT)/Core/Inc/*.h)

run: test_host test_rx test_tx test_mac test_mac_fixed
	./test_host
	./test_rx
	./test_tx
	./test_mac
	./test_mac_fixed

e2e: fake_bridge
	./test_sbctl.sh

test_host: test_main.c $(FW_SRCS) $(HOST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_main.c $(FW_SRCS) $(HOST_SRCS) $(LDLIBS)

test_rx: test_rx.c $(ROOT)/Core/Src/simplebus_rx.c $(ROOT)/Core/Src/sb_frame.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_rx.c $(ROOT)/Core/Src/simplebus_rx.c \
	    $(ROOT)/Core/Src/sb_frame.c

test_tx: test_tx.c $(ROOT)/Core/Src/sb_tx_table.c $(ROOT)/Core/Src/simplebus_rx.c $(ROOT)/Core/Src/sb_frame.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_tx.c $(ROOT)/Core/Src/sb_tx_table.c \
	    $(ROOT)/Core/Src/simplebus_rx.c $(ROOT)/Core/Src/sb_frame.c

test_mac: test_mac.c $(ROOT)/Core/Src/sb_mac.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_mac.c $(ROOT)/Core/Src/sb_mac.c

test_mac_fixed: test_mac.c $(ROOT)/Core/Src/sb_mac.c $(HDRS)
	$(CC) $(CFLAGS) -DSB_ETH_MAC_FIXED=0x00,0x80,0xE1,0x00,0x05,0x77 -o $@ \
	    test_mac.c $(ROOT)/Core/Src/sb_mac.c

fake_bridge: fake_bridge.c $(FW_SRCS) $(HOST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ fake_bridge.c $(FW_SRCS) $(HOST_SRCS) $(LDLIBS)

clean:
	rm -f test_host test_rx test_tx test_mac test_mac_fixed fake_bridge

.PHONY: run e2e clean
