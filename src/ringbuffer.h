#ifndef ULLLAS_RINGBUFFER_H
#define ULLLAS_RINGBUFFER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t *buffer;
    size_t    size;
    size_t    mask;
    size_t    read_idx;
    size_t    write_idx;
    char      _pad[56];
} RingBuffer;

int  rb_init(RingBuffer *rb, size_t capacity_samples);
void rb_destroy(RingBuffer *rb);

size_t rb_write(RingBuffer *rb, const int32_t * const *channels,
                int num_channels, size_t samples);

size_t rb_read(RingBuffer *rb, int32_t * const *channels,
               int num_channels, size_t samples);

size_t rb_available_read(const RingBuffer *rb);
size_t rb_available_write(const RingBuffer *rb);
void   rb_reset(RingBuffer *rb);

#ifdef __cplusplus
}
#endif

#endif
