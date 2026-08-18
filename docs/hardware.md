# Hardware Truths

Everything here was verified on the physical unit (boot log + I2C probes,
2026-08-15). When a datasheet, vendor example, or component default disagrees
with this document, **this document wins** — several vendor defaults are
actively wrong for this board.

## Summary

| Subsystem | Part | Notes |
|---|---|---|
| Display | CO5300 AMOLED, 368x448, QSPI | NOT SH8601 as some vendor docs claim |
| Touch | CST816S @ I2C 0x15 | **Single-point only.** No multi-finger gestures, ever |
| IMU | QMI8658 @ I2C 0x6B | Accel + gyro; axis orientation is non-obvious (below) |
| Audio | ES8311 codec + speaker, PA on GPIO46 | Bring-up trap (below) |
| PMIC | AXP2101 @ I2C 0x34 | Battery header populated, pack often absent (trap below) |
| Button | BOOT (GPIO0) | The only physical button |
| Board rev | V2 (detected: CST816S probes at 0x15) | V2 → panel x-gap 0x10, audio base volume 90 |
| Boot | ~1.0 s to interactive | |

## Display: CO5300 over QSPI

- Physical panel is 368x448. The OS renders games at **184x224 indexed-8** and
  2x-upscales during flush (see rendering.md), or full-res RGB565 for
  `GOS_CAP_FB565` games.
- **80 MHz pclk trap:** the `co5300` managed component defaults to 40 MHz QSPI,
  which caps full-frame refresh at ~54 fps. `hal_display.c` owns panel init
  (the BSP's exact 11-command sequence) at 80 MHz → 60.0 fps measured. Never
  let the component re-take panel init.
- The panel wants **2px-aligned windows** for `draw_bitmap`.
- **Rounded corners eat pixels:** each panel corner covers ~6 render px
  (12 physical px) per edge. All UI must respect the `SAFE = 6` margin
  (defined in `shell.c`). Details in input-and-ux.md.
- **esp_lcd IO is NOT thread-safe** against the flush task's queued color DMA.
  Never call `esp_lcd_panel_io_tx_param` (e.g. brightness cmd 0x51) from the
  game task — the race wedges the DMA pipeline, `present()` blocks forever,
  and the device appears frozen with dead buttons. The HAL already routes
  brightness through a `pending_brightness` handoff applied by the flush task
  between frames. If you add any new panel command, use the same pattern.

## Touch: CST816S

- **Single-point.** Design every interaction for one finger. Any "two-finger"
  spec idea must be redesigned (e.g. the gunship's recenter became a `CAL`
  tap zone).
- **Touch parallax (measured):** finger contact registers **15–25 px below**
  the aimed target in render space. This is the root cause of every
  "menus do nothing" bug this device has produced. All hit-testing rules live
  in input-and-ux.md — read them before writing any touch zone.
- Touch coordinates arrive in render space (184x224) via `gos_input_get()`.
  FB565 games multiply by 2 at update entry to get panel coords.

## IMU: QMI8658

- Read with `qmi8658_read_accel` / `qmi8658_read_gyro` (defaults are mg/dps).
  The component's `_mg`/`_dps` variants are **declared but not implemented** —
  linking against them fails or worse.
- **Axis truth (user-verified on device):** accel **Y runs along the screen's
  horizontal**, X along the vertical. So:
  - pitch (screen-vertical aim) = `atan2f(ax, …)`
  - roll (screen-horizontal aim) = `atan2f(ay, …)`
  - gyro pairs: pitch↔gy, roll↔gx
  The obvious guess is transposed and makes left/right tilt drive up/down.
- The HAL fuses at 200 Hz (complementary filter, **pitch/roll only — yaw is
  forbidden**, it drifts unusably). Gyro axis signs are learned online by
  correlating gyro rate against accel-derived angle deltas, so mounting
  guesses can't invert the fusion.
- Games never read the IMU directly: use the aim abstraction or the raw accel
  ring `gos_imu_accel_read()` (see game-contract.md). Frame-rate polling of
  accel **aliases short motion bursts** — gesture detectors must drain the
  200 Hz ring and process every sample.

## Audio: ES8311

- **Never call `bsp_audio_codec_speaker_init()`.** The vendor's own example
  README warns it boot-loops in `i2c_ctrl_if`. `hal_audio.c` uses the proven
  recipe: hand-rolled I2S1 on BSP pins (MCLK 16, BCLK 9, WS 45, DOUT 8),
  output-only `esp_codec_dev` (`WORK_MODE_DAC`, `TYPE_OUT`,
  `.pa_pin = GPIO46`, `.use_mclk = true`).
- The boot-log error `i2s_channel_disable ... not been enabled` is **benign**.
- V2 board base volume is 90 (V1: 70) — already handled by the HAL.
- Games use only the `gos_audio_*` API (see audio.md).

## Power: AXP2101

- **No-battery trap:** the board has a battery header but frequently runs
  USB-only with no pack. The AXP2101 still answers and reports **0% SOC**,
  which made a naive ≤5% check re-open the low-battery pause overlay every
  poll. The HAL checks the battery-present bit (REG 0x00 bit 3) and treats
  0% without it as "no reading", plus a one-shot warn latch. Don't build
  anything on raw SOC.

## Button: BOOT (GPIO0)

- Short press = pause overlay. 600 ms hold = debug overlay. Both are
  OS-level; games don't see BOOT directly and must not repurpose it.

## Memory

- 8 MB PSRAM. Game state (`state_size` in the contract) is allocated by the
  OS; large states fall back to PSRAM via the shell's 8-bit-capable allocator
  (golf's ~3.6 MB state lives there). Internal SRAM holds the double-buffered
  indexed framebuffers; the FB565 buffer is PSRAM.
- The default main task stack is small — static/heap-allocate large arrays
  (golf's path polylines overflowed a 3584 B stack before being made static).

## Serial / flashing

- Port is `/dev/cu.usbmodem101`, sometimes re-enumerating as `.usbmodem1101`
  after a flash; it disappears entirely when unplugged. Detect with `ls`.
- **Opening the port resets the board unless done carefully** — see
  testing-and-verification.md for the no-reset pyserial recipe.
