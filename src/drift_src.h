#ifndef ULLLAS_DRIFT_SRC_H
#define ULLLAS_DRIFT_SRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRIFT_SRC_TAPS   8
#define DRIFT_SRC_SUBPH  256
#define DRIFT_SRC_FIFO   2048  /* power-of-two internal ring buffer (frames) */

typedef struct {
    double  ratio;
    int     channels;

    /* Internal ring buffer */
    int32_t *buf;           /* interleaved, FIFO-capacity frames */
    size_t   mask;          /* capacity - 1 */
    size_t   write_idx;     /* next write position in frames */
    size_t   avail;         /* frames available to read */

    /* Fractional read accumulator */
    double   read_frac;     /* sub-frame read position, 0..<1 */
    size_t   read_idx;      /* integer read base in ring buffer */
} DriftSrc;

void   drift_src_init(DriftSrc *s, int channels);
void   drift_src_destroy(DriftSrc *s);
void   drift_src_reset(DriftSrc *s);
void   drift_src_set_ratio(DriftSrc *s, double ratio);

/* Push interleaved frames into the internal FIFO. Never called from
 * the audio callback's real-time path if the FIFO is full (caller
 * must ensure space via drift_src_avail check). */
void   drift_src_push(DriftSrc *s, const int32_t *interleaved, size_t frames);

/* Available frames in FIFO for reading. */
size_t drift_src_avail(const DriftSrc *s);

/* Produce `out_frames` of planar output, consuming from the internal
 * FIFO at the variable ratio. Returns frames actually produced. */
size_t drift_src_process(DriftSrc *s,
                         int32_t * const *planar_out, size_t out_frames);

#ifdef __cplusplus
}
#endif

#endif
