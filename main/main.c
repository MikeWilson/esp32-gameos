// main.c — boot, service init, hand off to the shell. Nothing else.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "gos.h"
#include "gos_core.h"
#include "gos_hal.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "main";

void app_main(void)
{
    gos_save_init();                     // NVS first: settings live there
    ESP_ERROR_CHECK(bsp_i2c_init());

    if (!hal_display_init()) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }
    bool touch_ok = hal_touch_init();
    bool imu_ok = hal_imu_init();
    bool audio_ok = hal_audio_init();
    bool power_ok = hal_power_init();
    hal_button_init();

    gos_gfx_init();
    gos_input_init();
    gos_mixer_init();
    shell_init();

    ESP_LOGI(TAG, "services up: touch=%d imu=%d audio=%d power=%d",
             touch_ok, imu_ok, audio_ok, power_ok);

    gos_loop_start();
    vTaskDelete(NULL);
}
