/**
 * @file    sb_frame.h
 * @brief   SimpleBus frame codec: bytes <-> PLE bit strings.
 *
 *          A frame on the wire is a 10-symbol preamble followed by bytes,
 *          each sent as start bit '0', 8 data bits LSB first, stop bit '1'.
 *          The last byte is a checksum: seed + sum of the other bytes.
 *
 *          Preamble    seed   class
 *          0101010101  0x55   STD  poll / command / reply / ack
 *          0111011101  0x77   EXT  extended register read (not decoded)
 *          0110011001  0x33   RSP  extended register response
 *
 *          The receiver delivers 59 symbols for a 5-byte frame (the final
 *          stop bit has no closing edge); the decoder accepts that.
 *
 *          Pure C, no RTOS or HAL dependencies: also builds on the host for
 *          tests (tests/host).
 */

#ifndef SB_FRAME_H
#define SB_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SB_FRAME_MAX_BYTES 10

/* ─── STD frame types (verified on the bus, Comelit 20004606) ───────────── */

#define SB_TYPE_ACK 0x00        /* press ack / state announcement (module) */
#define SB_TYPE_POLL_REPLY 0x01 /* poll reply, out = relay state           */
#define SB_TYPE_PRESS 0x10      /* button press (master)                   */
#define SB_TYPE_POLL 0x21       /* poll (master)                           */

/* Button codes (press in/out bytes, ack in byte) */
#define SB_BTN_UP 0x01
#define SB_BTN_DOWN 0x02

/* Relay state (reply/ack out byte) */
#define SB_OUT_IDLE 0x00
#define SB_OUT_UP 0x01
#define SB_OUT_DOWN 0x02

/* Byte positions in a decoded STD frame */
#define SB_B_TYPE 0
#define SB_B_ADDR 1
#define SB_B_IN 2
#define SB_B_OUT 3
#define SB_B_CHK 4

typedef enum {
    SB_PROTO_UNKNOWN = 0,
    SB_PROTO_STD,
    SB_PROTO_EXT,
    SB_PROTO_RSP,
} sb_proto_t;

typedef struct {
    sb_proto_t proto;
    uint8_t n;                         /* bytes decoded, checksum included */
    uint8_t bytes[SB_FRAME_MAX_BYTES];
    uint8_t chk_ok;                    /* last byte matches the checksum   */
} sb_frame_t;

/** "STD", "EXT", "RSP" or "UNK". */
const char* sb_proto_label(sb_proto_t p);

/**
 * Encode bytes as an STD frame (preamble 0101010101, seed 0x55).
 * If add_checksum, the checksum byte is appended.
 * The final stop bit is left out, so a 5-byte frame is 59 symbols, sent as
 * 60 LOW pulses: the same shape as a module's frame.
 * `out` must hold out_max + 1 chars; bytes that don't fit are dropped.
 * Returns the number of symbols written (out is NUL-terminated).
 */
uint8_t sb_frame_encode(const uint8_t* data, uint8_t len, int add_checksum,
                        char* out, uint8_t out_max);

/**
 * Decode a received bit string. Returns 1 if a known preamble was found and
 * at least 5 bytes were decoded (f filled in), 0 otherwise (f->proto is
 * SB_PROTO_UNKNOWN and f->n is 0).
 */
int sb_frame_decode(const char* bits, uint8_t bit_len, sb_frame_t* f);

/** A well-formed 5-byte STD frame with a valid checksum. */
static inline int sb_frame_is_std(const sb_frame_t* f) {
    return f->proto == SB_PROTO_STD && f->n >= 5 && f->chk_ok;
}

#ifdef __cplusplus
}
#endif

#endif /* SB_FRAME_H */
