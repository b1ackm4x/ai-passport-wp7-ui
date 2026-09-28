idf_component_register(
    SRCS "wp7_ui.c" "wp7_apps.c" "wp7_capture.c" "passport_port.c"
    INCLUDE_DIRS "."
    REQUIRES passport_bsp lvgl__lvgl nvs_flash
    PRIV_REQUIRES esp_timer esp_driver_usb_serial_jtag vfs)
