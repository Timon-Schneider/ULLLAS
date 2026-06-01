#ifndef ULLLAS_PCM_PROTO_H
#define ULLLAS_PCM_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * ULLLAS wire protocol v2 (magic "ULLB").
 *
 * v1 (magic "ULLA") had two latent bugs that broke any non-trivial use:
 *   - the 8-byte timestamp at offset 8 overlapped info/bit_depth/num_samples
 *     written at 12..15, so timestamp was effectively only 4 bytes;
 *   - the magic was never validated by the receiver;
 *   - the 4-bit channel field overflowed for 16-channel streams;
 *   - only four sample rates round-tripped (any other rate silently
 *     became 48 kHz on the wire).
 *
 * v2 fixes all of those and adds room for FEC + flags. All multi-byte
 * fields are little-endian on the wire (independent of host endianness).
 *
 * Layout (28 bytes):
 *   [0..3]   magic            = 0x424C4C55 (bytes 'U','L','L','B' in memory)
 *   [4..7]   seq              uint32_t LE       (packet sequence, wraps)
 *   [8..15]  timestamp        uint64_t LE       (frame counter at sender)
 *   [16]     bit_depth        uint8_t           (16, 24, or 32)
 *   [17]     channels         uint8_t           (1..255)
 *   [18..19] num_samples      uint16_t LE       (frames in this packet)
 *   [20..23] sample_rate      uint32_t LE       (Hz)
 *   [24]     flags            uint8_t           (bit 0 = FEC parity packet)
 *   [25]     fec_group_size   uint8_t           (0 = FEC off; else N)
 *   [26..27] reserved         (zero on send, ignored on recv)
 */

#define PCM_PROTO_MAGIC 0x424C4C55u /* 'U','L','L','B' in memory order */
#define PCM_HEADER_SIZE 28u

#define PCM_FLAG_PARITY 0x01u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint64_t timestamp;
    uint8_t  bit_depth;
    uint8_t  channels;
    uint16_t num_samples;
    uint32_t sample_rate;
    uint8_t  flags;
    uint8_t  fec_group_size;
} PcmHeader;

static inline void le_u16_write(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void le_u32_write(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void le_u64_write(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

static inline uint16_t le_u16_read(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t le_u32_read(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t le_u64_read(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline void pcm_header_write(uint8_t *buf, const PcmHeader *h) {
    le_u32_write(buf + 0, PCM_PROTO_MAGIC);
    le_u32_write(buf + 4, h->seq);
    le_u64_write(buf + 8, h->timestamp);
    buf[16] = h->bit_depth;
    buf[17] = h->channels;
    le_u16_write(buf + 18, h->num_samples);
    le_u32_write(buf + 20, h->sample_rate);
    buf[24] = h->flags;
    buf[25] = h->fec_group_size;
    buf[26] = 0;
    buf[27] = 0;
}

/* Returns 1 on success (magic ok), 0 on bad magic. */
static inline int pcm_header_read(const uint8_t *buf, PcmHeader *h) {
    uint32_t magic = le_u32_read(buf + 0);
    if (magic != PCM_PROTO_MAGIC) {
        return 0;
    }
    h->magic          = magic;
    h->seq            = le_u32_read(buf + 4);
    h->timestamp      = le_u64_read(buf + 8);
    h->bit_depth      = buf[16];
    h->channels       = buf[17];
    h->num_samples    = le_u16_read(buf + 18);
    h->sample_rate    = le_u32_read(buf + 20);
    h->flags          = buf[24];
    h->fec_group_size = buf[25];
    return 1;
}

static inline int pcm_bytes_per_sample(int bit_depth) {
    if (bit_depth == 16) return 2;
    if (bit_depth == 24) return 3;
    return 4;
}

/* Pack from planar (de-interleaved) channel pointers into an interleaved
 * little-endian payload. Returns bytes written, 0 on capacity/argument error.
 *
 * Sample format on the wire: little-endian, signed, sample-rate-aligned at
 * the top of an internal int32 (i.e. a 16-bit sample is the top 16 bits of
 * the int32; a 24-bit sample is the top 24 bits; a 32-bit sample is the
 * whole int32). This convention matches what the audio backends produce.
 */
size_t pcm_pack(const int32_t *const *channels, int num_channels, int num_samples, int bit_depth, uint8_t *out,
                size_t out_capacity);

/* Pack from an already-interleaved int32 source (samples laid out
 * frame-by-frame: ch0,ch1,...chN-1, ch0,ch1,...). Same semantics as
 * pcm_pack otherwise. Used by the sender's audio callback. */
size_t pcm_pack_interleaved(const int32_t *interleaved, int num_channels, int num_samples, int bit_depth, uint8_t *out,
                            size_t out_capacity);

/* Unpack an interleaved little-endian payload into an interleaved int32
 * destination buffer (the layout the ring buffer also uses). Returns
 * sample count, 0 if input too small or buffer overflow. */
size_t pcm_unpack_interleaved(const uint8_t *in, size_t in_len, int num_channels, int bit_depth, int32_t *out,
                              int max_samples);

#ifdef __cplusplus
}
#endif

#endif
