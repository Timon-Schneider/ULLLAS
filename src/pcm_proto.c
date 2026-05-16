#include "pcm_proto.h"
#include <string.h>

size_t pcm_pack(const int32_t * const *channels, int num_channels, int num_samples,
                int bit_depth, uint8_t *out, size_t out_capacity) {
    if (num_channels < 1 || num_channels > 16 || num_samples < 1) return 0;

    size_t bps, frame_size, total;
    if (bit_depth == 16)      bps = 2;
    else if (bit_depth == 24) bps = 3;
    else                      bps = 4;

    frame_size = bps * num_channels;
    total = frame_size * num_samples;
    if (out_capacity < total) return 0;

    for (int i = 0; i < num_samples; i++) {
        for (int ch = 0; ch < num_channels; ch++) {
            int32_t s = channels[ch][i];
            size_t off = (size_t)i * frame_size + (size_t)ch * bps;

            switch (bit_depth) {
            case 16: {
                int32_t scaled = (s + (s >= 0 ? 32768 : -32768)) / 65536;
                if (scaled > 32767) scaled = 32767;
                if (scaled < -32768) scaled = -32768;
                int16_t v = (int16_t)scaled;
                out[off + 0] = (uint8_t)(v & 0xFF);
                out[off + 1] = (uint8_t)((v >> 8) & 0xFF);
                break;
            }
            case 24:
                out[off + 0] = (uint8_t)((s >> 8) & 0xFF);
                out[off + 1] = (uint8_t)((s >> 16) & 0xFF);
                out[off + 2] = (uint8_t)((s >> 24) & 0xFF);
                break;
            case 32:
                out[off + 0] = (uint8_t)(s & 0xFF);
                out[off + 1] = (uint8_t)((s >> 8) & 0xFF);
                out[off + 2] = (uint8_t)((s >> 16) & 0xFF);
                out[off + 3] = (uint8_t)((s >> 24) & 0xFF);
                break;
            }
        }
    }

    return total;
}

size_t pcm_unpack(const uint8_t *in, size_t in_len, int num_channels, int bit_depth,
                  int32_t **out_channels, int max_samples) {
    if (num_channels < 1 || num_channels > 16) return 0;

    size_t bps, frame_size;
    if (bit_depth == 16)      bps = 2;
    else if (bit_depth == 24) bps = 3;
    else                      bps = 4;

    frame_size = bps * num_channels;
    size_t num_samples = in_len / frame_size;
    if (num_samples == 0) return 0;
    if ((int)num_samples > max_samples) num_samples = (size_t)max_samples;

    for (int i = 0; i < (int)num_samples; i++) {
        for (int ch = 0; ch < num_channels; ch++) {
            size_t off = (size_t)i * frame_size + (size_t)ch * bps;
            int32_t v;

            switch (bit_depth) {
            case 16: {
                int16_t s16 = (int16_t)(in[off] | (in[off + 1] << 8));
                v = ((int32_t)s16) * 65536;
                break;
            }
            case 24:
                v = (int32_t)(in[off] | (in[off + 1] << 8) | (in[off + 2] << 16));
                if (v & 0x800000) v |= 0xFF000000;
                v = v * 256;
                break;
            case 32:
                v = (int32_t)(in[off] | (in[off + 1] << 8) | (in[off + 2] << 16) | (in[off + 3] << 24));
                break;
            default:
                v = 0;
                break;
            }

            out_channels[ch][i] = v;
        }
    }

    return num_samples;
}
