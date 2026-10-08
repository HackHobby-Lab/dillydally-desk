// SPDX-License-Identifier: Apache-2.0
//
// Setup and remote control over ESP-Iris RPC, so wifi can be set without the
// on screen sheet and pages can be captured without a hand on the panel.
//
//   .\m.ps1 iris rpc 20482 1 --payload "ssid=MyNetwork"
//   .\m.ps1 iris rpc 20482 1 --payload "pass=..."
//   .\m.ps1 iris rpc 20482 2
//   .\m.ps1 iris rpc 20482 3 --payload "2"      # go to page 2, the timer
//   .\m.ps1 iris rpc 20482 4 --payload "0"      # play media 0, "stop" stops
//   .\m.ps1 iris rpc 20482 5 --payload "search" # then "status", then "0"

#pragma once

#include "esp_err.h"

/* 0x5002, next to partid's 0x5001. The CLI takes decimal, 20482. */
#define DD_RPC_SERVICE_ID    0x5002U
#define DD_RPC_METHOD_SET    1U
#define DD_RPC_METHOD_STATUS 2U
#define DD_RPC_METHOD_PAGE   3U
/* Plays one entry of dd_media straight through dd_audio, so streams can be
 * tested with no hand on the panel. The UI does not see it. */
#define DD_RPC_METHOD_PLAY   4U
/* Bluetooth audio: "status", "search", or a found index to connect, "off". */
#define DD_RPC_METHOD_BT     5U

typedef esp_err_t (*dd_rpc_goto_fn)(unsigned page);

esp_err_t dd_rpc_register(dd_rpc_goto_fn go);
