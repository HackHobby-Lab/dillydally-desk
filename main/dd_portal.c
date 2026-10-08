// SPDX-License-Identifier: Apache-2.0

#include "dd_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dd_net.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "dd_portal";

#define BODY_MAX 512
#define PAGE_MAX 3072

static struct {
    httpd_handle_t server;
    bool running;
} s;

/* Named and keyed from the MAC, so two boards never collide and nothing is
 * baked into a publishable image. It only exists while someone is at the board. */
static void identity(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ssid, ssid_cap, "DillyDally-%02X%02X", mac[4], mac[5]);
    snprintf(pass, pass_cap, "dally%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static void url_decode(char *p)
{
    char *w = p;
    for (const char *r = p; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            const char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* False when absent, which is different from present and empty. */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    char needle[16];
    snprintf(needle, sizeof(needle), "%s=", key);
    const size_t len = strlen(needle);
    for (const char *p = body; p && *p;) {
        if (strncmp(p, needle, len) == 0) {
            const char *end = strchr(p, '&');
            size_t n = end ? (size_t)(end - p) - len : strlen(p + len);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, p + len, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return false;
}

/* Escape the stored name for an HTML attribute. */
static void attr_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; ++in) {
        if (*in == '"') {
            o += (size_t)snprintf(out + o, cap - o, "&quot;");
        } else if (*in == '<') {
            o += (size_t)snprintf(out + o, cap - o, "&lt;");
        } else if (*in == '&') {
            o += (size_t)snprintf(out + o, cap - o, "&amp;");
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Dilly-dally</title><style>"
    "body{background:#141413;color:#F0EEE6;font:16px system-ui,sans-serif;margin:0;padding:24px}"
    "h1{font-size:22px;margin:0 0 4px}p{color:#8A877F;font-size:14px;margin:0 0 20px}"
    "label{display:block;margin:16px 0 6px;font-size:13px;color:#8A877F}"
    "input{width:100%%;box-sizing:border-box;background:#201F1D;border:1px solid #2E2C29;"
    "color:#F0EEE6;border-radius:12px;padding:12px;font-size:16px}"
    "button{width:100%%;margin-top:24px;background:#FF4C01;color:#fff;border:0;"
    "border-radius:24px;padding:14px;font-size:16px;font-weight:600}"
    ".ok{border:1px solid #D97757;color:#D97757;padding:12px;border-radius:12px;margin-bottom:16px}"
    "</style></head><body><h1>Dilly-dally</h1><p>Your home Wi-Fi, for the clock and the radio.</p>"
    "%s<form method=post action=/save>"
    "<label>Network name</label><input name=ssid value=\"%s\" autocapitalize=none>"
    "<label>Password</label><input name=pass type=password placeholder='%s'>"
    "<button type=submit>Save</button></form></body></html>";

static esp_err_t get_root(httpd_req_t *req)
{
    char *page = heap_caps_malloc(PAGE_MAX, MALLOC_CAP_SPIRAM);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    char ssid[DD_NET_SSID_CAP];
    char esc[DD_NET_SSID_CAP * 6];
    dd_net_ssid(ssid, sizeof(ssid));
    attr_escape(ssid, esc, sizeof(esc));
    const int n = snprintf(page, PAGE_MAX, PAGE,
                           strstr(req->uri, "saved") ? "<div class=ok>Saved. Joining now.</div>" : "",
                           esc,
                           dd_net_has_password() ? "stored, leave blank to keep" : "");
    esp_err_t err;
    if (n < 0 || n >= PAGE_MAX) {
        err = httpd_resp_send_500(req);
    } else {
        httpd_resp_set_type(req, "text/html");
        err = httpd_resp_send(req, page, n);
    }
    free(page);
    return err;
}

static esp_err_t post_save(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char body[BODY_MAX];
    int got = 0;
    while (got < (int)req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return httpd_resp_send_500(req);
        }
        got += n;
    }
    body[got] = '\0';

    char ssid[DD_NET_SSID_CAP];
    char pass[DD_NET_PASS_CAP];
    const bool has_ssid = form_field(body, "ssid", ssid, sizeof(ssid)) && ssid[0];
    /* A blank password box keeps the stored one. */
    const bool has_pass = form_field(body, "pass", pass, sizeof(pass)) && pass[0];
    if (has_ssid) {
        /* Same network with a blank box keeps the password; a new network
         * with a blank box is an open one. */
        char current[DD_NET_SSID_CAP];
        dd_net_ssid(current, sizeof(current));
        const char *p = has_pass ? pass : (strcmp(current, ssid) == 0 ? NULL : "");
        (void)dd_net_set_credentials(ssid, p);
        ESP_LOGI(TAG, "network saved from the portal");
    } else if (has_pass) {
        (void)dd_net_set_credentials(NULL, pass);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved");
    return httpd_resp_send(req, NULL, 0);
}

/* Captive portal probes and typos land on the form. */
static esp_err_t get_any(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t dd_portal_start(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    identity(ssid, ssid_cap, pass, pass_cap);
    if (s.running) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(dd_net_ap_start(ssid, pass), TAG, "ap");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 5120;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s.server, &cfg);
    if (err != ESP_OK) {
        (void)dd_net_ap_stop();
        return err;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = get_root };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = post_save };
    const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = get_any };
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s.server, &root));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s.server, &save));
    ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(s.server, &any));
    s.running = true;
    ESP_LOGI(TAG, "portal up on %s", ssid);
    return ESP_OK;
}

esp_err_t dd_portal_stop(void)
{
    if (!s.running) {
        return ESP_OK;
    }
    if (s.server) {
        (void)httpd_stop(s.server);
        s.server = NULL;
    }
    s.running = false;
    return dd_net_ap_stop();
}

bool dd_portal_running(void)
{
    return s.running;
}
