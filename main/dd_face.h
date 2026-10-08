// SPDX-License-Identifier: Apache-2.0
//
// Moods and the face images that show them. Moods are in face_art.py order,
// which is also the order the generated tables use.

#pragma once

#include <stdint.h>

#include "gsp/gsp_component.h"

typedef enum {
    DD_MOOD_IDLE = 0,
    DD_MOOD_JOY,
    DD_MOOD_ENNUI,
    DD_MOOD_SAD,
    DD_MOOD_ANXIOUS,
    DD_MOOD_FEAR,
    DD_MOOD_ANGER,
    DD_MOOD_DISGUST,
    DD_MOOD_ENVY,
    DD_MOOD_EMBARRASSED,
    DD_MOOD_COUNT,
} dd_mood_t;

typedef struct {
    uint8_t mood;
    gsp_component_key_t open;
    gsp_component_key_t blink;
} dd_face_img_t;
