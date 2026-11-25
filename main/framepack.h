#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUD_MAGIC "HUD1"
#define HUD_FLAG_RLE16 0x01

typedef struct __attribute__((packed)) {
    char magic[4];       // "HUD1"
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t frame_count;
    uint16_t flags;      // bit 0: RLE16 compressed
    uint16_t reserved;
} hud_file_header_t;

esp_err_t framepack_init_spiffs(void);
bool framepack_exists(const char *path);

/**
 * Open a .hud file from SPIFFS for looped playback.
 */
esp_err_t framepack_open(const char *path);
void framepack_close(void);
bool framepack_is_open(void);
uint16_t framepack_fps(void);

/**
 * Decode the next frame into dst_fb (width * height RGB565 pixels).
 * Loops automatically at the end of the file.
 */
esp_err_t framepack_next_frame(uint16_t *dst_fb, uint16_t dst_w, uint16_t dst_h);

/**
 * Decode a single RLE16-compressed buffer into dst_fb.
 */
void framepack_decode_rle16(const uint8_t *src, size_t src_len, uint16_t *dst_fb, size_t max_pixels);

#ifdef __cplusplus
}
#endif
