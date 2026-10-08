// SPDX-License-Identifier: Apache-2.0

#include "dd_rpc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dd_app.h"
#include "dd_audio.h"
#include "dd_bt.h"
#include "dd_media.h"
#include "dd_net.h"
#include "esp_iris.h"
#include "esp_log.h"

static const char *TAG = "dd_rpc";

/* Handlers share the one ESP-Iris service task, so this scratch has one user. */
static struct {
    dd_rpc_goto_fn go;
    char line[256];
} s;

/* Always ESP_OK with the outcome in words. ESP-Iris reads any other return as
 * a transport failure and throws the message away (learnt on partid). */
static esp_err_t reply(uint8_t *out, size_t cap, size_t *size, const char *fmt, ...)
{
    if (!out || !size || !cap) {
        return ESP_OK;
    }
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf((char *)out, cap, fmt, args);
    va_end(args);
    *size = n < 0 ? 0 : ((size_t)n >= cap ? cap - 1 : (size_t)n);
    return ESP_OK;
}

static bool take_payload(const esp_iris_rpc_request_t *req)
{
    if (!req->payload || req->payload_size == 0 || req->payload_size >= sizeof(s.line)) {
        return false;
    }
    memcpy(s.line, req->payload, req->payload_size);
    s.line[req->payload_size] = '\0';
    return true;
}

static esp_err_t handle_set(const esp_iris_rpc_request_t *req, uint8_t *out,
                            size_t cap, size_t *size, void *ctx)
{
    (void)ctx;
    if (!take_payload(req)) {
        return reply(out, cap, size, "payload must be key=value, keys ssid pass");
    }
    char *eq = strchr(s.line, '=');
    if (!eq) {
        return reply(out, cap, size, "expected key=value, keys ssid pass");
    }
    *eq = '\0';
    const char *v = eq + 1;
    esp_err_t err;
    if (strcmp(s.line, "ssid") == 0 && strlen(v) < DD_NET_SSID_CAP) {
        err = dd_net_set_credentials(v, NULL);
    } else if (strcmp(s.line, "pass") == 0 && strlen(v) < DD_NET_PASS_CAP) {
        err = dd_net_set_credentials(NULL, v);
    } else {
        return reply(out, cap, size, "unknown key or value too long, keys ssid pass");
    }
    return reply(out, cap, size, err == ESP_OK ? "ok %s" : "failed %s", s.line);
}

static esp_err_t handle_status(const esp_iris_rpc_request_t *req, uint8_t *out,
                               size_t cap, size_t *size, void *ctx)
{
    (void)req;
    (void)ctx;
    char ssid[DD_NET_SSID_CAP];
    dd_net_ssid(ssid, sizeof(ssid));
    static const char *const AUDIO[] = { "idle", "connecting", "playing", "error" };
    int ui_line = 0;
    const int ui_err = (int)dd_app_last_error(&ui_line);
    return reply(out, cap, size,
                 "{\"ssid\":\"%s\",\"pass\":%s,\"up\":%s,\"time\":%s,\"audio\":\"%s\","
                 "\"ui_err\":%d,\"ui_line\":%d}",
                 ssid, dd_net_has_password() ? "true" : "false",
                 dd_net_is_up() ? "true" : "false",
                 dd_net_time_valid() ? "true" : "false",
                 AUDIO[dd_audio_state()], ui_err, ui_line);
}

static esp_err_t handle_page(const esp_iris_rpc_request_t *req, uint8_t *out,
                             size_t cap, size_t *size, void *ctx)
{
    (void)ctx;
    if (!take_payload(req) || !s.go) {
        return reply(out, cap, size, "payload is a page number, 0 to 4");
    }
    const int page = atoi(s.line);
    const esp_err_t err = s.go((unsigned)page);
    return reply(out, cap, size, err == ESP_OK ? "ok page %d" : "failed page %d", page);
}

static esp_err_t handle_play(const esp_iris_rpc_request_t *req, uint8_t *out,
                             size_t cap, size_t *size, void *ctx)
{
    (void)ctx;
    if (!take_payload(req)) {
        return reply(out, cap, size, "payload is a media index, or stop");
    }
    if (strcmp(s.line, "stop") == 0) {
        dd_audio_stop();
        return reply(out, cap, size, "ok stop");
    }
    /* A raw URL, for trying a stream before it goes in the list. */
    if (strncmp(s.line, "http", 4) == 0) {
        dd_audio_play(s.line);
        return reply(out, cap, size, "ok playing url");
    }
    const int i = atoi(s.line);
    if (i < 0 || i >= dd_media_count) {
        return reply(out, cap, size, "index 0 to %d", dd_media_count - 1);
    }
    dd_audio_play(dd_media[i].url);
    return reply(out, cap, size, "ok playing %s", dd_media[i].name);
}

static esp_err_t handle_bt(const esp_iris_rpc_request_t *req, uint8_t *out,
                           size_t cap, size_t *size, void *ctx)
{
    (void)ctx;
    if (!take_payload(req)) {
        return reply(out, cap, size, "payload is status, search, off or an index");
    }
    if (strcmp(s.line, "search") == 0) {
        dd_bt_search();
    } else if (strcmp(s.line, "off") == 0) {
        dd_bt_disconnect();
    } else if (s.line[0] >= '0' && s.line[0] <= '9') {
        dd_bt_connect(atoi(s.line));
    }
    static const char *const ST[] = { "off", "searching", "connecting", "connected" };
    char found[DD_BT_MAX_FOUND][DD_BT_NAME_CAP];
    const int n = dd_bt_found(found);
    char name[DD_BT_NAME_CAP];
    dd_bt_name(name, sizeof(name));
    size_t len = 0;
    reply(out, cap, &len, "{\"state\":\"%s\",\"streaming\":%s,\"device\":\"%s\",\"found\":[",
          ST[dd_bt_state()], dd_bt_streaming() ? "true" : "false", name);
    for (int i = 0; i < n && len + 40 < cap; ++i) {
        len += (size_t)snprintf((char *)out + len, cap - len, "%s\"%s\"", i ? "," : "", found[i]);
    }
    len += (size_t)snprintf((char *)out + len, cap - len, "]}");
    *size = len < cap ? len : cap - 1;
    return ESP_OK;
}

esp_err_t dd_rpc_register(dd_rpc_goto_fn go)
{
    s.go = go;
    const struct {
        uint16_t method;
        esp_iris_rpc_handler_t handler;
    } methods[] = {
        { DD_RPC_METHOD_SET, handle_set },
        { DD_RPC_METHOD_STATUS, handle_status },
        { DD_RPC_METHOD_PAGE, handle_page },
        { DD_RPC_METHOD_PLAY, handle_play },
        { DD_RPC_METHOD_BT, handle_bt },
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        const esp_err_t err = esp_iris_rpc_register(DD_RPC_SERVICE_ID, methods[i].method,
                                                    methods[i].handler, NULL);
        if (err != ESP_OK) {
            /* Not fatal. Losing one method beats losing the application. */
            ESP_LOGE(TAG, "method %u did not register, 0x%x",
                     (unsigned)methods[i].method, (int)err);
        }
    }
    ESP_LOGI(TAG, "RPC service %u ready", (unsigned)DD_RPC_SERVICE_ID);
    return ESP_OK;
}
