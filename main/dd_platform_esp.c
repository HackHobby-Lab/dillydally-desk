// SPDX-License-Identifier: Apache-2.0
//
// dd_platform on the board. Settings in NVS, time from SNTP, wifi from dd_net,
// sound from dd_audio. Called from the GSP render task, so nothing here blocks
// for longer than an NVS write.

#include <stdio.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "dd_audio.h"
#include "dd_bt.h"
#include "dd_net.h"
#include "dd_platform.h"
#include "dd_portal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "dd_platform";

#define NVS_NS       "dd"
#define NVS_KEY      "cfg"
#define CFG_VERSION  1

typedef struct {
    uint8_t version;
    dd_settings_t cfg;
} stored_t;

static const dd_settings_t DEFAULTS = {
    .tz_min = 5 * 60,
    .auto_idx = 0,
    .h24 = false,
    .brightness = 80,
    .volume = 60,
    .chime = true,
    .focus_min = 25,
};

static bool s_audio_ready;

void dd_platform_settings_load(dd_settings_t *out)
{
    *out = DEFAULTS;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    stored_t st;
    size_t n = sizeof(st);
    if (nvs_get_blob(h, NVS_KEY, &st, &n) == ESP_OK && n == sizeof(st) &&
        st.version == CFG_VERSION) {
        *out = st.cfg;
    }
    nvs_close(h);
}

void dd_platform_settings_save(const dd_settings_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "settings not saved, nvs open failed");
        return;
    }
    const stored_t st = { .version = CFG_VERSION, .cfg = *cfg };
    if (nvs_set_blob(h, NVS_KEY, &st, sizeof(st)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGE(TAG, "settings not saved");
    }
    nvs_close(h);
}

uint32_t dd_platform_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

time_t dd_platform_utc(bool *out_valid)
{
    *out_valid = dd_net_time_valid();
    return time(NULL);
}

uint32_t dd_platform_random(void)
{
    return esp_random();
}

bool dd_platform_wifi_up(void)
{
    return dd_net_is_up();
}

void dd_platform_wifi_name(char *out, size_t cap)
{
    dd_net_ssid(out, cap);
}

bool dd_platform_setup_start(char lines[3][48])
{
    char ssid[33];
    char pass[24];
    if (dd_portal_start(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
        return false;
    }
    snprintf(lines[0], 48, "Join %s", ssid);
    snprintf(lines[1], 48, "Password %s", pass);
    snprintf(lines[2], 48, "Then open 192.168.4.1");
    return true;
}

void dd_platform_setup_stop(void)
{
    (void)dd_portal_stop();
}

void dd_platform_set_brightness(int percent)
{
    (void)bsp_display_brightness_set(percent);
}

void dd_platform_set_volume(int percent)
{
    if (s_audio_ready) {
        dd_audio_set_volume(percent);
    }
}

void dd_platform_chime(void)
{
    if (s_audio_ready) {
        dd_audio_chime();
    }
}

dd_bt_state_t dd_platform_bt_state(void) { return dd_bt_state(); }
void dd_platform_bt_name(char *out, size_t cap) { dd_bt_name(out, cap); }
int dd_platform_bt_found(char names[DD_BT_MAX_FOUND][DD_BT_NAME_CAP])
{
    return dd_bt_found(names);
}
void dd_platform_bt_search(void) { dd_bt_search(); }
void dd_platform_bt_connect(int i) { dd_bt_connect(i); }
void dd_platform_bt_disconnect(void) { dd_bt_disconnect(); }

void dd_platform_audio_play(const char *url)
{
    if (s_audio_ready) {
        dd_audio_play(url);
    }
}

void dd_platform_audio_stop(void)
{
    if (s_audio_ready) {
        dd_audio_stop();
    }
}

dd_audio_state_t dd_platform_audio_state(void)
{
    return s_audio_ready ? dd_audio_state() : DD_AUDIO_IDLE;
}

/* Called from app_main before the UI starts. A failed speaker leaves the rest
 * of the app running, silent. */
void dd_platform_audio_ready(bool ready)
{
    s_audio_ready = ready;
}
