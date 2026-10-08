// SPDX-License-Identifier: Apache-2.0
//
// dd_platform for the simulator. Real wall clock, settings in memory, and a
// pretend radio that tunes in after a second, so every screen and mood can be
// exercised without a board.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "dd_app.h"
#include "dd_platform.h"
#include "gsp_sim_bridge.h"

static dd_settings_t s_cfg = {
    .tz_min = 5 * 60, .auto_idx = 0, .h24 = false, .brightness = 80,
    .volume = 60, .chime = true, .focus_min = 25,
};
static dd_audio_state_t s_audio;

/* A pretend Bluetooth: a search finds two speakers after two seconds, and a
 * connect lands after one. Enough to walk every state of the sheet. */
static dd_bt_state_t s_bt;
static uint32_t s_bt_since;
static int s_bt_found;
static int s_bt_peer = -1;
static const char *const BT_NAMES[] = { "Desk Speaker", "AirPods Pro" };
static uint32_t s_audio_since;

/* MinGW has neither timespec_get nor a usable CLOCK_MONOTONIC everywhere. */
uint32_t dd_platform_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U);
#endif
}

void dd_platform_settings_load(dd_settings_t *out) { *out = s_cfg; }
void dd_platform_settings_save(const dd_settings_t *cfg) { s_cfg = *cfg; }

time_t dd_platform_utc(bool *out_valid)
{
    *out_valid = true;
    return time(NULL);
}

uint32_t dd_platform_random(void) { return (uint32_t)rand(); }
bool dd_platform_wifi_up(void) { return true; }
void dd_platform_wifi_name(char *out, size_t cap) { snprintf(out, cap, "HomeNet"); }

bool dd_platform_setup_start(char lines[3][48])
{
    snprintf(lines[0], 48, "Join DillyDally-1A2B");
    snprintf(lines[1], 48, "Password dally0a1a2b");
    snprintf(lines[2], 48, "Then open 192.168.4.1");
    return true;
}

void dd_platform_setup_stop(void) {}
void dd_platform_set_brightness(int percent) { (void)percent; }
void dd_platform_set_volume(int percent) { (void)percent; }
void dd_platform_chime(void) { fprintf(stderr, "dd_sim: chime\n"); }

static void bt_advance(void)
{
    const uint32_t t = dd_platform_ms() - s_bt_since;
    if (s_bt == DD_BT_SEARCHING && t > 2000U) {
        s_bt_found = 2;
        s_bt = DD_BT_OFF;
    } else if (s_bt == DD_BT_CONNECTING && t > 1000U) {
        s_bt = DD_BT_CONNECTED;
    }
}

dd_bt_state_t dd_platform_bt_state(void)
{
    bt_advance();
    return s_bt;
}

void dd_platform_bt_name(char *out, size_t cap)
{
    snprintf(out, cap, "%s", s_bt_peer >= 0 ? BT_NAMES[s_bt_peer] : "");
}

int dd_platform_bt_found(char names[DD_BT_MAX_FOUND][DD_BT_NAME_CAP])
{
    bt_advance();
    for (int i = 0; i < s_bt_found; ++i) {
        snprintf(names[i], DD_BT_NAME_CAP, "%s", BT_NAMES[i]);
    }
    return s_bt_found;
}

void dd_platform_bt_search(void)
{
    s_bt = DD_BT_SEARCHING;
    s_bt_found = 0;
    s_bt_since = dd_platform_ms();
}

void dd_platform_bt_connect(int i)
{
    if (i >= 0 && i < s_bt_found) {
        s_bt_peer = i;
        s_bt = DD_BT_CONNECTING;
        s_bt_since = dd_platform_ms();
    }
}

void dd_platform_bt_disconnect(void)
{
    s_bt = DD_BT_OFF;
    s_bt_peer = -1;
}

void dd_platform_audio_play(const char *url)
{
    fprintf(stderr, "dd_sim: play %s\n", url);
    s_audio = DD_AUDIO_CONNECTING;
    s_audio_since = dd_platform_ms();
}

void dd_platform_audio_stop(void) { s_audio = DD_AUDIO_IDLE; }

dd_audio_state_t dd_platform_audio_state(void)
{
    if (s_audio == DD_AUDIO_CONNECTING && dd_platform_ms() - s_audio_since > 1000U) {
        s_audio = DD_AUDIO_PLAYING;
    }
    return s_audio;
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return dd_app_init(ui);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    dd_app_deinit(ui);
    int line = 0;
    const int err = (int)dd_app_last_error(&line);
    fprintf(stderr, "dd_sim: last_error=%d first_line=%d\n", err, line);
}
