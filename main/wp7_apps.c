#include "wp7_apps.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

static const char *const s_titles[WP7_APP_COUNT] = {
    "AI Usage", "Clock", "Battery", "Stopwatch", "Focus timer",
};
static const uint32_t s_focus_minutes[] = {15, 25, 45, 5};

static lv_obj_t *s_panel;
static lv_obj_t *s_title;
static lv_obj_t *s_value;
static lv_obj_t *s_detail;
static lv_obj_t *s_hint;
static lv_obj_t *s_status_time;
static lv_timer_t *s_timer;
static wp7_app_id_t s_app;
static bool s_clock_known;
static uint64_t s_clock_base_s;
static uint64_t s_clock_base_ms;
static int16_t s_clock_timezone;
static int s_battery_soc = -2;
static int s_battery_mv = -2;
static uint64_t s_stopwatch_base_ms;
static uint64_t s_stopwatch_start_ms;
static uint64_t s_stopwatch_lap_ms;
static uint32_t s_stopwatch_laps;
static bool s_stopwatch_running;
static uint32_t s_focus_preset = 1;
static uint64_t s_focus_left_ms = 25 * 60000ULL;
static uint64_t s_focus_start_ms;
static bool s_focus_running;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void set_text_if_changed(lv_obj_t *label, const char *value)
{
    if (label && strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

static uint64_t clock_local_seconds(uint64_t now)
{
    const uint64_t elapsed = now >= s_clock_base_ms ? (now - s_clock_base_ms) / 1000 : 0;
    const int64_t local = (int64_t)(s_clock_base_s + elapsed) + (int64_t)s_clock_timezone * 60;
    return (uint64_t)((local % 86400 + 86400) % 86400);
}

static void format_clock(char *out, size_t size, uint64_t now, bool seconds)
{
    if (!s_clock_known) {
        snprintf(out, size, seconds ? "--:--:--" : "--:--");
        return;
    }
    const uint64_t local = clock_local_seconds(now);
    if (seconds) {
        snprintf(out, size, "%02u:%02u:%02u", (unsigned)(local / 3600),
                 (unsigned)(local / 60 % 60), (unsigned)(local % 60));
    } else {
        snprintf(out, size, "%02u:%02u", (unsigned)(local / 3600),
                 (unsigned)(local / 60 % 60));
    }
}

static uint64_t stopwatch_elapsed(uint64_t now)
{
    return s_stopwatch_base_ms +
           (s_stopwatch_running && now >= s_stopwatch_start_ms ? now - s_stopwatch_start_ms : 0);
}

static uint64_t focus_remaining(uint64_t now)
{
    const uint64_t elapsed = s_focus_running && now >= s_focus_start_ms ? now - s_focus_start_ms : 0;
    return elapsed >= s_focus_left_ms ? 0 : s_focus_left_ms - elapsed;
}

static void format_mmss(char *out, size_t size, uint64_t ms, bool centiseconds)
{
    const uint64_t total_s = ms / 1000;
    if (centiseconds) {
        snprintf(out, size, "%02u:%02u.%02u", (unsigned)(total_s / 60),
                 (unsigned)(total_s % 60), (unsigned)((ms % 1000) / 10));
    } else {
        snprintf(out, size, "%02u:%02u", (unsigned)(total_s / 60),
                 (unsigned)(total_s % 60));
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    const uint64_t now = now_ms();
    char value[48];
    char detail[128];
    char clock[16];
    format_clock(clock, sizeof(clock), now, false);
    set_text_if_changed(s_status_time, clock);
    if (!s_panel) return;

    switch (s_app) {
        case WP7_APP_AI_QUOTA: {
            snprintf(value, sizeof(value), "Not set up");
            snprintf(detail, sizeof(detail), "AI quota source can be added later.");
            set_text_if_changed(s_hint, "HOLD OK  Back");
            break;
        }
        case WP7_APP_CLOCK:
            format_clock(value, sizeof(value), now, true);
            snprintf(detail, sizeof(detail), "%s\nTime resets when power is lost.",
                     s_clock_known ? "Manual time" : "Set time with buttons");
            set_text_if_changed(s_hint, "UP +1h   DOWN +1m\nHOLD UP/DOWN +6h/+10m\nHOLD OK Back");
            break;
        case WP7_APP_BATTERY:
            if (s_battery_soc >= 0) snprintf(value, sizeof(value), "%d%%", s_battery_soc);
            else snprintf(value, sizeof(value), "%s", s_battery_soc == -2 ? "Reading..." : "Unavailable");
            if (s_battery_mv >= 0) snprintf(detail, sizeof(detail), "Cell voltage: %d.%03d V\nCW2017 fuel gauge",
                                             s_battery_mv / 1000, s_battery_mv % 1000);
            else snprintf(detail, sizeof(detail), "CW2017 fuel gauge\n%s",
                          s_battery_soc == -2 ? "Waiting for sensor" : "Sensor not responding");
            set_text_if_changed(s_hint, "HOLD OK  Back");
            break;
        case WP7_APP_STOPWATCH:
            format_mmss(value, sizeof(value), stopwatch_elapsed(now), true);
            snprintf(detail, sizeof(detail), "%s\nLaps: %lu\nLast: ",
                     s_stopwatch_running ? "RUNNING" : "PAUSED",
                     (unsigned long)s_stopwatch_laps);
            if (s_stopwatch_laps) {
                char lap[24];
                format_mmss(lap, sizeof(lap), s_stopwatch_lap_ms, true);
                strncat(detail, lap, sizeof(detail) - strlen(detail) - 1);
            } else strncat(detail, "--", sizeof(detail) - strlen(detail) - 1);
            set_text_if_changed(s_hint, "OK Start/Pause  UP Lap\nDOWN Reset  HOLD OK Back");
            break;
        case WP7_APP_FOCUS:
            if (s_focus_running && !focus_remaining(now)) {
                s_focus_running = false;
                s_focus_left_ms = 0;
            }
            format_mmss(value, sizeof(value), focus_remaining(now), false);
            snprintf(detail, sizeof(detail), "%s\nPreset: %u minutes",
                     s_focus_running ? "FOCUSING" : s_focus_left_ms ? "READY / PAUSED" : "TIME IS UP",
                     (unsigned)s_focus_minutes[s_focus_preset]);
            set_text_if_changed(s_hint, "OK Start/Pause  UP Preset\nDOWN Reset  HOLD OK Back");
            break;
        default:
            return;
    }
    set_text_if_changed(s_value, value);
    set_text_if_changed(s_detail, detail);
}

void wp7_apps_init(lv_obj_t *status_time_label)
{
    s_status_time = status_time_label;
    if (!s_timer) s_timer = lv_timer_create(refresh, 100, NULL);
    refresh(NULL);
}

void wp7_apps_set_battery(int soc, int mv)
{
    s_battery_soc = soc;
    s_battery_mv = mv;
    refresh(NULL);
}

bool wp7_apps_open(lv_obj_t *screen, wp7_app_id_t app, int32_t status_h,
                   lv_color_t bg, lv_color_t text, lv_color_t accent)
{
    if (s_panel || app >= WP7_APP_COUNT) return false;
    s_app = app;
    const int32_t width = lv_obj_get_width(screen);
    const int32_t height = lv_obj_get_height(screen) - status_h;
    s_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_pos(s_panel, 0, status_h);
    lv_obj_set_size(s_panel, width, height);
    lv_obj_set_style_bg_color(s_panel, bg, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_title, text, 0);
    lv_label_set_text(s_title, s_titles[app]);
    lv_obj_set_pos(s_title, 12, 13);

    lv_obj_t *stripe = lv_obj_create(s_panel);
    lv_obj_remove_style_all(stripe);
    lv_obj_set_pos(stripe, 12, 49);
    lv_obj_set_size(stripe, 54, 5);
    lv_obj_set_style_bg_color(stripe, accent, 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

    s_value = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_value, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_value, accent, 0);
    lv_obj_set_pos(s_value, 12, 78);
    lv_obj_set_width(s_value, width - 24);

    s_detail = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_detail, text, 0);
    lv_obj_set_pos(s_detail, 12, 124);
    lv_obj_set_width(s_detail, width - 24);

    s_hint = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint, text, 0);
    lv_obj_set_pos(s_hint, 12, height - 46);
    lv_obj_set_width(s_hint, width - 24);
    refresh(NULL);
    return true;
}

void wp7_apps_close(void)
{
    if (!s_panel) return;
    lv_obj_delete(s_panel);
    s_panel = s_title = s_value = s_detail = s_hint = NULL;
}

bool wp7_apps_active(void)
{
    return s_panel != NULL;
}

void wp7_apps_key(wp7_key_t key, bool long_press)
{
    const uint64_t now = now_ms();
    if (s_app == WP7_APP_CLOCK) {
        if (key == WP7_KEY_UP || key == WP7_KEY_DOWN) {
            const uint64_t local = s_clock_known ? clock_local_seconds(now) : 0;
            const uint64_t step = key == WP7_KEY_UP ?
                                  (long_press ? 6 * 3600 : 3600) :
                                  (long_press ? 10 * 60 : 60);
            const uint64_t adjusted = (local + step) % 86400;
            s_clock_known = true;
            s_clock_base_s = adjusted;
            s_clock_base_ms = now;
            s_clock_timezone = 0;
        }
    } else if (s_app == WP7_APP_STOPWATCH && !long_press) {
        if (key == WP7_KEY_OK) {
            if (s_stopwatch_running) s_stopwatch_base_ms = stopwatch_elapsed(now);
            else s_stopwatch_start_ms = now;
            s_stopwatch_running = !s_stopwatch_running;
        } else if (key == WP7_KEY_UP && s_stopwatch_running) {
            s_stopwatch_lap_ms = stopwatch_elapsed(now);
            ++s_stopwatch_laps;
        } else if (key == WP7_KEY_DOWN && !s_stopwatch_running) {
            s_stopwatch_base_ms = s_stopwatch_lap_ms = 0;
            s_stopwatch_laps = 0;
        }
    } else if (s_app == WP7_APP_FOCUS && !long_press) {
        if (key == WP7_KEY_OK) {
            if (s_focus_running) s_focus_left_ms = focus_remaining(now);
            else {
                if (!s_focus_left_ms) s_focus_left_ms = s_focus_minutes[s_focus_preset] * 60000ULL;
                s_focus_start_ms = now;
            }
            s_focus_running = !s_focus_running;
        } else if (key == WP7_KEY_UP && !s_focus_running) {
            s_focus_preset = (s_focus_preset + 1) % (sizeof(s_focus_minutes) / sizeof(s_focus_minutes[0]));
            s_focus_left_ms = s_focus_minutes[s_focus_preset] * 60000ULL;
        } else if (key == WP7_KEY_DOWN && !s_focus_running) {
            s_focus_left_ms = s_focus_minutes[s_focus_preset] * 60000ULL;
        }
    }
    refresh(NULL);
}
