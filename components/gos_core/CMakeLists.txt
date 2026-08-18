idf_component_register(
    SRCS "gfx.c" "font.c" "font565.c" "input.c" "mixer.c" "core.c" "loop.c"
    INCLUDE_DIRS "include"
    REQUIRES gos_hal
    PRIV_REQUIRES nvs_flash esp_timer lvgl__lvgl
)
