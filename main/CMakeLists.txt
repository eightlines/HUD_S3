idf_component_register(
    SRCS
        "app_main.cpp"
        "rm67162.cpp"
        "hud_renderer.cpp"
        "framepack.cpp"
        "wifi_stream.cpp"
        "usb_stream.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        nvs_flash
        spiffs
        lwip
)

if(DEFINED ENV{HUD_WIFI_SSID} AND NOT "$ENV{HUD_WIFI_SSID}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        HUD_ENV_WIFI_SSID="$ENV{HUD_WIFI_SSID}"
        HUD_ENV_WIFI_PASSWORD="$ENV{HUD_WIFI_PASSWORD}"
    )
endif()
