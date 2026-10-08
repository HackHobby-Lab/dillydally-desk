// SPDX-License-Identifier: Apache-2.0
//
// The whole Dilly-dally UI: pages, moods, clock, focus timer, audio controls
// and settings. Portable, so the device and the simulator run the same code.

#pragma once

#include "esp_gsp.h"

esp_gsp_err_t dd_app_init(esp_gsp_handle_t ui);
void dd_app_deinit(esp_gsp_handle_t ui);

/** Jump to a page, 0 face to 4 settings. Safe from other tasks. */
esp_gsp_err_t dd_app_goto(esp_gsp_handle_t ui, unsigned page);

/** Last error any UI setter reported, and the source line of the first. */
esp_gsp_err_t dd_app_last_error(int *out_line);
