#ifndef ULLLAS_ASIO_BACKEND_H
#define ULLLAS_ASIO_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "audio.h"

AudioBackend *asio_backend_create(void);

#ifdef __cplusplus
}
#endif

#endif
