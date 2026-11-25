#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// LILYGO T-Display S3 AMOLED v1.0 QSPI Pinout
#define RM67162_PIN_CS   6
#define RM67162_PIN_SCLK 47
#define RM67162_PIN_D0   18
#define RM67162_PIN_D1   7
#define RM67162_PIN_D2   48
#define RM67162_PIN_D3   5
#define RM67162_PIN_RST  17
#define RM67162_PIN_TE   9

#define RM67162_NATIVE_WIDTH  240
#define RM67162_NATIVE_HEIGHT 536

typedef struct {
    bool landscape;
    bool mirror_x;
    bool mirror_y;
    uint8_t brightness;
} rm67162_config_t;

esp_err_t rm67162_init(const rm67162_config_t *cfg);
esp_err_t rm67162_set_orientation(bool landscape, bool mirror_x, bool mirror_y);
esp_err_t rm67162_set_brightness(uint8_t brightness);
esp_err_t rm67162_set_sleep(bool sleep);

uint16_t rm67162_width(void);
uint16_t rm67162_height(void);

/**
 * Push a full RGB565 (little-endian uint16_t) framebuffer to the AMOLED panel.
 * Dimensions must match rm67162_width() x rm67162_height().
 */
esp_err_t rm67162_push_framebuffer(const uint16_t *fb, uint16_t w, uint16_t h);

/**
 * Push a pre-byte-swapped (big-endian) RGB565 window directly to the panel.
 */
esp_err_t rm67162_push_window_be(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels_be);

#ifdef __cplusplus
}
#endif
