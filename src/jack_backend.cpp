#include "jack_backend.h"

#ifdef HAS_JACK

#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct JackContext {
    jack_client_t *client;
    std::vector<jack_port_t *> in_ports;
    std::vector<jack_port_t *> out_ports;
    audio_callback_t user_callback;
    void *user_data;
    int num_in_ch;
    int num_out_ch;
    int buffer_size;
    bool auto_connect;
    bool active;

    std::vector<int32_t> in_scratch;
    std::vector<int32_t> out_scratch;
    std::vector<int32_t *> in_s32;
    std::vector<int32_t *> out_s32;
};

static int jack_process_cb(jack_nframes_t nframes, void *arg) {
    JackContext *ctx = (JackContext *)arg;
    int nc_in  = ctx->num_in_ch;
    int nc_out = ctx->num_out_ch;

    ctx->in_scratch.assign(nc_in * nframes, 0);
    ctx->out_scratch.assign(nc_out * nframes, 0);
    ctx->in_s32.resize(nc_in);
    ctx->out_s32.resize(nc_out);

    for (int i = 0; i < nc_in; i++) {
        jack_default_audio_sample_t *src = (jack_default_audio_sample_t *)
            jack_port_get_buffer(ctx->in_ports[i], nframes);
        int32_t *dst = ctx->in_s32[i] = ctx->in_scratch.data() + i * nframes;
        for (jack_nframes_t j = 0; j < nframes; j++) {
            float f = src[j];
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            dst[j] = (int32_t)(f * 2147483647.0f);
        }
    }

    for (int i = 0; i < nc_out; i++) {
        ctx->out_s32[i] = ctx->out_scratch.data() + i * nframes;
    }
    memset(ctx->out_scratch.data(), 0, nc_out * nframes * sizeof(int32_t));

    if (ctx->user_callback) {
        ctx->user_callback(
            nc_in > 0 ? ctx->in_s32.data() : NULL,
            nc_out > 0 ? ctx->out_s32.data() : NULL,
            (int)nframes, ctx->user_data);
    }

    for (int i = 0; i < nc_out; i++) {
        jack_default_audio_sample_t *dst = (jack_default_audio_sample_t *)
            jack_port_get_buffer(ctx->out_ports[i], nframes);
        int32_t *src = ctx->out_s32[i];
        for (jack_nframes_t j = 0; j < nframes; j++) {
            dst[j] = (float)src[j] / 2147483648.0f;
        }
    }

    return 0;
}

static int jack_srate_cb(jack_nframes_t nframes, void *arg) {
    (void)nframes;
    (void)arg;
    return 0;
}

static void jack_shutdown_cb(void *arg) {
    (void)arg;
    fprintf(stderr, "JACK: server shutdown\n");
}

static int jack_init(AudioBackend *ab, const char *device_name,
                     unsigned int sample_rate, int num_in_ch, int num_out_ch,
                     int buffer_size, audio_callback_t cb, void *userdata) {
    (void)sample_rate;
    (void)buffer_size;

    {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "JACK: --sample-rate and --buffer are ignored (JACK server controls these)\n");
            warned = 1;
        }
    }

    JackContext *ctx = new JackContext();
    ab->ctx = ctx;

    ctx->num_in_ch     = num_in_ch;
    ctx->num_out_ch    = num_out_ch;
    ctx->user_callback = cb;
    ctx->user_data     = userdata;
    ctx->auto_connect  = true;
    ctx->active        = false;

    const char *client_name = device_name && device_name[0] ?
                               device_name : "ulllas";
    jack_status_t status;
    ctx->client = jack_client_open(client_name, JackNullOption, &status, NULL);
    if (!ctx->client) {
        fprintf(stderr, "JACK: Failed to open client\n");
        delete ctx; ab->ctx = NULL;
        return -1;
    }

    ctx->buffer_size = (int)jack_get_buffer_size(ctx->client);

    jack_set_process_callback(ctx->client, jack_process_cb, ctx);
    jack_set_sample_rate_callback(ctx->client, jack_srate_cb, ctx);
    jack_on_shutdown(ctx->client, jack_shutdown_cb, NULL);

    char port_name[64];
    for (int i = 0; i < num_in_ch; i++) {
        snprintf(port_name, sizeof(port_name), "input_%d", i);
        jack_port_t *p = jack_port_register(ctx->client, port_name,
                                             JACK_DEFAULT_AUDIO_TYPE,
                                             JackPortIsInput, 0);
        if (!p) {
            fprintf(stderr, "JACK: Failed to register input port %s\n", port_name);
            jack_client_close(ctx->client);
            delete ctx; ab->ctx = NULL;
            return -1;
        }
        ctx->in_ports.push_back(p);
    }

    for (int i = 0; i < num_out_ch; i++) {
        snprintf(port_name, sizeof(port_name), "output_%d", i);
        jack_port_t *p = jack_port_register(ctx->client, port_name,
                                             JACK_DEFAULT_AUDIO_TYPE,
                                             JackPortIsOutput, 0);
        if (!p) {
            fprintf(stderr, "JACK: Failed to register output port %s\n", port_name);
            jack_client_close(ctx->client);
            delete ctx; ab->ctx = NULL;
            return -1;
        }
        ctx->out_ports.push_back(p);
    }

    ab->num_input_channels  = num_in_ch;
    ab->num_output_channels = num_out_ch;
    ab->buffer_size         = ctx->buffer_size;
    ab->sample_rate         = (unsigned int)jack_get_sample_rate(ctx->client);

    ctx->in_scratch.reserve(num_in_ch * ctx->buffer_size);
    ctx->out_scratch.reserve(num_out_ch * ctx->buffer_size);
    ctx->in_s32.reserve(num_in_ch);
    ctx->out_s32.reserve(num_out_ch);

    return 0;
}

static int jack_start(AudioBackend *ab) {
    JackContext *ctx = (JackContext *)ab->ctx;
    if (!ctx) return -1;

    if (jack_activate(ctx->client) != 0) {
        fprintf(stderr, "JACK: Failed to activate client\n");
        return -1;
    }
    ctx->active = true;
    ab->is_active = 1;

    if (ctx->auto_connect) {
        if (ctx->num_in_ch > 0) {
            const char **ports = jack_get_ports(ctx->client, NULL, NULL,
                                                 JackPortIsPhysical | JackPortIsOutput);
            if (ports) {
                int pcount = 0;
                while (ports[pcount]) pcount++;
                for (int i = 0; i < ctx->num_in_ch && i < pcount; i++) {
                    if (jack_connect(ctx->client, ports[i],
                                     jack_port_name(ctx->in_ports[i])) != 0) {
                        fprintf(stderr, "JACK: Failed to auto-connect input %d\n", i);
                    }
                }
                jack_free(ports);
            }
        }
        if (ctx->num_out_ch > 0) {
            const char **ports = jack_get_ports(ctx->client, NULL, NULL,
                                                 JackPortIsPhysical | JackPortIsInput);
            if (ports) {
                int pcount = 0;
                while (ports[pcount]) pcount++;
                for (int i = 0; i < ctx->num_out_ch && i < pcount; i++) {
                    if (jack_connect(ctx->client,
                                     jack_port_name(ctx->out_ports[i]),
                                     ports[i]) != 0) {
                        fprintf(stderr, "JACK: Failed to auto-connect output %d\n", i);
                    }
                }
                jack_free(ports);
            }
        }
    }

    return 0;
}

static int jack_stop(AudioBackend *ab) {
    JackContext *ctx = (JackContext *)ab->ctx;
    if (!ctx) return -1;
    jack_deactivate(ctx->client);
    ctx->active = false;
    ab->is_active = 0;
    return 0;
}

static void jack_destroy(AudioBackend *ab) {
    JackContext *ctx = (JackContext *)ab->ctx;
    if (!ctx) return;
    if (ctx->active) jack_deactivate(ctx->client);
    jack_client_close(ctx->client);
    delete ctx;
    ab->ctx = NULL;
}

static int jack_list_devices(AudioBackend *ab) {
    (void)ab;
    printf("JACK: Use system:capture_* and system:playback_* ports.\n");
    printf("     Run 'jack_lsp' to see available ports.\n");
    return 0;
}

static AudioBackendVtable jack_vtable = {
    jack_init,
    jack_start,
    jack_stop,
    jack_destroy,
    jack_list_devices
};

extern "C" AudioBackend *jack_backend_create(void) {
    AudioBackend *ab = (AudioBackend *)calloc(1, sizeof(AudioBackend));
    if (!ab) return NULL;
    ab->vtable = &jack_vtable;
    ab->ctx    = NULL;
    return ab;
}

#else

extern "C" AudioBackend *jack_backend_create(void) {
    fprintf(stderr, "JACK support not compiled in (install libjack-dev)\n");
    return NULL;
}

#endif
