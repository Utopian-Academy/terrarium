#include "terrarium_runtime.hpp"

#include "terrarium_app.hpp"
#include "terrarium_patch.hpp"
#include "terrarium_render.hpp"
#include "terrarium_version.hpp"

namespace {

struct CliOptions {
  Biome biome = MEADOW;
  bool startFullscreen = true;
  UiLang uiLang = UI_EN;
  bool wantSynth = false;
  bool wantMidi = false;
  bool printVersion = false;
  bool kiosk = false;  // start straight into the world, no menu
  int microFont = 8;   // world glyph size: 8 = full, 4 or 2 = micro marks
  std::string sf2Path = defaultSf2Path();
  float synthGain = 0.7f;
  std::string synthAudioDriver;
  std::string synthAudioDevice;
  std::string patchPath = defaultPatchPath();
};

struct RuntimeResources {
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  SynthOut synth;
  GlyphCache worldGlyphs;
  GlyphCache textGlyphs;
  Layout layout;
  MidiOut midi;
  std::vector<MidiParam> params;
  TelemetrySnapshot telemetry;

  ~RuntimeResources() {
    g_midiMirror = nullptr;
    midi.close();
    worldGlyphs.destroy();
    textGlyphs.destroy();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    synth.close();
  }
};

struct LoopState {
  bool running = true;
  bool paused = false;
  bool showMenu = true;
  bool midiClockOut = false;
  bool useSimClock = true;
  int menuPage = 0;
  int menuSel = 0;
  int rootKey = 0;
  int tps = DEFAULT_TPS;
  int tick = 0;
  int heldNote = -1;
  int heldNote2 = -1;
  int heldNote3 = -1;
  ScaleType scaleType = SCALE_PENTATONIC;
  std::string banner = "calm";
  uint32_t lastClockMs = 0;
  uint32_t lastParamSendMs = 0;
  std::chrono::steady_clock::time_point lastFrame =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point lastRippleUpdate =
      std::chrono::steady_clock::now();
};

struct SdlSession {
  bool initialized = false;

  ~SdlSession() {
    if (initialized) SDL_Quit();
  }
};

Biome parseBiomeName(const std::string& value) {
  if (value == "meadow") return MEADOW;
  if (value == "wetland") return WETLAND;
  if (value == "alpine") return ALPINE;
  if (value == "alien") return ALIEN;
  if (value == "tropical") return TROPICAL;
  if (value == "desert") return DESERT;
  return MEADOW;
}

UiLang parseUiLanguage(const std::string& value) {
  if (value == "kata" || value == "katakana" || value == "ja" ||
      value == "jp") {
    return UI_KATA;
  }
  return UI_EN;
}

CliOptions parseCliOptions(int argc, char** argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--biome") == 0 && i + 1 < argc) {
      options.biome = parseBiomeName(argv[++i]);
    } else if (std::strcmp(argv[i], "--windowed") == 0) {
      options.startFullscreen = false;
    } else if (std::strcmp(argv[i], "--fullscreen") == 0) {
      options.startFullscreen = true;
    } else if (std::strcmp(argv[i], "--synth") == 0) {
      options.wantSynth = true;
    } else if (std::strcmp(argv[i], "--midi") == 0) {
      options.wantMidi = true;
    } else if (std::strcmp(argv[i], "--sf2") == 0 && i + 1 < argc) {
      options.sf2Path = argv[++i];
      options.wantSynth = true;
    } else if (std::strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
      options.synthGain = (float)std::atof(argv[++i]);
      options.wantSynth = true;
    } else if (std::strcmp(argv[i], "--audio-driver") == 0 && i + 1 < argc) {
      options.synthAudioDriver = argv[++i];
      options.wantSynth = true;
    } else if (std::strcmp(argv[i], "--audio-device") == 0 && i + 1 < argc) {
      options.synthAudioDevice = argv[++i];
      options.wantSynth = true;
    } else if (std::strcmp(argv[i], "--patch") == 0 && i + 1 < argc) {
      options.patchPath = argv[++i];
    } else if (std::strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
      options.uiLang = parseUiLanguage(argv[++i]);
    } else if (std::strcmp(argv[i], "--version") == 0) {
      options.printVersion = true;
    } else if (std::strcmp(argv[i], "--kiosk") == 0) {
      options.kiosk = true;
    } else if (std::strcmp(argv[i], "--daynight") == 0 && i + 1 < argc) {
      const char* m = argv[++i];
      g_daynightMode = (std::strcmp(m, "clock") == 0) ? 2
                       : (std::strcmp(m, "off") == 0) ? 0
                                                      : 1;
    } else if (std::strcmp(argv[i], "--microfont") == 0) {
      options.microFont = 4;
      if (i + 1 < argc && (std::strcmp(argv[i + 1], "2") == 0 ||
                           std::strcmp(argv[i + 1], "4") == 0)) {
        options.microFont = std::atoi(argv[++i]);
      }
    }
  }
  return options;
}

bool initializeSdl(SdlSession& session) {
  // Video only: FluidSynth owns audio via its own driver (SDL audio unused).
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return false;
  }
  session.initialized = true;
  return true;
}

bool createWindowAndRenderer(const CliOptions& options,
                             RuntimeResources& resources) {
  Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
  if (options.startFullscreen) {
    windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  }

  resources.window = SDL_CreateWindow(
      terrarium::kDisplayName, SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, 1280, 720, windowFlags);
  if (!resources.window) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    return false;
  }

  resources.renderer = SDL_CreateRenderer(
      resources.window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
  if (!resources.renderer) {
    resources.renderer = SDL_CreateRenderer(
        resources.window, -1,
        SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
  }
  if (!resources.renderer) {
    std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
    return false;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  resources.worldGlyphs.textMode = false;
  resources.worldGlyphs.microSize = options.microFont;
  resources.textGlyphs.textMode = true;
  resources.layout = computeLayout(resources.renderer);
  return true;
}

uint32_t makeSeed() {
  return (uint32_t)std::chrono::high_resolution_clock::now()
      .time_since_epoch()
      .count();
}

void initializeSimulation(const CliOptions& options, RuntimeResources& resources,
                          uint32_t& seed, Rng& rng, World& world) {
  seed = makeSeed();
  rng = Rng(seed);
  seedWorld(world, rng, options.biome);

  if (resources.midi.open(0)) {
    std::cerr << "[midi] ALSA port 'Terrarium MIDI OUT' created — connect it "
                 "to your synth/host and press O to start sending.\n";
  }
  // Opt-in via the O key, or start sending immediately with --midi.
  resources.midi.enabled = options.wantMidi;
  g_midiMirror = &resources.midi;
  resources.params = makeDefaultMidiParams();
  resources.telemetry = collectTelemetry(world, 0);
  updateTelemetryParams(resources.params, resources.telemetry);
}

void initializeSynth(const CliOptions& options, RuntimeResources& resources) {
  if (!options.wantSynth) {
    applyVoiceMixer(resources.synth);
    return;
  }

  if (options.sf2Path.empty()) {
    std::cerr << "[synth] No SoundFont found. Provide --sf2 path/to/file.sf2\n";
  } else if (!resources.synth.open(
                 options.sf2Path, std::clamp(options.synthGain, 0.0f, 2.0f),
                 options.synthAudioDriver, options.synthAudioDevice)) {
    std::cerr << "[synth] Failed to start synth (check --sf2 path and audio "
                 "driver). Try: --audio-driver pipewire (or pulseaudio/alsa).\n";
  }

  applyVoiceMixer(resources.synth);
}

void adjustSelectedParamWeight(std::vector<MidiParam>& params, int menuSel,
                               float delta) {
  int selected = clampi(menuSel, 0, (int)params.size() - 1);
  params[selected].weight = std::clamp(params[selected].weight + delta, 0.0f, 2.0f);
  refreshMidiParamValues(params);
}

void reseedWorld(uint32_t& seed, Rng& rng, World& world, LoopState& loop) {
  seed = makeSeed();
  rng = Rng(seed);
  seedWorld(world, rng, world.biome);
  loop.tick = 0;
  loop.banner = "reset";
}

void toggleFullscreen(RuntimeResources& resources, bool showHud) {
  Uint32 flags = SDL_GetWindowFlags(resources.window);
  bool fullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  SDL_SetWindowFullscreen(resources.window,
                          fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  resources.layout = computeLayout(resources.renderer, showHud);
}

void setMenuVisible(RuntimeResources& resources, LoopState& loop, bool show) {
  loop.showMenu = show;
  resources.layout = computeLayout(resources.renderer, show);
}

void adjustModMapField(int dir) {
  g_g_mmSel = clampi(g_g_mmSel, 0, MOD_SLOTS - 1);
  ModMap& mapping = g_modMap[g_g_mmSel];
  switch (clampi(g_g_mmField, 0, 4)) {
    case 0:
      mapping.src = clampi(mapping.src + dir, 0, MOD_N - 1);
      break;
    case 1:
      mapping.dest = clampi(mapping.dest + dir, DEST_NONE, DEST_COUNT - 1);
      break;
    case 2:
      mapping.amt = std::clamp(mapping.amt + 0.05f * (float)dir, -2.0f, 2.0f);
      break;
    case 3:
      mapping.smooth =
          std::clamp(mapping.smooth + 0.02f * (float)dir, 0.0f, 0.98f);
      break;
    default:
      mapping.cc = clampi(mapping.cc + dir, 0, 127);
      break;
  }
  g_patchDirty = true;
}

void handleMenuVertical(LoopState& loop, RuntimeResources& resources,
                        const World& world, int dir) {
  const int page = loop.menuPage % kMenuPageCount;
  if (loop.showMenu && page == 6) {
    const int agentCount = (int)world.agents.size();
    if (agentCount > 0) {
      g_inspectIdx = clampi(g_inspectIdx + dir, 0, agentCount - 1);
    }
    return;
  }
  if (loop.showMenu && page == 7) {
    g_g_modScroll = clampi(g_g_modScroll + dir, 0, std::max(0, MOD_N - 14));
    return;
  }
  if (loop.showMenu && page == 8) {
    g_g_mmSel = (g_g_mmSel + MOD_SLOTS + dir) % MOD_SLOTS;
    return;
  }
  cycleMenuSelection(
      loop.menuSel,
      menuSelectionCount(loop.showMenu, loop.menuPage, resources.params, world),
      dir);
}

void handleKeyDown(SDL_Keycode key, RuntimeResources& resources, LoopState& loop,
                   uint32_t& seed, Rng& rng, World& world, UiLang& uiLang) {
  switch (key) {
    case SDLK_ESCAPE:
      loop.running = false;
      break;
    case SDLK_TAB:
      if (loop.showMenu) {
        loop.menuPage = (loop.menuPage + 1) % kMenuPageCount;
      }
      break;
    case SDLK_f:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 6) {
        g_followInspect = !g_followInspect;
      }
      break;
    case SDLK_F2:
      uiLang = (uiLang == UI_EN) ? UI_KATA : UI_EN;
      break;
    case SDLK_b:
      startBiomeMorph(world, rng);
      break;
    case SDLK_SPACE:
      loop.paused = !loop.paused;
      break;
    case SDLK_w:
      if (g_zoom > 1 && !loop.showMenu) {
        g_camY -= 2;
        clampCameraToWorld();
      }
      break;
    case SDLK_a:
      if (g_zoom > 1 && !loop.showMenu) {
        g_camX -= 2;
        clampCameraToWorld();
      }
      break;
    case SDLK_d:
      if (g_zoom > 1 && !loop.showMenu) {
        g_camX += 2;
        clampCameraToWorld();
      }
      break;
    case SDLK_PERIOD:
      if (loop.paused) {
        stepSimulationOnce(world, rng, loop.banner, loop.tick, resources.synth,
                           loop.heldNote, loop.heldNote2, loop.heldNote3,
                           loop.rootKey, loop.scaleType, resources.params);
      }
      break;
    case SDLK_LEFTBRACKET:
      if (loop.tps > 1) loop.tps--;
      break;
    case SDLK_RIGHTBRACKET:
      if (loop.tps < 30) loop.tps++;
      break;
    case SDLK_r:
      reseedWorld(seed, rng, world, loop);
      break;
    case SDLK_F11:
      toggleFullscreen(resources, loop.showMenu);
      break;
    case SDLK_F1:
      // Universal UI toggle — works even on the MIXER page where M = mute.
      setMenuVisible(resources, loop, !loop.showMenu);
      break;
    case SDLK_m:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 5) {
        toggleMixerMuteSelection(resources.synth, loop.menuSel);
      } else {
        setMenuVisible(resources, loop, !loop.showMenu);
      }
      break;
    case SDLK_o:
      resources.midi.enabled = !resources.midi.enabled;
      if (resources.midi.enabled) {
        resources.midi.sendStart();
      } else {
        resources.midi.sendStop();
      }
      break;
    case SDLK_c:
      loop.midiClockOut = !loop.midiClockOut;
      break;
    case SDLK_v:
      loop.useSimClock = !loop.useSimClock;
      break;
    case SDLK_UP:
      handleMenuVertical(loop, resources, world, -1);
      break;
    case SDLK_DOWN:
      handleMenuVertical(loop, resources, world, 1);
      break;
    case SDLK_LEFT:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 8) {
        g_g_mmField = (g_g_mmField + 4) % 5;
      }
      break;
    case SDLK_RIGHT:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 8) {
        g_g_mmField = (g_g_mmField + 1) % 5;
      }
      break;
    case SDLK_e:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 8) {
        g_g_mmSel = clampi(g_g_mmSel, 0, MOD_SLOTS - 1);
        g_modMap[g_g_mmSel].enabled = !g_modMap[g_g_mmSel].enabled;
        g_patchDirty = true;
      }
      break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 1) {
        adjustChaosWeight(loop.menuSel, -kParamAdjustStep);
      } else if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 4) {
        adjustVoiceSettings(loop.menuSel, -1);
      } else if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 5) {
        adjustMixerLevel(resources.synth, loop.menuSel, -kParamAdjustStep);
      } else if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 8) {
        adjustModMapField(-1);
      } else {
        adjustSelectedParamWeight(resources.params, loop.menuSel,
                                  -kParamAdjustStep);
      }
      break;
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 1) {
        adjustChaosWeight(loop.menuSel, kParamAdjustStep);
      } else if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 4) {
        adjustVoiceSettings(loop.menuSel, 1);
      } else if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 5) {
        adjustMixerLevel(resources.synth, loop.menuSel, kParamAdjustStep);
      } else if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 8) {
        adjustModMapField(1);
      } else {
        adjustSelectedParamWeight(resources.params, loop.menuSel,
                                  kParamAdjustStep);
      }
      break;
    case SDLK_k:
      loop.rootKey = (loop.rootKey + 1) % 12;
      g_patchDirty = true;
      break;
    case SDLK_s:
      if (loop.showMenu && (loop.menuPage % kMenuPageCount) == 5) {
        toggleMixerSoloSelection(resources.synth, loop.menuSel);
        break;
      }
      if (g_zoom > 1 && !loop.showMenu) {
        g_camY += 2;
        clampCameraToWorld();
        break;
      }
      loop.scaleType = (ScaleType)(((int)loop.scaleType + 1) % 7);
      g_patchDirty = true;
      break;
    default:
      break;
  }
}

void handleEvent(const SDL_Event& event, RuntimeResources& resources,
                 LoopState& loop, uint32_t& seed, Rng& rng, World& world,
                 UiLang& uiLang) {
  if (event.type == SDL_QUIT) {
    loop.running = false;
    return;
  }

  if (event.type == SDL_KEYDOWN) {
    handleKeyDown(event.key.keysym.sym, resources, loop, seed, rng, world,
                  uiLang);
    return;
  }

  if (event.type == SDL_WINDOWEVENT &&
      (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
       event.window.event == SDL_WINDOWEVENT_RESIZED ||
       event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) {
    resources.layout = computeLayout(resources.renderer, loop.showMenu);
    return;
  }

  if (event.type == SDL_MOUSEWHEEL) {
    if (event.wheel.y > 0) g_zoom = std::min(4, g_zoom + 1);
    if (event.wheel.y < 0) g_zoom = std::max(1, g_zoom - 1);
    clampCameraToWorld();
    return;
  }

  if (event.type == SDL_MOUSEBUTTONDOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    handleWorldClick(world, rng, resources.layout, event.button.x,
                     event.button.y);
  }
}

void updateAndRender(RuntimeResources& resources, LoopState& loop, World& world,
                     Rng& rng, const CliOptions& options, UiLang uiLang) {
  auto now = std::chrono::steady_clock::now();
  auto dtMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - loop.lastFrame)
          .count();

  // Ripples advance on real frame time; lastFrame only advances on sim ticks,
  // so it must not be reused here or ripples age many times too fast.
  auto rippleDtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - loop.lastRippleUpdate)
                        .count();
  loop.lastRippleUpdate = now;
  updateRipples((float)rippleDtMs / 1000.0f);
  const int msPerTick = 1000 / std::max(1, loop.tps);

  advanceBiomeFade(world, rng);

  if (!loop.paused && dtMs >= msPerTick) {
    loop.lastFrame = now;
    stepSimulationOnce(world, rng, loop.banner, loop.tick, resources.synth,
                       loop.heldNote, loop.heldNote2, loop.heldNote3,
                       loop.rootKey, loop.scaleType, resources.params);
    followSelectedAgent(world);
  }

  SDL_SetWindowTitle(resources.window,
                     buildWindowTitle(world, loop.tick, loop.paused, loop.tps,
                                      loop.banner)
                         .c_str());

  resources.telemetry = collectTelemetry(world, loop.tick);
  updateTelemetryParams(resources.params, resources.telemetry);

  uint32_t nowMs = SDL_GetTicks();
  sendChangedMidiParams(resources.midi, resources.params, nowMs,
                        loop.lastParamSendMs);
  sendModMatrixMidi(resources.midi);
  pumpMidiClock(resources.midi, loop.midiClockOut, loop.useSimClock,
                resources.telemetry, nowMs, loop.lastClockMs);

  // Debounced patch autosave so edits survive a crash, not just clean quits.
  if (g_patchDirty) {
    static uint32_t lastPatchSaveMs = 0;
    if (nowMs - lastPatchSaveMs > 1500) {
      savePatch(options.patchPath, loop.rootKey, (int)loop.scaleType);
      lastPatchSaveMs = nowMs;
      g_patchDirty = false;
    }
  }

  clampCameraToWorld();
  render(resources.renderer, resources.layout, world, resources.worldGlyphs,
         resources.textGlyphs, loop.tick, loop.showMenu, loop.menuPage,
         resources.params, loop.menuSel, resources.synth.enabled,
         options.sf2Path, uiLang);
  // ~60fps cap. The sim ticks at most 30/s; rendering faster than 60fps just
  // burns CPU on the per-cell glyph pass with no visible benefit.
  SDL_Delay(16);
}

}  // namespace

int runTerrarium(int argc, char** argv) {
  CliOptions options = parseCliOptions(argc, argv);

  if (options.printVersion) {
    std::cout << terrarium::kDisplayName << "\n";
    return 0;
  }

  SdlSession sdl;
  if (!initializeSdl(sdl)) {
    return 1;
  }

  RuntimeResources resources;
  if (!createWindowAndRenderer(options, resources)) {
    return 1;
  }

  uint32_t seed = 0;
  Rng rng;
  World world;
  initializeSimulation(options, resources, seed, rng, world);

  // Restore the saved patch (mod matrix, chaos, voices, mixer, key/scale)
  // before the synth starts so applyVoiceMixer reflects it.
  int patchRootKey = 0;
  int patchScale = (int)SCALE_PENTATONIC;
  const bool patchLoaded =
      loadPatch(options.patchPath, patchRootKey, patchScale);
  if (patchLoaded) {
    std::cerr << "[patch] loaded " << options.patchPath << "\n";
  }

  initializeSynth(options, resources);

  LoopState loop;
  if (options.kiosk) loop.showMenu = false;
  if (patchLoaded) {
    loop.rootKey = patchRootKey;
    loop.scaleType = (ScaleType)patchScale;
  }
  loop.lastFrame = std::chrono::steady_clock::now();

  while (loop.running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      handleEvent(event, resources, loop, seed, rng, world, options.uiLang);
    }
    updateAndRender(resources, loop, world, rng, options, options.uiLang);
  }

  savePatch(options.patchPath, loop.rootKey, (int)loop.scaleType);
  return 0;
}
