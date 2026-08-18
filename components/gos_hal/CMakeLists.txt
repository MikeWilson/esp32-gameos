idf_component_register(
    SRCS "hal_display.c" "hal_touch.c" "hal_imu.c" "hal_audio.c"
         "hal_power.c" "hal_button.c"
    INCLUDE_DIRS "include"
    REQUIRES esp32_s3_touch_amoled_1_8 qmi8658
    PRIV_REQUIRES esp_lcd esp_lcd_touch esp_driver_gpio esp_driver_i2s esp_codec_dev esp_timer
)
