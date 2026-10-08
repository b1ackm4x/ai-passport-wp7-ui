#include "wp7_ui.h"
#include "passport_wifi.h"

#include <stdlib.h>

#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "wp7_capture.h"
#include "usage_link.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef enum {
    INPUT_DOWN,
    INPUT_CLICK,
    INPUT_LONG,
} input_kind_t;

typedef struct {
    wp7_key_t key;
    input_kind_t kind;
} input_event_t;

static QueueHandle_t s_keys;
static QueueHandle_t s_battery_updates;
static QueueHandle_t s_wifi_updates;
static const char *TAG = "passport_wp7";

/* The Mac bridge connects only for each push (every 60 s by default), so a
   live link shows as a recent packet rather than a held connection. */
#define BLE_RECENT_DATA_US (3 * 60 * 1000000LL)

/* Retry a missing fuel gauge, backing off so a board without one logs rarely. */
#define BATTERY_POLL_MS        30000
#define BATTERY_RETRY_MAX_MS   (5 * 60 * 1000)

typedef struct {
    int soc;
    int mv;
} battery_update_t;

static void battery_task(void *argument)
{
    (void)argument;
    battery_update_t update = { .soc = -1, .mv = -1 };
    bool ready = false;
    bool warned = false;
    uint32_t retry_ms = BATTERY_POLL_MS;
    for (;;) {
        if (!ready) {
            const esp_err_t err = bsp_battery_init();
            ready = err == ESP_OK;
            if (!ready && !warned) {
                ESP_LOGW(TAG, "battery gauge unavailable: %s; retrying", esp_err_to_name(err));
                warned = true;
            }
        }
        update.soc = ready ? bsp_battery_soc() : -1;
        update.mv = ready ? bsp_battery_mv() : -1;
        xQueueOverwrite(s_battery_updates, &update);
        vTaskDelay(pdMS_TO_TICKS(ready ? BATTERY_POLL_MS : retry_ms));
        if (!ready && retry_ms < BATTERY_RETRY_MAX_MS) {
            retry_ms = retry_ms * 2 > BATTERY_RETRY_MAX_MS ? BATTERY_RETRY_MAX_MS : retry_ms * 2;
        }
    }
}

static void button_cb(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    input_event_t input = { .key = (wp7_key_t)button };
    switch (event) {
        case BSP_BTN_PRESS: input.kind = INPUT_DOWN; break;
        case BSP_BTN_CLICK: input.kind = INPUT_CLICK; break;
        case BSP_BTN_LONG: input.kind = INPUT_LONG; break;
        default: return;
    }
    ESP_LOGD(TAG, "Button event: btn=%d, ev=%d", (int)button, (int)event);
    xQueueSend(s_keys, &input, 0);
}

static wp7_ble_status_t ble_status(esp_err_t start_err)
{
    if (start_err != ESP_OK) return WP7_BLE_DISABLED;
    if (usage_link_connected()) return WP7_BLE_CONNECTED;
    usage_snapshot_t snapshot;
    if (usage_link_get(&snapshot, NULL) &&
            esp_timer_get_time() - snapshot.received_monotonic_us < BLE_RECENT_DATA_US) {
        return WP7_BLE_CONNECTED;
    }
    return WP7_BLE_ADVERTISING;
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    lv_display_t *display = bsp_lvgl_init();
    if (!display) {
        ESP_LOGE(TAG, "LVGL display initialization failed");
        abort();
    }
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock timed out at startup");
        abort();
    }
    wp7_ui_start();
    wp7_capture_init(display);
    bsp_lvgl_unlock();

    s_keys = xQueueCreate(12, sizeof(input_event_t));
    ESP_ERROR_CHECK(s_keys ? ESP_OK : ESP_ERR_NO_MEM);
    s_battery_updates = xQueueCreate(1, sizeof(battery_update_t));
    ESP_ERROR_CHECK(s_battery_updates ? ESP_OK : ESP_ERR_NO_MEM);
    s_wifi_updates = xQueueCreate(1, sizeof(wp7_wifi_status_t));
    ESP_ERROR_CHECK(s_wifi_updates ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(bsp_button_init(button_cb, NULL));
    if (xTaskCreate(battery_task, "passport_battery", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "battery task unavailable");
        battery_update_t unavailable = { .soc = -1, .mv = -1 };
        xQueueOverwrite(s_battery_updates, &unavailable);
    }
    /* BLE starts after the UI has initialized NVS. On failure the status bar
       hides the Bluetooth icon and the usage pages say Bluetooth is
       unavailable; the launcher still works. */
    const esp_err_t ble_err = usage_link_start();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE link unavailable: %s", esp_err_to_name(ble_err));
    }
    passport_wifi_init(s_wifi_updates);
    wp7_ble_status_t last_ble = ble_status(ble_err);
    if (bsp_lvgl_lock(1000)) {
        wp7_ui_set_ble(last_ble);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "WP7 launcher ready, 240x320 RGB565, heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    input_event_t input;
    battery_update_t battery;
    wp7_wifi_status_t wifi;
    while (true) {
        if (xQueueReceive(s_keys, &input, pdMS_TO_TICKS(250)) == pdTRUE &&
                bsp_lvgl_lock(1000)) {
            if (input.kind == INPUT_DOWN) {
                wp7_ui_key_down(input.key);
            } else {
                wp7_ui_key(input.key, input.kind == INPUT_LONG);
            }
            bsp_lvgl_unlock();
        }
        if (xQueueReceive(s_battery_updates, &battery, 0) == pdTRUE &&
                bsp_lvgl_lock(1000)) {
            wp7_ui_set_battery(battery.soc, battery.mv);
            bsp_lvgl_unlock();
        }
        if (xQueueReceive(s_wifi_updates, &wifi, 0) == pdTRUE &&
                bsp_lvgl_lock(1000)) {
            wp7_ui_set_wifi(wifi);
            bsp_lvgl_unlock();
        }
        const wp7_ble_status_t ble = ble_status(ble_err);
        if (ble != last_ble && bsp_lvgl_lock(1000)) {
            last_ble = ble;
            wp7_ui_set_ble(ble);
            bsp_lvgl_unlock();
        }
    }
}
