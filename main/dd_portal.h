// SPDX-License-Identifier: Apache-2.0
//
// The Wi-Fi setup page, served from the board's own access point while the
// Wi-Fi sheet in Settings is open. Adapted from partid's portal, cut down to
// the two fields a clock needs.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** Raise the access point and the form. Fills the network name and password. */
esp_err_t dd_portal_start(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
esp_err_t dd_portal_stop(void);
bool dd_portal_running(void);
