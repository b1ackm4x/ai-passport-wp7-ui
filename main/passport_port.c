#include "wp7_ui.h"

#include <stdlib.h>

#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "wp7_capture.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    wp7_key_t key;
    bool long_press;
} input_event_t;

static QueueHandle_t s_keys;
static QueueHandle_t s_battery_updates;
static const char *TAG = "passport_wp7";

typedef struct {
    int soc;
    int mv;
} battery_update_t;

static void battery_task(void *argument)
{
    (void)argument;
    battery_update_t update = { .soc = -1, .mv = -1 };
    const esp_err_t err = bsp_battery_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable: %s", esp_err_to_name(err));
        xQueueOverwrite(s_battery_updates, &update);
        vTaskDelete(NULL);
    }
    for (;;) {
        update.soc = bsp_battery_soc();
        update.mv = bsp_battery_mv();
        xQueueOverwrite(s_battery_updates, &update);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void button_cb(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (event != BSP_BTN_CLICK && event != BSP_BTN_LONG) return;
    input_event_t input = {
        .key = (wp7_key_t)button,
        .long_press = event == BSP_BTN_LONG,
    };
    xQueueSend(s_keys, &input, 0);
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
    ESP_ERROR_CHECK(bsp_button_init(button_cb, NULL));
    if (xTaskCreate(battery_task, "passport_battery", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "battery task unavailable");
        battery_update_t unavailable = { .soc = -1, .mv = -1 };
        xQueueOverwrite(s_battery_updates, &unavailable);
    }
    ESP_LOGI(TAG, "WP7 launcher ready, 240x320 RGB565");

    input_event_t input;
    battery_update_t battery;
    while (true) {
        if (xQueueReceive(s_keys, &input, pdMS_TO_TICKS(250)) == pdTRUE &&
                bsp_lvgl_lock(1000)) {
            wp7_ui_key(input.key, input.long_press);
            bsp_lvgl_unlock();
        }
        if (xQueueReceive(s_battery_updates, &battery, 0) == pdTRUE &&
                bsp_lvgl_lock(1000)) {
            wp7_ui_set_battery(battery.soc, battery.mv);
            bsp_lvgl_unlock();
        }
    }
}
