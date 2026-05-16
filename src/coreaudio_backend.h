#ifndef ULLLAS_COREAUDIO_BACKEND_H
#define ULLLAS_COREAUDIO_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "audio.h"

AudioBackend *coreaudio_backend_create(void);

#ifdef __cplusplus
}
#endif

#endif
