#include "terrarium_patch.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>  // _mkdir
#endif

std::string defaultPatchPath() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  if (!base) return "terrarium-patch.txt";
  return std::string(base) + "\\terrarium\\patch.txt";
#else
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/terrarium/patch.txt";
  const char* home = std::getenv("HOME");
  if (!home) return "terrarium-patch.txt";
  return std::string(home) + "/.config/terrarium/patch.txt";
#endif
}

static void ensureParentDir(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return;
  std::string dir = path.substr(0, slash);
#ifdef _WIN32
  _mkdir(dir.c_str());
#else
  // Create up to two levels (e.g. ~/.config/terrarium).
  size_t parent = dir.find_last_of('/');
  if (parent != std::string::npos) mkdir(dir.substr(0, parent).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
#endif
}

bool savePatch(const std::string& path, int rootKey, int scaleType) {
  ensureParentDir(path);
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;

  std::fprintf(f, "terrarium-patch 1\n");
  std::fprintf(f, "alea %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
               g_alea.chaos, g_alea.rainChance, g_alea.spawnChance,
               g_alea.mutationRate, g_alea.drift, g_alea.noteLen,
               g_alea.holdChance);
  std::fprintf(f, "key %d %d\n", rootKey, scaleType);
  for (int v = 0; v < NUM_VOICES; ++v) {
    std::fprintf(f, "voice %d %d %d %d %d %d %d %.3f %d\n", v,
                 g_voice[v].program, g_voice[v].bankMSB, g_voice[v].bankLSB,
                 g_voice[v].minNote, g_voice[v].maxNote, g_voice[v].transpose,
                 g_voice[v].velMul, g_voiceProgManual[v] ? 1 : 0);
    std::fprintf(f, "mixer %d %.4f %d\n", v, g_voiceFader[v],
                 g_voiceMute[v] ? 1 : 0);
  }
  std::fprintf(f, "drums %.4f %d\n", g_drumsFader, g_drumsMute ? 1 : 0);
  for (int i = 0; i < MOD_SLOTS; ++i) {
    const ModMap& m = g_modMap[i];
    std::fprintf(f, "slot %d %d %d %.4f %.4f %d %d\n", i, m.src, m.dest,
                 m.amt, m.smooth, m.enabled ? 1 : 0, m.cc);
  }
  std::fclose(f);
  return true;
}

bool loadPatch(const std::string& path, int& rootKey, int& scaleType) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;

  char line[512];
  bool valid = false;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "terrarium-patch", 15) == 0) {
      valid = true;
      continue;
    }
    if (!valid) break;

    if (std::strncmp(line, "alea ", 5) == 0) {
      std::sscanf(line + 5, "%f %f %f %f %f %f %f", &g_alea.chaos,
                  &g_alea.rainChance, &g_alea.spawnChance,
                  &g_alea.mutationRate, &g_alea.drift, &g_alea.noteLen,
                  &g_alea.holdChance);
    } else if (std::strncmp(line, "key ", 4) == 0) {
      int rk = rootKey, st = scaleType;
      if (std::sscanf(line + 4, "%d %d", &rk, &st) == 2) {
        rootKey = clampi(rk, 0, 11);
        scaleType = clampi(st, 0, 6);
      }
    } else if (std::strncmp(line, "voice ", 6) == 0) {
      int v, prog, msb, lsb, mn, mx, tr, manual;
      float vel;
      if (std::sscanf(line + 6, "%d %d %d %d %d %d %d %f %d", &v, &prog, &msb,
                      &lsb, &mn, &mx, &tr, &vel, &manual) == 9 &&
          v >= 0 && v < NUM_VOICES) {
        g_voice[v].program = clampi(prog, 0, 127);
        g_voice[v].bankMSB = clampi(msb, 0, 127);
        g_voice[v].bankLSB = clampi(lsb, 0, 127);
        g_voice[v].minNote = clampi(mn, 0, 127);
        g_voice[v].maxNote = clampi(mx, 0, 127);
        g_voice[v].transpose = clampi(tr, -48, 48);
        g_voice[v].velMul = std::clamp(vel, 0.0f, 2.0f);
        g_voiceProgManual[v] = manual != 0;
      }
    } else if (std::strncmp(line, "mixer ", 6) == 0) {
      int v, mute;
      float fader;
      if (std::sscanf(line + 6, "%d %f %d", &v, &fader, &mute) == 3 &&
          v >= 0 && v < NUM_VOICES) {
        g_voiceFader[v] = std::clamp(fader, 0.0f, 2.0f);
        g_voiceMute[v] = mute != 0;
      }
    } else if (std::strncmp(line, "drums ", 6) == 0) {
      int mute;
      float fader;
      if (std::sscanf(line + 6, "%f %d", &fader, &mute) == 2) {
        g_drumsFader = std::clamp(fader, 0.0f, 2.0f);
        g_drumsMute = mute != 0;
      }
    } else if (std::strncmp(line, "slot ", 5) == 0) {
      int i, src, dest, enabled, cc;
      float amt, smooth;
      if (std::sscanf(line + 5, "%d %d %d %f %f %d %d", &i, &src, &dest, &amt,
                      &smooth, &enabled, &cc) == 7 &&
          i >= 0 && i < MOD_SLOTS) {
        g_modMap[i].src = clampi(src, 0, MOD_N - 1);
        g_modMap[i].dest = clampi(dest, DEST_NONE, DEST_COUNT - 1);
        g_modMap[i].amt = std::clamp(amt, -2.0f, 2.0f);
        g_modMap[i].smooth = std::clamp(smooth, 0.0f, 0.98f);
        g_modMap[i].enabled = enabled != 0;
        g_modMap[i].cc = clampi(cc, 0, 127);
        g_modMap[i].state = 0.0f;
      }
    }
  }
  std::fclose(f);
  return valid;
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

void sendModMatrixMidi(MidiSink& midi) {
  if (!midi.enabled) return;
  static int lastSent[MOD_SLOTS] = {-1, -1, -1, -1, -1, -1,
                                    -1, -1, -1, -1, -1, -1};
  for (int i = 0; i < MOD_SLOTS; ++i) {
    if (g_modCC01[i] < 0.0f) {
      lastSent[i] = -1;
      continue;
    }
    const int val = cc127f(g_modCC01[i]);
    if (val != lastSent[i]) {
      midi.sendCC(0, g_modMap[i].cc, val);
      lastSent[i] = val;
    }
  }
}
