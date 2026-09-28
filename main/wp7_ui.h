#pragma once

#include <stdbool.h>

typedef enum {
    WP7_KEY_UP,
    WP7_KEY_DOWN,
    WP7_KEY_OK,
} wp7_key_t;

/* Caller holds the LVGL port lock for both functions. */
void wp7_ui_start(void);
void wp7_ui_key(wp7_key_t key, bool long_press);
void wp7_ui_set_battery(int soc, int mv);
