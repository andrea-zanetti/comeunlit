/**
 * @file    sb_frame.c
 * @brief   SimpleBus frame codec. See sb_frame.h.
 */

#include "sb_frame.h"

#include <string.h>

typedef struct {
    const char* pattern;
    uint8_t seed;
    sb_proto_t proto;
} preamble_t;

static const preamble_t preambles[] = {
    {"0101010101", 0x55, SB_PROTO_STD},
    {"0111011101", 0x77, SB_PROTO_EXT},
    {"0110011001", 0x33, SB_PROTO_RSP},
};
#define NUM_PREAMBLES (sizeof(preambles) / sizeof(preambles[0]))

const char* sb_proto_label(sb_proto_t p) {
    switch (p) {
        case SB_PROTO_STD: return "STD";
        case SB_PROTO_EXT: return "EXT";
        case SB_PROTO_RSP: return "RSP";
        default: return "UNK";
    }
}

uint8_t sb_frame_encode(const uint8_t* data, uint8_t len, int add_checksum,
                        char* out, uint8_t out_max) {
    uint8_t pos = 0;

    if (out_max < 10) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, preambles[0].pattern, 10);
    pos = 10;

    uint8_t checksum = preambles[0].seed;
    uint8_t total = len;
    if (add_checksum) {
        for (uint8_t i = 0; i < len; i++) checksum += data[i];
        total = len + 1;
    }

    uint8_t groups = 0;
    for (uint8_t b = 0; b < total; b++) {
        uint8_t v = (b < len) ? data[b] : checksum;
        if (pos + 10 > out_max) break;

        out[pos++] = '0'; /* start bit */
        for (int i = 0; i < 8; i++) out[pos++] = (v & (1u << i)) ? '1' : '0';
        out[pos++] = '1'; /* stop bit */
        groups++;
    }

    /* Leave out the last stop bit, as the modules do: the transmitter's
     * closing LOW pulse ends the last data bit, and the idle HIGH after it
     * is the stop bit. Sending it as a symbol adds a 61st pulse, which makes
     * a module wait ~33 ms before replying (docs/simplebus_protocol_
     * reference.md, section 2). */
    if (groups > 0) pos--;

    out[pos] = '\0';
    return pos;
}

int sb_frame_decode(const char* bits, uint8_t bit_len, sb_frame_t* f) {
    memset(f, 0, sizeof(*f));
    if (bit_len < 59) return 0;

    const preamble_t* pre = NULL;
    for (unsigned t = 0; t < NUM_PREAMBLES; t++) {
        if (strncmp(bits, preambles[t].pattern, 10) == 0) {
            pre = &preambles[t];
            break;
        }
    }
    if (pre == NULL) return 0;

    /* Bytes: start bit '0', 8 data bits LSB first, stop bit. The stop bit
     * of the last byte may be missing (59-symbol frames), so a byte only
     * needs its start bit and data bits present. */
    int pos = 10;
    uint8_t n = 0;
    while (pos + 9 <= bit_len && n < SB_FRAME_MAX_BYTES) {
        if (bits[pos] != '0') break;
        uint8_t v = 0;
        for (int i = 0; i < 8; i++)
            if (bits[pos + 1 + i] == '1') v |= (uint8_t)(1u << i);
        f->bytes[n++] = v;
        pos += 10;
    }

    if (n < 5) {
        memset(f, 0, sizeof(*f));
        return 0;
    }

    uint8_t calc = pre->seed;
    for (uint8_t i = 0; i < n - 1; i++) calc += f->bytes[i];

    f->proto = pre->proto;
    f->n = n;
    f->chk_ok = (calc == f->bytes[n - 1]);
    return 1;
}
