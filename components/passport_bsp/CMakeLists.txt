idf_component_register(
    SRCS "src/bsp_display.c" "src/bsp_button.c" "src/bsp_i2c.c" "src/bsp_battery.c"
         "src/bsp_display_rounding.c" "src/bsp_display_lvgl.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd button esp_adc esp_lvgl_port
    PRIV_REQUIRES esp_timer
)
