/**
 * Host tests for the DMA transmitter's table (Core/Src/sb_tx_table.c) and
 * the receiver mute (simplebus_rx_mute in Core/Src/simplebus_rx.c).
 *
 * The table is "played" here the way TIM7 + GPDMA play it on the board: one
 * word per SB_TX_UNIT_US tick into BSRR. The resulting pin waveform is
 * checked against the PLE timing, and its falling edges are fed through the
 * real receiver to prove the bridge's own frames decode as sent.
 */
#include <stdio.h>
#include <string.h>

#include "sb_frame.h"
#include "sb_ring.h"
#include "sb_tx_table.h"
#include "simplebus_rx.h"

sb_ring_t g_sb_ring;
uint32_t HAL_GetTick(void) { return 4321; }

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PIN 0x4000u /* PG14, bus 2's data pin on rev 1 */

/* Pin level runs from playing the table: levels[k], lengths[k] in ticks. */
typedef struct {
    int n;
    int level[1024];
    uint32_t ticks[1024];
    int bad_word;   /* a word touching another pin or both set and reset */
    int final_high; /* pin HIGH after the last word */
} wave_t;

static void play(const uint32_t* t, uint32_t n, wave_t* w) {
    memset(w, 0, sizeof(*w));
    int level = 1; /* idle HIGH before the first tick */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t x = t[k];
        if (x == PIN) level = 1;
        else if (x == (PIN << 16)) level = 0;
        else if (x != 0) w->bad_word = 1;
        if (w->n == 0 || w->level[w->n - 1] != level) {
            w->level[w->n] = level;
            w->ticks[w->n] = 0;
            w->n++;
        }
        w->ticks[w->n - 1]++;
    }
    w->final_high = (level == 1);
}

/* Check the waveform encodes `bits` with the agreed absolute timing (not
 * the code's own constants): LOW 186 us, HIGH 186 us ('1') or 279 us ('0'),
 * then a closing LOW 186 us and HIGH (the final word). */
#define US(k) (w->ticks[k] * SB_TX_UNIT_US)
static int wave_matches(const wave_t* w, const char* bits) {
    int len = (int)strlen(bits);
    if (w->n != 2 * len + 2) return 0;
    for (int b = 0; b < len; b++) {
        if (w->level[2 * b] != 0 || US(2 * b) != 186) return 0;
        uint32_t hi = bits[b] == '1' ? 186 : 279;
        if (w->level[2 * b + 1] != 1 || US(2 * b + 1) != hi) return 0;
    }
    return w->level[2 * len] == 0 && US(2 * len) == 186 &&
           w->level[2 * len + 1] == 1 && w->ticks[2 * len + 1] == 1;
}

/* Feed the waveform's falling edges to the receiver on timer channel ch
 * (1 tick = 1 us, 16-bit timer), starting at t0. Returns edges fed. */
static int feed_wave(uint8_t ch, const wave_t* w, uint16_t t0) {
    uint32_t t = t0;
    int edges = 0;
    for (int k = 0; k < w->n; k++) {
        if (w->level[k] == 0) {
            simplebus_rx_edge(ch, (uint16_t)t);
            edges++;
        }
        t += w->ticks[k] * SB_TX_UNIT_US;
    }
    return edges;
}

static const sb_ring_frame_t* run(sb_ring_reader_t* c) {
    simplebus_rx_process();
    return sb_ring_read(c, &g_sb_ring);
}

int main(void) {
    static uint32_t tab[SB_TX_TABLE_WORDS(128)];
    wave_t w;

    /* 1. The poll 21 06 00 00 as verified on the hardware (2026-11-08). */
    const char* poll =
        "01010101010100001001001100000100000000010000000001000111110";
    uint32_t n = sb_tx_build(poll, 59, PIN, tab, SB_TX_TABLE_WORDS(128));
    play(tab, n, &w);
    CHECK(n > 0 && !w.bad_word && w.final_high, "poll table built, n=%u", n);
    CHECK(wave_matches(&w, poll), "poll waveform: 186/186/279 us grid");
    int lows = 0;
    for (int k = 0; k < w.n; k++) lows += (w.level[k] == 0);
    CHECK(lows == 60, "60 LOW pulses (got %d)", lows);
    uint32_t us = n * SB_TX_UNIT_US;
    CHECK(us > 25000 && us < 27500, "frame length %u us", us);

    /* 2. Every encoded frame round-trips through the real receiver. */
    TIM_HandleTypeDef h = {0};
    sb_ring_init(&g_sb_ring);
    simplebus_rx_init(&h);
    sb_ring_reader_t c;
    sb_ring_reader_init(&c, &g_sb_ring);

    const uint8_t frames[][4] = {{0x21, 0x06, 0x00, 0x00},
                                 {0x10, 0x06, 0x01, 0x01},
                                 {0x10, 0x06, 0x02, 0x02},
                                 {0x00, 0x0A, 0x01, 0x01},
                                 {0xFF, 0xFF, 0xFF, 0xFF},
                                 {0x00, 0x00, 0x00, 0x00}};
    for (unsigned f = 0; f < sizeof(frames) / sizeof(frames[0]); f++) {
        char bits[129];
        uint8_t nb = sb_frame_encode(frames[f], 4, 1, bits, 128);
        n = sb_tx_build(bits, nb, PIN, tab, SB_TX_TABLE_WORDS(128));
        play(tab, n, &w);
        CHECK(wave_matches(&w, bits), "frame %u waveform", f);
        feed_wave(1, &w, (uint16_t)(1000u + 7919u * f));
        const sb_ring_frame_t* r = run(&c);
        sb_frame_t d;
        CHECK(r && r->trunk == 2 && r->bit_len == 59 &&
                  strcmp(r->bits, bits) == 0 &&
                  sb_frame_decode(r->bits, r->bit_len, &d) && sb_frame_is_std(&d) &&
                  memcmp(d.bytes, frames[f], 4) == 0,
              "frame %u received as sent: %s", f, r ? r->bits : "(none)");
    }

    /* 3. Mute: our own frame on bus 2 is not received, bus 3 still is. */
    char own[129], other[129];
    const uint8_t pd[4] = {0x21, 0x06, 0x00, 0x00};
    const uint8_t an[4] = {0x00, 0x0A, 0x01, 0x01};
    sb_frame_encode(pd, 4, 1, own, 128);
    sb_frame_encode(an, 4, 1, other, 128);
    wave_t wo, wa;
    play(tab, sb_tx_build(own, 59, PIN, tab, SB_TX_TABLE_WORDS(128)), &wo);
    static uint32_t tab2[SB_TX_TABLE_WORDS(128)];
    play(tab2, sb_tx_build(other, 59, PIN, tab2, SB_TX_TABLE_WORDS(128)), &wa);

    simplebus_rx_mute(0, 1);         /* bus 2 (index 0) transmitting   */
    feed_wave(1, &wo, 100);          /* our frame on bus 2's receiver  */
    feed_wave(2, &wa, 3000);         /* a module on bus 3 meanwhile    */
    const sb_ring_frame_t* r = run(&c);
    CHECK(r && r->trunk == 3 && strcmp(r->bits, other) == 0,
          "bus 3 received during bus 2 TX: %s", r ? r->bits : "(none)");
    CHECK(run(&c) == NULL, "own frame on muted bus 2 not reported");

    /* Half a frame before the mute ends must not leak into the next one. */
    simplebus_rx_edge(1, 40000);
    simplebus_rx_edge(1, 40372);
    simplebus_rx_mute(0, 0);
    feed_wave(1, &wa, 50000);
    r = run(&c);
    CHECK(r && r->trunk == 2 && strcmp(r->bits, other) == 0,
          "bus 2 receives normally after unmute: %s", r ? r->bits : "(none)");

    /* A frame half received when the mute starts is dropped, not completed
     * with our own edges. */
    simplebus_rx_edge(1, 1000);
    simplebus_rx_edge(1, 1372);
    simplebus_rx_mute(0, 1);
    feed_wave(1, &wo, 2000);
    simplebus_rx_mute(0, 0);
    CHECK(run(&c) == NULL, "partial frame dropped by mute");

    /* Same, with no edge at all during the mute, and the next frame starting
     * exactly one 16-bit timer wrap (65.536 ms) + 400 us after the stale
     * edge: without the reset in simplebus_rx_mute() the 400 us would look
     * like a symbol and the stale edges would corrupt the new frame. */
    simplebus_rx_edge(1, 1000);
    simplebus_rx_edge(1, 1372);
    simplebus_rx_mute(0, 1);
    simplebus_rx_mute(0, 0);
    feed_wave(1, &wa, (uint16_t)(1372u + 400u));
    r = run(&c);
    CHECK(r && r->trunk == 2 && strcmp(r->bits, other) == 0,
          "stale edges cleared by mute (timer wrap case): %s",
          r ? r->bits : "(none)");

    /* 4. Limits and bad input. */
    char zeros[129];
    memset(zeros, '0', 128);
    zeros[128] = '\0';
    n = sb_tx_build(zeros, 128, PIN, tab, SB_TX_TABLE_WORDS(128));
    CHECK(n == SB_TX_TABLE_WORDS(128), "worst case fills the table exactly (%u)", n);
    CHECK(sb_tx_build(zeros, 128, PIN, tab, SB_TX_TABLE_WORDS(128) - 1) == 0,
          "one word short: refused");
    CHECK(sb_tx_build("0120", 4, PIN, tab, 100) == 0, "bad symbol refused");
    CHECK(sb_tx_build("01", 0, PIN, tab, 100) == 0, "empty frame refused");
    CHECK(sb_tx_build("01", 2, 0x0003, tab, 100) == 0, "two-pin mask refused");
    CHECK(sb_tx_build("01", 2, 0, tab, 100) == 0, "zero mask refused");
    CHECK(sb_tx_build("01", 2, 0x10000, tab, 100) == 0, "mask past pin 15 refused");
    n = sb_tx_build("1", 1, 0x0001, tab, 100);
    CHECK(n == 7 && tab[0] == 0x10000 && tab[1] == 0 && tab[2] == 1 &&
              tab[3] == 0 && tab[4] == 0x10000 && tab[5] == 0 && tab[6] == 1,
          "single '1' on pin 0: exact words");

    printf("tx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
