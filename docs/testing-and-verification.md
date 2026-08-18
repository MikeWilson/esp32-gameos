# Testing & Verification

The board is often unplugged, mid-flash, or in the user's hands. The platform
is built so that **rendering, pacing, and input flows are all verifiable on
the host Mac** — and the expectation is that you do so before claiming
anything works. "It compiles" is not verification; neither is one PPM frame
that looks plausible.

## Host simulator pattern

`gos_core/gfx.c`, `font.c`, and any game source compile on the host with
`-DGOS_HOST_SIM` (gfx.c has a sim palette hook). A sim harness (`simN.c`)
stubs the OS services, scripts input frame-by-frame, and dumps 2x-upscaled
PPM frames:

```sh
cc -O1 -DGOS_HOST_SIM -I components/gos_core/include \
   simN.c components/gos_core/gfx.c components/gos_core/font.c \
   components/games/<game>/<game>.c -lm
```

Working examples live in session scratchpads (gunship: `sim.c` frame dumps,
`sim2.c` warm-pixel first-contact + calib-count instrumentation, `sim3.c`
edge-marker probes; shell: `simui.c` + `fakeinc/` stub ESP headers compiling
the **real** `shell.c`, scripted taps walking grid→settings→launch→overlay→
calib).

### What good sims measure

Don't just eyeball frames — instrument the claim:

- Pacing: "first zombie visible N seconds after start" (was 9.8 s, fixed to
  1.0 s — a number, not an impression).
- Flows: scripted tap sequences through every menu path, asserting screen
  state via pixel probes.
- Soak: bot-driven full sessions (golf's soak bot plays 18 holes solo AND
  2-player). Passive sims stall on terminal screens (LOSE after 6 deaths) —
  script periodic inputs to restart.
- Distributions: loop a generator over hundreds of seeds and histogram the
  output (golf's par distribution, cart-path audit over 720 holes).

### Sim gotchas

- Tap detection: hold the last touch coords across the release edge, or the
  sim's release reads as a giant slide and eats the tap.
- Big state buffers: allocate generously — a 2 MB soak buffer segfaulted
  silently where 8 MB worked.
- FB565/LVGL-font games need the extra shims (`lv_shim.h` struct mirror with
  designated initializers, copied font `.c` files, `-DLV_LVGL_H_INCLUDE_SIMPLE`).
- zsh doesn't word-split `$FLAGS` — write compile flags inline.

## Building & flashing firmware

```sh
cd ~/esp32/gameos          # ALWAYS — shell cwd resets between commands;
                           # idf.py silently builds the wrong dir otherwise
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem101 build flash
```

Port re-enumerates between `usbmodem101` and `usbmodem1101`; detect with `ls
/dev/cu.usbmodem*`.

## Serial logging without resetting the board

Opening the port naively pulses DTR and **reboots the board** (esptool can
also be blocked by a held port). The no-reset recipe on this Mac:

```python
import serial
s = serial.Serial()          # construct UNOPENED
s.port = "/dev/cu.usbmodem101"; s.baudrate = 115200
s.dtr = False; s.rts = False # set BEFORE open
s.open()                     # passing dsrdtr/rtscts to the ctor still pulses DTR
```

Run it as a passive logger into a file. Touch down/up and all shell actions
are permanently `ESP_LOGI`'d, so a user report like "QUIT does nothing"
becomes a per-press coordinate trace — this is how the touch-parallax numbers
were measured. Keep that logging in place; add the same level of logging to
new games' input handling.

## On-device diagnostics

- `DIAG` app: live IMU/touch/battery/heap/grid/audio.
- `AIM TEST` app: the 30-trial median time-to-acquire protocol with live
  tuning sliders — needs a human, pass is median < 1.2 s.
- BOOT 600 ms hold: debug overlay in any game.
- `gos_loop: fps … avg frame … ms` logs every 600 frames — check after any
  render change.

## The verification ladder for a change

1. Host sim proves the logic/rendering (instrumented, not eyeballed).
2. Firmware builds clean from `~/esp32/gameos`.
3. Flash when the board is available; watch the boot log and fps line.
4. Anything feel-related (tilt, swing, pacing, touch) ultimately needs the
   human — state clearly what still needs on-device confirmation.
