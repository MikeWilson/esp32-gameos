# Rendering

Two modes exist. Pick one per game via caps; the OS switches pipelines at
launch and always restores the shell state on exit.

## Mode 1 (default): 184x224 @ 8bpp indexed

`gos_color_t` is a **palette index**, not a color. The framebuffer is 184x224
bytes, double-buffered in internal SRAM. A flush task (core 1, prio 6) applies
a 256-entry RGB565 LUT while 2x-nearest-upscaling to the 368x448 panel, in
16-source-row bands over ping-pong DMA buffers. Measured 60.0 fps with ~1.5 ms
of frame CPU left for the shell — you have a real budget, spend it on gameplay.

Why indexed: it halves fill bandwidth, and palette tricks are a feature —
thermal ramps, whole-screen tints, and dimming are single LUT writes.

### Palette rules (violating these corrupts the shell overlay)

The pause overlay and calibration screen draw **over your live palette** using
only `GOS_GRAY64(0..63)` = indices **16..79**. Therefore:

- **Indices 16..79 are reserved.** Leave the gray ramp intact.
- Indices 0..15 are the default named colors (`GOS_BLACK`..`GOS_AMBER`) —
  overwrite freely.
- Indices 80..255 are yours (gunship: white-hot thermal ramp; golf v1:
  material ramps + ball tints at 216+).
- `gos_gfx_set_palette()` takes normal RGB565; the OS handles panel byte order.
  The default palette is restored automatically on exit.

### Primitives worth knowing

- `gos_gfx_circle_soft(cx, cy, r, hot, base)` — radial-falloff blob,
  max-blended so overlapping blobs merge; the thermal/glow workhorse
  (radius capped at 16).
- `gos_gfx_blit_rot` — sprite rotation about center, angle256 (0..255 = full
  turn) to match `gos_sin256/cos256`.
- `gos_gfx_text(font, …)` — font 0 = 5x7, font 1 = 10x14. Bitmap fonts;
  fine for HUDs, too crude for long prose.
- `gos_gfx_scanlines(true)` — dims every 3rd output row in the flush LUT for
  free (a CRT/thermal-monitor look with zero frame cost).
- `gos_gfx_fb()` — raw buffer for full-frame effects (sensor noise); it's
  184*224 bytes row-major, cheap to whole-buffer post-process.
- Sprites: 8bpp indices, `0xFF` = transparent.

## Mode 2: `GOS_CAP_FB565` — full-res 368x448 RGB565 direct

For ports that need the panel's native fidelity (the golf port's users
rejected two half-res attempts before this mode existed).

- `gos_gfx_fb565()` returns a single-buffered 368x448 RGB565 buffer in PSRAM,
  normal byte order. **None of the indexed primitives apply** — you own every
  pixel.
- The flush byteswap-copies bands — actually *cheaper* per frame than the
  indexed LUT-upscale path.
- **Single-buffered means the buffer persists between frames.** This is a
  feature: "draw-once and hold" screens (cards, menus) render exactly like an
  LVGL canvas did — draw once, early-return in render until state changes.
  It also means you must repaint anything that moves; there is no free clear.
- Text: `gos_gfx_text565(px, x, y, rgb565, fmt, …)` renders Montserrat
  (14/18/22/28 px, alpha-blended, class-kerned, UTF-8) straight from LVGL
  font data. Use it — it is the difference between "demo" and "product" for
  text-heavy screens.
- The pause overlay can't composite over your buffer, so it draws on a flat
  backdrop while your game is suspended — that's expected, don't fight it.
- Touch coords are still render-space 184x224: multiply by 2 at update entry.

## Choosing a mode

- New original games: **indexed**. It's faster to iterate, palette effects are
  powerful, and the 2x chunky look is a coherent aesthetic on this panel.
- Ports of games with established full-res art/typography: **FB565**. Halving
  the resolution of a game the user already loves reads as a downgrade
  ("this sucks compared to my golf game") no matter how clever the palette is.

## Performance rules

- Fixed-point or integer math in inner loops; `gos_sin256/cos256` for trig.
- Immediate-mode full redraw is the norm in indexed mode and is affordable at
  60 fps. In FB565, prefer draw-once-and-hold for static screens and sliced
  incremental building for expensive backgrounds (golf rebuilds its world
  texture in per-tick slices behind a card: ROUGH→GRIDS→PAINT→PATH→TREES→DECOR).
- The loop logs `gos_loop: fps … avg frame … ms` every 600 frames — check it
  after any rendering change.
- If you need a per-frame whole-screen effect, do it in the palette (indexed)
  or fused into an existing pass (golf fuses CRT stripes into its blit), never
  as a separate full-buffer pass.
