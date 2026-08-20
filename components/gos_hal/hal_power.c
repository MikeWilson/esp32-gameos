// hal_power.c — minimal AXP2101 PMIC reader (battery percent + charge state).
// If the PMIC doesn't answer (USB-only board variant), everything stubs to
// "no battery" and the shell shows nothing — per spec §2 Task 0.
#include "gos_hal.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

#define AXP_ADDR 0x34
#define REG_STATUS1 0x00    // bit 3: battery present
#define REG_STATUS2 0x01    // bits 6:5 charge status: 01 = charging
#define REG_PWROFF 0x22     // power off control
#define REG_PWRKEY 0x27     // power key press timing
#define REG_IRQ0 0x48       // 0x48..0x4A: latched IRQ status, write 1s to clear
#define REG_SOC 0xA4        // battery percentage

#define PWROFF_LONGPRESS (1u << 1)   // long press powers the rail down
#define PWROFF_RESTART   (1u << 0)   // ...and then brings it back up (unwanted)
#define PWRKEY_TIMING    0x0F

static i2c_master_dev_handle_t axp;
static hal_batt_t batt = { .percent = -1 };

static bool axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(axp, &reg, 1, val, 1, 50) == ESP_OK;
}

static bool axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(axp, buf, 2, 50) == ESP_OK;
}

// Field incident: after a battery was connected for the first time, the board
// would not power on from that battery until a 10 s PWR hold forced the PMIC
// off and a normal press woke it again. Afterwards it behaved. The AXP2101
// comes up from a first cell with stale power-key timing and latched IRQs, and
// nothing here was clearing them. Normalizing on every boot (USB boots
// included, since that is when a fresh pack gets installed) is what makes the
// next battery install heal itself instead of needing that ritual taught.
// Same sequence tinydraw ships (its power_manager configure_power_button).
//
// Housekeeping, so failures are non-fatal: a PMIC that answers the probe but
// refuses these writes still reports its battery perfectly well.
static void axp_normalize(void)
{
    uint8_t pwroff, timing;
    bool ok = axp_read(REG_PWROFF, &pwroff) && axp_read(REG_PWRKEY, &timing);
    if (ok) {
        pwroff = (uint8_t)((pwroff | PWROFF_LONGPRESS) & ~PWROFF_RESTART);
        timing = (uint8_t)(timing & ~PWRKEY_TIMING);
        ok = axp_write(REG_PWROFF, pwroff) && axp_write(REG_PWRKEY, timing);
    }
    for (uint8_t r = REG_IRQ0; r <= REG_IRQ0 + 2; r++)
        ok = axp_write(r, 0xFF) && ok;   // drop whatever it latched while dark
    if (!ok) ESP_LOGW("hal_power", "AXP2101 power-key normalization incomplete");
}

bool hal_power_init(void)
{
    if (i2c_master_probe(bsp_i2c_get_handle(), AXP_ADDR, 50) != ESP_OK) {
        ESP_LOGW("hal_power", "AXP2101 not found — running as USB-powered");
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &axp) != ESP_OK)
        return false;
    axp_normalize();
    batt.present = true;
    hal_power_poll();
    return true;
}

void hal_power_poll(void)
{
    if (!batt.present) return;
    // The PMIC being on the bus does not mean a battery is attached: this
    // board ships battery-optional, and with no pack the SOC register reads 0
    // (which would look like "critical"). Trust the battery-present bit, and
    // treat 0% as "no reading" — a truly-0% board would have browned out.
    uint8_t st1;
    if (!axp_read(REG_STATUS1, &st1) || !(st1 & 0x08)) {
        batt.percent = -1;
        batt.charging = false;
        return;
    }
    uint8_t soc, st;
    if (axp_read(REG_SOC, &soc) && soc > 0 && soc <= 100) batt.percent = soc;
    else batt.percent = -1;
    if (axp_read(REG_STATUS2, &st)) batt.charging = ((st >> 5) & 3) == 1;
}

void hal_power_get(hal_batt_t *out) { *out = batt; }
