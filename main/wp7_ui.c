/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from ZyoungInc/JC4880P443C_BSP main/main.c (wp7 branch,
 * 9d1743a9444a8d1fe9226e24c7f2b930e22f8612) for the FoloToy AI Passport.
 * Changes include the ESP32-C3 display and button bridge, 240x320 layout,
 * named tiles, and application-page integration. See UPSTREAM.md.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "wp7_apps.h"
#include "wp7_ui.h"

#define WP7_COLUMNS                  2
#define WP7_MAX_ROWS                 4
#define WP7_MAX_TILES                (WP7_COLUMNS * WP7_MAX_ROWS)
#define WP7_MAX_LIST_ITEMS           (WP7_MAX_TILES + 1)
#define WP7_LIST_UI_SETTINGS_INDEX   6
#define WP7_STATUS_BAR_PERMILLE      50
#define WP7_SCREEN_PAD_PERMILLE      34
#define WP7_TILE_GAP_PERMILLE        34
#define WP7_LIST_RIGHT_OFFSET_PERMILLE 40
#define WP7_LIST_ROW_H_PERMILLE      78
#define WP7_LIST_ROW_GAP_PERMILLE    18
#define WP7_TILE_PROGRESS_UNIT       1000
/* Each item moves for one progress unit and the next starts a quarter unit
   later (an eighth with fast animations), so neighbors overlap. Upstream
   started each item only after the previous one finished, which took about
   3.4 s to open UI Settings at the default speed; keys are ignored meanwhile. */
#define WP7_TILE_STAGGER_UNIT        (WP7_TILE_PROGRESS_UNIT / 4)
#define WP7_TILE_FAST_STAGGER_UNIT   (WP7_TILE_PROGRESS_UNIT / 8)
#define WP7_PAGE_BLANK_HOLD_MS       80
#define WP7_DRAG_DISTANCE_PERMILLE   550
#define WP7_DRAG_THRESHOLD_PERMILLE  20
#define WP7_RELEASE_COMMIT_PERMILLE  220
#define WP7_BASE_PROGRESS_PER_MS     8
#define WP7_BASE_RELEASE_MIN_MS      620
#define WP7_SETTINGS_TILE_PHASE_UNIT (WP7_TILE_PROGRESS_UNIT * 6 / 5)
#define WP7_SETTINGS_TITLE_PHASE_UNIT WP7_TILE_PROGRESS_UNIT
#define WP7_SETTINGS_TILE_INDEX      5
#define WP7_SETTINGS_CONTENT_COUNT   12
#define WP7_ANIM_SPEED_MIN           50
#define WP7_ANIM_SPEED_DEFAULT       100
#define WP7_ANIM_SPEED_MAX           145
#define WP7_ACTUAL_SPEED_MIN_PERMILLE 280
#define WP7_ACTUAL_SPEED_DEFAULT_PERMILLE 1000
#define WP7_ACTUAL_SPEED_MAX_PERMILLE 1420
#define WP7_HORIZONTAL_SPEED_PERMILLE 820
#define WP7_SETTINGS_SPEED_PERMILLE 760
#define WP7_BRIGHTNESS_MIN           5
#define WP7_BRIGHTNESS_DEFAULT       100
#define WP7_BRIGHTNESS_MAX           100
#define WP7_THEME_COLOR_COUNT        10
#define WP7_THEME_ROW_COUNT          2
#define WP7_THEME_COLUMNS            (WP7_THEME_COLOR_COUNT / WP7_THEME_ROW_COUNT)
#define WP7_THEME_COLOR_ANIM_MS      280
#define WP7_THEME_SWATCH_ANIM_MS     180
#define WP7_THEME_SWATCH_SELECTED_PERMILLE 1000
#define WP7_THEME_SWATCH_IDLE_PERMILLE 900
#define WP7_SWITCH_VALUE_MAX         100
#define WP7_SWITCH_THRESHOLD         50
#define WP7_PRESS_ANIM_MS            110
#define WP7_OBJ_PRESS_SCALE          250
#define WP7_OBJ_RELEASE_SCALE        256
#define WP7_KNOB_PRESS_EXTEND        4
#define WP7_KNOB_RELEASE_EXTEND      0
#define WP7_NVS_NAMESPACE            "wp7_ui"
#define WP7_NVS_SPEED_KEY            "anim_spd"
#define WP7_NVS_BRIGHTNESS_KEY       "bright"
#define WP7_NVS_THEME_KEY            "theme"
#define WP7_NVS_DARK_KEY             "dark"
#define WP7_NVS_FAST_KEY             "fast"

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *label;
    int32_t x;
    int32_t y;
    int32_t size;
    int32_t label_page;
    int32_t label_index;
    int32_t current_x;
    int32_t current_y;
    int32_t current_w;
    int32_t current_h;
    int32_t current_scale;
    lv_opa_t current_opa;
    bool hidden;
    bool frame_valid;
    bool style_valid;
    bool press_active;
    bool press_animating;
    bool press_releasing;
} wp7_tile_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *icon;
    lv_obj_t *icon_label;
    lv_obj_t *label;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t icon_size;
    int32_t current_x;
    int32_t current_y;
    int32_t current_w;
    int32_t current_h;
    int32_t current_icon_size;
    lv_opa_t current_opa;
    int32_t current_scale;
    bool hidden;
    bool frame_valid;
    bool style_valid;
    bool press_active;
    bool press_animating;
    bool press_releasing;
} wp7_list_item_t;

typedef struct {
    lv_obj_t *obj;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t current_x;
    lv_opa_t current_opa;
    bool hidden;
    bool frame_valid;
    bool style_valid;
} wp7_setting_item_t;

typedef enum {
    WP7_DIR_NONE,
    WP7_DIR_NEXT,
    WP7_DIR_PREV,
    WP7_DIR_LIST,
    WP7_DIR_HOME,
    WP7_DIR_SETTINGS_OPEN,
    WP7_DIR_SETTINGS_CLOSE,
    WP7_DIR_APP_OPEN,
    WP7_DIR_APP_CLOSE,
} wp7_page_dir_t;

typedef enum {
    WP7_SETTINGS_ITEM_BRIGHTNESS_LABEL,
    WP7_SETTINGS_ITEM_BRIGHTNESS_SLIDER,
    WP7_SETTINGS_ITEM_MODE_LABEL,
    WP7_SETTINGS_ITEM_MODE_SWITCH,
    WP7_SETTINGS_ITEM_THEME_LABEL,
    WP7_SETTINGS_ITEM_THEME_ROW_0,
    WP7_SETTINGS_ITEM_THEME_ROW_1,
    WP7_SETTINGS_ITEM_SPEED_LABEL,
    WP7_SETTINGS_ITEM_SPEED_SLIDER,
    WP7_SETTINGS_ITEM_RESET_BUTTON,
    WP7_SETTINGS_ITEM_FAST_LABEL,
    WP7_SETTINGS_ITEM_FAST_SWITCH,
} wp7_setting_item_index_t;

typedef struct {
    wp7_tile_t tiles[WP7_MAX_TILES];
    wp7_list_item_t list_items[WP7_MAX_LIST_ITEMS];
    wp7_setting_item_t settings_items[WP7_SETTINGS_CONTENT_COUNT];
    /* The open app page's title and rows (see wp7_apps_items). The page
       transitions move them like the UI Settings title and controls. */
    wp7_setting_item_t app_items[WP7_APP_MAX_ITEMS];
    int32_t app_item_count;
    int32_t app_title_scale;
    wp7_app_id_t open_app;
    lv_obj_t *status_bar;
    lv_obj_t *status_left_cont;
    lv_obj_t *status_wifi_label;
    lv_obj_t *status_ble_label;
    lv_obj_t *status_time_label;
    lv_obj_t *status_battery_label;
    wp7_wifi_status_t wifi_status;
    wp7_ble_status_t ble_status;
    lv_obj_t *settings_title;
    lv_obj_t *brightness_slider;
    lv_obj_t *mode_switch;
    lv_obj_t *mode_label;
    lv_obj_t *theme_rows[WP7_THEME_ROW_COUNT];
    lv_obj_t *theme_buttons[WP7_THEME_COLOR_COUNT];
    lv_obj_t *settings_slider;
    lv_obj_t *fast_switch;
    lv_obj_t *settings_reset_button;
    lv_obj_t *settings_reset_label;
    int32_t tile_count;
    int32_t list_count;
    int32_t screen_w;
    int32_t screen_h;
    int32_t settings_title_x;
    int32_t settings_title_y;
    int32_t settings_title_w;
    int32_t settings_title_h;
    int32_t settings_title_current_x;
    int32_t settings_title_current_scale;
    lv_opa_t settings_title_current_opa;
    uint32_t settings_title_current_text_hex;
    /* The tile or list row that opened the current page, UI Settings or an
       app; its open and close transitions start and end there. */
    int32_t settings_tile_index;
    int32_t settings_list_index;
    int32_t anim_speed_percent;
    int32_t brightness_percent;
    int32_t brightness_hw_percent;
    int32_t theme_index;
    int32_t theme_swatch_cell_size;
    uint32_t theme_anim_from_hex;
    uint32_t theme_anim_to_hex;
    uint32_t theme_anim_from_text_hex;
    uint32_t theme_anim_to_text_hex;
    int32_t theme_anim_progress;
    uint32_t ui_anim_from_bg_hex;
    uint32_t ui_anim_to_bg_hex;
    uint32_t ui_anim_from_text_hex;
    uint32_t ui_anim_to_text_hex;
    uint32_t ui_anim_from_track_hex;
    uint32_t ui_anim_to_track_hex;
    uint32_t ui_anim_from_disabled_bg_hex;
    uint32_t ui_anim_to_disabled_bg_hex;
    int32_t ui_anim_progress;
    int32_t control_press_x;
    int32_t control_press_y;
    int32_t brightness_slider_press_value;
    int32_t settings_slider_press_value;
    int32_t page;
    int32_t target_page;
    int32_t drag_start_x;
    int32_t drag_start_y;
    int32_t drag_progress;
    int32_t drag_target_progress;
    int32_t drag_base_progress;
    int32_t anim_progress;
    uint32_t drag_last_tick;
    bool drag_active;
    bool drag_interrupted_anim;
    bool drag_interrupted_commit;
    bool animating;
    bool commit_transition;
    bool in_list;
    bool in_settings;
    bool in_app;
    bool settings_from_list;
    bool dark_mode;
    bool fast_animations;
    bool theme_animating;
    bool ui_animating;
    bool brightness_slider_tap_pending;
    bool brightness_slider_tap_animating;
    bool settings_slider_tap_pending;
    bool mode_switch_press_checked;
    bool mode_switch_tap_pending;
    bool fast_switch_press_checked;
    bool fast_switch_tap_pending;
    bool settings_title_hidden;
    bool settings_title_frame_valid;
    bool settings_title_style_valid;
    int32_t key_tile_index;
    int32_t key_list_index;
    int32_t key_setting_index;
    wp7_page_dir_t drag_dir;
    wp7_page_dir_t anim_dir;
} wp7_screen_t;

static wp7_screen_t s_wp7;

/* App list rows. Transitions create and destroy the list page part way
   through, so its length must not depend on whether the page exists: the
   list-to-tiles transition used to recount 9 rows after the blank phase had
   destroyed the 7-row list, step back into the slide-out phase and touch rows
   that do not exist. */
static const char *const s_list_labels[] = {
    "Kaboo",
    "Claude",
    "Clock",
    "Battery",
    "Stopwatch",
    "Focus timer",
    "UI Settings",
};
#define WP7_LIST_ITEM_COUNT ((int32_t)(sizeof(s_list_labels) / sizeof(s_list_labels[0])))
_Static_assert(sizeof(s_list_labels) / sizeof(s_list_labels[0]) <= WP7_MAX_LIST_ITEMS,
               "the app list must fit WP7_MAX_LIST_ITEMS");

static void create_list_page(lv_obj_t *screen, int32_t screen_w, int32_t screen_h, int32_t status_h);
static void create_settings_page(lv_obj_t *screen, int32_t screen_w, int32_t screen_h, int32_t status_h);
static void key_render_focus(void);
static void clear_key_focus(void);
static int32_t animation_progress_per_ms(wp7_page_dir_t dir);
static void set_wp7_switch_style(lv_obj_t *sw, bool checked);
static void update_wp7_switch_accent(lv_obj_t *sw);
static void set_reset_button_press_scale(int32_t scale);
static void release_tile_press_for_drag(void);
static void release_list_press_for_transition(void);
static void set_list_item_box(wp7_list_item_t *item, int32_t x, lv_opa_t opa, int32_t scale);

static const uint32_t s_wp7_theme_colors[WP7_THEME_COLOR_COUNT] = {
    0xFF0097,
    0xA200FF,
    0x00ABA9,
    0x8CBF26,
    0xA05000,
    0xE671B8,
    0xF09609,
    0x1BA1E2,
    0xE51400,
    0x339933,
};

static int32_t scaled_px(int32_t base, int32_t permille)
{
    int32_t value = (base * permille) / 1000;
    return value > 0 ? value : 1;
}

static int32_t clamp_i32(int32_t value, int32_t min, int32_t max)
{
    if (value < min) {
        return min;
    }

    if (value > max) {
        return max;
    }

    return value;
}

static int32_t abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

static int32_t animation_stagger_unit(void)
{
    return s_wp7.fast_animations ? WP7_TILE_FAST_STAGGER_UNIT : WP7_TILE_STAGGER_UNIT;
}

static int32_t staggered_phase_max(int32_t item_count)
{
    if (item_count <= 0) {
        return 0;
    }

    return ((item_count - 1) * animation_stagger_unit()) + WP7_TILE_PROGRESS_UNIT;
}

static bool is_horizontal_dir(wp7_page_dir_t dir)
{
    return dir == WP7_DIR_LIST || dir == WP7_DIR_HOME;
}

static bool is_swipe_transition_dir(wp7_page_dir_t dir)
{
    return dir == WP7_DIR_NEXT || dir == WP7_DIR_PREV ||
           dir == WP7_DIR_LIST || dir == WP7_DIR_HOME;
}

/* UI Settings and the app pages open and close with the same transitions. */
static bool is_open_dir(wp7_page_dir_t dir)
{
    return dir == WP7_DIR_SETTINGS_OPEN || dir == WP7_DIR_APP_OPEN;
}

static bool is_close_dir(wp7_page_dir_t dir)
{
    return dir == WP7_DIR_SETTINGS_CLOSE || dir == WP7_DIR_APP_CLOSE;
}

static bool is_app_dir(wp7_page_dir_t dir)
{
    return dir == WP7_DIR_APP_OPEN || dir == WP7_DIR_APP_CLOSE;
}

/* Rows of the app page after its title. The page lives from the start of its
   open transition to the end of its close transition, so the count, and with
   it the length of either transition, holds throughout. */
static int32_t app_row_count(void)
{
    return s_wp7.app_item_count > 1 ? s_wp7.app_item_count - 1 : 0;
}

/* Parts that enter after the page title. */
static int32_t page_content_count(wp7_page_dir_t dir)
{
    return is_app_dir(dir) ? app_row_count() : WP7_SETTINGS_CONTENT_COUNT;
}

static int32_t settings_other_tile_count(void)
{
    return s_wp7.tile_count > 0 ? s_wp7.tile_count - 1 : 0;
}

static int32_t list_transition_count(void)
{
    return WP7_LIST_ITEM_COUNT;
}

static int32_t settings_other_list_count(void)
{
    const int32_t count = list_transition_count();

    return count > 0 ? count - 1 : 0;
}

static int32_t settings_other_item_count(void)
{
    return s_wp7.settings_from_list ?
           settings_other_list_count() :
           settings_other_tile_count();
}

static int32_t transition_blank_progress_for_dir(wp7_page_dir_t dir)
{
    return animation_progress_per_ms(dir) * WP7_PAGE_BLANK_HOLD_MS;
}

static int32_t transition_progress_max_for_dir(wp7_page_dir_t dir)
{
    if (is_open_dir(dir)) {
        return staggered_phase_max(settings_other_item_count()) +
               WP7_SETTINGS_TILE_PHASE_UNIT +
               transition_blank_progress_for_dir(dir) +
               staggered_phase_max(page_content_count(dir) + 1);
    }

    if (is_close_dir(dir)) {
        return staggered_phase_max(page_content_count(dir)) +
               WP7_SETTINGS_TITLE_PHASE_UNIT +
               transition_blank_progress_for_dir(dir) +
               WP7_SETTINGS_TILE_PHASE_UNIT +
               staggered_phase_max(settings_other_item_count());
    }

    if (dir == WP7_DIR_LIST) {
        return staggered_phase_max(s_wp7.tile_count) +
               transition_blank_progress_for_dir(dir) +
               staggered_phase_max(list_transition_count());
    }

    if (dir == WP7_DIR_HOME) {
        return staggered_phase_max(list_transition_count()) +
               transition_blank_progress_for_dir(dir) +
               staggered_phase_max(s_wp7.tile_count);
    }

    return staggered_phase_max(s_wp7.tile_count) * 2 +
           transition_blank_progress_for_dir(dir);
}

static int32_t transition_progress_max(void)
{
    if (s_wp7.drag_dir != WP7_DIR_NONE) {
        return transition_progress_max_for_dir(s_wp7.drag_dir);
    }

    return transition_progress_max_for_dir(s_wp7.anim_dir);
}

static int32_t transition_drag_distance(void)
{
    if (is_horizontal_dir(s_wp7.drag_dir)) {
        return scaled_px(s_wp7.screen_w, WP7_DRAG_DISTANCE_PERMILLE);
    }

    return scaled_px(s_wp7.screen_h, WP7_DRAG_DISTANCE_PERMILLE);
}

static int32_t transition_drag_delta(wp7_page_dir_t dir, int32_t delta_x, int32_t delta_y)
{
    if (dir == WP7_DIR_LIST) {
        return -delta_x;
    }

    if (dir == WP7_DIR_HOME) {
        return delta_x;
    }

    if (dir == WP7_DIR_NEXT) {
        return -delta_y;
    }

    if (dir == WP7_DIR_PREV) {
        return delta_y;
    }

    return 0;
}

static int32_t transition_drag_threshold(void)
{
    return scaled_px(s_wp7.screen_h, WP7_DRAG_THRESHOLD_PERMILLE);
}

static int32_t sanitize_animation_speed(int32_t speed)
{
    return clamp_i32(speed, WP7_ANIM_SPEED_MIN, WP7_ANIM_SPEED_MAX);
}

static int32_t sanitize_brightness(int32_t brightness)
{
    return clamp_i32(brightness, WP7_BRIGHTNESS_MIN, WP7_BRIGHTNESS_MAX);
}

static int32_t sanitize_theme_index(int32_t index)
{
    return clamp_i32(index, 0, WP7_THEME_COLOR_COUNT - 1);
}

static uint32_t blend_color_hex(uint32_t from, uint32_t to, int32_t progress)
{
    progress = clamp_i32(progress, 0, WP7_TILE_PROGRESS_UNIT);

    const int32_t from_red = (from >> 16) & 0xFF;
    const int32_t from_green = (from >> 8) & 0xFF;
    const int32_t from_blue = from & 0xFF;
    const int32_t to_red = (to >> 16) & 0xFF;
    const int32_t to_green = (to >> 8) & 0xFF;
    const int32_t to_blue = to & 0xFF;
    const uint32_t red = from_red + ((to_red - from_red) * progress / WP7_TILE_PROGRESS_UNIT);
    const uint32_t green = from_green + ((to_green - from_green) * progress / WP7_TILE_PROGRESS_UNIT);
    const uint32_t blue = from_blue + ((to_blue - from_blue) * progress / WP7_TILE_PROGRESS_UNIT);

    return (red << 16) | (green << 8) | blue;
}

static uint32_t raw_theme_color_hex(int32_t index)
{
    return s_wp7_theme_colors[sanitize_theme_index(index)];
}

static uint32_t theme_color_hex(void)
{
    if (s_wp7.theme_animating) {
        return blend_color_hex(s_wp7.theme_anim_from_hex,
                               s_wp7.theme_anim_to_hex,
                               s_wp7.theme_anim_progress);
    }

    return raw_theme_color_hex(s_wp7.theme_index);
}

static lv_color_t theme_color(void)
{
    return lv_color_hex(theme_color_hex());
}

static uint32_t theme_text_color_hex_for_color(uint32_t color)
{
    const int32_t red = (color >> 16) & 0xFF;
    const int32_t green = (color >> 8) & 0xFF;
    const int32_t blue = color & 0xFF;
    const int32_t luma = red * 299 + green * 587 + blue * 114;

    return luma > 145000 ? 0x06324A : 0xFFFFFF;
}

static uint32_t theme_text_color_hex(void)
{
    if (s_wp7.theme_animating) {
        return blend_color_hex(s_wp7.theme_anim_from_text_hex,
                               s_wp7.theme_anim_to_text_hex,
                               s_wp7.theme_anim_progress);
    }

    return theme_text_color_hex_for_color(theme_color_hex());
}

static lv_color_t theme_text_color(void)
{
    return lv_color_hex(theme_text_color_hex());
}

static uint32_t raw_ui_bg_color_hex(bool dark_mode)
{
    return dark_mode ? 0x000000 : 0xF2F2F2;
}

static uint32_t raw_ui_text_color_hex(bool dark_mode)
{
    return dark_mode ? 0xEAF7FF : 0x111111;
}

static uint32_t raw_ui_track_color_hex(bool dark_mode)
{
    return dark_mode ? 0x2D2D2D : 0xD8D8D8;
}

static uint32_t raw_ui_disabled_bg_color_hex(bool dark_mode)
{
    return dark_mode ? 0x2C2C2C : 0xC8C8C8;
}

static uint32_t ui_bg_color_hex(void)
{
    if (s_wp7.ui_animating) {
        return blend_color_hex(s_wp7.ui_anim_from_bg_hex,
                               s_wp7.ui_anim_to_bg_hex,
                               s_wp7.ui_anim_progress);
    }

    return raw_ui_bg_color_hex(s_wp7.dark_mode);
}

static uint32_t ui_text_color_hex(void)
{
    if (s_wp7.ui_animating) {
        return blend_color_hex(s_wp7.ui_anim_from_text_hex,
                               s_wp7.ui_anim_to_text_hex,
                               s_wp7.ui_anim_progress);
    }

    return raw_ui_text_color_hex(s_wp7.dark_mode);
}

static lv_color_t ui_bg_color(void)
{
    return lv_color_hex(ui_bg_color_hex());
}

static lv_color_t ui_text_color(void)
{
    return lv_color_hex(ui_text_color_hex());
}

static uint32_t ui_track_color_hex(void)
{
    if (s_wp7.ui_animating) {
        return blend_color_hex(s_wp7.ui_anim_from_track_hex,
                               s_wp7.ui_anim_to_track_hex,
                               s_wp7.ui_anim_progress);
    }

    return raw_ui_track_color_hex(s_wp7.dark_mode);
}

static lv_color_t ui_track_color(void)
{
    return lv_color_hex(ui_track_color_hex());
}

static uint32_t ui_disabled_bg_color_hex(void)
{
    if (s_wp7.ui_animating) {
        return blend_color_hex(s_wp7.ui_anim_from_disabled_bg_hex,
                               s_wp7.ui_anim_to_disabled_bg_hex,
                               s_wp7.ui_anim_progress);
    }

    return raw_ui_disabled_bg_color_hex(s_wp7.dark_mode);
}

static lv_color_t ui_disabled_bg_color(void)
{
    return lv_color_hex(ui_disabled_bg_color_hex());
}

static lv_color_t ui_disabled_text_color(void)
{
    return lv_color_hex(0x777777);
}

static int32_t animation_speed_permille(void)
{
    const int32_t speed = sanitize_animation_speed(s_wp7.anim_speed_percent);

    if (speed <= WP7_ANIM_SPEED_DEFAULT) {
        return WP7_ACTUAL_SPEED_MIN_PERMILLE +
               (speed - WP7_ANIM_SPEED_MIN) *
               (WP7_ACTUAL_SPEED_DEFAULT_PERMILLE - WP7_ACTUAL_SPEED_MIN_PERMILLE) /
               (WP7_ANIM_SPEED_DEFAULT - WP7_ANIM_SPEED_MIN);
    }

    return WP7_ACTUAL_SPEED_DEFAULT_PERMILLE +
           (speed - WP7_ANIM_SPEED_DEFAULT) *
           (WP7_ACTUAL_SPEED_MAX_PERMILLE - WP7_ACTUAL_SPEED_DEFAULT_PERMILLE) /
           (WP7_ANIM_SPEED_MAX - WP7_ANIM_SPEED_DEFAULT);
}

static int32_t animation_dir_permille(wp7_page_dir_t dir)
{
    if (is_horizontal_dir(dir)) {
        return WP7_HORIZONTAL_SPEED_PERMILLE;
    }

    if (is_open_dir(dir) || is_close_dir(dir)) {
        return WP7_SETTINGS_SPEED_PERMILLE;
    }

    return 1000;
}

static int32_t animation_progress_per_ms(wp7_page_dir_t dir)
{
    const int64_t scaled_progress = (int64_t)WP7_BASE_PROGRESS_PER_MS *
                                    animation_speed_permille() *
                                    animation_dir_permille(dir);
    const int32_t progress_per_ms = (int32_t)((scaled_progress + 500000) / 1000000);

    return progress_per_ms > 0 ? progress_per_ms : 1;
}

static int32_t animation_duration_ms(wp7_page_dir_t dir, int32_t progress_delta)
{
    const int64_t scaled_progress = (int64_t)WP7_BASE_PROGRESS_PER_MS *
                                    animation_speed_permille() *
                                    animation_dir_permille(dir);
    const int64_t scaled_delta = (int64_t)progress_delta * 1000000;

    return (int32_t)((scaled_delta + scaled_progress - 1) / scaled_progress);
}

static int32_t animation_release_min_ms(wp7_page_dir_t dir)
{
    const int64_t scaled_speed = (int64_t)animation_speed_permille() *
                                 animation_dir_permille(dir);
    const int64_t scaled_duration = (int64_t)WP7_BASE_RELEASE_MIN_MS * 1000000;

    return (int32_t)((scaled_duration + scaled_speed - 1) / scaled_speed);
}

static int32_t scaled_ui_anim_ms(int32_t base_ms)
{
    const int32_t speed = animation_speed_permille();
    const int32_t ms = (int32_t)((int64_t)base_ms *
                                 WP7_ACTUAL_SPEED_DEFAULT_PERMILLE / speed);

    return clamp_i32(ms, 80, 1200);
}

static bool animation_speed_is_default(void)
{
    return sanitize_animation_speed(s_wp7.anim_speed_percent) == WP7_ANIM_SPEED_DEFAULT;
}

static void init_nvs(void)
{
    /* The Passport NVS partition can contain settings owned by other firmware.
       Never erase it as an automatic recovery step. */
    ESP_ERROR_CHECK(nvs_flash_init());
}

static void load_ui_settings(void)
{
    nvs_handle_t handle;
    uint16_t speed = WP7_ANIM_SPEED_DEFAULT;
    uint16_t brightness = WP7_BRIGHTNESS_DEFAULT;
    uint16_t theme = 7;
    uint16_t dark = 1;
    uint16_t fast = 0;

    s_wp7.anim_speed_percent = WP7_ANIM_SPEED_DEFAULT;
    s_wp7.brightness_percent = WP7_BRIGHTNESS_DEFAULT;
    s_wp7.theme_index = 7;
    s_wp7.dark_mode = true;
    s_wp7.fast_animations = false;

    if (nvs_open(WP7_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    if (nvs_get_u16(handle, WP7_NVS_SPEED_KEY, &speed) == ESP_OK) {
        s_wp7.anim_speed_percent = sanitize_animation_speed(speed);
    }

    if (nvs_get_u16(handle, WP7_NVS_BRIGHTNESS_KEY, &brightness) == ESP_OK) {
        s_wp7.brightness_percent = sanitize_brightness(brightness);
    }

    if (nvs_get_u16(handle, WP7_NVS_THEME_KEY, &theme) == ESP_OK) {
        s_wp7.theme_index = sanitize_theme_index(theme);
    }

    if (nvs_get_u16(handle, WP7_NVS_DARK_KEY, &dark) == ESP_OK) {
        s_wp7.dark_mode = dark != 0;
    }

    if (nvs_get_u16(handle, WP7_NVS_FAST_KEY, &fast) == ESP_OK) {
        s_wp7.fast_animations = fast != 0;
    }

    nvs_close(handle);
}

/* Settings are written once, a second after the last change. A flash write
   stalls the whole CPU, so writing on every key press made the slider and
   color animations of that press stutter. */
#define WP7_SAVE_DELAY_MS            1000
#define WP7_SAVE_SPEED               (1u << 0)
#define WP7_SAVE_BRIGHTNESS          (1u << 1)
#define WP7_SAVE_THEME               (1u << 2)
#define WP7_SAVE_DARK                (1u << 3)
#define WP7_SAVE_FAST                (1u << 4)

static uint8_t s_pending_saves;
static lv_timer_t *s_save_timer;

static void flush_settings_cb(lv_timer_t *timer)
{
    nvs_handle_t handle;
    const uint8_t pending = s_pending_saves;

    lv_timer_pause(timer);
    s_pending_saves = 0;

    if (pending == 0 || nvs_open(WP7_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    if (pending & WP7_SAVE_SPEED) {
        nvs_set_u16(handle, WP7_NVS_SPEED_KEY,
                    (uint16_t)sanitize_animation_speed(s_wp7.anim_speed_percent));
    }
    if (pending & WP7_SAVE_BRIGHTNESS) {
        nvs_set_u16(handle, WP7_NVS_BRIGHTNESS_KEY,
                    (uint16_t)sanitize_brightness(s_wp7.brightness_percent));
    }
    if (pending & WP7_SAVE_THEME) {
        nvs_set_u16(handle, WP7_NVS_THEME_KEY, (uint16_t)sanitize_theme_index(s_wp7.theme_index));
    }
    if (pending & WP7_SAVE_DARK) {
        nvs_set_u16(handle, WP7_NVS_DARK_KEY, s_wp7.dark_mode ? 1 : 0);
    }
    if (pending & WP7_SAVE_FAST) {
        nvs_set_u16(handle, WP7_NVS_FAST_KEY, s_wp7.fast_animations ? 1 : 0);
    }

    nvs_commit(handle);
    nvs_close(handle);
}

static void schedule_settings_save(uint8_t setting)
{
    if (s_save_timer == NULL) {
        s_save_timer = lv_timer_create(flush_settings_cb, WP7_SAVE_DELAY_MS, NULL);
        if (s_save_timer == NULL) {
            return;
        }
    }

    s_pending_saves |= setting;
    lv_timer_reset(s_save_timer);
    lv_timer_resume(s_save_timer);
}

static void save_animation_speed(void)
{
    schedule_settings_save(WP7_SAVE_SPEED);
}

static void save_brightness(void)
{
    schedule_settings_save(WP7_SAVE_BRIGHTNESS);
}

static void save_theme(void)
{
    schedule_settings_save(WP7_SAVE_THEME);
}

static void save_dark_mode(void)
{
    schedule_settings_save(WP7_SAVE_DARK);
}

static void save_fast_animations(void)
{
    schedule_settings_save(WP7_SAVE_FAST);
}

static int32_t current_knob_extend(lv_obj_t *slider)
{
    return lv_obj_get_style_transform_width(slider, LV_PART_KNOB);
}

static void knob_extend_anim_cb(void *slider, int32_t extend)
{
    lv_obj_set_style_transform_width((lv_obj_t *)slider, extend, LV_PART_KNOB);
    lv_obj_set_style_transform_height((lv_obj_t *)slider, extend, LV_PART_KNOB);
}

static void start_knob_extend_anim(lv_obj_t *slider, int32_t target_extend)
{
    if (slider == NULL) {
        return;
    }

    lv_anim_del(slider, knob_extend_anim_cb);

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, slider);
    lv_anim_set_exec_cb(&anim, knob_extend_anim_cb);
    lv_anim_set_values(&anim, current_knob_extend(slider), target_extend);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_PRESS_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void set_control_anim_duration(lv_obj_t *obj)
{
    if (obj != NULL) {
        lv_obj_set_style_anim_duration(obj, scaled_ui_anim_ms(WP7_PRESS_ANIM_MS),
                                       LV_PART_MAIN);
    }
}

static void store_control_press_point(void)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;

    if (indev == NULL) {
        s_wp7.control_press_x = 0;
        s_wp7.control_press_y = 0;
        return;
    }

    lv_indev_get_point(indev, &point);
    s_wp7.control_press_x = point.x;
    s_wp7.control_press_y = point.y;
}

static bool control_moved_past_tap_threshold(void)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;

    if (indev == NULL) {
        return false;
    }

    lv_indev_get_point(indev, &point);

    const int32_t threshold = scaled_px(s_wp7.screen_w, 18);

    return abs_i32(point.x - s_wp7.control_press_x) > threshold ||
           abs_i32(point.y - s_wp7.control_press_y) > threshold;
}

static int32_t slider_value_from_active_point(lv_obj_t *slider)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    lv_area_t coords;

    if (indev == NULL || slider == NULL) {
        return slider != NULL ? lv_slider_get_value(slider) : 0;
    }

    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(slider, &coords);

    const int32_t min = lv_slider_get_min_value(slider);
    const int32_t max = lv_slider_get_max_value(slider);
    const int32_t pad_left = lv_obj_get_style_pad_left(slider, LV_PART_MAIN);
    const int32_t pad_right = lv_obj_get_style_pad_right(slider, LV_PART_MAIN);
    const int32_t left = coords.x1 + pad_left;
    const int32_t right = coords.x2 - pad_right;
    const int32_t width = right - left;

    if (width <= 0 || max <= min) {
        return lv_slider_get_value(slider);
    }

    const int32_t x = clamp_i32(point.x, left, right) - left;

    return min + (int32_t)(((int64_t)x * (max - min) + width / 2) / width);
}

static void animate_slider_to_value(lv_obj_t *slider, int32_t value)
{
    if (slider == NULL) {
        return;
    }

    set_control_anim_duration(slider);
    lv_slider_set_value(slider, value, LV_ANIM_ON);
}

static void brightness_anim_cb(void *var, int32_t value)
{
    (void)var;

    s_wp7.brightness_hw_percent = sanitize_brightness(value);
    bsp_display_backlight(s_wp7.brightness_hw_percent);
}

static void set_brightness_immediate(int32_t value)
{
    value = sanitize_brightness(value);

    lv_anim_del(&s_wp7, brightness_anim_cb);
    s_wp7.brightness_percent = value;
    s_wp7.brightness_hw_percent = value;
    bsp_display_backlight(value);
}

static void animate_brightness_to(int32_t target)
{
    target = sanitize_brightness(target);

    lv_anim_del(&s_wp7, brightness_anim_cb);
    s_wp7.brightness_percent = target;

    if (s_wp7.brightness_hw_percent <= 0) {
        s_wp7.brightness_hw_percent = sanitize_brightness(s_wp7.brightness_percent);
    }

    if (s_wp7.brightness_hw_percent == target) {
        bsp_display_backlight(target);
        return;
    }

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s_wp7);
    lv_anim_set_exec_cb(&anim, brightness_anim_cb);
    lv_anim_set_values(&anim, s_wp7.brightness_hw_percent, target);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_PRESS_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static bool wp7_switch_value_checked(lv_obj_t *sw)
{
    return sw != NULL && lv_slider_get_value(sw) >= WP7_SWITCH_THRESHOLD;
}

static void set_wp7_switch_value(lv_obj_t *sw, bool checked, lv_anim_enable_t anim)
{
    if (sw == NULL) {
        return;
    }

    set_control_anim_duration(sw);
    lv_slider_set_value(sw, checked ? WP7_SWITCH_VALUE_MAX : 0, anim);
}

static void set_wp7_switch_style(lv_obj_t *sw, bool checked)
{
    if (sw == NULL) {
        return;
    }

    if (lv_obj_has_state(sw, LV_STATE_CHECKED) != checked) {
        if (checked) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
    }

    lv_obj_set_style_bg_color(sw, ui_track_color(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, ui_track_color(), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, ui_track_color(), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, ui_track_color(),
                              LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, theme_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, theme_color(), LV_PART_INDICATOR | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, theme_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, theme_color(),
                              LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, ui_text_color(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, ui_text_color(), LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, ui_text_color(), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, ui_text_color(),
                              LV_PART_KNOB | LV_STATE_CHECKED | LV_STATE_PRESSED);
}

static void update_wp7_switch_accent(lv_obj_t *sw)
{
    if (sw == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(sw, theme_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, theme_color(), LV_PART_INDICATOR | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, theme_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, theme_color(),
                              LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, ui_text_color(), LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, ui_text_color(), LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(sw, ui_text_color(), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, ui_text_color(),
                              LV_PART_KNOB | LV_STATE_CHECKED | LV_STATE_PRESSED);
}

static void set_tile_number(wp7_tile_t *tile, int32_t page, int32_t index)
{
    static const char *const names[] = {
        "Kaboo", "Claude", "Clock", "Battery", "Stopwatch", "UI\nSettings",
    };
    if (tile->label_page == page && tile->label_index == index) {
        return;
    }

    if (page == 0 && index >= 0 && index < (int32_t)(sizeof(names) / sizeof(names[0]))) {
        lv_label_set_text(tile->label, names[index]);
    } else {
        lv_label_set_text_fmt(tile->label, "%ld", (long)(page * s_wp7.tile_count + index + 1));
    }

    lv_obj_set_style_text_color(tile->label, theme_text_color(), 0);
    lv_obj_set_style_text_align(tile->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(tile->label);
    tile->label_page = page;
    tile->label_index = index;
}

static void set_tile_hidden(wp7_tile_t *tile, bool hidden)
{
    if (tile->hidden == hidden) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(tile->obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(tile->obj, LV_OBJ_FLAG_HIDDEN);
    }

    tile->hidden = hidden;
}

static void set_tile_visual_style(wp7_tile_t *tile, lv_opa_t opa, int32_t scale)
{
    if (tile->style_valid && tile->current_opa == opa && tile->current_scale == scale) {
        return;
    }

    lv_obj_set_style_opa(tile->obj, opa, 0);
    lv_obj_set_style_transform_scale(tile->obj, scale, 0);
    tile->current_opa = opa;
    tile->current_scale = scale;
    tile->style_valid = true;
}

static void set_tile_frame(wp7_tile_t *tile, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const bool position_changed = !tile->frame_valid ||
                                  tile->current_x != x ||
                                  tile->current_y != y;
    const bool size_changed = !tile->frame_valid ||
                              tile->current_w != w ||
                              tile->current_h != h;

    if (!position_changed && !size_changed) {
        return;
    }

    if (position_changed) {
        lv_obj_set_pos(tile->obj, x, y);
        tile->current_x = x;
        tile->current_y = y;
    }

    if (size_changed) {
        lv_obj_set_size(tile->obj, w, h);
        lv_obj_center(tile->label);
        tile->current_w = w;
        tile->current_h = h;
    }

    tile->frame_valid = true;
}

static bool tile_release_anim_holds_full_frame(wp7_tile_t *tile,
                                               int32_t x,
                                               int32_t y,
                                               int32_t w,
                                               int32_t h,
                                               lv_opa_t opa)
{
    return tile->press_animating &&
           tile->press_releasing &&
           tile->current_w < tile->size &&
           x == tile->x &&
           y == tile->y &&
           w == tile->size &&
           h == tile->size &&
           opa == LV_OPA_COVER;
}

static void apply_theme_to_tiles(void)
{
    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        if (s_wp7.tiles[i].obj == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(s_wp7.tiles[i].obj, theme_color(), 0);
        lv_obj_set_style_bg_color(s_wp7.tiles[i].obj, theme_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s_wp7.tiles[i].obj, theme_color(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(s_wp7.tiles[i].obj, theme_color(),
                                  LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_wp7.tiles[i].obj, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_wp7.tiles[i].obj, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_wp7.tiles[i].obj, LV_OPA_COVER,
                                LV_STATE_PRESSED | LV_STATE_FOCUSED);

        if (s_wp7.tiles[i].label != NULL) {
            lv_obj_set_style_text_color(s_wp7.tiles[i].label, theme_text_color(), 0);
        }
    }

    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        if (s_wp7.list_items[i].icon != NULL) {
            lv_obj_set_style_bg_color(s_wp7.list_items[i].icon, theme_color(), 0);
            lv_obj_set_style_bg_color(s_wp7.list_items[i].icon, theme_color(), LV_STATE_PRESSED);
            lv_obj_set_style_bg_color(s_wp7.list_items[i].icon, theme_color(), LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(s_wp7.list_items[i].icon, theme_color(),
                                      LV_STATE_PRESSED | LV_STATE_FOCUSED);
        }

        if (s_wp7.list_items[i].icon_label != NULL) {
            lv_obj_set_style_text_color(s_wp7.list_items[i].icon_label, theme_text_color(), 0);
        }
    }
}

static void apply_color_scheme(void)
{
    lv_obj_t *screen = lv_screen_active();

    if (screen != NULL) {
        lv_obj_set_style_bg_color(screen, ui_bg_color(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    }

    if (s_wp7.status_wifi_label != NULL) {
        lv_obj_set_style_text_color(s_wp7.status_wifi_label, ui_text_color(), 0);
    }

    if (s_wp7.status_ble_label != NULL) {
        lv_obj_set_style_text_color(s_wp7.status_ble_label, ui_text_color(), 0);
    }

    if (s_wp7.status_time_label != NULL) {
        lv_obj_set_style_text_color(s_wp7.status_time_label, ui_text_color(), 0);
    }

    if (s_wp7.status_battery_label != NULL) {
        lv_obj_set_style_text_color(s_wp7.status_battery_label, ui_text_color(), 0);
    }

    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        if (s_wp7.list_items[i].obj != NULL) {
            lv_obj_set_style_bg_color(s_wp7.list_items[i].obj, ui_bg_color(), 0);
            lv_obj_set_style_bg_color(s_wp7.list_items[i].obj, ui_bg_color(), LV_STATE_PRESSED);
            lv_obj_set_style_bg_color(s_wp7.list_items[i].obj, ui_bg_color(), LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(s_wp7.list_items[i].obj, ui_bg_color(),
                                      LV_STATE_PRESSED | LV_STATE_FOCUSED);
        }

        if (s_wp7.list_items[i].label != NULL) {
            lv_obj_set_style_text_color(s_wp7.list_items[i].label, ui_text_color(), 0);
        }
    }

    if (s_wp7.settings_title != NULL) {
        lv_obj_set_style_text_color(s_wp7.settings_title, ui_text_color(), 0);
        s_wp7.settings_title_current_text_hex = ui_text_color_hex();
    }

    for (int32_t i = 0; i < WP7_SETTINGS_CONTENT_COUNT; i++) {
        if (s_wp7.settings_items[i].obj != NULL) {
            lv_obj_set_style_text_color(s_wp7.settings_items[i].obj, ui_text_color(), 0);
        }
    }

    if (s_wp7.brightness_slider != NULL) {
        lv_obj_set_style_bg_color(s_wp7.brightness_slider, ui_track_color(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_wp7.brightness_slider, ui_text_color(), LV_PART_KNOB);
    }

    if (s_wp7.settings_slider != NULL) {
        lv_obj_set_style_bg_color(s_wp7.settings_slider, ui_track_color(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_wp7.settings_slider, ui_text_color(), LV_PART_KNOB);
    }

    set_wp7_switch_style(s_wp7.mode_switch, s_wp7.dark_mode);
    set_wp7_switch_style(s_wp7.fast_switch, s_wp7.fast_animations);

    if (s_wp7.mode_label != NULL) {
        lv_obj_set_style_text_color(s_wp7.mode_label, ui_text_color(), 0);
    }
}

static void set_tile_geometry(wp7_tile_t *tile, int32_t y, int32_t height)
{
    if (height <= 0) {
        set_tile_hidden(tile, true);
        return;
    }

    if (tile_release_anim_holds_full_frame(tile, tile->x, y, tile->size,
                                           height, LV_OPA_COVER)) {
        return;
    }

    set_tile_hidden(tile, false);
    set_tile_visual_style(tile, LV_OPA_COVER, 256);
    set_tile_frame(tile, tile->x, y, tile->size, height);
}

static void set_tile_x(wp7_tile_t *tile, int32_t x)
{
    if (x <= -tile->size || x >= s_wp7.screen_w) {
        set_tile_hidden(tile, true);
        return;
    }

    if (tile_release_anim_holds_full_frame(tile, x, tile->y, tile->size,
                                           tile->size, LV_OPA_COVER)) {
        return;
    }

    set_tile_hidden(tile, false);
    set_tile_visual_style(tile, LV_OPA_COVER, 256);
    set_tile_frame(tile, x, tile->y, tile->size, tile->size);
}

static void set_tile_width(wp7_tile_t *tile, int32_t width)
{
    if (width <= 0) {
        set_tile_hidden(tile, true);
        return;
    }

    if (tile_release_anim_holds_full_frame(tile, tile->x, tile->y, width,
                                           tile->size, LV_OPA_COVER)) {
        return;
    }

    set_tile_hidden(tile, false);
    set_tile_visual_style(tile, LV_OPA_COVER, 256);
    set_tile_frame(tile, tile->x, tile->y, width, tile->size);
}

static void set_tile_box(wp7_tile_t *tile, int32_t x, int32_t y, int32_t w, int32_t h, lv_opa_t opa)
{
    if (w <= 0 || h <= 0 || opa == 0) {
        set_tile_hidden(tile, true);
        return;
    }

    if (tile_release_anim_holds_full_frame(tile, x, y, w, h, opa)) {
        return;
    }

    set_tile_hidden(tile, false);
    set_tile_visual_style(tile, opa, 256);
    set_tile_frame(tile, x, y, w, h);
}

static void set_tile_zoom_box(wp7_tile_t *tile, int32_t scale, lv_opa_t opa)
{
    const int32_t size = tile->size * scale / 256;
    const int32_t x = tile->x - (size - tile->size) / 2;
    const int32_t y = tile->y - (size - tile->size) / 2;

    set_tile_box(tile, x, y, size, size, opa);
}

static void set_list_item_geometry(wp7_list_item_t *item, int32_t x)
{
    set_list_item_box(item, x, LV_OPA_COVER, WP7_OBJ_RELEASE_SCALE);
}

static void set_list_item_hidden(wp7_list_item_t *item, bool hidden)
{
    if (item->hidden == hidden) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(item->obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(item->obj, LV_OBJ_FLAG_HIDDEN);
    }

    item->hidden = hidden;
}

static void align_list_item_content(wp7_list_item_t *item)
{
    if (item->icon != NULL) {
        lv_obj_align(item->icon, LV_ALIGN_LEFT_MID, 0, 0);
    }

    if (item->icon_label != NULL) {
        lv_obj_center(item->icon_label);
    }

    if (item->label != NULL && item->icon != NULL) {
        lv_obj_align_to(item->label, item->icon, LV_ALIGN_OUT_RIGHT_MID,
                        scaled_px(s_wp7.screen_w, 28), 0);
    }
}

static bool set_list_item_icon_size(wp7_list_item_t *item, int32_t size)
{
    if (item == NULL || item->icon == NULL || size <= 0) {
        return false;
    }

    if (item->current_icon_size == size) {
        return false;
    }

    lv_obj_set_size(item->icon, size, size);
    item->current_icon_size = size;
    return true;
}

static void set_list_item_visual_style(wp7_list_item_t *item, lv_opa_t opa, int32_t scale)
{
    if (item->style_valid &&
            item->current_opa == opa &&
            item->current_scale == scale) {
        return;
    }

    lv_obj_set_style_opa(item->obj, opa, 0);
    item->current_opa = opa;
    item->current_scale = scale;
    item->style_valid = true;
}

static void set_list_item_box(wp7_list_item_t *item, int32_t x, lv_opa_t opa, int32_t scale)
{
    bool layout_changed = false;

    /* Rows of a destroyed list page are zeroed; never touch their objects. */
    if (item->obj == NULL) {
        return;
    }

    if (opa == 0 || scale <= 0) {
        set_list_item_hidden(item, true);
        return;
    }

    const int32_t w = item->w * scale / WP7_OBJ_RELEASE_SCALE;
    const int32_t h = item->h * scale / WP7_OBJ_RELEASE_SCALE;
    const int32_t scaled_x = x - (w - item->w) / 2;
    const int32_t scaled_y = item->y - (h - item->h) / 2;

    if (scaled_x <= -w || scaled_x >= s_wp7.screen_w || w <= 0 || h <= 0) {
        set_list_item_hidden(item, true);
        return;
    }

    set_list_item_hidden(item, false);

    const bool position_changed = !item->frame_valid ||
                                  item->current_x != scaled_x ||
                                  item->current_y != scaled_y;
    const bool size_changed = !item->frame_valid ||
                              item->current_w != w ||
                              item->current_h != h;

    if (position_changed) {
        lv_obj_set_pos(item->obj, scaled_x, scaled_y);
        item->current_x = scaled_x;
        item->current_y = scaled_y;
        layout_changed = true;
    }

    if (size_changed) {
        lv_obj_set_size(item->obj, w, h);
        item->current_w = w;
        item->current_h = h;
        layout_changed = true;
    }

    const int32_t icon_size = item->icon_size * scale / WP7_OBJ_RELEASE_SCALE;

    if (set_list_item_icon_size(item, icon_size)) {
        layout_changed = true;
    }

    item->frame_valid = true;

    set_list_item_visual_style(item, opa, scale);

    if (layout_changed) {
        align_list_item_content(item);
    }
}

static void set_setting_item_geometry(wp7_setting_item_t *item, int32_t x, lv_opa_t opa)
{
    if (x <= -item->w || x >= s_wp7.screen_w || opa == 0) {
        if (!item->hidden) {
            lv_obj_add_flag(item->obj, LV_OBJ_FLAG_HIDDEN);
            item->hidden = true;
        }

        return;
    }

    if (item->hidden) {
        lv_obj_remove_flag(item->obj, LV_OBJ_FLAG_HIDDEN);
        item->hidden = false;
    }

    if (!item->style_valid || item->current_opa != opa) {
        lv_obj_set_style_opa(item->obj, opa, 0);
        item->current_opa = opa;
        item->style_valid = true;
    }

    if (!item->frame_valid || item->current_x != x) {
        lv_obj_set_pos(item->obj, x, item->y);
        item->current_x = x;
    }

    if (!item->frame_valid) {
        lv_obj_set_size(item->obj, item->w, item->h);
        item->frame_valid = true;
    }
}

static void set_settings_title_geometry(int32_t x, int32_t scale, lv_opa_t opa)
{
    if (opa == 0 || scale <= 0) {
        if (!s_wp7.settings_title_hidden) {
            lv_obj_add_flag(s_wp7.settings_title, LV_OBJ_FLAG_HIDDEN);
            s_wp7.settings_title_hidden = true;
        }

        return;
    }

    if (s_wp7.settings_title_hidden) {
        lv_obj_remove_flag(s_wp7.settings_title, LV_OBJ_FLAG_HIDDEN);
        s_wp7.settings_title_hidden = false;
    }

    if (!s_wp7.settings_title_style_valid ||
            s_wp7.settings_title_current_opa != opa ||
            s_wp7.settings_title_current_scale != scale ||
            s_wp7.settings_title_current_text_hex != ui_text_color_hex()) {
        lv_obj_set_style_transform_scale(s_wp7.settings_title, scale, 0);
        lv_obj_set_style_text_color(s_wp7.settings_title, ui_text_color(), 0);
        s_wp7.settings_title_current_opa = opa;
        s_wp7.settings_title_current_scale = scale;
        s_wp7.settings_title_current_text_hex = ui_text_color_hex();
        s_wp7.settings_title_style_valid = true;
    }

    if (!s_wp7.settings_title_frame_valid || s_wp7.settings_title_current_x != x) {
        lv_obj_set_pos(s_wp7.settings_title, x, s_wp7.settings_title_y);
        lv_obj_set_size(s_wp7.settings_title, s_wp7.settings_title_w, s_wp7.settings_title_h);
        s_wp7.settings_title_current_x = x;
        s_wp7.settings_title_frame_valid = true;
    }
}

/* The app page title, item 0, scales about its left edge like the UI
   Settings title. */
static void set_app_title_geometry(int32_t x, int32_t scale, lv_opa_t opa)
{
    if (s_wp7.app_item_count <= 0) {
        return;
    }

    if (opa == 0 || scale <= 0) {
        opa = 0;
    } else if (s_wp7.app_title_scale != scale) {
        lv_obj_set_style_transform_scale(s_wp7.app_items[0].obj, scale, 0);
        s_wp7.app_title_scale = scale;
    }

    set_setting_item_geometry(&s_wp7.app_items[0], x, opa);
}

static void show_status_bar(void)
{
    if (s_wp7.status_bar != NULL) {
        lv_obj_remove_flag(s_wp7.status_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_tiles(void)
{
    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        set_tile_hidden(&s_wp7.tiles[i], true);
    }
}

static void hide_list_items(void)
{
    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        if (s_wp7.list_items[i].obj != NULL) {
            if (!s_wp7.list_items[i].hidden) {
                lv_obj_add_flag(s_wp7.list_items[i].obj, LV_OBJ_FLAG_HIDDEN);
                s_wp7.list_items[i].hidden = true;
            }
        }
    }
}

static void hide_settings_items(void)
{
    if (s_wp7.settings_title != NULL) {
        if (!s_wp7.settings_title_hidden) {
            lv_obj_add_flag(s_wp7.settings_title, LV_OBJ_FLAG_HIDDEN);
            s_wp7.settings_title_hidden = true;
        }

        if (!s_wp7.settings_title_style_valid ||
                s_wp7.settings_title_current_scale != 256 ||
                s_wp7.settings_title_current_opa != LV_OPA_COVER ||
                s_wp7.settings_title_current_text_hex != ui_text_color_hex()) {
            lv_obj_set_style_transform_scale(s_wp7.settings_title, 256, 0);
            lv_obj_set_style_text_color(s_wp7.settings_title, ui_text_color(), 0);
            s_wp7.settings_title_current_scale = 256;
            s_wp7.settings_title_current_opa = LV_OPA_COVER;
            s_wp7.settings_title_current_text_hex = ui_text_color_hex();
            s_wp7.settings_title_style_valid = true;
        }
    }

    for (int32_t i = 0; i < WP7_SETTINGS_CONTENT_COUNT; i++) {
        if (s_wp7.settings_items[i].obj != NULL) {
            if (!s_wp7.settings_items[i].hidden) {
                lv_obj_add_flag(s_wp7.settings_items[i].obj, LV_OBJ_FLAG_HIDDEN);
                s_wp7.settings_items[i].hidden = true;
            }

            if (!s_wp7.settings_items[i].style_valid ||
                    s_wp7.settings_items[i].current_opa != LV_OPA_COVER) {
                lv_obj_set_style_opa(s_wp7.settings_items[i].obj, LV_OPA_COVER, 0);
                s_wp7.settings_items[i].current_opa = LV_OPA_COVER;
                s_wp7.settings_items[i].style_valid = true;
            }
        }
    }
}

static void hide_rows(wp7_setting_item_t *items, int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        if (items[i].obj != NULL && !items[i].hidden) {
            lv_obj_add_flag(items[i].obj, LV_OBJ_FLAG_HIDDEN);
            items[i].hidden = true;
        }
    }
}

static bool list_page_created(void)
{
    return s_wp7.list_count > 0 && s_wp7.list_items[0].obj != NULL;
}

static bool settings_page_created(void)
{
    return s_wp7.settings_title != NULL;
}

static int32_t wp7_status_height(void)
{
    return scaled_px(s_wp7.screen_h, WP7_STATUS_BAR_PERMILLE);
}

static void destroy_list_page(void)
{
    if (s_wp7.list_count <= 0) {
        return;
    }

    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        if (s_wp7.list_items[i].obj != NULL) {
            lv_obj_delete(s_wp7.list_items[i].obj);
        }

        s_wp7.list_items[i] = (wp7_list_item_t) { 0 };
    }

    s_wp7.list_count = 0;
}

static void destroy_settings_page(void)
{
    if (!settings_page_created()) {
        return;
    }

    if (s_wp7.settings_title != NULL) {
        lv_obj_delete(s_wp7.settings_title);
        s_wp7.settings_title = NULL;
    }

    for (int32_t i = 0; i < WP7_SETTINGS_CONTENT_COUNT; i++) {
        if (s_wp7.settings_items[i].obj != NULL) {
            lv_obj_delete(s_wp7.settings_items[i].obj);
        }

        s_wp7.settings_items[i] = (wp7_setting_item_t) { 0 };
    }

    s_wp7.brightness_slider = NULL;
    s_wp7.mode_switch = NULL;
    s_wp7.mode_label = NULL;
    s_wp7.settings_slider = NULL;
    s_wp7.fast_switch = NULL;
    s_wp7.settings_reset_button = NULL;
    s_wp7.settings_reset_label = NULL;
    s_wp7.theme_swatch_cell_size = 0;
    s_wp7.settings_title_current_x = 0;
    s_wp7.settings_title_current_scale = 0;
    s_wp7.settings_title_current_opa = 0;
    s_wp7.settings_title_current_text_hex = 0;
    s_wp7.settings_title_hidden = false;
    s_wp7.settings_title_frame_valid = false;
    s_wp7.settings_title_style_valid = false;

    for (int32_t i = 0; i < WP7_THEME_ROW_COUNT; i++) {
        s_wp7.theme_rows[i] = NULL;
    }

    for (int32_t i = 0; i < WP7_THEME_COLOR_COUNT; i++) {
        s_wp7.theme_buttons[i] = NULL;
    }
}

static void destroy_app_page(void)
{
    wp7_apps_close();

    for (int32_t i = 0; i < WP7_APP_MAX_ITEMS; i++) {
        s_wp7.app_items[i] = (wp7_setting_item_t) { 0 };
    }

    s_wp7.app_item_count = 0;
}

static void ensure_list_page(void)
{
    if (!list_page_created()) {
        create_list_page(lv_screen_active(), s_wp7.screen_w, s_wp7.screen_h, wp7_status_height());
    }
}

static void ensure_settings_page(void)
{
    if (!settings_page_created()) {
        create_settings_page(lv_screen_active(), s_wp7.screen_w, s_wp7.screen_h, wp7_status_height());
    }
}

/* Builds the page of s_wp7.open_app, hidden, and takes its parts for the
   transitions. Its items start at rest and visible inside the hidden page. */
static void ensure_app_page(void)
{
    wp7_app_item_t items[WP7_APP_MAX_ITEMS];

    if (wp7_apps_active() ||
            !wp7_apps_open(lv_screen_active(), s_wp7.open_app, wp7_status_height(),
                           ui_bg_color(), ui_text_color(), theme_color())) {
        return;
    }

    s_wp7.app_item_count = wp7_apps_items(items, WP7_APP_MAX_ITEMS);

    for (int32_t i = 0; i < s_wp7.app_item_count; i++) {
        s_wp7.app_items[i] = (wp7_setting_item_t) {
            .obj = items[i].obj,
            .x = items[i].x,
            .y = items[i].y,
            .w = items[i].w,
            .h = items[i].h,
            .current_x = items[i].x,
            .current_opa = LV_OPA_COVER,
            .frame_valid = true,
            .style_valid = true,
        };
    }

    s_wp7.app_title_scale = 256;
}

static int32_t tile_row(int32_t index)
{
    return index / WP7_COLUMNS;
}

static int32_t tile_col(int32_t index)
{
    return index % WP7_COLUMNS;
}

static int32_t tile_distance_from_settings(int32_t index)
{
    return abs_i32(tile_row(index) - tile_row(s_wp7.settings_tile_index)) +
           abs_i32(tile_col(index) - tile_col(s_wp7.settings_tile_index));
}

static int32_t ordered_settings_other_index(int32_t order)
{
    const int32_t clicked = s_wp7.settings_tile_index;

    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        int32_t rank = 0;

        if (i == clicked) {
            continue;
        }

        for (int32_t j = 0; j < s_wp7.tile_count; j++) {
            if (j == clicked || j == i) {
                continue;
            }

            const int32_t dist_i = tile_distance_from_settings(i);
            const int32_t dist_j = tile_distance_from_settings(j);

            if (dist_j < dist_i || (dist_j == dist_i && j < i)) {
                rank++;
            }
        }

        if (rank == order) {
            return i;
        }
    }

    return clicked;
}

static int32_t list_distance_from_settings(int32_t index)
{
    return abs_i32(index - s_wp7.settings_list_index);
}

static int32_t ordered_settings_list_other_index(int32_t order)
{
    const int32_t clicked = s_wp7.settings_list_index;
    const int32_t count = list_transition_count();

    for (int32_t i = 0; i < count; i++) {
        int32_t rank = 0;

        if (i == clicked) {
            continue;
        }

        for (int32_t j = 0; j < count; j++) {
            if (j == clicked || j == i) {
                continue;
            }

            const int32_t dist_i = list_distance_from_settings(i);
            const int32_t dist_j = list_distance_from_settings(j);

            if (dist_j < dist_i || (dist_j == dist_i && j < i)) {
                rank++;
            }
        }

        if (rank == order) {
            return i;
        }
    }

    return clicked;
}

static int32_t ordered_out_index(wp7_page_dir_t dir, int32_t index)
{
    return dir == WP7_DIR_PREV ? s_wp7.tile_count - 1 - index : index;
}

static int32_t ordered_column_index(int32_t order)
{
    const int32_t rows = s_wp7.tile_count / WP7_COLUMNS;

    if (order < rows) {
        return order * WP7_COLUMNS;
    }

    return (order - rows) * WP7_COLUMNS + 1;
}

static int32_t step_progress(int32_t progress, int32_t order)
{
    return clamp_i32(progress - order * animation_stagger_unit(), 0, WP7_TILE_PROGRESS_UNIT);
}

static int32_t phase_progress(int32_t progress, int32_t phase_unit)
{
    progress = clamp_i32(progress, 0, phase_unit);

    return (int32_t)((int64_t)progress * WP7_TILE_PROGRESS_UNIT / phase_unit);
}

static int32_t ease_in_out_cubic(int32_t progress)
{
    progress = clamp_i32(progress, 0, WP7_TILE_PROGRESS_UNIT);

    if (progress < WP7_TILE_PROGRESS_UNIT / 2) {
        return (int32_t)(4LL * progress * progress * progress /
                         (WP7_TILE_PROGRESS_UNIT * WP7_TILE_PROGRESS_UNIT));
    }

    int32_t inverse = WP7_TILE_PROGRESS_UNIT - progress;

    return WP7_TILE_PROGRESS_UNIT -
           (int32_t)(4LL * inverse * inverse * inverse /
                     (WP7_TILE_PROGRESS_UNIT * WP7_TILE_PROGRESS_UNIT));
}

static void render_static_page(int32_t page)
{
    show_status_bar();
    destroy_list_page();
    destroy_settings_page();
    destroy_app_page();

    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        wp7_tile_t *tile = &s_wp7.tiles[i];

        set_tile_number(tile, page, i);
        lv_obj_remove_flag(tile->label, LV_OBJ_FLAG_HIDDEN);
        set_tile_geometry(tile, tile->y, tile->size);
    }
}

static void render_static_list(void)
{
    show_status_bar();
    ensure_list_page();
    destroy_settings_page();
    destroy_app_page();
    hide_tiles();

    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        set_list_item_geometry(&s_wp7.list_items[i], s_wp7.list_items[i].x);
    }
}

static void render_static_app(void)
{
    show_status_bar();
    destroy_list_page();
    destroy_settings_page();
    hide_tiles();
    ensure_app_page();
    wp7_apps_set_visible(true);

    if (s_wp7.app_item_count > 0) {
        set_app_title_geometry(s_wp7.app_items[0].x, 256, LV_OPA_COVER);
    }

    for (int32_t i = 1; i < s_wp7.app_item_count; i++) {
        set_setting_item_geometry(&s_wp7.app_items[i], s_wp7.app_items[i].x, LV_OPA_COVER);
    }

    /* Quota bars fill and other page animations start once the page is in. */
    wp7_apps_entered();
}

static void update_reset_button_state(void)
{
    if (s_wp7.settings_reset_button == NULL || s_wp7.settings_reset_label == NULL) {
        return;
    }

    if (animation_speed_is_default()) {
        lv_obj_add_state(s_wp7.settings_reset_button, LV_STATE_DISABLED);
        lv_obj_remove_flag(s_wp7.settings_reset_button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button, ui_disabled_bg_color(), 0);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button,
                                  ui_disabled_bg_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button,
                                  ui_disabled_bg_color(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button, ui_disabled_bg_color(),
                                  LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER,
                                LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER,
                                LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER,
                                LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(s_wp7.settings_reset_label, ui_disabled_text_color(), 0);
    } else {
        lv_obj_remove_state(s_wp7.settings_reset_button, LV_STATE_DISABLED);
        lv_obj_add_flag(s_wp7.settings_reset_button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button, theme_color(), 0);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button, theme_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button, theme_color(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(s_wp7.settings_reset_button, theme_color(),
                                  LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER,
                                LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER,
                                LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER,
                                LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(s_wp7.settings_reset_label, theme_text_color(), 0);
    }
}

static void update_theme_buttons(void)
{
    for (int32_t i = 0; i < WP7_THEME_COLOR_COUNT; i++) {
        if (s_wp7.theme_buttons[i] == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(s_wp7.theme_buttons[i], lv_color_hex(raw_theme_color_hex(i)), 0);
        lv_obj_set_style_border_width(s_wp7.theme_buttons[i], 0, 0);
    }
}

static void update_theme_accent_controls(void)
{
    if (s_wp7.brightness_slider != NULL) {
        lv_obj_set_style_bg_color(s_wp7.brightness_slider, theme_color(), LV_PART_INDICATOR);
    }

    if (s_wp7.settings_slider != NULL) {
        lv_obj_set_style_bg_color(s_wp7.settings_slider, theme_color(), LV_PART_INDICATOR);
    }

    update_wp7_switch_accent(s_wp7.mode_switch);
    update_wp7_switch_accent(s_wp7.fast_switch);

    update_reset_button_state();
}

static void update_theme_controls(void)
{
    apply_color_scheme();

    apply_theme_to_tiles();
    update_theme_buttons();
    update_theme_accent_controls();
}

static void update_color_scheme_anim_controls(void)
{
    apply_color_scheme();
    update_theme_accent_controls();
}

static void theme_color_anim_cb(void *var, int32_t progress)
{
    (void)var;

    s_wp7.theme_anim_progress = progress;
    update_theme_accent_controls();
}

static void theme_color_anim_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    s_wp7.theme_animating = false;
    s_wp7.theme_anim_progress = WP7_TILE_PROGRESS_UNIT;
    update_theme_controls();
    save_theme();
}

static void start_theme_color_anim(uint32_t from_color,
                                   uint32_t from_text_color,
                                   uint32_t to_color,
                                   uint32_t to_text_color)
{
    lv_anim_del(&s_wp7, theme_color_anim_cb);

    s_wp7.theme_anim_from_hex = from_color;
    s_wp7.theme_anim_to_hex = to_color;
    s_wp7.theme_anim_from_text_hex = from_text_color;
    s_wp7.theme_anim_to_text_hex = to_text_color;
    s_wp7.theme_anim_progress = 0;
    s_wp7.theme_animating = from_color != to_color ||
                             from_text_color != to_text_color;

    if (!s_wp7.theme_animating) {
        update_theme_controls();
        save_theme();
        return;
    }

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s_wp7);
    lv_anim_set_exec_cb(&anim, theme_color_anim_cb);
    lv_anim_set_values(&anim, 0, WP7_TILE_PROGRESS_UNIT);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_THEME_COLOR_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, theme_color_anim_completed_cb);
    lv_anim_start(&anim);
}

static void color_scheme_anim_cb(void *var, int32_t progress)
{
    (void)var;

    s_wp7.ui_anim_progress = progress;
    update_color_scheme_anim_controls();
}

static void color_scheme_anim_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    s_wp7.ui_animating = false;
    s_wp7.ui_anim_progress = WP7_TILE_PROGRESS_UNIT;
    update_color_scheme_anim_controls();
}

static void start_color_scheme_anim(bool dark_mode)
{
    const uint32_t from_bg = ui_bg_color_hex();
    const uint32_t from_text = ui_text_color_hex();
    const uint32_t from_track = ui_track_color_hex();
    const uint32_t from_disabled_bg = ui_disabled_bg_color_hex();
    const uint32_t to_bg = raw_ui_bg_color_hex(dark_mode);
    const uint32_t to_text = raw_ui_text_color_hex(dark_mode);
    const uint32_t to_track = raw_ui_track_color_hex(dark_mode);
    const uint32_t to_disabled_bg = raw_ui_disabled_bg_color_hex(dark_mode);

    lv_anim_del(&s_wp7, color_scheme_anim_cb);

    s_wp7.dark_mode = dark_mode;
    s_wp7.ui_anim_from_bg_hex = from_bg;
    s_wp7.ui_anim_to_bg_hex = to_bg;
    s_wp7.ui_anim_from_text_hex = from_text;
    s_wp7.ui_anim_to_text_hex = to_text;
    s_wp7.ui_anim_from_track_hex = from_track;
    s_wp7.ui_anim_to_track_hex = to_track;
    s_wp7.ui_anim_from_disabled_bg_hex = from_disabled_bg;
    s_wp7.ui_anim_to_disabled_bg_hex = to_disabled_bg;
    s_wp7.ui_anim_progress = 0;
    s_wp7.ui_animating = from_bg != to_bg ||
                          from_text != to_text ||
                          from_track != to_track ||
                          from_disabled_bg != to_disabled_bg;

    if (!s_wp7.ui_animating) {
        update_color_scheme_anim_controls();
        return;
    }

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s_wp7);
    lv_anim_set_exec_cb(&anim, color_scheme_anim_cb);
    lv_anim_set_values(&anim, 0, WP7_TILE_PROGRESS_UNIT);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_THEME_COLOR_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, color_scheme_anim_completed_cb);
    lv_anim_start(&anim);
}

static int32_t theme_swatch_size(bool selected)
{
    const int32_t permille = selected ? WP7_THEME_SWATCH_SELECTED_PERMILLE :
                            WP7_THEME_SWATCH_IDLE_PERMILLE;

    return s_wp7.theme_swatch_cell_size * permille / 1000;
}

static void set_theme_button_size(lv_obj_t *obj, int32_t size)
{
    const int32_t center_x = lv_obj_get_x(obj) + lv_obj_get_width(obj) / 2;
    const int32_t center_y = lv_obj_get_y(obj) + lv_obj_get_height(obj) / 2;

    lv_obj_set_size(obj, size, size);
    lv_obj_set_pos(obj, center_x - size / 2, center_y - size / 2);
}

static void theme_button_size_anim_cb(void *obj, int32_t size)
{
    set_theme_button_size((lv_obj_t *)obj, size);
}

static void start_theme_button_size_anim(lv_obj_t *obj, int32_t from_size, int32_t to_size)
{
    if (obj == NULL || from_size == to_size) {
        return;
    }

    lv_anim_del(obj, theme_button_size_anim_cb);
    set_theme_button_size(obj, from_size);

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, theme_button_size_anim_cb);
    lv_anim_set_values(&anim, from_size, to_size);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_THEME_SWATCH_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void render_static_settings(void)
{
    show_status_bar();
    ensure_settings_page();
    destroy_list_page();
    destroy_app_page();
    hide_tiles();

    if (s_wp7.brightness_slider != NULL) {
        lv_slider_set_value(s_wp7.brightness_slider,
                            sanitize_brightness(s_wp7.brightness_percent),
                            LV_ANIM_OFF);
    }

    if (s_wp7.settings_slider != NULL) {
        lv_slider_set_value(s_wp7.settings_slider, sanitize_animation_speed(s_wp7.anim_speed_percent), LV_ANIM_OFF);
    }

    if (s_wp7.mode_switch != NULL) {
        set_wp7_switch_value(s_wp7.mode_switch, s_wp7.dark_mode, LV_ANIM_OFF);
        set_wp7_switch_style(s_wp7.mode_switch, s_wp7.dark_mode);
    }

    if (s_wp7.fast_switch != NULL) {
        set_wp7_switch_value(s_wp7.fast_switch, s_wp7.fast_animations, LV_ANIM_OFF);
        set_wp7_switch_style(s_wp7.fast_switch, s_wp7.fast_animations);
    }

    set_settings_title_geometry(s_wp7.settings_title_x, 256, LV_OPA_COVER);

    for (int32_t i = 0; i < WP7_SETTINGS_CONTENT_COUNT; i++) {
        set_setting_item_geometry(&s_wp7.settings_items[i], s_wp7.settings_items[i].x, LV_OPA_COVER);
    }

    update_theme_controls();
}

static void render_vertical_transition(wp7_page_dir_t dir, int32_t progress)
{
    const int32_t out_phase_end = staggered_phase_max(s_wp7.tile_count);
    const int32_t in_phase_start = out_phase_end + transition_blank_progress_for_dir(dir);
    const int32_t max_progress = transition_progress_max();

    show_status_bar();
    destroy_list_page();
    destroy_settings_page();
    progress = clamp_i32(progress, 0, max_progress);

    if (progress < out_phase_end) {
        for (int32_t i = 0; i < s_wp7.tile_count; i++) {
            const int32_t order = ordered_out_index(dir, i);
            const int32_t local_progress = step_progress(progress, order);
            const int32_t eased_progress = ease_in_out_cubic(local_progress);
            wp7_tile_t *tile = &s_wp7.tiles[i];
            const int32_t height = tile->size * (WP7_TILE_PROGRESS_UNIT - eased_progress) / WP7_TILE_PROGRESS_UNIT;
            const int32_t y = dir == WP7_DIR_NEXT ? tile->y : tile->y + (tile->size - height);

            set_tile_number(tile, s_wp7.page, i);
            set_tile_geometry(tile, y, height);
        }

        return;
    }

    if (progress < in_phase_start) {
        hide_tiles();
        return;
    }

    progress -= in_phase_start;

    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        const int32_t local_progress = step_progress(progress, i);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);
        wp7_tile_t *tile = &s_wp7.tiles[i];
        const int32_t height = tile->size * eased_progress / WP7_TILE_PROGRESS_UNIT;
        const int32_t y = dir == WP7_DIR_NEXT ? tile->y + (tile->size - height) : tile->y;

        set_tile_number(tile, s_wp7.target_page, i);
        set_tile_geometry(tile, y, height);
    }
}

static void render_horizontal_transition(wp7_page_dir_t dir, int32_t progress)
{
    const int32_t out_count = dir == WP7_DIR_LIST ? s_wp7.tile_count : list_transition_count();
    const int32_t in_count = dir == WP7_DIR_LIST ? list_transition_count() : s_wp7.tile_count;
    const int32_t out_phase_end = staggered_phase_max(out_count);
    const int32_t in_phase_start = out_phase_end + transition_blank_progress_for_dir(dir);
    const int32_t max_progress = transition_progress_max();

    show_status_bar();
    destroy_settings_page();
    progress = clamp_i32(progress, 0, max_progress);

    if (progress < out_phase_end) {
        if (dir == WP7_DIR_LIST) {
            destroy_list_page();
        } else {
            ensure_list_page();
            hide_tiles();
        }

        for (int32_t order = 0; order < out_count; order++) {
            const int32_t index = dir == WP7_DIR_LIST ? ordered_column_index(order) : order;
            const int32_t local_progress = step_progress(progress, order);
            const int32_t eased_progress = ease_in_out_cubic(local_progress);

            if (dir == WP7_DIR_LIST) {
                wp7_tile_t *tile = &s_wp7.tiles[index];
                const int32_t x = tile->x - ((tile->x + tile->size) * eased_progress / WP7_TILE_PROGRESS_UNIT);

                set_tile_number(tile, s_wp7.page, index);
                set_tile_x(tile, x);
            } else {
                wp7_list_item_t *item = &s_wp7.list_items[order];
                const int32_t x = item->x + ((s_wp7.screen_w - item->x) * eased_progress / WP7_TILE_PROGRESS_UNIT);

                set_list_item_geometry(item, x);
            }
        }

        return;
    }

    if (progress < in_phase_start) {
        if (dir == WP7_DIR_LIST) {
            hide_tiles();
            ensure_list_page();
            hide_list_items();
        } else {
            hide_tiles();
            destroy_list_page();
        }

        return;
    }

    progress -= in_phase_start;

    if (dir == WP7_DIR_LIST) {
        ensure_list_page();
        hide_tiles();
    } else {
        destroy_list_page();
    }

        for (int32_t order = 0; order < in_count; order++) {
            const int32_t index = dir == WP7_DIR_LIST ? order : ordered_column_index(order);
            const int32_t local_progress = step_progress(progress, order);
            const int32_t eased_progress = ease_in_out_cubic(local_progress);

        if (dir == WP7_DIR_LIST) {
            wp7_list_item_t *item = &s_wp7.list_items[order];
            const int32_t x = s_wp7.screen_w - ((s_wp7.screen_w - item->x) * eased_progress / WP7_TILE_PROGRESS_UNIT);

            set_list_item_geometry(item, x);
        } else {
            wp7_tile_t *tile = &s_wp7.tiles[index];

            set_tile_number(tile, s_wp7.page, index);

            if ((index % WP7_COLUMNS) == 0) {
                const int32_t x = -tile->size + ((tile->x + tile->size) * eased_progress / WP7_TILE_PROGRESS_UNIT);
                set_tile_x(tile, x);
            } else {
                const int32_t width = tile->size * eased_progress / WP7_TILE_PROGRESS_UNIT;
                set_tile_width(tile, width);
            }
        }
    }
}

static void render_tile_collapse(wp7_tile_t *tile, int32_t eased_progress, bool toward_right)
{
    const int32_t width = tile->size * (WP7_TILE_PROGRESS_UNIT - eased_progress) / WP7_TILE_PROGRESS_UNIT;
    const int32_t x = toward_right ? tile->x + (tile->size - width) : tile->x;

    set_tile_box(tile, x, tile->y, width, tile->size, LV_OPA_COVER);
}

static void render_tile_restore(wp7_tile_t *tile, int32_t eased_progress, bool grow_to_right)
{
    const int32_t width = tile->size * eased_progress / WP7_TILE_PROGRESS_UNIT;
    const int32_t x = grow_to_right ? tile->x : tile->x + (tile->size - width);

    set_tile_box(tile, x, tile->y, width, tile->size, LV_OPA_COVER);
}

static void render_clicked_tile_fade(wp7_tile_t *tile, int32_t eased_progress, bool appear)
{
    lv_opa_t opa;
    int32_t scale;

    if (appear) {
        opa = (lv_opa_t)(LV_OPA_COVER * eased_progress / WP7_TILE_PROGRESS_UNIT);
        scale = 512 - (256 * eased_progress / WP7_TILE_PROGRESS_UNIT);
    } else {
        opa = (lv_opa_t)(LV_OPA_COVER * (WP7_TILE_PROGRESS_UNIT - eased_progress) / WP7_TILE_PROGRESS_UNIT);
        scale = 256 + (256 * eased_progress / WP7_TILE_PROGRESS_UNIT);
    }

    set_tile_zoom_box(tile, scale, opa);
}

static void render_clicked_list_item_fade(wp7_list_item_t *item, int32_t eased_progress, bool appear)
{
    lv_opa_t opa;
    int32_t scale;

    if (appear) {
        opa = (lv_opa_t)(LV_OPA_COVER * eased_progress / WP7_TILE_PROGRESS_UNIT);
        scale = 512 - (256 * eased_progress / WP7_TILE_PROGRESS_UNIT);
    } else {
        opa = (lv_opa_t)(LV_OPA_COVER * (WP7_TILE_PROGRESS_UNIT - eased_progress) /
                         WP7_TILE_PROGRESS_UNIT);
        scale = 256 + (256 * eased_progress / WP7_TILE_PROGRESS_UNIT);
    }

    set_list_item_box(item, item->x, opa, scale);
}

/* Rows slide in from the left one after another, fading in. The title takes
   step 0 of the entrance, so row i takes step i + 1. */
static void render_rows_in(wp7_setting_item_t *items, int32_t count, int32_t progress)
{
    for (int32_t i = 0; i < count; i++) {
        wp7_setting_item_t *item = &items[i];
        const int32_t local_progress = step_progress(progress, i + 1);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);
        const int32_t x = -item->w + ((item->x + item->w) * eased_progress / WP7_TILE_PROGRESS_UNIT);

        set_setting_item_geometry(item, x, (lv_opa_t)(LV_OPA_COVER *
                                  eased_progress / WP7_TILE_PROGRESS_UNIT));
    }
}

/* Rows leave to the left, last row first, fading out. */
static void render_rows_out(wp7_setting_item_t *items, int32_t count, int32_t progress)
{
    for (int32_t order = 0; order < count; order++) {
        wp7_setting_item_t *item = &items[count - 1 - order];
        const int32_t local_progress = step_progress(progress, order);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);
        const int32_t x = item->x - ((item->x + item->w) * eased_progress / WP7_TILE_PROGRESS_UNIT);

        set_setting_item_geometry(item, x, (lv_opa_t)(LV_OPA_COVER *
                                  (WP7_TILE_PROGRESS_UNIT - eased_progress) /
                                  WP7_TILE_PROGRESS_UNIT));
    }
}

static void render_settings_content_in(int32_t progress)
{
    const int32_t title_local = step_progress(progress, 0);
    const int32_t title_eased = ease_in_out_cubic(title_local);
    const int32_t title_x = -s_wp7.settings_title_w +
                            ((s_wp7.settings_title_x + s_wp7.settings_title_w) * title_eased /
                             WP7_TILE_PROGRESS_UNIT);
    const int32_t title_scale = 384 - (128 * title_eased / WP7_TILE_PROGRESS_UNIT);

    set_settings_title_geometry(title_x, title_scale,
                                title_local > 0 ? LV_OPA_COVER : 0);
    render_rows_in(s_wp7.settings_items, WP7_SETTINGS_CONTENT_COUNT, progress);
}

static void render_settings_content_out(int32_t progress)
{
    set_settings_title_geometry(s_wp7.settings_title_x, 256, LV_OPA_COVER);
    render_rows_out(s_wp7.settings_items, WP7_SETTINGS_CONTENT_COUNT, progress);
}

static void render_settings_title_out(int32_t progress)
{
    const int32_t eased_progress = ease_in_out_cubic(progress);
    const int32_t title_x = s_wp7.settings_title_x -
                            ((s_wp7.settings_title_x + s_wp7.settings_title_w) *
                             eased_progress / WP7_TILE_PROGRESS_UNIT);
    const int32_t scale = 256 + (128 * eased_progress / WP7_TILE_PROGRESS_UNIT);

    set_settings_title_geometry(title_x, scale,
                                progress < WP7_TILE_PROGRESS_UNIT ? LV_OPA_COVER : 0);
}

/* The app page title and rows move like the UI Settings title and controls. */
static void render_app_content_in(int32_t progress)
{
    const wp7_setting_item_t *title = &s_wp7.app_items[0];
    const int32_t title_local = step_progress(progress, 0);
    const int32_t title_eased = ease_in_out_cubic(title_local);
    const int32_t title_x = -title->w + ((title->x + title->w) * title_eased /
                                         WP7_TILE_PROGRESS_UNIT);
    const int32_t title_scale = 384 - (128 * title_eased / WP7_TILE_PROGRESS_UNIT);

    set_app_title_geometry(title_x, title_scale, title_local > 0 ? LV_OPA_COVER : 0);
    render_rows_in(&s_wp7.app_items[1], app_row_count(), progress);
}

static void render_app_content_out(int32_t progress)
{
    set_app_title_geometry(s_wp7.app_items[0].x, 256, LV_OPA_COVER);
    render_rows_out(&s_wp7.app_items[1], app_row_count(), progress);
}

static void render_app_title_out(int32_t progress)
{
    const wp7_setting_item_t *title = &s_wp7.app_items[0];
    const int32_t eased_progress = ease_in_out_cubic(progress);
    const int32_t title_x = title->x - ((title->x + title->w) * eased_progress /
                                        WP7_TILE_PROGRESS_UNIT);
    const int32_t scale = 256 + (128 * eased_progress / WP7_TILE_PROGRESS_UNIT);

    set_app_title_geometry(title_x, scale,
                           progress < WP7_TILE_PROGRESS_UNIT ? LV_OPA_COVER : 0);
}

/* The open and close transitions below serve UI Settings and the app pages;
   these helpers do the page's part of each phase. */

/* Takes the page off screen. UI Settings is destroyed; an app page is only
   hidden, and destroyed when the transition ends (see app_row_count). */
static void hide_page(wp7_page_dir_t dir)
{
    if (is_app_dir(dir)) {
        wp7_apps_set_visible(false);
    } else {
        destroy_settings_page();
    }
}

/* The blank hold before the page enters: it exists, but nothing on it shows. */
static void render_page_blank(wp7_page_dir_t dir)
{
    if (is_app_dir(dir)) {
        ensure_app_page();
        hide_rows(s_wp7.app_items, s_wp7.app_item_count);
        wp7_apps_set_visible(true);
    } else {
        ensure_settings_page();
        hide_settings_items();
    }
}

static void render_page_content_in(wp7_page_dir_t dir, int32_t progress)
{
    if (is_app_dir(dir)) {
        ensure_app_page();
        wp7_apps_set_visible(true);
        render_app_content_in(progress);
    } else {
        ensure_settings_page();
        render_settings_content_in(progress);
    }
}

static void render_page_content_out(wp7_page_dir_t dir, int32_t progress)
{
    if (is_app_dir(dir)) {
        ensure_app_page();
        wp7_apps_set_visible(true);
        render_app_content_out(progress);
    } else {
        ensure_settings_page();
        render_settings_content_out(progress);
    }
}

static void render_page_title_out(wp7_page_dir_t dir, int32_t progress)
{
    if (is_app_dir(dir)) {
        ensure_app_page();
        wp7_apps_set_visible(true);
        hide_rows(&s_wp7.app_items[1], app_row_count());
        render_app_title_out(progress);
    } else {
        ensure_settings_page();
        hide_rows(s_wp7.settings_items, WP7_SETTINGS_CONTENT_COUNT);
        render_settings_title_out(progress);
    }
}

static void render_list_page_open_transition(wp7_page_dir_t dir, int32_t progress)
{
    const int32_t other_count = settings_other_list_count();
    const int32_t other_phase = staggered_phase_max(other_count);
    const int32_t clicked_phase_start = other_phase;
    const int32_t blank_phase_start = clicked_phase_start + WP7_SETTINGS_TILE_PHASE_UNIT;
    const int32_t content_phase_start = blank_phase_start +
                                        transition_blank_progress_for_dir(dir);
    const int32_t max_progress = transition_progress_max_for_dir(dir);

    progress = clamp_i32(progress, 0, max_progress);
    show_status_bar();
    hide_tiles();

    if (progress < other_phase) {
        ensure_list_page();
        hide_page(dir);

        for (int32_t order = 0; order < other_count; order++) {
            const int32_t index = ordered_settings_list_other_index(order);
            wp7_list_item_t *item = &s_wp7.list_items[index];
            const int32_t local_progress = step_progress(progress, order);
            const int32_t eased_progress = ease_in_out_cubic(local_progress);
            const int32_t x = item->x + ((s_wp7.screen_w - item->x) * eased_progress /
                                         WP7_TILE_PROGRESS_UNIT);

            set_list_item_geometry(item, x);
        }

        wp7_list_item_t *clicked_item = &s_wp7.list_items[s_wp7.settings_list_index];
        set_list_item_box(clicked_item, clicked_item->x, LV_OPA_COVER, WP7_OBJ_RELEASE_SCALE);
        return;
    }

    if (progress < blank_phase_start) {
        ensure_list_page();
        hide_page(dir);
        hide_list_items();

        wp7_list_item_t *clicked_item = &s_wp7.list_items[s_wp7.settings_list_index];
        const int32_t local_progress = phase_progress(progress - clicked_phase_start,
                                                     WP7_SETTINGS_TILE_PHASE_UNIT);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);

        render_clicked_list_item_fade(clicked_item, eased_progress, false);
        return;
    }

    destroy_list_page();

    if (progress < content_phase_start) {
        render_page_blank(dir);
        return;
    }

    render_page_content_in(dir, progress - content_phase_start);
}

static void render_list_page_close_transition(wp7_page_dir_t dir, int32_t progress)
{
    const int32_t other_count = settings_other_list_count();
    const int32_t content_phase = staggered_phase_max(page_content_count(dir));
    const int32_t title_phase_start = content_phase;
    const int32_t blank_phase_start = title_phase_start + WP7_SETTINGS_TITLE_PHASE_UNIT;
    const int32_t clicked_phase_start = blank_phase_start +
                                       transition_blank_progress_for_dir(dir);
    const int32_t other_phase_start = clicked_phase_start + WP7_SETTINGS_TILE_PHASE_UNIT;
    const int32_t max_progress = transition_progress_max_for_dir(dir);

    progress = clamp_i32(progress, 0, max_progress);
    show_status_bar();
    hide_tiles();

    if (progress < content_phase) {
        destroy_list_page();
        render_page_content_out(dir, progress);
        return;
    }

    if (progress < blank_phase_start) {
        destroy_list_page();
        render_page_title_out(dir, phase_progress(progress - title_phase_start,
                                                  WP7_SETTINGS_TITLE_PHASE_UNIT));
        return;
    }

    hide_page(dir);
    ensure_list_page();

    if (progress < clicked_phase_start) {
        hide_list_items();
        return;
    }

    if (progress < other_phase_start) {
        hide_list_items();

        wp7_list_item_t *clicked_item = &s_wp7.list_items[s_wp7.settings_list_index];
        const int32_t local_progress = phase_progress(progress - clicked_phase_start,
                                                     WP7_SETTINGS_TILE_PHASE_UNIT);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);

        render_clicked_list_item_fade(clicked_item, eased_progress, true);
        return;
    }

    wp7_list_item_t *clicked_item = &s_wp7.list_items[s_wp7.settings_list_index];
    set_list_item_box(clicked_item, clicked_item->x, LV_OPA_COVER, WP7_OBJ_RELEASE_SCALE);

    progress -= other_phase_start;

    for (int32_t order = 0; order < other_count; order++) {
        const int32_t index = ordered_settings_list_other_index(order);
        wp7_list_item_t *item = &s_wp7.list_items[index];
        const int32_t local_progress = step_progress(progress, order);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);
        const int32_t x = s_wp7.screen_w - ((s_wp7.screen_w - item->x) * eased_progress /
                                            WP7_TILE_PROGRESS_UNIT);

        set_list_item_geometry(item, x);
    }
}

static void render_page_open_transition(wp7_page_dir_t dir, int32_t progress)
{
    if (s_wp7.settings_from_list) {
        render_list_page_open_transition(dir, progress);
        return;
    }

    const int32_t other_count = settings_other_tile_count();
    const int32_t other_phase = staggered_phase_max(other_count);
    const int32_t clicked_phase_start = other_phase;
    const int32_t blank_phase_start = clicked_phase_start + WP7_SETTINGS_TILE_PHASE_UNIT;
    const int32_t content_phase_start = blank_phase_start +
                                        transition_blank_progress_for_dir(dir);
    const int32_t max_progress = transition_progress_max_for_dir(dir);
    const bool collapse_right = tile_col(s_wp7.settings_tile_index) == 0;

    progress = clamp_i32(progress, 0, max_progress);
    show_status_bar();
    destroy_list_page();

    if (progress < other_phase) {
        hide_page(dir);

        for (int32_t order = 0; order < other_count; order++) {
            const int32_t index = ordered_settings_other_index(order);
            const int32_t local_progress = step_progress(progress, order);
            const int32_t eased_progress = ease_in_out_cubic(local_progress);
            wp7_tile_t *tile = &s_wp7.tiles[index];

            set_tile_number(tile, s_wp7.page, index);
            render_tile_collapse(tile, eased_progress, collapse_right);
        }

        wp7_tile_t *clicked_tile = &s_wp7.tiles[s_wp7.settings_tile_index];
        set_tile_number(clicked_tile, s_wp7.page, s_wp7.settings_tile_index);
        set_tile_geometry(clicked_tile, clicked_tile->y, clicked_tile->size);
        return;
    }

    if (progress < blank_phase_start) {
        hide_page(dir);
        hide_tiles();

        wp7_tile_t *clicked_tile = &s_wp7.tiles[s_wp7.settings_tile_index];
        const int32_t local_progress = phase_progress(progress - clicked_phase_start,
                                                     WP7_SETTINGS_TILE_PHASE_UNIT);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);

        set_tile_number(clicked_tile, s_wp7.page, s_wp7.settings_tile_index);
        render_clicked_tile_fade(clicked_tile, eased_progress, false);
        return;
    }

    hide_tiles();

    if (progress < content_phase_start) {
        render_page_blank(dir);
        return;
    }

    render_page_content_in(dir, progress - content_phase_start);
}

static void render_page_close_transition(wp7_page_dir_t dir, int32_t progress)
{
    if (s_wp7.settings_from_list) {
        render_list_page_close_transition(dir, progress);
        return;
    }

    const int32_t other_count = settings_other_tile_count();
    const int32_t content_phase = staggered_phase_max(page_content_count(dir));
    const int32_t title_phase_start = content_phase;
    const int32_t blank_phase_start = title_phase_start + WP7_SETTINGS_TITLE_PHASE_UNIT;
    const int32_t clicked_phase_start = blank_phase_start +
                                       transition_blank_progress_for_dir(dir);
    const int32_t other_phase_start = clicked_phase_start + WP7_SETTINGS_TILE_PHASE_UNIT;
    const int32_t max_progress = transition_progress_max_for_dir(dir);
    const bool grow_to_right = tile_col(s_wp7.settings_tile_index) != 0;

    progress = clamp_i32(progress, 0, max_progress);
    show_status_bar();
    destroy_list_page();

    if (progress < content_phase) {
        hide_tiles();
        render_page_content_out(dir, progress);
        return;
    }

    if (progress < blank_phase_start) {
        hide_tiles();
        render_page_title_out(dir, phase_progress(progress - title_phase_start,
                                                  WP7_SETTINGS_TITLE_PHASE_UNIT));
        return;
    }

    hide_page(dir);

    if (progress < clicked_phase_start) {
        hide_tiles();
        return;
    }

    if (progress < other_phase_start) {
        hide_tiles();

        wp7_tile_t *clicked_tile = &s_wp7.tiles[s_wp7.settings_tile_index];
        const int32_t local_progress = phase_progress(progress - clicked_phase_start,
                                                     WP7_SETTINGS_TILE_PHASE_UNIT);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);

        set_tile_number(clicked_tile, s_wp7.page, s_wp7.settings_tile_index);
        render_clicked_tile_fade(clicked_tile, eased_progress, true);
        return;
    }

    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        set_tile_number(&s_wp7.tiles[i], s_wp7.page, i);
    }

    wp7_tile_t *clicked_tile = &s_wp7.tiles[s_wp7.settings_tile_index];
    set_tile_geometry(clicked_tile, clicked_tile->y, clicked_tile->size);

    progress -= other_phase_start;

    for (int32_t order = 0; order < other_count; order++) {
        const int32_t index = ordered_settings_other_index(order);
        const int32_t reverse_order = other_count - 1 - order;
        const int32_t local_progress = step_progress(progress, reverse_order);
        const int32_t eased_progress = ease_in_out_cubic(local_progress);

        render_tile_restore(&s_wp7.tiles[index], eased_progress, grow_to_right);
    }
}

static void render_transition(wp7_page_dir_t dir, int32_t progress)
{
    if (is_open_dir(dir)) {
        render_page_open_transition(dir, progress);
        return;
    }

    if (is_close_dir(dir)) {
        render_page_close_transition(dir, progress);
        return;
    }

    if (is_horizontal_dir(dir)) {
        render_horizontal_transition(dir, progress);
        return;
    }

    render_vertical_transition(dir, progress);
}

static void transition_anim_cb(void *var, int32_t progress)
{
    (void)var;
    s_wp7.anim_progress = progress;
    render_transition(s_wp7.anim_dir, progress);
}

static bool interrupt_transition_anim_for_drag(void)
{
    if (!s_wp7.animating || !is_swipe_transition_dir(s_wp7.anim_dir)) {
        return false;
    }

    const wp7_page_dir_t dir = s_wp7.anim_dir;
    const int32_t max_progress = transition_progress_max_for_dir(dir);
    const int32_t progress = clamp_i32(s_wp7.anim_progress, 0, max_progress);
    const bool commit = s_wp7.commit_transition;

    lv_anim_del(&s_wp7, transition_anim_cb);
    s_wp7.animating = false;
    s_wp7.commit_transition = false;
    s_wp7.anim_dir = WP7_DIR_NONE;
    s_wp7.drag_dir = dir;
    s_wp7.drag_progress = progress;
    s_wp7.drag_target_progress = progress;
    s_wp7.drag_base_progress = progress;
    s_wp7.drag_interrupted_anim = true;
    s_wp7.drag_interrupted_commit = commit;
    return true;
}

static void transition_anim_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    if (s_wp7.commit_transition) {
        if (s_wp7.anim_dir == WP7_DIR_LIST) {
            s_wp7.in_list = true;
        } else if (s_wp7.anim_dir == WP7_DIR_HOME) {
            s_wp7.in_list = false;
        } else if (s_wp7.anim_dir == WP7_DIR_SETTINGS_OPEN) {
            s_wp7.in_settings = true;
        } else if (s_wp7.anim_dir == WP7_DIR_SETTINGS_CLOSE) {
            s_wp7.in_settings = false;
        } else if (s_wp7.anim_dir == WP7_DIR_APP_OPEN) {
            s_wp7.in_app = true;
        } else if (s_wp7.anim_dir == WP7_DIR_APP_CLOSE) {
            s_wp7.in_app = false;
        } else {
            s_wp7.page = s_wp7.target_page;
        }
    }

    if (s_wp7.in_settings) {
        render_static_settings();
    } else if (s_wp7.in_app) {
        render_static_app();
    } else if (s_wp7.in_list) {
        render_static_list();
    } else {
        render_static_page(s_wp7.page);
    }

    s_wp7.animating = false;
    s_wp7.commit_transition = false;
    s_wp7.anim_dir = WP7_DIR_NONE;
    s_wp7.drag_dir = WP7_DIR_NONE;
    s_wp7.drag_progress = 0;
    s_wp7.drag_target_progress = 0;
    s_wp7.drag_base_progress = 0;
    s_wp7.anim_progress = 0;
    s_wp7.drag_interrupted_anim = false;
    s_wp7.drag_interrupted_commit = false;
    key_render_focus();
}

static void start_progress_anim(wp7_page_dir_t dir, int32_t start, int32_t end, bool commit)
{
    lv_anim_del(&s_wp7, transition_anim_cb);
    s_wp7.animating = true;
    s_wp7.commit_transition = commit;
    s_wp7.anim_dir = dir;
    s_wp7.anim_progress = start;

    if (start == end) {
        render_transition(dir, end);
        transition_anim_completed_cb(NULL);
        return;
    }

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s_wp7);
    lv_anim_set_exec_cb(&anim, transition_anim_cb);
    lv_anim_set_values(&anim, start, end);
    const int32_t progress_delta = end > start ? end - start : start - end;
    int32_t duration = animation_duration_ms(dir, progress_delta);
    const int32_t release_min_ms = animation_release_min_ms(dir);

    if (duration < release_min_ms) {
        duration = release_min_ms;
    }
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_completed_cb(&anim, transition_anim_completed_cb);
    lv_anim_start(&anim);
}

static int32_t limit_drag_progress(int32_t target_progress)
{
    const uint32_t tick = lv_tick_get();
    const uint32_t elapsed = tick - s_wp7.drag_last_tick;
    int32_t max_step = (int32_t)(elapsed > 0 ? elapsed : 1) *
                       animation_progress_per_ms(s_wp7.drag_dir);

    s_wp7.drag_last_tick = tick;

    if (target_progress > s_wp7.drag_progress + max_step) {
        return s_wp7.drag_progress + max_step;
    }

    if (target_progress < s_wp7.drag_progress - max_step) {
        return s_wp7.drag_progress - max_step;
    }

    return target_progress;
}

static void update_drag_progress(int32_t x, int32_t y)
{
    const int32_t delta_x = x - s_wp7.drag_start_x;
    const int32_t delta_y = y - s_wp7.drag_start_y;
    const int32_t threshold = transition_drag_threshold();

    if (s_wp7.drag_dir == WP7_DIR_NONE) {
        const int32_t abs_x = abs_i32(delta_x);
        const int32_t abs_y = abs_i32(delta_y);

        if (abs_x > threshold && abs_x > abs_y && !s_wp7.in_list && delta_x < -threshold) {
            s_wp7.drag_dir = WP7_DIR_LIST;
            s_wp7.drag_base_progress = 0;
        } else if (abs_x > threshold && abs_x > abs_y && s_wp7.in_list && delta_x > threshold) {
            s_wp7.drag_dir = WP7_DIR_HOME;
            s_wp7.drag_base_progress = 0;
        } else if (abs_y > threshold && abs_y >= abs_x && !s_wp7.in_list && delta_y < -threshold) {
            s_wp7.drag_dir = WP7_DIR_NEXT;
            s_wp7.target_page = s_wp7.page + 1;
            s_wp7.drag_base_progress = 0;
        } else if (abs_y > threshold && abs_y >= abs_x && !s_wp7.in_list && delta_y > threshold && s_wp7.page > 0) {
            s_wp7.drag_dir = WP7_DIR_PREV;
            s_wp7.target_page = s_wp7.page - 1;
            s_wp7.drag_base_progress = 0;
        } else {
            return;
        }
    }

    release_tile_press_for_drag();
    release_list_press_for_transition();

    const int32_t distance = transition_drag_distance();
    const int32_t max_progress = transition_progress_max();
    const int32_t drag_distance = transition_drag_delta(s_wp7.drag_dir, delta_x, delta_y);

    const int32_t progress = clamp_i32(s_wp7.drag_base_progress +
                                       drag_distance * max_progress / distance,
                                       0, max_progress);

    s_wp7.drag_target_progress = progress;
    s_wp7.drag_progress = limit_drag_progress(progress);
    render_transition(s_wp7.drag_dir, s_wp7.drag_progress);
}

static void finish_drag(void)
{
    if (!s_wp7.drag_active || s_wp7.drag_dir == WP7_DIR_NONE) {
        s_wp7.drag_active = false;
        s_wp7.drag_base_progress = 0;
        s_wp7.drag_interrupted_anim = false;
        s_wp7.drag_interrupted_commit = false;
        return;
    }

    const int32_t max_progress = transition_progress_max();
    const int32_t commit_progress = max_progress * WP7_RELEASE_COMMIT_PERMILLE / 1000;
    const bool untouched_interrupt = s_wp7.drag_interrupted_anim &&
                                     s_wp7.drag_target_progress == s_wp7.drag_base_progress;
    const bool commit = untouched_interrupt ?
                        s_wp7.drag_interrupted_commit :
                        s_wp7.drag_target_progress >= commit_progress;
    const int32_t end = commit ? max_progress : 0;

    s_wp7.drag_active = false;
    s_wp7.drag_interrupted_anim = false;
    s_wp7.drag_interrupted_commit = false;
    start_progress_anim(s_wp7.drag_dir, s_wp7.drag_progress, end, commit);
}

static int32_t tile_press_current_scale(wp7_tile_t *tile)
{
    if (tile == NULL || tile->size <= 0 || tile->current_w <= 0) {
        return WP7_OBJ_RELEASE_SCALE;
    }

    return clamp_i32(tile->current_w * WP7_OBJ_RELEASE_SCALE / tile->size,
                     WP7_OBJ_PRESS_SCALE, WP7_OBJ_RELEASE_SCALE);
}

static void set_tile_press_scale(wp7_tile_t *tile, int32_t scale)
{
    if (tile == NULL || tile->obj == NULL || tile->hidden ||
            s_wp7.in_list || s_wp7.in_settings) {
        return;
    }

    scale = clamp_i32(scale, WP7_OBJ_PRESS_SCALE, WP7_OBJ_RELEASE_SCALE);

    const int32_t size = tile->size * scale / WP7_OBJ_RELEASE_SCALE;
    const int32_t x = tile->x + (tile->size - size) / 2;
    const int32_t y = tile->y + (tile->size - size) / 2;

    lv_obj_set_style_transform_scale(tile->obj, WP7_OBJ_RELEASE_SCALE, 0);
    lv_obj_set_style_bg_color(tile->obj, theme_color(), 0);
    lv_obj_set_style_bg_opa(tile->obj, LV_OPA_COVER, 0);
    set_tile_visual_style(tile, LV_OPA_COVER, WP7_OBJ_RELEASE_SCALE);
    set_tile_frame(tile, x, y, size, size);
}

static void tile_press_scale_anim_cb(void *var, int32_t scale)
{
    set_tile_press_scale((wp7_tile_t *)var, scale);
}

static void tile_press_anim_completed_cb(lv_anim_t *anim)
{
    wp7_tile_t *tile = (wp7_tile_t *)anim->var;

    if (tile == NULL) {
        return;
    }

    tile->press_animating = false;
    tile->press_releasing = false;

    if (!tile->press_active && tile->current_w >= tile->size) {
        set_tile_press_scale(tile, WP7_OBJ_RELEASE_SCALE);
    }
}

static void start_tile_press_anim(wp7_tile_t *tile, bool pressed)
{
    if (tile == NULL || tile->obj == NULL) {
        return;
    }

    lv_anim_del(tile, tile_press_scale_anim_cb);
    tile->press_active = pressed;
    tile->press_animating = true;
    tile->press_releasing = !pressed;

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, tile);
    lv_anim_set_exec_cb(&anim, tile_press_scale_anim_cb);
    lv_anim_set_values(&anim, tile_press_current_scale(tile),
                       pressed ? WP7_OBJ_PRESS_SCALE : WP7_OBJ_RELEASE_SCALE);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_PRESS_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, tile_press_anim_completed_cb);
    lv_anim_start(&anim);
}

static void tile_press_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    wp7_tile_t *tile = (wp7_tile_t *)lv_event_get_user_data(event);

    if (code == LV_EVENT_PRESSED) {
        store_control_press_point();
        start_tile_press_anim(tile, true);
    } else if (code == LV_EVENT_PRESSING) {
        if (tile != NULL && tile->press_active && control_moved_past_tap_threshold()) {
            start_tile_press_anim(tile, false);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        start_tile_press_anim(tile, false);
    }
}

static void release_tile_press_for_drag(void)
{
    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        wp7_tile_t *tile = &s_wp7.tiles[i];

        if (tile->obj == NULL || tile->hidden) {
            continue;
        }

        if (tile->press_active || tile->press_animating) {
            lv_anim_del(tile, tile_press_scale_anim_cb);
            tile->press_active = false;
            tile->press_animating = false;
            tile->press_releasing = false;
        }
    }
}

static int32_t list_item_press_current_scale(wp7_list_item_t *item)
{
    if (item == NULL || item->icon_size <= 0 || item->current_icon_size <= 0) {
        return WP7_OBJ_RELEASE_SCALE;
    }

    return clamp_i32(item->current_icon_size * WP7_OBJ_RELEASE_SCALE / item->icon_size,
                     WP7_OBJ_PRESS_SCALE, WP7_OBJ_RELEASE_SCALE);
}

static void set_list_item_press_scale(wp7_list_item_t *item, int32_t scale)
{
    if (item == NULL || item->obj == NULL || item->icon == NULL || item->hidden ||
            !s_wp7.in_list || s_wp7.in_settings) {
        return;
    }

    scale = clamp_i32(scale, WP7_OBJ_PRESS_SCALE, WP7_OBJ_RELEASE_SCALE);
    const int32_t size = item->icon_size * scale / WP7_OBJ_RELEASE_SCALE;

    if (set_list_item_icon_size(item, size)) {
        align_list_item_content(item);
    }
}

static void list_item_press_scale_anim_cb(void *var, int32_t scale)
{
    set_list_item_press_scale((wp7_list_item_t *)var, scale);
}

static void list_item_press_anim_completed_cb(lv_anim_t *anim)
{
    wp7_list_item_t *item = (wp7_list_item_t *)anim->var;

    if (item == NULL) {
        return;
    }

    item->press_animating = false;
    item->press_releasing = false;

    if (!item->press_active && item->current_scale >= WP7_OBJ_RELEASE_SCALE) {
        set_list_item_press_scale(item, WP7_OBJ_RELEASE_SCALE);
    }
}

static void start_list_item_press_anim(wp7_list_item_t *item, bool pressed)
{
    if (item == NULL || item->obj == NULL) {
        return;
    }

    lv_anim_del(item, list_item_press_scale_anim_cb);
    item->press_active = pressed;
    item->press_animating = true;
    item->press_releasing = !pressed;

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, item);
    lv_anim_set_exec_cb(&anim, list_item_press_scale_anim_cb);
    lv_anim_set_values(&anim, list_item_press_current_scale(item),
                       pressed ? WP7_OBJ_PRESS_SCALE : WP7_OBJ_RELEASE_SCALE);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_PRESS_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, list_item_press_anim_completed_cb);
    lv_anim_start(&anim);
}

static void list_item_press_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    wp7_list_item_t *item = (wp7_list_item_t *)lv_event_get_user_data(event);

    if (code == LV_EVENT_PRESSED) {
        store_control_press_point();
        start_list_item_press_anim(item, true);
    } else if (code == LV_EVENT_PRESSING) {
        if (item != NULL && item->press_active && control_moved_past_tap_threshold()) {
            start_list_item_press_anim(item, false);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        start_list_item_press_anim(item, false);
    }
}

static void release_list_press_for_transition(void)
{
    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        wp7_list_item_t *item = &s_wp7.list_items[i];

        if (item->obj == NULL || item->hidden) {
            continue;
        }

        if (item->press_active || item->press_animating) {
            lv_anim_del(item, list_item_press_scale_anim_cb);
            item->press_active = false;
            item->press_animating = false;
            item->press_releasing = false;
            set_list_item_press_scale(item, WP7_OBJ_RELEASE_SCALE);
        }
    }
}

/* Opens the page behind home tile or app list row `index` with the open
   transition, which starts from that tile or row. The tiles and rows before
   UI Settings are the apps, in wp7_app_id_t order. */
static void open_page(bool from_list, int32_t index)
{
    const int32_t settings_index = from_list ? WP7_LIST_UI_SETTINGS_INDEX :
                                               WP7_SETTINGS_TILE_INDEX;
    const wp7_page_dir_t dir = index >= settings_index ? WP7_DIR_SETTINGS_OPEN :
                                                         WP7_DIR_APP_OPEN;

    if (dir == WP7_DIR_APP_OPEN) {
        if (index < 0 || index >= WP7_APP_COUNT) {
            return;
        }

        s_wp7.open_app = (wp7_app_id_t)index;
        ensure_app_page();

        if (s_wp7.app_item_count == 0) {
            return;
        }
    }

    s_wp7.settings_from_list = from_list;

    if (from_list) {
        s_wp7.settings_list_index = index;
        release_list_press_for_transition();
    } else {
        s_wp7.settings_tile_index = index;
        release_tile_press_for_drag();
    }

    /* The key focus outline would zoom with the tile; it returns on the way back. */
    clear_key_focus();
    start_progress_anim(dir, 0, transition_progress_max_for_dir(dir), true);
}

static void settings_tile_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wp7_tile_t *tile = (wp7_tile_t *)lv_event_get_user_data(event);
    const int32_t index = (int32_t)(tile - s_wp7.tiles);

    if (s_wp7.page != 0 || s_wp7.in_list || s_wp7.in_settings ||
            s_wp7.animating || s_wp7.in_app || index < 0 || index >= s_wp7.tile_count) {
        return;
    }

    open_page(false, index);
}

static void settings_list_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wp7_list_item_t *item = (wp7_list_item_t *)lv_event_get_user_data(event);

    if (item == NULL) {
        return;
    }

    const int32_t index = (int32_t)(item - s_wp7.list_items);

    if (!s_wp7.in_list || s_wp7.in_settings || s_wp7.animating ||
            s_wp7.in_app || index < 0 || index >= s_wp7.list_count) {
        return;
    }

    open_page(true, index);
}

static void settings_title_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_wp7.in_settings || s_wp7.animating) {
        return;
    }

    start_progress_anim(WP7_DIR_SETTINGS_CLOSE, 0,
                        transition_progress_max_for_dir(WP7_DIR_SETTINGS_CLOSE), true);
}

static void settings_slider_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        store_control_press_point();
        s_wp7.settings_slider_press_value = lv_slider_get_value(s_wp7.settings_slider);
        s_wp7.settings_slider_tap_pending = true;
        start_knob_extend_anim(s_wp7.settings_slider, WP7_KNOB_PRESS_EXTEND);

        const int32_t value = sanitize_animation_speed(
            slider_value_from_active_point(s_wp7.settings_slider));

        if (value != s_wp7.settings_slider_press_value) {
            s_wp7.settings_slider_tap_pending = false;
            animate_slider_to_value(s_wp7.settings_slider, value);
            s_wp7.anim_speed_percent = value;
            update_reset_button_state();
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (control_moved_past_tap_threshold()) {
            s_wp7.settings_slider_tap_pending = false;
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        const int32_t value = sanitize_animation_speed(lv_slider_get_value(s_wp7.settings_slider));

        s_wp7.settings_slider_tap_pending = false;
        s_wp7.anim_speed_percent = value;
        update_reset_button_state();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_wp7.anim_speed_percent = sanitize_animation_speed(lv_slider_get_value(s_wp7.settings_slider));
        s_wp7.settings_slider_tap_pending = false;
        start_knob_extend_anim(s_wp7.settings_slider, WP7_KNOB_RELEASE_EXTEND);
        update_reset_button_state();
        save_animation_speed();
    }
}

static void settings_brightness_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        store_control_press_point();
        s_wp7.brightness_slider_press_value = lv_slider_get_value(s_wp7.brightness_slider);
        s_wp7.brightness_slider_tap_pending = true;
        start_knob_extend_anim(s_wp7.brightness_slider, WP7_KNOB_PRESS_EXTEND);

        const int32_t value = sanitize_brightness(
            slider_value_from_active_point(s_wp7.brightness_slider));

        if (value != s_wp7.brightness_slider_press_value) {
            s_wp7.brightness_slider_tap_pending = false;
            s_wp7.brightness_slider_tap_animating = true;
            animate_slider_to_value(s_wp7.brightness_slider, value);
            animate_brightness_to(value);
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (control_moved_past_tap_threshold()) {
            s_wp7.brightness_slider_tap_pending = false;
            s_wp7.brightness_slider_tap_animating = false;
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        const int32_t value = sanitize_brightness(lv_slider_get_value(s_wp7.brightness_slider));

        if (s_wp7.brightness_slider_tap_animating && !control_moved_past_tap_threshold()) {
            return;
        }

        s_wp7.brightness_slider_tap_pending = false;
        s_wp7.brightness_slider_tap_animating = false;
        set_brightness_immediate(value);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_wp7.brightness_slider_tap_animating) {
            set_brightness_immediate(lv_slider_get_value(s_wp7.brightness_slider));
        }

        s_wp7.brightness_slider_tap_pending = false;
        s_wp7.brightness_slider_tap_animating = false;
        start_knob_extend_anim(s_wp7.brightness_slider, WP7_KNOB_RELEASE_EXTEND);
        save_brightness();
    }
}

static void settings_mode_switch_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        store_control_press_point();
        s_wp7.mode_switch_press_checked = s_wp7.dark_mode;
        s_wp7.mode_switch_tap_pending = true;
        start_knob_extend_anim(s_wp7.mode_switch, WP7_KNOB_PRESS_EXTEND);
    } else if (code == LV_EVENT_PRESSING) {
        if (control_moved_past_tap_threshold()) {
            s_wp7.mode_switch_tap_pending = false;
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (s_wp7.mode_switch_tap_pending && !control_moved_past_tap_threshold()) {
            set_wp7_switch_value(s_wp7.mode_switch, s_wp7.mode_switch_press_checked,
                                 LV_ANIM_OFF);
            set_wp7_switch_style(s_wp7.mode_switch, s_wp7.mode_switch_press_checked);
            return;
        }

        set_wp7_switch_style(s_wp7.mode_switch,
                             wp7_switch_value_checked(s_wp7.mode_switch));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        const bool tap = code == LV_EVENT_RELEASED &&
                         s_wp7.mode_switch_tap_pending &&
                         !control_moved_past_tap_threshold();
        const bool checked = tap ? !s_wp7.mode_switch_press_checked :
                             wp7_switch_value_checked(s_wp7.mode_switch);

        s_wp7.mode_switch_tap_pending = false;
        set_wp7_switch_value(s_wp7.mode_switch, checked, LV_ANIM_ON);
        set_wp7_switch_style(s_wp7.mode_switch, checked);
        start_knob_extend_anim(s_wp7.mode_switch, WP7_KNOB_RELEASE_EXTEND);

        if (checked != s_wp7.dark_mode) {
            start_color_scheme_anim(checked);
        }

        save_dark_mode();
    }
}

static void settings_fast_switch_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        store_control_press_point();
        s_wp7.fast_switch_press_checked = s_wp7.fast_animations;
        s_wp7.fast_switch_tap_pending = true;
        start_knob_extend_anim(s_wp7.fast_switch, WP7_KNOB_PRESS_EXTEND);
    } else if (code == LV_EVENT_PRESSING) {
        if (control_moved_past_tap_threshold()) {
            s_wp7.fast_switch_tap_pending = false;
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (s_wp7.fast_switch_tap_pending && !control_moved_past_tap_threshold()) {
            set_wp7_switch_value(s_wp7.fast_switch, s_wp7.fast_switch_press_checked,
                                 LV_ANIM_OFF);
            set_wp7_switch_style(s_wp7.fast_switch, s_wp7.fast_switch_press_checked);
            return;
        }

        set_wp7_switch_style(s_wp7.fast_switch,
                             wp7_switch_value_checked(s_wp7.fast_switch));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        const bool tap = code == LV_EVENT_RELEASED &&
                         s_wp7.fast_switch_tap_pending &&
                         !control_moved_past_tap_threshold();
        const bool checked = tap ? !s_wp7.fast_switch_press_checked :
                             wp7_switch_value_checked(s_wp7.fast_switch);

        s_wp7.fast_switch_tap_pending = false;
        set_wp7_switch_value(s_wp7.fast_switch, checked, LV_ANIM_ON);
        set_wp7_switch_style(s_wp7.fast_switch, checked);
        start_knob_extend_anim(s_wp7.fast_switch, WP7_KNOB_RELEASE_EXTEND);

        if (checked != s_wp7.fast_animations) {
            s_wp7.fast_animations = checked;
            update_theme_controls();
        }

        save_fast_animations();
    }
}

static void settings_theme_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const intptr_t index = (intptr_t)lv_event_get_user_data(event);
    const int32_t next_index = sanitize_theme_index((int32_t)index);
    const int32_t prev_index = sanitize_theme_index(s_wp7.theme_index);
    const uint32_t current_color = theme_color_hex();
    const uint32_t current_text_color = theme_text_color_hex();
    const uint32_t next_color = raw_theme_color_hex(next_index);

    if (next_index == prev_index && !s_wp7.theme_animating) {
        return;
    }

    for (int32_t i = 0; i < WP7_THEME_COLOR_COUNT; i++) {
        const int32_t from_size = theme_swatch_size(i == prev_index);
        const int32_t to_size = theme_swatch_size(i == next_index);

        start_theme_button_size_anim(s_wp7.theme_buttons[i], from_size, to_size);
    }

    s_wp7.theme_index = next_index;
    start_theme_color_anim(current_color, current_text_color,
                           next_color, theme_text_color_hex_for_color(next_color));
}

static void settings_reset_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || animation_speed_is_default()) {
        return;
    }

    s_wp7.anim_speed_percent = WP7_ANIM_SPEED_DEFAULT;
    set_control_anim_duration(s_wp7.settings_slider);
    lv_slider_set_value(s_wp7.settings_slider, s_wp7.anim_speed_percent, LV_ANIM_ON);
    set_reset_button_press_scale(WP7_OBJ_RELEASE_SCALE);
    update_reset_button_state();
    save_animation_speed();
}

static int32_t reset_button_current_scale(void)
{
    wp7_setting_item_t *item = &s_wp7.settings_items[WP7_SETTINGS_ITEM_RESET_BUTTON];

    if (s_wp7.settings_reset_button == NULL || item->w <= 0) {
        return WP7_OBJ_RELEASE_SCALE;
    }

    return clamp_i32(lv_obj_get_width(s_wp7.settings_reset_button) *
                     WP7_OBJ_RELEASE_SCALE / item->w,
                     WP7_OBJ_PRESS_SCALE, WP7_OBJ_RELEASE_SCALE);
}

static void set_reset_button_press_scale(int32_t scale)
{
    wp7_setting_item_t *item = &s_wp7.settings_items[WP7_SETTINGS_ITEM_RESET_BUTTON];

    if (s_wp7.settings_reset_button == NULL || s_wp7.settings_reset_label == NULL ||
            item->obj == NULL || lv_obj_has_state(s_wp7.settings_reset_button, LV_STATE_DISABLED)) {
        return;
    }

    scale = clamp_i32(scale, WP7_OBJ_PRESS_SCALE, WP7_OBJ_RELEASE_SCALE);

    const int32_t w = item->w * scale / WP7_OBJ_RELEASE_SCALE;
    const int32_t h = item->h * scale / WP7_OBJ_RELEASE_SCALE;
    const int32_t x = item->x + (item->w - w) / 2;
    const int32_t y = item->y + (item->h - h) / 2;

    lv_obj_set_style_transform_scale(s_wp7.settings_reset_button, WP7_OBJ_RELEASE_SCALE, 0);
    lv_obj_set_style_bg_color(s_wp7.settings_reset_button, theme_color(), 0);
    lv_obj_set_style_bg_opa(s_wp7.settings_reset_button, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_wp7.settings_reset_button, x, y);
    lv_obj_set_size(s_wp7.settings_reset_button, w, h);
    lv_obj_center(s_wp7.settings_reset_label);
}

static void reset_button_press_scale_anim_cb(void *var, int32_t scale)
{
    (void)var;

    set_reset_button_press_scale(scale);
}

static void start_reset_button_press_anim(bool pressed)
{
    if (s_wp7.settings_reset_button == NULL) {
        return;
    }

    lv_anim_del(s_wp7.settings_reset_button, reset_button_press_scale_anim_cb);

    lv_anim_t anim;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_wp7.settings_reset_button);
    lv_anim_set_exec_cb(&anim, reset_button_press_scale_anim_cb);
    lv_anim_set_values(&anim, reset_button_current_scale(),
                       pressed ? WP7_OBJ_PRESS_SCALE : WP7_OBJ_RELEASE_SCALE);
    lv_anim_set_duration(&anim, scaled_ui_anim_ms(WP7_PRESS_ANIM_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void settings_reset_press_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        start_reset_button_press_anim(true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        start_reset_button_press_anim(false);
    }
}

static void screen_touch_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;

    /* Presses on the status bar bubble here; no page swipes from inside a page. */
    if (s_wp7.in_settings || s_wp7.in_app) {
        return;
    }

    if (indev == NULL) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &point);
        const bool interrupted = interrupt_transition_anim_for_drag();

        if (s_wp7.animating) {
            return;
        }

        s_wp7.drag_start_x = point.x;
        s_wp7.drag_start_y = point.y;
        if (!interrupted) {
            s_wp7.drag_progress = 0;
            s_wp7.drag_target_progress = 0;
            s_wp7.drag_base_progress = 0;
            s_wp7.drag_dir = WP7_DIR_NONE;
            s_wp7.drag_interrupted_anim = false;
            s_wp7.drag_interrupted_commit = false;
        }
        s_wp7.drag_last_tick = lv_tick_get();
        s_wp7.drag_active = true;
    } else if (code == LV_EVENT_PRESSING) {
        if (!s_wp7.drag_active || s_wp7.animating) {
            return;
        }

        lv_indev_get_point(indev, &point);
        update_drag_progress(point.x, point.y);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        finish_drag();
    }
}

static void create_status_bar(lv_obj_t *screen, int32_t screen_w, int32_t status_h, int32_t pad)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, screen_w, status_h);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s_wp7.status_bar = bar;

    lv_obj_t *left_cont = lv_obj_create(bar);
    lv_obj_remove_style_all(left_cont);
    lv_obj_set_size(left_cont, LV_SIZE_CONTENT, status_h);
    lv_obj_align(left_cont, LV_ALIGN_LEFT_MID, pad, 0);
    lv_obj_set_flex_flow(left_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_cont, 5, 0);
    lv_obj_remove_flag(left_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(left_cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s_wp7.status_left_cont = left_cont;

    lv_obj_t *wifi_label = lv_label_create(left_cont);
    lv_label_set_text(wifi_label, "");
    lv_obj_set_style_text_color(wifi_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s_wp7.status_wifi_label = wifi_label;

    lv_obj_t *ble_label = lv_label_create(left_cont);
    lv_label_set_text(ble_label, "");
    lv_obj_set_style_text_color(ble_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(ble_label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(ble_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s_wp7.status_ble_label = ble_label;

    lv_obj_t *time_label = lv_label_create(bar);
    lv_label_set_text(time_label, "--:--");
    lv_obj_set_style_text_color(time_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_16, 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(time_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s_wp7.status_time_label = time_label;

    lv_obj_t *battery_label = lv_label_create(bar);
    /* Blank until the fuel gauge reports; see wp7_ui_set_battery(). */
    lv_label_set_text(battery_label, "");
    lv_obj_set_style_text_color(battery_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -pad, 0);
    lv_obj_add_flag(battery_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    s_wp7.status_battery_label = battery_label;

    wp7_ui_set_wifi(s_wp7.wifi_status);
    wp7_ui_set_ble(s_wp7.ble_status);
}

static void create_tile_grid(lv_obj_t *screen, int32_t screen_w, int32_t screen_h, int32_t status_h)
{
    const int32_t pad = scaled_px(screen_w, WP7_SCREEN_PAD_PERMILLE);
    const int32_t gap = scaled_px(screen_w, WP7_TILE_GAP_PERMILLE);
    const int32_t content_top = status_h + pad;
    const int32_t content_h = screen_h - content_top - pad;
    int32_t tile = (screen_w - (pad * 2) - gap) / WP7_COLUMNS;
    const int32_t three_row_tile = (content_h - gap * 2) / 3;
    if (tile > three_row_tile) tile = three_row_tile;
    const int32_t grid_w = tile * WP7_COLUMNS + gap;
    const int32_t grid_left = (screen_w - grid_w) / 2;
    int32_t rows = (content_h + gap) / (tile + gap);

    if (rows > WP7_MAX_ROWS) {
        rows = WP7_MAX_ROWS;
    }

    s_wp7.tile_count = rows * WP7_COLUMNS;

    for (int32_t row = 0; row < rows; row++) {
        for (int32_t col = 0; col < WP7_COLUMNS; col++) {
            const int32_t tile_number = row * WP7_COLUMNS + col + 1;
            if (tile_number > WP7_MAX_TILES) {
                return;
            }

            lv_obj_t *tile_obj = lv_obj_create(screen);
            lv_obj_remove_style_all(tile_obj);
            lv_obj_set_size(tile_obj, tile, tile);
            lv_obj_set_pos(tile_obj, grid_left + col * (tile + gap), content_top + row * (tile + gap));
            lv_obj_set_style_bg_color(tile_obj, theme_color(), 0);
            lv_obj_set_style_bg_color(tile_obj, theme_color(), LV_STATE_PRESSED);
            lv_obj_set_style_bg_color(tile_obj, theme_color(), LV_STATE_FOCUSED);
            lv_obj_set_style_bg_color(tile_obj, theme_color(),
                                      LV_STATE_PRESSED | LV_STATE_FOCUSED);
            lv_obj_set_style_bg_opa(tile_obj, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_opa(tile_obj, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(tile_obj, LV_OPA_COVER, LV_STATE_FOCUSED);
            lv_obj_set_style_bg_opa(tile_obj, LV_OPA_COVER,
                                    LV_STATE_PRESSED | LV_STATE_FOCUSED);
            lv_obj_set_style_radius(tile_obj, 0, 0);
            lv_obj_remove_flag(tile_obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(tile_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

            lv_obj_t *label = lv_label_create(tile_obj);
            lv_label_set_text(label, "");
            lv_obj_set_style_text_color(label, theme_text_color(), 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(label);
            lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

            wp7_tile_t *tile_data = &s_wp7.tiles[tile_number - 1];
            tile_data->obj = tile_obj;
            tile_data->label = label;
            tile_data->x = grid_left + col * (tile + gap);
            tile_data->y = content_top + row * (tile + gap);
            tile_data->size = tile;
            tile_data->label_page = -1;
            tile_data->label_index = -1;
            tile_data->current_x = tile_data->x;
            tile_data->current_y = tile_data->y;
            tile_data->current_w = tile;
            tile_data->current_h = tile;
            tile_data->current_scale = 256;
            tile_data->current_opa = LV_OPA_COVER;
            tile_data->hidden = false;
            tile_data->frame_valid = true;
            tile_data->style_valid = true;
            tile_data->press_active = false;
            tile_data->press_animating = false;
            tile_data->press_releasing = false;

            lv_obj_add_event_cb(tile_obj, tile_press_event_cb, LV_EVENT_PRESSED, tile_data);
            lv_obj_add_event_cb(tile_obj, tile_press_event_cb, LV_EVENT_PRESSING, tile_data);
            lv_obj_add_event_cb(tile_obj, tile_press_event_cb, LV_EVENT_RELEASED, tile_data);
            lv_obj_add_event_cb(tile_obj, tile_press_event_cb, LV_EVENT_PRESS_LOST, tile_data);
            lv_obj_add_event_cb(tile_obj, settings_tile_clicked_cb, LV_EVENT_CLICKED, tile_data);
        }
    }
}

static void create_list_page(lv_obj_t *screen, int32_t screen_w, int32_t screen_h, int32_t status_h)
{
    const int32_t gap = scaled_px(screen_h, WP7_LIST_ROW_GAP_PERMILLE);
    const int32_t row_h = scaled_px(screen_h, WP7_LIST_ROW_H_PERMILLE);
    const int32_t content_top = status_h + scaled_px(screen_h, 72);
    const int32_t row_w = scaled_px(screen_w, 700);
    const int32_t row_x = (screen_w - row_w) / 2 +
                          scaled_px(screen_w, WP7_LIST_RIGHT_OFFSET_PERMILLE);
    const int32_t icon_size = row_h * 7 / 10;

    s_wp7.list_count = WP7_LIST_ITEM_COUNT;

    for (int32_t i = 0; i < s_wp7.list_count; i++) {
        lv_obj_t *item_obj = lv_obj_create(screen);
        lv_obj_remove_style_all(item_obj);
        lv_obj_set_size(item_obj, row_w, row_h);
        lv_obj_set_pos(item_obj, row_x, content_top + i * (row_h + gap));
        lv_obj_set_style_bg_color(item_obj, ui_bg_color(), 0);
        lv_obj_set_style_bg_color(item_obj, ui_bg_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(item_obj, ui_bg_color(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(item_obj, ui_bg_color(),
                                  LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(item_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(item_obj, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(item_obj, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(item_obj, LV_OPA_COVER,
                                LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_radius(item_obj, 0, 0);
        lv_obj_remove_flag(item_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *icon = lv_obj_create(item_obj);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, icon_size, icon_size);
        lv_obj_set_style_bg_color(icon, theme_color(), 0);
        lv_obj_set_style_bg_color(icon, theme_color(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(icon, theme_color(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(icon, theme_color(),
                                  LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER,
                                LV_STATE_PRESSED | LV_STATE_FOCUSED);
        lv_obj_set_style_radius(icon, 0, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *icon_label = lv_label_create(icon);
        lv_label_set_text_fmt(icon_label, "%c", s_list_labels[i][0]);
        lv_obj_set_style_text_color(icon_label, theme_text_color(), 0);
        lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_14, 0);
        lv_obj_center(icon_label);
        lv_obj_add_flag(icon_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t *label = lv_label_create(item_obj);
        lv_label_set_text(label, s_list_labels[i]);
        lv_obj_set_style_text_color(label, ui_text_color(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_align_to(label, icon, LV_ALIGN_OUT_RIGHT_MID, scaled_px(screen_w, 28), 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);

        wp7_list_item_t *item_data = &s_wp7.list_items[i];
        item_data->obj = item_obj;
        item_data->icon = icon;
        item_data->icon_label = icon_label;
        item_data->label = label;
        item_data->x = row_x;
        item_data->y = content_top + i * (row_h + gap);
        item_data->w = row_w;
        item_data->h = row_h;
        item_data->icon_size = icon_size;
        item_data->current_x = row_x;
        item_data->current_y = content_top + i * (row_h + gap);
        item_data->current_w = row_w;
        item_data->current_h = row_h;
        item_data->current_icon_size = icon_size;
        item_data->current_opa = LV_OPA_COVER;
        item_data->current_scale = WP7_OBJ_RELEASE_SCALE;
        item_data->hidden = true;
        item_data->frame_valid = true;
        item_data->style_valid = true;
        item_data->press_active = false;
        item_data->press_animating = false;
        item_data->press_releasing = false;

        lv_obj_add_flag(item_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(item_obj, list_item_press_event_cb, LV_EVENT_PRESSED, item_data);
        lv_obj_add_event_cb(item_obj, list_item_press_event_cb, LV_EVENT_PRESSING, item_data);
        lv_obj_add_event_cb(item_obj, list_item_press_event_cb, LV_EVENT_RELEASED, item_data);
        lv_obj_add_event_cb(item_obj, list_item_press_event_cb, LV_EVENT_PRESS_LOST, item_data);
        lv_obj_add_event_cb(item_obj, settings_list_clicked_cb, LV_EVENT_CLICKED, item_data);
    }
}

static void set_setting_item(wp7_setting_item_index_t index, lv_obj_t *obj,
                             int32_t x, int32_t y, int32_t w, int32_t h)
{
    s_wp7.settings_items[index] = (wp7_setting_item_t) {
        .obj = obj,
        .x = x,
        .y = y,
        .w = w,
        .h = h,
    };
}

static lv_obj_t *create_settings_label(lv_obj_t *screen, const char *text,
                                       int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *label = lv_label_create(screen);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, ui_text_color(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_size(label, w, h);
    lv_obj_set_pos(label, x, y);

    return label;
}

/* LVGL draws a slider knob as wide as the slider is tall and centres it on
   the indicator's last pixel, so at either end of the range half the knob
   hung outside the control and over the focus outline. Start the knob just
   after that pixel and end the indicator a whole knob short of the right
   edge: at the minimum the knob fills the left end, at the maximum the right.
   Call it once the slider has its height. */
static void keep_knob_inside(lv_obj_t *slider, int32_t height)
{
    lv_obj_set_style_pad_left(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(slider, height, LV_PART_MAIN);
    lv_obj_set_style_pad_left(slider, -(height / 2) - 1, LV_PART_KNOB);
    lv_obj_set_style_pad_right(slider, height / 2 + 1, LV_PART_KNOB);
}

static void style_settings_slider(lv_obj_t *slider, int32_t screen_w, int32_t screen_h)
{
    lv_obj_remove_style_all(slider);
    set_control_anim_duration(slider);
    lv_obj_set_style_bg_color(slider, ui_track_color(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_height(slider, scaled_px(screen_h, 12), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, theme_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, ui_text_color(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_width(slider, scaled_px(screen_w, 36), LV_PART_KNOB);
    lv_obj_set_style_height(slider, scaled_px(screen_h, 36), LV_PART_KNOB);
    lv_obj_set_style_transform_width(slider, WP7_KNOB_RELEASE_EXTEND, LV_PART_KNOB);
    lv_obj_set_style_transform_height(slider, WP7_KNOB_RELEASE_EXTEND, LV_PART_KNOB);
}

static void style_wp7_switch_slider(lv_obj_t *sw, int32_t w, int32_t h)
{
    lv_obj_remove_style_all(sw);
    lv_slider_set_range(sw, 0, WP7_SWITCH_VALUE_MAX);
    set_control_anim_duration(sw);
    lv_obj_set_size(sw, w, h);
    keep_knob_inside(sw, h);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            LV_PART_INDICATOR | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            LV_PART_KNOB | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_radius(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_width(sw, h, LV_PART_KNOB);
    lv_obj_set_style_height(sw, h, LV_PART_KNOB);
    lv_obj_set_style_transform_width(sw, WP7_KNOB_RELEASE_EXTEND, LV_PART_KNOB);
    lv_obj_set_style_transform_height(sw, WP7_KNOB_RELEASE_EXTEND, LV_PART_KNOB);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_settings_page(lv_obj_t *screen, int32_t screen_w, int32_t screen_h, int32_t status_h)
{
    /* Fit the original controls to 240x320 without making a label shorter than its font. */
    const int32_t pad = scaled_px(screen_w, WP7_SCREEN_PAD_PERMILLE);
    const int32_t content_w = screen_w - pad * 2;
    const int32_t theme_columns = WP7_THEME_COLUMNS;
    const int32_t swatch_gap_x = scaled_px(screen_w, 100);
    const int32_t swatch_gap_y = 4;
    const int32_t swatch = (content_w - swatch_gap_x * (theme_columns - 1)) / theme_columns;
    const int32_t picker_h = swatch * WP7_THEME_ROW_COUNT +
                             swatch_gap_y * (WP7_THEME_ROW_COUNT - 1);
    const int32_t title_y = status_h + 4;
    const int32_t title_h = lv_font_montserrat_22.line_height + 4;
    const int32_t label_h = lv_font_montserrat_16.line_height + 2;
    const int32_t slider_h = 14;
    const int32_t mode_h = label_h;
    const int32_t button_w = scaled_px(screen_w, 230);
    const int32_t button_h = 24;
    const int32_t mode_switch_h = 16;
    const int32_t mode_switch_w = mode_switch_h * 2;
    const int32_t brightness_label_y = title_y + title_h + 4;
    const int32_t brightness_slider_y = brightness_label_y + label_h + 2;
    /* The gaps fit the last switch and its 3 px focus outline above the
       bottom edge of a 320 px screen. */
    const int32_t mode_y = brightness_slider_y + slider_h + 3;
    const int32_t mode_switch_y = mode_y + mode_h + 2;
    const int32_t theme_label_y = mode_switch_y + mode_switch_h + 3;
    const int32_t theme_picker_y = theme_label_y + label_h + 3;
    const int32_t speed_label_y = theme_picker_y + picker_h + 3;
    const int32_t speed_slider_y = speed_label_y + label_h + 2;
    const int32_t button_x = screen_w - pad - button_w;
    const int32_t button_y = speed_slider_y + slider_h + 3;
    const int32_t fast_label_y = button_y + button_h + 3;
    const int32_t fast_switch_y = fast_label_y + mode_h + 2;
    const int32_t mode_label_w = content_w;
    const int32_t mode_switch_x = pad + content_w - mode_switch_w -
                                  scaled_px(screen_w, 30);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "UI Settings");
    lv_obj_set_style_text_color(title, ui_text_color(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_pivot_x(title, 0, 0);
    lv_obj_set_style_transform_pivot_y(title, title_h / 2, 0);
    lv_obj_set_size(title, content_w, title_h);
    lv_obj_set_pos(title, pad, title_y);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, settings_title_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_wp7.settings_title = title;
    s_wp7.settings_title_x = pad;
    s_wp7.settings_title_y = title_y;
    s_wp7.settings_title_w = content_w;
    s_wp7.settings_title_h = title_h;
    s_wp7.settings_title_current_x = pad;
    s_wp7.settings_title_current_scale = 256;
    s_wp7.settings_title_current_opa = LV_OPA_COVER;
    s_wp7.settings_title_current_text_hex = ui_text_color_hex();
    s_wp7.settings_title_hidden = false;
    s_wp7.settings_title_frame_valid = true;
    s_wp7.settings_title_style_valid = true;

    lv_obj_t *brightness_label = create_settings_label(screen, "Brightness",
                                                       pad, brightness_label_y,
                                                       content_w, label_h);
    set_setting_item(WP7_SETTINGS_ITEM_BRIGHTNESS_LABEL, brightness_label,
                     pad, brightness_label_y, content_w, label_h);

    lv_obj_t *brightness_slider = lv_slider_create(screen);
    style_settings_slider(brightness_slider, screen_w, screen_h);
    lv_slider_set_range(brightness_slider, WP7_BRIGHTNESS_MIN, WP7_BRIGHTNESS_MAX);
    lv_slider_set_value(brightness_slider, sanitize_brightness(s_wp7.brightness_percent), LV_ANIM_OFF);
    lv_obj_set_size(brightness_slider, content_w, slider_h);
    keep_knob_inside(brightness_slider, slider_h);
    lv_obj_set_pos(brightness_slider, pad, brightness_slider_y);
    lv_obj_add_event_cb(brightness_slider, settings_brightness_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(brightness_slider, settings_brightness_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(brightness_slider, settings_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_slider, settings_brightness_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(brightness_slider, settings_brightness_cb, LV_EVENT_PRESS_LOST, NULL);
    s_wp7.brightness_slider = brightness_slider;
    set_setting_item(WP7_SETTINGS_ITEM_BRIGHTNESS_SLIDER, brightness_slider,
                     pad, brightness_slider_y, content_w, slider_h);

    lv_obj_t *mode_label = lv_label_create(screen);
    lv_label_set_text(mode_label, "Dark mode");
    lv_obj_set_style_text_color(mode_label, ui_text_color(), 0);
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_16, 0);
    lv_obj_set_size(mode_label, mode_label_w, mode_h);
    lv_obj_set_pos(mode_label, pad, mode_y);
    s_wp7.mode_label = mode_label;
    set_setting_item(WP7_SETTINGS_ITEM_MODE_LABEL, mode_label,
                     pad, mode_y, mode_label_w, mode_h);

    lv_obj_t *mode_switch = lv_slider_create(screen);
    style_wp7_switch_slider(mode_switch, mode_switch_w, mode_switch_h);
    lv_obj_set_pos(mode_switch, mode_switch_x, mode_switch_y);
    set_wp7_switch_value(mode_switch, s_wp7.dark_mode, LV_ANIM_OFF);
    set_wp7_switch_style(mode_switch, s_wp7.dark_mode);
    lv_obj_add_event_cb(mode_switch, settings_mode_switch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(mode_switch, settings_mode_switch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(mode_switch, settings_mode_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(mode_switch, settings_mode_switch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(mode_switch, settings_mode_switch_cb, LV_EVENT_PRESS_LOST, NULL);
    s_wp7.mode_switch = mode_switch;
    set_setting_item(WP7_SETTINGS_ITEM_MODE_SWITCH, mode_switch,
                     mode_switch_x, mode_switch_y, mode_switch_w, mode_switch_h);

    lv_obj_t *theme_label = create_settings_label(screen, "Theme color",
                                                  pad, theme_label_y,
                                                  content_w, label_h);
    set_setting_item(WP7_SETTINGS_ITEM_THEME_LABEL, theme_label,
                     pad, theme_label_y, content_w, label_h);

    s_wp7.theme_swatch_cell_size = swatch;

    for (int32_t row = 0; row < WP7_THEME_ROW_COUNT; row++) {
        lv_obj_t *theme_row = lv_obj_create(screen);
        const int32_t row_y = theme_picker_y + row * (swatch + swatch_gap_y);

        lv_obj_remove_style_all(theme_row);
        lv_obj_set_size(theme_row, content_w, swatch);
        lv_obj_set_pos(theme_row, pad, row_y);
        lv_obj_remove_flag(theme_row, LV_OBJ_FLAG_SCROLLABLE);
        s_wp7.theme_rows[row] = theme_row;
        set_setting_item(row == 0 ? WP7_SETTINGS_ITEM_THEME_ROW_0 :
                         WP7_SETTINGS_ITEM_THEME_ROW_1,
                         theme_row, pad, row_y, content_w, swatch);
    }

    for (int32_t i = 0; i < WP7_THEME_COLOR_COUNT; i++) {
        const int32_t swatch_col = i % theme_columns;
        const int32_t swatch_row = i / theme_columns;
        const bool selected = i == sanitize_theme_index(s_wp7.theme_index);
        const int32_t swatch_size = theme_swatch_size(selected);
        const int32_t swatch_cell_x = swatch_col * (swatch + swatch_gap_x);
        lv_obj_t *swatch_obj = lv_obj_create(s_wp7.theme_rows[swatch_row]);

        lv_obj_remove_style_all(swatch_obj);
        lv_obj_set_size(swatch_obj, swatch_size, swatch_size);
        lv_obj_set_pos(swatch_obj,
                       swatch_cell_x + (swatch - swatch_size) / 2,
                       (swatch - swatch_size) / 2);
        lv_obj_set_style_bg_color(swatch_obj, lv_color_hex(s_wp7_theme_colors[i]), 0);
        lv_obj_set_style_bg_opa(swatch_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(swatch_obj, 0, 0);
        lv_obj_set_style_border_opa(swatch_obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(swatch_obj, 0, 0);
        lv_obj_remove_flag(swatch_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(swatch_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch_obj, settings_theme_clicked_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_wp7.theme_buttons[i] = swatch_obj;
    }

    lv_obj_t *speed_label = create_settings_label(screen, "Animation speed",
                                                  pad, speed_label_y,
                                                  content_w, label_h);
    set_setting_item(WP7_SETTINGS_ITEM_SPEED_LABEL, speed_label,
                     pad, speed_label_y, content_w, label_h);

    lv_obj_t *slider = lv_slider_create(screen);
    style_settings_slider(slider, screen_w, screen_h);
    lv_slider_set_range(slider, WP7_ANIM_SPEED_MIN, WP7_ANIM_SPEED_MAX);
    lv_slider_set_value(slider, sanitize_animation_speed(s_wp7.anim_speed_percent), LV_ANIM_OFF);
    lv_obj_set_size(slider, content_w, slider_h);
    keep_knob_inside(slider, slider_h);
    lv_obj_set_pos(slider, pad, speed_slider_y);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(slider, settings_slider_cb, LV_EVENT_PRESS_LOST, NULL);
    s_wp7.settings_slider = slider;
    set_setting_item(WP7_SETTINGS_ITEM_SPEED_SLIDER, slider,
                     pad, speed_slider_y, content_w, slider_h);

    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, button_w, button_h);
    lv_obj_set_pos(button, button_x, button_y);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, settings_reset_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(button, settings_reset_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(button, settings_reset_press_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(button, settings_reset_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Reset");
    lv_obj_set_style_text_font(button_label, &lv_font_montserrat_16, 0);
    lv_obj_center(button_label);

    s_wp7.settings_reset_button = button;
    s_wp7.settings_reset_label = button_label;
    set_setting_item(WP7_SETTINGS_ITEM_RESET_BUTTON, button,
                     button_x, button_y, button_w, button_h);

    lv_obj_t *fast_label = create_settings_label(screen, "Fast animations",
                                                 pad, fast_label_y,
                                                 content_w, label_h);
    set_setting_item(WP7_SETTINGS_ITEM_FAST_LABEL, fast_label,
                     pad, fast_label_y, content_w, label_h);

    lv_obj_t *fast_switch = lv_slider_create(screen);
    style_wp7_switch_slider(fast_switch, mode_switch_w, mode_switch_h);
    lv_obj_set_pos(fast_switch, mode_switch_x, fast_switch_y);
    set_wp7_switch_value(fast_switch, s_wp7.fast_animations, LV_ANIM_OFF);
    set_wp7_switch_style(fast_switch, s_wp7.fast_animations);
    lv_obj_add_event_cb(fast_switch, settings_fast_switch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(fast_switch, settings_fast_switch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(fast_switch, settings_fast_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(fast_switch, settings_fast_switch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(fast_switch, settings_fast_switch_cb, LV_EVENT_PRESS_LOST, NULL);
    s_wp7.fast_switch = fast_switch;
    set_setting_item(WP7_SETTINGS_ITEM_FAST_SWITCH, fast_switch,
                     mode_switch_x, fast_switch_y, mode_switch_w, mode_switch_h);

    update_theme_controls();
    hide_settings_items();
}

static void create_wp7_first_screen(void)
{
    lv_obj_t *screen = lv_screen_active();
    const int32_t screen_w = BSP_LCD_W;
    const int32_t screen_h = BSP_LCD_H;
    const int32_t status_h = scaled_px(screen_h, WP7_STATUS_BAR_PERMILLE);
    const int32_t pad = scaled_px(screen_w, WP7_SCREEN_PAD_PERMILLE);

    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, screen_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, screen_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(screen, screen_touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(screen, screen_touch_cb, LV_EVENT_PRESS_LOST, NULL);

    s_wp7 = (wp7_screen_t) {
        .page = 0,
        .target_page = 0,
        .screen_w = screen_w,
        .screen_h = screen_h,
        .settings_tile_index = WP7_SETTINGS_TILE_INDEX,
        .settings_list_index = WP7_LIST_UI_SETTINGS_INDEX,
        .anim_speed_percent = WP7_ANIM_SPEED_DEFAULT,
        .brightness_percent = WP7_BRIGHTNESS_DEFAULT,
        .theme_index = 7,
        .dark_mode = true,
        .fast_animations = false,
        .key_tile_index = -1,
        .key_list_index = -1,
        .key_setting_index = -1,
    };
    load_ui_settings();
    s_wp7.brightness_hw_percent = sanitize_brightness(s_wp7.brightness_percent);
    lv_obj_set_style_bg_color(screen, ui_bg_color(), 0);

    create_status_bar(screen, screen_w, status_h, pad);
    wp7_apps_init(s_wp7.status_time_label);
    create_tile_grid(screen, screen_w, screen_h, status_h);
    render_static_page(0);
}

void wp7_ui_start(void)
{
    init_nvs();
    create_wp7_first_screen();
    set_brightness_immediate(s_wp7.brightness_percent);
}

int32_t wp7_ui_anim_ms(int32_t base_ms)
{
    return scaled_ui_anim_ms(base_ms);
}

static const char *battery_symbol(int soc)
{
    /* Five glyphs, each centred on 0/25/50/75/100%. */
    static const char *const symbols[] = {
        LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
        LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL,
    };
    if (soc > 100) soc = 100;
    return symbols[(soc + 12) / 25];
}

void wp7_ui_set_battery(int soc, int mv)
{
    wp7_apps_set_battery(soc, mv);
    /* A failed read (-1) keeps the last icon rather than flashing empty. */
    if (soc >= 0 && s_wp7.status_battery_label != NULL) {
        char text[16];
        snprintf(text, sizeof(text), "%d%% %s", soc > 100 ? 100 : soc, battery_symbol(soc));
        lv_label_set_text(s_wp7.status_battery_label, text);
    }
}

void wp7_ui_set_wifi(wp7_wifi_status_t status)
{
    s_wp7.wifi_status = status;
    if (s_wp7.status_wifi_label == NULL) return;

    switch (status) {
        case WP7_WIFI_CONNECTED:
            lv_label_set_text(s_wp7.status_wifi_label, LV_SYMBOL_WIFI);
            lv_obj_set_style_opa(s_wp7.status_wifi_label, LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_wp7.status_wifi_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case WP7_WIFI_CONNECTING:
            lv_label_set_text(s_wp7.status_wifi_label, LV_SYMBOL_WIFI);
            lv_obj_set_style_opa(s_wp7.status_wifi_label, LV_OPA_40, 0);
            lv_obj_remove_flag(s_wp7.status_wifi_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case WP7_WIFI_DISCONNECTED:
        default:
            lv_label_set_text(s_wp7.status_wifi_label, "");
            lv_obj_add_flag(s_wp7.status_wifi_label, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

void wp7_ui_set_ble(wp7_ble_status_t status)
{
    s_wp7.ble_status = status;
    wp7_apps_set_ble_available(status != WP7_BLE_DISABLED);
    if (s_wp7.status_ble_label == NULL) return;

    switch (status) {
        case WP7_BLE_CONNECTED:
            lv_label_set_text(s_wp7.status_ble_label, LV_SYMBOL_BLUETOOTH);
            lv_obj_set_style_opa(s_wp7.status_ble_label, LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_wp7.status_ble_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case WP7_BLE_ADVERTISING:
            lv_label_set_text(s_wp7.status_ble_label, LV_SYMBOL_BLUETOOTH);
            lv_obj_set_style_opa(s_wp7.status_ble_label, LV_OPA_50, 0);
            lv_obj_remove_flag(s_wp7.status_ble_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case WP7_BLE_DISABLED:
        default:
            lv_label_set_text(s_wp7.status_ble_label, "");
            lv_obj_add_flag(s_wp7.status_ble_label, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

static lv_obj_t *key_setting_obj(int32_t index)
{
    switch (index) {
        case 0: return s_wp7.brightness_slider;
        case 1: return s_wp7.mode_switch;
        case 2: return s_wp7.theme_buttons[s_wp7.theme_index];
        case 3: return s_wp7.settings_slider;
        case 4: return s_wp7.settings_reset_button;
        case 5: return s_wp7.fast_switch;
        default: return NULL;
    }
}

static void clear_key_focus(void)
{
    for (int32_t i = 0; i < s_wp7.tile_count; ++i) {
        if (s_wp7.tiles[i].obj) lv_obj_set_style_outline_width(s_wp7.tiles[i].obj, 0, 0);
    }
    for (int32_t i = 0; i < s_wp7.list_count; ++i) {
        if (s_wp7.list_items[i].obj) lv_obj_set_style_outline_width(s_wp7.list_items[i].obj, 0, 0);
    }
    for (int32_t i = 0; i < 6; ++i) {
        lv_obj_t *obj = key_setting_obj(i);
        if (obj) lv_obj_set_style_outline_width(obj, 0, 0);
    }
}

static void key_render_focus(void)
{
    clear_key_focus();
    lv_obj_t *target = NULL;
    if (s_wp7.in_app) {
        return;
    } else if (s_wp7.in_settings) {
        target = key_setting_obj(s_wp7.key_setting_index);
    } else if (s_wp7.in_list && s_wp7.key_list_index >= 0 &&
               s_wp7.key_list_index < s_wp7.list_count) {
        target = s_wp7.list_items[s_wp7.key_list_index].obj;
    } else if (!s_wp7.in_list && s_wp7.key_tile_index >= 0 &&
               s_wp7.key_tile_index < s_wp7.tile_count) {
        target = s_wp7.tiles[s_wp7.key_tile_index].obj;
    }
    if (target) {
        lv_obj_set_style_outline_color(target, ui_text_color(), 0);
        lv_obj_set_style_outline_pad(target, 1, 0);
        lv_obj_set_style_outline_width(target, 2, 0);
    }
}

static void key_open_settings(bool from_list)
{
    s_wp7.key_setting_index = 0;
    open_page(from_list, from_list ? WP7_LIST_UI_SETTINGS_INDEX : WP7_SETTINGS_TILE_INDEX);
}

static void key_change_setting(void)
{
    switch (s_wp7.key_setting_index) {
        case 0: {
            /* Stop at the maximum before wrapping so 100% stays reachable:
               100 -> 5 -> 15 ... 95 -> 100. */
            int32_t next = s_wp7.brightness_percent + 10;
            if (s_wp7.brightness_percent >= WP7_BRIGHTNESS_MAX) next = WP7_BRIGHTNESS_MIN;
            else if (next > WP7_BRIGHTNESS_MAX) next = WP7_BRIGHTNESS_MAX;
            animate_brightness_to(next);
            lv_slider_set_value(s_wp7.brightness_slider, next, LV_ANIM_ON);
            save_brightness();
            break;
        }
        case 1: {
            const bool next = !s_wp7.dark_mode;
            set_wp7_switch_value(s_wp7.mode_switch, next, LV_ANIM_ON);
            set_wp7_switch_style(s_wp7.mode_switch, next);
            start_color_scheme_anim(next);
            save_dark_mode();
            break;
        }
        case 2: {
            const int32_t next = (s_wp7.theme_index + 1) % WP7_THEME_COLOR_COUNT;
            lv_obj_send_event(s_wp7.theme_buttons[next], LV_EVENT_CLICKED, NULL);
            break;
        }
        case 3: {
            int32_t next = s_wp7.anim_speed_percent + 15;
            if (next > WP7_ANIM_SPEED_MAX) next = WP7_ANIM_SPEED_MIN;
            s_wp7.anim_speed_percent = next;
            lv_slider_set_value(s_wp7.settings_slider, next, LV_ANIM_ON);
            update_reset_button_state();
            save_animation_speed();
            break;
        }
        case 4:
            lv_obj_send_event(s_wp7.settings_reset_button, LV_EVENT_CLICKED, NULL);
            break;
        case 5: {
            const bool next = !s_wp7.fast_animations;
            s_wp7.fast_animations = next;
            set_wp7_switch_value(s_wp7.fast_switch, next, LV_ANIM_ON);
            set_wp7_switch_style(s_wp7.fast_switch, next);
            update_theme_controls();
            save_fast_animations();
            break;
        }
    }
    key_render_focus();
}

/* OK sinks the focused tile or list row in, like a touch press, while the key
   is held; the action runs on release or long press. */
void wp7_ui_key_down(wp7_key_t key)
{
    if (key != WP7_KEY_OK || s_wp7.animating || s_wp7.drag_active ||
            s_wp7.in_app || s_wp7.in_settings) {
        return;
    }

    if (s_wp7.in_list) {
        if (s_wp7.key_list_index >= 0 && s_wp7.key_list_index < s_wp7.list_count) {
            start_list_item_press_anim(&s_wp7.list_items[s_wp7.key_list_index], true);
        }
    } else if (s_wp7.page == 0 && s_wp7.key_tile_index >= 0 &&
               s_wp7.key_tile_index < s_wp7.tile_count) {
        start_tile_press_anim(&s_wp7.tiles[s_wp7.key_tile_index], true);
    }
}

/* Restores a tile or row pressed by wp7_ui_key_down() before an action takes
   over its geometry. */
static void release_key_press(void)
{
    if (s_wp7.in_list) {
        release_list_press_for_transition();
        return;
    }

    for (int32_t i = 0; i < s_wp7.tile_count; i++) {
        wp7_tile_t *tile = &s_wp7.tiles[i];

        if (tile->obj == NULL || (!tile->press_active && !tile->press_animating)) {
            continue;
        }

        lv_anim_del(tile, tile_press_scale_anim_cb);
        tile->press_active = false;
        tile->press_animating = false;
        tile->press_releasing = false;
        set_tile_press_scale(tile, WP7_OBJ_RELEASE_SCALE);
    }
}

void wp7_ui_key(wp7_key_t key, bool long_press)
{
    if (s_wp7.animating || s_wp7.drag_active) return;
    if (key == WP7_KEY_OK && !s_wp7.in_app && !s_wp7.in_settings) {
        release_key_press();
    }
    if (s_wp7.in_app) {
        if (key == WP7_KEY_OK && long_press) {
            start_progress_anim(WP7_DIR_APP_CLOSE, 0,
                                transition_progress_max_for_dir(WP7_DIR_APP_CLOSE), true);
        } else {
            wp7_apps_key(key, long_press);
        }
        return;
    }
    if (s_wp7.in_settings) {
        if (key == WP7_KEY_OK && long_press) {
            start_progress_anim(WP7_DIR_SETTINGS_CLOSE, 0,
                                transition_progress_max_for_dir(WP7_DIR_SETTINGS_CLOSE), true);
        } else if (key == WP7_KEY_OK) {
            key_change_setting();
        } else if (!long_press) {
            const int32_t step = key == WP7_KEY_DOWN ? 1 : -1;
            s_wp7.key_setting_index = (s_wp7.key_setting_index + step + 6) % 6;
            key_render_focus();
        }
        return;
    }
    if (s_wp7.in_list) {
        if (key == WP7_KEY_OK && long_press) {
            start_progress_anim(WP7_DIR_HOME, 0,
                                transition_progress_max_for_dir(WP7_DIR_HOME), true);
        } else if (key == WP7_KEY_OK &&
                   s_wp7.key_list_index == WP7_LIST_UI_SETTINGS_INDEX) {
            key_open_settings(true);
        } else if (key == WP7_KEY_OK && s_wp7.key_list_index >= 0 &&
                   s_wp7.key_list_index < WP7_APP_COUNT) {
            open_page(true, s_wp7.key_list_index);
        } else if (key != WP7_KEY_OK && !long_press && s_wp7.list_count > 0) {
            const int32_t step = key == WP7_KEY_DOWN ? 1 : -1;
            s_wp7.key_list_index = (s_wp7.key_list_index + step + s_wp7.list_count) % s_wp7.list_count;
            key_render_focus();
        }
        return;
    }
    if (key == WP7_KEY_OK) {
        if (!long_press && s_wp7.page == 0 &&
            s_wp7.key_tile_index == WP7_SETTINGS_TILE_INDEX) {
            key_open_settings(false);
        } else if (!long_press && s_wp7.page == 0 &&
                   s_wp7.key_tile_index >= 0 &&
                   s_wp7.key_tile_index < WP7_SETTINGS_TILE_INDEX) {
            open_page(false, s_wp7.key_tile_index);
        } else {
            s_wp7.key_list_index = 0;
            start_progress_anim(WP7_DIR_LIST, 0,
                                transition_progress_max_for_dir(WP7_DIR_LIST), true);
        }
        return;
    }
    if (long_press) {
        if (key == WP7_KEY_DOWN) {
            s_wp7.key_list_index = 0;
            start_progress_anim(WP7_DIR_LIST, 0,
                                transition_progress_max_for_dir(WP7_DIR_LIST), true);
        }
    } else if (s_wp7.tile_count > 0) {
        const int32_t step = key == WP7_KEY_DOWN ? 1 : -1;
        if (s_wp7.key_tile_index < 0) s_wp7.key_tile_index = 0;
        else s_wp7.key_tile_index = (s_wp7.key_tile_index + step + s_wp7.tile_count) % s_wp7.tile_count;
        key_render_focus();
    }
}
