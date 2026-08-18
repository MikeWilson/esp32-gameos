// hal_touch.c — CST816S (or FT5x06) via the BSP; single point, raw 368x448.
//
// The CST816S auto-sleeps after release and NACKs I2C until the next contact
// wakes it — a failed read means "no finger", never "keep the old state".
// Error counts are logged (rate-limited) so chip behavior is visible in the
// serial log instead of guessed at.
#include "gos_hal.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/touch.h"

static esp_lcd_touch_handle_t tp;
static uint32_t err_count, ok_count;
static int64_t last_report_us;

bool hal_touch_init(void)
{
    if (bsp_touch_new(NULL, &tp) != ESP_OK) {
        ESP_LOGE("hal_touch", "touch init failed");
        return false;
    }
    return true;
}

bool hal_touch_read(int16_t *x, int16_t *y)
{
    if (!tp) return false;

    esp_err_t err = esp_lcd_touch_read_data(tp);

    int64_t now = esp_timer_get_time();
    if (err != ESP_OK) err_count++;
    else ok_count++;
    if (now - last_report_us > 5000000) {
        last_report_us = now;
        if (err_count)
            ESP_LOGW("hal_touch", "i2c errs %lu / ok %lu in last 5s",
                     (unsigned long)err_count, (unsigned long)ok_count);
        err_count = ok_count = 0;
    }

    if (err != ESP_OK) return false;   // asleep chip = no finger

    uint16_t tx[1], ty[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_coordinates(tp, tx, ty, NULL, &n, 1) && n > 0) {
        *x = (int16_t)tx[0];
        *y = (int16_t)ty[0];
        return true;
    }
    return false;
}
