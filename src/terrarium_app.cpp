#include "terrarium_app.hpp"

#include "terrarium_patch.hpp"
#include "terrarium_render.hpp"
#include "terrarium_version.hpp"

void clampCameraToWorld() {
  int viewW = std::max(1, W / std::max(1, g_zoom));
  int viewH = std::max(1, H / std::max(1, g_zoom));
  g_camX = clampi(g_camX, 0, std::max(0, W - viewW));
  g_camY = clampi(g_camY, 0, std::max(0, H - viewH));
}

void updateRipples(float dt) {
  for (auto& ripple : g_ripples) ripple.t += dt;
  g_ripples.erase(
      std::remove_if(g_ripples.begin(), g_ripples.end(),
                     [](const Ripple& ripple) { return ripple.t > 3.0f; }),
      g_ripples.end());
}

std::string defaultSf2Path() {
#ifdef _WIN32
  return "";
#else
  const char* candidates[] = {
      "/usr/share/sounds/sf2/FluidR3_GM.sf2",
      "/usr/share/soundfonts/FluidR3_GM.sf2",
      "/usr/share/sounds/sf2/TimGM6mb.sf2",
      "/usr/share/sounds/sf2/GeneralUser_GS.sf2",
  };
  for (auto* path : candidates) {
    FILE* file = std::fopen(path, "rb");
    if (file) {
      std::fclose(file);
      return std::string(path);
    }
  }
  return std::string();
#endif
}

int menuSelectionCount(bool showMenu, int menuPage,
                       const std::vector<MidiParam>& params,
                       const World& world) {
  if (!showMenu) {
    return std::max(1, static_cast<int>(params.size()));
  }

  switch (menuPage % kMenuPageCount) {
    case 0:
      return std::max(1, static_cast<int>(params.size()));
    case 1:
      return kChaosWeightRowCount;
    case 4:
      return NUM_VOICES * 3;
    case 5:
      return NUM_VOICES + 1;
    case 6:
      return std::max(1, static_cast<int>(world.agents.size()));
    case 7:
      return MOD_N;
    default:
      return 1;
  }
}

void cycleMenuSelection(int& menuSel, int maxSel, int delta) {
  maxSel = std::max(1, maxSel);
  if (delta < 0) {
    menuSel = (menuSel + maxSel - 1) % maxSel;
  } else {
    menuSel = (menuSel + 1) % maxSel;
  }
}

void adjustChaosWeight(int menuSel, float delta) {
  std::array<float*, kChaosWeightRowCount> rows = {
      &g_alea.chaos,       &g_alea.rainChance, &g_alea.spawnChance,
      &g_alea.mutationRate, &g_alea.drift,     &g_alea.noteLen,
      &g_alea.holdChance};
  constexpr std::array<float, kChaosWeightRowCount> kMin = {
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.10f, 0.0f};
  constexpr std::array<float, kChaosWeightRowCount> kMax = {
      2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.50f, 0.50f};

  const int sel = clampi(menuSel, 0, kChaosWeightRowCount - 1);
  *rows[sel] = std::clamp(*rows[sel] + delta, kMin[sel], kMax[sel]);
  g_patchDirty = true;
}

void adjustVoiceSettings(int menuSel, int delta) {
  const int sel = clampi(menuSel, 0, NUM_VOICES * 3 - 1);
  const int voice = sel / 3;
  const int field = sel % 3;

  if (field == 0) {
    g_voice[voice].program = clampi(g_voice[voice].program + delta, 0, 127);
    g_voiceProgManual[voice] = true;
  } else if (field == 1) {
    g_voice[voice].minNote = clampi(g_voice[voice].minNote + delta, 0, 127);
  } else {
    g_voice[voice].maxNote = clampi(g_voice[voice].maxNote + delta, 0, 127);
  }

  if (g_voice[voice].maxNote < g_voice[voice].minNote) {
    g_voice[voice].maxNote = g_voice[voice].minNote;
  }
  g_patchDirty = true;
}

void adjustMixerLevel(SynthOut& synth, int menuSel, float delta) {
  const int rows = NUM_VOICES + 1;
  const int sel = clampi(menuSel, 0, rows - 1);

  if (sel < NUM_VOICES) {
    if (delta > 0.0f) g_voiceMute[sel] = false;
    g_voiceFader[sel] = std::clamp(g_voiceFader[sel] + delta, 0.0f, 1.0f);
    if (g_voiceFader[sel] <= kMixerMuteThreshold) g_voiceMute[sel] = true;
  } else {
    if (delta > 0.0f) g_drumsMute = false;
    g_drumsFader = std::clamp(g_drumsFader + delta, 0.0f, 1.0f);
    if (g_drumsFader <= kMixerMuteThreshold) g_drumsMute = true;
  }

  g_patchDirty = true;
  applyVoiceMixer(synth);
}

void toggleMixerMuteSelection(SynthOut& synth, int menuSel) {
  const int rows = NUM_VOICES + 1;
  const int sel = clampi(menuSel, 0, rows - 1);
  if (sel < NUM_VOICES) {
    g_voiceMute[sel] = !g_voiceMute[sel];
  } else {
    g_drumsMute = !g_drumsMute;
  }
  g_patchDirty = true;
  applyVoiceMixer(synth);
}

void toggleMixerSoloSelection(SynthOut& synth, int menuSel) {
  const int rows = NUM_VOICES + 1;
  const int sel = clampi(menuSel, 0, rows - 1);

  if (g_soloRow == sel) {
    g_soloRow = -1;
    for (int voice = 0; voice < NUM_VOICES; ++voice) {
      g_voiceFader[voice] = g_savedVoiceFader[voice];
      g_voiceMute[voice] = g_savedVoiceMute[voice];
    }
    g_drumsFader = g_savedDrumsFader;
    g_drumsMute = g_savedDrumsMute;
  } else {
    g_soloRow = sel;
    for (int voice = 0; voice < NUM_VOICES; ++voice) {
      g_savedVoiceFader[voice] = g_voiceFader[voice];
      g_savedVoiceMute[voice] = g_voiceMute[voice];
    }
    g_savedDrumsFader = g_drumsFader;
    g_savedDrumsMute = g_drumsMute;

    for (int voice = 0; voice < NUM_VOICES; ++voice) {
      g_voiceMute[voice] = (voice != sel);
    }
    g_drumsMute = (sel != NUM_VOICES);
  }

  applyVoiceMixer(synth);
}

void startBiomeMorph(World& world, Rng& r) {
  world.targetBiome = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
  world.biomeMorphActive = true;
  world.biomeMorphT = 0.0f;
  world.bwFrom = world.bw;
  world.bwTo = weightsFor(world.targetBiome);
  world.biome = world.targetBiome;
  initClouds(world.clouds, r, world.biome);
  world.biomeFadeDir = 0;
  world.biomeFade = 0.0f;
}

void stepSimulationOnce(World& world, Rng& r, std::string& banner, int& tick,
                        SynthOut& synth, int& heldNote, int& heldNote2,
                        int& heldNote3, int rootKey, ScaleType scaleType,
                        const std::vector<MidiParam>& params) {
  step(world, r, banner, tick);
  tick++;
  synthTickMusic(synth, world, r, tick, heldNote, heldNote2, heldNote3,
                 rootKey, scaleType, params);
  // synthTickMusic consumes step events only when the synth is enabled and it
  // reaches the micro-event block; drain here so the queue can never grow
  // without bound when audio is off.
  g_stepEvents.clear();
}

void advanceBiomeFade(World& world, Rng& r) {
  // Gradually blend growth weights toward the target biome after a morph.
  // Without this, pressing B changed the biome label but the world kept
  // growing with the old biome's weights until the next reseed.
  if (world.biomeMorphActive) {
    world.biomeMorphT = std::min(1.0f, world.biomeMorphT + 0.0025f);
    world.bw = lerpBiomeWeights(world.bwFrom, world.bwTo, world.biomeMorphT);
    if (world.biomeMorphT >= 1.0f) {
      world.biomeMorphActive = false;
      world.bw = world.bwTo;
    }
  }

  if (world.biomeFadeDir == 0) return;

  world.biomeFade += 0.02f * (float)world.biomeFadeDir;
  if (world.biomeFade >= 1.0f) {
    world.biomeFade = 0.0f;
    world.biomeFadeDir = 0;
    seedWorld(world, r, world.targetBiome);
  } else if (world.biomeFade <= 0.0f) {
    world.biomeFade = 0.0f;
    world.biomeFadeDir = 0;
  }
}

void followSelectedAgent(const World& world) {
  if (!g_followInspect || g_inspectIdx < 0 ||
      g_inspectIdx >= static_cast<int>(world.agents.size())) {
    return;
  }

  const int viewW = std::max(1, W / std::max(1, g_zoom));
  const int viewH = std::max(1, H / std::max(1, g_zoom));
  const Agent& agent = world.agents[g_inspectIdx];
  g_camX = clampi(agent.x - viewW / 2, 0, std::max(0, W - viewW));
  g_camY = clampi(agent.y - viewH / 2, 0, std::max(0, H - viewH));
}

void handleWorldClick(World& world, Rng& r, const Layout& layout, int mouseX,
                      int mouseY) {
  if (mouseY < 0 || mouseY >= layout.simHpx) return;

  const int viewW = std::max(1, W / std::max(1, g_zoom));
  const int viewH = std::max(1, H / std::max(1, g_zoom));
  const int sx =
      (int)((int64_t)mouseX * viewW / std::max(1, layout.screenW));
  const int sy =
      (int)((int64_t)mouseY * viewH / std::max(1, layout.simHpx));
  const int wx = clampi(g_camX + sx, 0, W - 1);
  const int wy = clampi(g_camY + sy, 0, H - 1);

  g_inspectIdx = -1;
  for (int i = 0; i < (int)world.agents.size(); ++i) {
    if (world.agents[i].x == wx && world.agents[i].y == wy) {
      g_inspectIdx = i;
      break;
    }
  }

  Ripple ripple;
  ripple.cx = wx;
  ripple.cy = wy;
  ripple.amp = 3.0f + 5.0f * (float)r.u01();
  ripple.speed = 16.0f + 18.0f * (float)r.u01();
  ripple.width = 2.0f + 2.5f * (float)r.u01();
  ripple.chaos = 0.5f + 0.8f * (float)r.u01();
  g_ripples.push_back(ripple);
}

std::string buildWindowTitle(const World& world, int tick, bool paused, int tps,
                             const std::string& banner) {
  const Season season = seasonAt(tick);
  return std::string(terrarium::kDisplayName) + " | biome " +
         biomeName(world.biome) + " | " + std::to_string(W) + "x" +
         std::to_string(H) + " | tick " + std::to_string(tick) + " | " +
         (paused ? "PAUSED" : ("tps " + std::to_string(tps))) + " | " +
         seasonName(season) + " | weather " +
         weatherName(world.weather.state) + " (" +
         std::to_string((int)(world.weather.rainStrength * 100)) + "%)" +
         " | wind " + std::to_string(world.wind.strength) + " | " + banner +
         " | SPACE pause  . step  [ ] speed  r reset  F11 fullscreen  ESC quit";
}

void sendChangedMidiParams(MidiOut& midi, std::vector<MidiParam>& params,
                           uint32_t nowMs, uint32_t& lastParamSendMs) {
  if (!midi.enabled || (nowMs - lastParamSendMs) < kMidiParamSendIntervalMs) {
    return;
  }

  lastParamSendMs = nowMs;
  for (auto& param : params) {
    if (param.cc < 0) continue;
    if (param.lastSent01 < 0.0f ||
        std::abs(param.lastSent01 - param.value01) > 0.01f) {
      midi.sendCC(0, param.cc, (int)std::lround(param.value01 * 127.0f));
      param.lastSent01 = param.value01;
    }
  }
}

void pumpMidiClock(MidiOut& midi, bool midiClockOut, bool useSimClock,
                   const TelemetrySnapshot& telemetry, uint32_t nowMs,
                   uint32_t& lastClockMs) {
  if (!midi.enabled || !midiClockOut || !useSimClock) return;

  const float bpm =
      90.0f + 60.0f * (0.5f * telemetry.windMag + 0.5f * telemetry.rain01);
  const float msPerClock = (60000.0f / bpm) / 24.0f;
  if (lastClockMs == 0) lastClockMs = nowMs;

  while ((nowMs - lastClockMs) >= (uint32_t)msPerClock) {
    midi.sendClock();
    lastClockMs += (uint32_t)msPerClock;
  }
}
