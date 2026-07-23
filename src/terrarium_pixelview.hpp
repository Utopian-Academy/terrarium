// One-pixel-per-cell world rendering, shared by the pico build and the
// plugin UI: cheap enough for a Pi Zero (or a host's audio thread),
// detailed enough to watch the vat live.
//
// SDL-free on purpose — consumers pack the returned triple into whatever
// pixel format their surface wants (pico: ARGB texture, plugin UI: RGBA GL).
#pragma once

#include "terrarium_core.hpp"

struct PixelviewRGB {
  uint8_t r = 0, g = 0, b = 0;
};

// Nearest-neighbour cloud sample (the full app does bilinear; not needed at
// 1px/cell).
inline uint8_t pixelviewCloudAt(const World& w, int x, int y) {
  int cx = ((int)(((float)x / (float)W) * CW + w.clouds.offX)) % CW;
  int cy = ((int)(((float)y / (float)H) * CH + w.clouds.offY)) % CH;
  if (cx < 0) cx += CW;
  if (cy < 0) cy += CH;
  uint8_t c = w.clouds.field[cy * CW + cx];
  return (uint8_t)std::min<int>(255, (int)(c * w.cloudOpacity));
}

// Shaded per-cell color: entities, overlays, water, terrain, then season,
// cloud shadow, lightning, and the day/night cycle.
// animT: seconds for water motion; pass wall-clock time for smooth flow
// between sim ticks (defaults to tick-derived time when negative).
inline PixelviewRGB pixelviewCellColor(const World& w, int x, int y, int tick,
                                       float animT = -1.0f) {
  if (animT < 0.0f) animT = (float)tick * 0.2f;
  // Small stable per-cell jitter so flat areas read as texture, not banding.
  uint32_t h = hash3((uint32_t)x, (uint32_t)y, w.worldSeed);
  int j = (int)(h & 15u) - 8;
  Season season = seasonAt(tick);
  bool snowy = (season == WINTER && w.biome != TROPICAL && w.biome != DESERT);

  int r = 0, g = 0, b = 0;
  bool tintable = false;  // terrain cells take the season grade

  char e = w.entities[y][x];
  char o = w.overlay[y][x];
  uint8_t d = w.water[y][x];
  char t = w.terrain[y][x];

  if (e != ' ') {
    // Agents: warm bright dots so they pop at one pixel.
    r = 255; g = 230; b = 160;
  } else if (o == '|' || o == '/' || o == '\\') {
    if (snowy) { r = 238; g = 242; b = 250; }  // winter rain falls as snow
    else       { r = 150; g = 190; b = 235; }  // rain streak
  } else if (o == '=' || o == '-' || o == '~' || o == '+' || o == '!') {
    // Rainbow bands, in actual rainbow order (was hashed confetti).
    switch (o) {
      case '=': r = 235; g = 70;  b = 60;  break;  // red
      case '-': r = 255; g = 150; b = 45;  break;  // orange
      case '~': r = 255; g = 220; b = 70;  break;  // yellow
      case '+': r = 95;  g = 205; b = 95;  break;  // green
      default:  r = 150; g = 110; b = 235; break;  // violet
    }
  } else if (o != ' ') {
    // Chaos overlays: hue from the glyph itself.
    uint32_t oh = hash3((uint32_t)o, 7u, 77u);
    r = 140 + (int)(oh & 0x7F); g = 140 + (int)((oh >> 7) & 0x7F);
    b = 140 + (int)((oh >> 14) & 0x7F);
  } else if (d > 0) {
    int dd = std::min<int>(7, d);
    r = 14 + dd + j / 2;
    g = 90 + dd * 12 + j;
    b = 150 + dd * 13 + j;
    // sparse foam shimmer
    if (((h >> 4) + (uint32_t)(tick / 6)) % 97u == 0u) { r = g = b = 235; }

    // Water in motion — stagnant reads dead outside wetland/oasis.
    // Rivers (steep gradient): light bands travel downhill. Open water:
    // surf — wave crests ride the height contours, so as time advances
    // they sweep toward the shallows and break white where they land.
    if (w.biome != WETLAND && w.biome != DESERT) {
      int gx = 0, gy = 0;
      if (x > 0 && x < W - 1) gx = (int)w.height[y][x-1] - (int)w.height[y][x+1];
      if (y > 0 && y < H - 1) gy = (int)w.height[y-1][x] - (int)w.height[y+1][x];
      bool river = (std::abs(gx) + std::abs(gy) >= 3) && d <= 3;

      if (river) {
        float fx = (float)((gx > 0) - (gx < 0));
        float fy = (float)((gy > 0) - (gy < 0));
        float ph = 0.8f * ((float)x * fx + (float)y * fy) - animT * 2.8f;
        float ripple = std::sin(ph + 0.7f * std::sin(ph * 0.37f + (float)(h & 7u)));
        int lift = (int)(ripple * 14.f);
        r += lift / 2; g += lift; b += lift;
        if (ripple > 0.92f) { r += 50; g += 55; b += 50; }  // whitewater glints
      } else {
        // Surf: primary crest follows height contours shoreward, a softer
        // wind swell crosses it so open water never looks striped.
        float shorePh = (float)w.height[y][x] * 0.55f - animT * 3.0f;
        float wx = (w.wind.dx == 0 && w.wind.dy == 0) ? 1.0f : (float)w.wind.dx;
        float wy = (w.wind.dx == 0 && w.wind.dy == 0) ? 0.3f : (float)w.wind.dy;
        float openPh = 0.35f * ((float)x * wx + (float)y * wy) - animT * 1.1f;
        float crest = std::sin(shorePh + 0.5f * std::sin(shorePh * 0.31f + (float)(h & 7u)));
        float swell = std::sin(openPh);
        float ripple = 0.7f * crest + 0.4f * swell;

        int lift = (int)(ripple * (d >= 4 ? 11.f : 16.f));
        r += lift / 2; g += lift; b += lift;

        // Breaking: near shore the crest goes foam-white, with a softer
        // spray band just behind it.
        bool nearShore = false;
        for (int oy = -1; oy <= 1 && !nearShore; ++oy)
          for (int ox = -1; ox <= 1; ++ox) {
            int nx2 = x + ox, ny2 = y + oy;
            if (nx2 < 0 || ny2 < 0 || nx2 >= W || ny2 >= H) continue;
            if (w.water[ny2][nx2] == 0) { nearShore = true; break; }
          }
        if (nearShore && crest > 0.55f) {
          float f = (crest - 0.55f) / 0.45f;  // 0..1 into the break
          r = (int)(r + (228 - r) * f);
          g = (int)(g + (238 - g) * f);
          b = (int)(b + (246 - b) * f);
        } else if (d <= 2 && crest > 0.80f) {
          r += 35; g += 40; b += 38;  // shallow crest sparkle offshore
        }
      }
    }
  } else {
    tintable = true;
    // Foliage picks a per-cell green *family* (yellow-green, deep forest,
    // blue-green, sage) — jitter alone read as one flat green.
    uint32_t fam = (h >> 6) & 3u;
    switch (t) {
      case ',':
        switch (fam) {
          case 0:  r = 60 + j; g = 140 + j; b = 70; break;
          case 1:  r = 82 + j; g = 148 + j; b = 56; break;   // yellow-green
          case 2:  r = 48 + j; g = 130 + j; b = 92; break;   // blue-green
          default: r = 92 + j; g = 138 + j; b = 78; break;   // sage
        }
        break;
      case '"':
        switch (fam) {
          case 0:  r = 48 + j; g = 126 + j; b = 62; break;
          case 1:  r = 66 + j; g = 132 + j; b = 48; break;
          case 2:  r = 38 + j; g = 116 + j; b = 82; break;
          default: r = 74 + j; g = 122 + j; b = 66; break;
        }
        break;
      case ';':
        r = (fam & 1) ? 52 + j : 40 + j;
        g = (fam & 1) ? 118 + j : 110 + j;
        b = (fam & 1) ? 46 : 58;
        break;
      case '#':
        switch (fam) {
          case 0:  r = 36 + j; g = 96 + j; b = 52; break;
          case 1:  r = 58 + j; g = 102 + j; b = 40; break;   // olive
          case 2:  r = 30 + j; g = 88 + j; b = 66; break;    // teal shrub
          default: r = 48 + j; g = 94 + j; b = 56; break;
        }
        break;
      case ':': r = 66 + j; g = 150 + j; b = 96; break;
      case 'T': case 'Y': case 'P':
        switch (fam) {
          case 0:  r = 30; g = 84 + j; b = 40; break;        // forest
          case 1:  r = 24; g = 74 + j; b = 52; break;        // dark pine
          case 2:  r = 44; g = 92 + j; b = 36; break;        // warm canopy
          default: r = 34; g = 88 + j; b = 58; break;        // mossy
        }
        break;
      case 'm':  // forest-floor caps: cream / tan / fly-agaric red
        switch ((h >> 5) % 3u) {
          case 0:  r = 230 + j; g = 210; b = 185; break;
          case 1:  r = 195 + j; g = 160; b = 120; break;
          default: r = 205; g = 70 + j; b = 55; break;
        }
        break;
      case 'f': case '+': case '&': case '!':
        // Wildflower mix per-cell (was flat bubblegum pink/magenta):
        // poppy, marigold, orchid, white, cornflower.
        switch ((h >> 5) % 5u) {
          case 0:  r = 225; g = 70 + j; b = 55; break;
          case 1:  r = 255; g = 215 + j; b = 90; break;
          case 2:  r = 175; g = 120 + j; b = 235; break;
          case 3:  r = 248; g = 246; b = 238; break;
          default: r = 120; g = 190 + j; b = 250; break;
        }
        break;
      case '$': r = 220; g = 190 + j; b = 90; break;
      case 'd': case 'e': case 'g': r = 105 + j; g = 70; b = 44; break;
      case '^': case 'B': r = 130 + j; g = 130 + j; b = 142; break;
      case 'M': r = 150 + j; g = 165 + j; b = 190; break;
      case '*': r = 255; g = 120 + ((tick * 13 + (int)(h & 31)) % 80); b = 30; break;
      case 'x': r = 70; g = 40; b = 36; break;
      case 's': r = 205 + j; g = 185 + j; b = 130; break;
      case 'c': r = 90; g = 170 + j; b = 100; break;
      default:
        if (displayBgMode() == 1) { r = g = b = 0; }             // oled: true black
        else { r = 26 + j / 2; g = 22 + j / 2; b = 18; }         // earth
        break;
    }
    if (t == KELP_GLYPH) { r = 24; g = 140 + j; b = 110; }
  }

  // Depth when the world fills in: once vegetation covers everything the
  // scene loses its value contrast, so shape the land itself —
  // hillshade (NW light: facing slopes brighten, far slopes shadow) and
  // canopy occlusion (cells buried in tall growth darken; clump edges
  // stay lit, so forests read as sculpted masses).
  if (tintable) {
    int sgx = 0, sgy = 0;
    if (x > 0 && x < W - 1) sgx = (int)w.height[y][x+1] - (int)w.height[y][x-1];
    if (y > 0 && y < H - 1) sgy = (int)w.height[y+1][x] - (int)w.height[y-1][x];
    float hill = 1.0f + std::max(-0.16f, std::min(0.20f, (float)(-sgx - sgy) * 0.010f));

    int tall = 0;
    for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
      if (!ox && !oy) continue;
      int nx2 = x + ox, ny2 = y + oy;
      if (nx2 < 0 || ny2 < 0 || nx2 >= W || ny2 >= H) continue;
      char n = w.terrain[ny2][nx2];
      if (n == 'T' || n == 'Y' || n == 'P' || n == '#') ++tall;
    }
    bool selfTall = (t == 'T' || t == 'Y' || t == 'P' || t == '#');
    float ao = 1.0f - (selfTall ? 0.030f : 0.020f) * (float)tall;

    float shape = hill * ao;
    r = (int)(r * shape); g = (int)(g * shape); b = (int)(b * shape);
  }

  // Season grade on terrain: autumn browns the foliage, spring vivifies,
  // winter cools — plus a frost/snow dusting on open ground in winter.
  if (tintable) {
    if (season == AUTUMN) { r += 14; g -= 6; }
    else if (season == SPRING) { g += 8; }
    else if (snowy) {
      r = (int)(r * 0.90f) + 14; g = (int)(g * 0.92f) + 12; b += 22;
      // Snow settles on open ground and grass; trees and shrubs just take
      // the frost tint above (snowy pine forest, not white-out).
      bool ground = (t == ',' || t == '"' || t == ';' || t == '.' ||
                     t == ':' || t == 's');
      if (ground) {
        uint32_t sh = hash3((uint32_t)x, (uint32_t)y, 0x534E4F57u);
        float cover = 0.10f + 0.22f * seasonLerp(tick);
        if ((float)(sh & 1023u) / 1023.0f < cover) { r = 226; g = 232; b = 244; }
      }
    }
  }

  // Cloud shadow + smooth day/night cycle (dawn gold, dusk amber, cool
  // moonlit nights — never a hard brightness step).
  float shade = 1.0f - (pixelviewCloudAt(w, x, y) / 255.0f) * 0.35f;
  Daylight dl = daylightNow(tick);
  float bright = shade * (0.38f + 0.62f * dl.level) * displayBrightness();
  float rr = (float)r * bright * (1.f + 0.20f * dl.warm);
  float gg = (float)g * bright * (1.f + 0.04f * (dl.warm > 0.f ? dl.warm : 0.f));
  float bb = (float)b * bright * (1.f - 0.18f * dl.warm);

  // Storm lightning: single-tick global flashes (the sim's strikes were
  // invisible at 1px/cell — the whole sky flickering sells the storm).
  if (w.weather.state == STORM && (hash3((uint32_t)tick, 99u, 7u) % 19u) == 0u) {
    rr = rr * 1.5f + 70.f; gg = gg * 1.5f + 70.f; bb = bb * 1.4f + 60.f;
  }
  return PixelviewRGB{clampU8((int)rr), clampU8((int)gg), clampU8((int)bb)};
}
