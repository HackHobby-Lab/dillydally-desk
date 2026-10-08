// SPDX-License-Identifier: Apache-2.0
//
// The speaker. Streams internet radio and speeches through ESP-GMF's simple
// player, and plays the end of focus chime. One task owns the codec and the
// player, and everything else talks to it through a queue, so any caller,
// the GSP render task included, returns at once.

#pragma once

#include "dd_platform.h"
#include "esp_err.h"

esp_err_t dd_audio_init(int volume);
void dd_audio_play(const char *url);
void dd_audio_stop(void);
void dd_audio_chime(void);
void dd_audio_set_volume(int percent);
dd_audio_state_t dd_audio_state(void);
