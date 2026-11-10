/**
 * Host test for the receiver (Core/Src/simplebus_rx.c): edges from the
 * capture ISR in, bit strings out through the frame ring.
 *
 * Frames are built with the real encoder (sb_frame_encode); edges are the
 * falling edges at the start of each symbol, 1 tick = 1 us, 16-bit timer.
 */
#include <stdio.h>
#include <string.h>

#include "sb_frame.h"
#include "simplebus_rx.h"
#include "sb_ring.h"

sb_ring_t g_sb_ring;
uint32_t HAL_GetTick(void) { return 1234; }

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Feed one frame on timer channel ch. glitch_at >= 0 adds a spurious edge
 * glitch_us after the edge that starts symbol glitch_at. */
static void feed(uint8_t ch, const char* bits, uint16_t t0, int glitch_at,
                 uint16_t glitch_us) {
    uint16_t t = t0;
    simplebus_rx_edge(ch, t);                    /* edge 0: frame start */
    for (int i = 0; i < 59; i++) {               /* 59 more edges       */
        if (i == glitch_at) simplebus_rx_edge(ch, (uint16_t)(t + glitch_us));
        t = (uint16_t)(t + (bits[i] == '1' ? 374 : 466));
        simplebus_rx_edge(ch, t);
    }
}

static const sb_ring_frame_t* run(sb_ring_reader_t* c) {
    simplebus_rx_process();
    return sb_ring_read(c, &g_sb_ring);
}

int main(void) {
    TIM_HandleTypeDef h = {0};
    sb_ring_init(&g_sb_ring);
    simplebus_rx_init(&h);
    sb_ring_reader_t c;
    sb_ring_reader_init(&c, &g_sb_ring);

    /* Wall-switch announcement from the 2026-11-07 capture */
    const uint8_t d[4] = {0x00, 0x06, 0x01, 0x01};
    char bits[129];
    sb_frame_encode(d, 4, 1, bits, 128);
    const char* want =
        "01010101010000000001001100000101000000010100000001010111010";

    /* 1. Clean frame, timer wrapping through 0 mid-frame */
    feed(1, bits, 60000, -1, 0);
    const sb_ring_frame_t* f = run(&c);
    CHECK(f && f->trunk == 2 && f->bit_len == 59 && strcmp(f->bits, want) == 0,
          "clean frame: %s", f ? f->bits : "(none)");

    /* 2. Glitch 60 us into a '0' symbol (symbol 10 = start bit of B0).
     *    Old code measured the next symbol from the glitch: 466-60 = 406 us
     *    < 420 -> decoded as '1'. */
    CHECK(bits[10] == '0', "test setup: symbol 10 is a 0");
    feed(1, bits, 1000, 10, 60);
    f = run(&c);
    CHECK(f && strcmp(f->bits, want) == 0, "glitch before a 0: %s",
          f ? f->bits : "(none)");

    /* 3. Glitch right after the frame-start edge, and one inside a '1' */
    feed(2, bits, 30000, 0, 30);
    f = run(&c);
    CHECK(f && f->trunk == 3 && strcmp(f->bits, want) == 0,
          "glitch at symbol 0 (bus 3): %s", f ? f->bits : "(none)");
    feed(4, bits, 500, 1, 90);
    f = run(&c);
    CHECK(f && f->trunk == 4 && strcmp(f->bits, want) == 0,
          "glitch inside a 1 (bus 4): %s", f ? f->bits : "(none)");

    /* 4. Channel 3 has no bus: edges ignored, nothing published */
    feed(3, bits, 0, -1, 0);
    f = run(&c);
    CHECK(f == NULL, "unmapped channel produced a frame");

    /* 5. A 2 ms gap restarts the frame: a half frame then a full one */
    uint16_t t = 100;
    simplebus_rx_edge(1, t);
    for (int i = 0; i < 20; i++) simplebus_rx_edge(1, t = (uint16_t)(t + 374));
    feed(1, bits, (uint16_t)(t + 2000), -1, 0);
    f = run(&c);
    CHECK(f && strcmp(f->bits, want) == 0 && run(&c) == NULL,
          "gap restart: %s", f ? f->bits : "(none)");

    printf("rx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
