#include "rm67162.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rm67162";

// Bounce buffer size in pixels (16 KB = 8192 pixels, fits easily in internal DMA SRAM)
#define DMA_CHUNK_PIXELS 8192

static spi_device_handle_t s_spi = NULL;
static uint16_t *s_dma_buf = NULL;
static uint16_t s_width = RM67162_NATIVE_WIDTH;
static uint16_t s_height = RM67162_NATIVE_HEIGHT;

typedef struct {
    uint8_t cmd;
    uint8_t data[4];
    uint8_t len;
    uint16_t delay_ms;
} rm67162_init_cmd_t;

// RM67162 QSPI initialization sequence for 240x536 1.91" AMOLED
static const rm67162_init_cmd_t s_init_cmds[] = {
    {0xFE, {0x00}, 1, 0},       // Page 0
    {0x11, {0x00}, 0, 120},     // Sleep Out
    {0x3A, {0x55}, 1, 0},       // COLMOD: 16-bit RGB565
    {0x51, {0x00}, 1, 0},       // Initial brightness 0 during setup
    {0x35, {0x00}, 1, 0},       // Tearing Effect Line ON (V-Blanking only)
    {0x53, {0x20}, 1, 0},       // WRCTRLD: Brightness control on, dimming off
    {0x29, {0x00}, 0, 20},      // Display ON
};

static esp_err_t send_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    spi_transaction_t t = {};
    t.cmd = 0x02;
    t.addr = ((uint32_t)cmd) << 8;
    t.length = len * 8;
    if (len > 0 && len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else if (len > 4) {
        t.tx_buffer = data;
    }
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t caset[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
    };
    uint8_t raset[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
    };
    esp_err_t err = send_cmd(0x2A, caset, 4);
    if (err != ESP_OK) return err;
    return send_cmd(0x2B, raset, 4);
}

esp_err_t rm67162_init(const rm67162_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;

    // Hardware reset pulse
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << RM67162_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level((gpio_num_t)RM67162_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)RM67162_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Allocate internal DMA bounce buffer
    s_dma_buf = (uint16_t *)heap_caps_malloc(DMA_CHUNK_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf) {
        ESP_LOGE(TAG, "Failed to allocate DMA bounce buffer");
        return ESP_ERR_NO_MEM;
    }

    // Initialize QSPI bus on SPI2_HOST
    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = RM67162_PIN_D0;
    buscfg.data1_io_num = RM67162_PIN_D1;
    buscfg.sclk_io_num  = RM67162_PIN_SCLK;
    buscfg.data2_io_num = RM67162_PIN_D2;
    buscfg.data3_io_num = RM67162_PIN_D3;
    buscfg.max_transfer_sz = DMA_CHUNK_PIXELS * sizeof(uint16_t) + 16;
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 8;
    devcfg.address_bits = 24;
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 40 * 1000 * 1000;  // 40 MHz QSPI
    devcfg.spics_io_num = RM67162_PIN_CS;
    devcfg.queue_size = 10;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); ++i) {
        ESP_ERROR_CHECK(send_cmd(s_init_cmds[i].cmd, s_init_cmds[i].data, s_init_cmds[i].len));
        if (s_init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_init_cmds[i].delay_ms));
        }
    }

    ESP_ERROR_CHECK(rm67162_set_orientation(cfg->landscape, cfg->mirror_x, cfg->mirror_y));
    ESP_ERROR_CHECK(rm67162_set_brightness(cfg->brightness));

    ESP_LOGI(TAG, "RM67162 initialized (%ux%u, mirror_x=%d, mirror_y=%d, brightness=%u)",
             s_width, s_height, cfg->mirror_x, cfg->mirror_y, cfg->brightness);
    return ESP_OK;
}

esp_err_t rm67162_set_orientation(bool landscape, bool mirror_x, bool mirror_y) {
    uint8_t madctl = 0x00;
    if (landscape) {
        madctl |= 0x20;  // MV: Row/Column Exchange
        s_width = RM67162_NATIVE_HEIGHT;
        s_height = RM67162_NATIVE_WIDTH;
    } else {
        s_width = RM67162_NATIVE_WIDTH;
        s_height = RM67162_NATIVE_HEIGHT;
    }
    if (mirror_x) madctl |= 0x40;  // MX
    if (mirror_y) madctl |= 0x80;  // MY

    return send_cmd(0x36, &madctl, 1);
}

esp_err_t rm67162_set_brightness(uint8_t brightness) {
    return send_cmd(0x51, &brightness, 1);
}

esp_err_t rm67162_set_sleep(bool sleep) {
    if (sleep) {
        esp_err_t err = send_cmd(0x28, NULL, 0);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(20));
        return send_cmd(0x10, NULL, 0);
    } else {
        esp_err_t err = send_cmd(0x11, NULL, 0);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(120));
        return send_cmd(0x29, NULL, 0);
    }
}

uint16_t rm67162_width(void) {
    return s_width;
}

uint16_t rm67162_height(void) {
    return s_height;
}

static esp_err_t push_pixels_internal(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, bool swap_bytes) {
    if (!s_spi || !s_dma_buf || !pixels || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = set_window(x, y, x + w - 1, y + h - 1);
    if (err != ESP_OK) return err;

    size_t total_pixels = (size_t)w * (size_t)h;
    size_t offset = 0;
    bool first = true;

    while (offset < total_pixels) {
        size_t chunk = total_pixels - offset;
        if (chunk > DMA_CHUNK_PIXELS) {
            chunk = DMA_CHUNK_PIXELS;
        }

        if (swap_bytes) {
            for (size_t i = 0; i < chunk; ++i) {
                uint16_t p = pixels[offset + i];
                s_dma_buf[i] = (uint16_t)((p >> 8) | (p << 8));
            }
        } else {
            memcpy(s_dma_buf, pixels + offset, chunk * sizeof(uint16_t));
        }

        spi_transaction_t t = {};
        t.flags = SPI_TRANS_MODE_QIO;
        t.cmd = 0x32;
        t.addr = first ? 0x002C00 : 0x003C00;  // RAMWR then RAMWRC
        t.length = chunk * 16;
        t.tx_buffer = s_dma_buf;

        err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;

        first = false;
        offset += chunk;
    }

    return ESP_OK;
}

esp_err_t rm67162_push_framebuffer(const uint16_t *fb, uint16_t w, uint16_t h) {
    return push_pixels_internal(0, 0, w, h, fb, true);
}

esp_err_t rm67162_push_window_be(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels_be) {
    return push_pixels_internal(x, y, w, h, pixels_be, false);
}
