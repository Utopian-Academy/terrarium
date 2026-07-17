# Terrarium

A Dwarf-Fortress-inspired ecosystem simulator that plays music. Weather,
water, flora, and fauna evolve on a 200-cell-wide world; a mod matrix turns
50 live ecosystem signals (water level, wind, stress, panic, swarm cohesion,
"trickster mischief"…) into melody, expression, and MIDI CC — either through
the built-in FluidSynth soundfont engine or by taking over any external
synth/VST via MIDI.

## Builds

| Target | What it is |
|---|---|
| `terrarium` | Desktop app (SDL2): glyph renderer, menus, mod matrix editor, FluidSynth + ALSA MIDI out |
| `terrarium-pico` | 200x200-pixel build for very low-power hardware (Raspberry Pi Zero): one pixel per world cell, no audio |
| `terrarium-vat` | CLAP / VST3 / LV2 plugin (MIDI generator): the sim runs inside your DAW and plays whatever you route it to |

```sh
# deps (Debian/Ubuntu): cmake g++ libsdl2-dev
#   optional synth:  libfluidsynth-dev libpipewire-0.3-dev
#   optional MIDI:   libasound2-dev
#   optional plugin: git clone --depth 1 https://github.com/DISTRHO/DPF.git plugin/dpf

cmake -S . -B build -DTERRARIUM_USE_FLUIDSYNTH=ON
cmake --build build -j$(nproc)

./build/terrarium --windowed --synth --sf2 path/to/font.sf2   # standalone
./build/terrarium --midi                                      # MIDI-only (play external synths)
./build/terrarium-pico --scale 3                              # pixel build
# plugin artifacts: build/bin/terrarium-vat.{clap,vst3,lv2}
```

Keys: SPACE pause · `.` step · `[ ]` speed · `r` reseed · `b` biome morph ·
TAB menu pages · `M`/`F1` hide UI · `O` MIDI out · `K` key · `S` scale ·
MODMAP page: arrows + `+/-` edit, `E` enable · F11 fullscreen · ESC quit.

Patches (mod matrix, chaos weights, voices, mixer, key/scale) autosave to
`~/.config/terrarium/patch.txt` and are shared by the standalone and the
plugin. See `CHANGES.md` and `docs/` for history and the 2026-07-15 audit.

`archive/monoliths/` holds the project's single-file lineage
(`terrarium_0.1` → `terrarium_remix_0.70.05`), kept for reference; the
modular `src/` tree is canonical.

## License & legal

Terrarium is released under the **ISC License** (see `LICENSE`).

Third-party components:

- **DPF (DISTRHO Plugin Framework)** — ISC license. Not vendored in this
  repository; the build fetches/expects a checkout at `plugin/dpf`. DPF
  provides its own independently written VST3 interface headers, so the
  Steinberg VST3 SDK is not used or required.
- **CLAP** headers — MIT license (via DPF).
- **LV2** — ISC license (via DPF).
- **SDL2** (zlib license), **FluidSynth** (LGPL-2.1), **ALSA lib**
  (LGPL-2.1), **PipeWire** (MIT) — used as dynamically linked system
  libraries; their licenses apply to those libraries, not to this code.

*VST is a trademark of Steinberg Media Technologies GmbH, registered in
Europe and other countries. Ableton and Live are trademarks of Ableton AG.
Serum is a trademark of Xfer Records. This project is not affiliated with
or endorsed by any of them.*

SoundFont files are **not** distributed with this repository; supply your
own and check their individual licenses.
