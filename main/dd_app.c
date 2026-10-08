// SPDX-License-Identifier: Apache-2.0
//
// Dilly-dally's UI and behaviour. Everything here runs on the GSP render task,
// from the 50 ms tick, the call events and the page flow events, so the state
// below has a single owner and needs no locking.
//
// The face is what makes it feel alive, so most of the care is there. Moods
// are resolved every tick from what is going on, with a short lived override
// for reactions such as being poked. Blinking and the slow bob run on their
// own clocks so the face never sits perfectly still.

#include "dd_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bundle_gsp.h"
#include "dd_face.h"
#include "dd_faces_gen.h"
#include "dd_media.h"
#include "dd_platform.h"

#define TICK_MS              50U
#define BOB_PERIOD_MS        3200U
#define BOB_AMPLITUDE_PX     6
#define FACE_BOX_Y           56
#define BLINK_MS             140U
#define BLINK_GAP_MIN_MS     2200U
#define BLINK_GAP_SPAN_MS    4200U
#define POKE_JOY_MS          2500U
#define POKE_SPAM_COUNT      4U
#define POKE_SPAM_WINDOW_MS  3000U
#define BLUSH_MS             3500U
#define DONE_JOY_MS          10000U
#define ANGER_MS             5000U
#define DISGUST_MS           5000U
#define STOP_ANGRY_AFTER_MS  10000U
#define LAST_MINUTE_MS       60000U
#define PAUSE_FEAR_MS        120000U
#define WIFI_GRACE_MS        30000U
#define IDLE_ENNUI_MS        (10U * 60U * 1000U)
#define TOUCH_HOLD_MS        60000U
#define DAYDREAM_MIN_MS      60000U
#define DAYDREAM_SPAN_MS     60000U
#define DAYDREAM_MS          3000U
#define FOCUS_MIN_MIN        5
#define FOCUS_MAX_MIN        180
#define FOCUS_STEP_MIN       5
#define TZ_MIN_MIN           (-12 * 60)
#define TZ_MAX_MIN           (14 * 60)
#define TZ_STEP_MIN          30
#define RING_FULL            1000

#define COLOR_CARD           0x201F1DU
#define COLOR_CARD_HI        0x2B2926U
#define COLOR_INK            0x141413U
#define COLOR_ACCENT         0xFF4C01U

enum { PAGE_FACE, PAGE_CLOCK, PAGE_TIMER, PAGE_AUDIO, PAGE_SETTINGS, PAGE_COUNT };

static const uint16_t AUTO_SECONDS[] = { 0, 15, 30, 60, 300 };
static const char *const AUTO_NAMES[] = { "Off", "15 s", "30 s", "1 min", "5 min" };
#define AUTO_COUNT (sizeof(AUTO_SECONDS) / sizeof(AUTO_SECONDS[0]))

static const uint8_t PRESETS[] = { 15, 25, 45, 60 };
#define PRESET_COUNT (sizeof(PRESETS) / sizeof(PRESETS[0]))

static const char *const DAYS[] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY",
};
static const char *const MONTHS[] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};

typedef enum { TIMER_SET, TIMER_RUN, TIMER_PAUSED } timer_state_t;
typedef enum { ED_NONE, ED_WIFI, ED_TZ, ED_AUTO, ED_BRIGHT, ED_VOL } editor_t;

/* One place on screen that shows a face. */
typedef struct {
    const dd_face_img_t *imgs;
    size_t count;
    int shown;          /* index into imgs, -1 before the first update */
    bool blink_shown;
} face_set_t;

static struct {
    esp_gsp_handle_t ui;
    void *tick;
    bool drawn;
    esp_gsp_err_t last_error;
    int first_error_line;
    dd_settings_t cfg;

    unsigned page;
    uint32_t now;
    uint32_t boot_ms;
    uint32_t last_touch;
    uint32_t last_auto_flip;

    /* face */
    face_set_t sets[4];
    dd_mood_t override;
    uint32_t override_until;
    uint32_t next_blink;
    uint32_t blink_until;
    bool double_blink;
    uint32_t next_daydream;
    int bob_y;
    uint32_t poke_times[POKE_SPAM_COUNT];
    unsigned poke_head;

    /* clock, refreshed when the minute changes */
    int shown_minute;
    int shown_second;
    bool shown_valid;

    /* focus timer */
    timer_state_t timer;
    uint32_t total_ms;
    uint32_t remaining_ms;
    uint32_t run_from;       /* ms when the current run segment started */
    uint32_t paused_at;
    unsigned shown_seconds;

    /* audio */
    bool tab_speech;
    int idx[2];
    bool want_play;
    dd_audio_state_t audio;

    /* Bluetooth sheet */
    bool bt_open;
    uint32_t bt_next_draw;
    char bt_shown[DD_BT_MAX_FOUND * DD_BT_NAME_CAP + 64];

    /* settings editor */
    editor_t editor;
    bool wifi_shown_up;
} s;

/* ------------------------------------------------------------- helpers */

/* Records the first failure with its line, so a failed setter can be traced
 * from one log line on the board. */
#define ok(expr) ok_at((expr), __LINE__)
static void ok_at(esp_gsp_err_t err, int line)
{
    if (err != ESP_GSP_OK) {
        s.last_error = err;
        if (!s.first_error_line) {
            s.first_error_line = line;
        }
    }
}

static bool due(uint32_t t)
{
    return (int32_t)(s.now - t) >= 0;
}

static uint32_t rnd(uint32_t span)
{
    return span ? dd_platform_random() % span : 0;
}

static void touched(void)
{
    s.last_touch = s.now;
}

/* ---------------------------------------------------------------- faces */

static void set_init(face_set_t *set, const dd_face_img_t *imgs, size_t count)
{
    set->imgs = imgs;
    set->count = count;
    set->shown = -1;
    set->blink_shown = false;
}

static void set_show(face_set_t *set, dd_mood_t mood, bool blink)
{
    int want = 0; /* idle is always entry 0, the fallback for a missing mood */
    for (size_t i = 0; i < set->count; ++i) {
        if (set->imgs[i].mood == mood) {
            want = (int)i;
            break;
        }
    }
    if (want == set->shown && blink == set->blink_shown) {
        return;
    }
    /* Show the new image before hiding the old so no frame is empty. */
    const dd_face_img_t *n = &set->imgs[want];
    ok(esp_gsp_component_set_visible(s.ui, blink ? n->blink : n->open, true));
    if (set->shown >= 0) {
        const dd_face_img_t *o = &set->imgs[set->shown];
        const gsp_component_key_t old = set->blink_shown ? o->blink : o->open;
        if (old != (blink ? n->blink : n->open)) {
            ok(esp_gsp_component_set_visible(s.ui, old, false));
        }
    }
    set->shown = want;
    set->blink_shown = blink;
}

static void override_mood(dd_mood_t mood, uint32_t ms)
{
    s.override = mood;
    s.override_until = s.now + ms;
}

static dd_mood_t resolve_mood(void)
{
    if (s.override != DD_MOOD_IDLE && !due(s.override_until)) {
        return s.override;
    }
    s.override = DD_MOOD_IDLE;

    if (s.timer == TIMER_RUN && s.remaining_ms <= LAST_MINUTE_MS) {
        return DD_MOOD_ANXIOUS;
    }
    if (s.timer == TIMER_PAUSED && s.now - s.paused_at >= PAUSE_FEAR_MS) {
        return DD_MOOD_FEAR;
    }
    if (s.audio == DD_AUDIO_PLAYING) {
        return DD_MOOD_ENVY;
    }
    if (s.audio == DD_AUDIO_CONNECTING) {
        return DD_MOOD_FEAR;
    }
    if (s.now - s.boot_ms > WIFI_GRACE_MS && !dd_platform_wifi_up()) {
        return DD_MOOD_SAD;
    }
    if (s.now - s.last_touch >= IDLE_ENNUI_MS) {
        return DD_MOOD_ENNUI;
    }
    return DD_MOOD_IDLE;
}

/* Every minute or two of quiet, a passing expression, so it never looks
 * frozen while nobody is touching it. */
static void daydream(void)
{
    if (!due(s.next_daydream)) {
        return;
    }
    s.next_daydream = s.now + DAYDREAM_MIN_MS + rnd(DAYDREAM_SPAN_MS);
    if (s.override != DD_MOOD_IDLE || s.timer != TIMER_SET ||
        s.audio != DD_AUDIO_IDLE) {
        return;
    }
    static const dd_mood_t PASSING[] = { DD_MOOD_JOY, DD_MOOD_ENNUI, DD_MOOD_ENVY };
    override_mood(PASSING[rnd(sizeof(PASSING) / sizeof(PASSING[0]))], DAYDREAM_MS);
}

static void blink_step(void)
{
    if (due(s.next_blink)) {
        s.blink_until = s.now + BLINK_MS;
        if (s.double_blink) {
            s.double_blink = false;
            s.next_blink = s.now + 2 * BLINK_MS + BLINK_GAP_MIN_MS + rnd(BLINK_GAP_SPAN_MS);
        } else {
            /* one in five is a double blink */
            s.double_blink = rnd(5) == 0;
            s.next_blink = s.now + (s.double_blink ? 3 * BLINK_MS
                                    : BLINK_GAP_MIN_MS + rnd(BLINK_GAP_SPAN_MS));
        }
    }
}

static void faces_step(void)
{
    daydream();
    blink_step();
    const dd_mood_t mood = resolve_mood();
    const bool blink = !due(s.blink_until);
    for (size_t i = 0; i < sizeof(s.sets) / sizeof(s.sets[0]); ++i) {
        set_show(&s.sets[i], mood, blink);
    }

    /* The slow bob, only while the big face is on screen. */
    if (s.page == PAGE_FACE) {
        const float phase = (float)(s.now % BOB_PERIOD_MS) / (float)BOB_PERIOD_MS;
        const int y = FACE_BOX_Y + (int)lroundf(BOB_AMPLITUDE_PX * sinf(phase * 6.2831853f));
        if (y != s.bob_y) {
            s.bob_y = y;
            ok(gsp_dd_face_box_set_y(s.ui, y));
        }
    }
}

static void poke(void)
{
    s.poke_times[s.poke_head] = s.now;
    s.poke_head = (s.poke_head + 1) % POKE_SPAM_COUNT;
    /* poke_head now points at the oldest of the last few pokes */
    const uint32_t oldest = s.poke_times[s.poke_head];
    if (oldest && s.now - oldest <= POKE_SPAM_WINDOW_MS) {
        override_mood(DD_MOOD_EMBARRASSED, BLUSH_MS);
        memset(s.poke_times, 0, sizeof(s.poke_times));
    } else if (s.override != DD_MOOD_EMBARRASSED) {
        override_mood(DD_MOOD_JOY, POKE_JOY_MS);
    }
    s.blink_until = s.now + BLINK_MS;
}

/* ---------------------------------------------------------------- clock */

/* Seconds to calendar fields without gmtime_r, which the Windows simulator
 * toolchain does not have. Days to civil date after Howard Hinnant. */
static void local_time(struct tm *out, bool *valid)
{
    const int64_t t = (int64_t)dd_platform_utc(valid) + (int64_t)s.cfg.tz_min * 60;
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    memset(out, 0, sizeof(*out));
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    out->tm_mday = (int)d;
    out->tm_mon = (int)(m - 1);
    out->tm_year = (int)(yoe + era * 400 + (m <= 2) - 1900);
}

/* seg is for the seven segment clock, where "!" is DSEG's blank digit and
 * keeps "9:05" the same width as "12:05". plain is for the status bar. */
static void format_hm(const struct tm *tm, char *seg, char *plain, size_t cap,
                      const char **ampm)
{
    if (s.cfg.h24) {
        snprintf(seg, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
        snprintf(plain, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
        *ampm = " ";
    } else {
        const int h = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        snprintf(seg, cap, h < 10 ? "!%d:%02d" : "%d:%02d", h, tm->tm_min);
        snprintf(plain, cap, "%d:%02d", h, tm->tm_min);
        *ampm = tm->tm_hour < 12 ? "AM" : "PM";
    }
}

static void clock_step(bool force)
{
    bool valid = false;
    struct tm tm;
    local_time(&tm, &valid);
    if (valid && tm.tm_sec != s.shown_second) {
        s.shown_second = tm.tm_sec;
        ok(gsp_dd_clock_sec_set_value(s.ui, tm.tm_sec * 100 / 59));
    }
    const int minute = tm.tm_hour * 60 + tm.tm_min;
    if (!force && minute == s.shown_minute && valid == s.shown_valid) {
        return;
    }
    s.shown_minute = minute;
    s.shown_valid = valid;

    if (!valid) {
        ok(gsp_dd_clock_time_set_text(s.ui, "--:--"));
        /* Blank labels get a space. The board drew an empty day as "?" and
         * refused an empty status time with a timeout. */
        ok(gsp_dd_clock_ampm_set_text(s.ui, " "));
        ok(gsp_dd_clock_day_set_text(s.ui, " "));
        ok(gsp_dd_clock_date_set_text(s.ui, "Waiting for the time"));
        ok(gsp_dd_status_time_set_text(s.ui, " "));
        ok(gsp_dd_clock_sec_set_value(s.ui, 0));
        return;
    }
    char seg[12];
    char hm[12];
    char line[48];
    const char *ampm;
    format_hm(&tm, seg, hm, sizeof(hm), &ampm);
    ok(gsp_dd_clock_time_set_text(s.ui, seg));
    ok(gsp_dd_clock_ampm_set_text(s.ui, ampm));
    ok(gsp_dd_clock_day_set_text(s.ui, DAYS[tm.tm_wday]));
    snprintf(line, sizeof(line), "%d %s %d", tm.tm_mday, MONTHS[tm.tm_mon],
             tm.tm_year + 1900);
    ok(gsp_dd_clock_date_set_text(s.ui, line));
    snprintf(line, sizeof(line), "%s%s%s", hm, ampm[0] != ' ' ? " " : "", ampm);
    ok(gsp_dd_status_time_set_text(s.ui, line));
}

static void wifi_icon_step(bool force)
{
    const bool up = dd_platform_wifi_up();
    if (force || up != s.wifi_shown_up) {
        s.wifi_shown_up = up;
        ok(gsp_dd_status_wifi_set_visible(s.ui, up));
        ok(gsp_dd_status_wifi_off_set_visible(s.ui, !up));
    }
}

/* ---------------------------------------------------------------- pages */

static void page_changed(unsigned page)
{
    if (page >= PAGE_COUNT) {
        return;
    }
    s.page = page;
    /* The clock is its own status bar, and settings has a title instead. */
    ok(gsp_dd_status_set_visible(s.ui, page != PAGE_CLOCK && page != PAGE_SETTINGS));
    /* The press shade is one switch for the whole UI. On the face page the
     * only control is the invisible poke target over the face, and its shade
     * drew a dark disc across the face on every touch. */
    ok(esp_gsp_set_press_feedback_enabled(s.ui, page != PAGE_FACE));
}

static void go(unsigned page)
{
    ok(esp_gsp_page_flow_set_page(s.ui, GSP_DD_OBJ_KEY_PAGES, (uint16_t)page, true));
}

/* Face and clock take turns when nobody is using the board. */
static void auto_step(void)
{
    const uint32_t every = AUTO_SECONDS[s.cfg.auto_idx] * 1000U;
    if (!every || (s.page != PAGE_FACE && s.page != PAGE_CLOCK) ||
        s.timer == TIMER_RUN || s.editor != ED_NONE || s.bt_open) {
        s.last_auto_flip = s.now;
        return;
    }
    if (s.now - s.last_touch < TOUCH_HOLD_MS || s.now - s.last_auto_flip < every) {
        return;
    }
    s.last_auto_flip = s.now;
    go(s.page == PAGE_FACE ? PAGE_CLOCK : PAGE_FACE);
}

/* ---------------------------------------------------------------- timer */

static void show_set_time(void)
{
    char t[12];
    snprintf(t, sizeof(t), "%d:00", s.cfg.focus_min);
    ok(gsp_dd_t_set_time_set_text(s.ui, t));
    static esp_err_t (*const BG[PRESET_COUNT])(esp_gsp_handle_t, uint32_t) = {
        gsp_dd_preset0_set_color_rgb888, gsp_dd_preset1_set_color_rgb888,
        gsp_dd_preset2_set_color_rgb888, gsp_dd_preset3_set_color_rgb888,
    };
    for (size_t i = 0; i < PRESET_COUNT; ++i) {
        ok(BG[i](s.ui, PRESETS[i] == s.cfg.focus_min ? COLOR_ACCENT : COLOR_CARD));
    }
}

static void timer_view(bool running)
{
    ok(gsp_dd_t_set_set_visible(s.ui, !running));
    ok(gsp_dd_t_run_set_visible(s.ui, running));
}

static void timer_draw(void)
{
    const unsigned secs = (s.remaining_ms + 999U) / 1000U;
    if (secs == s.shown_seconds) {
        return;
    }
    s.shown_seconds = secs;
    char t[12];
    snprintf(t, sizeof(t), "%02u:%02u", secs / 60U, secs % 60U);
    ok(gsp_dd_t_run_time_set_text(s.ui, t));
    const uint32_t ring = s.total_ms
        ? (uint32_t)((uint64_t)s.remaining_ms * RING_FULL / s.total_ms) : 0;
    ok(gsp_dd_t_ring_set_value(s.ui, (int32_t)ring));
}

static void timer_start(void)
{
    s.total_ms = (uint32_t)s.cfg.focus_min * 60000U;
    s.remaining_ms = s.total_ms;
    s.run_from = s.now;
    s.timer = TIMER_RUN;
    s.shown_seconds = UINT32_MAX;
    char sub[32];
    snprintf(sub, sizeof(sub), "of %d min focus", s.cfg.focus_min);
    ok(gsp_dd_t_run_sub_set_text(s.ui, sub));
    ok(gsp_dd_t_pause_text_set_text(s.ui, "Pause"));
    timer_draw();
    timer_view(true);
    dd_platform_settings_save(&s.cfg); /* remembers the length */
}

static void timer_pause_toggle(void)
{
    if (s.timer == TIMER_RUN) {
        s.timer = TIMER_PAUSED;
        s.paused_at = s.now;
        ok(gsp_dd_t_pause_text_set_text(s.ui, "Resume"));
        ok(gsp_dd_t_run_sub_set_text(s.ui, "Paused"));
    } else if (s.timer == TIMER_PAUSED) {
        s.timer = TIMER_RUN;
        s.run_from = s.now;
        char sub[32];
        snprintf(sub, sizeof(sub), "of %d min focus", s.cfg.focus_min);
        ok(gsp_dd_t_pause_text_set_text(s.ui, "Pause"));
        ok(gsp_dd_t_run_sub_set_text(s.ui, sub));
    }
}

static void timer_end(bool finished)
{
    const uint32_t done_ms = s.total_ms - s.remaining_ms;
    s.timer = TIMER_SET;
    timer_view(false);
    if (finished) {
        override_mood(DD_MOOD_JOY, DONE_JOY_MS);
        if (s.cfg.chime) {
            dd_platform_chime();
        }
        go(PAGE_FACE);
    } else if (done_ms >= STOP_ANGRY_AFTER_MS) {
        override_mood(DD_MOOD_ANGER, ANGER_MS);
    }
}

static void timer_step(void)
{
    if (s.timer != TIMER_RUN) {
        return;
    }
    const uint32_t ran = s.now - s.run_from;
    s.run_from = s.now;
    s.remaining_ms = ran >= s.remaining_ms ? 0 : s.remaining_ms - ran;
    timer_draw();
    if (s.remaining_ms == 0) {
        timer_end(true);
    }
}

static void focus_adjust(int delta)
{
    int m = s.cfg.focus_min + delta;
    if (m < FOCUS_MIN_MIN) {
        m = FOCUS_MIN_MIN;
    } else if (m > FOCUS_MAX_MIN) {
        m = FOCUS_MAX_MIN;
    }
    s.cfg.focus_min = (uint8_t)m;
    show_set_time();
}

/* ---------------------------------------------------------------- audio */

static int media_count(bool speech)
{
    int n = 0;
    for (int i = 0; i < dd_media_count; ++i) {
        n += (dd_media[i].kind == DD_MEDIA_SPEECH) == speech;
    }
    return n;
}

static const dd_media_t *media_at(bool speech, int idx)
{
    for (int i = 0; i < dd_media_count; ++i) {
        if ((dd_media[i].kind == DD_MEDIA_SPEECH) == speech && idx-- == 0) {
            return &dd_media[i];
        }
    }
    return NULL;
}

static void audio_draw(void)
{
    const bool sp = s.tab_speech;
    const dd_media_t *m = media_at(sp, s.idx[sp]);
    ok(gsp_dd_a_title_set_text(s.ui, m ? m->name : " "));
    char sub[48];
    if (s.audio == DD_AUDIO_CONNECTING) {
        snprintf(sub, sizeof(sub), "Tuning in...");
    } else if (s.audio == DD_AUDIO_ERROR) {
        snprintf(sub, sizeof(sub), "Could not play this one");
    } else {
        snprintf(sub, sizeof(sub), "%s  %d of %d", sp ? "Speech" : "Radio",
                 s.idx[sp] + 1, media_count(sp));
    }
    ok(gsp_dd_a_sub_set_text(s.ui, sub));
    const bool busy = s.audio == DD_AUDIO_PLAYING || s.audio == DD_AUDIO_CONNECTING;
    ok(gsp_dd_a_icon_play_set_visible(s.ui, !busy));
    ok(gsp_dd_a_icon_pause_set_visible(s.ui, busy));
    ok(gsp_dd_tab_radio_set_color_rgb888(s.ui, sp ? COLOR_INK : COLOR_CARD_HI));
    ok(gsp_dd_tab_speech_set_color_rgb888(s.ui, sp ? COLOR_CARD_HI : COLOR_INK));
}

static void audio_play_current(void)
{
    const dd_media_t *m = media_at(s.tab_speech, s.idx[s.tab_speech]);
    if (m) {
        s.want_play = true;
        dd_platform_audio_play(m->url);
    }
}

static void audio_step_idx(int delta)
{
    const bool sp = s.tab_speech;
    const int n = media_count(sp);
    if (n == 0) {
        return;
    }
    s.idx[sp] = (s.idx[sp] + delta + n) % n;
    if (s.want_play) {
        audio_play_current();
    }
    audio_draw();
}

static void audio_tab(bool speech)
{
    if (speech == s.tab_speech) {
        return;
    }
    s.tab_speech = speech;
    if (s.want_play) {
        s.want_play = false;
        dd_platform_audio_stop();
    }
    audio_draw();
}

static void audio_step(void)
{
    const dd_audio_state_t st = dd_platform_audio_state();
    if (st == s.audio) {
        return;
    }
    if (st == DD_AUDIO_ERROR) {
        override_mood(DD_MOOD_DISGUST, DISGUST_MS);
        s.want_play = false;
    }
    if (st == DD_AUDIO_IDLE && s.audio == DD_AUDIO_PLAYING && s.want_play &&
        s.tab_speech) {
        /* A speech that ran to the end. */
        s.want_play = false;
    }
    s.audio = st;
    audio_draw();
}

/* ------------------------------------------------------------- settings */

static void tz_text(int tz, char *out, size_t cap)
{
    if (tz == 0) {
        snprintf(out, cap, "UTC");
        return;
    }
    const int a = tz < 0 ? -tz : tz;
    if (a % 60) {
        snprintf(out, cap, "UTC%c%d:%02d", tz < 0 ? '-' : '+', a / 60, a % 60);
    } else {
        snprintf(out, cap, "UTC%c%d", tz < 0 ? '-' : '+', a / 60);
    }
}

static void bt_value(char *out, size_t cap)
{
    char name[DD_BT_NAME_CAP];
    switch (dd_platform_bt_state()) {
    case DD_BT_CONNECTED:
        dd_platform_bt_name(name, sizeof(name));
        snprintf(out, cap, "%s", name[0] ? name : "Connected");
        break;
    case DD_BT_CONNECTING:
        snprintf(out, cap, "Connecting...");
        break;
    case DD_BT_SEARCHING:
        snprintf(out, cap, "Searching...");
        break;
    default:
        snprintf(out, cap, "Off");
        break;
    }
}

static void settings_draw(void)
{
    char v[40];
    dd_platform_wifi_name(v, sizeof(v));
    ok(gsp_dd_val_wifi_set_text(s.ui, v[0] ? v : "Not set"));
    bt_value(v, sizeof(v));
    ok(gsp_dd_val_bt_set_text(s.ui, v));
    tz_text(s.cfg.tz_min, v, sizeof(v));
    ok(gsp_dd_val_tz_set_text(s.ui, v));
    ok(gsp_dd_val_auto_set_text(s.ui, AUTO_NAMES[s.cfg.auto_idx]));
    snprintf(v, sizeof(v), "%u%%", s.cfg.brightness);
    ok(gsp_dd_val_bright_set_text(s.ui, v));
    snprintf(v, sizeof(v), "%u%%", s.cfg.volume);
    ok(gsp_dd_val_vol_set_text(s.ui, v));
}

static void editor_draw(void)
{
    char v[24];
    switch (s.editor) {
    case ED_TZ:     tz_text(s.cfg.tz_min, v, sizeof(v)); break;
    case ED_AUTO:   snprintf(v, sizeof(v), "%s", AUTO_NAMES[s.cfg.auto_idx]); break;
    case ED_BRIGHT: snprintf(v, sizeof(v), "%u%%", s.cfg.brightness); break;
    case ED_VOL:    snprintf(v, sizeof(v), "%u%%", s.cfg.volume); break;
    default:        return;
    }
    ok(gsp_dd_ed_value_set_text(s.ui, v));
}

static void editor_open(editor_t which)
{
    static const char *const TITLES[] = {
        "", "Wi-Fi setup", "Time zone", "Auto change page", "Brightness", "Volume",
    };
    s.editor = which;
    ok(gsp_dd_ed_title_set_text(s.ui, TITLES[which]));
    const bool info = which == ED_WIFI;
    ok(gsp_dd_ed_adjust_set_visible(s.ui, !info));
    ok(gsp_dd_ed_info_set_visible(s.ui, info));
    if (info) {
        char lines[3][48] = { { 0 } };
        if (!dd_platform_setup_start(lines)) {
            snprintf(lines[0], sizeof(lines[0]), "Setup network failed");
            snprintf(lines[1], sizeof(lines[1]), "Try again in a moment");
        }
        ok(gsp_dd_ed_line0_set_text(s.ui, lines[0]));
        ok(gsp_dd_ed_line1_set_text(s.ui, lines[1]));
        ok(gsp_dd_ed_line2_set_text(s.ui, lines[2]));
    } else {
        editor_draw();
    }
    /* Keep a drag on the sheet from moving the pages underneath. */
    ok(esp_gsp_component_set_enabled(s.ui, GSP_DD_OBJ_KEY_PAGES, false));
    ok(gsp_dd_editor_set_visible(s.ui, true));
}

static void editor_close(void)
{
    if (s.editor == ED_WIFI) {
        dd_platform_setup_stop();
    }
    s.editor = ED_NONE;
    ok(gsp_dd_editor_set_visible(s.ui, false));
    ok(esp_gsp_component_set_enabled(s.ui, GSP_DD_OBJ_KEY_PAGES, true));
    dd_platform_settings_save(&s.cfg);
    settings_draw();
    clock_step(true);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void editor_step(int dir)
{
    switch (s.editor) {
    case ED_TZ:
        s.cfg.tz_min = (int16_t)clampi(s.cfg.tz_min + dir * TZ_STEP_MIN,
                                       TZ_MIN_MIN, TZ_MAX_MIN);
        break;
    case ED_AUTO:
        s.cfg.auto_idx = (uint8_t)clampi(s.cfg.auto_idx + dir, 0, AUTO_COUNT - 1);
        break;
    case ED_BRIGHT:
        s.cfg.brightness = (uint8_t)clampi(s.cfg.brightness + dir * 10, 10, 100);
        dd_platform_set_brightness(s.cfg.brightness);
        break;
    case ED_VOL:
        s.cfg.volume = (uint8_t)clampi(s.cfg.volume + dir * 10, 0, 100);
        dd_platform_set_volume(s.cfg.volume);
        break;
    default:
        return;
    }
    editor_draw();
}

/* set_checked moves the knob but leaves the track in its authored colour,
 * so an off toggle authored on stayed orange until it was tapped twice. The
 * track is set explicitly every time the state is. */
#define TRACK_ON  COLOR_ACCENT
#define TRACK_OFF 0x3A3835U

static void show_toggles(void)
{
    ok(gsp_dd_tg_h24_set_checked(s.ui, s.cfg.h24));
    ok(gsp_dd_tg_h24_set_track_color_rgb888(s.ui, s.cfg.h24 ? TRACK_ON : TRACK_OFF));
    ok(gsp_dd_tg_chime_set_checked(s.ui, s.cfg.chime));
    ok(gsp_dd_tg_chime_set_track_color_rgb888(s.ui, s.cfg.chime ? TRACK_ON : TRACK_OFF));
}

/* ------------------------------------------------------------- Bluetooth */

#define BT_DRAW_MS 300U

static esp_err_t (*const BT_ITEM[DD_BT_MAX_FOUND])(esp_gsp_handle_t, bool) = {
    gsp_dd_bt_item0_set_visible, gsp_dd_bt_item1_set_visible,
    gsp_dd_bt_item2_set_visible, gsp_dd_bt_item3_set_visible,
};
static esp_err_t (*const BT_NAME[DD_BT_MAX_FOUND])(esp_gsp_handle_t, const char *) = {
    gsp_dd_bt_name0_set_text, gsp_dd_bt_name1_set_text,
    gsp_dd_bt_name2_set_text, gsp_dd_bt_name3_set_text,
};
static esp_err_t (*const BT_TAG[DD_BT_MAX_FOUND])(esp_gsp_handle_t, const char *) = {
    gsp_dd_bt_tag0_set_text, gsp_dd_bt_tag1_set_text,
    gsp_dd_bt_tag2_set_text, gsp_dd_bt_tag3_set_text,
};

/* Polled while the sheet is open. Redraws only when something changed. */
static void bt_draw(bool force)
{
    char found[DD_BT_MAX_FOUND][DD_BT_NAME_CAP] = { { 0 } };
    const int n = dd_platform_bt_found(found);
    const dd_bt_state_t st = dd_platform_bt_state();
    char current[DD_BT_NAME_CAP];
    dd_platform_bt_name(current, sizeof(current));

    char sig[sizeof(s.bt_shown)];
    int len = snprintf(sig, sizeof(sig), "%d|%d|%s", (int)st, n, current);
    for (int i = 0; i < n && len > 0 && len < (int)sizeof(sig); ++i) {
        len += snprintf(sig + len, sizeof(sig) - (size_t)len, "|%s", found[i]);
    }
    if (!force && strcmp(sig, s.bt_shown) == 0) {
        return;
    }
    snprintf(s.bt_shown, sizeof(s.bt_shown), "%s", sig);

    char line[64];
    switch (st) {
    case DD_BT_SEARCHING:
        snprintf(line, sizeof(line), "Searching. Put your speaker in pairing mode");
        break;
    case DD_BT_CONNECTING:
        snprintf(line, sizeof(line), "Connecting to %s", current);
        break;
    case DD_BT_CONNECTED:
        snprintf(line, sizeof(line), "Playing through %s", current);
        break;
    default:
        snprintf(line, sizeof(line), n ? "Tap a device to connect"
                                       : "Tap Search to find speakers");
        break;
    }
    ok(gsp_dd_bt_status_set_text(s.ui, line));
    for (int i = 0; i < DD_BT_MAX_FOUND; ++i) {
        const bool show = i < n;
        ok(BT_ITEM[i](s.ui, show));
        if (show) {
            ok(BT_NAME[i](s.ui, found[i]));
            const bool here = strcmp(found[i], current) == 0;
            ok(BT_TAG[i](s.ui, here && st == DD_BT_CONNECTED ? "Connected"
                               : here && st == DD_BT_CONNECTING ? "..." : " "));
        }
    }
    ok(gsp_dd_bt_scan_text_set_text(s.ui, st == DD_BT_CONNECTED ? "Disconnect"
                                          : st == DD_BT_SEARCHING ? "Searching" : "Search"));
}

static void bt_open(void)
{
    s.bt_open = true;
    s.bt_shown[0] = '\0';
    ok(esp_gsp_component_set_enabled(s.ui, GSP_DD_OBJ_KEY_PAGES, false));
    ok(gsp_dd_bt_sheet_set_visible(s.ui, true));
    char found[DD_BT_MAX_FOUND][DD_BT_NAME_CAP];
    if (dd_platform_bt_state() == DD_BT_OFF && dd_platform_bt_found(found) == 0) {
        dd_platform_bt_search(); /* nothing to show yet, so look */
    }
    bt_draw(true);
}

static void bt_close(void)
{
    s.bt_open = false;
    ok(gsp_dd_bt_sheet_set_visible(s.ui, false));
    ok(esp_gsp_component_set_enabled(s.ui, GSP_DD_OBJ_KEY_PAGES, true));
    settings_draw();
}

static void bt_step(void)
{
    if (s.bt_open && due(s.bt_next_draw)) {
        s.bt_next_draw = s.now + BT_DRAW_MS;
        bt_draw(false);
    }
}

/* ---------------------------------------------------------------- events */

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *e, void *ctx)
{
    (void)ui;
    (void)ctx;
    if (e->type != ESP_GSP_EVENT_CALL) {
        return;
    }
    touched();

    if (gsp_dd_event_is_poke(e)) {
        poke();
    } else if (gsp_dd_event_is_preset0(e)) {
        s.cfg.focus_min = PRESETS[0], show_set_time();
    } else if (gsp_dd_event_is_preset1(e)) {
        s.cfg.focus_min = PRESETS[1], show_set_time();
    } else if (gsp_dd_event_is_preset2(e)) {
        s.cfg.focus_min = PRESETS[2], show_set_time();
    } else if (gsp_dd_event_is_preset3(e)) {
        s.cfg.focus_min = PRESETS[3], show_set_time();
    } else if (gsp_dd_event_is_t_minus(e)) {
        focus_adjust(-FOCUS_STEP_MIN);
    } else if (gsp_dd_event_is_t_plus(e)) {
        focus_adjust(FOCUS_STEP_MIN);
    } else if (gsp_dd_event_is_t_start(e)) {
        timer_start();
    } else if (gsp_dd_event_is_t_pause(e)) {
        timer_pause_toggle();
    } else if (gsp_dd_event_is_t_stop(e)) {
        timer_end(false);
    } else if (gsp_dd_event_is_tab_radio(e)) {
        audio_tab(false);
    } else if (gsp_dd_event_is_tab_speech(e)) {
        audio_tab(true);
    } else if (gsp_dd_event_is_a_prev(e)) {
        audio_step_idx(-1);
    } else if (gsp_dd_event_is_a_next(e)) {
        audio_step_idx(1);
    } else if (gsp_dd_event_is_a_play(e)) {
        if (s.audio == DD_AUDIO_PLAYING || s.audio == DD_AUDIO_CONNECTING) {
            s.want_play = false;
            dd_platform_audio_stop();
        } else {
            audio_play_current();
        }
    } else if (gsp_dd_event_is_row_wifi(e)) {
        editor_open(ED_WIFI);
    } else if (gsp_dd_event_is_row_bt(e)) {
        bt_open();
    } else if (gsp_dd_event_is_bt_done(e)) {
        bt_close();
    } else if (gsp_dd_event_is_bt_scan(e)) {
        if (dd_platform_bt_state() == DD_BT_CONNECTED) {
            dd_platform_bt_disconnect();
        } else if (dd_platform_bt_state() != DD_BT_SEARCHING) {
            dd_platform_bt_search();
        }
        bt_draw(true);
    } else if (gsp_dd_event_is_bt_row0(e)) {
        dd_platform_bt_connect(0);
    } else if (gsp_dd_event_is_bt_row1(e)) {
        dd_platform_bt_connect(1);
    } else if (gsp_dd_event_is_bt_row2(e)) {
        dd_platform_bt_connect(2);
    } else if (gsp_dd_event_is_bt_row3(e)) {
        dd_platform_bt_connect(3);
    } else if (gsp_dd_event_is_row_tz(e)) {
        editor_open(ED_TZ);
    } else if (gsp_dd_event_is_row_auto(e)) {
        editor_open(ED_AUTO);
    } else if (gsp_dd_event_is_row_bright(e)) {
        editor_open(ED_BRIGHT);
    } else if (gsp_dd_event_is_row_vol(e)) {
        editor_open(ED_VOL);
    } else if (gsp_dd_event_is_ed_minus(e)) {
        editor_step(-1);
    } else if (gsp_dd_event_is_ed_plus(e)) {
        editor_step(1);
    } else if (gsp_dd_event_is_ed_done(e)) {
        editor_close();
    } else if (gsp_dd_event_is_tg_h24(e)) {
        /* Flip our own copy and push it back. Reading the toggle here returns
         * its state from before the tap, because setters are asynchronous. */
        s.cfg.h24 = !s.cfg.h24;
        show_toggles();
        dd_platform_settings_save(&s.cfg);
        clock_step(true);
    } else if (gsp_dd_event_is_tg_chime(e)) {
        s.cfg.chime = !s.cfg.chime;
        show_toggles();
        dd_platform_settings_save(&s.cfg);
    }
}

static void on_component(esp_gsp_handle_t ui, const esp_gsp_component_event_t *e,
                         void *ctx)
{
    (void)ui;
    (void)ctx;
    if (e->key == GSP_DD_OBJ_KEY_PAGES &&
        (e->flags & (ESP_GSP_COMPONENT_EVENT_VALUE_CHANGED |
                     ESP_GSP_COMPONENT_EVENT_MOTION_FINISHED))) {
        if (e->state.value != s.page) {
            touched();
            s.last_auto_flip = s.now;
        }
        page_changed(e->state.value);
    }
}

/* The first screen state, drawn from the first tick. Setters made on the
 * render task apply in place; the same forty made from app_main before the
 * runtime drained its queue overflowed it and one of them timed out. */
static void initial_draw(void)
{
    show_toggles();
    show_set_time();
    timer_view(false);
    settings_draw();
    audio_draw();
    clock_step(true);
    wifi_icon_step(true);
    page_changed(PAGE_FACE);
}

static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    (void)ctx;
    s.now = dd_platform_ms();
    if (!s.drawn) {
        s.drawn = true;
        initial_draw();
    }
    timer_step();
    audio_step();
    faces_step();
    clock_step(false);
    wifi_icon_step(false);
    bt_step();
    if (s.page == PAGE_SETTINGS && !s.bt_open && due(s.bt_next_draw)) {
        /* the Bluetooth row follows a connection made or lost in the background */
        s.bt_next_draw = s.now + 1000U;
        char v[40];
        bt_value(v, sizeof(v));
        ok(gsp_dd_val_bt_set_text(s.ui, v));
    }
    auto_step();
}

/* ------------------------------------------------------------ lifecycle */

esp_gsp_err_t dd_app_init(esp_gsp_handle_t ui)
{
    if (!ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof(s));
    s.ui = ui;
    s.now = dd_platform_ms();
    s.boot_ms = s.now;
    s.last_touch = s.now;
    s.last_auto_flip = s.now;
    s.next_blink = s.now + BLINK_GAP_MIN_MS;
    s.next_daydream = s.now + DAYDREAM_MIN_MS;
    s.shown_minute = -1;
    s.shown_second = -1;
    s.bob_y = FACE_BOX_Y;
    s.audio = DD_AUDIO_IDLE;

    dd_platform_settings_load(&s.cfg);
    if (s.cfg.auto_idx >= AUTO_COUNT) {
        s.cfg.auto_idx = 0;
    }
    s.cfg.focus_min = (uint8_t)clampi(s.cfg.focus_min, FOCUS_MIN_MIN, FOCUS_MAX_MIN);
    dd_platform_set_brightness(s.cfg.brightness);
    dd_platform_set_volume(s.cfg.volume);

    set_init(&s.sets[0], dd_faces_l, sizeof(dd_faces_l) / sizeof(dd_faces_l[0]));
    set_init(&s.sets[1], dd_faces_c, sizeof(dd_faces_c) / sizeof(dd_faces_c[0]));
    set_init(&s.sets[2], dd_faces_t, sizeof(dd_faces_t) / sizeof(dd_faces_t[0]));
    set_init(&s.sets[3], dd_faces_a, sizeof(dd_faces_a) / sizeof(dd_faces_a[0]));

    ok(esp_gsp_on_event(ui, on_event, NULL));
    ok(esp_gsp_on_component_event(ui, on_component, NULL));
    s.tick = esp_gsp_timer_create(ui, TICK_MS, tick, NULL);
    if (!s.tick) {
        ok(ESP_GSP_ERR_NO_MEM);
    }
    return s.last_error;
}

void dd_app_deinit(esp_gsp_handle_t ui)
{
    if (!ui) {
        return;
    }
    ok(esp_gsp_on_event(ui, NULL, NULL));
    ok(esp_gsp_on_component_event(ui, NULL, NULL));
    if (s.tick) {
        ok(esp_gsp_timer_delete(ui, s.tick));
        s.tick = NULL;
    }
    dd_platform_audio_stop();
}

esp_gsp_err_t dd_app_goto(esp_gsp_handle_t ui, unsigned page)
{
    if (!ui || page >= PAGE_COUNT) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    return esp_gsp_page_flow_set_page(ui, GSP_DD_OBJ_KEY_PAGES, (uint16_t)page, true);
}

esp_gsp_err_t dd_app_last_error(int *out_line)
{
    if (out_line) {
        *out_line = s.first_error_line;
    }
    return s.last_error;
}
