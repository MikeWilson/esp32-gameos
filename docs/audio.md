# Audio

The OS owns the codec (ES8311) and mixes an **8-voice chip synth** at
22050 Hz mono 16-bit. There are no PCM assets; every sound on the device is a
synth program — and that's a feature: sounds are tiny const tables in flash,
tweakable in code review.

Games only touch `gos_audio_*`. The bring-up trap (never
`bsp_audio_codec_speaker_init()` — it boot-loops) is HAL territory; see
hardware.md if you ever work below the contract.

## The API

```c
gos_note_t   {f0, f1, ms, wave, vol}   // f0→f1 Hz glide over ms
gos_sfx_t    {notes, count, prio}      // higher prio survives voice stealing
gos_audio_play(sfx, vol)               // one-shot, returns handle
gos_audio_loop(sfx, vol)               // repeats until stopped
gos_audio_set_vol(handle, vol)         // live — for ducking/throttle
gos_audio_stop / stop_all
gos_audio_master(0..3)                 // OS setting: mute/low/med/high
```

Waves: `SQ50, SQ25, TRI, SAW, NOISE, REST`. Envelope: ~2 ms attack + linear
decay per note. The mixer hands voices off lock-free and steals the
lowest-priority voice when full.

## Sound design recipes that survived user testing

- **Impacts/strikes:** a brief `NOISE` contact click gliding into a woody
  `TRI` body. Never plain squares for physical hits — "buzzy", not club-like.
  A pure short triangle "tock" works for small precise hits (putter).
- **Jingles vs blips:** reserve high `prio` for jingles/fanfares so gameplay
  blips can't steal them mid-phrase; keep gameplay blips latest-wins at low
  prio.
- **Engine/ambience:** use `gos_audio_loop` + live `set_vol` for ducking
  (drop the engine under a fanfare, restore after).
- **Frequency-triggered sounds must earn their keep.** Golf shipped a
  pass-card ding and removed it — anything the player hears every 20 seconds
  grates. Celebrate rare events; keep frequent events silent or near-silent.
- **Demo the change:** when the user adjusts a volume/sound setting, play a
  tap sound immediately at the new level so it demos itself.
- **One sting per screen entry**, not per animation tick — golf's title
  attract mode reseeds every ~14 s and routes through a flag so the title
  sting doesn't replay each cycle.

## Rules

- Silence looping voices in `suspend()`; resume them in `resume()`.
- Volume is an **OS setting** (`gos_audio_master`, 0–3, NVS-persisted).
  Games do not ship their own volume row — note "sound is in OS settings" on
  your options page if players will look for it there.
- Keep per-sfx `vol` headroom below 255 so ducking and master scaling have
  room to work.
