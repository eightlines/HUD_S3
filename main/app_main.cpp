#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "framepack.h"
#include "hud_renderer.h"
#include "rm67162.h"
#include "usb_stream.h"
#include "wifi_stream.h"

static const char *TAG = "app_main";

// LILYGO T-Display S3 AMOLED v1.0 User Button
#define BTN_USER_GPIO 21

static volatile hud_mode_t s_mode = HUD_MODE_RETICLE;
static volatile bool s_live_dirty = false;
static uint16_t *s_live_fb = NULL;
static SemaphoreHandle_t s_fb_mux = NULL;

static void on_live_frame(const uint8_t *data, size_t len, bool is_rle16) {
    if (!s_live_fb || !s_fb_mux) return;
    if (xSemaphoreTake(s_fb_mux, pdMS_TO_TICKS(10)) == pdTRUE) {
        size_t max_pixels = (size_t)rm67162_width() * rm67162_height();
        if (is_rle16) {
            memset(s_live_fb, 0, max_pixels * sizeof(uint16_t));
            framepack_decode_rle16(data, len, s_live_fb, max_pixels);
        } else {
            size_t copy_bytes = len;
            if (copy_bytes > max_pixels * sizeof(uint16_t)) {
                copy_bytes = max_pixels * sizeof(uint16_t);
            }
            memcpy(s_live_fb, data, copy_bytes);
        }
        s_live_dirty = true;
        s_mode = HUD_MODE_LIVE_STREAM;
        xSemaphoreGive(s_fb_mux);
    }
}

static void on_mode_change(hud_mode_t mode) {
    if (mode == HUD_MODE_FRAMEPACK && !framepack_is_open()) {
        if (framepack_open("/spiffs/anim.hud") != ESP_OK) {
            ESP_LOGW(TAG, "No /spiffs/anim.hud found; staying in current mode");
            return;
        }
    }
    s_mode = mode;
}

static void on_settings_change(uint8_t brightness, bool mirror_x, bool mirror_y, bool landscape) {
    rm67162_set_brightness(brightness);
    rm67162_set_orientation(landscape, mirror_x, mirror_y);
    hud_renderer_resize(rm67162_width(), rm67162_height());
}

static void on_framepack_uploaded(void) {
    if (framepack_open("/spiffs/anim.hud") == ESP_OK) {
        s_mode = HUD_MODE_FRAMEPACK;
    }
}

static hud_mode_t get_mode(void) {
    return s_mode;
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Booting HUD S3 (LILYGO T-Display S3 AMOLED v1.0)...");

    rm67162_config_t disp_cfg = {
#ifdef CONFIG_HUD_LANDSCAPE
        .landscape = true,
#else
        .landscape = false,
#endif
#ifdef CONFIG_HUD_MIRROR_X
        .mirror_x = true,
#else
        .mirror_x = false,
#endif
#ifdef CONFIG_HUD_MIRROR_Y
        .mirror_y = true,
#else
        .mirror_y = false,
#endif
        .brightness = CONFIG_HUD_DEFAULT_BRIGHTNESS,
    };
    ESP_ERROR_CHECK(rm67162_init(&disp_cfg));
    ESP_ERROR_CHECK(hud_renderer_init(rm67162_width(), rm67162_height()));
    ESP_ERROR_CHECK(framepack_init_spiffs());

    s_fb_mux = xSemaphoreCreateMutex();
    s_live_fb = (uint16_t *)heap_caps_calloc(240 * 536, sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    // If an uploaded animation already exists on flash, play it on boot
    if (framepack_exists("/spiffs/anim.hud") && framepack_open("/spiffs/anim.hud") == ESP_OK) {
        s_mode = HUD_MODE_FRAMEPACK;
    }

    // Configure User button (GPIO 21) to cycle modes
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_USER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    hud_callbacks_t cbs = {
        .on_live_frame = on_live_frame,
        .on_mode_change = on_mode_change,
        .on_settings_change = on_settings_change,
        .on_framepack_uploaded = on_framepack_uploaded,
        .get_mode = get_mode,
    };
    wifi_stream_init(&cbs);
    usb_stream_init(&cbs);

    int prev_btn = 1;
    int64_t t_start = esp_timer_get_time();
    int64_t t_last = t_start;
    float fps = 30.0f;

    while (true) {
        int btn = gpio_get_level((gpio_num_t)BTN_USER_GPIO);
        if (prev_btn == 1 && btn == 0) {
            int next = ((int)s_mode + 1) % 3;
            if (next == HUD_MODE_FRAMEPACK && !framepack_is_open()) {
                next = HUD_MODE_RETICLE;
            }
            s_mode = (hud_mode_t)next;
            ESP_LOGI(TAG, "Button pressed -> mode %d", (int)s_mode);
            vTaskDelay(pdMS_TO_TICKS(180));  // debounce
        }
        prev_btn = btn;

        int64_t now = esp_timer_get_time();
        float dt = (float)(now - t_last) / 1000000.0f;
        if (dt > 0.001f) fps = 0.9f * fps + 0.1f * (1.0f / dt);
        t_last = now;
        float t_sec = (float)(now - t_start) / 1000000.0f;

        uint16_t w = rm67162_width();
        uint16_t h = rm67162_height();
        hud_mode_t mode = s_mode;

        if (mode == HUD_MODE_LIVE_STREAM) {
            if (s_live_dirty && xSemaphoreTake(s_fb_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(hud_renderer_fb(), s_live_fb, (size_t)w * h * sizeof(uint16_t));
                s_live_dirty = false;
                xSemaphoreGive(s_fb_mux);
                rm67162_push_framebuffer(hud_renderer_fb(), w, h);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else if (mode == HUD_MODE_FRAMEPACK && framepack_is_open()) {
            framepack_next_frame(hud_renderer_fb(), w, h);
            rm67162_push_framebuffer(hud_renderer_fb(), w, h);
            uint32_t delay_ms = 1000 / framepack_fps();
            vTaskDelay(pdMS_TO_TICKS(delay_ms > 5 ? delay_ms : 5));
        } else if (mode == HUD_MODE_WAVEFORM) {
            hud_render_demo_waveform(t_sec, fps);
            rm67162_push_framebuffer(hud_renderer_fb(), w, h);
            vTaskDelay(pdMS_TO_TICKS(16));
        } else {
            hud_render_demo_reticle(t_sec, fps);
            rm67162_push_framebuffer(hud_renderer_fb(), w, h);
            vTaskDelay(pdMS_TO_TICKS(16));
        }
    }
}
