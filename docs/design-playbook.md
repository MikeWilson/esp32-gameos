# Design Playbook

Cross-cutting lessons from shipping Zombie Gunship and porting Infinite Golf.
These are the expensive ones — each bullet cost a debugging session or a
user-rejected build.

## Time

- **Time every user-facing duration with the wall clock**
  (`gos_time_us`/`esp_timer`), never frame counts. Frame-counted timers ran
  2–4x slow under display contention in golf, paused during aiming, and
  popped back later. Notices, celebrations, hold-to-confirm, lingering bars —
  all wall-clock.
- **Feel is calibrated to effective tick rate, not nominal.** Golf's
  "20 ms loop" actually delivered ~20 Hz wall-clock, and every physics
  constant was hand-tuned to that. The port runs a 0.05 s accumulator inside
  the 60 Hz update; the tick period is the *only* pacing knob. When porting
  or retuning, never "fix" pacing by rewriting physics constants — find the
  one knob.
- Fixed dt is 1/60 with catch-up (up to 4 updates per render). Updates and
  renders are not 1:1 — never do per-render gameplay work.

## The camera and what the player can see

- **Spawn action relative to the camera, not the world.** Gunship spawned
  zombies on a ring around the town center; with an orbiting camera the
  screen sat empty for 15–30 s ("I'm not seeing any zombies"). Spawning on a
  ring just outside the *view* radius fixed it: first visible enemy at 1.0 s.
- **Pull the camera toward what matters.** Survivors died off-screen until
  the aim point biased 70% toward the live-survivor centroid (clamped, ~0.9 s
  chase). If an event matters to the player, it must happen on screen.
- **Off-screen things the player must track get edge markers** (clamped
  inside the HUD, blinking when urgent). Golf's flag arrow is anchored to the
  **ball→target ray**, not the screen center — center-anchored was up to
  ~230 px of aim error once the camera clamped at world edges.
- **No animated zoom.** It made the user motion-sick in golf. Fixed zoom per
  context (per-club in golf), eased only on explicit user action.
- Screen-space aim error scales by 1/zoom in world space — widen hit queries
  accordingly (gunship's gatling query radius accounts for ZOOM).

## First-session experience

- The first interesting thing must happen within ~1–2 seconds of pressing
  start. Measure it in the sim; don't guess.
- Attract modes sell the game from the launcher (golf's title camera drifts
  the course and reseeds every ~14 s) — but route reseeds so entry stings
  don't replay every cycle.
- Boot is ~1.0 s and games launch instantly from the grid; don't add your own
  splash screens.

## Difficulty & fairness

- Ramp difficulty with progress, not time (golf: par/length/wind/dogleg all
  scale with hole number; gunship: 10 waves with telegraphs).
- Telegraph before punishing: spawn pings, wind pennants, wave breaks.
- Deliberate information asymmetry is a skill hook: golf's aim preview
  ignores wind — *reading the wind indicator* is the skill. Don't
  "helpfully" collapse those gaps.
- Randomness through `ctx->rng` so a seed reproduces a run.

## State machines & screens

- Terminal screens (win/lose/round-end) need: an explicit way out
  (see input-and-ux.md), edge-triggered dismissal (the tap that opens a
  screen must not also dismiss it), and kill of any lingering particles/
  effects from the previous state (a tapped-through celebration replayed
  its confetti on a later screen).
- Modal panels: back out only on taps **clearly outside** the panel rect;
  split rows at midlines and extend the last row to the panel edge — exact
  row bounds made low taps on the bottom row cancel the menu.
- Draw-once-and-hold for static screens (cards, menus): render once, early
  return until state changes. Repaint-in-place on toggle; never re-run the
  dim/backdrop pass (it compounds).
- Auto-advance after achievement moments (hole-out → beat → scorecard) beats
  requiring a tap, but keep taps as skip.

## Multiplayer on one device (pass-and-play pattern)

Golf's approach, proven through 4 players: keep the core single-actor —
globals always describe the *active* player, `save/load_player()` swaps state
through a slots array, and `num_players == 1` guards every MP branch so solo
stays untouched. Hand-over cards show whose turn + their color; per-player
"first look" intros track a seen flag per hole. Color pickers need more
colors than seats (6 tints for 4 seats) so cycling always has a spare — a
"no spares" guard once swallowed the tap entirely.

## Procedural generation

- Audit generators statistically, not anecdotally: histogram hundreds of
  seeds (par distribution), BFS-verify invariants (720/720 cart paths reach
  the edge).
- Chained-blob shapes need overlap discipline: spacing > ~2x lobe radius
  degrades organic chains into bead strings.
- Wall-following/marching algorithms: fix handedness per attempt and try both
  ends; re-picking the tangent sign mid-march oscillates in concave pockets.
- Expensive generation hides behind cards/transitions, sliced per tick;
  "tap to continue" appears only when the next thing is actually ready.

## Sensors as game mechanics

- Gesture power should come from **integrated** motion (golf: integrated
  forward-stroke acceleration = hand speed), not sampled peaks — peak
  sampling at frame rate "felt like a slot machine".
- Detectors run over the full 200 Hz accel ring
  (`gos_imu_accel_read`), in phases with an explicit settle step
  (SETTLE → WAIT → BACK → STROKE), thresholds in g / g·s, and a cancel path
  (tap while armed re-aims).
- Give live visual feedback during the gesture (power bar filling during the
  stroke, lingering after) — the sensor is invisible; the bar is the game.
- Keep detectors in the game, not the OS. The OS provides the raw stream only.

## Porting an existing game to gameos

1. Don't scope down fidelity to fit the default pipeline — users compare
   against the original. Use `GOS_CAP_FB565` and keep pixels 1:1 if the
   original was full-res.
2. Carry gameplay constants **verbatim**; adapt only the I/O edges (touch
   coords ×2, OS-owned volume, tick-rate knob).
3. Split big monoliths at natural seams (logic / render / screens) but keep
   proven touch zones and constants byte-identical.
4. The OS gains only *generic* primitives from a port (the accel ring, FB565,
   Montserrat text) — never game-specific logic.

## Process

- Users report symptoms, not causes ("menus do nothing" = touch parallax;
  "screen frozen" = DMA race). Get a serial trace before theorizing — input
  logging is permanent for exactly this reason.
- When a user rejects a build, the fix is usually one calibrated number
  (tick rate, spawn radius, zone height), not a redesign. Find the number.
- Record rejected approaches and *why* (animated zoom → motion sickness;
  minimap → stole attention; drop shadows → read as black borders) so they
  don't get re-proposed.
