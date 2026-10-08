// SPDX-License-Identifier: Apache-2.0

#include "dd_audio.h"

#include <math.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "dd_bt.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "dd_audio";

#define URL_CAP          256
#define QUEUE_LEN        6
#define TASK_STACK       4096
#define CHIME_RATE       16000
#define CHIME_FRAME      256

typedef enum { CMD_PLAY, CMD_STOP, CMD_CHIME, CMD_VOLUME } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    int value;
    char url[URL_CAP];
} cmd_t;

static struct {
    QueueHandle_t q;
    esp_codec_dev_handle_t spk;
    esp_asp_handle_t player;
    int volume;
    bool streaming;
    int rate;       /* of the stream now decoding, for the Bluetooth resampler */
    int channels;
    volatile dd_audio_state_t state;
} s;

/* ------------------------------------------------------------- codec */

static void codec_open(int rate, int bits, int channels)
{
    (void)esp_codec_dev_close(s.spk);
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = (uint32_t)rate,
        .bits_per_sample = (uint8_t)bits,
        .channel = (uint8_t)channels,
    };
    if (esp_codec_dev_open(s.spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open %d Hz failed", rate);
    }
    (void)esp_codec_dev_set_out_vol(s.spk, s.volume);
}

/* ------------------------------------------------------------- player */

/* Decoded audio goes to the headphones when a Bluetooth link is streaming,
 * otherwise to the board's speaker. Never both. */
static int on_pcm(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    if (!data || size <= 0) {
        return 0;
    }
    if (dd_bt_streaming() && s.channels > 0) {
        dd_bt_write((const int16_t *)data, size / (2 * s.channels), s.rate, s.channels,
                    s.volume);
    } else {
        (void)esp_codec_dev_write(s.spk, data, size);
    }
    return 0;
}

static int on_player_event(esp_asp_event_pkt_t *e, void *ctx)
{
    (void)ctx;
    if (e->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
        esp_asp_music_info_t info = { 0 };
        memcpy(&info, e->payload, e->payload_size < sizeof(info) ? e->payload_size : sizeof(info));
        ESP_LOGI(TAG, "stream %d Hz, %d ch, %d bit", info.sample_rate, info.channels, info.bits);
        s.rate = info.sample_rate;
        s.channels = info.channels;
        codec_open(info.sample_rate, info.bits, info.channels);
    } else if (e->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t st = 0;
        memcpy(&st, e->payload, e->payload_size < sizeof(st) ? e->payload_size : sizeof(st));
        ESP_LOGI(TAG, "state %s", esp_audio_simple_player_state_to_str(st));
        if (st == ESP_ASP_STATE_RUNNING) {
            s.state = DD_AUDIO_PLAYING;
        } else if (st == ESP_ASP_STATE_ERROR) {
            s.state = DD_AUDIO_ERROR;
            s.streaming = false;
        } else if (st == ESP_ASP_STATE_FINISHED || st == ESP_ASP_STATE_STOPPED) {
            if (s.state != DD_AUDIO_ERROR) {
                s.state = DD_AUDIO_IDLE;
            }
            s.streaming = false;
        }
    }
    return 0;
}

static void stream_stop(void)
{
    if (s.streaming) {
        (void)esp_audio_simple_player_stop(s.player);
        s.streaming = false;
    }
}

/*
 * Follow redirects here, before the player sees the URL.
 *
 * GMF's HTTP reader follows a 302 by reopening on the same client without
 * draining the first response, and on the board the TLS connection to the
 * new host then dropped a second after its headers arrived. Every archive.org
 * speech redirects to a mirror, so every speech failed, while the same file
 * fetched straight from the mirror played. HEAD with redirects off, hop by
 * hop, and hand the player the final address. On any failure the original URL
 * is used, so this can only help.
 */
#define MAX_HOPS 5
static void resolve_redirects(const char *url, char *out, size_t cap)
{
    strlcpy(out, url, cap);
    for (int hop = 0; hop < MAX_HOPS; ++hop) {
        esp_http_client_config_t cfg = {
            .url = out,
            .method = HTTP_METHOD_HEAD,
            .disable_auto_redirect = true,
            .timeout_ms = 8000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            return;
        }
        const esp_err_t err = esp_http_client_perform(c);
        const int code = esp_http_client_get_status_code(c);
        bool moved = err == ESP_OK && (code == 301 || code == 302 || code == 303 ||
                                       code == 307 || code == 308);
        if (moved) {
            moved = esp_http_client_set_redirection(c) == ESP_OK &&
                    esp_http_client_get_url(c, out, (int)cap) == ESP_OK;
        }
        esp_http_client_cleanup(c);
        if (!moved) {
            return;
        }
        ESP_LOGI(TAG, "redirected to %s", out);
    }
}

static char s_resolved[URL_CAP];

static void stream_play(const char *url)
{
    stream_stop();
    s.state = DD_AUDIO_CONNECTING;
    /* Only files are resolved. A live stream answers HEAD slowly or not at
     * all, and none of the stations in dd_media redirect. */
    if (strncmp(url, "https://", 8) == 0 || strstr(url, "archive.org/")) {
        resolve_redirects(url, s_resolved, sizeof(s_resolved));
        url = s_resolved;
    }
    ESP_LOGI(TAG, "play %s", url);
    if (esp_audio_simple_player_run(s.player, url, NULL) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "run failed");
        s.state = DD_AUDIO_ERROR;
        return;
    }
    s.streaming = true;
}

/* ------------------------------------------------------------- chime */

/* Two soft bell notes, a fifth apart, with an exponential decay. Synthesised
 * rather than embedded: a few lines of code against a few kB of MP3. */
static void chime(void)
{
    if (s.streaming) {
        return; /* the music is already making the point */
    }
    codec_open(CHIME_RATE, 16, 1);
    static const float NOTES[] = { 880.0f, 1318.5f };
    int16_t *buf = heap_caps_malloc(CHIME_FRAME * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!buf) {
        return;
    }
    for (size_t n = 0; n < sizeof(NOTES) / sizeof(NOTES[0]); ++n) {
        const int total = CHIME_RATE * 9 / 10;
        float phase = 0.0f;
        const float step = 6.2831853f * NOTES[n] / CHIME_RATE;
        for (int i = 0; i < total; i += CHIME_FRAME) {
            for (int j = 0; j < CHIME_FRAME; ++j) {
                const float t = (float)(i + j) / CHIME_RATE;
                const float env = expf(-4.0f * t) * (t < 0.01f ? t / 0.01f : 1.0f);
                buf[j] = (int16_t)(9000.0f * env * (sinf(phase) + 0.3f * sinf(2.0f * phase)));
                phase += step;
            }
            if (dd_bt_streaming()) {
                dd_bt_write(buf, CHIME_FRAME, CHIME_RATE, 1, s.volume);
            } else {
                (void)esp_codec_dev_write(s.spk, buf, CHIME_FRAME * sizeof(int16_t));
            }
        }
    }
    free(buf);
}

/* ------------------------------------------------------------- task */

static void audio_task(void *arg)
{
    (void)arg;
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s.q, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (c.kind) {
        case CMD_PLAY:
            stream_play(c.url);
            break;
        case CMD_STOP:
            stream_stop();
            s.state = DD_AUDIO_IDLE;
            break;
        case CMD_CHIME:
            chime();
            break;
        case CMD_VOLUME:
            s.volume = c.value;
            (void)esp_codec_dev_set_out_vol(s.spk, s.volume);
            break;
        }
    }
}

static void post(cmd_kind_t kind, int value, const char *url)
{
    if (!s.q) {
        return;
    }
    cmd_t c = { .kind = kind, .value = value };
    if (url) {
        strlcpy(c.url, url, sizeof(c.url));
    }
    if (xQueueSend(s.q, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped command %d", kind);
    }
}

esp_err_t dd_audio_init(int volume)
{
    s.volume = volume;
    s.spk = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s.spk, ESP_FAIL, TAG, "speaker");
    codec_open(BSP_AUDIO_DEFAULT_SAMPLE_RATE, BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
               BSP_AUDIO_DEFAULT_CHANNELS);

    esp_asp_cfg_t cfg = {
        .out.cb = on_pcm,
        .task_prio = 5,
        .task_stack = 6144,
        .task_core = 0,
        .task_stack_in_ext = true,
    };
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_new(&cfg, &s.player) == ESP_GMF_ERR_OK,
                        ESP_FAIL, TAG, "player");
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_set_event(s.player, on_player_event, NULL)
                        == ESP_GMF_ERR_OK, ESP_FAIL, TAG, "player events");

    s.q = xQueueCreate(QUEUE_LEN, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s.q, ESP_ERR_NO_MEM, TAG, "queue");
    if (xTaskCreateWithCaps(audio_task, "dd_audio", TASK_STACK, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        vQueueDelete(s.q);
        s.q = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dd_audio_play(const char *url)
{
    s.state = DD_AUDIO_CONNECTING;
    post(CMD_PLAY, 0, url);
}

void dd_audio_stop(void)
{
    post(CMD_STOP, 0, NULL);
}

void dd_audio_chime(void)
{
    post(CMD_CHIME, 0, NULL);
}

void dd_audio_set_volume(int percent)
{
    post(CMD_VOLUME, percent, NULL);
}

dd_audio_state_t dd_audio_state(void)
{
    return s.state;
}
