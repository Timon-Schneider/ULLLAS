#ifndef ULLLAS_PCM_PROTO_H
#define ULLLAS_PCM_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PCM_PROTO_PACKET_MAGIC 0x554C4C41
#define PCM_HEADER_SIZE         16

#define PCM_RATE_44100  0
#define PCM_RATE_48000  1
#define PCM_RATE_88200  2
#define PCM_RATE_96000  3

static inline int sample_rate_to_rate_id(unsigned int sample_rate) {
    switch (sample_rate) {
    case 44100: return PCM_RATE_44100;
    case 48000: return PCM_RATE_48000;
    case 88200: return PCM_RATE_88200;
    case 96000: return PCM_RATE_96000;
    default:    return PCM_RATE_48000;
    }
}

static inline void pcm_header_write(uint8_t *buf,
                                     uint32_t seq, uint64_t timestamp,
                                     int rate_id, int channels, int bit_depth,
                                     uint16_t num_samples) {
    uint32_t magic = PCM_PROTO_PACKET_MAGIC;
    uint8_t info = (uint8_t)(((rate_id & 0x0F) << 4) | (channels & 0x0F));
    memcpy(buf + 0,  &magic,      4);
    memcpy(buf + 4,  &seq,        4);
    memcpy(buf + 8,  &timestamp,  8);
    buf[12] = info;
    buf[13] = (uint8_t)bit_depth;
    memcpy(buf + 14, &num_samples, 2);
}

static inline void pcm_header_read(const uint8_t *buf,
                                    uint32_t *seq, uint64_t *timestamp,
                                    int *rate_id, int *channels, int *bit_depth,
                                    uint16_t *num_samples) {
    memcpy(seq,         buf + 4,  4);
    memcpy(timestamp,   buf + 8,  8);
    uint8_t info = buf[12];
    *rate_id     = (info >> 4) & 0x0F;
    *channels    = info & 0x0F;
    *bit_depth   = buf[13];
    memcpy(num_samples, buf + 14, 2);
}

size_t pcm_pack(const int32_t * const *channels, int num_channels, int num_samples,
                int bit_depth, uint8_t *out, size_t out_capacity);

size_t pcm_unpack(const uint8_t *in, size_t in_len, int num_channels, int bit_depth,
                  int32_t **out_channels, int max_samples);

#endif
