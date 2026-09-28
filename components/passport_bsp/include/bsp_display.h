#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdint.h>
#include <stdbool.h>

#define BSP_LVGL_SCREEN_RADIUS 30

#ifdef __cplusplus
extern "C" {
#endif

// Passport ST7789P3 panel initialization and SPI transport.
esp_err_t bsp_display_init(void);
esp_lcd_panel_handle_t bsp_display_panel(void);
esp_lcd_panel_io_handle_t bsp_display_io(void);
void bsp_display_backlight(uint8_t percent);
esp_err_t bsp_display_prepare_deep_sleep(void);

struct _lv_display_t;
struct _lv_display_t *bsp_lvgl_init(void);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
