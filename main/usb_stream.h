#pragma once

#include "esp_err.h"
#include "wifi_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_stream_init(const hud_callbacks_t *cbs);

#ifdef __cplusplus
}
#endif
