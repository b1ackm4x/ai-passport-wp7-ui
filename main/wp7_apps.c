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
/* Set from the Mac's push rather than by hand; generation of that push. */
static bool s_clock_synced;
static uint32_t s_clock_generation;
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
static bool s_ble_available = true;

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
/* Bars grow to each new reading, including the first after the page opens. */
#define QUOTA_BAR_ANIM_MS  420
/* Switching the Kaboo period slides the numbers out and the new ones in, like
   a WP7 pivot: the first half of the progress range leaves, the second enters. */
#define KABOO_SLIDE_MS     300
#define KABOO_SLIDE_PX     24
#define KABOO_SLIDE_HALF   1000
typedef struct {
    lv_obj_t *pct;
    lv_obj_t *bar;
    lv_obj_t *reset;
    int32_t bar_target;
} quota_row_t;
static quota_row_t s_quota[2];
static lv_obj_t *s_note;
static lv_obj_t *s_kaboo_periods[KABOO_PERIOD_COUNT];
static lv_obj_t *s_kaboo_tokens;
static lv_obj_t *s_kaboo_cost;
static lv_obj_t *s_kaboo_model;
static uint32_t s_kaboo_period;
static uint32_t s_kaboo_shown;   /* period whose numbers are on screen */
static int32_t s_kaboo_slide_dir;
static lv_color_t s_text_color;
static lv_color_t s_accent_color;
/* Title and rows of the open page, in entrance order (see wp7_apps_items). */
static wp7_app_item_t s_items[WP7_APP_MAX_ITEMS];
static int32_t s_item_count;
/* The open transition has finished; until then data animations wait. */
static bool s_entered;

static void refresh(lv_timer_t *timer);

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

static void bar_width_anim_cb(void *bar, int32_t width)
{
    lv_obj_set_width((lv_obj_t *)bar, width);
}

static void quota_bar_set(quota_row_t *row, int32_t width)
{
    /* Bars stay empty while the page slides in and fill once it has landed. */
    if (!s_entered) width = 0;
    if (row->bar_target == width) return;
    row->bar_target = width;

    /* The bar object is the animation's var, so deleting the page stops it. */
    lv_anim_delete(row->bar, bar_width_anim_cb);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, row->bar);
    lv_anim_set_exec_cb(&anim, bar_width_anim_cb);
    lv_anim_set_values(&anim, lv_obj_get_style_width(row->bar, 0), width);
    lv_anim_set_duration(&anim, wp7_ui_anim_ms(QUOTA_BAR_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void quota_row_blank(quota_row_t *row, const char *reset)
{
    set_text_if_changed(row->pct, "--");
    set_color_if_changed(row->pct, s_text_color);
    quota_bar_set(row, 0);
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
    quota_bar_set(row, QUOTA_BAR_W * pct / 100);

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

static const char *no_data_note(void)
{
    return s_ble_available ? "Waiting for Mac" : "Bluetooth unavailable";
}

static void refresh_kaboo(const usage_snapshot_t *snap, bool have)
{
    for (uint32_t i = 0; i < KABOO_PERIOD_COUNT; ++i) {
        const lv_opa_t opa = i == s_kaboo_period ? LV_OPA_COVER : LV_OPA_40;
        if (lv_obj_get_style_text_opa(s_kaboo_periods[i], 0) != opa) {
            lv_obj_set_style_text_opa(s_kaboo_periods[i], opa, 0);
        }
    }

    if (!have || !(snap->flags & USAGE_FLAG_KABOO_VALID)) {
        set_text_if_changed(s_kaboo_tokens, "--");
        set_text_if_changed(s_kaboo_cost, "--");
        set_text_if_changed(s_kaboo_model, "--");
        set_text_if_changed(s_note, have ? "No Kaboo data" : no_data_note());
        set_color_if_changed(s_note, lv_color_hex(USAGE_WARNING_HEX));
        return;
    }

    /* The numbers follow s_kaboo_shown, which a period switch changes halfway
       through its slide; the header follows s_kaboo_period at once. */
    uint64_t tokens = snap->today_tokens;
    uint32_t cents = snap->today_cost_cents;
    if (s_kaboo_shown == 1) {
        tokens = snap->week_tokens;
        cents = snap->week_cost_cents;
    } else if (s_kaboo_shown == 2) {
        tokens = snap->month_tokens;
        cents = snap->month_cost_cents;
    }
    char text[32];
    format_tokens(text, sizeof(text), tokens);
    set_text_if_changed(s_kaboo_tokens, text);
    snprintf(text, sizeof(text), "$%" PRIu32 ".%02" PRIu32, cents / 100, cents % 100);
    set_text_if_changed(s_kaboo_cost, text);
    set_text_if_changed(s_kaboo_model, snap->top_model[0] ? snap->top_model : "--");

    uint32_t now_unix = 0;
    const bool have_now = usage_model_now_unix(snap, true, esp_timer_get_time(),
                                               &now_unix);
    /* A stale source must not pass its old numbers off as current. */
    const bool fresh = refresh_source_note(snap->kaboo_sampled_unix, now_unix, have_now);
    set_color_if_changed(s_kaboo_tokens, fresh ? s_accent_color : s_text_color);
}

static void kaboo_slide_cb(void *var, int32_t progress)
{
    (void)var;
    if (progress >= KABOO_SLIDE_HALF && s_kaboo_shown != s_kaboo_period) {
        s_kaboo_shown = s_kaboo_period;
        refresh(NULL);
    }

    /* Distance from rest: grows while the old numbers leave, shrinks while
       the new ones arrive; squared so both halves ease toward the swap. */
    const bool leaving = progress < KABOO_SLIDE_HALF;
    const int32_t away = leaving ? progress : 2 * KABOO_SLIDE_HALF - progress;
    const int32_t eased = away * away / KABOO_SLIDE_HALF;
    const int32_t offset = KABOO_SLIDE_PX * eased / KABOO_SLIDE_HALF *
                           (leaving ? -s_kaboo_slide_dir : s_kaboo_slide_dir);
    const lv_opa_t opa = (lv_opa_t)(LV_OPA_COVER * (KABOO_SLIDE_HALF - eased) /
                                    KABOO_SLIDE_HALF);
    lv_obj_t *const values[] = { s_kaboo_tokens, s_kaboo_cost, s_kaboo_model };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] == NULL) continue;
        lv_obj_set_style_translate_x(values[i], offset, 0);
        lv_obj_set_style_opa(values[i], opa, 0);
    }
}

/* direction is +1 for the next period (numbers leave to the left) and -1 for
   the previous one. */
static void kaboo_switch_period(int32_t direction)
{
    if (s_kaboo_tokens != NULL && lv_anim_get(s_kaboo_tokens, kaboo_slide_cb) != NULL) {
        /* Land the switch in progress before starting the next one. */
        lv_anim_delete(s_kaboo_tokens, kaboo_slide_cb);
        kaboo_slide_cb(NULL, 2 * KABOO_SLIDE_HALF);
    }

    s_kaboo_period = (s_kaboo_period + (direction > 0 ? 1 : KABOO_PERIOD_COUNT - 1)) %
                     KABOO_PERIOD_COUNT;
    s_kaboo_slide_dir = direction;
    if (s_kaboo_tokens == NULL) {
        s_kaboo_shown = s_kaboo_period;
        return;
    }

    /* Keyed to a page object so deleting the page also stops the slide. */
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_kaboo_tokens);
    lv_anim_set_exec_cb(&anim, kaboo_slide_cb);
    lv_anim_set_values(&anim, 0, 2 * KABOO_SLIDE_HALF);
    lv_anim_set_duration(&anim, wp7_ui_anim_ms(KABOO_SLIDE_MS));
    lv_anim_start(&anim);
}

static void refresh_claude(const usage_snapshot_t *snap, bool have)
{
    const lv_color_t warning = lv_color_hex(USAGE_WARNING_HEX);
    if (!have || !(snap->flags & USAGE_FLAG_CLAUDE_VALID)) {
        quota_row_blank(&s_quota[0], "");
        quota_row_blank(&s_quota[1], "");
        set_text_if_changed(s_note, have ? "No quota data" : no_data_note());
        set_color_if_changed(s_note, warning);
        return;
    }

    uint32_t now_unix = 0;
    const bool have_now = usage_model_now_unix(snap, true, esp_timer_get_time(),
                                               &now_unix);
    const bool fresh = refresh_source_note(snap->claude_sampled_unix, now_unix, have_now);

    /* Claude Code reports a window only while it is active; show a missing
       window as inactive rather than as 0%. */
    if (snap->flags & USAGE_FLAG_FIVE_HOUR) {
        quota_row_show(&s_quota[0], snap->five_hour_pct, snap->five_hour_resets_unix,
                       now_unix, have_now, fresh);
    } else {
        quota_row_blank(&s_quota[0], "Not active");
    }
    if (snap->flags & USAGE_FLAG_SEVEN_DAY) {
        quota_row_show(&s_quota[1], snap->seven_day_pct, snap->seven_day_resets_unix,
                       now_unix, have_now, fresh);
    } else {
        quota_row_blank(&s_quota[1], "Not active");
    }
}

/* Every Mac push carries its UTC time and offset, so the clock is right after
   each boot without setting it by hand. A manual change lasts until the next
   push. */
static void sync_clock(const usage_snapshot_t *snap, uint32_t generation)
{
    uint32_t now_unix = 0;
    if (generation == s_clock_generation ||
            !usage_model_now_unix(snap, true, esp_timer_get_time(), &now_unix)) {
        return;
    }
    s_clock_generation = generation;
    s_clock_known = true;
    s_clock_synced = true;
    s_clock_base_s = now_unix;
    s_clock_base_ms = now_ms();
    s_clock_timezone = snap->tz_offset_minutes;
}

static void add_item(lv_obj_t *obj, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (s_item_count >= WP7_APP_MAX_ITEMS) return;
    s_items[s_item_count++] = (wp7_app_item_t) { .obj = obj, .x = x, .y = y, .w = w, .h = h };
}

/* A transparent, full-width row of the page. The launcher slides whole rows in
   and out, so everything on one line of the page lives in the same row. */
static lv_obj_t *add_row(int32_t y, int32_t h)
{
    /* The new panel has no layout yet, so take the width it was given. */
    const int32_t width = lv_obj_get_style_width(s_panel, LV_PART_MAIN);
    lv_obj_t *row = lv_obj_create(s_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, width, h);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    add_item(row, 0, y, width, h);
    return row;
}

static lv_obj_t *row_label(lv_obj_t *row, int32_t x, int32_t y, const lv_font_t *font,
                           lv_color_t color)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void create_quota_row(quota_row_t *row, int32_t y, const char *name,
                             lv_color_t text, lv_color_t accent)
{
    lv_obj_t *block = add_row(y, 64);
    lv_label_set_text(row_label(block, 12, 4, &lv_font_montserrat_16, text), name);
    row->pct = row_label(block, 12, 0, &lv_font_montserrat_22, accent);
    lv_obj_set_width(row->pct, QUOTA_BAR_W);
    lv_obj_set_style_text_align(row->pct, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *track = lv_obj_create(block);
    lv_obj_remove_style_all(track);
    lv_obj_set_pos(track, 12, 32);
    lv_obj_set_size(track, QUOTA_BAR_W, 8);
    lv_obj_set_style_bg_color(track, text, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    row->bar = lv_obj_create(track);
    lv_obj_remove_style_all(row->bar);
    lv_obj_set_size(row->bar, 0, 8);
    lv_obj_set_style_bg_color(row->bar, accent, 0);
    lv_obj_set_style_bg_opa(row->bar, LV_OPA_COVER, 0);

    row->reset = row_label(block, 12, 46, &lv_font_montserrat_14, text);
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    const uint64_t now = now_ms();
    char value[48];
    char detail[128];
    char clock[16];
    usage_snapshot_t snap;
    uint32_t generation = 0;
    const bool have = usage_link_get(&snap, &generation);
    if (have) sync_clock(&snap, generation);
    format_clock(clock, sizeof(clock), now, false);
    set_text_if_changed(s_status_time, clock);
    if (!s_panel) return;

    switch (s_app) {
        case WP7_APP_KABOO:
            refresh_kaboo(&snap, have);
            set_text_if_changed(s_hint, "UP/DOWN  Period\nHOLD OK  Back");
            return;
        case WP7_APP_CLAUDE:
            refresh_claude(&snap, have);
            set_text_if_changed(s_hint, "HOLD OK  Back");
            return;
        case WP7_APP_CLOCK:
            format_clock(value, sizeof(value), now, true);
            if (s_clock_synced) {
                snprintf(detail, sizeof(detail), "Synced from Mac\nUpdates with each push.");
            } else {
                snprintf(detail, sizeof(detail), "%s\nTime resets when power is lost.",
                         s_clock_known ? "Manual time" : "Set with buttons or the Mac");
            }
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

void wp7_apps_set_ble_available(bool available)
{
    s_ble_available = available;
}

bool wp7_apps_open(lv_obj_t *screen, wp7_app_id_t app, int32_t status_h,
                   lv_color_t bg, lv_color_t text, lv_color_t accent)
{
    if (s_panel || app >= WP7_APP_COUNT) return false;
    s_app = app;
    s_item_count = 0;
    s_entered = false;
    const int32_t width = lv_obj_get_width(screen);
    const int32_t height = lv_obj_get_height(screen) - status_h;
    const int32_t content_w = width - 24;
    s_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_pos(s_panel, 0, status_h);
    lv_obj_set_size(s_panel, width, height);
    lv_obj_set_style_bg_color(s_panel, bg, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Hidden until the launcher's open transition reaches the page. */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    /* The title is sized and pivoted like the UI Settings title, which the
       transition scales as it flies in. */
    const int32_t title_h = lv_font_montserrat_22.line_height + 4;
    s_title = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_title, text, 0);
    lv_label_set_text(s_title, s_titles[app]);
    lv_obj_set_pos(s_title, 12, 13);
    lv_obj_set_size(s_title, content_w, title_h);
    lv_obj_set_style_transform_pivot_x(s_title, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_title, title_h / 2, 0);
    add_item(s_title, 12, 13, content_w, title_h);

    lv_obj_t *stripe_row = add_row(49, 5);
    lv_obj_t *stripe = lv_obj_create(stripe_row);
    lv_obj_remove_style_all(stripe);
    lv_obj_set_pos(stripe, 12, 0);
    lv_obj_set_size(stripe, 54, 5);
    lv_obj_set_style_bg_color(stripe, accent, 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

    s_text_color = text;
    s_accent_color = accent;
    s_kaboo_shown = s_kaboo_period;
    if (app == WP7_APP_KABOO) {
        static const char *const periods[KABOO_PERIOD_COUNT] = {
            "today", "7 days", "30 days",
        };
        lv_obj_t *row = add_row(70, 20);
        int32_t x = 12;
        for (uint32_t i = 0; i < KABOO_PERIOD_COUNT; ++i) {
            s_kaboo_periods[i] = row_label(row, x, 0, &lv_font_montserrat_16, text);
            lv_label_set_text(s_kaboo_periods[i], periods[i]);
            lv_obj_update_layout(s_kaboo_periods[i]);
            x += lv_obj_get_width(s_kaboo_periods[i]) + 16;
        }
        row = add_row(104, 28);
        lv_label_set_text(row_label(row, 12, 4, &lv_font_montserrat_16, text), "Tokens");
        s_kaboo_tokens = row_label(row, 12, 0, &lv_font_montserrat_22, accent);
        lv_obj_set_width(s_kaboo_tokens, content_w);
        lv_obj_set_style_text_align(s_kaboo_tokens, LV_TEXT_ALIGN_RIGHT, 0);
        row = add_row(138, 28);
        lv_label_set_text(row_label(row, 12, 4, &lv_font_montserrat_16, text), "Cost");
        s_kaboo_cost = row_label(row, 12, 0, &lv_font_montserrat_22, text);
        lv_obj_set_width(s_kaboo_cost, content_w);
        lv_obj_set_style_text_align(s_kaboo_cost, LV_TEXT_ALIGN_RIGHT, 0);
        row = add_row(174, 38);
        lv_label_set_text(row_label(row, 12, 0, &lv_font_montserrat_14, text), "Top model");
        s_kaboo_model = row_label(row, 12, 18, &lv_font_montserrat_16, text);
        lv_obj_set_width(s_kaboo_model, content_w);
        lv_obj_set_height(s_kaboo_model, lv_font_montserrat_16.line_height);
        lv_label_set_long_mode(s_kaboo_model, LV_LABEL_LONG_DOT);
        s_note = row_label(add_row(218, 18), 12, 0, &lv_font_montserrat_14, text);
    } else if (app == WP7_APP_CLAUDE) {
        create_quota_row(&s_quota[0], 70, "5-hour", text, accent);
        create_quota_row(&s_quota[1], 144, "7-day", text, accent);
        s_note = row_label(add_row(218, 18), 12, 0, &lv_font_montserrat_14, text);
    } else {
        s_value = row_label(add_row(78, 28), 12, 0, &lv_font_montserrat_22, accent);
        lv_obj_set_width(s_value, content_w);
        /* The detail runs to four lines when the Clock text wraps, so its row
           takes all the space down to the hint. */
        s_detail = row_label(add_row(124, height - 46 - 124), 12, 0,
                             &lv_font_montserrat_16, text);
        lv_obj_set_width(s_detail, content_w);
    }

    s_hint = row_label(add_row(height - 46, 46), 12, 0, &lv_font_montserrat_14, text);
    lv_obj_set_width(s_hint, content_w);
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
    memset(s_items, 0, sizeof(s_items));
    s_item_count = 0;
    s_entered = false;
}

bool wp7_apps_active(void)
{
    return s_panel != NULL;
}

int32_t wp7_apps_items(wp7_app_item_t *items, int32_t capacity)
{
    const int32_t count = s_item_count < capacity ? s_item_count : capacity;
    memcpy(items, s_items, (size_t)count * sizeof(items[0]));
    return count;
}

void wp7_apps_set_visible(bool visible)
{
    if (s_panel == NULL || lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN) == !visible) return;
    if (visible) lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void wp7_apps_entered(void)
{
    if (s_panel == NULL || s_entered) return;
    s_entered = true;
    refresh(NULL);
}

void wp7_apps_key(wp7_key_t key, bool long_press)
{
    const uint64_t now = now_ms();
    if (s_app == WP7_APP_KABOO && !long_press && key != WP7_KEY_OK) {
        kaboo_switch_period(key == WP7_KEY_DOWN ? 1 : -1);
    } else if (s_app == WP7_APP_CLOCK) {
        if (key == WP7_KEY_UP || key == WP7_KEY_DOWN) {
            const uint64_t local = s_clock_known ? clock_local_seconds(now) : 0;
            const uint64_t step = key == WP7_KEY_UP ?
                                  (long_press ? 6 * 3600 : 3600) :
                                  (long_press ? 10 * 60 : 60);
            const uint64_t adjusted = (local + step) % 86400;
            s_clock_known = true;
            s_clock_synced = false;
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
