#include "wifi_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_stream";

static hud_callbacks_t s_cbs = {};
static httpd_handle_t s_httpd = NULL;

// UDP chunk header: magic "HUDF" + frame_id + chunk_idx + total_chunks + flags
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t frame_id;
    uint16_t chunk_idx;
    uint16_t total_chunks;
    uint16_t flags;
} udp_chunk_hdr_t;

#define MAX_LIVE_FRAME_BYTES (240 * 536 * 2 + 1024)
#define MAX_UDP_CHUNKS 256

static const char *HTML_UI =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>HUD S3 Control</title>"
"<style>"
"body{background:#080a0c;color:#d8f3ff;font-family:system-ui,sans-serif;max-width:520px;margin:24px auto;padding:0 16px}"
"h1{color:#00e5ff;font-size:1.4rem;margin-bottom:4px}"
".card{background:#10151c;border:1px solid #1e2a38;border-radius:10px;padding:16px;margin:14px 0}"
"button{background:#00b8d4;color:#041014;border:none;padding:10px 14px;border-radius:6px;font-weight:600;cursor:pointer;margin:4px 4px 4px 0}"
"button.sec{background:#1b2836;color:#d8f3ff}"
"label{display:block;margin:10px 0 4px;font-size:.9rem;color:#9ab}"
"input[type=range]{width:100%}"
"#status{font-size:.85rem;color:#4eff9f;margin-top:8px}"
"</style></head><body>"
"<h1>HUD S3 // T-Display AMOLED</h1>"
"<div class='card'>"
"<label>Animation Mode</label>"
"<button onclick='setMode(0)'>Reticle</button>"
"<button onclick='setMode(1)'>Waveform</button>"
"<button onclick='setMode(2)'>Flash (.hud)</button>"
"<button class='sec' onclick='setMode(3)'>Live Stream</button>"
"</div>"
"<div class='card'>"
"<label>Brightness: <span id='bval'>220</span></label>"
"<input id='bright' type='range' min='10' max='255' value='220' oninput='bval.innerText=this.value' onchange='applyCfg()'>"
"<label><input id='mx' type='checkbox' checked onchange='applyCfg()'> Mirror X (Beamsplitter HUD)</label>"
"<label><input id='my' type='checkbox' onchange='applyCfg()'> Mirror Y</label>"
"<label><input id='ls' type='checkbox' onchange='applyCfg()'> Landscape (536x240)</label>"
"</div>"
"<div class='card'>"
"<label>Upload .hud Animation to Flash</label>"
"<input id='file' type='file' accept='.hud'><br><br>"
"<button onclick='uploadHud()'>Upload & Play</button>"
"<div id='status'>Ready</div>"
"</div>"
"<script>"
"function setMode(m){fetch('/api/settings?mode='+m,{method:'POST'}).then(()=>status.innerText='Mode '+m);}"
"function applyCfg(){"
"const b=bright.value,mx=mxEl.checked?1:0,my=myEl.checked?1:0,ls=lsEl.checked?1:0;"
"fetch(`/api/settings?brightness=${b}&mirror_x=${mx}&mirror_y=${my}&landscape=${ls}`,{method:'POST'})"
".then(()=>status.innerText='Settings updated');}"
"const mxEl=document.getElementById('mx'),myEl=document.getElementById('my'),lsEl=document.getElementById('ls'),status=document.getElementById('status');"
"async function uploadHud(){"
"const f=document.getElementById('file').files[0];if(!f){status.innerText='Pick a .hud file first';return;}"
"status.innerText='Uploading '+f.name+'...';"
"const r=await fetch('/upload',{method:'POST',body:f});"
"status.innerText=await r.text();}"
"</script></body></html>";

static esp_err_t handle_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_UI, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_settings(httpd_req_t *req) {
    char query[160] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {};
        if (httpd_query_key_value(query, "mode", val, sizeof(val)) == ESP_OK) {
            int m = atoi(val);
            if (s_cbs.on_mode_change) s_cbs.on_mode_change((hud_mode_t)m);
        }
        bool has_cfg = false;
        static uint8_t b = 220;
        static bool mx = true, my = false, ls = false;
        if (httpd_query_key_value(query, "brightness", val, sizeof(val)) == ESP_OK) { b = (uint8_t)atoi(val); has_cfg = true; }
        if (httpd_query_key_value(query, "mirror_x", val, sizeof(val)) == ESP_OK)   { mx = atoi(val) != 0; has_cfg = true; }
        if (httpd_query_key_value(query, "mirror_y", val, sizeof(val)) == ESP_OK)   { my = atoi(val) != 0; has_cfg = true; }
        if (httpd_query_key_value(query, "landscape", val, sizeof(val)) == ESP_OK)  { ls = atoi(val) != 0; has_cfg = true; }
        if (has_cfg && s_cbs.on_settings_change) {
            s_cbs.on_settings_change(b, mx, my, ls);
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t handle_upload(httpd_req_t *req) {
    FILE *fp = fopen("/spiffs/anim.hud", "wb");
    if (!fp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(4096);
    if (!buf) {
        fclose(fp);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            fclose(fp);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        fwrite(buf, 1, r, fp);
        remaining -= r;
    }

    free(buf);
    fclose(fp);
    ESP_LOGI(TAG, "Uploaded /spiffs/anim.hud (%d bytes)", req->content_len);

    if (s_cbs.on_framepack_uploaded) {
        s_cbs.on_framepack_uploaded();
    }
    httpd_resp_sendstr(req, "Uploaded and playing!");
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t idx = { .uri = "/", .method = HTTP_GET, .handler = handle_index, .user_ctx = NULL };
    httpd_uri_t cfg = { .uri = "/api/settings", .method = HTTP_POST, .handler = handle_settings, .user_ctx = NULL };
    httpd_uri_t upl = { .uri = "/upload", .method = HTTP_POST, .handler = handle_upload, .user_ctx = NULL };
    httpd_register_uri_handler(s_httpd, &idx);
    httpd_register_uri_handler(s_httpd, &cfg);
    httpd_register_uri_handler(s_httpd, &upl);
    ESP_LOGI(TAG, "HTTP server started on port 80");
}

static void udp_stream_task(void *arg) {
    uint8_t *frame_buf = (uint8_t *)heap_caps_malloc(MAX_LIVE_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pkt_buf = (uint8_t *)malloc(1500);
    if (!frame_buf || !pkt_buf) {
        ESP_LOGE(TAG, "Failed to allocate UDP buffers");
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HUD_UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    ESP_LOGI(TAG, "UDP live stream listening on port %d", HUD_UDP_PORT);

    uint16_t cur_frame_id = 0xFFFF;
    uint16_t chunks_seen = 0;
    size_t max_end = 0;
    uint16_t cur_flags = 0;
    bool chunk_map[MAX_UDP_CHUNKS] = {};

    while (true) {
        int len = recvfrom(sock, pkt_buf, 1500, 0, NULL, NULL);
        if (len < (int)sizeof(udp_chunk_hdr_t)) continue;

        udp_chunk_hdr_t *hdr = (udp_chunk_hdr_t *)pkt_buf;
        if (memcmp(hdr->magic, "HUDF", 4) != 0) continue;
        if (hdr->total_chunks == 0 || hdr->total_chunks > MAX_UDP_CHUNKS || hdr->chunk_idx >= hdr->total_chunks) continue;

        if (hdr->frame_id != cur_frame_id) {
            cur_frame_id = hdr->frame_id;
            chunks_seen = 0;
            max_end = 0;
            cur_flags = hdr->flags;
            memset(chunk_map, 0, sizeof(bool) * hdr->total_chunks);
        }

        size_t payload_len = (size_t)len - sizeof(udp_chunk_hdr_t);
        size_t offset = (size_t)hdr->chunk_idx * 1400;
        if (offset + payload_len <= MAX_LIVE_FRAME_BYTES) {
            memcpy(frame_buf + offset, pkt_buf + sizeof(udp_chunk_hdr_t), payload_len);
            if (!chunk_map[hdr->chunk_idx]) {
                chunk_map[hdr->chunk_idx] = true;
                chunks_seen++;
            }
            if (offset + payload_len > max_end) {
                max_end = offset + payload_len;
            }
        }

        if (chunks_seen == hdr->total_chunks) {
            if (s_cbs.on_live_frame) {
                s_cbs.on_live_frame(frame_buf, max_end, (cur_flags & 0x01) != 0);
            }
            cur_frame_id = 0xFFFF;
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

esp_err_t wifi_stream_init(const hud_callbacks_t *cbs) {
    if (cbs) s_cbs = *cbs;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

#ifdef HUD_ENV_WIFI_SSID
    const char *ssid = HUD_ENV_WIFI_SSID;
#ifdef HUD_ENV_WIFI_PASSWORD
    const char *pass = HUD_ENV_WIFI_PASSWORD;
#else
    const char *pass = "";
#endif
#else
    const char *ssid = CONFIG_HUD_WIFI_SSID;
    const char *pass = CONFIG_HUD_WIFI_PASSWORD;
#endif

    if (strlen(ssid) > 0) {
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        wifi_config_t wifi_cfg = {};
        strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "Wi-Fi STA connecting to '%s'...", ssid);
    } else {
        esp_netif_create_default_wifi_ap();
        wifi_config_t ap_cfg = {};
        strcpy((char *)ap_cfg.ap.ssid, "HUD-S3");
        ap_cfg.ap.ssid_len = 6;
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        if (strlen(pass) >= 8) {
            strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "Wi-Fi SoftAP 'HUD-S3' started at 192.168.4.1");
    }

    start_webserver();
    xTaskCreatePinnedToCore(udp_stream_task, "udp_stream", 4096, NULL, 5, NULL, 0);
    return ESP_OK;
}
