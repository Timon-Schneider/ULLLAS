#include "drift_src.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LANCZOS_A 4

static float coeff[DRIFT_SRC_SUBPH][DRIFT_SRC_TAPS];
static int   coeff_ready = 0;

static double sinc_pi(double x)
{
    if (fabs(x) < 1e-12) return 1.0;
    double px = 3.14159265358979323846 * x;
    return sin(px) / px;
}

static double lanczos_kernel(double x, int a)
{
    double ax = fabs(x);
    if (ax >= (double)a) return 0.0;
    if (ax < 1e-12) return 1.0;
    return sinc_pi(x) * sinc_pi(x / (double)a);
}

static void ensure_coeff(void)
{
    if (coeff_ready) return;
    int leftmost = DRIFT_SRC_TAPS / 2 - 1;
    for (int s = 0; s < DRIFT_SRC_SUBPH; s++) {
        double t   = (double)s / DRIFT_SRC_SUBPH;
        double sum = 0.0;
        for (int k = 0; k < DRIFT_SRC_TAPS; k++) {
            int    offset = k - leftmost;
            double d      = t - (double)offset;
            double w      = lanczos_kernel(d, LANCZOS_A);
            coeff[s][k] = (float)w;
            sum += w;
        }
        if (sum > 1e-12) {
            for (int k = 0; k < DRIFT_SRC_TAPS; k++)
                coeff[s][k] /= (float)sum;
        } else {
            coeff[s][leftmost] = 1.0f;
        }
    }
    coeff_ready = 1;
}

static size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

void drift_src_init(DriftSrc *s, int channels)
{
    ensure_coeff();
    size_t cap = next_pow2((size_t)DRIFT_SRC_FIFO);
    s->buf = (int32_t *)calloc(cap * (size_t)channels, sizeof(int32_t));
    s->mask     = cap - 1;
    s->channels = channels;
    s->ratio    = 1.0;
    s->write_idx = 0;
    s->read_idx  = 0;
    s->read_frac = 0.0;
    s->avail     = 0;
}

void drift_src_destroy(DriftSrc *s)
{
    free(s->buf);
    s->buf = NULL;
}

void drift_src_reset(DriftSrc *s)
{
    s->write_idx = 0;
    s->read_idx  = 0;
    s->read_frac = 0.0;
    s->avail     = 0;
}

void drift_src_set_ratio(DriftSrc *s, double ratio)
{
    s->ratio = ratio;
}

void drift_src_push(DriftSrc *s, const int32_t *interleaved, size_t frames)
{
    if (frames == 0) return;
    int      ch  = s->channels;
    size_t   cap = s->mask + 1;
    size_t   w   = s->write_idx;

    size_t first = cap - w;
    if (first > frames) first = frames;
    memcpy(s->buf + w * (size_t)ch, interleaved, first * (size_t)ch * sizeof(int32_t));
    if (first < frames) {
        memcpy(s->buf, interleaved + first * (size_t)ch,
               (frames - first) * (size_t)ch * sizeof(int32_t));
    }
    s->write_idx = (w + frames) & s->mask;
    s->avail    += frames;
}

size_t drift_src_avail(const DriftSrc *s)
{
    return s->avail;
}

static inline int32_t clamp32(double v)
{
    if (v >  2147483647.0) return  2147483647;
    if (v < -2147483648.0) return -2147483647 - 1;
    return (int32_t)v;
}

size_t drift_src_process(DriftSrc *s,
                         int32_t * const *planar_out, size_t out_frames)
{
    int    ch    = s->channels;
    double step  = 1.0 / s->ratio;

    if (s->avail < (size_t)DRIFT_SRC_TAPS) {
        for (int c = 0; c < ch; c++)
            memset(planar_out[c], 0, out_frames * sizeof(int32_t));
        s->read_frac = 0.0;
        return 0;
    }

    size_t stride   = (size_t)ch;
    int    leftmost = DRIFT_SRC_TAPS / 2 - 1;

    for (size_t k = 0; k < out_frames; k++) {
        double pos  = s->read_frac + (double)k * step;
        size_t ridx = (size_t)pos;
        double t    = pos - (double)ridx;

        int sub = (int)(t * DRIFT_SRC_SUBPH + 0.5);
        if (sub < 0) sub = 0;
        if (sub >= DRIFT_SRC_SUBPH) sub = DRIFT_SRC_SUBPH - 1;

        float *w   = coeff[sub];
        int    si0 = (int)ridx - leftmost;

        for (int c = 0; c < ch; c++) {
            double v = 0.0;
            for (int tap = 0; tap < DRIFT_SRC_TAPS; tap++) {
                int    si = si0 + tap;
                size_t rb_pos;
                if (si < 0)
                    rb_pos = (s->read_idx - (size_t)(-si)) & s->mask;
                else if ((size_t)si >= s->avail)
                    rb_pos = ((s->read_idx + s->avail - 1) & s->mask);
                else
                    rb_pos = ((s->read_idx + (size_t)si) & s->mask);
                v += w[tap] * (double)s->buf[rb_pos * stride + c];
            }
            planar_out[c][k] = clamp32(v);
        }
    }

    double advance  = s->read_frac + (double)out_frames * step;
    size_t consumed = (size_t)advance;
    s->read_frac    = advance - (double)consumed;
    s->read_idx     = (s->read_idx + consumed) & s->mask;
    s->avail       -= consumed;

    return consumed;
}
