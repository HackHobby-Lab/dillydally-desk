idf_component_register(
    SRCS "main.c" "board_display.c" "dd_app.c" "dd_media.c" "dd_platform_esp.c"
         "dd_net.c" "dd_portal.c" "dd_audio.c" "dd_rpc.c" "dd_bt.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd
        esp_wifi esp_netif esp_event esp_http_server esp_http_client mbedtls lwip bt esp_ringbuf
        espressif__esp_audio_simple_player)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")
