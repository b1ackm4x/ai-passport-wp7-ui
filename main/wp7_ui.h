#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WP7_KEY_UP,
    WP7_KEY_DOWN,
    WP7_KEY_OK,
} wp7_key_t;

typedef enum {
    WP7_WIFI_DISCONNECTED = 0,
    WP7_WIFI_CONNECTING,
    WP7_WIFI_CONNECTED,
} wp7_wifi_status_t;

typedef enum {
    WP7_BLE_DISABLED = 0,   /* Bluetooth failed to start: icon hidden. */
    WP7_BLE_ADVERTISING,    /* Waiting for the Mac: dimmed icon. */
    WP7_BLE_CONNECTED,      /* Connected, or data arrived recently: solid icon. */
} wp7_ble_status_t;

/* Caller holds the LVGL port lock for all functions. */
void wp7_ui_start(void);
/* A key went down. Only press feedback reacts; actions wait for wp7_ui_key(). */
void wp7_ui_key_down(wp7_key_t key);
/* A key was released after a short press, or held past the long-press time. */
void wp7_ui_key(wp7_key_t key, bool long_press);
void wp7_ui_set_battery(int soc, int mv);
void wp7_ui_set_wifi(wp7_wifi_status_t status);
void wp7_ui_set_ble(wp7_ble_status_t status);
/* Scales an in-page animation duration by the UI Settings animation speed. */
int32_t wp7_ui_anim_ms(int32_t base_ms);
