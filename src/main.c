#include "audio.h"
#include "config.h"
#include "network.h"
#include "pcm_proto.h"
#include "plc.h"
#include "fec.h"
#include "ringbuffer.h"
#include "rt_thread.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#define sleep_ms(ms) Sleep(ms)
static volatile LONG g_running = 1;
static BOOL WINAPI    ctrl_handler(DWORD type) {
    (void)type;
    InterlockedExchange(&g_running, 0);
    return TRUE;
}
static int g_running_load(void) {
    return (int)InterlockedExchangeAdd(&g_running, 0);
}
#else
#include <unistd.h>
#include <signal.h>
#include <time.h>
#define sleep_ms(ms) usleep((ms) * 1000)
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}
static int g_running_load(void) {
    return (int)g_running;
}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * v2 architecture:
 *   - Sender: the audio callback packs and sends UDP packets directly.
 *     No more userland ring buffer + semaphore. This removes a thread
 *     hop's worth of jitter on every audio period.
 *   - Receiver: a network thread receives, optionally runs FEC reorder,
 *     applies PLC for losses, and writes interleaved frames to a ring
 *     buffer. The audio callback drains the ring buffer.
 *
 * Status printing happens on the main thread, which sleeps between
 * reports.
 */

#define RX_RB_FRAMES 16384
#define STATUS_INTERVAL_S 2

#ifdef HAS_ASIO
extern AudioBackend *asio_backend_create(void);
#endif
#ifdef HAS_COREAUDIO
extern AudioBackend *coreaudio_backend_create(void);
#endif
#ifdef HAS_JACK
extern AudioBackend *jack_backend_create(void);
#endif

/* ----------------------------------------------------------------------
 *  Sender
 * ---------------------------------------------------------------------- */

typedef struct {
    Config       *cfg;
    UdpSender     tx;
    AudioBackend *audio;
    int           channels;
    int           buffer_size;
    int           sample_rate;
    /* Stats. Audio callback writes, status thread reads - stale OK. */
    volatile uint64_t total_frames_tx;
    volatile uint64_t total_send_errors;
    volatile int32_t  peak_level;
    int               verbose;
} SenderState;

static int32_t compute_peak_planar(const int32_t *const *bufs, int channels, int samples) {
    int32_t peak = 0;
    for (int ch = 0; ch < channels; ch++) {
        for (int i = 0; i < samples; i++) {
            int32_t v = bufs[ch][i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
    }
    return peak;
}

static int sender_callback(const int32_t *const *inputs, int32_t *const *outputs, int nframes, void *userdata) {
    SenderState *st = (SenderState *)userdata;
    (void)outputs;

    /* The hottest path in the program: pack and sendto, in the audio
     * driver's real-time thread. No locks, no allocations, no I/O. */
    udp_sender_send_planar(&st->tx, inputs, st->channels, nframes);

    if (st->verbose) {
        st->peak_level = compute_peak_planar(inputs, st->channels, nframes);
    }
    st->total_frames_tx   = st->tx.total_frames_tx;
    st->total_send_errors = st->tx.total_send_errors;
    return 0;
}

/* ----------------------------------------------------------------------
 *  Receiver
 * ---------------------------------------------------------------------- */

typedef struct {
    Config       *cfg;
    RingBuffer    rb;
    UdpReceiver   rx;
    AudioBackend *audio;

    int channels;
    int buffer_size;
    int sample_rate;
    int bit_depth;

    int jitter_target_frames;

    PlcState   plc;
    FecRxState fec;
    int        fec_enabled;

    int started;

    /* For loss / dup / reorder detection on the non-FEC fast path. */
    uint32_t last_emitted_seq;
    int      have_last_seq;
    int      drift_comp_enabled;
    int      drift_tick;

    /* Pre-allocated unpacking scratch (interleaved int32). Sized for the
     * largest single packet (MTU-driven). */
    int32_t *unpack_scratch;
    size_t   unpack_scratch_frames;

    /* Stats. */
    uint64_t total_frames_rx;
    uint64_t total_packets_rx;
    uint64_t total_packets_lost;
    uint64_t total_dups;
    uint64_t total_reorders;
    uint64_t total_recovered;
    uint64_t total_drift_drops;
    uint64_t total_drift_dups;
    int      underruns;
    int32_t  peak_level;
} ReceiverState;

static int32_t compute_peak_interleaved(const int32_t *interleaved, int channels, int frames) {
    int32_t peak  = 0;
    size_t  total = (size_t)channels * (size_t)frames;
    for (size_t i = 0; i < total; i++) {
        int32_t v = interleaved[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return peak;
}

static int receiver_callback(const int32_t *const *inputs, int32_t *const *outputs, int nframes, void *userdata) {
    ReceiverState *st = (ReceiverState *)userdata;
    (void)inputs;

    size_t avail = rb_available_read_frames(&st->rb);

    if (!st->started) {
        if (avail >= (size_t)st->jitter_target_frames) {
            st->started = 1;
        } else {
            for (int ch = 0; ch < st->channels; ch++) {
                memset(outputs[ch], 0, (size_t)nframes * sizeof(int32_t));
            }
            return 0;
        }
    }

    size_t read = rb_read(&st->rb, outputs, (size_t)nframes);
    if (read < (size_t)nframes) {
        size_t missing = (size_t)nframes - read;
        for (int ch = 0; ch < st->channels; ch++) {
            memset(outputs[ch] + read, 0, missing * sizeof(int32_t));
        }
        st->underruns++;
        /* Static-jitter re-prime: empty ring means we need to wait for
         * the configured jitter target before audio resumes. The user
         * chose this jitter size; we honor it consistently. */
        st->started = 0;
    }
    return 0;
}

/* Compute the expected payload byte-count for a packet. */
static size_t expected_payload_bytes(int channels, int bit_depth, int num_samples) {
    return (size_t)num_samples * (size_t)channels * (size_t)pcm_bytes_per_sample(bit_depth);
}

/* Called for each "emitted" packet (data, FEC-recovered, or unrecoverable). */
static void rx_emit_packet(uint32_t seq, uint16_t num_samples, const uint8_t *payload, int recovered, void *userdata) {
    ReceiverState *st = (ReceiverState *)userdata;

    st->total_packets_rx++;
    if (recovered) st->total_recovered++;

    /* Sender restart: a large backwards seq jump means the sender's
     * sequence counter was reset. Wipe our seq tracking. */
    if (st->have_last_seq) {
        uint32_t diff = seq - st->last_emitted_seq;
        if (diff > 0x80000000u) {
            /* Backwards jump beyond half the seq space - treat as
             * restart. */
            st->have_last_seq    = 0;
            st->last_emitted_seq = 0;
            rb_reset(&st->rb);
            st->started = 0;
        }
    }

    /* Duplicate / reorder filter (non-FEC path). */
    if (st->have_last_seq && seq == st->last_emitted_seq) {
        st->total_dups++;
        return;
    }
    if (st->have_last_seq && (int32_t)(seq - st->last_emitted_seq) < 0) {
        st->total_reorders++;
        return;
    }

    /* Fill PLC frames for any gap before this packet. */
    if (st->have_last_seq && seq > st->last_emitted_seq + 1) {
        uint32_t missing  = seq - st->last_emitted_seq - 1;
        uint16_t fill_len = num_samples;
        if (fill_len == 0) fill_len = (uint16_t)st->buffer_size;
        if (fill_len > st->unpack_scratch_frames) fill_len = (uint16_t)st->unpack_scratch_frames;
        st->total_packets_lost += missing;
        for (uint32_t g = 0; g < missing; g++) {
            plc_conceal(&st->plc, (size_t)fill_len, st->unpack_scratch);
            rb_write_interleaved(&st->rb, st->unpack_scratch, (size_t)fill_len);
        }
    }

    /* Unrecoverable loss reported by FEC: PLC-fill for this slot too. */
    if (payload == NULL) {
        uint16_t fill_len = num_samples;
        if (fill_len == 0) fill_len = (uint16_t)st->buffer_size;
        if (fill_len > st->unpack_scratch_frames) fill_len = (uint16_t)st->unpack_scratch_frames;
        plc_conceal(&st->plc, (size_t)fill_len, st->unpack_scratch);
        rb_write_interleaved(&st->rb, st->unpack_scratch, (size_t)fill_len);
        st->total_packets_lost++;
        st->last_emitted_seq = seq;
        st->have_last_seq    = 1;
        return;
    }

    /* Normal path: unpack into scratch, then write to ring buffer. */
    size_t want_bytes = expected_payload_bytes(st->channels, st->bit_depth, num_samples);
    if (num_samples > st->unpack_scratch_frames) {
        /* Should never happen with MTU-clamped sender, but cap defensively. */
        num_samples = (uint16_t)st->unpack_scratch_frames;
        want_bytes  = expected_payload_bytes(st->channels, st->bit_depth, num_samples);
    }
    size_t n = pcm_unpack_interleaved(payload, want_bytes, st->channels, st->bit_depth, st->unpack_scratch,
                                      (int)st->unpack_scratch_frames);
    if (n == 0) {
        st->last_emitted_seq = seq;
        st->have_last_seq    = 1;
        return;
    }
    plc_observe_packet(&st->plc, st->unpack_scratch, (int)n);

    size_t avail_before = rb_available_read_frames(&st->rb);
    rb_write_interleaved(&st->rb, st->unpack_scratch, n);

    if (st->cfg->verbose) {
        int32_t p = compute_peak_interleaved(st->unpack_scratch, st->channels, (int)n);
        if (p > st->peak_level) st->peak_level = p;
    }

    st->last_emitted_seq = seq;
    st->have_last_seq    = 1;

    if (st->drift_comp_enabled && ++st->drift_tick >= 5) {
        st->drift_tick = 0;
        size_t target = (size_t)st->jitter_target_frames;
        size_t bs     = (size_t)st->buffer_size;
        if (avail_before > target + bs * 3) {
            rb_skip_read(&st->rb, 1);
            st->total_drift_drops++;
        } else if (avail_before > 0 && avail_before < bs && st->unpack_scratch_frames > 0) {
            for (int ch = 0; ch < st->channels; ch++) {
                st->unpack_scratch[ch] = st->plc.last_frame[ch];
            }
            rb_write_interleaved(&st->rb, st->unpack_scratch, 1);
            st->total_drift_dups++;
        }
    }
}

/* ----------------------------------------------------------------------
 *  Common helpers
 * ---------------------------------------------------------------------- */

static AudioBackend *create_audio_backend(const Config *cfg) {
#ifdef HAS_ASIO
    if (strcmp(cfg->backend, "asio") == 0) return asio_backend_create();
#endif
#ifdef HAS_COREAUDIO
    if (strcmp(cfg->backend, "coreaudio") == 0) return coreaudio_backend_create();
#endif
#ifdef HAS_JACK
    if (strcmp(cfg->backend, "jack") == 0) return jack_backend_create();
#endif
    fprintf(stderr, "No supported backend available for '%s'\n", cfg->backend);
    return NULL;
}

/* ----------------------------------------------------------------------
 *  Sender main
 * ---------------------------------------------------------------------- */

static int run_sender(Config *cfg) {
    SenderState st;
    memset(&st, 0, sizeof(st));
    st.cfg         = cfg;
    st.channels    = cfg->in_channel_count;
    st.buffer_size = (int)cfg->buffer_size;
    st.verbose     = cfg->verbose ? 1 : 0;

    st.audio = create_audio_backend(cfg);
    if (!st.audio) return -1;

    if (st.audio->vtable->init(st.audio, cfg->device_name, cfg->sample_rate, st.channels, 0, (int)cfg->buffer_size,
                                sender_callback, &st) != 0) {
        fprintf(stderr, "Failed to init audio\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        return -1;
    }

    st.buffer_size = st.audio->buffer_size;
    if ((unsigned int)st.buffer_size != cfg->buffer_size) {
        fprintf(stderr, "Warning: requested buffer size %u, driver uses %d\n", cfg->buffer_size, st.buffer_size);
    }
    st.sample_rate = (int)st.audio->sample_rate;
    if ((unsigned int)st.sample_rate != cfg->sample_rate) {
        fprintf(stderr, "Warning: requested sample rate %u, driver uses %d\n", cfg->sample_rate, st.sample_rate);
    }

    int fec = cfg->fec_group_size > 0 ? (int)cfg->fec_group_size : 0;
    if (udp_sender_init(&st.tx, cfg->target_addr, cfg->port, cfg->iface_addr, cfg->use_multicast, st.sample_rate,
                        st.channels, (int)cfg->bit_depth, st.buffer_size, fec) != 0) {
        fprintf(stderr, "Failed to init network sender\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        return -1;
    }

    /* Audio callback is about to start running. Lock memory before that. */
    rt_lock_memory();

    if (st.audio->vtable->start(st.audio) != 0) {
        fprintf(stderr, "Failed to start audio\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        udp_sender_destroy(&st.tx);
        return -1;
    }

    fprintf(stderr, "ULLLAS sender: %d ch @ %u Hz, %u-bit, buffer %u -> %s%s\n", st.channels, (unsigned)st.sample_rate,
            cfg->bit_depth, (unsigned)st.buffer_size, cfg->target_addr, fec > 0 ? " [FEC]" : "");
#ifdef _WIN32
    fprintf(stderr, "Configure ASIO4ALL tray icon to enable your input device.\n");
#endif

    /* Main thread just prints status and sleeps; the audio callback owns
     * the hot path. */
    uint64_t last_status_frames = 0;
    time_t   last_status_time   = time(NULL);
    while (g_running_load()) {
        sleep_ms(200);
        time_t now = time(NULL);
        if (now - last_status_time < STATUS_INTERVAL_S) continue;
        last_status_time      = now;
        uint64_t frames       = st.tx.total_frames_tx;
        uint64_t errs         = st.tx.total_send_errors;
        uint64_t parity_count = st.tx.total_fec_parity_tx;
        (void)last_status_frames;
        last_status_frames = frames;
        if (cfg->verbose) {
            fprintf(stderr, "\rTX: %llu frames, %llu sends with errors, FEC parity %llu, peak %d          \n",
                    (unsigned long long)frames, (unsigned long long)errs, (unsigned long long)parity_count,
                    (int)st.peak_level);
        } else {
            fprintf(stderr, "\rTX: %llu frames, %llu sends with errors, FEC parity %llu          \n",
                    (unsigned long long)frames, (unsigned long long)errs, (unsigned long long)parity_count);
        }
    }

    fprintf(stderr, "\nShutting down sender...\n");
    st.audio->vtable->stop(st.audio);
    st.audio->vtable->destroy(st.audio);
    free(st.audio);
    udp_sender_destroy(&st.tx);
    return 0;
}

/* ----------------------------------------------------------------------
 *  Receiver main
 * ---------------------------------------------------------------------- */

static int run_receiver(Config *cfg) {
    ReceiverState st;
    memset(&st, 0, sizeof(st));
    st.cfg                = cfg;
    st.channels           = cfg->out_channel_count;
    st.buffer_size        = (int)cfg->buffer_size;
    st.bit_depth          = (int)cfg->bit_depth;
    st.fec_enabled        = cfg->fec_group_size > 0 ? 1 : 0;
    st.drift_comp_enabled = cfg->drift_comp ? 1 : 0;
    st.started            = 0;

    if (rb_init(&st.rb, RX_RB_FRAMES, st.channels) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }

    st.audio = create_audio_backend(cfg);
    if (!st.audio) {
        rb_destroy(&st.rb);
        return -1;
    }

    if (st.audio->vtable->init(st.audio, cfg->device_name, cfg->sample_rate, 0, st.channels, (int)cfg->buffer_size,
                                receiver_callback, &st) != 0) {
        fprintf(stderr, "Failed to init audio\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    st.buffer_size = st.audio->buffer_size;
    st.sample_rate = (int)st.audio->sample_rate;
    if ((unsigned int)st.buffer_size != cfg->buffer_size) {
        fprintf(stderr, "Warning: requested buffer size %u, driver uses %d\n", cfg->buffer_size, st.buffer_size);
    }
    if ((unsigned int)st.sample_rate != cfg->sample_rate) {
        fprintf(stderr, "Warning: requested sample rate %u, driver uses %d\n", cfg->sample_rate, st.sample_rate);
    }

    st.jitter_target_frames = (int)cfg->jitter_packets * st.buffer_size;

    int max_per_pkt = udp_compute_max_samples_per_packet(st.channels, st.bit_depth);
    if (max_per_pkt <= 0) {
        fprintf(stderr, "Bad channels/bit_depth combination\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    /* Use a generous unpack scratch that fits any single MTU-sized
     * packet plus a comfort margin. */
    st.unpack_scratch_frames = (size_t)max_per_pkt;
    if (st.unpack_scratch_frames < (size_t)st.buffer_size) {
        st.unpack_scratch_frames = (size_t)st.buffer_size;
    }
    st.unpack_scratch = (int32_t *)calloc(st.unpack_scratch_frames * (size_t)st.channels, sizeof(int32_t));
    if (!st.unpack_scratch) {
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    plc_init(&st.plc, st.channels, (unsigned)st.sample_rate, cfg->plc ? 1 : 0);

    if (st.fec_enabled) {
        size_t payload_size = expected_payload_bytes(st.channels, st.bit_depth, st.buffer_size);
        if (fec_rx_init(&st.fec, (int)cfg->fec_group_size, payload_size) != 0) {
            fprintf(stderr, "Failed to init FEC receiver\n");
            free(st.unpack_scratch);
            st.audio->vtable->destroy(st.audio);
            free(st.audio);
            rb_destroy(&st.rb);
            return -1;
        }
    }

    const char *mcast = (cfg->use_multicast && cfg->target_addr[0] != '\0') ? cfg->target_addr : NULL;
    if (udp_receiver_init(&st.rx, cfg->bind_addr, cfg->port, mcast, cfg->iface_addr, cfg->use_multicast, st.channels,
                          st.bit_depth, max_per_pkt) != 0) {
        fprintf(stderr, "Failed to init network receiver\n");
        free(st.unpack_scratch);
        if (st.fec_enabled) fec_rx_destroy(&st.fec);
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    rt_lock_memory();

    if (st.audio->vtable->start(st.audio) != 0) {
        fprintf(stderr, "Failed to start audio\n");
        free(st.unpack_scratch);
        if (st.fec_enabled) fec_rx_destroy(&st.fec);
        udp_receiver_destroy(&st.rx);
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    fprintf(stderr,
            "ULLLAS receiver: %d ch @ %u Hz, %u-bit, buffer %u, jitter %u pkts (%d frames)%s%s%s -> port %u\n",
            st.channels, (unsigned)st.sample_rate, cfg->bit_depth, (unsigned)st.buffer_size, cfg->jitter_packets,
            st.jitter_target_frames, cfg->plc ? " [PLC]" : "", cfg->fec_group_size > 0 ? " [FEC]" : "",
            cfg->drift_comp ? " [drift-comp]" : "", cfg->port);
#ifdef _WIN32
    fprintf(stderr, "IMPORTANT: Click the ASIO4ALL tray icon and enable your speaker/headphone output!\n");
#endif
    fprintf(stderr, "Waiting for stream (need %d frames to start)...\n", st.jitter_target_frames);

    /* This thread does the receive loop. Raise its priority - the audio
     * callback already runs at driver RT priority. */
    rt_raise_thread_priority();

    time_t last_status = time(NULL);
    while (g_running_load()) {
        PcmHeader      hdr;
        const uint8_t *payload     = NULL;
        size_t         payload_len = 0;
        int            r           = udp_receiver_recv(&st.rx, &hdr, &payload, &payload_len);
        if (r < 0) {
            break;
        }
        if (r > 0) {
            /* If the sender announces FEC but the receiver didn't enable
             * it, the parity packets get dropped here naturally. We do
             * still honor data packets in that case. */
            if (st.fec_enabled) {
                fec_rx_process(&st.fec, hdr.seq, (hdr.flags & PCM_FLAG_PARITY) ? 1 : 0, hdr.num_samples, payload,
                               rx_emit_packet, &st);
            } else if ((hdr.flags & PCM_FLAG_PARITY) == 0) {
                rx_emit_packet(hdr.seq, hdr.num_samples, payload, 0, &st);
            }
        }

        time_t now = time(NULL);
        if (now - last_status >= STATUS_INTERVAL_S) {
            last_status        = now;
            size_t   avail     = rb_available_read_frames(&st.rb);
            uint64_t recovered = st.total_recovered;
            uint64_t lost      = st.total_packets_lost;
            if (cfg->verbose) {
                fprintf(stderr,
                        "\rRX: %llu pkts, lost %llu, recovered %llu, dups %llu, reord %llu, drift-d/u %llu/%llu, "
                        "underrun %d, buf %u frames, peak %d          \n",
                        (unsigned long long)st.total_packets_rx, (unsigned long long)lost,
                        (unsigned long long)recovered, (unsigned long long)st.total_dups,
                        (unsigned long long)st.total_reorders, (unsigned long long)st.total_drift_drops,
                        (unsigned long long)st.total_drift_dups, st.underruns, (unsigned)avail, (int)st.peak_level);
            } else {
                fprintf(stderr,
                        "\rRX: %llu pkts, lost %llu, recovered %llu, underrun %d, buf %u frames          \n",
                        (unsigned long long)st.total_packets_rx, (unsigned long long)lost,
                        (unsigned long long)recovered, st.underruns, (unsigned)avail);
            }
        }
    }

    fprintf(stderr, "\nShutting down receiver...\n");
    if (st.fec_enabled) {
        fec_rx_flush(&st.fec, rx_emit_packet, &st);
        fec_rx_destroy(&st.fec);
    }
    st.audio->vtable->stop(st.audio);
    st.audio->vtable->destroy(st.audio);
    free(st.audio);
    udp_receiver_destroy(&st.rx);
    free(st.unpack_scratch);
    rb_destroy(&st.rb);
    return 0;
}

int main(int argc, char **argv) {
    Config cfg;
    if (config_parse(&cfg, argc, argv) != 0) {
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    if (cfg.list_devices) {
        AudioBackend *ab = create_audio_backend(&cfg);
        if (ab) {
            ab->vtable->list_devices(ab);
            free(ab);
            return 0;
        }
        fprintf(stderr, "No backend available to list devices\n");
        return 1;
    }

    if (cfg.is_sender) {
        return run_sender(&cfg);
    }
    return run_receiver(&cfg);
}
