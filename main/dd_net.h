// SPDX-License-Identifier: Apache-2.0
//
// Station wifi that stays up, and network time once it is.
//
// Unlike partid, which held the radio only per query because it was measuring
// what a query costs, a desk clock wants the network all the time. It joins at
// boot, rejoins on its own after a drop, and starts SNTP on the first address.
// Credentials live in the app owned `nvs` partition, never in the image.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define DD_NET_SSID_CAP 33
#define DD_NET_PASS_CAP 65

/** Bring up the driver and join the stored network, if there is one. */
esp_err_t dd_net_start(void);

/** Store new credentials and rejoin with them. Either may be NULL to keep. */
esp_err_t dd_net_set_credentials(const char *ssid, const char *pass);

void dd_net_ssid(char *out, size_t cap);
bool dd_net_has_password(void);
bool dd_net_is_up(void);
/** True once SNTP has set the clock. */
bool dd_net_time_valid(void);

/** Raise a setup access point alongside the station. Idempotent. */
esp_err_t dd_net_ap_start(const char *ssid, const char *pass);
esp_err_t dd_net_ap_stop(void);
