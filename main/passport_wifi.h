#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "wp7_ui.h"

// Initialize Wi-Fi in Station mode and attempt connecting to saved AP.
// State changes are sent to status_queue.
void passport_wifi_init(QueueHandle_t status_queue);

wp7_wifi_status_t passport_wifi_get_status(void);
