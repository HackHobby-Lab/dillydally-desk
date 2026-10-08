// SPDX-License-Identifier: Apache-2.0
//
// Based on ESP-IDF's a2dp_source example, which lists the ESP32-S31.

#include "dd_bt.h"

#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "dd_bt";

#define DEVICE_NAME        "Dilly-dally"
#define NVS_NS             "ddbt"
#define OUT_RATE           44100
#define RING_BYTES         (32 * 1024)   /* about 185 ms of 44.1 kHz stereo */
#define WRITE_WAIT_MS      200
#define CHUNK_FRAMES       256
#define SEARCH_UNITS       8             /* x 1.28 s, about ten seconds */
#define CONNECT_TIMEOUT_US (20 * 1000 * 1000)

typedef struct {
    esp_bd_addr_t bda;
    char name[DD_BT_NAME_CAP];
} peer_t;

static struct {
    portMUX_TYPE lock;
    bool ready;
    volatile bool starting;
    dd_bt_state_t state;
    peer_t found[DD_BT_MAX_FOUND];
    int found_count;
    peer_t peer;          /* the connected, connecting or remembered device */
    bool have_peer;
    volatile bool media_started;
    RingbufHandle_t ring;
    esp_timer_handle_t connect_timer;

    /* resampler state, owned by the single writer */
    float pos;
    int16_t prev_l;
    int16_t prev_r;
} s = { .lock = portMUX_INITIALIZER_UNLOCKED };

/* ----------------------------------------------------------- persistence */

static void remember(const peer_t *p)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (p) {
        (void)nvs_set_blob(h, "peer", p, sizeof(*p));
    } else {
        (void)nvs_erase_key(h, "peer");
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

static bool recall(peer_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = sizeof(*out);
    const bool ok = nvs_get_blob(h, "peer", out, &n) == ESP_OK && n == sizeof(*out);
    nvs_close(h);
    return ok;
}

static void set_state(dd_bt_state_t st)
{
    portENTER_CRITICAL(&s.lock);
    s.state = st;
    portEXIT_CRITICAL(&s.lock);
}

/* -------------------------------------------------------------- discovery */

static bool name_from_eir(uint8_t *eir, char *out, size_t cap)
{
    uint8_t len = 0;
    uint8_t *p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!p) {
        p = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    }
    if (!p || !len) {
        return false;
    }
    const size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Keep audio sinks only: the rendering service bit, or the audio/video
 * major class, which some headphones set without the service bit. */
static bool is_audio_sink(uint32_t cod)
{
    if (!esp_bt_gap_is_valid_cod(cod)) {
        return false;
    }
    return (esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING) ||
           esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV;
}

static void on_found(esp_bt_gap_cb_param_t *param)
{
    uint32_t cod = 0;
    char name[DD_BT_NAME_CAP] = { 0 };
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
            cod = *(uint32_t *)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && !name[0]) {
            (void)name_from_eir(p->val, name, sizeof(name));
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && !name[0]) {
            const int n = p->len < (int)sizeof(name) - 1 ? p->len : (int)sizeof(name) - 1;
            memcpy(name, p->val, (size_t)n);
            name[n] = '\0';
        }
    }
    /* A device with no name cannot be told apart on screen, so skip it. */
    if (!is_audio_sink(cod) || !name[0]) {
        return;
    }
    portENTER_CRITICAL(&s.lock);
    bool dup = false;
    for (int i = 0; i < s.found_count; ++i) {
        dup |= memcmp(s.found[i].bda, param->disc_res.bda, ESP_BD_ADDR_LEN) == 0;
    }
    if (!dup && s.found_count < DD_BT_MAX_FOUND) {
        memcpy(s.found[s.found_count].bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        strlcpy(s.found[s.found_count].name, name, DD_BT_NAME_CAP);
        s.found_count++;
    }
    portEXIT_CRITICAL(&s.lock);
    if (!dup) {
        ESP_LOGI(TAG, "found %s", name);
    }
}

static void on_gap(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        on_found(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
            s.state == DD_BT_SEARCHING) {
            set_state(DD_BT_OFF);
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing %s", param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS
                 ? "done" : "failed");
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        /* Legacy pairing. Speakers without a keypad use 0000. */
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        (void)esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* Just works. The board has no way to show or type a code. */
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ A2DP */

static void set_pref_codec(esp_a2d_conn_hdl_t hdl, const esp_a2d_mcc_t *caps)
{
    if (caps->type != ESP_A2D_MCT_SBC) {
        return;
    }
    const esp_a2d_cie_sbc_t *c = &caps->cie.sbc_info;
    esp_a2d_mcc_t pref = { .type = ESP_A2D_MCT_SBC };
    esp_a2d_cie_sbc_t *p = &pref.cie.sbc_info;
    p->samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    p->ch_mode = (c->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO)
                 ? ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO : ESP_A2D_SBC_CIE_CH_MODE_STEREO;
    p->block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    p->num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    p->alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    p->min_bitpool = c->min_bitpool > 2 ? c->min_bitpool : 2;
    p->max_bitpool = c->max_bitpool < 53 ? c->max_bitpool : 53;
    if (!(c->samp_freq & p->samp_freq) || !(c->ch_mode & p->ch_mode) ||
        !(c->block_len & p->block_len) || !(c->num_subbands & p->num_subbands) ||
        !(c->alloc_mthd & p->alloc_mthd)) {
        ESP_LOGW(TAG, "sink does not take 44.1 kHz stereo SBC, using its default");
        return;
    }
    (void)esp_a2d_source_set_pref_mcc(hdl, &pref);
}

static void on_a2d(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            (void)esp_timer_stop(s.connect_timer);
            set_state(DD_BT_CONNECTED);
            remember(&s.peer);
            ESP_LOGI(TAG, "connected to %s", s.peer.name);
            (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            (void)esp_timer_stop(s.connect_timer);
            s.media_started = false;
            if (s.state != DD_BT_SEARCHING) {
                set_state(DD_BT_OFF);
            }
            ESP_LOGI(TAG, "disconnected");
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            break;
        }
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            (void)esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START) {
            s.media_started = true;
            ESP_LOGI(TAG, "streaming");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state != ESP_A2D_AUDIO_STATE_STARTED) {
            s.media_started = false;
        }
        break;
    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT:
        set_pref_codec(param->a2d_report_snk_codec_caps_stat.conn_hdl,
                       &param->a2d_report_snk_codec_caps_stat.mcc);
        break;
    default:
        break;
    }
}

/* Pulled by the stack for every packet. Silence when the ring runs dry, so an
 * underrun is a gap, never a stall. */
static int32_t on_a2d_data(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) {
        return 0;
    }
    int32_t got = 0;
    while (got < len && s.ring) {
        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(s.ring, &n, 0, (size_t)(len - got));
        if (!p) {
            break;
        }
        memcpy(data + got, p, n);
        vRingbufferReturnItem(s.ring, p);
        got += (int32_t)n;
    }
    if (got < len) {
        memset(data + got, 0, (size_t)(len - got));
    }
    return len;
}

static void connect_timeout(void *arg)
{
    (void)arg;
    if (s.state == DD_BT_CONNECTING) {
        ESP_LOGW(TAG, "connect timed out");
        set_state(DD_BT_OFF);
    }
}

/* --------------------------------------------------------------- public */

/*
 * The stack starts only when it is wanted: at boot if a device is
 * remembered, otherwise the first time Search is pressed. Running, it holds
 * about 90 kB of internal RAM and shares the radio with wifi, and a clock
 * that never uses headphones should pay neither.
 */
static esp_err_t start_stack(void)
{
    /* A log per step. The first attempt hung somewhere in here with nothing
     * in the log to say where. */
    (void)esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t ccfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&ccfg), TAG, "controller init");
    ESP_LOGI(TAG, "controller enable");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG, "controller");
    ESP_LOGI(TAG, "bluedroid init");
    esp_bluedroid_config_t bcfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bcfg), TAG, "bluedroid init");
    ESP_LOGI(TAG, "bluedroid enable");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid");
    ESP_LOGI(TAG, "profiles");

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    (void)esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = { 0 };
    (void)esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);
    (void)esp_bt_gap_set_device_name(DEVICE_NAME);
    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(on_gap), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_a2d_register_callback(on_a2d), TAG, "a2d cb");
    ESP_RETURN_ON_ERROR(esp_a2d_source_register_data_callback(on_a2d_data), TAG, "a2d data");
    ESP_RETURN_ON_ERROR(esp_a2d_source_init(), TAG, "a2d init");
    /* The board finds speakers; speakers do not find the board. */
    (void)esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    s.ready = true;
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

static void start_and_search_task(void *arg)
{
    (void)arg;
    if (start_stack() == ESP_OK) {
        if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, SEARCH_UNITS, 0) != ESP_OK) {
            set_state(DD_BT_OFF);
        }
    } else {
        set_state(DD_BT_OFF);
    }
    s.starting = false;
    vTaskDelete(NULL);
}

esp_err_t dd_bt_init(void)
{
    s.ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s.ring, ESP_ERR_NO_MEM, TAG, "ring");
    const esp_timer_create_args_t targs = { .callback = connect_timeout, .name = "dd_bt_conn" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s.connect_timer), TAG, "timer");

    if (!recall(&s.peer)) {
        ESP_LOGI(TAG, "no remembered device, stack starts on first search");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(start_stack(), TAG, "stack");
    s.have_peer = true;
    ESP_LOGI(TAG, "rejoining %s", s.peer.name);
    set_state(DD_BT_CONNECTING);
    (void)esp_timer_start_once(s.connect_timer, CONNECT_TIMEOUT_US);
    (void)esp_a2d_source_connect(s.peer.bda);
    return ESP_OK;
}

dd_bt_state_t dd_bt_state(void)
{
    return s.state;
}

void dd_bt_name(char *out, size_t cap)
{
    portENTER_CRITICAL(&s.lock);
    strlcpy(out, s.have_peer ? s.peer.name : "", cap);
    portEXIT_CRITICAL(&s.lock);
}

int dd_bt_found(char names[DD_BT_MAX_FOUND][DD_BT_NAME_CAP])
{
    portENTER_CRITICAL(&s.lock);
    const int n = s.found_count;
    for (int i = 0; i < n; ++i) {
        memcpy(names[i], s.found[i].name, DD_BT_NAME_CAP);
    }
    portEXIT_CRITICAL(&s.lock);
    return n;
}

void dd_bt_search(void)
{
    if (s.starting || s.state == DD_BT_SEARCHING || s.state == DD_BT_CONNECTED) {
        return;
    }
    portENTER_CRITICAL(&s.lock);
    s.found_count = 0;
    portEXIT_CRITICAL(&s.lock);
    set_state(DD_BT_SEARCHING);
    if (!s.ready) {
        /* Bringing the stack up takes a moment, so not on the caller's task. */
        s.starting = true;
        if (xTaskCreate(start_and_search_task, "dd_bt_start", 4096, NULL, 3, NULL) != pdPASS) {
            s.starting = false;
            set_state(DD_BT_OFF);
        }
        return;
    }
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, SEARCH_UNITS, 0) != ESP_OK) {
        set_state(DD_BT_OFF);
    }
}

void dd_bt_connect(int i)
{
    if (!s.ready || i < 0) {
        return;
    }
    portENTER_CRITICAL(&s.lock);
    const bool valid = i < s.found_count;
    if (valid) {
        s.peer = s.found[i];
        s.have_peer = true;
    }
    portEXIT_CRITICAL(&s.lock);
    if (!valid || s.state == DD_BT_CONNECTED || s.state == DD_BT_CONNECTING) {
        return;
    }
    if (s.state == DD_BT_SEARCHING) {
        (void)esp_bt_gap_cancel_discovery();
    }
    ESP_LOGI(TAG, "connecting to %s", s.peer.name);
    set_state(DD_BT_CONNECTING);
    (void)esp_timer_start_once(s.connect_timer, CONNECT_TIMEOUT_US);
    if (esp_a2d_source_connect(s.peer.bda) != ESP_OK) {
        set_state(DD_BT_OFF);
    }
}

void dd_bt_disconnect(void)
{
    if (!s.ready || s.state != DD_BT_CONNECTED) {
        return;
    }
    /* Forgotten too, so the next boot does not grab it back. */
    remember(NULL);
    s.media_started = false;
    (void)esp_a2d_source_disconnect(s.peer.bda);
}

bool dd_bt_streaming(void)
{
    return s.ready && s.state == DD_BT_CONNECTED && s.media_started;
}

/* Linear interpolation to 44.1 kHz stereo. Good enough for speech and radio
 * at these bitrates, and cheap. pos carries the fractional read position from
 * one call to the next, prev the last frame of the previous call. */
void dd_bt_write(const int16_t *pcm, int frames, int rate, int channels, int volume)
{
    if (!s.ring || !pcm || frames <= 0 || rate <= 0 || (channels != 1 && channels != 2)) {
        return;
    }
    const float step = (float)rate / OUT_RATE;
    const int32_t gain = volume < 0 ? 0 : volume > 100 ? 256 : volume * 256 / 100;
    int16_t out[CHUNK_FRAMES * 2];
    int o = 0;
    float p = s.pos;
    while (p < frames - 1) {
        const int i = (int)p;          /* p >= -1, so -1 here means prev */
        const float f = p - (float)i;
        int32_t al, ar, bl, br;
        if (p < 0) {
            al = s.prev_l, ar = s.prev_r;
            bl = pcm[0], br = pcm[channels - 1];
        } else {
            al = pcm[i * channels], ar = pcm[i * channels + channels - 1];
            bl = pcm[(i + 1) * channels], br = pcm[(i + 1) * channels + channels - 1];
        }
        const float fp = p < 0 ? p + 1.0f : f;
        const int32_t l = (int32_t)((float)al + fp * (float)(bl - al));
        const int32_t r = (int32_t)((float)ar + fp * (float)(br - ar));
        out[o * 2] = (int16_t)(l * gain / 256);
        out[o * 2 + 1] = (int16_t)(r * gain / 256);
        if (++o == CHUNK_FRAMES) {
            (void)xRingbufferSend(s.ring, out, sizeof(out), pdMS_TO_TICKS(WRITE_WAIT_MS));
            o = 0;
        }
        p += step;
    }
    if (o) {
        (void)xRingbufferSend(s.ring, out, (size_t)o * 4, pdMS_TO_TICKS(WRITE_WAIT_MS));
    }
    s.pos = p - (float)frames;
    s.prev_l = pcm[(frames - 1) * channels];
    s.prev_r = pcm[(frames - 1) * channels + channels - 1];
}
