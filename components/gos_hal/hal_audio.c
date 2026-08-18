// hal_audio.c — I2S + ES8311, following infinite-golf's proven bring-up.
//
// Deliberately bypasses bsp_audio_codec_speaker_init(): the vendor's own
// example README warns it boot-loops in i2c_ctrl_if. This is the working
// recipe from their 12_i2s_codec example: hand-rolled I2S on the BSP pins,
// output-only ES8311 esp_codec_dev, PA on GPIO46, MCLK on.
//
// The audio task (core 0, prio 7) pulls 256-sample chunks from gos_core's
// mixer; esp_codec_dev_write blocking on I2S DMA provides the pacing. When
// the mixer reports idle it drops to a 20 ms poll.

#include "gos_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

#define RATE 22050
#define CHUNK 256

static const char *TAG = "hal_audio";
static esp_codec_dev_handle_t speaker;
static bool ready;
static int base_vol = 70;   // V1 boards; V2 (CST816 at 0x15) uses 90

static void audio_task(void *arg)
{
    static int16_t chunk[CHUNK];   // internal RAM: feeds I2S DMA
    for (;;) {
        if (!gos_mixer_render(chunk, CHUNK)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int ret = esp_codec_dev_write(speaker, chunk, sizeof(chunk));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec write failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

bool hal_audio_init(void)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx, &rx) != ESP_OK) return false;

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    if (i2s_channel_init_std_mode(tx, &i2s_cfg) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(rx, &i2s_cfg) != ESP_OK) return false;

    audio_codec_i2s_cfg_t data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .tx_handle = tx,
        .rx_handle = rx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);
    if (!data_if) return false;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return false;

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,   // output only, no mic path
        .pa_pin = BSP_POWER_AMP_IO,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *es_if = es8311_codec_new(&es_cfg);
    if (!es_if) return false;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es_if,
        .data_if = data_if,
    };
    speaker = esp_codec_dev_new(&dev_cfg);
    if (!speaker) return false;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = RATE,
        .channel = 1,
        .bits_per_sample = 16,
        .mclk_multiple = 256,
    };
    // esp_codec_dev disables channels that were never enabled during open;
    // harmless, but it logs an error — silence i2s_common for the call.
    esp_log_level_t lvl = esp_log_level_get("i2s_common");
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    int open_ret = esp_codec_dev_open(speaker, &fs);
    esp_log_level_set("i2s_common", lvl);
    if (open_ret != ESP_CODEC_DEV_OK) return false;

    if (i2c_master_probe(bsp_i2c_get_handle(), 0x15, 50) == ESP_OK)
        base_vol = 90;
    esp_codec_dev_set_out_vol(speaker, base_vol);

    xTaskCreatePinnedToCore(audio_task, "gos_audio", 4096, NULL, 7, NULL, 0);
    ready = true;
    ESP_LOGI(TAG, "ES8311 up, base volume %d", base_vol);
    return true;
}

void hal_audio_volume(int pct)
{
    if (!ready) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    esp_codec_dev_set_out_vol(speaker, base_vol - 20 + pct * 20 / 100);
}

void hal_audio_mute(bool on)
{
    if (ready) esp_codec_dev_set_out_mute(speaker, on);
}
