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

// Shaded per-cell color: entities, overlays, water, terrain, then cloud
// shadow and the day/night cycle.
inline PixelviewRGB pixelviewCellColor(const World& w, int x, int y, int tick) {
  // Small stable per-cell jitter so flat areas read as texture, not banding.
  uint32_t h = hash3((uint32_t)x, (uint32_t)y, w.worldSeed);
  int j = (int)(h & 15u) - 8;

  int r = 0, g = 0, b = 0;

  char e = w.entities[y][x];
  char o = w.overlay[y][x];
  uint8_t d = w.water[y][x];
  char t = w.terrain[y][x];

  if (e != ' ') {
    // Agents: warm bright dots so they pop at one pixel.
    r = 255; g = 230; b = 160;
  } else if (o == '|' || o == '/' || o == '\\') {
    r = 150; g = 190; b = 235;  // rain streak
  } else if (o != ' ') {
    // Rainbow / chaos overlays: hue from the glyph itself.
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
  } else {
    switch (t) {
      case ',': r = 60 + j; g = 140 + j; b = 70; break;
      case '"': r = 48 + j; g = 126 + j; b = 62; break;
      case ';': r = 40 + j; g = 110 + j; b = 58; break;
      case '#': r = 36 + j; g = 96 + j; b = 52; break;
      case ':': r = 66 + j; g = 150 + j; b = 96; break;
      case 'T': case 'Y': case 'P': r = 30; g = 84 + j; b = 40; break;
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
      default:  r = 26 + j / 2; g = 22 + j / 2; b = 18; break;  // bare soil
    }
    if (t == KELP_GLYPH) { r = 24; g = 140 + j; b = 110; }
  }

  // Cloud shadow + gentle day/night cycle.
  float shade = 1.0f - (pixelviewCloudAt(w, x, y) / 255.0f) * 0.35f;
  if (nightish(tick)) shade *= 0.55f;
  return PixelviewRGB{clampU8((int)(r * shade)), clampU8((int)(g * shade)),
                      clampU8((int)(b * shade))};
}
