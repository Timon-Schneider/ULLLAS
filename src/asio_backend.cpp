#include "asio_backend.h"

#ifdef HAS_ASIO

#include "asiosys.h"
#include "asiodrivers.h"
#include "asio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <atomic>

extern AsioDrivers *asioDrivers;

struct AsioContext {
    ASIOBufferInfo *bufferInfos;
    ASIOChannelInfo *channelInfos;
    long totalInputs;
    long totalOutputs;
    long bufferSize;
    ASIOCallbacks callbacks;
    audio_callback_t user_callback;
    void *user_data;
    std::atomic<bool> running;
    double sampleRate;
    int num_in_ch;
    int num_out_ch;
    int *in_sample_types;
    int *out_sample_types;
    HWND hwnd;

    std::vector<int32_t *> in_bufs;
    std::vector<int32_t *> out_bufs;
    std::vector<int32_t> in_scratch;
    std::vector<int32_t> out_scratch;
};

static AsioContext *g_asio_ctx = NULL;

static long asioMessage(long selector, long value, void *message, double *opt) {
    (void)selector; (void)value; (void)message; (void)opt;
    return 0;
}

static void asio_process_buffers(long index) {
    AsioContext *ctx = g_asio_ctx;
    if (!ctx->running.load(std::memory_order_relaxed)) return;

    long nframes = ctx->bufferSize;

    for (int i = 0; i < ctx->num_in_ch; i++) {
        ASIOBufferInfo *bi = &ctx->bufferInfos[i];
        void *src = bi->buffers[index];
        int32_t *dst = ctx->in_bufs[i];
        switch (ctx->in_sample_types[i]) {
        case ASIOSTInt16LSB:
            for (long j = 0; j < nframes; j++)
                dst[j] = ((int32_t)((int16_t *)src)[j]) << 16;
            break;
        case ASIOSTInt24LSB:
            for (long j = 0; j < nframes; j++) {
                uint8_t *b = (uint8_t *)src + j * 3;
                int32_t v = (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16));
                if (v & 0x800000) v |= 0xFF000000;
                dst[j] = v;
            }
            break;
        case ASIOSTFloat32LSB:
            for (long j = 0; j < nframes; j++) {
                float f = ((float *)src)[j];
                if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
                dst[j] = (int32_t)(f * 2147483647.0f);
            }
            break;
        default:
            memcpy(dst, src, nframes * sizeof(int32_t));
            break;
        }
    }

    memset(ctx->out_scratch.data(), 0, ctx->num_out_ch * nframes * sizeof(int32_t));
    for (int i = 0; i < ctx->num_out_ch; i++)
        ctx->out_bufs[i] = ctx->out_scratch.data() + i * nframes;

    if (ctx->user_callback) {
        ctx->user_callback(
            ctx->num_in_ch > 0 ? ctx->in_bufs.data() : NULL,
            ctx->num_out_ch > 0 ? ctx->out_bufs.data() : NULL,
            (int)nframes, ctx->user_data);
    }

    for (int i = 0; i < ctx->num_out_ch; i++) {
        int idx = ctx->num_in_ch + i;
        ASIOBufferInfo *bi = &ctx->bufferInfos[idx];
        int32_t *src = ctx->out_bufs[i];
        void *dst = bi->buffers[index];
        if (!dst) continue;
        switch (ctx->out_sample_types[i]) {
        case ASIOSTInt16LSB:
            for (long j = 0; j < nframes; j++)
                ((int16_t *)dst)[j] = (int16_t)(src[j] >> 16);
            break;
        case ASIOSTInt24LSB:
            for (long j = 0; j < nframes; j++) {
                int32_t s = src[j];
                uint8_t *b = (uint8_t *)dst + j * 3;
                b[0] = (uint8_t)(s & 0xFF);
                b[1] = (uint8_t)((s >> 8) & 0xFF);
                b[2] = (uint8_t)((s >> 16) & 0xFF);
            }
            break;
        case ASIOSTFloat32LSB:
            for (long j = 0; j < nframes; j++) {
                float f = (float)src[j] / 2147483648.0f;
                if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
                ((float *)dst)[j] = f;
            }
            break;
        default:
            memcpy(dst, src, nframes * sizeof(int32_t));
            break;
        }
    }

    if (ctx->num_out_ch > 0 && ctx->running.load(std::memory_order_relaxed)) ASIOOutputReady();
}

static void asioBufferSwitch(long index, ASIOBool directProcess) {
    (void)directProcess;
    asio_process_buffers(index);
}

static ASIOTime *asioBufferSwitchTimeInfo(ASIOTime *timeInfo, long index, ASIOBool directProcess) {
    (void)directProcess;
    asio_process_buffers(index);
    return timeInfo;
}

static int asio_init(AudioBackend *ab, const char *device_name,
                     unsigned int sample_rate, int num_in_ch, int num_out_ch,
                     int buffer_size, audio_callback_t cb, void *userdata) {
    AsioContext *ctx = new AsioContext();
    if (!ctx) return -1;
    ctx->bufferInfos     = nullptr;
    ctx->channelInfos    = nullptr;
    ctx->totalInputs     = 0;
    ctx->totalOutputs    = 0;
    ctx->bufferSize      = 0;
    ctx->user_callback   = nullptr;
    ctx->user_data       = nullptr;
    ctx->running.store(false);
    ctx->sampleRate      = 0.0;
    ctx->num_in_ch       = 0;
    ctx->num_out_ch      = 0;
    ctx->in_sample_types = nullptr;
    ctx->out_sample_types = nullptr;
    ctx->hwnd             = NULL;
    ab->ctx = ctx;
    g_asio_ctx = ctx;

    ctx->num_in_ch     = num_in_ch;
    ctx->num_out_ch    = num_out_ch;
    ctx->user_callback = cb;
    ctx->user_data     = userdata;
    ctx->sampleRate    = (double)sample_rate;

    ctx->hwnd = CreateWindowA("STATIC", "ULLLAS", WS_POPUP, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (!asioDrivers) asioDrivers = new AsioDrivers();

    int driverCount = asioDrivers->asioGetNumDev();
    if (driverCount <= 0) {
        fprintf(stderr, "ASIO: No drivers found\n");
        CoUninitialize();
        if (ctx->hwnd) DestroyWindow(ctx->hwnd);
        delete ctx; ab->ctx = NULL; g_asio_ctx = NULL;
        return -1;
    }

    int driverIndex = 0;
    if (device_name && device_name[0] != '\0') {
        driverIndex = -1;
        for (int i = 0; i < driverCount; i++) {
            char name[128];
            if (asioDrivers->asioGetDriverName(i, name, sizeof(name)) == 0) {
                if (strstr(name, device_name) != NULL) { driverIndex = i; break; }
            }
        }
        if (driverIndex < 0) driverIndex = 0;
    }

    char driverNameBuf[128];
    asioDrivers->asioGetDriverName(driverIndex, driverNameBuf, sizeof(driverNameBuf));

    if (!asioDrivers->loadDriver(driverNameBuf)) {
        fprintf(stderr, "ASIO: Failed to load driver '%s'\n", driverNameBuf);
        CoUninitialize();
        if (ctx->hwnd) DestroyWindow(ctx->hwnd);
        delete ctx; ab->ctx = NULL; g_asio_ctx = NULL;
        return -1;
    }

    ASIODriverInfo driverInfo;
    memset(&driverInfo, 0, sizeof(driverInfo));
    driverInfo.asioVersion = 2;
    driverInfo.sysRef = ctx->hwnd;
    if (ASIOInit(&driverInfo) != ASE_OK) {
        fprintf(stderr, "ASIO: Init failed\n");
        asioDrivers->removeCurrentDriver();
        CoUninitialize();
        if (ctx->hwnd) DestroyWindow(ctx->hwnd);
        delete ctx; ab->ctx = NULL; g_asio_ctx = NULL;
        return -1;
    }

    long totalIn, totalOut;
    ASIOGetChannels(&totalIn, &totalOut);
    ctx->totalInputs  = totalIn;
    ctx->totalOutputs = totalOut;

    if (num_in_ch > totalIn || num_out_ch > totalOut) {
        fprintf(stderr, "ASIO: Not enough channels (in:%ld/%d out:%ld/%d)\n",
                totalIn, num_in_ch, totalOut, num_out_ch);
        ASIOExit(); asioDrivers->removeCurrentDriver();
        CoUninitialize(); if (ctx->hwnd) DestroyWindow(ctx->hwnd);
        delete ctx; ab->ctx = NULL; g_asio_ctx = NULL;
        return -1;
    }

    if (ASIOCanSampleRate(ctx->sampleRate) != ASE_OK) {
        fprintf(stderr, "ASIO: Sample rate %u not supported\n", sample_rate);
        ASIOExit(); asioDrivers->removeCurrentDriver();
        CoUninitialize(); if (ctx->hwnd) DestroyWindow(ctx->hwnd);
        delete ctx; ab->ctx = NULL; g_asio_ctx = NULL;
        return -1;
    }
    ASIOSetSampleRate(ctx->sampleRate);

    long longBufs = num_in_ch + num_out_ch;
    ctx->bufferInfos = new ASIOBufferInfo[longBufs];
    ctx->in_sample_types  = new int[num_in_ch > 0 ? num_in_ch : 1];
    ctx->out_sample_types = new int[num_out_ch > 0 ? num_out_ch : 1];
    memset(ctx->bufferInfos, 0, longBufs * sizeof(ASIOBufferInfo));

    for (int i = 0; i < num_in_ch; i++) {
        ctx->bufferInfos[i].isInput = ASIOTrue;
        ctx->bufferInfos[i].channelNum = i;
    }
    for (int i = 0; i < num_out_ch; i++) {
        int idx = num_in_ch + i;
        ctx->bufferInfos[idx].isInput = ASIOFalse;
        ctx->bufferInfos[idx].channelNum = i;
    }

    memset(&ctx->callbacks, 0, sizeof(ctx->callbacks));
    ctx->callbacks.bufferSwitch         = asioBufferSwitch;
    ctx->callbacks.bufferSwitchTimeInfo = asioBufferSwitchTimeInfo;
    ctx->callbacks.asioMessage          = asioMessage;

    long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;
    ASIOGetBufferSize(&minSize, &maxSize, &preferredSize, &granularity);
    if (buffer_size < (int)minSize) buffer_size = (int)minSize;
    if (buffer_size > (int)maxSize) buffer_size = (int)maxSize;
    if (granularity > 0 && (buffer_size % granularity) != 0)
        buffer_size = (int)((buffer_size / granularity) * granularity);
    ctx->bufferSize = buffer_size;

    if (ASIOCreateBuffers(ctx->bufferInfos, longBufs, ctx->bufferSize, &ctx->callbacks) != ASE_OK) {
        fprintf(stderr, "ASIO: CreateBuffers failed\n");
        ASIOExit(); asioDrivers->removeCurrentDriver();
        CoUninitialize(); if (ctx->hwnd) DestroyWindow(ctx->hwnd);
        delete[] ctx->bufferInfos; delete[] ctx->in_sample_types; delete[] ctx->out_sample_types;
        delete ctx; ab->ctx = NULL; g_asio_ctx = NULL;
        return -1;
    }

    ctx->channelInfos = new ASIOChannelInfo[longBufs];
    for (int i = 0; i < num_in_ch; i++) {
        ctx->channelInfos[i].channel = i;
        ctx->channelInfos[i].isInput = ASIOTrue;
        ASIOGetChannelInfo(&ctx->channelInfos[i]);
        ctx->in_sample_types[i] = ctx->channelInfos[i].type;
    }
    for (int i = 0; i < num_out_ch; i++) {
        int idx = num_in_ch + i;
        ctx->channelInfos[idx].channel = i;
        ctx->channelInfos[idx].isInput = ASIOFalse;
        ASIOGetChannelInfo(&ctx->channelInfos[idx]);
        ctx->out_sample_types[i] = ctx->channelInfos[idx].type;
    }

    if (num_out_ch > 0) {
        for (int i = 0; i < num_out_ch; i++) {
            const char *t = "?";
            switch (ctx->out_sample_types[i]) {
            case ASIOSTInt16LSB: t = "Int16LSB"; break;
            case ASIOSTInt24LSB: t = "Int24LSB"; break;
            case ASIOSTInt32LSB: t = "Int32LSB"; break;
            case ASIOSTFloat32LSB: t = "Float32LSB"; break;
            case ASIOSTInt32LSB24: t = "Int32LSB24"; break;
            case ASIOSTInt32LSB20: t = "Int32LSB20"; break;
            case ASIOSTInt32LSB18: t = "Int32LSB18"; break;
            case ASIOSTInt32LSB16: t = "Int32LSB16"; break;
            }
        }
    }

    ctx->in_scratch.resize(num_in_ch * buffer_size);
    ctx->out_scratch.resize(num_out_ch * buffer_size);
    ctx->in_bufs.resize(num_in_ch > 0 ? num_in_ch : 1);
    ctx->out_bufs.resize(num_out_ch > 0 ? num_out_ch : 1);
    for (int i = 0; i < num_in_ch; i++)
        ctx->in_bufs[i] = ctx->in_scratch.data() + i * buffer_size;
    for (int i = 0; i < num_out_ch; i++)
        ctx->out_bufs[i] = ctx->out_scratch.data() + i * buffer_size;

    ctx->running.store(true);
    ab->num_input_channels  = num_in_ch;
    ab->num_output_channels = num_out_ch;
    ab->buffer_size         = buffer_size;
    ab->sample_rate         = sample_rate;
    return 0;
}

static int asio_start(AudioBackend *ab) {
    AsioContext *ctx = (AsioContext *)ab->ctx;
    if (!ctx) return -1;
    if (ASIOStart() != ASE_OK) return -1;
    ab->is_active = 1;
    return 0;
}

static int asio_stop(AudioBackend *ab) {
    AsioContext *ctx = (AsioContext *)ab->ctx;
    if (!ctx) return -1;
    ctx->running.store(false);
    ASIOStop();
    ab->is_active = 0;
    return 0;
}

static void asio_destroy(AudioBackend *ab) {
    AsioContext *ctx = (AsioContext *)ab->ctx;
    if (!ctx) return;
    ctx->running.store(false);
    ASIOStop();
    ASIODisposeBuffers();
    ASIOExit();
    asioDrivers->removeCurrentDriver();
    delete[] ctx->bufferInfos;
    delete[] ctx->channelInfos;
    delete[] ctx->in_sample_types;
    delete[] ctx->out_sample_types;
    if (ctx->hwnd) DestroyWindow(ctx->hwnd);
    CoUninitialize();
    delete ctx;
    ab->ctx = NULL;
    g_asio_ctx = NULL;
}

static int asio_list_devices(AudioBackend *ab) {
    (void)ab;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    AsioDriverList driverList;
    int count = driverList.asioGetNumDev();
    if (count > 0) {
        printf("ASIO Devices (%d):\n", count);
        for (int i = 0; i < count; i++) {
            char name[128];
            if (driverList.asioGetDriverName(i, name, sizeof(name)) == 0)
                printf("  [%d] %s\n", i, name);
        }
    } else {
        printf("No ASIO devices found\n");
    }
    CoUninitialize();
    return 0;
}

static AudioBackendVtable asio_vtable = { asio_init, asio_start, asio_stop, asio_destroy, asio_list_devices };

extern "C" AudioBackend *asio_backend_create(void) {
    AudioBackend *ab = (AudioBackend *)calloc(1, sizeof(AudioBackend));
    if (!ab) return NULL;
    ab->vtable = &asio_vtable;
    ab->ctx = NULL;
    return ab;
}

#else

extern "C" AudioBackend *asio_backend_create(void) {
    fprintf(stderr, "ASIO support not compiled in\n");
    return NULL;
}

#endif
