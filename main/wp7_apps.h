#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "wp7_ui.h"

typedef enum {
    WP7_APP_KABOO,
    WP7_APP_CLAUDE,
    WP7_APP_CLOCK,
    WP7_APP_BATTERY,
    WP7_APP_STOPWATCH,
    WP7_APP_FOCUS,
    WP7_APP_COUNT,
} wp7_app_id_t;

/* Most parts an app page has: its title plus up to seven rows. */
#define WP7_APP_MAX_ITEMS 8

/* One part of an open app page, in entrance order: item 0 is the title and
   the rest are full-width rows. The frame is the resting one inside the page
   panel. The launcher's transitions move and fade the items and scale the
   title, like the UI Settings controls. */
typedef struct {
    lv_obj_t *obj;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} wp7_app_item_t;

void wp7_apps_init(lv_obj_t *status_time_label);
void wp7_apps_set_battery(int soc, int mv);
/* False when Bluetooth failed to start, so the usage pages stop waiting. */
void wp7_apps_set_ble_available(bool available);
/* Builds the page hidden; wp7_apps_set_visible() shows it. */
bool wp7_apps_open(lv_obj_t *screen, wp7_app_id_t app, int32_t status_h,
                   lv_color_t bg, lv_color_t text, lv_color_t accent);
void wp7_apps_close(void);
bool wp7_apps_active(void);
/* Copies the open page's items; returns how many were copied. */
int32_t wp7_apps_items(wp7_app_item_t *items, int32_t capacity);
void wp7_apps_set_visible(bool visible);
/* The open transition has finished: start the page's own animations. */
void wp7_apps_entered(void);
void wp7_apps_key(wp7_key_t key, bool long_press);
