/**
 * fake_bridge - the firmware's API code (api_v1, sb_auth, sb_blind) on a
 * host TCP socket, with simulated modules, so tools/sbctl.py can be tested
 * end to end without hardware.  Usage: fake_bridge <port>
 *
 * Simulated bus 2: addr 3 idle, 4 moving down, 5 moving up, 6 idle.
 * Streams replay a fixed set of frames (a bridge press + ack, a wall-switch
 * press sent twice, a poll) through the real sb_stream code, then close.
 * FAKE_CORRUPT=1 flips one byte of the second event, to test the client.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api_v1.h"
#include "fake_bus.h"
#include "sb_auth.h"
#include "sb_frame.h"
#include "sb_stream.h"

int lwip_write(int s, const void* data, int size) {
    return (int)write(s, data, (size_t)size);
}

static sb_ring_frame_t mk(uint8_t bus, uint8_t dir, uint32_t ts, uint8_t b0,
                          uint8_t b1, uint8_t b2, uint8_t b3) {
    sb_ring_frame_t f;
    memset(&f, 0, sizeof(f));
    const uint8_t d[4] = {b0, b1, b2, b3};
    uint8_t n = sb_frame_encode(d, 4, 1, f.bits, MAX_BIT_STRING);
    if (dir == SB_DIR_RX) n = 59;
    f.bits[n] = '\0';
    f.bit_len = n;
    f.trunk = bus;
    f.dir = dir;
    f.timestamp_ms = ts;
    return f;
}

int api_v1_stream_open(int sock, sb_stream_type_t type, const char* req_mac,
                       const char* hdr_mac) {
    static sb_stream_t st;
    char line[SB_STREAM_LINE_MAX];
    int n = snprintf(line, sizeof(line),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "X-SB-MAC: %s\r\n\r\n", hdr_mac);
    lwip_write(sock, line, n);
    sb_stream_init(&st, type, req_mac);

    const sb_ring_frame_t frames[] = {
        mk(2, SB_DIR_TX, 1000, 0x10, 0x06, 0x01, 0x01),
        mk(2, SB_DIR_RX, 1061, 0x00, 0x06, 0x01, 0x01),
        mk(2, SB_DIR_RX, 5000, 0x00, 0x05, 0x02, 0x00),
        mk(2, SB_DIR_RX, 5252, 0x00, 0x05, 0x02, 0x00),
        mk(2, SB_DIR_TX, 7000, 0x21, 0x06, 0x00, 0x00),
        mk(2, SB_DIR_RX, 7061, 0x01, 0x06, 0x00, 0x01),
    };
    int corrupt = getenv("FAKE_CORRUPT") != NULL;

    n = sb_stream_heartbeat(&st, 1, line, sizeof(line));
    lwip_write(sock, line, n);
    for (unsigned i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        n = sb_stream_frame(&st, &frames[i], line, sizeof(line));
        if (n <= 0) continue;
        if (corrupt && st.seq == 3) line[n - 4] ^= 0x01; /* inside the JSON */
        lwip_write(sock, line, n);
    }
    return 0; /* serve() closes the socket afterwards */
}

static char buf[8192];

static void serve(int fd) {
    int len = 0;
    char* hdr_end = NULL;
    while (!hdr_end && len < (int)sizeof(buf) - 1) {
        int n = (int)read(fd, buf + len, sizeof(buf) - 1 - (size_t)len);
        if (n <= 0) return;
        len += n;
        buf[len] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    if (!hdr_end) return;
    int hlen = (int)(hdr_end - buf) + 4;
    int clen = 0;
    const char* cl = strcasestr(buf, "\r\ncontent-length:");
    if (cl && cl < hdr_end) clen = atoi(cl + 17);
    while (len < hlen + clen && len < (int)sizeof(buf) - 1) {
        int n = (int)read(fd, buf + len, sizeof(buf) - 1 - (size_t)len);
        if (n <= 0) return;
        len += n;
    }
    buf[hlen + clen] = '\0';
    api_v1_handle(fd, buf, buf + hlen, clen);
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 18080;

    const uint8_t session[8] = {0xc0, 0xff, 0xee, 0x00, 0x12, 0x34, 0x56, 0x78};
    sb_auth_init(session);
    fake_reset();
    fake_add(2, 3, SB_OUT_IDLE);
    fake_add(2, 4, SB_OUT_DOWN);
    fake_add(2, 5, SB_OUT_UP);
    fake_add(2, 6, SB_OUT_IDLE);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(ls, 4) < 0) {
        perror("fake_bridge");
        return 1;
    }
    printf("fake_bridge on 127.0.0.1:%d, session %s\n", port,
           sb_auth_session_hex());
    fflush(stdout);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        serve(fd);
        close(fd);
    }
}
