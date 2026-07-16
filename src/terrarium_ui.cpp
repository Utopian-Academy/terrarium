#include "terrarium_ui.hpp"

#include "terrarium_midi_names.hpp"
#include "terrarium_visuals.hpp"

#include <cmath>
#include <cstdio>

namespace {

constexpr int kMenuOverlayPageCount = 9;
constexpr int kMenuPanelInsetPx = 8;
constexpr int kMenuTextInsetPx = 12;
constexpr int kMenuHeaderStepPx = 10;
constexpr int kMenuRowStepPx = 9;
constexpr int kMenuListRowStepPx = 10;
constexpr int kMenuBarOffsetXPx = 240;
constexpr int kMenuBarWidthPx = 200;
constexpr int kMenuBarHeightPx = 6;
constexpr int kMenuBarOffsetYPx = 2;
constexpr int kInspectVisibleRows = 14;
constexpr int kVisibleModRows = 14;

struct MenuOverlayStats {
  int waterTiles = 0;
  int shallowTiles = 0;
  int deepTiles = 0;
};

const char* menuSeasonName(Season season) {
  switch (season) {
    case SPRING:
      return "SPRING";
    case SUMMER:
      return "SUMMER";
    case AUTUMN:
      return "AUTUMN";
    case WINTER:
      return "WINTER";
  }
  return "?";
}

const char* menuBiomeName(Biome biome) {
  switch (biome) {
    case MEADOW:
      return "MEADOW";
    case WETLAND:
      return "WETLAND";
    case DESERT:
      return "DESERT";
    case TROPICAL:
      return "TROPICAL";
    case ALPINE:
      return "ALPINE";
    case ALIEN:
      return "ALIEN";
  }
  return "?";
}

const char* menuPageName(int page, UiLang uiLang) {
  static const char* kMidiPage = "MIDI";
  static const char* kWaterPage = "WATER";
  static const char* kSpawnsPage = "SPAWNS";
  static const char* kAudioPage = "AUDIO";
  static const char* kVoicesPage = "VOICES";
  static const char* kMixerPage = "MIXER";
  static const char* kInspectPage = "INSPECT";
  static const char* kModsPage = "MODS";
  static const char* kModMapPage = "MODMAP";
  static const char kKataMizu[] = {(char)0x80, (char)0x81, 0};
  static const char kKataSpawn[] = {(char)0x84, (char)0x85, (char)0x87,
                                    (char)0x86, 0};
  static const char kKataOto[] = {(char)0x82, (char)0x83, 0};

  switch (page % kMenuOverlayPageCount) {
    case 0:
      return kMidiPage;
    case 1:
      return (uiLang == UI_KATA) ? kKataMizu : kWaterPage;
    case 2:
      return (uiLang == UI_KATA) ? kKataSpawn : kSpawnsPage;
    case 3:
      return (uiLang == UI_KATA) ? kKataOto : kAudioPage;
    case 4:
      return kVoicesPage;
    case 5:
      return kMixerPage;
    case 6:
      return kInspectPage;
    case 7:
      return kModsPage;
    case 8:
      return kModMapPage;
  }
  return "?";
}

MenuOverlayStats collectMenuWaterStats(const World& world) {
  MenuOverlayStats stats;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint8_t depth = world.water[y][x];
      if (depth <= 0) continue;
      ++stats.waterTiles;
      if (depth <= 2) ++stats.shallowTiles;
      if (depth >= 5) ++stats.deepTiles;
    }
  }
  return stats;
}

RGB menuRowColor(bool selected) {
  return selected ? RGB{255, 240, 210} : RGB{200, 200, 200};
}

void drawMenuLine(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x, int y,
                  const std::string& text, const RGB& color, int scale,
                  uint8_t alpha = 230) {
  drawString(renderer, textGlyphs, x, y, text, color.r, color.g, color.b, alpha,
             scale);
}

void drawMenuBar(SDL_Renderer* renderer, int x, int y, int scale, float fill01,
                 const RGB& fillColor) {
  SDL_Rect bar{x + kMenuBarOffsetXPx * scale, y + kMenuBarOffsetYPx * scale,
               kMenuBarWidthPx * scale, kMenuBarHeightPx * scale};
  setColor(renderer, 60, 60, 60, 200);
  SDL_RenderFillRect(renderer, &bar);

  SDL_Rect fill = bar;
  fill.w = (int)(bar.w * clamp01(fill01));
  setColor(renderer, fillColor.r, fillColor.g, fillColor.b, 200);
  SDL_RenderFillRect(renderer, &fill);
}

void drawMenuBarLine(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x, int y,
                     const std::string& text, bool selected, float fill01,
                     int scale) {
  RGB color = menuRowColor(selected);
  drawMenuLine(renderer, textGlyphs, x, y, text, color, scale);
  drawMenuBar(renderer, x, y, scale, fill01, color);
}

void drawMenuSummaryLine(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                         int y, int scale, const World& world, int menuPage,
                         bool synthEnabled, UiLang uiLang, Season season,
                         const MenuOverlayStats& stats) {
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "[TAB] Page:%s   Biome:%s   Season:%s   Wind:(%d,%d) s=%d   Weather:%d   Water:%d%%   Synth:%s",
      menuPageName(menuPage, uiLang), menuBiomeName(world.biome),
      menuSeasonName(season), world.wind.dx, world.wind.dy,
      world.wind.strength, (int)world.weather.state,
      (int)(100.0f * (float)stats.waterTiles / (float)(W * H)),
      synthEnabled ? "ON" : "OFF");
  drawMenuLine(renderer, textGlyphs, x, y, buf, RGB{240, 240, 240}, scale);
}

void drawLegendaryCoupleLine(SDL_Renderer* renderer, GlyphCache& textGlyphs,
                             int x, int y, int scale, const World& world) {
  int legendaryA = 0;
  int legendaryB = 0;
  for (const auto& agent : world.agents) {
    if (agent.flags & AGF_LEGEND_A) ++legendaryA;
    if (agent.flags & AGF_LEGEND_B) ++legendaryB;
  }

  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "Legendary Couple: %s%s  (click ripples to stir fate)",
                legendaryA ? "y/Y " : "(missing A) ",
                legendaryB ? "z/Z" : "(missing B)");
  drawMenuLine(renderer, textGlyphs, x, y, buf, RGB{240, 210, 140}, scale);
}

void renderMidiMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                        int y, int scale,
                        const std::vector<MidiParam>& params, int menuSel) {
  drawMenuLine(
      renderer, textGlyphs, x, y,
      "MIDI/CC (UP/DOWN select, +/- edit, O MIDI, C clock, V clock src, K key, S scale, M menu)",
      RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  for (int i = 0; i < (int)params.size(); ++i) {
    const auto& param = params[i];
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s  CC:%d  w:%.2f  v:%.2f", param.name,
                  param.cc, param.weight, param.value01);
    drawMenuBarLine(renderer, textGlyphs, x, y + i * kMenuRowStepPx * scale,
                    buf, i == menuSel, param.value01, scale);
  }
}

void renderChaosMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                         int y, int scale, int menuSel,
                         const MenuOverlayStats& stats) {
  drawMenuLine(renderer, textGlyphs, x, y,
               "CHAOS WEIGHTS (UP/DOWN select, +/- adjust)",
               RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  struct ChaosRow {
    const char* name;
    float* value;
    float low;
    float high;
  };
  ChaosRow rows[] = {
      {"Chaos", &g_alea.chaos, 0.0f, 2.0f},
      {"RainChance", &g_alea.rainChance, 0.0f, 2.0f},
      {"SpawnChance", &g_alea.spawnChance, 0.0f, 2.0f},
      {"Mutation", &g_alea.mutationRate, 0.0f, 2.0f},
      {"Drift", &g_alea.drift, 0.0f, 2.0f},
      {"NoteLen", &g_alea.noteLen, 0.10f, 2.50f},
      {"HoldChance", &g_alea.holdChance, 0.00f, 0.50f},
  };

  const int rowCount = (int)(sizeof(rows) / sizeof(rows[0]));
  const int selected = clampi(menuSel, 0, rowCount - 1);
  for (int i = 0; i < rowCount; ++i) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s  %.2f", rows[i].name, *rows[i].value);
    float fill01 = (*rows[i].value - rows[i].low) /
                   (rows[i].high - rows[i].low + 1e-6f);
    drawMenuBarLine(renderer, textGlyphs, x, y + i * kMenuRowStepPx * scale,
                    buf, i == selected, fill01, scale);
  }

  char buf[256];
  std::snprintf(
      buf, sizeof(buf),
      "Water tiles:%d  shallow:%d  deep:%d  (Scroll=zoom, click=ripple, WASD pan when zoomed)",
      stats.waterTiles, stats.shallowTiles, stats.deepTiles);
  drawMenuLine(renderer, textGlyphs, x,
               y + rowCount * kMenuRowStepPx * scale + 2 * scale, buf,
               RGB{180, 190, 200}, scale, 220);
}

void renderSpawnsMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                          int y, int scale, const World& world) {
  int whales = 0;
  int dinos = 0;
  int yetis = 0;
  int fish = 0;
  int cranes = 0;
  for (int yy = 0; yy < H; ++yy) {
    for (int xx = 0; xx < W; ++xx) {
      char entity = world.entities[yy][xx];
      if (entity == 'W') ++whales;
      if (entity == 'K') ++dinos;
      if (entity == 'H') ++yetis;
      if (entity == '>' || entity == '<') ++fish;
      if (entity == 'C') ++cranes;
    }
  }

  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "Whales(W):%d  Fish(</>):%d  Cranes(C):%d  Dinos(K):%d  Yetis(H):%d",
                whales, fish, cranes, dinos, yetis);
  drawMenuLine(renderer, textGlyphs, x, y, buf, RGB{220, 220, 220}, scale);
}

void renderAudioMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                         int y, int scale, const std::string& sf2Path) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "SoundFont: %s",
                sf2Path.empty() ? "(none)" : sf2Path.c_str());
  drawMenuLine(renderer, textGlyphs, x, y, buf, RGB{220, 220, 220}, scale);
  y += kMenuHeaderStepPx * scale;
  drawMenuLine(
      renderer, textGlyphs, x, y,
      "Use: --synth --sf2 <path.sf2> --gain <0..2>  (compile with -DUSE_FLUIDSYNTH)",
      RGB{180, 180, 180}, scale, 220);
}

void renderVoicesMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                          int y, int scale, int menuSel) {
  drawMenuLine(renderer, textGlyphs, x, y,
               "VOICES (UP/DOWN select, +/- adjust: Prog/Min/Max)",
               RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  const int rowCount = NUM_VOICES * 3;
  const int selected = clampi(menuSel, 0, rowCount - 1);
  for (int voice = 0; voice < NUM_VOICES; ++voice) {
    const int baseRow = voice * 3;
    char buf[256];

    std::snprintf(buf, sizeof(buf), "V%d Program: %d %s", voice,
                  g_voice[voice].program, gmProgramName(g_voice[voice].program));
    drawMenuLine(renderer, textGlyphs, x,
                 y + (baseRow + 0) * kMenuRowStepPx * scale, buf,
                 menuRowColor(baseRow + 0 == selected), scale);

    std::snprintf(buf, sizeof(buf), "V%d MinNote: %d", voice,
                  g_voice[voice].minNote);
    drawMenuLine(renderer, textGlyphs, x,
                 y + (baseRow + 1) * kMenuRowStepPx * scale, buf,
                 menuRowColor(baseRow + 1 == selected), scale);

    std::snprintf(buf, sizeof(buf), "V%d MaxNote: %d", voice,
                  g_voice[voice].maxNote);
    drawMenuLine(renderer, textGlyphs, x,
                 y + (baseRow + 2) * kMenuRowStepPx * scale, buf,
                 menuRowColor(baseRow + 2 == selected), scale);
  }

  drawMenuLine(renderer, textGlyphs, x,
               y + rowCount * kMenuRowStepPx * scale + 2 * scale,
               "Drums are on ch9 (weather-triggered).", RGB{180, 180, 180},
               scale, 220);
}

void renderMixerMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                         int y, int scale, int menuSel) {
  drawMenuLine(renderer, textGlyphs, x, y,
               "MIXER (UP/DOWN select, +/- level, M mute, S solo)",
               RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  const int rowCount = NUM_VOICES + 1;
  const int selected = clampi(menuSel, 0, rowCount - 1);
  for (int voice = 0; voice < NUM_VOICES; ++voice) {
    float level = g_voiceMute[voice] ? 0.0f : g_voiceFader[voice];
    char buf[128];
    std::snprintf(buf, sizeof(buf), "V%d Level: %.2f%s%s", voice, level,
                  g_voiceMute[voice] ? " (MUTE)" : "",
                  g_soloRow == voice ? " (SOLO)" : "");
    drawMenuBarLine(renderer, textGlyphs, x,
                    y + voice * kMenuRowStepPx * scale, buf,
                    voice == selected, level, scale);
  }

  const int drumRow = NUM_VOICES;
  const float drumLevel = g_drumsMute ? 0.0f : g_drumsFader;
  char drumBuf[128];
  std::snprintf(drumBuf, sizeof(drumBuf), "DRUMS Level: %.2f%s%s", drumLevel,
                g_drumsMute ? " (MUTE)" : "",
                g_soloRow == NUM_VOICES ? " (SOLO)" : "");
  drawMenuBarLine(renderer, textGlyphs, x,
                  y + drumRow * kMenuRowStepPx * scale, drumBuf,
                  drumRow == selected, drumLevel, scale);

  drawMenuLine(renderer, textGlyphs, x,
               y + rowCount * kMenuRowStepPx * scale + 2 * scale,
               "Tip: use solo to tune mappings, then blend voices live.",
               RGB{180, 180, 180}, scale, 220);
}

void renderInspectMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                           int y, int scale, const World& world) {
  drawMenuLine(renderer, textGlyphs, x, y,
               "INSPECT (click an agent, or UP/DOWN select; F follow)",
               RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  const int agentCount = (int)world.agents.size();
  if (agentCount <= 0) {
    drawMenuLine(renderer, textGlyphs, x, y, "(no agents)",
                 RGB{200, 200, 200}, scale);
    return;
  }

  g_inspectIdx = clampi(g_inspectIdx, 0, agentCount - 1);
  const int selected = g_inspectIdx;
  const Agent& agent = world.agents[selected];

  char buf[256];
  std::snprintf(buf, sizeof(buf), "#%d '%c'  (%d,%d)  %s", selected,
                agent.glyph, agent.x, agent.y, speciesName(agent.species));
  drawMenuLine(renderer, textGlyphs, x, y, buf, RGB{240, 240, 210}, scale);
  y += kMenuHeaderStepPx * scale;

  std::snprintf(buf, sizeof(buf),
                "hp %3d  stress %3d  hunger %3d  thirst %3d  fatigue %3d",
                (int)std::lround(100.0f * agent.health),
                (int)std::lround(100.0f * agent.stress),
                (int)std::lround(100.0f * agent.hunger),
                (int)std::lround(100.0f * agent.thirst),
                (int)std::lround(100.0f * agent.fatigue));
  drawMenuLine(renderer, textGlyphs, x, y, buf, RGB{210, 210, 220}, scale);
  y += kMenuHeaderStepPx * scale;

  int start = std::max(0, selected - kInspectVisibleRows / 2);
  start = clampi(start, 0, std::max(0, agentCount - kInspectVisibleRows));
  for (int i = 0; i < kInspectVisibleRows && (start + i) < agentCount; ++i) {
    const int agentIndex = start + i;
    const Agent& rowAgent = world.agents[agentIndex];
    std::snprintf(buf, sizeof(buf),
                  "%4d '%c' (%3d,%3d) %s st%3d hu%3d th%3d", agentIndex,
                  rowAgent.glyph, rowAgent.x, rowAgent.y,
                  speciesName(rowAgent.species),
                  (int)std::lround(100.0f * rowAgent.stress),
                  (int)std::lround(100.0f * rowAgent.hunger),
                  (int)std::lround(100.0f * rowAgent.thirst));
    drawMenuLine(renderer, textGlyphs, x,
                 y + i * kMenuRowStepPx * scale, buf,
                 agentIndex == selected ? RGB{255, 240, 210}
                                        : RGB{180, 180, 180},
                 scale);
  }
}

void renderModsMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                        int y, int scale) {
  drawMenuLine(renderer, textGlyphs, x, y,
               "MODS (50 spiky bipolar signals)  (UP/DOWN scroll)",
               RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  g_g_modScroll = std::clamp(g_g_modScroll, 0, std::max(0, MOD_N - 14));
  const int rowCount = std::min(kVisibleModRows, MOD_N - g_g_modScroll);
  for (int i = 0; i < rowCount; ++i) {
    int modIndex = g_g_modScroll + i;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%2d %-14s %+.3f", modIndex,
                  g_modName[modIndex], g_modVal[modIndex]);
    drawMenuLine(renderer, textGlyphs, x,
                 y + i * kMenuListRowStepPx * scale, buf,
                 RGB{210, 210, 220}, scale);
  }
}

void renderModMapMenuPage(SDL_Renderer* renderer, GlyphCache& textGlyphs, int x,
                          int y, int scale) {
  drawMenuLine(
      renderer, textGlyphs, x, y,
      "MODMAP (UP/DOWN slot, LEFT/RIGHT field, +/- edit, E enable)",
      RGB{180, 180, 180}, scale, 220);
  y += kMenuHeaderStepPx * scale;

  g_g_mmSel = clampi(g_g_mmSel, 0, MOD_SLOTS - 1);
  g_g_mmField = clampi(g_g_mmField, 0, 4);
  for (int i = 0; i < MOD_SLOTS; ++i) {
    const ModMap& mapping = g_modMap[i];
    char destBuf[24];
    if (mapping.dest == DEST_MIDI_CC) {
      std::snprintf(destBuf, sizeof(destBuf), "MIDI CC%03d", mapping.cc);
    } else {
      std::snprintf(destBuf, sizeof(destBuf), "%s", modDestName(mapping.dest));
    }
    char buf[196];
    std::snprintf(buf, sizeof(buf), "%2d %c src:%02d %-12s amt:%+0.2f sm:%0.2f",
                  i, mapping.enabled ? '*' : ' ', mapping.src, destBuf,
                  mapping.amt, mapping.smooth);
    drawMenuLine(renderer, textGlyphs, x,
                 y + i * kMenuListRowStepPx * scale, buf,
                 i == g_g_mmSel ? RGB{255, 255, 220} : RGB{200, 200, 220},
                 scale);

    if (i == g_g_mmSel) {
      static const int kFieldCaretX[5] = {30, 92, 170, 230, 120};
      int caretX = x + kFieldCaretX[g_g_mmField] * scale / 2;
      drawMenuLine(renderer, textGlyphs, caretX,
                   y + i * kMenuListRowStepPx * scale, "^",
                   RGB{255, 220, 120}, scale);
    }
  }

  drawMenuLine(renderer, textGlyphs, x,
               y + MOD_SLOTS * kMenuListRowStepPx * scale + 4 * scale,
               "Map MODS -> CC11/CC74/Pan/Porta/MIDI CC (any external knob "
               "via MIDI-learn). 5th field = CC number.",
               RGB{150, 200, 255}, scale, 220);
}

}  // namespace

void renderMenuOverlay(SDL_Renderer* renderer, const Layout& layout, World& world,
                       GlyphCache& textGlyphs, int menuPage,
                       const std::vector<MidiParam>& params, int menuSel,
                       bool synthEnabled, const std::string& sf2Path,
                       UiLang uiLang, Season season) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_Rect panel{kMenuPanelInsetPx, kMenuPanelInsetPx,
                 layout.screenW - 2 * kMenuPanelInsetPx, 8 * layout.scale * 11};
  setColor(renderer, 0, 0, 0, 170);
  SDL_RenderFillRect(renderer, &panel);

  const int textX = kMenuTextInsetPx;
  int textY = kMenuTextInsetPx;
  const MenuOverlayStats stats = collectMenuWaterStats(world);

  drawMenuSummaryLine(renderer, textGlyphs, textX, textY, layout.scale, world,
                      menuPage, synthEnabled, uiLang, season, stats);
  textY += kMenuHeaderStepPx * layout.scale;
  drawLegendaryCoupleLine(renderer, textGlyphs, textX, textY, layout.scale,
                          world);
  textY += kMenuHeaderStepPx * layout.scale;

  switch (menuPage % kMenuOverlayPageCount) {
    case 0:
      renderMidiMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                         params, menuSel);
      break;
    case 1:
      renderChaosMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                          menuSel, stats);
      break;
    case 2:
      renderSpawnsMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                           world);
      break;
    case 3:
      renderAudioMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                          sf2Path);
      break;
    case 4:
      renderVoicesMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                           menuSel);
      break;
    case 5:
      renderMixerMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                          menuSel);
      break;
    case 6:
      renderInspectMenuPage(renderer, textGlyphs, textX, textY, layout.scale,
                            world);
      break;
    case 7:
      renderModsMenuPage(renderer, textGlyphs, textX, textY, layout.scale);
      break;
    case 8:
      renderModMapMenuPage(renderer, textGlyphs, textX, textY, layout.scale);
      break;
  }
}
