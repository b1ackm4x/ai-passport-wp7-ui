#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "wp7_ui.h"

typedef enum {
    WP7_APP_AI_QUOTA,
    WP7_APP_CLOCK,
    WP7_APP_BATTERY,
    WP7_APP_STOPWATCH,
    WP7_APP_FOCUS,
    WP7_APP_COUNT,
} wp7_app_id_t;

void wp7_apps_init(lv_obj_t *status_time_label);
void wp7_apps_set_battery(int soc, int mv);
bool wp7_apps_open(lv_obj_t *screen, wp7_app_id_t app, int32_t status_h,
                   lv_color_t bg, lv_color_t text, lv_color_t accent);
void wp7_apps_close(void);
bool wp7_apps_active(void);
void wp7_apps_key(wp7_key_t key, bool long_press);
