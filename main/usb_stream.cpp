#include "usb_stream.h"

#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_stream";

static hud_callbacks_t s_cbs = {};

// Binary packet header over USB Serial/JTAG:
// magic[4] = "HUDU", cmd (1=live_frame, 2=save_start, 3=save_chunk, 4=save_end), flags (bit0=RLE16), len (uint32_t)
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t cmd;
    uint8_t flags;
    uint32_t len;
} usb_pkt_hdr_t;

#define USB_MAX_PAYLOAD (240 * 536 * 2 + 1024)

static int read_exact(uint8_t *dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = usb_serial_jtag_read_bytes(dst + got, len - got, pdMS_TO_TICKS(100));
        if (r > 0) got += (size_t)r;
        else return -1;
    }
    return (int)got;
}

static void usb_stream_task(void *arg) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(USB_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate USB PSRAM buffer");
        vTaskDelete(NULL);
        return;
    }

    FILE *upload_fp = NULL;
    const char magic[4] = {'H', 'U', 'D', 'U'};
    int match = 0;

    while (true) {
        uint8_t b = 0;
        int r = usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(50));
        if (r <= 0) continue;

        if (b == (uint8_t)magic[match]) {
            match++;
            if (match < 4) continue;
            match = 0;

            usb_pkt_hdr_t hdr = {};
            memcpy(hdr.magic, magic, 4);
            if (read_exact(((uint8_t *)&hdr) + 4, sizeof(hdr) - 4) < 0) continue;
            if (hdr.len > USB_MAX_PAYLOAD) continue;
            if (hdr.len > 0 && read_exact(buf, hdr.len) < 0) continue;

            if (hdr.cmd == 1 && s_cbs.on_live_frame) {
                s_cbs.on_live_frame(buf, hdr.len, (hdr.flags & 0x01) != 0);
            } else if (hdr.cmd == 2) {
                if (upload_fp) fclose(upload_fp);
                upload_fp = fopen("/spiffs/anim.hud", "wb");
            } else if (hdr.cmd == 3 && upload_fp && hdr.len > 0) {
                fwrite(buf, 1, hdr.len, upload_fp);
            } else if (hdr.cmd == 4) {
                if (upload_fp) {
                    fclose(upload_fp);
                    upload_fp = NULL;
                    if (s_cbs.on_framepack_uploaded) s_cbs.on_framepack_uploaded();
                }
            }
        } else {
            match = (b == (uint8_t)magic[0]) ? 1 : 0;
        }
    }
}

esp_err_t usb_stream_init(const hud_callbacks_t *cbs) {
    if (cbs) s_cbs = *cbs;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 8192,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB Serial/JTAG driver install skipped/failed: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreatePinnedToCore(usb_stream_task, "usb_stream", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "USB stream listener started");
    return ESP_OK;
}
