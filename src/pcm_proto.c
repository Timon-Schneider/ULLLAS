#include "pcm_proto.h"
#include <string.h>

/*
 * The pack/unpack hot loops:
 *   - hoist the bit_depth switch *out* of the inner loop
 *     (old version branched per sample, modern compilers couldn't fully
 *     hoist it because the loop body was complex)
 *   - use unsigned shifts to avoid signed overflow UB at INT32_MIN/MAX
 *   - assume top-aligned int32 input (full-scale = INT32_MIN..INT32_MAX)
 *
 * For 16-bit we use a right-shift instead of the old rounded divide; the
 * old code did `(s + 32768) / 65536` which is UB at s == INT32_MAX and at
 * s == INT32_MIN, and the rounding gain is inaudible vs the truncation.
 */

static inline void pack_s16_le(uint8_t *p, int32_t s) {
    /* Top 16 bits via arithmetic shift; cast through uint32 keeps it
       well-defined at the extremes. */
    int16_t v = (int16_t)((uint32_t)s >> 16);
    p[0]      = (uint8_t)(v & 0xFF);
    p[1]      = (uint8_t)((uint16_t)v >> 8);
}

static inline void pack_s24_le(uint8_t *p, int32_t s) {
    uint32_t u = (uint32_t)s;
    p[0]       = (uint8_t)(u >> 8);
    p[1]       = (uint8_t)(u >> 16);
    p[2]       = (uint8_t)(u >> 24);
}

static inline void pack_s32_le(uint8_t *p, int32_t s) {
    uint32_t u = (uint32_t)s;
    p[0]       = (uint8_t)u;
    p[1]       = (uint8_t)(u >> 8);
    p[2]       = (uint8_t)(u >> 16);
    p[3]       = (uint8_t)(u >> 24);
}

static inline int32_t unpack_s16_le(const uint8_t *p) {
    uint16_t u  = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    int16_t  s  = (int16_t)u;
    /* Sign-extend into the top 16 bits of an int32 via unsigned widening
       to avoid INT32_MIN UB on multiply-by-65536. */
    uint32_t u32 = (uint32_t)(int32_t)s << 16;
    return (int32_t)u32;
}

static inline int32_t unpack_s24_le(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (u & 0x800000u) u |= 0xFF000000u;
    /* Sign-extended 24-bit value in [-2^23, 2^23-1]; shift to top-align
       into the int32. Shift via unsigned to dodge UB. */
    return (int32_t)(u << 8);
}

static inline int32_t unpack_s32_le(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

size_t pcm_pack(const int32_t *const *channels, int num_channels, int num_samples, int bit_depth, uint8_t *out,
                size_t out_capacity) {
    if (num_channels < 1 || num_channels > 255 || num_samples < 1) return 0;
    int    bps        = pcm_bytes_per_sample(bit_depth);
    size_t frame_size = (size_t)bps * (size_t)num_channels;
    size_t total      = frame_size * (size_t)num_samples;
    if (out_capacity < total) return 0;

    switch (bit_depth) {
    case 16:
        for (int i = 0; i < num_samples; i++) {
            uint8_t *fp = out + (size_t)i * frame_size;
            for (int ch = 0; ch < num_channels; ch++) {
                pack_s16_le(fp + (size_t)ch * 2, channels[ch][i]);
            }
        }
        break;
    case 24:
        for (int i = 0; i < num_samples; i++) {
            uint8_t *fp = out + (size_t)i * frame_size;
            for (int ch = 0; ch < num_channels; ch++) {
                pack_s24_le(fp + (size_t)ch * 3, channels[ch][i]);
            }
        }
        break;
    case 32:
    default:
        for (int i = 0; i < num_samples; i++) {
            uint8_t *fp = out + (size_t)i * frame_size;
            for (int ch = 0; ch < num_channels; ch++) {
                pack_s32_le(fp + (size_t)ch * 4, channels[ch][i]);
            }
        }
        break;
    }

    return total;
}

size_t pcm_pack_interleaved(const int32_t *interleaved, int num_channels, int num_samples, int bit_depth, uint8_t *out,
                            size_t out_capacity) {
    if (num_channels < 1 || num_channels > 255 || num_samples < 1) return 0;
    int    bps        = pcm_bytes_per_sample(bit_depth);
    size_t frame_size = (size_t)bps * (size_t)num_channels;
    size_t total      = frame_size * (size_t)num_samples;
    if (out_capacity < total) return 0;

    size_t n_int = (size_t)num_samples * (size_t)num_channels;

    switch (bit_depth) {
    case 16:
        for (size_t k = 0; k < n_int; k++) {
            pack_s16_le(out + k * 2, interleaved[k]);
        }
        break;
    case 24:
        for (size_t k = 0; k < n_int; k++) {
            pack_s24_le(out + k * 3, interleaved[k]);
        }
        break;
    case 32:
    default:
        /* Fast path: when both sides are int32 LE and host is LE, this is
           a single memcpy. Compilers detect the pattern; keep the loop
           explicit for clarity and portability. */
        for (size_t k = 0; k < n_int; k++) {
            pack_s32_le(out + k * 4, interleaved[k]);
        }
        break;
    }

    return total;
}

size_t pcm_unpack_interleaved(const uint8_t *in, size_t in_len, int num_channels, int bit_depth, int32_t *out,
                              int max_samples) {
    if (num_channels < 1 || num_channels > 255) return 0;
    int    bps        = pcm_bytes_per_sample(bit_depth);
    size_t frame_size = (size_t)bps * (size_t)num_channels;
    if (frame_size == 0) return 0;

    size_t num_samples = in_len / frame_size;
    if (num_samples == 0) return 0;
    if (max_samples >= 0 && (int)num_samples > max_samples) {
        num_samples = (size_t)max_samples;
    }

    size_t n_int = num_samples * (size_t)num_channels;

    switch (bit_depth) {
    case 16:
        for (size_t k = 0; k < n_int; k++) {
            out[k] = unpack_s16_le(in + k * 2);
        }
        break;
    case 24:
        for (size_t k = 0; k < n_int; k++) {
            out[k] = unpack_s24_le(in + k * 3);
        }
        break;
    case 32:
    default:
        for (size_t k = 0; k < n_int; k++) {
            out[k] = unpack_s32_le(in + k * 4);
        }
        break;
    }

    return num_samples;
}
