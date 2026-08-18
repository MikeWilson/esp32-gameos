# Input & UX

This is the most bug-dense area of the platform. Every rule below traces to a
user-reported failure ("menus do nothing", "it launches gunship by default",
"QUIT does nothing"). Follow them exactly.

## Touch parallax — the #1 trap

**Measured on this panel: finger contact registers 15–25 px below the aimed
target** (render space). Serial traces showed seven consecutive QUIT presses
landing at y=170–182 against a zone ending at y=167. Independently, golf's
title screen needed a 30 px "low thumbs" allowance.

Compensation rules (the shell already does all of this — copy it):

1. Bias the touch point up by `TOUCH_Y_BIAS = 12` before hit-testing
   (`shell.c:78`).
2. **Extend every touch zone ~20 px below its visual.**
3. **Never ship a touch row shorter than ~40 px** (render space). A ≤26 px row
   is unusable on this device. The shell's overlay buttons are 40 px apart
   with 44-tall zones.
4. Overlapping zones: test top-down, first match wins, `return` after a hit.

## Tap semantics

- **UI actions fire on edges** (`pressed`/`released`), never on held state.
- Menus: fire on press-edge OR release-tap, with a `press_consumed` guard so
  one contact = one action.
- **Launching/destructive actions: release-only.** A press-edge launcher fired
  on the very first boot contact ("it launches gunship by default").
- Swipes re-arm only after a full no-touch frame.
- The touch panel flickers during holds: hold-to-X timers must tolerate brief
  release gaps (golf accumulates across flickers with a release-age
  continuation, and allows <16 px of jitter from the press origin).
- A missed release edge makes the next press read as a held continuation —
  design state machines to recover from that.

## Screen safe zone

The rounded panel corners eat ~6 render px (12 physical) per edge.
**Keep all UI inside `SAFE = 6`** (`shell.c:151`): HUD chips at x≥6, bars
ending ≤178, bottom text at H−14. In FB565 mode the margin is 12 panel px.

## Escape routes — non-negotiable

**Never ship a screen without a way out.** The OS guarantees:

- Swipe down from the top edge (press y<14, drag >40) → pause overlay, in any
  game.
- BOOT is a GAME button: delivered to the running game as `GOS_BTN_A` press/release edges. A 600 ms hold toggles the debug overlay (dev-only; the game sees the press-edge but never the release). On the pause overlay a short press resumes. Pause = swipe down from the top edge, only.

Games must additionally:

- Draw an explicit EXIT affordance on terminal screens (briefing, win, lose).
  Gunship uses a "< EXIT" chip (zone y<26, x<56) plus a "SWIPE DOWN FROM TOP
  = MENU" hint on the brief.
- Wire EXIT to `gos_request_exit()` — never try to draw the launcher yourself.

## Aim modes (tilt games)

Three modes, user-selectable in OS settings — games read the abstraction and
must work in all three:

- `GOS_AIM_TILT_ABS` (default): 25° tilt range, pow-1.4 response curve,
  deadzone 0.04, one-euro filtered. Tuned via the AIM TEST harness
  (pass bar: 30-trial median time-to-acquire < 1.2 s).
- `GOS_AIM_GYRO_RATE`: rate control, gain 0.04.
- `GOS_AIM_TOUCH_DRAG`: for users who don't want tilt.

Tips:

- Provide an in-game **recenter** affordance for tilt play (gunship: tap the
  CAL zone in the top strip, x 100..140, flashes white 0.5 s as feedback).
  There is no two-finger gesture — touch is single-point.
- Recenter must be **tap-edge only**. A per-frame recenter captured the user's
  mid-reach hand pose whenever they reached for another control.
- Leave a dead gap (~10 px) between adjacent top-strip controls so a reach
  for one can't trigger its neighbor.
- Yaw does not exist. Design aiming around pitch/roll only.

## `GOS_CAP_TOUCH_FIRE` layout implications

With this cap, any touch in the **lower 2/3** sets `GOS_BTN_FIRE`. So:

- The top 1/3 is your UI strip (chips, EXIT, CAL) — but those touches never
  set FIRE, so top-strip UI needs its own touch-edge handling.
- Don't put tappable UI in the lower 2/3 of an action game; a tap there fires
  the weapon. If you must (upgrade rows), bias zones and expect FIRE noise.

## Settings the OS owns (don't duplicate them)

Volume, brightness, aim mode/sensitivity/invert, and calibration all live in
the OS settings screen. A game options page should link the concept
("sound is in OS settings") rather than grow its own row — golf ships exactly
that note. Grouped tilt settings are labeled "TILT GAMES" in the shell so
they don't read as one game's leftovers.

## Visual language of the shell (for coherence, not conformity)

The launcher is a Wii-menu look: light striped field, white rounded cards,
bottom bar with a round SET button, battery on the right. Games are free to
have their own art direction, but system-ish screens inside a game (pause
text, confirmations) read best in the same calm register: generous spacing,
40 px rows, no drop shadows over dim backgrounds (they read as black borders).

## Text legibility

- Indexed mode fonts are 5x7 and 10x14 — ALL-CAPS short labels only.
- Anything sentence-length needs FB565 + Montserrat (`gos_gfx_text565`).
- Type ladder that worked in golf: headline 28 / primary 22 / meta 18 /
  hint 14.
