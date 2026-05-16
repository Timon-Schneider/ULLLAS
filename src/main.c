#include "audio.h"
#include "config.h"
#include "network.h"
#include "ringbuffer.h"
#include "pcm_proto.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #define sleep_ms(ms) Sleep(ms)
    static volatile LONG g_running = 1;
    static BOOL WINAPI ctrl_handler(DWORD type) {
        (void)type;
        InterlockedExchange(&g_running, 0);
        return TRUE;
    }
#else
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <semaphore.h>
#endif
    #define sleep_ms(ms) usleep((ms) * 1000)
    static volatile sig_atomic_t g_running = 1;
    static void sig_handler(int sig) { (void)sig; g_running = 0; }
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RB_CAPACITY (16384)
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

typedef struct {
    Config     *cfg;
    RingBuffer  rb;
    UdpSender   tx;
    AudioBackend *audio;
    int32_t   **bufs;
    int32_t    *buf_flat;
    int         channels;
    int         buffer_size;
    int         sample_rate;
    uint64_t    total_frames_tx;
    uint64_t    total_lost;
    int32_t     peak_level;
#ifdef _WIN32
    HANDLE      data_event;
#elif defined(__APPLE__)
    dispatch_semaphore_t data_sem;
#else
    sem_t       data_sem;
#endif
} SenderState;

typedef struct {
    Config     *cfg;
    RingBuffer  rb;
    UdpReceiver rx;
    AudioBackend *audio;
    int32_t   **bufs;
    int32_t    *buf_flat;
    int         channels;
    int         buffer_size;
    int         sample_rate;
    int         jitter_target;
    uint64_t    total_frames_rx;
    uint64_t    total_lost;
    int32_t     peak_level;
    int         underruns;
    int         started;
} ReceiverState;

static int sender_callback(const int32_t * const *inputs,
                           int32_t * const *outputs,
                           int nframes, void *userdata) {
    SenderState *st = (SenderState *)userdata;
    (void)outputs;

    size_t written = rb_write(&st->rb, inputs, st->channels, (size_t)nframes);
    if ((int)written < nframes) {
        st->total_lost += (size_t)nframes - written;
    }
#ifdef _WIN32
    SetEvent(st->data_event);
#elif defined(__APPLE__)
    dispatch_semaphore_signal(st->data_sem);
#else
    sem_post(&st->data_sem);
#endif
    return 0;
}

static int receiver_callback(const int32_t * const *inputs,
                             int32_t * const *outputs,
                             int nframes, void *userdata) {
    ReceiverState *st = (ReceiverState *)userdata;
    (void)inputs;

    size_t avail = rb_available_read(&st->rb) / st->channels;

    if (!st->started) {
        if (avail >= (size_t)st->jitter_target) {
            st->started = 1;
        } else {
            for (int ch = 0; ch < st->channels; ch++) {
                memset(outputs[ch], 0, (size_t)nframes * sizeof(int32_t));
            }
            return 0;
        }
    }

    size_t read = rb_read(&st->rb, outputs, st->channels, (size_t)nframes);
    if ((int)read < nframes) {
        int missing = nframes - (int)read;
        for (int ch = 0; ch < st->channels; ch++) {
            memset(outputs[ch] + read, 0, (size_t)missing * sizeof(int32_t));
        }
        st->underruns++;
    }
    st->total_frames_rx += nframes;
    return 0;
}

static AudioBackend *create_audio_backend(const Config *cfg) {
#ifdef HAS_ASIO
    if (strcmp(cfg->backend, "asio") == 0) {
        return asio_backend_create();
    }
#endif
#ifdef HAS_COREAUDIO
    if (strcmp(cfg->backend, "coreaudio") == 0) {
        return coreaudio_backend_create();
    }
#endif
#ifdef HAS_JACK
    if (strcmp(cfg->backend, "jack") == 0) {
        return jack_backend_create();
    }
#endif
    fprintf(stderr, "No supported backend available for '%s'\n", cfg->backend);
    return NULL;
}

static int32_t **alloc_bufs(int channels, int buffer_size) {
    int32_t *flat = (int32_t *)calloc((size_t)channels * buffer_size, sizeof(int32_t));
    int32_t **bufs = (int32_t **)calloc((size_t)channels, sizeof(int32_t *));
    if (!flat || !bufs) {
        free(flat); free(bufs);
        return NULL;
    }
    for (int ch = 0; ch < channels; ch++) {
        bufs[ch] = flat + ch * buffer_size;
    }
    return bufs;
}

static void free_bufs(int32_t **bufs) {
    if (bufs) {
        free(bufs[0]);
        free(bufs);
    }
}

static int32_t compute_peak(const int32_t * const *bufs, int channels, int samples) {
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

static int run_sender(Config *cfg) {
    SenderState st;
    memset(&st, 0, sizeof(st));
    st.cfg         = cfg;
    st.channels    = cfg->in_channel_count;
    st.buffer_size = (int)cfg->buffer_size;

    if (rb_init(&st.rb, RB_CAPACITY) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }

#ifdef _WIN32
    st.data_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!st.data_event) {
        fprintf(stderr, "Failed to create event\n");
        rb_destroy(&st.rb);
        return -1;
    }
#elif defined(__APPLE__)
    st.data_sem = dispatch_semaphore_create(0);
    if (!st.data_sem) {
        fprintf(stderr, "Failed to create semaphore\n");
        rb_destroy(&st.rb);
        return -1;
    }
#else
    if (sem_init(&st.data_sem, 0, 0) != 0) {
        perror("sem_init");
        rb_destroy(&st.rb);
        return -1;
    }
#endif

    st.audio = create_audio_backend(cfg);
    if (!st.audio) {
        rb_destroy(&st.rb);
        return -1;
    }

    if (st.audio->vtable->init(st.audio, cfg->device_name,
                                cfg->sample_rate, st.channels, 0,
                                (int)cfg->buffer_size,
                                sender_callback, &st) != 0) {
        fprintf(stderr, "Failed to init audio\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    st.buffer_size = st.audio->buffer_size;
    if ((unsigned int)st.buffer_size != cfg->buffer_size) {
        fprintf(stderr, "Warning: requested buffer size %u, driver uses %d\n",
                cfg->buffer_size, st.buffer_size);
    }
    st.sample_rate = (int)st.audio->sample_rate;
    if ((unsigned int)st.sample_rate != cfg->sample_rate) {
        fprintf(stderr, "Warning: requested sample rate %u, driver uses %d\n",
                cfg->sample_rate, st.sample_rate);
    }

    if (udp_sender_init(&st.tx, cfg->target_addr, cfg->port,
                        cfg->iface_addr, cfg->use_multicast,
                        st.sample_rate, st.channels,
                        (int)cfg->bit_depth, st.buffer_size) != 0) {
        fprintf(stderr, "Failed to init network sender\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    st.bufs = alloc_bufs(st.channels, st.buffer_size);
    if (!st.bufs) {
        fprintf(stderr, "Failed to allocate buffers\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        udp_sender_destroy(&st.tx);
        rb_destroy(&st.rb);
        return -1;
    }

    if (st.audio->vtable->start(st.audio) != 0) {
        fprintf(stderr, "Failed to start audio\n");
        st.audio->vtable->destroy(st.audio);
        goto cleanup;
    }

    fprintf(stderr, "ULLLAS sender: %d ch @ %u Hz, %u-bit, buffer %u -> %s\n",
            st.channels, (unsigned int)st.sample_rate, cfg->bit_depth,
            (unsigned int)st.buffer_size, cfg->target_addr);
#ifdef _WIN32
    fprintf(stderr, "Configure ASIO4ALL tray icon to enable your input device.\n");
#endif

    {
        uint64_t last_status_frames = 0;
        while (g_running) {
            while (g_running) {
                size_t avail_samples = rb_available_read(&st.rb) / (size_t)st.channels;
                if (avail_samples < (size_t)st.buffer_size) break;

                rb_read(&st.rb, st.bufs, st.channels, (size_t)st.buffer_size);
                if (cfg->verbose) {
                    st.peak_level = compute_peak((const int32_t * const *)st.bufs,
                                                 st.channels, st.buffer_size);
                }
                int sent = udp_sender_send(&st.tx, (const int32_t * const *)st.bufs,
                                           st.channels, st.buffer_size);
                if (sent < 0) {
                    st.total_lost += st.buffer_size;
                } else {
                    st.total_frames_tx += st.buffer_size;
                }
            }

            {
                uint64_t elapsed_frames = st.total_frames_tx - last_status_frames;
                uint64_t elapsed_secs = (uint64_t)st.sample_rate > 0
                    ? elapsed_frames / (uint64_t)st.sample_rate : 0;
                if (elapsed_secs >= STATUS_INTERVAL_S) {
                    if (cfg->verbose) {
                        fprintf(stderr, "\rTX: %llu frames, lost %llu, buf %u, peak %d          \n",
                                (unsigned long long)st.total_frames_tx,
                                (unsigned long long)st.total_lost,
                                (unsigned)(rb_available_read(&st.rb) / (size_t)st.channels),
                                (int)st.peak_level);
                    } else {
                        fprintf(stderr, "\rTX: %llu frames, lost %llu, buf %u          \n",
                                (unsigned long long)st.total_frames_tx,
                                (unsigned long long)st.total_lost,
                                (unsigned)(rb_available_read(&st.rb) / (size_t)st.channels));
                    }
                    last_status_frames = st.total_frames_tx;
                }
            }

#ifdef _WIN32
            WaitForSingleObject(st.data_event, 1000);
#elif defined(__APPLE__)
            dispatch_semaphore_wait(st.data_sem, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
#else
            {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                sem_timedwait(&st.data_sem, &ts);
            }
#endif
        }
    }

    fprintf(stderr, "\nShutting down sender...\n");
    st.audio->vtable->stop(st.audio);
    st.audio->vtable->destroy(st.audio);
    free_bufs(st.bufs);
    free(st.audio);
    udp_sender_destroy(&st.tx);
#ifdef _WIN32
    CloseHandle(st.data_event);
#elif defined(__APPLE__)
    dispatch_release(st.data_sem);
#else
    sem_destroy(&st.data_sem);
#endif
    rb_destroy(&st.rb);
    return 0;

cleanup:
    free_bufs(st.bufs);
    free(st.audio);
    udp_sender_destroy(&st.tx);
#ifdef _WIN32
    if (st.data_event) CloseHandle(st.data_event);
#elif defined(__APPLE__)
    if (st.data_sem) dispatch_release(st.data_sem);
#else
    sem_destroy(&st.data_sem);
#endif
    rb_destroy(&st.rb);
    return -1;
}

static int run_receiver(Config *cfg) {
    ReceiverState st;
    memset(&st, 0, sizeof(st));
    st.cfg           = cfg;
    st.channels      = cfg->out_channel_count;
    st.buffer_size   = (int)cfg->buffer_size;
    st.started       = 0;

    if (rb_init(&st.rb, RB_CAPACITY) != 0) {
        fprintf(stderr, "Failed to init ring buffer\n");
        return -1;
    }

    st.audio = create_audio_backend(cfg);
    if (!st.audio) {
        rb_destroy(&st.rb);
        return -1;
    }

    if (st.audio->vtable->init(st.audio, cfg->device_name,
                                cfg->sample_rate, 0, st.channels,
                                (int)cfg->buffer_size,
                                receiver_callback, &st) != 0) {
        fprintf(stderr, "Failed to init audio\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    st.buffer_size   = st.audio->buffer_size;
    if ((unsigned int)st.buffer_size != cfg->buffer_size) {
        fprintf(stderr, "Warning: requested buffer size %u, driver uses %d\n",
                cfg->buffer_size, st.buffer_size);
    }
    st.sample_rate = (int)st.audio->sample_rate;
    if ((unsigned int)st.sample_rate != cfg->sample_rate) {
        fprintf(stderr, "Warning: requested sample rate %u, driver uses %d\n",
                cfg->sample_rate, st.sample_rate);
    }
    st.jitter_target = (int)cfg->jitter_packets * st.buffer_size;

    const char *mcast = (cfg->use_multicast && cfg->target_addr[0] != '\0') ? cfg->target_addr : NULL;
    if (udp_receiver_init(&st.rx, cfg->bind_addr, cfg->port,
                          mcast, cfg->use_multicast,
                          st.channels, (int)cfg->bit_depth,
                          st.buffer_size, cfg->jitter_packets) != 0) {
        fprintf(stderr, "Failed to init network receiver\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        rb_destroy(&st.rb);
        return -1;
    }

    st.bufs = alloc_bufs(st.channels, 4096);
    if (!st.bufs) {
        fprintf(stderr, "Failed to allocate buffers\n");
        st.audio->vtable->destroy(st.audio);
        free(st.audio);
        udp_receiver_destroy(&st.rx);
        rb_destroy(&st.rb);
        return -1;
    }

    if (st.audio->vtable->start(st.audio) != 0) {
        fprintf(stderr, "Failed to start audio\n");
        st.audio->vtable->destroy(st.audio);
        goto cleanup;
    }

    fprintf(stderr, "ULLLAS receiver: %d ch @ %u Hz, %u-bit, buffer %u, jitter %u pkts -> port %u\n",
            st.channels, (unsigned int)st.sample_rate, cfg->bit_depth,
            (unsigned int)st.buffer_size, cfg->jitter_packets, cfg->port);
#ifndef __APPLE__
    fprintf(stderr, "IMPORTANT: Click the ASIO4ALL tray icon and enable your speaker/headphone output!\n");
#endif
    fprintf(stderr, "Waiting for stream (need %d samples to start)...\n", st.jitter_target);

    {
#ifdef _WIN32
        u_long block = 0;
        ioctlsocket(st.rx.sock, FIONBIO, &block);
        DWORD timeout_ms = 500;
        setsockopt(st.rx.sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&timeout_ms, sizeof(timeout_ms));
#else
        int flags = fcntl(st.rx.sock, F_GETFL, 0);
        fcntl(st.rx.sock, F_SETFL, flags & ~O_NONBLOCK);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 500000;
        setsockopt(st.rx.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    {
        time_t last_status = time(NULL);
        int status_tick = 0;
        while (g_running) {
            uint32_t seq;
            int samples = udp_receiver_recv(&st.rx, st.bufs, 4096, &seq, NULL);
            if (samples < 0) {
                if (!g_running) break;
                sleep_ms(2);
                continue;
            }
            if (samples > 0) {
                if (cfg->verbose) {
                    st.peak_level = compute_peak((const int32_t * const *)st.bufs,
                                                 st.channels, samples);
                }
                size_t written = rb_write(&st.rb, (const int32_t * const *)st.bufs,
                                          st.channels, (size_t)samples);
                if (written < (size_t)samples) {
                    st.total_lost += (size_t)samples - written;
                }

                if (!st.started) {
                    size_t avail = rb_available_read(&st.rb) / st.channels;
                    if (avail >= (size_t)st.jitter_target) {
                        st.started = 1;
                    }
                }
            }

            if (++status_tick >= 64) {
                status_tick = 0;
                time_t now = time(NULL);
                if (now - last_status >= STATUS_INTERVAL_S) {
                    size_t avail = rb_available_read(&st.rb) / (size_t)st.channels;
                    if (cfg->verbose) {
                        fprintf(stderr, "\rRX: %llu frames, lost %llu, underruns %d, buf %u, peak %d          \n",
                                (unsigned long long)st.total_frames_rx,
                                (unsigned long long)st.total_lost,
                                st.underruns,
                                (unsigned)avail,
                                (int)st.peak_level);
                    } else {
                        fprintf(stderr, "\rRX: %llu frames, lost %llu, underruns %d, buf %u          \n",
                                (unsigned long long)st.total_frames_rx,
                                (unsigned long long)st.total_lost,
                                st.underruns,
                                (unsigned)avail);
                    }
                    last_status = now;
                }
            }
        }
    }

    fprintf(stderr, "\nShutting down receiver...\n");
    st.audio->vtable->stop(st.audio);
    st.audio->vtable->destroy(st.audio);
    free_bufs(st.bufs);
    free(st.audio);
    udp_receiver_destroy(&st.rx);
    rb_destroy(&st.rb);
    return 0;

cleanup:
    free_bufs(st.bufs);
    free(st.audio);
    udp_receiver_destroy(&st.rx);
    rb_destroy(&st.rb);
    return -1;
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
    } else {
        return run_receiver(&cfg);
    }
}
