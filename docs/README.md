# gameos — Game Developer Design Specs

These documents exist so that an agent (or human) can build a new game for this
device **without rediscovering the hardware's traps**. Every constant and rule
in here was verified on the physical unit or paid for with a user-reported bug.
Trust these documents over intuition; verify against source before changing them.

**The device:** Waveshare ESP32-S3-Touch-AMOLED-1.8 — 368x448 AMOLED (CO5300,
QSPI), CST816S single-point touch, QMI8658 IMU, ES8311 codec + speaker,
AXP2101 PMIC, one physical button (BOOT), 8 MB PSRAM. ESP-IDF v6.x, plain C.

## Reading order

| Doc | What it covers |
|---|---|
| [hardware.md](hardware.md) | The board: display, touch, IMU, audio, power, button — and every bring-up trap |
| [game-contract.md](game-contract.md) | `gos.h`: lifecycle, capabilities, registration, save data, how to add a game |
| [rendering.md](rendering.md) | The 8bpp indexed pipeline, palette rules, the FB565 full-res mode, performance budget |
| [input-and-ux.md](input-and-ux.md) | Touch parallax, hit-target sizes, tilt aim, gestures, escape routes, screen safe zone |
| [audio.md](audio.md) | The 8-voice synth mixer, sound design recipes that work on this speaker |
| [testing-and-verification.md](testing-and-verification.md) | Host simulator workflow, serial logging without resetting the board, build/flash |
| [design-playbook.md](design-playbook.md) | Cross-cutting lessons from shipping Zombie Gunship and Infinite Golf |

## The three rules that outrank everything else

1. **A game includes only `gos.h`.** Never a HAL header, never a driver, never
   LVGL in the frame path. If the contract is missing something, extend the
   contract (generically) — don't punch through it.
2. **Never ship a screen without a way out.** The OS guarantees swipe-down-from-
   top and the BOOT button open the pause overlay, but explicit EXIT affordances
   in the game are still expected on terminal screens (see input-and-ux.md).
3. **Verify on the host simulator before claiming anything works.** Rendering,
   pacing, and input flows are all testable without hardware
   (see testing-and-verification.md). "It compiles" is not verification.
