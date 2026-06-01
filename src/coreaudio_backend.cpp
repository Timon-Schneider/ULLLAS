#include "coreaudio_backend.h"

#ifdef HAS_COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <new>

/*
 * CoreAudio's render callback is real-time: no allocations, no syscalls.
 * v1 called calloc/free/resize the first time each buffer size was seen,
 * which caused a one-off (or recurring on device reconfig) audio glitch.
 * v2 sizes all scratch up-front to a comfortable max (4x the configured
 * buffer size, capped at 4096 frames). The render callbacks clamp
 * incoming nframes to that cap so they never need to allocate.
 */
struct CoreAudioContext {
    AudioUnit        unit;
    audio_callback_t user_callback;
    void            *user_data;
    int              num_in_ch;
    int              num_out_ch;
    int              buffer_size;
    int              max_scratch_frames;
    unsigned int     sample_rate;
    bool             active;
    AudioDeviceID    device_id;

    AudioBufferList *input_buf_list;
    float           *input_buf_data;

    int32_t  *int32_scratch;
    int32_t **int32_ptr_scratch;
};

static bool alloc_all_scratch(CoreAudioContext *ctx, int frames_max) {
    int ch = std::max(ctx->num_in_ch, ctx->num_out_ch);
    if (ch <= 0) ch = 1;
    ctx->max_scratch_frames = frames_max;

    size_t total = (size_t)ch * (size_t)frames_max;
    ctx->int32_scratch     = (int32_t *)calloc(total, sizeof(int32_t));
    ctx->int32_ptr_scratch = (int32_t **)calloc((size_t)ch, sizeof(int32_t *));
    if (!ctx->int32_scratch || !ctx->int32_ptr_scratch) return false;
    for (int i = 0; i < ch; i++) {
        ctx->int32_ptr_scratch[i] = ctx->int32_scratch + (size_t)i * (size_t)frames_max;
    }

    if (ctx->num_in_ch > 0) {
        size_t list_sz      = offsetof(AudioBufferList, mBuffers[ctx->num_in_ch]);
        ctx->input_buf_list = (AudioBufferList *)calloc(1, list_sz);
        if (!ctx->input_buf_list) return false;

        ctx->input_buf_data = (float *)calloc((size_t)ctx->num_in_ch * (size_t)frames_max, sizeof(float));
        if (!ctx->input_buf_data) return false;

        ctx->input_buf_list->mNumberBuffers = ctx->num_in_ch;
        for (int i = 0; i < ctx->num_in_ch; i++) {
            ctx->input_buf_list->mBuffers[i].mNumberChannels = 1;
            ctx->input_buf_list->mBuffers[i].mDataByteSize   = (UInt32)(frames_max * (int)sizeof(float));
            ctx->input_buf_list->mBuffers[i].mData           = ctx->input_buf_data + (size_t)i * (size_t)frames_max;
        }
    }
    return true;
}

static inline UInt32 clamp_frames(CoreAudioContext *ctx, UInt32 nframes) {
    if ((int)nframes > ctx->max_scratch_frames) {
        return (UInt32)ctx->max_scratch_frames;
    }
    return nframes;
}

static OSStatus input_callback_proc(void *inRefCon,
                                     AudioUnitRenderActionFlags *ioActionFlags,
                                     const AudioTimeStamp *inTimeStamp,
                                     UInt32 inBusNumber,
                                     UInt32 inNumberFrames,
                                     AudioBufferList *ioData) {
    CoreAudioContext *ctx = (CoreAudioContext *)inRefCon;
    (void)ioData;
    (void)inBusNumber;

    inNumberFrames = clamp_frames(ctx, inNumberFrames);

    /* Reset each input AudioBuffer's mDataByteSize because AudioUnitRender
     * adjusts it down to the bytes actually delivered and then back. */
    for (int i = 0; i < ctx->num_in_ch; i++) {
        ctx->input_buf_list->mBuffers[i].mDataByteSize = inNumberFrames * (UInt32)sizeof(float);
    }

    OSStatus err = AudioUnitRender(ctx->unit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames,
                                   ctx->input_buf_list);
    if (err != noErr) {
        return noErr;
    }

    for (int ch = 0; ch < ctx->num_in_ch; ch++) {
        float   *src = (float *)ctx->input_buf_list->mBuffers[ch].mData;
        int32_t *dst = ctx->int32_ptr_scratch[ch];
        for (UInt32 i = 0; i < inNumberFrames; i++) {
            float f = src[i];
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            dst[i] = (int32_t)(f * 2147483647.0f);
        }
    }

    ctx->user_callback((const int32_t * const *)ctx->int32_ptr_scratch, NULL, (int)inNumberFrames, ctx->user_data);

    return noErr;
}

static OSStatus render_callback_proc(void *inRefCon,
                                       AudioUnitRenderActionFlags *ioActionFlags,
                                       const AudioTimeStamp *inTimeStamp,
                                       UInt32 inBusNumber,
                                       UInt32 inNumberFrames,
                                       AudioBufferList *ioData) {
    CoreAudioContext *ctx = (CoreAudioContext *)inRefCon;
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;

    if (ctx->num_out_ch == 0) {
        for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
            memset(ioData->mBuffers[i].mData, 0, ioData->mBuffers[i].mDataByteSize);
        }
        return noErr;
    }

    inNumberFrames = clamp_frames(ctx, inNumberFrames);

    ctx->user_callback(NULL, ctx->int32_ptr_scratch, (int)inNumberFrames, ctx->user_data);

    for (int ch = 0; ch < ctx->num_out_ch; ch++) {
        int32_t *src = ctx->int32_ptr_scratch[ch];
        float   *dst = (float *)ioData->mBuffers[ch].mData;
        for (UInt32 i = 0; i < inNumberFrames; i++) {
            dst[i] = (float)src[i] / 2147483648.0f;
        }
    }

    return noErr;
}

static AudioDeviceID find_device_by_name(const char *name, bool prefer_input) {
    if (!name || !name[0]) return 0;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &size);
    if (err != noErr) return 0;

    int num_devices = (int)(size / sizeof(AudioDeviceID));
    if (num_devices <= 0) return 0;

    AudioDeviceID *devices = (AudioDeviceID *)malloc(size);
    if (!devices) return 0;

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &size, devices);
    if (err != noErr) {
        free(devices);
        return 0;
    }

    AudioDeviceID found = 0;

    for (int i = 0; i < num_devices; i++) {
        AudioObjectPropertyAddress name_prop = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        CFStringRef cfname = NULL;
        UInt32 name_size = sizeof(cfname);
        err = AudioObjectGetPropertyData(devices[i], &name_prop, 0, NULL, &name_size, &cfname);
        if (err != noErr || !cfname) continue;

        char dev_name[256];
        if (!CFStringGetCString(cfname, dev_name, sizeof(dev_name), kCFStringEncodingUTF8)) {
            CFRelease(cfname);
            continue;
        }
        CFRelease(cfname);

        if (strcasestr(dev_name, name) != NULL) {
            if (prefer_input) {
                AudioObjectPropertyAddress input_prop = {
                    kAudioDevicePropertyStreamConfiguration,
                    kAudioDevicePropertyScopeInput,
                    kAudioObjectPropertyElementMain
                };
                UInt32 cfg_size = 0;
                err = AudioObjectGetPropertyDataSize(devices[i], &input_prop, 0, NULL, &cfg_size);
                if (err == noErr && cfg_size > 0) {
                    AudioBufferList *cfg = (AudioBufferList *)malloc(cfg_size);
                    err = AudioObjectGetPropertyData(devices[i], &input_prop, 0, NULL, &cfg_size, cfg);
                    if (err == noErr && cfg->mNumberBuffers > 0) {
                        found = devices[i];
                        free(cfg);
                        break;
                    }
                    free(cfg);
                }
            } else {
                AudioObjectPropertyAddress output_prop = {
                    kAudioDevicePropertyStreamConfiguration,
                    kAudioDevicePropertyScopeOutput,
                    kAudioObjectPropertyElementMain
                };
                UInt32 cfg_size = 0;
                err = AudioObjectGetPropertyDataSize(devices[i], &output_prop, 0, NULL, &cfg_size);
                if (err == noErr && cfg_size > 0) {
                    AudioBufferList *cfg = (AudioBufferList *)malloc(cfg_size);
                    err = AudioObjectGetPropertyData(devices[i], &output_prop, 0, NULL, &cfg_size, cfg);
                    if (err == noErr && cfg->mNumberBuffers > 0) {
                        found = devices[i];
                        free(cfg);
                        break;
                    }
                    free(cfg);
                }
            }
        }
    }

    free(devices);
    return found;
}

static int coreaudio_init(AudioBackend *ab, const char *device_name,
                           unsigned int sample_rate, int num_in_ch, int num_out_ch,
                           int buffer_size, audio_callback_t cb, void *userdata) {
    CoreAudioContext *ctx = new (std::nothrow) CoreAudioContext();
    if (!ctx) return -1;
    ab->ctx = ctx;

    ctx->num_in_ch          = num_in_ch;
    ctx->num_out_ch         = num_out_ch;
    ctx->user_callback      = cb;
    ctx->user_data          = userdata;
    ctx->active             = false;
    ctx->device_id          = 0;
    ctx->unit               = NULL;
    ctx->sample_rate        = sample_rate;
    ctx->buffer_size        = (int)buffer_size;
    ctx->max_scratch_frames = 0;
    ctx->input_buf_list     = NULL;
    ctx->input_buf_data     = NULL;
    ctx->int32_scratch      = NULL;
    ctx->int32_ptr_scratch  = NULL;

    AudioComponentDescription desc;
    memset(&desc, 0, sizeof(desc));
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "CoreAudio: Failed to find HALOutput component\n");
        delete ctx; ab->ctx = NULL;
        return -1;
    }

    OSStatus err = AudioComponentInstanceNew(comp, &ctx->unit);
    if (err != noErr) {
        fprintf(stderr, "CoreAudio: Failed to create AudioUnit: %d\n", (int)err);
        delete ctx; ab->ctx = NULL;
        return -1;
    }

    bool is_input  = (num_in_ch > 0);
    bool is_output = (num_out_ch > 0);

    // Set the device BEFORE enabling IO.
    // CRITICAL: For input-only, disable output bus 0 FIRST, then set device.
    if (is_input && !is_output) {
        UInt32 disable_out = 0;
        AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_EnableIO,
                              kAudioUnitScope_Output, 0,
                              &disable_out, sizeof(disable_out));
    }

    if (is_input) {
        AudioDeviceID input_dev = 0;
        if (device_name && device_name[0]) {
            input_dev = find_device_by_name(device_name, true);
        }
        if (input_dev == 0) {
            AudioObjectPropertyAddress def_prop = {
                kAudioHardwarePropertyDefaultInputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = sizeof(input_dev);
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &def_prop, 0, NULL, &sz, &input_dev);
        }
        if (input_dev != 0) {
            ctx->device_id = input_dev;
            err = AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_CurrentDevice,
                                        kAudioUnitScope_Global, 0,
                                        &input_dev, sizeof(input_dev));
            if (err != noErr) {
                fprintf(stderr, "CoreAudio: Failed to set input device %d: %d\n", (int)input_dev, (int)err);
                ctx->device_id = 0;
            }
        }
    } else if (device_name && device_name[0]) {
        ctx->device_id = find_device_by_name(device_name, false);
        if (ctx->device_id != 0) {
            AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, 0,
                                  &ctx->device_id, sizeof(ctx->device_id));
        } else {
            fprintf(stderr, "CoreAudio: Device '%s' not found, using default\n", device_name);
        }
    }

    // Enable output bus 0 if output is needed
    if (is_output) {
        UInt32 enable_out = 1;
        err = AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Output, 0,
                                    &enable_out, sizeof(enable_out));
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: Failed to enable output bus: %d\n", (int)err);
            AudioComponentInstanceDispose(ctx->unit);
            delete ctx; ab->ctx = NULL;
            return -1;
        }

        AudioStreamBasicDescription out_fmt;
        memset(&out_fmt, 0, sizeof(out_fmt));
        out_fmt.mSampleRate       = (double)sample_rate;
        out_fmt.mFormatID         = kAudioFormatLinearPCM;
        out_fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved;
        out_fmt.mBitsPerChannel   = 32;
        out_fmt.mBytesPerPacket   = 4;
        out_fmt.mFramesPerPacket  = 1;
        out_fmt.mBytesPerFrame    = 4;
        out_fmt.mChannelsPerFrame = num_out_ch;

        err = AudioUnitSetProperty(ctx->unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Input, 0, &out_fmt, sizeof(out_fmt));
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: using device default output format (err %d)\n", (int)err);
        }
    }
    int out_ch = is_output ? num_out_ch : (is_input ? num_in_ch : 2);

    if (is_input) {
        UInt32 enable = 1;
        err = AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Input, 1, &enable, sizeof(enable));
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: Failed to enable input bus: %d\n", (int)err);
            AudioComponentInstanceDispose(ctx->unit);
            delete ctx; ab->ctx = NULL;
            return -1;
        }

        AudioStreamBasicDescription fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.mSampleRate       = (double)sample_rate;
        fmt.mFormatID         = kAudioFormatLinearPCM;
        fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved;
        fmt.mBitsPerChannel   = 32;
        fmt.mBytesPerPacket   = 4;
        fmt.mFramesPerPacket  = 1;
        fmt.mBytesPerFrame    = 4;
        fmt.mChannelsPerFrame = num_in_ch;

        err = AudioUnitSetProperty(ctx->unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
        if (err != noErr) {
            static int warned = 0;
            if (!warned) { fprintf(stderr, "CoreAudio: using device default input format\n"); warned = 1; }
        }

        AURenderCallbackStruct input_cb;
        input_cb.inputProc       = input_callback_proc;
        input_cb.inputProcRefCon = ctx;
        err = AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_SetInputCallback,
                                    kAudioUnitScope_Global, 1, &input_cb, sizeof(input_cb));
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: Failed to set input callback: %d\n", (int)err);
            AudioComponentInstanceDispose(ctx->unit);
            delete ctx; ab->ctx = NULL;
            return -1;
        }
    } else {
        UInt32 enable = 0;
        err = AudioUnitSetProperty(ctx->unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Input, 1, &enable, sizeof(enable));
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: Failed to disable input bus: %d\n", (int)err);
        }
    }

    AURenderCallbackStruct render_cb;
    render_cb.inputProc       = render_callback_proc;
    render_cb.inputProcRefCon = ctx;
    err = AudioUnitSetProperty(ctx->unit, kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Global, 0, &render_cb, sizeof(render_cb));
    if (err != noErr) {
        fprintf(stderr, "CoreAudio: Failed to set render callback: %d\n", (int)err);
        AudioComponentInstanceDispose(ctx->unit);
        delete ctx; ab->ctx = NULL;
        return -1;
    }

    if (sample_rate > 0) {
        double sr = (double)sample_rate;
        if (is_output) {
            err = AudioUnitSetProperty(ctx->unit, kAudioUnitProperty_SampleRate,
                                        kAudioUnitScope_Input, 0, &sr, sizeof(sr));
        } else {
            err = AudioUnitSetProperty(ctx->unit, kAudioUnitProperty_SampleRate,
                                        kAudioUnitScope_Output, 1, &sr, sizeof(sr));
        }
        if (err != noErr) {
            fprintf(stderr, "CoreAudio: Failed to set sample rate: %d\n", (int)err);
        }
    }

    {
        if (ctx->device_id == 0) {
            UInt32 sz = sizeof(ctx->device_id);
            AudioUnitGetProperty(ctx->unit, kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, 0,
                                  &ctx->device_id, &sz);
        }

        AudioObjectPropertyScope scope = is_output ? kAudioDevicePropertyScopeOutput
                                                    : kAudioDevicePropertyScopeInput;

        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyBufferFrameSizeRange,
            scope,
            kAudioObjectPropertyElementMain
        };
        AudioValueRange range;
        UInt32 sz = sizeof(range);
        if (AudioObjectGetPropertyData(ctx->device_id, &prop, 0, NULL, &sz, &range) == noErr) {
            UInt32 requested = (UInt32)buffer_size;
            if (requested < (UInt32)range.mMinimum) requested = (UInt32)range.mMinimum;
            if (requested > (UInt32)range.mMaximum) requested = (UInt32)range.mMaximum;

            prop.mSelector = kAudioDevicePropertyBufferFrameSize;
            OSStatus set_err = AudioObjectSetPropertyData(ctx->device_id, &prop, 0, NULL,
                                                           sizeof(requested), &requested);
            if (set_err != noErr) {
                fprintf(stderr, "CoreAudio: cannot set buffer size on device (err %d)\n", (int)set_err);
            }
        }

        UInt32 actual = 0;
        sz = sizeof(actual);
        prop.mSelector = kAudioDevicePropertyBufferFrameSize;
        prop.mScope    = scope;
        prop.mElement  = kAudioObjectPropertyElementMain;
        if (AudioObjectGetPropertyData(ctx->device_id, &prop, 0, NULL, &sz, &actual) == noErr) {
            ctx->buffer_size = (int)actual;
        }
    }

    err = AudioUnitInitialize(ctx->unit);
    if (err != noErr) {
        fprintf(stderr, "CoreAudio: Failed to initialize AudioUnit: %d\n", (int)err);
        AudioComponentInstanceDispose(ctx->unit);
        delete ctx; ab->ctx = NULL;
        return -1;
    }

    ctx->sample_rate = sample_rate;

    /* Size all RT scratch up-front with 4x headroom so the render
     * callbacks never need to allocate. Cap at 4096 frames which is
     * the largest CoreAudio buffer we ever practically see. */
    int frames_max = ctx->buffer_size * 4;
    if (frames_max < 256) frames_max = 256;
    if (frames_max > 4096) frames_max = 4096;
    if (!alloc_all_scratch(ctx, frames_max)) {
        fprintf(stderr, "CoreAudio: failed to allocate scratch buffers\n");
        AudioComponentInstanceDispose(ctx->unit);
        delete ctx;
        ab->ctx = NULL;
        return -1;
    }

    ab->num_input_channels  = num_in_ch;
    ab->num_output_channels = out_ch;
    ab->buffer_size         = ctx->buffer_size;
    ab->sample_rate         = ctx->sample_rate;

    return 0;
}

static int coreaudio_start(AudioBackend *ab) {
    CoreAudioContext *ctx = (CoreAudioContext *)ab->ctx;
    if (!ctx) return -1;

    OSStatus err = AudioOutputUnitStart(ctx->unit);
    if (err != noErr) {
        fprintf(stderr, "CoreAudio: Failed to start AudioUnit: %d\n", (int)err);
        return -1;
    }
    ctx->active = true;
    ab->is_active = 1;
    return 0;
}

static int coreaudio_stop(AudioBackend *ab) {
    CoreAudioContext *ctx = (CoreAudioContext *)ab->ctx;
    if (!ctx) return -1;

    AudioOutputUnitStop(ctx->unit);
    ctx->active = false;
    ab->is_active = 0;
    return 0;
}

static void coreaudio_destroy(AudioBackend *ab) {
    CoreAudioContext *ctx = (CoreAudioContext *)ab->ctx;
    if (!ctx) return;

    if (ctx->active) {
        AudioOutputUnitStop(ctx->unit);
        ctx->active = false;
    }
    AudioUnitUninitialize(ctx->unit);
    AudioComponentInstanceDispose(ctx->unit);

    free(ctx->input_buf_data);
    free(ctx->input_buf_list);
    free(ctx->int32_scratch);
    free(ctx->int32_ptr_scratch);

    delete ctx;
    ab->ctx = NULL;
}

static int count_channels_for_scope(AudioDeviceID device_id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress prop = {
        kAudioDevicePropertyStreamConfiguration,
        scope,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(device_id, &prop, 0, NULL, &size);
    if (err != noErr) return 0;

    AudioBufferList *cfg = (AudioBufferList *)malloc(size);
    if (!cfg) return 0;

    err = AudioObjectGetPropertyData(device_id, &prop, 0, NULL, &size, cfg);
    if (err != noErr) {
        free(cfg);
        return 0;
    }

    int channels = 0;
    for (UInt32 i = 0; i < cfg->mNumberBuffers; i++) {
        channels += (int)cfg->mBuffers[i].mNumberChannels;
    }

    free(cfg);
    return channels;
}

static int coreaudio_list_devices(AudioBackend *ab) {
    (void)ab;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, NULL, &size);
    if (err != noErr) {
        fprintf(stderr, "CoreAudio: Failed to get device list size: %d\n", (int)err);
        return -1;
    }

    int num_devices = (int)(size / sizeof(AudioDeviceID));
    AudioDeviceID *devices = (AudioDeviceID *)malloc(size);
    if (!devices) return -1;

    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, NULL, &size, devices);
    if (err != noErr) {
        free(devices);
        return -1;
    }

    AudioObjectPropertyAddress default_in_prop = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID default_in = 0;
    UInt32 def_size = sizeof(default_in);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_in_prop, 0, NULL, &def_size, &default_in);

    AudioObjectPropertyAddress default_out_prop = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID default_out = 0;
    def_size = sizeof(default_out);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_out_prop, 0, NULL, &def_size, &default_out);

    printf("CoreAudio devices:\n");
    printf("%-6s  %-8s  %-8s  %s\n", "ID", "IN_CH", "OUT_CH", "NAME");
    printf("------  --------  --------  ----\n");

    for (int i = 0; i < num_devices; i++) {
        AudioObjectPropertyAddress name_prop = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef cfname = NULL;
        UInt32 name_size = sizeof(cfname);
        err = AudioObjectGetPropertyData(devices[i], &name_prop, 0, NULL, &name_size, &cfname);
        if (err != noErr || !cfname) continue;

        char dev_name[256];
        if (!CFStringGetCString(cfname, dev_name, sizeof(dev_name), kCFStringEncodingUTF8)) {
            CFRelease(cfname);
            continue;
        }
        CFRelease(cfname);

        int in_ch  = count_channels_for_scope(devices[i], kAudioDevicePropertyScopeInput);
        int out_ch = count_channels_for_scope(devices[i], kAudioDevicePropertyScopeOutput);

        const char *marker = "";
        if (devices[i] == default_in)  marker = " [default in]";
        if (devices[i] == default_out) marker = " [default out]";

        printf("%-6d  %-8d  %-8d  %s%s\n", (int)devices[i], in_ch, out_ch, dev_name, marker);
    }

    free(devices);
    return 0;
}

static AudioBackendVtable coreaudio_vtable = {
    coreaudio_init,
    coreaudio_start,
    coreaudio_stop,
    coreaudio_destroy,
    coreaudio_list_devices
};

extern "C" AudioBackend *coreaudio_backend_create(void) {
    AudioBackend *ab = (AudioBackend *)calloc(1, sizeof(AudioBackend));
    if (!ab) return NULL;
    ab->vtable = &coreaudio_vtable;
    ab->ctx    = NULL;
    return ab;
}

#else

extern "C" AudioBackend *coreaudio_backend_create(void) {
    fprintf(stderr, "CoreAudio support not compiled in\n");
    return NULL;
}

#endif
