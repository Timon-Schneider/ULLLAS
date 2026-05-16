#ifndef ULLLAS_AUDIO_H
#define ULLLAS_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    int max_in_channels;
    int max_out_channels;
} AudioDeviceInfo;

typedef int (*audio_callback_t)(
    const int32_t * const *inputs,
    int32_t * const *outputs,
    int nframes,
    void *userdata
);

typedef struct AudioBackend AudioBackend;

typedef struct {
    int (*init)(AudioBackend *ab, const char *device_name,
                unsigned int sample_rate, int num_in_ch, int num_out_ch,
                int buffer_size, audio_callback_t cb, void *userdata);
    int (*start)(AudioBackend *ab);
    int (*stop)(AudioBackend *ab);
    void (*destroy)(AudioBackend *ab);
    int (*list_devices)(AudioBackend *ab);
} AudioBackendVtable;

struct AudioBackend {
    const AudioBackendVtable *vtable;
    void *ctx;
    int num_input_channels;
    int num_output_channels;
    int buffer_size;
    unsigned int sample_rate;
    int is_active;
};

AudioBackend *audio_backend_create(const char *backend_name);

#ifdef __cplusplus
}
#endif

#endif
