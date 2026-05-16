#include "ringbuffer.h"
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define ATOMIC_LOAD(p)   __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define ATOMIC_STORE(p,v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define ATOMIC_LOAD_RELAXED(p) __atomic_load_n(p, __ATOMIC_RELAXED)
#elif defined(_MSC_VER)
#include <windows.h>
#define ATOMIC_LOAD(p)   InterlockedExchangeAdd64((LONG64*)(p), 0)
#define ATOMIC_STORE(p,v) InterlockedExchange64((LONG64*)(p), (LONG64)(v))
#define ATOMIC_LOAD_RELAXED(p) (*(p))
#else
#error Unsupported compiler
#endif

int rb_init(RingBuffer *rb, size_t capacity_samples) {
    size_t size = 1;
    while (size < capacity_samples) size <<= 1;
    rb->buffer = (int32_t *)calloc(size, sizeof(int32_t));
    if (!rb->buffer) return -1;
    rb->size = size;
    rb->mask = size - 1;
    rb->read_idx = 0;
    rb->write_idx = 0;
    return 0;
}

void rb_destroy(RingBuffer *rb) {
    free(rb->buffer);
    rb->buffer = NULL;
    rb->size = 0;
}

size_t rb_available_read(const RingBuffer *rb) {
    size_t w = ATOMIC_LOAD(&rb->write_idx);
    size_t r = ATOMIC_LOAD_RELAXED(&rb->read_idx);
    return w - r;
}

size_t rb_available_write(const RingBuffer *rb) {
    size_t r = ATOMIC_LOAD(&rb->read_idx);
    size_t w = ATOMIC_LOAD_RELAXED(&rb->write_idx);
    return rb->size - (w - r);
}

size_t rb_write(RingBuffer *rb, const int32_t * const *channels,
                int num_channels, size_t samples) {
    size_t avail = rb_available_write(rb) / num_channels;
    if (avail < samples) samples = avail;
    if (samples == 0) return 0;

    size_t w = ATOMIC_LOAD_RELAXED(&rb->write_idx);
    size_t total = samples * num_channels;
    size_t idx = w & rb->mask;

    if (idx + total <= rb->size) {
        if (num_channels == 2) {
            const int32_t *src0 = channels[0];
            const int32_t *src1 = channels[1];
            int32_t *dst = rb->buffer + idx;
            for (size_t i = 0; i < samples; i++) {
                dst[0] = src0[i];
                dst[1] = src1[i];
                dst += 2;
            }
        } else {
            for (int ch = 0; ch < num_channels; ch++) {
                const int32_t *src = channels[ch];
                int32_t *dst = rb->buffer + idx + ch;
                for (size_t i = 0; i < samples; i++) {
                    *dst = src[i];
                    dst += num_channels;
                }
            }
        }
    } else {
        size_t first = rb->size - idx;
        size_t first_samples = first / num_channels;
        size_t remaining = samples - first_samples;

        if (num_channels == 2) {
            const int32_t *src0 = channels[0];
            const int32_t *src1 = channels[1];
            int32_t *dst = rb->buffer + idx;
            for (size_t i = 0; i < first_samples; i++) {
                dst[0] = src0[i];
                dst[1] = src1[i];
                dst += 2;
            }
            dst = rb->buffer;
            for (size_t i = 0; i < remaining; i++) {
                dst[0] = src0[first_samples + i];
                dst[1] = src1[first_samples + i];
                dst += 2;
            }
        } else {
            for (int ch = 0; ch < num_channels; ch++) {
                const int32_t *src_ch = channels[ch];
                int32_t *dst = rb->buffer + idx + ch;
                for (size_t i = 0; i < first_samples; i++) {
                    *dst = src_ch[i];
                    dst += num_channels;
                }
                dst = rb->buffer + ch;
                for (size_t i = 0; i < remaining; i++) {
                    *dst = src_ch[first_samples + i];
                    dst += num_channels;
                }
            }
        }
    }

    ATOMIC_STORE(&rb->write_idx, w + total);
    return samples;
}

size_t rb_read(RingBuffer *rb, int32_t * const *channels,
               int num_channels, size_t samples) {
    size_t avail = rb_available_read(rb) / num_channels;
    if (avail < samples) samples = avail;
    if (samples == 0) return 0;

    size_t r = ATOMIC_LOAD_RELAXED(&rb->read_idx);
    size_t total = samples * num_channels;
    size_t idx = r & rb->mask;

    if (idx + total <= rb->size) {
        for (int ch = 0; ch < num_channels; ch++) {
            int32_t *dst = channels[ch];
            const int32_t *src = rb->buffer + idx + ch;
            for (size_t i = 0; i < samples; i++) {
                dst[i] = *src;
                src += num_channels;
            }
        }
    } else {
        size_t first = rb->size - idx;
        size_t first_samples = first / num_channels;
        size_t remaining = samples - first_samples;

        for (int ch = 0; ch < num_channels; ch++) {
            int32_t *dst_ch = channels[ch];
            const int32_t *src = rb->buffer + idx + ch;
            for (size_t i = 0; i < first_samples; i++) {
                dst_ch[i] = *src;
                src += num_channels;
            }
            src = rb->buffer + ch;
            for (size_t i = 0; i < remaining; i++) {
                dst_ch[first_samples + i] = *src;
                src += num_channels;
            }
        }
    }

    ATOMIC_STORE(&rb->read_idx, r + total);
    return samples;
}

void rb_reset(RingBuffer *rb) {
    size_t w = ATOMIC_LOAD(&rb->write_idx);
    ATOMIC_STORE(&rb->read_idx, w);
}
