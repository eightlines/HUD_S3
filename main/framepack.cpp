#include "framepack.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "framepack";

static FILE *s_fp = NULL;
static hud_file_header_t s_hdr = {};
static uint16_t s_cur_frame = 0;
static uint8_t *s_read_buf = NULL;
static size_t s_read_buf_cap = 0;

esp_err_t framepack_init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u / %u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

bool framepack_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size >= (off_t)sizeof(hud_file_header_t);
}

esp_err_t framepack_open(const char *path) {
    framepack_close();
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fread(&s_hdr, 1, sizeof(s_hdr), fp) != sizeof(s_hdr)) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(s_hdr.magic, HUD_MAGIC, 4) != 0 || s_hdr.frame_count == 0) {
        fclose(fp);
        return ESP_ERR_INVALID_ARG;
    }
    s_fp = fp;
    s_cur_frame = 0;
    ESP_LOGI(TAG, "Opened %s: %ux%u, %u frames @ %u fps (flags=0x%x)",
             path, s_hdr.width, s_hdr.height, s_hdr.frame_count, s_hdr.fps, s_hdr.flags);
    return ESP_OK;
}

void framepack_close(void) {
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_cur_frame = 0;
}

bool framepack_is_open(void) {
    return s_fp != NULL;
}

uint16_t framepack_fps(void) {
    return (s_hdr.fps > 0) ? s_hdr.fps : 30;
}

void framepack_decode_rle16(const uint8_t *src, size_t src_len, uint16_t *dst_fb, size_t max_pixels) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos + 2 <= src_len && out_pos < max_pixels) {
        uint16_t ctrl = (uint16_t)(src[in_pos] | (src[in_pos + 1] << 8));
        in_pos += 2;

        if (ctrl & 0x8000) {
            // Run of identical pixels
            uint16_t count = ctrl & 0x7FFF;
            if (in_pos + 2 > src_len) break;
            uint16_t color = (uint16_t)(src[in_pos] | (src[in_pos + 1] << 8));
            in_pos += 2;
            for (uint16_t i = 0; i < count && out_pos < max_pixels; ++i) {
                dst_fb[out_pos++] = color;
            }
        } else {
            // Literal pixels
            uint16_t count = ctrl & 0x7FFF;
            for (uint16_t i = 0; i < count && in_pos + 2 <= src_len && out_pos < max_pixels; ++i) {
                dst_fb[out_pos++] = (uint16_t)(src[in_pos] | (src[in_pos + 1] << 8));
                in_pos += 2;
            }
        }
    }
}

esp_err_t framepack_next_frame(uint16_t *dst_fb, uint16_t dst_w, uint16_t dst_h) {
    if (!s_fp) return ESP_ERR_INVALID_STATE;

    if (s_cur_frame >= s_hdr.frame_count) {
        fseek(s_fp, sizeof(hud_file_header_t), SEEK_SET);
        s_cur_frame = 0;
    }

    uint32_t payload_size = 0;
    if (fread(&payload_size, 1, sizeof(payload_size), s_fp) != sizeof(payload_size)) {
        fseek(s_fp, sizeof(hud_file_header_t), SEEK_SET);
        s_cur_frame = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    if (payload_size > s_read_buf_cap) {
        uint8_t *nb = (uint8_t *)heap_caps_realloc(s_read_buf, payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) return ESP_ERR_NO_MEM;
        s_read_buf = nb;
        s_read_buf_cap = payload_size;
    }

    if (fread(s_read_buf, 1, payload_size, s_fp) != payload_size) {
        fseek(s_fp, sizeof(hud_file_header_t), SEEK_SET);
        s_cur_frame = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    size_t max_pixels = (size_t)dst_w * dst_h;
    memset(dst_fb, 0, max_pixels * sizeof(uint16_t));

    if (s_hdr.flags & HUD_FLAG_RLE16) {
        framepack_decode_rle16(s_read_buf, payload_size, dst_fb, max_pixels);
    } else {
        size_t copy_bytes = payload_size;
        if (copy_bytes > max_pixels * sizeof(uint16_t)) {
            copy_bytes = max_pixels * sizeof(uint16_t);
        }
        memcpy(dst_fb, s_read_buf, copy_bytes);
    }

    s_cur_frame++;
    return ESP_OK;
}
