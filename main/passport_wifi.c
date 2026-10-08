#include "passport_wifi.h"

static wp7_wifi_status_t s_wifi_status = WP7_WIFI_DISCONNECTED;

void passport_wifi_init(QueueHandle_t status_queue)
{
    (void)status_queue;
    // On the ESP32-C3 without PSRAM, running NimBLE alongside the 96 KB LVGL pool
    // leaves insufficient internal heap for the lwIP/Wi-Fi stack, so Wi-Fi remains
    // offline and the status bar hides the Wi-Fi symbol.
    s_wifi_status = WP7_WIFI_DISCONNECTED;
}

wp7_wifi_status_t passport_wifi_get_status(void)
{
    return s_wifi_status;
}
