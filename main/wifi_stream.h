#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hud_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUD_UDP_PORT 5555

typedef struct {
    void (*on_live_frame)(const uint8_t *data, size_t len, bool is_rle16);
    void (*on_mode_change)(hud_mode_t mode);
    void (*on_settings_change)(uint8_t brightness, bool mirror_x, bool mirror_y, bool landscape);
    void (*on_framepack_uploaded)(void);
    hud_mode_t (*get_mode)(void);
} hud_callbacks_t;

esp_err_t wifi_stream_init(const hud_callbacks_t *cbs);

#ifdef __cplusplus
}
#endif
