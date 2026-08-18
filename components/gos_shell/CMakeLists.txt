idf_component_register(
    SRCS "shell.c" "registry.c" "apps.c"
    REQUIRES gos_core
    PRIV_REQUIRES gos_hal gunship golf slots bomber nvs_flash esp_timer
)
