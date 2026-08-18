// hal_display.c — CO5300 AMOLED over QSPI via the Waveshare BSP's raw panel
// path (bsp_display_new; no LVGL anywhere near this).
//
// The game renders 184x224 indexed-8 in internal SRAM. A dedicated flush task
// (core 1, prio 6) upscales 2x through the palette LUT into two ping-pong DMA
// band buffers (368x32 RGB565 each) and pushes them with async draw_bitmap
// calls, so the DMA of band N overlaps the scaling of band N+1.
//
// The panel wants 2px-aligned windows (BSP rounder does the same for LVGL);
// full-width bands at even y satisfy that by construction.

#include "gos_hal.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

#define SRC_W 184
#define SRC_H 224
#define OUT_W 368
#define BAND_SRC_ROWS 16
#define BAND_OUT_ROWS (BAND_SRC_ROWS * 2)
#define NUM_BANDS (SRC_H / BAND_SRC_ROWS)

static const char *TAG = "hal_disp";

static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static uint16_t *band_buf[2];            // DMA-capable internal RAM
static SemaphoreHandle_t dma_slots;      // counting: free band buffers
static SemaphoreHandle_t flush_done;     // binary: flush task -> presenters
static TaskHandle_t flush_task_handle;

static struct {
    const uint8_t *fb;
    const uint16_t *lut, *lut_dim;
    bool scanlines;
    const uint16_t *fb565;   // non-NULL: full-res direct mode, no LUT/upscale
} req;

// Brightness is a panel *command*, and the SPI panel IO is not thread-safe:
// a tx_param racing the flush task's queued color DMA corrupts the IO's
// in-flight state and wedges the whole pipeline (field-reported as "QUIT
// does nothing" — quit applied settings, settings set brightness). So other
// tasks only post a request here; the flush task applies it at its one safe
// point, when a frame's DMA has fully drained.
static volatile int pending_brightness = -1;

static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *user)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(dma_slots, &hp);
    return hp == pdTRUE;
}

static void scale_row(uint16_t *dst, const uint8_t *src, const uint16_t *lut)
{
    for (int x = 0; x < SRC_W; x++) {
        uint16_t c = lut[src[x]];
        *dst++ = c;
        *dst++ = c;
    }
}

static void flush_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint8_t *fb = req.fb;
        const uint16_t *lut = req.lut, *lut_dim = req.lut_dim;
        const uint16_t *fb565 = req.fb565;
        bool scan = req.scanlines;

        if (fb565) {
            // full-res direct mode: byteswap-copy 368x448 RGB565 (PSRAM)
            // into the same ping-pong bands — cheaper than the LUT upscale
            for (int b = 0; b < NUM_BANDS; b++) {
                xSemaphoreTake(dma_slots, portMAX_DELAY);
                uint16_t *buf = band_buf[b & 1];
                int oy0 = b * BAND_OUT_ROWS;
                const uint16_t *src = fb565 + oy0 * OUT_W;
                int n = BAND_OUT_ROWS * OUT_W;
                for (int i = 0; i < n; i++) {
                    uint16_t c = src[i];
                    buf[i] = (uint16_t)((c << 8) | (c >> 8));
                }
                esp_lcd_panel_draw_bitmap(panel, 0, oy0, OUT_W, oy0 + BAND_OUT_ROWS, buf);
            }
        } else {
            for (int b = 0; b < NUM_BANDS; b++) {
                xSemaphoreTake(dma_slots, portMAX_DELAY);
                uint16_t *buf = band_buf[b & 1];
                int sy0 = b * BAND_SRC_ROWS;
                for (int r = 0; r < BAND_SRC_ROWS; r++) {
                    const uint8_t *src = fb + (sy0 + r) * SRC_W;
                    int oy = (sy0 + r) * 2;
                    uint16_t *d0 = buf + r * 2 * OUT_W;
                    uint16_t *d1 = d0 + OUT_W;
                    const uint16_t *l0 = (scan && (oy % 3) == 2) ? lut_dim : lut;
                    const uint16_t *l1 = (scan && ((oy + 1) % 3) == 2) ? lut_dim : lut;
                    scale_row(d0, src, l0);
                    if (l1 == l0) memcpy(d1, d0, OUT_W * 2);
                    else scale_row(d1, src, l1);
                }
                esp_lcd_panel_draw_bitmap(panel, 0, sy0 * 2, OUT_W, sy0 * 2 + BAND_OUT_ROWS, buf);
            }
        }
        // All bands queued; when both slots are free again the panel has the
        // final band in flight or done — one more take/give pair drains it.
        xSemaphoreTake(dma_slots, portMAX_DELAY);
        xSemaphoreTake(dma_slots, portMAX_DELAY);
        int bp = pending_brightness;
        if (bp >= 0) {
            pending_brightness = -1;
            uint8_t level = (uint8_t)(bp * 255 / 100);
            // QSPI panel: commands must be wrapped exactly like the co5300
            // driver's tx_param does — (0x02 << 24) | (cmd << 8). A raw
            // 0x51 goes out as a malformed opcode the panel silently
            // ignores (field-reported: "brightness doesn't do anything").
            int cmd = (0x02 << 24) | (0x51 << 8);
            esp_lcd_panel_io_tx_param(panel_io, cmd, &level, 1);
        }
        xSemaphoreGive(dma_slots);
        xSemaphoreGive(dma_slots);
        xSemaphoreGive(flush_done);
    }
}

// The BSP's bsp_display_new() pins the QSPI clock at the co5300 component's
// 40 MHz default, which caps a full 368x448x16bpp frame at ~54 fps. We need
// 60, so panel bring-up lives here: the BSP's exact init-command sequence,
// but with the bus at 80 MHz.
static const co5300_lcd_init_cmd_t init_cmds[] = {
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x51, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x2A, (uint8_t[]){ 0x00, 0x00, 0x01, 0x6F }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xBF }, 4, 0 },
    { 0x11, (uint8_t[]){ 0x00 }, 0, 100 },
    { 0x29, (uint8_t[]){ 0x00 }, 0, 0 },
};

bool hal_display_init(void)
{
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2,
        BSP_LCD_DATA3, OUT_W * BAND_OUT_ROWS * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.pclk_hz = 80 * 1000 * 1000;

    co5300_vendor_config_t vendor_config = {
        .init_cmds = init_cmds,
        .init_cmds_size = sizeof(init_cmds) / sizeof(init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &panel_io));
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    if (esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel) != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed");
        return false;
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    // CST816S-touch board revision carries its panel RAM at a 16 px x offset
    // (same probe the BSP uses to pick the gap)
    if (i2c_master_probe(bsp_i2c_get_handle(), 0x15, 50) == ESP_OK)
        esp_lcd_panel_set_gap(panel, 0x10, 0);
    esp_lcd_panel_disp_on_off(panel, true);
    for (int i = 0; i < 2; i++) {
        band_buf[i] = heap_caps_malloc(OUT_W * BAND_OUT_ROWS * 2,
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!band_buf[i]) return false;
    }
    dma_slots = xSemaphoreCreateCounting(2, 2);
    flush_done = xSemaphoreCreateBinary();
    xSemaphoreGive(flush_done);

    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_done };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, NULL);

    xTaskCreatePinnedToCore(flush_task, "gos_flush", 4096, NULL, 6,
                            &flush_task_handle, 1);
    return true;
}

void hal_display_present(const uint8_t *fb, const uint16_t *lut,
                         const uint16_t *lut_dim, bool scanlines)
{
    // previous frame fully out; a flush takes ~9 ms, so a second of silence
    // means the pipeline wedged — say so instead of freezing mutely
    while (xSemaphoreTake(flush_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "flush pipeline stalled");
    req.fb = fb;
    req.lut = lut;
    req.lut_dim = lut_dim;
    req.scanlines = scanlines;
    req.fb565 = NULL;
    xTaskNotifyGive(flush_task_handle);
}

// full-res RGB565 present (normal byte order; the flush swaps for the panel)
void hal_display_present_565(const uint16_t *fb565)
{
    while (xSemaphoreTake(flush_done, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "flush pipeline stalled");
    req.fb565 = fb565;
    xTaskNotifyGive(flush_task_handle);
}

void hal_display_wait(void)
{
    xSemaphoreTake(flush_done, portMAX_DELAY);
    xSemaphoreGive(flush_done);
}

void hal_display_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    pending_brightness = pct;   // applied by the flush task between frames
}
