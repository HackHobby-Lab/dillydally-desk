// SPDX-License-Identifier: Apache-2.0
//
// Bluetooth audio out. Classic Bluetooth A2DP source, so the radio, the
// speeches and the chime can go to earphones, AirPods or a speaker instead of
// the board's own speaker.
//
// The ESP32-S31 has a BR/EDR radio, which is what nearly every headphone and
// speaker still uses for music. LE Audio would need the sink to support it,
// and most do not yet.
//
// Bluedroid calls back on its own task. The found list and the state are read
// from the GSP render task, so both sit behind one spinlock and are copied out.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dd_platform.h"
#include "esp_err.h"

/** Bring up the controller, Bluedroid and A2DP, and rejoin the remembered device. */
esp_err_t dd_bt_init(void);

dd_bt_state_t dd_bt_state(void);
void dd_bt_name(char *out, size_t cap);
int dd_bt_found(char names[DD_BT_MAX_FOUND][DD_BT_NAME_CAP]);
void dd_bt_search(void);
void dd_bt_connect(int found_index);
void dd_bt_disconnect(void);

/** True while audio should go to Bluetooth rather than the speaker. */
bool dd_bt_streaming(void);

/**
 * @brief Queue 16 bit PCM for the headphones.
 *
 * Any rate, mono or stereo. Converted to the 44.1 kHz stereo A2DP wants and
 * scaled by volume. Blocks for at most a fraction of a second when the link
 * falls behind, which paces the decoder to real time. One caller at a time.
 */
void dd_bt_write(const int16_t *pcm, int frames, int rate, int channels, int volume);
