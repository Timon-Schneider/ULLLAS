#include "plc.h"
#include <string.h>

void plc_init(PlcState *s, int channels, unsigned int sample_rate, int enabled) {
    memset(s, 0, sizeof(*s));
    s->channels = channels;
    s->enabled  = enabled;
    /* 5 ms fade. Inaudible click-suppression for short losses. */
    if (sample_rate == 0) sample_rate = 48000;
    s->fade_frames = sample_rate / 200;
    if (s->fade_frames < 16) s->fade_frames = 16;
}

void plc_observe_packet(PlcState *s, const int32_t *interleaved, int num_frames) {
    if (!s || !interleaved || num_frames <= 0) return;
    const int32_t *last = interleaved + (size_t)(num_frames - 1) * (size_t)s->channels;
    for (int ch = 0; ch < s->channels && ch < 16; ch++) {
        s->last_frame[ch] = last[ch];
    }
}

void plc_conceal(PlcState *s, size_t frames, int32_t *out) {
    if (!s || frames == 0) return;

    if (!s->enabled) {
        memset(out, 0, frames * (size_t)s->channels * sizeof(int32_t));
        return;
    }

    unsigned int fade = s->fade_frames;
    for (size_t i = 0; i < frames; i++) {
        int32_t *dst = out + i * (size_t)s->channels;
        if (i >= fade) {
            for (int ch = 0; ch < s->channels; ch++) dst[ch] = 0;
            continue;
        }
        /* Linear fade from 1.0 to 0.0 over `fade` frames. 64-bit math
         * keeps the multiply well-defined for INT32_MIN. */
        int64_t num = (int64_t)(fade - i);
        int64_t den = (int64_t)fade;
        for (int ch = 0; ch < s->channels && ch < 16; ch++) {
            int64_t v = (int64_t)s->last_frame[ch] * num / den;
            dst[ch]   = (int32_t)v;
        }
    }
}
