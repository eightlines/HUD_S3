#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HUD_MODE_RETICLE = 0,
    HUD_MODE_WAVEFORM = 1,
    HUD_MODE_FRAMEPACK = 2,
    HUD_MODE_LIVE_STREAM = 3,
} hud_mode_t;

// RGB565 helper
static inline uint16_t hud_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

esp_err_t hud_renderer_init(uint16_t width, uint16_t height);
void hud_renderer_resize(uint16_t width, uint16_t height);

uint16_t *hud_renderer_fb(void);
uint16_t hud_renderer_width(void);
uint16_t hud_renderer_height(void);

void hud_clear_black(void);
void hud_draw_pixel(int x, int y, uint16_t color);
void hud_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void hud_draw_rect(int x, int y, int w, int h, uint16_t color);
void hud_fill_rect(int x, int y, int w, int h, uint16_t color);
void hud_draw_circle(int cx, int cy, int r, uint16_t color);
void hud_draw_arc(int cx, int cy, int r, float start_deg, float end_deg, uint16_t color);
void hud_draw_char(int x, int y, char c, uint16_t color, int scale);
void hud_draw_text(int x, int y, const char *text, uint16_t color, int scale);

void hud_render_demo_reticle(float t_sec, float fps);
void hud_render_demo_waveform(float t_sec, float fps);

#ifdef __cplusplus
}
#endif
