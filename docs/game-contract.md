# The Game Contract (`gos.h`)

A game is a component under `components/games/<name>/` that includes **only**
`gos.h`. No HAL headers, no drivers, no LVGL, no FreeRTOS primitives in the
game unless you truly need a task (prefer not to — the OS loop is enough for
almost everything).

Layering, dependencies point down only:

```
games  →  gos.h contract  →  gos_shell  →  gos_core  →  gos_hal
```

## Registering a game (two edits, easy to forget the second)

1. Add one line to `gos_shell/registry.c` referencing your `gos_game_t`.
2. Add your component to `gos_shell/CMakeLists.txt` **PRIV_REQUIRES**.

Forgetting step 2 gives a link error only after a clean build — it has bitten
before.

## The `gos_game_t` struct

```c
typedef struct gos_game_s {
    const char *id;              // stable — used as the NVS save key
    const char *title;
    const gos_sprite_t *icon;    // 48x48 or NULL (shell draws a monogram)
    uint32_t caps;               // GOS_CAP_* flags
    size_t   state_size;         // OS allocates + zeroes this for you

    void (*init)    (gos_ctx_t *ctx);
    void (*update)  (gos_ctx_t *ctx, float dt, const gos_input_t *in);
    void (*render)  (gos_ctx_t *ctx);
    void (*suspend) (gos_ctx_t *ctx);   // pause overlay / low battery
    void (*resume)  (gos_ctx_t *ctx);
    void (*teardown)(gos_ctx_t *ctx);
} gos_game_t;
```

- `ctx->state` is your `state_size` bytes, zeroed at launch. Put **all** game
  state in it — no file-scope mutable globals (the shell relaunches games
  without rebooting). Large states are fine; they fall back to PSRAM.
- `ctx->rng` is a per-game xorshift32 stream — use `gos_rand*` for gameplay
  randomness so runs are replay-reproducible. Seed variation comes from the OS.
- `update` runs at a fixed dt of 1/60 s on core 1 (prio 5); the loop catches
  up to 4 missed updates before dropping a render. Do not assume update:render
  is 1:1.
- `render` is immediate-mode full redraw (indexed pipeline) — see rendering.md.
- `suspend`/`resume` bracket the pause overlay. Silence looping audio in
  suspend; restart it in resume.

## Capability flags

| Flag | Meaning |
|---|---|
| `GOS_CAP_IMU` | you use aim modes or the raw accel ring |
| `GOS_CAP_AUDIO` | you play sound |
| `GOS_CAP_SAVE` | you use the save blob |
| `GOS_CAP_TOUCH_FIRE` | `GOS_BTN_FIRE` = any touch in the **lower 2/3** of the screen. **Trap:** touches in the top 1/3 never set FIRE — any in-game UI up there needs its own touch-edge detection (this silently broke the gunship upgrade screen once). |
| `GOS_CAP_FB565` | full-res 368x448 RGB565 direct mode instead of the indexed pipeline (see rendering.md) |

## Save data

- One blob per game, keyed by `id`, **max 4 KB**, versioned by the game itself
  (put a version byte first; check it on read).
- `gos_save_write(ctx, blob, len)` / `gos_save_read(ctx, out, max)` (returns
  bytes read, -1 if none).
- Write at natural checkpoints (end of wave, hole-out, settings change) — not
  every frame.

## Exiting

`gos_request_exit()` asks the OS to quit to the launcher after the current
update; suspend/teardown run normally. Use it for explicit EXIT buttons. The
OS-level escapes (swipe down from the top edge) always work; BOOT belongs to games as `GOS_BTN_A`
regardless — never disable or fight them.

## Input snapshot

`gos_input_get()` returns a normalized snapshot — games never learn whether
aim came from tilt, gyro, or touch:

- `aim_x/aim_y` ∈ [-1, 1], `(0,0)` = screen center. Respect the user's chosen
  aim mode; don't force one.
- `buttons` (held) / `pressed` / `released` (edges). **Always act on edges for
  UI**, on held only for continuous actions (firing, thrust).
- `touch` (render-space 184x224) + `touching`.
- `shake` — a 0..1 impulse that decays; fine for "shake to X" flavor.
- `pitch/roll/gx/gy/gz` are **diagnostics only** — do not build gameplay on
  them; use the aim abstraction or the accel ring.

### Gesture detection (swings, flicks)

`gos_imu_accel_read(out, max)` drains a wall-clock-stamped ~200 Hz raw accel
ring (single consumer). Games with custom gesture detectors (golf's swing) run
their own state machine over **every** drained sample each update. The OS
deliberately contains no game-specific gesture logic — keep it that way; if
two games need the same detector, promote a *generic* primitive.

## Services cheat sheet

- **Timing:** `gos_time_ms()` / `gos_time_us()`. **All user-facing durations
  must be wall-clock**, never frame-counted (see design-playbook.md).
- **Audio:** `gos_audio_play/loop/stop/set_vol/stop_all`, master 0–3
  (OS-owned setting — games don't ship their own volume UI; see audio.md).
- **Spatial hash:** `gos_grid_init/clear/insert/query` — one shared instance,
  rebuild each frame. Used by the gunship for hundreds of entities.
- **Trig:** `gos_sin256/gos_cos256` (Q14, angle256 domain). Never call libm
  trig per entity per frame.
