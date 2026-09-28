#include "wp7_capture.h"

#include <string.h>

#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wp7_capture";
static const char command[] = "WP7_SCREENSHOT_V1";
static lv_display_t *s_display;
static bool s_capturing;
static bool s_capture_ok;
static uint32_t s_rect_count;

static bool send_bytes(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size > 0) {
        const size_t chunk = size > 512 ? 512 : size;
        const int sent = usb_serial_jtag_write_bytes(bytes, chunk, pdMS_TO_TICKS(1500));
        if (sent <= 0) return false;
        bytes += sent;
        size -= sent;
    }
    return true;
}

static void put_u16le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void capture_flush(lv_event_t *event)
{
    if (!s_capturing || !s_capture_ok) return;

    lv_display_t *display = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *buffer = lv_display_get_buf_active(display);
    if (!area || !buffer || !buffer->data ||
        lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) {
        s_capture_ok = false;
        return;
    }

    const int32_t width = lv_area_get_width(area);
    const int32_t height = lv_area_get_height(area);
    if (area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= BSP_LCD_W || area->y2 >= BSP_LCD_H ||
        width <= 0 || height <= 0 ||
        buffer->header.stride < (uint32_t)width * 2U) {
        s_capture_ok = false;
        return;
    }

    uint8_t header[12] = {'R', 'E', 'C', 'T'};
    put_u16le(header + 4, (uint16_t)area->x1);
    put_u16le(header + 6, (uint16_t)area->y1);
    put_u16le(header + 8, (uint16_t)width);
    put_u16le(header + 10, (uint16_t)height);
    if (!send_bytes(header, sizeof(header))) {
        s_capture_ok = false;
        return;
    }

    for (int32_t row = 0; row < height; row++) {
        const uint8_t *pixels = buffer->data + (size_t)row * buffer->header.stride;
        if (!send_bytes(pixels, (size_t)width * 2U)) {
            s_capture_ok = false;
            return;
        }
    }
    s_rect_count++;
}

static void capture_once(void)
{
    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGW(TAG, "LVGL lock unavailable");
        return;
    }

    /* The host reconstructs the frame from flush rectangles, so the ESP32-C3
       never needs a 153600-byte full-screen buffer. */
    esp_log_level_set("*", ESP_LOG_NONE);
    s_capture_ok = true;
    s_rect_count = 0;
    s_capture_ok = send_bytes("WP7_SCREENSHOT_V1 240 320 RGB565LE RECT\n",
                              sizeof("WP7_SCREENSHOT_V1 240 320 RGB565LE RECT\n") - 1);
    s_capturing = s_capture_ok;
    if (s_capture_ok) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_display);
    }
    s_capturing = false;
    if (s_capture_ok && s_rect_count > 0) {
        send_bytes("DONE", 4);
    }
    bsp_lvgl_unlock();
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
}

static void capture_task(void *argument)
{
    (void)argument;
    size_t matched = 0;
    uint8_t input[64];
    for (;;) {
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        const int count = usb_serial_jtag_read_bytes(input, sizeof(input), pdMS_TO_TICKS(100));
        if (count < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        for (int i = 0; i < count; i++) {
            const uint8_t ch = input[i];
            if (ch == '\r' || ch == '\n') {
                matched = 0;
                continue;
            }
            matched = ch == (uint8_t)command[matched] ? matched + 1 :
                      ch == (uint8_t)command[0] ? 1 : 0;
            if (matched == sizeof(command) - 1) {
                matched = 0;
                capture_once();
            }
        }
    }
}

void wp7_capture_init(lv_display_t *display)
{
    if (!display || s_display) return;
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 4096,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB capture unavailable: %s", esp_err_to_name(err));
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    s_display = display;
    lv_display_add_event_cb(display, capture_flush, LV_EVENT_FLUSH_START, NULL);
    if (xTaskCreate(capture_task, "wp7_capture", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "capture task unavailable");
    }
}
