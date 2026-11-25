#include "hud_renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "hud_renderer";

static uint16_t *s_fb = NULL;
static uint16_t s_w = 0;
static uint16_t s_h = 0;

// Minimal 5x7 uppercase/symbol bitmap font (ASCII 32..95)
static const uint8_t s_font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
};

esp_err_t hud_renderer_init(uint16_t width, uint16_t height) {
    s_w = width;
    s_h = height;
    size_t bytes = (size_t)240 * 536 * sizeof(uint16_t);
    s_fb = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM framebuffer (%u bytes)", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    hud_clear_black();
    ESP_LOGI(TAG, "PSRAM framebuffer allocated (%ux%u, %u bytes)", s_w, s_h, (unsigned)bytes);
    return ESP_OK;
}

void hud_renderer_resize(uint16_t width, uint16_t height) {
    s_w = width;
    s_h = height;
    hud_clear_black();
}

uint16_t *hud_renderer_fb(void) { return s_fb; }
uint16_t hud_renderer_width(void) { return s_w; }
uint16_t hud_renderer_height(void) { return s_h; }

void hud_clear_black(void) {
    if (s_fb) {
        memset(s_fb, 0, (size_t)s_w * s_h * sizeof(uint16_t));
    }
}

void hud_draw_pixel(int x, int y, uint16_t color) {
    if ((unsigned)x < s_w && (unsigned)y < s_h) {
        s_fb[y * s_w + x] = color;
    }
}

void hud_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        hud_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void hud_draw_rect(int x, int y, int w, int h, uint16_t color) {
    hud_draw_line(x, y, x + w - 1, y, color);
    hud_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    hud_draw_line(x, y, x, y + h - 1, color);
    hud_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void hud_fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int j = y; j < y + h; ++j) {
        if ((unsigned)j >= s_h) continue;
        for (int i = x; i < x + w; ++i) {
            if ((unsigned)i < s_w) {
                s_fb[j * s_w + i] = color;
            }
        }
    }
}

void hud_draw_circle(int cx, int cy, int r, uint16_t color) {
    int x = r, y = 0, err = 1 - x;
    while (x >= y) {
        hud_draw_pixel(cx + x, cy + y, color);
        hud_draw_pixel(cx + y, cy + x, color);
        hud_draw_pixel(cx - y, cy + x, color);
        hud_draw_pixel(cx - x, cy + y, color);
        hud_draw_pixel(cx - x, cy - y, color);
        hud_draw_pixel(cx - y, cy - x, color);
        hud_draw_pixel(cx + y, cy - x, color);
        hud_draw_pixel(cx + x, cy - y, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void hud_draw_arc(int cx, int cy, int r, float start_deg, float end_deg, uint16_t color) {
    const float step = 2.0f;
    float prev_x = cx + r * cosf(start_deg * (float)M_PI / 180.0f);
    float prev_y = cy + r * sinf(start_deg * (float)M_PI / 180.0f);
    for (float deg = start_deg + step; deg <= end_deg; deg += step) {
        float nx = cx + r * cosf(deg * (float)M_PI / 180.0f);
        float ny = cy + r * sinf(deg * (float)M_PI / 180.0f);
        hud_draw_line((int)roundf(prev_x), (int)roundf(prev_y), (int)roundf(nx), (int)roundf(ny), color);
        prev_x = nx;
        prev_y = ny;
    }
}

void hud_draw_char(int x, int y, char c, uint16_t color, int scale) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 32 || c > 95) c = ' ';
    const uint8_t *glyph = s_font5x7[c - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1 << row)) {
                hud_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void hud_draw_text(int x, int y, const char *text, uint16_t color, int scale) {
    int cx = x;
    while (*text) {
        hud_draw_char(cx, y, *text, color, scale);
        cx += 6 * scale;
        text++;
    }
}

void hud_render_demo_reticle(float t_sec, float fps) {
    hud_clear_black();

    const int cx = s_w / 2;
    const int cy = s_h / 2;
    const int min_dim = (s_w < s_h) ? s_w : s_h;

    uint16_t cyan   = hud_rgb565(0, 235, 255);
    uint16_t dim    = hud_rgb565(0, 110, 140);
    uint16_t amber  = hud_rgb565(255, 170, 0);
    uint16_t green  = hud_rgb565(40, 255, 120);

    // Outer rotating arcs
    float rot = fmodf(t_sec * 45.0f, 360.0f);
    int r_outer = min_dim / 2 - 18;
    hud_draw_arc(cx, cy, r_outer, rot, rot + 70.0f, cyan);
    hud_draw_arc(cx, cy, r_outer, rot + 180.0f, rot + 250.0f, cyan);

    // Counter-rotating inner ring
    float rot2 = fmodf(-t_sec * 80.0f, 360.0f);
    int r_mid = r_outer - 18;
    hud_draw_arc(cx, cy, r_mid, rot2, rot2 + 110.0f, amber);
    hud_draw_arc(cx, cy, r_mid, rot2 + 180.0f, rot2 + 290.0f, amber);

    // Pulsing center reticle
    int r_pulse = 14 + (int)(5.0f * sinf(t_sec * 3.5f));
    hud_draw_circle(cx, cy, r_pulse, green);
    hud_draw_line(cx - 24, cy, cx - 8, cy, cyan);
    hud_draw_line(cx + 8, cy, cx + 24, cy, cyan);
    hud_draw_line(cx, cy - 24, cx, cy - 8, cyan);
    hud_draw_line(cx, cy + 8, cx, cy + 24, cyan);

    // Simulated horizon tilt line
    float roll = 0.35f * sinf(t_sec * 1.1f);
    int hx = (int)(cosf(roll) * (r_mid - 10));
    int hy = (int)(sinf(roll) * (r_mid - 10));
    hud_draw_line(cx - hx, cy - hy, cx - hx / 3, cy - hy / 3, dim);
    hud_draw_line(cx + hx / 3, cy + hy / 3, cx + hx, cy + hy, dim);

    // Corner brackets
    int pad = 12;
    int blen = 18;
    hud_draw_line(pad, pad, pad + blen, pad, dim);
    hud_draw_line(pad, pad, pad, pad + blen, dim);
    hud_draw_line(s_w - pad, pad, s_w - pad - blen, pad, dim);
    hud_draw_line(s_w - pad, pad, s_w - pad, pad + blen, dim);
    hud_draw_line(pad, s_h - pad, pad + blen, s_h - pad, dim);
    hud_draw_line(pad, s_h - pad, pad, s_h - pad - blen, dim);
    hud_draw_line(s_w - pad, s_h - pad, s_w - pad - blen, s_h - pad, dim);
    hud_draw_line(s_w - pad, s_h - pad, s_w - pad, s_h - pad - blen, dim);

    // HUD telemetry text
    hud_draw_text(18, 22, "HUD S3 // ACTIVE", cyan, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "FPS:%4.1f  T:%5.1FS", fps, t_sec);
    hud_draw_text(18, s_h - 32, buf, green, 2);
}

void hud_render_demo_waveform(float t_sec, float fps) {
    hud_clear_black();

    const int cy = s_h / 2;
    uint16_t cyan  = hud_rgb565(0, 240, 255);
    uint16_t mag   = hud_rgb565(255, 60, 200);
    uint16_t green = hud_rgb565(60, 255, 130);

    // Centerline
    hud_draw_line(10, cy, s_w - 10, cy, hud_rgb565(0, 60, 80));

    // Dual phase sine waves
    int prev_y1 = cy, prev_y2 = cy;
    for (int x = 10; x < s_w - 10; x += 2) {
        float norm = (float)x / (float)s_w;
        float env = sinf(norm * (float)M_PI);
        int y1 = cy + (int)(env * 75.0f * sinf(norm * 14.0f - t_sec * 5.0f));
        int y2 = cy + (int)(env * 55.0f * cosf(norm * 9.0f + t_sec * 3.8f));
        if (x > 10) {
            hud_draw_line(x - 2, prev_y1, x, y1, cyan);
            hud_draw_line(x - 2, prev_y2, x, y2, mag);
        }
        prev_y1 = y1;
        prev_y2 = y2;
    }

    // Spectrum bars at bottom
    int bars = 16;
    int bw = (s_w - 32) / bars;
    for (int i = 0; i < bars; ++i) {
        float val = 0.5f + 0.5f * sinf(t_sec * 6.0f + i * 0.65f);
        int bh = 8 + (int)(val * 44.0f);
        hud_fill_rect(16 + i * bw, s_h - 56 - bh, bw - 3, bh, green);
    }

    hud_draw_text(18, 22, "WAVEFORM // HUD", cyan, 2);
    char buf[32];
    snprintf(buf, sizeof(buf), "FPS:%4.1f", fps);
    hud_draw_text(18, s_h - 32, buf, green, 2);
}
