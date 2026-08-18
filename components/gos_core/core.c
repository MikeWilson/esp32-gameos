// core.c — save, settings, RNG, spatial grid, timing.
#include "gos.h"
#include "gos_core.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "gos_hal.h"

// --- timing ----------------------------------------------------------------

uint32_t gos_time_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
int64_t gos_time_us(void) { return esp_timer_get_time(); }

// --- RNG -------------------------------------------------------------------

uint32_t gos_rand(gos_rng_t *r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

int gos_rand_range(gos_rng_t *r, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(gos_rand(r) % (uint32_t)(hi - lo + 1));
}

float gos_randf(gos_rng_t *r) { return (gos_rand(r) >> 8) / 16777216.0f; }

// --- exit request ----------------------------------------------------------

static volatile bool exit_req;
void gos_request_exit(void) { exit_req = true; }
bool gos_core_take_exit(void)
{
    bool e = exit_req;
    exit_req = false;
    return e;
}

// --- save ------------------------------------------------------------------

void gos_save_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

static void save_ns(const gos_game_t *g, char *out, size_t n)
{
    snprintf(out, n, "save_%.9s", g->id);
}

bool gos_save_write(gos_ctx_t *ctx, const void *blob, size_t len)
{
    if (len > 4096) return false;
    char ns[16];
    save_ns(ctx->game, ns, sizeof ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "blob", blob, len) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

int gos_save_read(gos_ctx_t *ctx, void *out, size_t max)
{
    char ns[16];
    save_ns(ctx->game, ns, sizeof ns);
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t len = max;
    int ret = nvs_get_blob(h, "blob", out, &len) == ESP_OK ? (int)len : -1;
    nvs_close(h);
    return ret;
}

// --- settings (sys namespace) ----------------------------------------------

gos_settings_t g_settings = {
    .version = 1, .aim_mode = GOS_AIM_TILT_ABS, .sensitivity = 1,
    .volume = 2, .brightness = 4,
};

void gos_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READONLY, &h) != ESP_OK) return;
    gos_settings_t s;
    size_t len = sizeof s;
    if (nvs_get_blob(h, "settings", &s, &len) == ESP_OK &&
        len == sizeof s && s.version == 1)
        g_settings = s;
    nvs_close(h);
}

void gos_settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "settings", &g_settings, sizeof g_settings);
    nvs_commit(h);
    nvs_close(h);
}

void gos_settings_apply(void)
{
    gos_input_set_aim_mode((gos_aim_mode_t)g_settings.aim_mode);
    gos_aim_cfg_t *c = gos_input_aim_config();
    static const float ranges[3] = { 35.f, 25.f, 15.f };   // low/med/high sens
    c->range_deg = ranges[g_settings.sensitivity % 3];
    c->invert_x = g_settings.invert_x;
    c->invert_y = g_settings.invert_y;
    gos_audio_master(g_settings.volume);
    hal_display_brightness(g_settings.brightness * 25);
}

// --- spatial hash grid ------------------------------------------------------

#define GRID_MAX_CELLS (80 * 80)
#define GRID_MAX_NODES 512

static struct {
    int cell, cols, rows;
    int16_t head[GRID_MAX_CELLS];
    struct { int16_t next; uint16_t id; int16_t x, y; } node[GRID_MAX_NODES];
    int nnodes;
} grid;

void gos_grid_init(int cell_size, int world_w, int world_h)
{
    grid.cell = cell_size > 0 ? cell_size : 16;
    grid.cols = (world_w + grid.cell - 1) / grid.cell;
    grid.rows = (world_h + grid.cell - 1) / grid.cell;
    if (grid.cols > 80) grid.cols = 80;
    if (grid.rows > 80) grid.rows = 80;
    gos_grid_clear();
}

void gos_grid_clear(void)
{
    memset(grid.head, 0xFF, sizeof(int16_t) * grid.cols * grid.rows);
    grid.nnodes = 0;
}

void gos_grid_insert(uint16_t id, int16_t x, int16_t y)
{
    if (grid.nnodes >= GRID_MAX_NODES) return;
    int cx = x / grid.cell, cy = y / grid.cell;
    if (cx < 0 || cy < 0 || cx >= grid.cols || cy >= grid.rows) return;
    int c = cy * grid.cols + cx;
    int n = grid.nnodes++;
    grid.node[n].next = grid.head[c];
    grid.node[n].id = id;
    grid.node[n].x = x;
    grid.node[n].y = y;
    grid.head[c] = (int16_t)n;
}

int gos_grid_query(int16_t x, int16_t y, int16_t r, uint16_t *out, int max)
{
    int n = 0;
    int cx0 = (x - r) / grid.cell, cy0 = (y - r) / grid.cell;
    int cx1 = (x + r) / grid.cell, cy1 = (y + r) / grid.cell;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= grid.cols) cx1 = grid.cols - 1;
    if (cy1 >= grid.rows) cy1 = grid.rows - 1;
    int r2 = r * r;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++)
            for (int16_t i = grid.head[cy * grid.cols + cx]; i >= 0;
                 i = grid.node[i].next) {
                int dx = grid.node[i].x - x, dy = grid.node[i].y - y;
                if (dx * dx + dy * dy > r2) continue;
                if (n >= max) return n;
                out[n++] = grid.node[i].id;
            }
    return n;
}
