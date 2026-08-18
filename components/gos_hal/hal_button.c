// hal_button.c — the BOOT button (GPIO0) is the one physical input.
#include "gos_hal.h"
#include "driver/gpio.h"

bool hal_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}

bool hal_button_down(void) { return gpio_get_level(GPIO_NUM_0) == 0; }
