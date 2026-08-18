// loop.c — the fixed-timestep frame loop. Runs on core 1 at priority 5.
// dt handed to updates is always 1/60 s; if a frame overruns, the loop runs
// extra updates (up to 4) and skips renders, never the other way around.
#include "gos.h"
#include "gos_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#define FRAME_US 16667

static void game_task(void *arg)
{
    int64_t next = esp_timer_get_time();
    int64_t busy_us = 0, stat_t0 = next;
    int frames = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int n_updates = 1;
        if (now > next + FRAME_US) {
            int64_t lag = now - next;
            n_updates = 1 + (int)(lag / FRAME_US);
            if (n_updates > 4) { n_updates = 4; next = now; }
        }

        int64_t work_t0 = esp_timer_get_time();
        gos_input_update(1.0f / 60.0f);
        shell_frame(gos_input_get(), n_updates);
        busy_us += esp_timer_get_time() - work_t0;

        if (++frames == 600) {
            int64_t span = esp_timer_get_time() - stat_t0;
            ESP_LOGI("gos_loop", "fps %.1f, avg frame %.1f ms",
                     600.0 / (span / 1e6), busy_us / 600.0 / 1000.0);
            frames = 0;
            busy_us = 0;
            stat_t0 = esp_timer_get_time();
        }

        next += FRAME_US * n_updates;
        now = esp_timer_get_time();
        if (now < next) {
            int64_t us = next - now;
            if (us > 2000) vTaskDelay((TickType_t)((us - 1000) / 1000));
            while (esp_timer_get_time() < next) { }
        }
    }
}

void gos_loop_start(void)
{
    xTaskCreatePinnedToCore(game_task, "gos_game", 12288, NULL, 5, NULL, 1);
}
