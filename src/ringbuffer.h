#ifndef ULLLAS_RINGBUFFER_H
#define ULLLAS_RINGBUFFER_H

#include <stdint.h>
#include <stddef.h>

/*
 * Single-producer / single-consumer lock-free ring buffer for audio frames.
 *
 * The buffer stores whole frames (channels * int32 per frame). Frame
 * indices are atomic uint64; the producer only writes write_frames and
 * reads read_frames; the consumer is symmetric. write_frames and
 * read_frames live on dedicated cache lines to avoid false sharing - the
 * v1 implementation packed them adjacent in the struct which made every
 * audio callback bounce the line back and forth with the network thread.
 *
 * Capacity is rounded up to a power of two frames so the index modulus is
 * a bit-mask. Because frames are stored intact (not as a flat int32 array)
 * the buffer no longer corrupts samples for non-power-of-two channel
 * counts the way v1 did.
 */

#if defined(__cplusplus)
#define ULLLAS_ALIGNAS(x) alignas(x)
#elif defined(_MSC_VER)
#define ULLLAS_ALIGNAS(x) __declspec(align(x))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define ULLLAS_ALIGNAS(x) _Alignas(x)
#else
#define ULLLAS_ALIGNAS(x)
#endif

#define ULLLAS_CACHE_LINE 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Read-only after init - producer and consumer both read these. */
    int32_t *buffer;
    size_t   frame_count;
    size_t   mask;
    int      channels;

    /* Producer-modified, consumer-read. Padded onto its own cache line. */
    ULLLAS_ALIGNAS(ULLLAS_CACHE_LINE) uint64_t write_frames;
    char _pad_after_write[ULLLAS_CACHE_LINE - sizeof(uint64_t)];

    /* Consumer-modified, producer-read. */
    ULLLAS_ALIGNAS(ULLLAS_CACHE_LINE) uint64_t read_frames;
    char _pad_after_read[ULLLAS_CACHE_LINE - sizeof(uint64_t)];
} RingBuffer;

int  rb_init(RingBuffer *rb, size_t capacity_frames, int channels);
void rb_destroy(RingBuffer *rb);

/* Frame-counted accessors. Callers always speak frames now. */
size_t rb_available_read_frames(const RingBuffer *rb);
size_t rb_available_write_frames(const RingBuffer *rb);

/* Planar write (channel[ch][i] = sample). Returns frames actually written. */
size_t rb_write(RingBuffer *rb, const int32_t *const *channels, size_t frames);

/* Planar read. Returns frames actually read. */
size_t rb_read(RingBuffer *rb, int32_t *const *channels, size_t frames);

/* Interleaved variants: src/dst is laid out frame-by-frame
 * (ch0,ch1,...,chN-1, ch0,ch1,...). Used by the network thread + FEC. */
size_t rb_write_interleaved(RingBuffer *rb, const int32_t *src, size_t frames);
size_t rb_read_interleaved(RingBuffer *rb, int32_t *dst, size_t frames);

/* Drift compensation primitive: drop the next `frames` from the read
 * side (consumer-side, but it's safe for the producer to call in our
 * usage because the receiver's network thread owns both ends of the
 * compensation operation). Returns frames actually skipped. */
size_t rb_skip_read(RingBuffer *rb, size_t frames);

void rb_reset(RingBuffer *rb);

#ifdef __cplusplus
}
#endif

#endif
