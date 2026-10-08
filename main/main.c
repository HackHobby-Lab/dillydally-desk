// SPDX-License-Identifier: Apache-2.0
//
// Dilly-dally. A desk companion with a face, a clock, a focus timer and a
// radio, on ESP-Mosaico.
//
// Start order matters only in one way: nothing optional may stop the rest.
// Wifi, the speaker and RPC each log and carry on when they fail, so a board
// with no network still keeps time once it has had it, and a board with no
// speaker still shows its face.

#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "dd_app.h"
#include "dd_audio.h"
#include "dd_bt.h"
#include "dd_net.h"
#include "dd_platform.h"
#include "dd_rpc.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "dillydally";
static esp_gsp_handle_t s_ui;

void dd_platform_audio_ready(bool ready);

/* Bluetooth starts on its own task once the UI is up. On the first build it
 * ran inline and never returned, which left a board with no screen at all. A
 * stuck Bluetooth stack must cost Bluetooth, not the clock. */
static void bt_start_task(void *arg)
{
    (void)arg;
    const esp_err_t err = dd_bt_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth unavailable (0x%x), speaker only", err);
    }
    vTaskDelete(NULL);
}

static esp_err_t rpc_goto(unsigned page)
{
    return s_ui ? dd_app_goto(s_ui, page) : ESP_ERR_INVALID_STATE;
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Keep Recovery reachable even when the external UI image is absent or
     * invalid. */
    iris_ota_support_start();

    /* The driver's per packet chatter filled the 4 kB log ring and pushed
     * out the lines that mattered. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    ESP_ERROR_CHECK_WITHOUT_ABORT(dd_rpc_register(rpc_goto));
    ESP_ERROR_CHECK_WITHOUT_ABORT(dd_net_start());

    dd_settings_t cfg;
    dd_platform_settings_load(&cfg);
    const esp_err_t audio_err = dd_audio_init(cfg.volume);
    if (audio_err != ESP_OK) {
        ESP_LOGE(TAG, "speaker unavailable (0x%x), running silent", audio_err);
    }
    dd_platform_audio_ready(audio_err == ESP_OK);

    esp_gsp_config_t app_config;
    const esp_err_t bundle_err = ui_bundle_open(&app_config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "GSP UI unavailable (0x%x); ESP-Iris Recovery RPC remains active",
                 bundle_err);
        return;
    }

    esp_display_present_target_config_t display;
    ESP_ERROR_CHECK(board_display_init(&display));

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(board_touch_init(&touch));

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;

    ESP_ERROR_CHECK(iris_screen_mirror_init());

    esp_gsp_handle_t ui;
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&app_config, &lcd, &ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(ui));
    const esp_gsp_err_t app_err = dd_app_init(ui);
    if (app_err != ESP_GSP_OK) {
        int line = 0;
        (void)dd_app_last_error(&line);
        ESP_LOGE(TAG, "UI setup reported 0x%x, first at dd_app.c:%d", app_err, line);
    }
    s_ui = ui;

    ESP_LOGI(TAG, "Dilly-dally ready at %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (xTaskCreate(bt_start_task, "dd_bt_start", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Bluetooth start task not created");
    }
}
