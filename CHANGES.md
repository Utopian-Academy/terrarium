# Terrarium changes

## 2026-07-22 — plugin UI: see the vat (+ Windows build)

- **The plugin shows the environment now.** A resizable editor window renders
  the living world 1px-per-cell (the pico renderer, factored into a shared
  `terrarium_pixelview.hpp`) — nearest-neighbour scaled, cloud shadows,
  day/night, wildflower colors and all. The DSP renders the frame after each
  sim tick, only while an editor is open; the UI just uploads a texture, so
  tps stays the repaint rate. Works in VST3 and CLAP (single-binary formats);
  LV2 splits UI and DSP into separate modules and stays dark for now.
- Whole repo now builds on Windows/MSVC (standalone, pico, and all three
  plugin formats): `NOMINMAX` + legacy `near`/`far` macro undefs, `_mkdir`
  include, SDL2 via CMake config package instead of pkg-config, and
  `SDL_MAIN_HANDLED` for pico.

## 2026-07-18 — nature-vivid color pass (standalone, plugin, pico)

Ported from Deckboy's embedded terrarium (the owner: scenes read faded/pastel,
pinks read Lisa Frank — should be vibrant and life-evoking, and read as
*nature*):

- **Flowers** draw from wildflower tones instead of bubblegum: meadow gets
  poppy/marigold/orchid/thistle/white/cornflower; tropical trades hot pink
  for hibiscus red; wetland's pale lavenders deepen to iris and marsh-orchid
  violets. Alpine keeps its edelweiss whites.
- **Mushrooms** are forest-floor caps (cream, tan, fly-agaric red) instead of
  candy pink / lilac / baby blue.
- **Vivid pass pushes harder**: `VIVID_SAT` 1.18 → 1.32, `VIVID_VAL`
  1.08 → 1.10.
- **Pico** (1 px/cell): flowers were two flat pinks; now a per-cell hash
  picks from five wildflower colors, and mushrooms rotate cream/tan/red-cap.

Standalone and the DPF plugin share `terrarium_visuals.cpp`, so both get the
palette; pico's inline palette updated to match.

## 2026-07-16 (night) — patch persistence + Terrarium as a plugin

### Patch persistence
- New SDL-free module `src/terrarium_patch.{hpp,cpp}`: saves/loads the whole
  sonic patch — all 12 mod-matrix slots (src/dest/amt/smooth/enabled/cc),
  chaos weights, voice settings (program/range/transpose/velMul/manual flag),
  mixer faders+mutes, root key and scale — to
  `~/.config/terrarium/patch.txt` (override with `--patch <file>`).
- Autoloads at startup; autosaves 1.5s after any edit (crash-safe) and on
  quit. Round-trip covered by a test harness (all fields verified).
- Telemetry helpers (makeDefaultMidiParams/collectTelemetry/
  updateTelemetryParams/refreshMidiParamValues) moved from app.cpp into the
  patch module so plugin builds can link them without SDL.
- `MidiSink` abstraction: `g_midiMirror` is now an interface; the ALSA
  `MidiOut` implements it for the standalone, the plugin queues into the
  host. Removed the vestigial SDL include from `terrarium_core.hpp` — the
  whole engine (core+audio+patch) now compiles with no SDL at all.

### Plugin (CLAP / VST3 / LV2, via DPF)
- `plugin/TerrariumPlugin.cpp` + DPF checkout in `plugin/dpf` (gitignored;
  clone with `git clone --depth 1 https://github.com/DISTRHO/DPF.git
  plugin/dpf`). Built by default when present
  (`TERRARIUM_BUILD_PLUGIN=ON`); artifacts land in
  `build/bin/terrarium-vat.{clap,vst3,lv2}`.
- MIDI-generator plugin: runs the full ecosystem sim on a sample clock
  inside the host and emits the same notes + CC streams (melody/harmony/
  bass/drums + mod-matrix `MIDI CC` slots) into the host graph — route to
  Serum etc. on another track. Silent audio outs.
- 12 host-automatable parameters: sim speed, biome (reseeds), the 7 chaos
  weights, root key, scale, and a Reseed trigger.
- **Shares the standalone's patch file** — design the matrix in the app,
  perform with the plugin.
- Verified with a minimal CLAP host (dlopen + process loop): 20s of audio
  time produced 11 note-ons with 11 matching note-offs and 726 CCs.
- Known limitation: engine state is global — run one instance per host
  process for now. Also requires `LANGUAGES C CXX` in CMake (DPF has C
  sources; missing C caused "internal CMake variable" errors).

## 2026-07-16 (evening) — external MIDI: play any synth/VST with the vat

Goal: point Terrarium at an external instrument (e.g. Serum in a VST host)
and let the ecosystem play it and take over its parameters.

- **Real ALSA sequencer MIDI output** (`MidiOut`, guarded by
  `TERRARIUM_HAS_ALSA`, auto-detected in CMake). Creates client "Terrarium"
  with port "Terrarium MIDI OUT" — visible in aconnect/qjackctl/PipeWire and
  routable to any host or soft-synth. Enable with the `O` key or the new
  `--midi` flag.
- **Full mirroring**: every note on/off and CC the engine generates
  (melody ch0, harmony ch1, bass ch2, drums ch9, CC7/10/11/74/65/5 lines)
  is mirrored out the port via `g_midiMirror` inside `SynthOut`.
- **Synthless MIDI mode**: the note/CC engine now runs when only MIDI is
  enabled (`audioActive()`), so Terrarium can drive an external instrument
  with FluidSynth completely off (verified: notes + CC captured by aseqdump
  with no soundfont loaded).
- **Mod matrix → any external knob**: new `MIDI CC` destination with a
  per-slot CC number (5th MODMAP field, LEFT/RIGHT to reach it). Any of the
  50 ecosystem signals can drive any MIDI-learnable parameter. Verified
  end-to-end: water level drove CC74 through a real ALSA port (254 events,
  value tracking the wetland filling).
- Muted voices no longer leak velocity-1 ghost notes to external MIDI.
- MIDI clock (`C` key) and start/stop already existed and now actually emit.

Serum workflow: run `./build/terrarium --midi`, connect "Terrarium MIDI OUT"
to your host's MIDI input (PipeWire/qjackctl/aconnect), put Serum on that
track, then MIDI-learn Serum knobs from MODMAP slots set to `MIDI CC<n>`.
Mod-matrix slots are still session-only (not saved) — persistence is the
next obvious piece.

## 2026-07-16 (later) — clean UI hide + mod matrix takeover

- **UI can now fully disappear**: hiding the menu (`M`, or the new universal
  `F1` which works even on the MIXER page where M = mute) also collapses the
  bottom HUD strip — the world fills the whole window. `computeLayout()`
  takes a `showHud` flag; layout recomputes on toggle/resize/fullscreen.
- **Mod matrix has authority over its destinations** (verified end-to-end):
  - Animated automation no longer overwrites CC10 pan when an enabled slot
    targets Pan (`modMapControls()` helper in core.hpp).
  - When a slot targets CC11, the matrix takes over expression outright
    instead of only attenuating the automation value.
- **Data flow verified** by headless harness (wetland, 1800 ticks): water
  level drove CC11 across 0.12–1.00, wind drove pan 0.11–0.90, stress drove
  V0 portamento; disabling slots returned targets to neutral (1.0/0.5/0.0).
  MODMAP editing keys (UP/DOWN/LEFT/RIGHT/+/−/E) were fixed yesterday.

## 2026-07-16 — renderer cache + remaining audit smells

Note: the src/ tree's fork from the Feb 23 baseline (not remix 0.70.05) was
**intentional** — the owner was unhappy with the late remix versions. The remix
monoliths stay as reference; do not port their ecosystem unless asked.

- **World render cache**: the world pass (~2 SDL calls x 22400 cells) now
  renders into a target texture and is redrawn only when the tick, camera,
  or zoom changes or ripples are animating; quiet frames just blit it.
  Renderer is created with `SDL_RENDERER_TARGETTEXTURE`; falls back to the
  direct path if targets are unsupported. ~61% → ~48% of a core under the
  software renderer; bigger win on accelerated displays.
- **INSPECT selection survives deaths**: the inspected agent is re-found by
  id after the cull pass (indices shift on erase).
- **Channel-0 program unified**: auto instrument changes now write
  `g_voice[0].program` (single sender via the per-voice cache) and are
  skipped once V0's program has been set manually.
- **Agent motion history keyed by id** in `computeViewAudioMetrics` —
  index-keyed history attributed one agent's motion to another after deaths.
- **Species sprite colors**: the 8 abstract species (glyphs 0x80+) each get
  a distinct color instead of falling through to plain white.

## 2026-07-15 — audit fixes, perf pass, terrarium-pico

Full audit of the modular `src/` tree (see `docs/AUDIT_2026-07-15.md`).

### Fixed
- **Ecosystem was empty**: nothing ever spawned agents, so the world only
  contained the 2 legendary agents. Added `maybeImmigrateAgents()` — migrants
  (grazers/birds/predators/fish) arrive from world edges up to a cap of
  `(W*H)/300`.
- **Biome morph did nothing**: pressing `B` changed the biome label but
  `world.bw` kept the old biome's growth weights until reseed
  (`biomeMorphT` was never advanced). Weights now lerp `bwFrom → bwTo` over
  ~7 s in `advanceBiomeFade()`.
- **Ripples aged ~10x too fast**: `updateRipples()` was fed time since the
  last *sim tick* on every rendered frame (quadratic accumulation). Ripples
  now use their own frame-time clock.
- **MODS / MODMAP / INSPECT menu pages were dead**: no key handling existed
  for scrolling mods, selecting/editing/enabling mod-matrix slots, or
  selecting agents. The whole mod matrix was unreachable. Wired UP/DOWN,
  LEFT/RIGHT, `+/-` and `E` per page.
- **W/A/D fell through to "step"**: at zoom 1 (or with menu open) pressing
  W/A/D single-stepped the sim via case fallthrough.
- **`S` scale cycle skipped Whole-tone**: `% 6` → `% 7`.
- **FluidSynth pipewire driver had no `pw_init()`** (regression vs the
  0.70.05 monolith): CMake now links libpipewire when FluidSynth is enabled
  and `SynthOut::open()` calls `pw_init()` once. Also removed the
  `synth.chorus.type` setting (removed from FluidSynth in 2.3.x).
- **`SDL_INIT_AUDIO` removed** — FluidSynth owns audio (0.70.05 lesson).
- Ripple chaos could write water depth 8–9 (max everywhere else is 7).
- `g_stepEvents` grew without bound when the synth was disabled (its only
  consumer was the synth path); now drained every tick.

### Performance
- `stepTerrain` was 87% of tick time (gprof): 12 `countNeighborsChar()`
  calls (~96 probes) per cell replaced with one bucketing 8-neighbor pass;
  fire-spread scan skipped when nothing is burning. ~12 ms → ~5 ms per tick
  at 200x200 on an i5-5257U.
- `Rng` switched from mt19937 + per-call `uniform_*_distribution` to
  xorshift64* (same API, ~15% tick cost).
- Main app frame loop capped at ~60 fps (`SDL_Delay(6)` → `16`); it was
  rendering the full glyph pass at ~150 fps for a 6 TPS sim.

### Added
- **`terrarium-pico`** target: 200x200-pixel build for very low-power
  hardware (Raspberry Pi Zero). One pixel per world cell into a streaming
  texture, redrawn only on sim ticks. World is 200x200 cells (`TERRA_W/H`
  compile defines). Flags: `--biome --scale --tps --seed --fullscreen`.
  Optional `-DTERRA_PICO_PROF` compile define prints loop/tick/section
  timings.

### Known remaining issues at the time (all addressed 2026-07-16 above,
### except:)
- The remix line's audio watchdog (periodic resetAudio) has no equivalent
  in `src/` yet.
- `menuSel` is shared across menu pages; a page switch can leave a stale
  selection row highlighted until the next UP/DOWN.
