#ifndef ULLLAS_JACK_BACKEND_H
#define ULLLAS_JACK_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "audio.h"

AudioBackend *jack_backend_create(void);

#ifdef __cplusplus
}
#endif

#endif
