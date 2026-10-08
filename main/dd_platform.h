// SPDX-License-Identifier: Apache-2.0
//
// Everything the UI needs from the world. The device implements it with
// SNTP, NVS, the wifi driver and the speaker; the simulator backend fakes it.
// Every call is made from the GSP render task and must return promptly.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int16_t tz_min;      /* offset from UTC in minutes */
    uint8_t auto_idx;    /* index into the auto change choices, 0 is off */
    bool h24;
    uint8_t brightness;  /* percent */
    uint8_t volume;      /* percent */
    bool chime;
    uint8_t focus_min;
} dd_settings_t;

typedef enum {
    DD_AUDIO_IDLE = 0,
    DD_AUDIO_CONNECTING,
    DD_AUDIO_PLAYING,
    DD_AUDIO_ERROR,
} dd_audio_state_t;

void dd_platform_settings_load(dd_settings_t *out);
void dd_platform_settings_save(const dd_settings_t *s);

/** Monotonic milliseconds. */
uint32_t dd_platform_ms(void);
/** UTC seconds, and whether it has been set from the network yet. */
time_t dd_platform_utc(bool *out_valid);
uint32_t dd_platform_random(void);

bool dd_platform_wifi_up(void);
/** Stored network name, empty when none. */
void dd_platform_wifi_name(char *out, size_t cap);
/** Raise the setup network and fill three lines telling the user what to do. */
bool dd_platform_setup_start(char lines[3][48]);
void dd_platform_setup_stop(void);

void dd_platform_set_brightness(int percent);
void dd_platform_set_volume(int percent);
void dd_platform_chime(void);

/* Bluetooth audio out, A2DP to headphones or a speaker. */
typedef enum {
    DD_BT_OFF = 0,      /* not connected, not searching */
    DD_BT_SEARCHING,
    DD_BT_CONNECTING,
    DD_BT_CONNECTED,
} dd_bt_state_t;

#define DD_BT_MAX_FOUND 4
#define DD_BT_NAME_CAP  32

dd_bt_state_t dd_platform_bt_state(void);
/** Name of the connected device, or the remembered one when not connected. */
void dd_platform_bt_name(char *out, size_t cap);
/** Devices found by the last or current search, best first. Returns count. */
int dd_platform_bt_found(char names[DD_BT_MAX_FOUND][DD_BT_NAME_CAP]);
void dd_platform_bt_search(void);
void dd_platform_bt_connect(int found_index);
void dd_platform_bt_disconnect(void);

void dd_platform_audio_play(const char *url);
void dd_platform_audio_stop(void);
dd_audio_state_t dd_platform_audio_state(void);
