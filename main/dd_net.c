// SPDX-License-Identifier: Apache-2.0

#include "dd_net.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "nvs.h"

static const char *TAG = "dd_net";

#define NVS_NS            "ddnet"
#define RETRY_FIRST_US    (2 * 1000 * 1000)
#define RETRY_MAX_US      (60 * 1000 * 1000)
/* Anything before 2025 means SNTP has not run yet. */
#define TIME_VALID_AFTER  1735689600

static struct {
    bool inited;
    bool sntp;
    bool ap;
    volatile bool up;
    esp_netif_t *sta;
    esp_netif_t *ap_netif;
    esp_timer_handle_t retry;
    int64_t backoff_us;
    char ssid[DD_NET_SSID_CAP];
    char pass[DD_NET_PASS_CAP];
} s;

static void load_credentials(void)
{
    nvs_handle_t h;
    s.ssid[0] = s.pass[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s.ssid);
    if (nvs_get_str(h, "ssid", s.ssid, &n) != ESP_OK) {
        s.ssid[0] = '\0';
    }
    n = sizeof(s.pass);
    if (nvs_get_str(h, "pass", s.pass, &n) != ESP_OK) {
        s.pass[0] = '\0';
    }
    nvs_close(h);
}

static void join(void)
{
    if (!s.ssid[0]) {
        return;
    }
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, s.ssid, strnlen(s.ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, s.pass, strnlen(s.pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = s.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        ESP_LOGE(TAG, "set config failed");
        return;
    }
    (void)esp_wifi_connect();
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (!s.up) {
        join();
    }
}

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "time set from the network, %lld", (long long)tv->tv_sec);
}

static void start_sntp(void)
{
    if (s.sntp) {
        return;
    }
    /* One lookup up front, so a log shows whether DNS works before blaming
     * NTP when the clock never sets. */
    struct addrinfo *res = NULL;
    const int dns = getaddrinfo("pool.ntp.org", "123", NULL, &res);
    ESP_LOGI(TAG, "dns pool.ntp.org %s", dns == 0 ? "ok" : "failed");
    if (res) {
        freeaddrinfo(res);
    }
    /* Three servers. With one, a lost first reply left the clock blank for
     * 80 s on the bench while lwIP backed off. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("time.google.com", "time.cloudflare.com", "pool.ntp.org"));
    cfg.sync_cb = on_time_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s.sntp = true;
        ESP_LOGI(TAG, "sntp started");
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        join();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s.up = false;
        const wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "disconnected, reason %d, retry in %lld s",
                 e ? (int)e->reason : 0, (long long)(s.backoff_us / 1000000));
        /* Back off so a wrong password does not keep the radio hammering. */
        (void)esp_timer_stop(s.retry);
        (void)esp_timer_start_once(s.retry, s.backoff_us);
        s.backoff_us = s.backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s.backoff_us * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "up at " IPSTR, IP2STR(&e->ip_info.ip));
        s.up = true;
        s.backoff_us = RETRY_FIRST_US;
        start_sntp();
    }
}

esp_err_t dd_net_start(void)
{
    if (s.inited) {
        return ESP_OK;
    }
    /* ESP-Iris may have done either already; both say so with INVALID_STATE. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s.sta = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s.sta, ESP_FAIL, TAG, "sta netif");

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "dd_net_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s.retry), TAG, "retry timer");
    s.backoff_us = RETRY_FIRST_US;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL), TAG, "ip ev");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    load_credentials();
    s.inited = true;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "started, network %s", s.ssid[0] ? s.ssid : "(none stored)");
    return ESP_OK;
}

esp_err_t dd_net_set_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = ESP_OK;
    if (ssid) {
        err = nvs_set_str(h, "ssid", ssid);
    }
    if (err == ESP_OK && pass) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");

    load_credentials();
    if (s.inited) {
        s.backoff_us = RETRY_FIRST_US;
        (void)esp_timer_stop(s.retry);
        if (s.up) {
            (void)esp_wifi_disconnect(); /* the disconnect event rejoins */
        } else {
            join();
        }
    }
    return ESP_OK;
}

void dd_net_ssid(char *out, size_t cap)
{
    if (cap) {
        strlcpy(out, s.ssid, cap);
    }
}

bool dd_net_has_password(void)
{
    return s.pass[0] != '\0';
}

bool dd_net_is_up(void)
{
    return s.up;
}

bool dd_net_time_valid(void)
{
    return time(NULL) > TIME_VALID_AFTER;
}

esp_err_t dd_net_ap_start(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_INVALID_STATE, TAG, "not started");
    if (s.ap) {
        return ESP_OK;
    }
    if (!s.ap_netif) {
        s.ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s.ap_netif, ESP_FAIL, TAG, "ap netif");
    }
    wifi_config_t ap = { 0 };
    const size_t n = strnlen(ssid, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, ssid, n);
    ap.ap.ssid_len = (uint8_t)n;
    strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    /* APSTA keeps the clock's own connection while the form is open. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "apsta");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    s.ap = true;
    return ESP_OK;
}

esp_err_t dd_net_ap_stop(void)
{
    if (!s.ap) {
        return ESP_OK;
    }
    s.ap = false;
    return esp_wifi_set_mode(WIFI_MODE_STA);
}
