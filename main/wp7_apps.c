#include "wp7_apps.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "usage_link.h"

static const char *const s_titles[WP7_APP_COUNT] = {
    "Kaboo", "Claude", "Clock", "Battery", "Stopwatch", "Focus timer",
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

/* Usage pages. Kaboo shows token count, cost and top model for one of three
   periods; Claude shows two quota windows, each a label, percentage, bar and
   reset line. Both end with a source note. Widgets exist only while the page
   is open. */
#define USAGE_SOURCE_TTL_S  900
#define KABOO_PERIOD_COUNT  3
#define QUOTA_BAR_W        216
#define USAGE_WARNING_HEX  0xF09609
/* Quota use turns from the theme color to yellow, then red, as a window
   nears its limit. The colors are the Windows Phone yellow and red accents. */
#define QUOTA_WARN_PCT     70
#define QUOTA_ALERT_PCT    90
#define QUOTA_WARN_HEX     0xE3C800
#define QUOTA_ALERT_HEX    0xE51400
typedef struct {
    lv_obj_t *pct;
    lv_obj_t *bar;
    lv_obj_t *reset;
} quota_row_t;
static quota_row_t s_quota[2];
static lv_obj_t *s_note;
static lv_obj_t *s_kaboo_periods[KABOO_PERIOD_COUNT];
static lv_obj_t *s_kaboo_tokens;
static lv_obj_t *s_kaboo_cost;
static lv_obj_t *s_kaboo_model;
static uint32_t s_kaboo_period;
static lv_color_t s_text_color;
static lv_color_t s_accent_color;
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

static void set_color_if_changed(lv_obj_t *obj, lv_color_t color)
{
    if (obj && !lv_color_eq(lv_obj_get_style_text_color(obj, 0), color)) {
        lv_obj_set_style_text_color(obj, color, 0);
    }
}

static void quota_row_blank(quota_row_t *row, const char *reset)
{
    set_text_if_changed(row->pct, "--");
    set_color_if_changed(row->pct, s_text_color);
    if (lv_obj_get_width(row->bar) != 0) lv_obj_set_width(row->bar, 0);
    set_text_if_changed(row->reset, reset);
}

static lv_color_t quota_color(uint8_t pct)
{
    if (pct >= QUOTA_ALERT_PCT) return lv_color_hex(QUOTA_ALERT_HEX);
    if (pct >= QUOTA_WARN_PCT) return lv_color_hex(QUOTA_WARN_HEX);
    return s_accent_color;
}

static void quota_row_show(quota_row_t *row, uint8_t pct, uint32_t resets_unix,
                           uint32_t now_unix, bool have_now, bool fresh)
{
    char text[40];
    snprintf(text, sizeof(text), "%u%%", (unsigned)pct);
    set_text_if_changed(row->pct, text);
    const int32_t width = QUOTA_BAR_W * pct / 100;
    if (lv_obj_get_width(row->bar) != width) lv_obj_set_width(row->bar, width);

    /* Past the reset time the percentage belongs to the previous window. */
    const bool expired = have_now && usage_model_quota_expired(resets_unix, now_unix);
    const bool current = fresh && !expired;
    const lv_color_t color = quota_color(pct);
    set_color_if_changed(row->pct, current ? color : s_text_color);
    /* An old reading keeps its level color on the bar, only dimmed, so a full
       but stale window never reads as an empty one. */
    if (!lv_color_eq(lv_obj_get_style_bg_color(row->bar, 0), color)) {
        lv_obj_set_style_bg_color(row->bar, color, 0);
    }
    const lv_opa_t opa = current ? LV_OPA_COVER : LV_OPA_50;
    if (lv_obj_get_style_bg_opa(row->bar, 0) != opa) {
        lv_obj_set_style_bg_opa(row->bar, opa, 0);
    }

    if (!have_now) {
        text[0] = '\0';
    } else if (expired) {
        snprintf(text, sizeof(text), "Awaiting update");
    } else {
        const uint32_t left = usage_model_seconds_until(resets_unix, now_unix);
        if (left >= 86400) {
            snprintf(text, sizeof(text), "Resets in %" PRIu32 "d %" PRIu32 "h",
                     left / 86400, left % 86400 / 3600);
        } else if (left >= 3600) {
            snprintf(text, sizeof(text), "Resets in %" PRIu32 "h %" PRIu32 "m",
                     left / 3600, left % 3600 / 60);
        } else {
            snprintf(text, sizeof(text), "Resets in %" PRIu32 "m", left / 60);
        }
    }
    set_text_if_changed(row->reset, text);
}

/* Sample age is independent of receipt time: the bridge may resend an old
   sample, so the note stays visible even while the source is fresh. */
static bool refresh_source_note(uint32_t sampled_unix, uint32_t now_unix, bool have_now)
{
    const bool fresh = have_now && usage_model_source_fresh(
        sampled_unix, now_unix, USAGE_SOURCE_TTL_S);
    char note[32];
    if (!have_now) {
        snprintf(note, sizeof(note), "Update time unknown");
    } else if (!fresh) {
        snprintf(note, sizeof(note), "Data may be stale");
    } else if (now_unix - sampled_unix < 60) {
        snprintf(note, sizeof(note), "Updated <1m ago");
    } else {
        snprintf(note, sizeof(note), "Updated %" PRIu32 "m ago",
                 (now_unix - sampled_unix) / 60);
    }
    set_text_if_changed(s_note, note);
    set_color_if_changed(s_note, fresh ? s_text_color : lv_color_hex(USAGE_WARNING_HEX));
    return fresh;
}

static void format_tokens(char *out, size_t size, uint64_t tokens)
{
    static const struct { uint64_t unit; char suffix; } scales[] = {
        {1000000000ULL, 'B'}, {1000000ULL, 'M'}, {1000ULL, 'K'},
    };
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        if (tokens >= scales[i].unit) {
            snprintf(out, size, "%" PRIu64 ".%" PRIu64 "%c", tokens / scales[i].unit,
                     tokens % scales[i].unit / (scales[i].unit / 10), scales[i].suffix);
            return;
        }
    }
    snprintf(out, size, "%" PRIu64, tokens);
}

static void refresh_kaboo(void)
{
    for (uint32_t i = 0; i < KABOO_PERIOD_COUNT; ++i) {
        const lv_opa_t opa = i == s_kaboo_period ? LV_OPA_COVER : LV_OPA_40;
        if (lv_obj_get_style_text_opa(s_kaboo_periods[i], 0) != opa) {
            lv_obj_set_style_text_opa(s_kaboo_periods[i], opa, 0);
        }
    }

    usage_snapshot_t snap;
    const bool have = usage_link_get(&snap, NULL);
    if (!have || !(snap.flags & USAGE_FLAG_KABOO_VALID)) {
        set_text_if_changed(s_kaboo_tokens, "--");
        set_text_if_changed(s_kaboo_cost, "--");
        set_text_if_changed(s_kaboo_model, "--");
        set_text_if_changed(s_note, have ? "No Kaboo data" : "Waiting for Mac");
        set_color_if_changed(s_note, lv_color_hex(USAGE_WARNING_HEX));
        return;
    }

    uint64_t tokens = snap.today_tokens;
    uint32_t cents = snap.today_cost_cents;
    if (s_kaboo_period == 1) {
        tokens = snap.week_tokens;
        cents = snap.week_cost_cents;
    } else if (s_kaboo_period == 2) {
        tokens = snap.month_tokens;
        cents = snap.month_cost_cents;
    }
    char text[32];
    format_tokens(text, sizeof(text), tokens);
    set_text_if_changed(s_kaboo_tokens, text);
    snprintf(text, sizeof(text), "$%" PRIu32 ".%02" PRIu32, cents / 100, cents % 100);
    set_text_if_changed(s_kaboo_cost, text);
    set_text_if_changed(s_kaboo_model, snap.top_model[0] ? snap.top_model : "--");

    uint32_t now_unix = 0;
    const bool have_now = usage_model_now_unix(&snap, true, esp_timer_get_time(),
                                               &now_unix);
    /* A stale source must not pass its old numbers off as current. */
    const bool fresh = refresh_source_note(snap.kaboo_sampled_unix, now_unix, have_now);
    set_color_if_changed(s_kaboo_tokens, fresh ? s_accent_color : s_text_color);
}

static void refresh_claude(void)
{
    usage_snapshot_t snap;
    const bool have = usage_link_get(&snap, NULL);
    const lv_color_t warning = lv_color_hex(USAGE_WARNING_HEX);
    if (!have || !(snap.flags & USAGE_FLAG_CLAUDE_VALID)) {
        quota_row_blank(&s_quota[0], "");
        quota_row_blank(&s_quota[1], "");
        set_text_if_changed(s_note, have ? "No quota data" : "Waiting for Mac");
        set_color_if_changed(s_note, warning);
        return;
    }

    uint32_t now_unix = 0;
    const bool have_now = usage_model_now_unix(&snap, true, esp_timer_get_time(),
                                               &now_unix);
    const bool fresh = refresh_source_note(snap.claude_sampled_unix, now_unix, have_now);

    /* Claude Code reports a window only while it is active; show a missing
       window as inactive rather than as 0%. */
    if (snap.flags & USAGE_FLAG_FIVE_HOUR) {
        quota_row_show(&s_quota[0], snap.five_hour_pct, snap.five_hour_resets_unix,
                       now_unix, have_now, fresh);
    } else {
        quota_row_blank(&s_quota[0], "Not active");
    }
    if (snap.flags & USAGE_FLAG_SEVEN_DAY) {
        quota_row_show(&s_quota[1], snap.seven_day_pct, snap.seven_day_resets_unix,
                       now_unix, have_now, fresh);
    } else {
        quota_row_blank(&s_quota[1], "Not active");
    }
}

static lv_obj_t *panel_label(int32_t x, int32_t y, const lv_font_t *font,
                             lv_color_t color)
{
    lv_obj_t *label = lv_label_create(s_panel);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void create_quota_row(quota_row_t *row, int32_t y, const char *name,
                             lv_color_t text, lv_color_t accent)
{
    lv_label_set_text(panel_label(12, y + 4, &lv_font_montserrat_16, text), name);
    row->pct = panel_label(12, y, &lv_font_montserrat_22, accent);
    lv_obj_set_width(row->pct, QUOTA_BAR_W);
    lv_obj_set_style_text_align(row->pct, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *track = lv_obj_create(s_panel);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, 12, y + 32);
    lv_obj_set_size(track, QUOTA_BAR_W, 8);
    lv_obj_set_style_bg_color(track, text, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    row->bar = lv_obj_create(track);
    lv_obj_remove_style_all(row->bar);
    lv_obj_set_size(row->bar, 0, 8);
    lv_obj_set_style_bg_color(row->bar, accent, 0);
    lv_obj_set_style_bg_opa(row->bar, LV_OPA_COVER, 0);

    row->reset = panel_label(12, y + 46, &lv_font_montserrat_14, text);
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
        case WP7_APP_KABOO:
            refresh_kaboo();
            set_text_if_changed(s_hint, "UP/DOWN  Period\nHOLD OK  Back");
            return;
        case WP7_APP_CLAUDE:
            refresh_claude();
            set_text_if_changed(s_hint, "HOLD OK  Back");
            return;
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

    s_text_color = text;
    s_accent_color = accent;
    if (app == WP7_APP_KABOO) {
        static const char *const periods[KABOO_PERIOD_COUNT] = {
            "today", "7 days", "30 days",
        };
        int32_t x = 12;
        for (uint32_t i = 0; i < KABOO_PERIOD_COUNT; ++i) {
            s_kaboo_periods[i] = panel_label(x, 70, &lv_font_montserrat_16, text);
            lv_label_set_text(s_kaboo_periods[i], periods[i]);
            lv_obj_update_layout(s_kaboo_periods[i]);
            x += lv_obj_get_width(s_kaboo_periods[i]) + 16;
        }
        lv_label_set_text(panel_label(12, 108, &lv_font_montserrat_16, text), "Tokens");
        s_kaboo_tokens = panel_label(12, 104, &lv_font_montserrat_22, accent);
        lv_label_set_text(panel_label(12, 142, &lv_font_montserrat_16, text), "Cost");
        s_kaboo_cost = panel_label(12, 138, &lv_font_montserrat_22, text);
        lv_obj_set_width(s_kaboo_tokens, width - 24);
        lv_obj_set_width(s_kaboo_cost, width - 24);
        lv_obj_set_style_text_align(s_kaboo_tokens, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_align(s_kaboo_cost, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(panel_label(12, 174, &lv_font_montserrat_14, text), "Top model");
        s_kaboo_model = panel_label(12, 192, &lv_font_montserrat_16, text);
        lv_obj_set_width(s_kaboo_model, width - 24);
        lv_obj_set_height(s_kaboo_model, lv_font_montserrat_16.line_height);
        lv_label_set_long_mode(s_kaboo_model, LV_LABEL_LONG_DOT);
        s_note = panel_label(12, 218, &lv_font_montserrat_14, text);
    } else if (app == WP7_APP_CLAUDE) {
        create_quota_row(&s_quota[0], 70, "5-hour", text, accent);
        create_quota_row(&s_quota[1], 144, "7-day", text, accent);
        s_note = panel_label(12, 218, &lv_font_montserrat_14, text);
    } else {
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
    }

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
    s_note = s_kaboo_tokens = s_kaboo_cost = s_kaboo_model = NULL;
    memset(s_kaboo_periods, 0, sizeof(s_kaboo_periods));
    memset(s_quota, 0, sizeof(s_quota));
}

bool wp7_apps_active(void)
{
    return s_panel != NULL;
}

void wp7_apps_key(wp7_key_t key, bool long_press)
{
    const uint64_t now = now_ms();
    if (s_app == WP7_APP_KABOO && !long_press && key != WP7_KEY_OK) {
        s_kaboo_period = (s_kaboo_period + (key == WP7_KEY_DOWN ? 1 : KABOO_PERIOD_COUNT - 1)) %
                         KABOO_PERIOD_COUNT;
    } else if (s_app == WP7_APP_CLOCK) {
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
