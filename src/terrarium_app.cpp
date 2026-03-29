#include "terrarium_app.hpp"

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

std::vector<MidiParam> makeDefaultMidiParams() {
  return {
      {MIDI_PARAM_WATER, "Water", 20, 1.0f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_RAIN, "Rain", 21, 1.0f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_WIND, "Wind", 22, 1.0f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_SEASON, "Season", 23, 0.6f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_BIOME, "Biome", 24, 0.6f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_FLORA, "Flora", 25, 1.0f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_FAUNA, "Fauna", 26, 1.0f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_INSTR, "Instr", 27, 1.0f, 0.0f, 0.0f, -1.0f},
      {MIDI_PARAM_AUTOKEY, "AutoKey", 28, 1.0f, 1.0f, 1.0f, -1.0f},
      {MIDI_PARAM_AUTOSCALE, "AutoScale", 29, 1.0f, 1.0f, 1.0f, -1.0f},
  };
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
}

void advanceBiomeFade(World& world, Rng& r) {
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

TelemetrySnapshot collectTelemetry(const World& world, int tick) {
  TelemetrySnapshot telemetry{};
  telemetry.windMag =
      std::min(1.0f,
               (std::abs((float)world.wind.dx) +
                std::abs((float)world.wind.dy)) /
                   6.0f);
  telemetry.rain01 = clamp01(world.weather.rainStrength);

  constexpr int kSampleStep = 4;
  int waterSum = 0;
  int waterCount = 0;
  int floraCount = 0;
  int faunaCount = 0;
  int cellCount = 0;
  for (int y = 0; y < H; y += kSampleStep) {
    for (int x = 0; x < W; x += kSampleStep) {
      waterSum += (int)world.water[y][x];
      waterCount++;
      char terrain = world.terrain[y][x];
      char entity = world.entities[y][x];
      if (terrain != '.' && terrain != '^' && terrain != ' ') floraCount++;
      if (entity != ' ') faunaCount++;
      cellCount++;
    }
  }

  telemetry.water01 =
      clamp01((waterCount ? (float)waterSum / (float)waterCount : 0.0f) / 8.0f);
  telemetry.flora01 =
      clamp01(cellCount ? (float)floraCount / (float)cellCount : 0.0f);
  telemetry.fauna01 =
      clamp01(cellCount ? (float)faunaCount / (float)cellCount : 0.0f);
  telemetry.season01 = (float)((int)seasonAt(tick)) / 3.0f;
  telemetry.biome01 = (float)((int)world.biome) / 5.0f;
  return telemetry;
}

void refreshMidiParamValues(std::vector<MidiParam>& params) {
  for (auto& param : params) {
    param.value01 = clamp01(param.rawValue01 * param.weight);
  }
}

void updateTelemetryParams(std::vector<MidiParam>& params,
                           const TelemetrySnapshot& telemetry) {
  if (params.size() > MIDI_PARAM_WATER) {
    params[MIDI_PARAM_WATER].rawValue01 = telemetry.water01;
  }
  if (params.size() > MIDI_PARAM_RAIN) {
    params[MIDI_PARAM_RAIN].rawValue01 = telemetry.rain01;
  }
  if (params.size() > MIDI_PARAM_WIND) {
    params[MIDI_PARAM_WIND].rawValue01 = telemetry.windMag;
  }
  if (params.size() > MIDI_PARAM_SEASON) {
    params[MIDI_PARAM_SEASON].rawValue01 = telemetry.season01;
  }
  if (params.size() > MIDI_PARAM_BIOME) {
    params[MIDI_PARAM_BIOME].rawValue01 = telemetry.biome01;
  }
  if (params.size() > MIDI_PARAM_FLORA) {
    params[MIDI_PARAM_FLORA].rawValue01 = telemetry.flora01;
  }
  if (params.size() > MIDI_PARAM_FAUNA) {
    params[MIDI_PARAM_FAUNA].rawValue01 = telemetry.fauna01;
  }
  refreshMidiParamValues(params);
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
