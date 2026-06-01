#include "ringbuffer.h"
#include <stdlib.h>
#include <string.h>

/*
 * Atomic ops on uint64_t for SPSC. Acquire on the foreign-written side,
 * release on the own-written side. The relaxed loads on the own side are
 * intentional - we always store the latest value ourselves on the same
 * thread, so a relaxed load races with no other writer.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ATOMIC_LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ATOMIC_LOAD_RELAXED(p)  __atomic_load_n((p), __ATOMIC_RELAXED)
#define ATOMIC_STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define ATOMIC_STORE_RELAXED(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#elif defined(_MSC_VER)
#include <windows.h>
static inline uint64_t rb_atomic_load_acq(const uint64_t *p) {
    return (uint64_t)InterlockedExchangeAdd64((volatile LONG64 *)p, 0);
}
static inline void rb_atomic_store_rel(uint64_t *p, uint64_t v) {
    InterlockedExchange64((volatile LONG64 *)p, (LONG64)v);
}
#define ATOMIC_LOAD_ACQ(p)         rb_atomic_load_acq(p)
#define ATOMIC_LOAD_RELAXED(p)     (*(volatile uint64_t *)(p))
#define ATOMIC_STORE_REL(p, v)     rb_atomic_store_rel((p), (v))
#define ATOMIC_STORE_RELAXED(p, v) (*(volatile uint64_t *)(p) = (v))
#else
#error Unsupported compiler for atomics
#endif

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

int rb_init(RingBuffer *rb, size_t capacity_frames, int channels) {
    if (channels <= 0 || channels > 255) return -1;
    memset(rb, 0, sizeof(*rb));
    size_t frames = next_pow2(capacity_frames < 2 ? 2 : capacity_frames);
    rb->buffer = (int32_t *)calloc(frames * (size_t)channels, sizeof(int32_t));
    if (!rb->buffer) return -1;
    rb->frame_count  = frames;
    rb->mask         = frames - 1;
    rb->channels     = channels;
    rb->write_frames = 0;
    rb->read_frames  = 0;
    return 0;
}

void rb_destroy(RingBuffer *rb) {
    if (!rb) return;
    free(rb->buffer);
    rb->buffer = NULL;
    rb->frame_count = 0;
}

size_t rb_available_read_frames(const RingBuffer *rb) {
    uint64_t w = ATOMIC_LOAD_ACQ(&rb->write_frames);
    uint64_t r = ATOMIC_LOAD_RELAXED(&rb->read_frames);
    return (size_t)(w - r);
}

size_t rb_available_write_frames(const RingBuffer *rb) {
    uint64_t r = ATOMIC_LOAD_ACQ(&rb->read_frames);
    uint64_t w = ATOMIC_LOAD_RELAXED(&rb->write_frames);
    return rb->frame_count - (size_t)(w - r);
}

size_t rb_write(RingBuffer *rb, const int32_t *const *channels, size_t frames) {
    if (frames == 0) return 0;
    size_t avail = rb_available_write_frames(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    int      nch = rb->channels;
    uint64_t w   = ATOMIC_LOAD_RELAXED(&rb->write_frames);
    size_t   idx = (size_t)w & rb->mask;

    size_t first = rb->frame_count - idx;
    if (first > frames) first = frames;

    /* Contiguous chunk. */
    {
        int32_t *base = rb->buffer + idx * (size_t)nch;
        if (nch == 2) {
            const int32_t *s0 = channels[0];
            const int32_t *s1 = channels[1];
            for (size_t i = 0; i < first; i++) {
                base[i * 2 + 0] = s0[i];
                base[i * 2 + 1] = s1[i];
            }
        } else {
            for (size_t i = 0; i < first; i++) {
                int32_t *f = base + i * (size_t)nch;
                for (int ch = 0; ch < nch; ch++) f[ch] = channels[ch][i];
            }
        }
    }

    /* Wrapped tail. */
    if (first < frames) {
        size_t   remain = frames - first;
        int32_t *base   = rb->buffer;
        if (nch == 2) {
            const int32_t *s0 = channels[0];
            const int32_t *s1 = channels[1];
            for (size_t i = 0; i < remain; i++) {
                base[i * 2 + 0] = s0[first + i];
                base[i * 2 + 1] = s1[first + i];
            }
        } else {
            for (size_t i = 0; i < remain; i++) {
                int32_t *f = base + i * (size_t)nch;
                for (int ch = 0; ch < nch; ch++) f[ch] = channels[ch][first + i];
            }
        }
    }

    ATOMIC_STORE_REL(&rb->write_frames, w + frames);
    return frames;
}

size_t rb_read(RingBuffer *rb, int32_t *const *channels, size_t frames) {
    if (frames == 0) return 0;
    size_t avail = rb_available_read_frames(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    int      nch = rb->channels;
    uint64_t r   = ATOMIC_LOAD_RELAXED(&rb->read_frames);
    size_t   idx = (size_t)r & rb->mask;

    size_t first = rb->frame_count - idx;
    if (first > frames) first = frames;

    {
        const int32_t *base = rb->buffer + idx * (size_t)nch;
        if (nch == 2) {
            int32_t *d0 = channels[0];
            int32_t *d1 = channels[1];
            for (size_t i = 0; i < first; i++) {
                d0[i] = base[i * 2 + 0];
                d1[i] = base[i * 2 + 1];
            }
        } else {
            for (size_t i = 0; i < first; i++) {
                const int32_t *f = base + i * (size_t)nch;
                for (int ch = 0; ch < nch; ch++) channels[ch][i] = f[ch];
            }
        }
    }

    if (first < frames) {
        size_t         remain = frames - first;
        const int32_t *base   = rb->buffer;
        if (nch == 2) {
            int32_t *d0 = channels[0];
            int32_t *d1 = channels[1];
            for (size_t i = 0; i < remain; i++) {
                d0[first + i] = base[i * 2 + 0];
                d1[first + i] = base[i * 2 + 1];
            }
        } else {
            for (size_t i = 0; i < remain; i++) {
                const int32_t *f = base + i * (size_t)nch;
                for (int ch = 0; ch < nch; ch++) channels[ch][first + i] = f[ch];
            }
        }
    }

    ATOMIC_STORE_REL(&rb->read_frames, r + frames);
    return frames;
}

size_t rb_write_interleaved(RingBuffer *rb, const int32_t *src, size_t frames) {
    if (frames == 0) return 0;
    size_t avail = rb_available_write_frames(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    int      nch = rb->channels;
    uint64_t w   = ATOMIC_LOAD_RELAXED(&rb->write_frames);
    size_t   idx = (size_t)w & rb->mask;

    size_t first = rb->frame_count - idx;
    if (first > frames) first = frames;

    memcpy(rb->buffer + idx * (size_t)nch, src, first * (size_t)nch * sizeof(int32_t));
    if (first < frames) {
        memcpy(rb->buffer, src + first * (size_t)nch, (frames - first) * (size_t)nch * sizeof(int32_t));
    }

    ATOMIC_STORE_REL(&rb->write_frames, w + frames);
    return frames;
}

size_t rb_read_interleaved(RingBuffer *rb, int32_t *dst, size_t frames) {
    if (frames == 0) return 0;
    size_t avail = rb_available_read_frames(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    int      nch = rb->channels;
    uint64_t r   = ATOMIC_LOAD_RELAXED(&rb->read_frames);
    size_t   idx = (size_t)r & rb->mask;

    size_t first = rb->frame_count - idx;
    if (first > frames) first = frames;

    memcpy(dst, rb->buffer + idx * (size_t)nch, first * (size_t)nch * sizeof(int32_t));
    if (first < frames) {
        memcpy(dst + first * (size_t)nch, rb->buffer, (frames - first) * (size_t)nch * sizeof(int32_t));
    }

    ATOMIC_STORE_REL(&rb->read_frames, r + frames);
    return frames;
}

size_t rb_skip_read(RingBuffer *rb, size_t frames) {
    if (frames == 0) return 0;
    size_t avail = rb_available_read_frames(rb);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;
    uint64_t r = ATOMIC_LOAD_RELAXED(&rb->read_frames);
    ATOMIC_STORE_REL(&rb->read_frames, r + frames);
    return frames;
}

void rb_reset(RingBuffer *rb) {
    uint64_t w = ATOMIC_LOAD_ACQ(&rb->write_frames);
    ATOMIC_STORE_REL(&rb->read_frames, w);
}
