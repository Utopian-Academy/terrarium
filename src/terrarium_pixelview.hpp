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

// Ambient mote scheduler: per-cell time offsets so nothing blinks in sync,
// a sin^2 lifetime envelope so motes fade in and out instead of popping,
// and an individual flutter rate per mote. Returns 0..1 intensity, or 0
// when this cell has no mote in its current epoch (sparse is sexy).
inline float pixelviewMote(uint32_t h, float animT, float lifeSec,
                           uint32_t density, float flutterHz) {
  float t = (animT + (float)(h % 1024u) * 0.037f) / lifeSec;
  uint32_t epoch = (uint32_t)t;
  float frac = t - (float)epoch;
  uint32_t sh = hash3(h, epoch * 2654435761u, 0x4D4F5445u);
  if ((sh % density) != 0u) return 0.f;
  float env = std::sin(3.14159f * frac);
  env *= env;
  float rate = flutterHz * (0.7f + 0.6f * (float)((sh >> 3) & 15u) / 15.f);
  float fl = 0.75f + 0.25f * std::sin(animT * rate + (float)(sh & 63u));
  return env * fl;
}

// Shared ocean swell field: three wave components, angular shape noise,
// slow group envelope (waves arrive in sets). Island mode propagates
// radially inward; mainland follows the wind. Used by BOTH the deep and
// the shallows so sets roll continuously from open sea into the break.
inline float pixelviewSwell(const World& w, int x, int y, float animT,
                            uint32_t h, float* grpOut) {
  float base, ang;
  if (w.island) {
    float ccx = (float)W * 0.5f - 0.5f;
    float ddx = (float)x - ccx, ddy = (float)y - ccx;
    base = std::sqrt(ddx * ddx + ddy * ddy);
    ang = std::atan2(ddy, ddx);
  } else {
    float wx2 = (w.wind.dx == 0 && w.wind.dy == 0) ? 0.8f : (float)w.wind.dx;
    float wy2 = (w.wind.dx == 0 && w.wind.dy == 0) ? 0.5f : (float)w.wind.dy;
    base = (float)x * wx2 + (float)y * wy2;
    ang = 0.13f * (float)x - 0.11f * (float)y;
  }
  float s1 = std::sin(0.42f * base + animT * 1.9f +
                      1.3f * std::sin(ang * 3.f + animT * 0.20f));
  float s2 = std::sin(0.23f * base + animT * 1.15f +
                      1.7f * std::sin(ang * 5.f - animT * 0.13f));
  float s3 = std::sin(0.70f * base + animT * 2.6f + (float)(h & 7u) * 0.22f);
  float grp = 0.6f + 0.4f * std::sin(0.06f * base + animT * 0.45f +
                                     0.8f * std::sin(ang * 2.f));
  if (grpOut) *grpOut = grp;
  return (0.55f * s1 + 0.30f * s2 + 0.15f * s3) * grp;
}

// Island cast (ship, seabirds, whale): positions computed once per frame.
struct PixelviewCast {
  float t = -1e9f;
  float birdX[3], birdY[3];
  float shipX, shipY, shipAng;
  bool whaleUp = false;
  float whaleX, whaleY, whaleAge;
  bool serpentUp = false;
  float serpX[6], serpY[6];
};
inline PixelviewCast& pixelviewCast(float animT) {
  static PixelviewCast C;
  if (C.t != animT) {
    C.t = animT;
    float cc = (float)W * 0.5f - 0.5f;
    float R = (float)W * 0.5f;
    for (int i = 0; i < 3; ++i) {
      float a = animT * (0.09f + 0.02f * (float)i) + (float)i * 2.09f;
      float rad = R * (0.52f + 0.05f * std::sin(animT * 0.21f + (float)i));
      C.birdX[i] = cc + rad * std::cos(a);
      C.birdY[i] = cc + rad * std::sin(a);
    }
    C.shipAng = animT * 0.045f;
    C.shipX = cc + R * 0.85f * std::cos(C.shipAng);
    C.shipY = cc + R * 0.85f * std::sin(C.shipAng);
    uint32_t wep = (uint32_t)(animT / 75.f);
    uint32_t wh = hash3(wep, 0x37A1Eu, 0x1234u);
    C.whaleAge = animT - (float)wep * 75.f;
    C.whaleUp = ((wh % 3u) == 0u) && C.whaleAge < 14.f;
    if (C.whaleUp) {
      float wa = (float)((wh >> 4) & 1023u) / 1023.f * 6.283f;
      float wr = R * (0.68f + 0.14f * (float)((wh >> 14) & 255u) / 255.f);
      C.whaleX = cc + wr * std::cos(wa);
      C.whaleY = cc + wr * std::sin(wa);
    }
    // Sea serpent: rare, surfaces for half a minute, undulating humps.
    uint32_t sep2 = (uint32_t)(animT / 160.f);
    uint32_t sh3 = hash3(sep2, 0x5EA9u, 0x77u);
    float sAge = animT - (float)sep2 * 160.f;
    C.serpentUp = ((sh3 % 4u) == 0u) && sAge < 30.f;
    if (C.serpentUp) {
      float dir = ((sh3 >> 8) & 1u) ? 1.f : -1.f;
      float sa = (float)((sh3 >> 4) & 1023u) / 1023.f * 6.283f +
                 dir * sAge * 0.06f;
      float srad = R * (0.70f + 0.08f * std::sin(sAge * 0.35f));
      for (int i = 0; i < 6; ++i) {
        float aa = sa - dir * (float)i * 0.05f;
        float rr2 = srad + 1.6f * std::sin(sAge * 1.1f + (float)i * 1.3f);
        C.serpX[i] = cc + rr2 * std::cos(aa);
        C.serpY[i] = cc + rr2 * std::sin(aa);
      }
    }
  }
  return C;
}

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
    if (isAquatic(e)) {
      // Sea critters: silvery blue-white, so fish read as fish and not
      // as bananas bobbing offshore.
      r = 185; g = 220; b = 250;
    } else {
      // Land critters: warm bright dots so they pop at one pixel.
      r = 255; g = 230; b = 160;
    }
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
    // Layered blues, Surf Sandbox style: pale aqua shallows deepening to
    // rich navy (was inverted — deep water rendered brighter than shallow).
    int dd = std::min<int>(7, d);
    int sh = 7 - dd;  // shallowness 0..6
    r = 10 + sh * 4 + j / 2;
    g = 56 + sh * 14 + j;
    b = 118 + sh * 15 + j;
    // sparse foam shimmer
    if (((h >> 4) + (uint32_t)(tick / 6)) % 97u == 0u) { r = g = b = 235; }

    // Coral colonies glow through the shallow water.
    if (t == 'C' && d <= 3) {
      switch ((h >> 5) % 4u) {
        case 0:  r = 235; g = 110; b = 140; break;  // pink
        case 1:  r = 240; g = 140; b = 70;  break;  // orange
        case 2:  r = 175; g = 110; b = 220; break;  // violet
        default: r = 245; g = 205; b = 160; break;  // cream
      }
      r -= d * 14; g -= d * 8; b += d * 4;  // seen through the water
    }

    // Water in motion. Rivers (steep gradient) flow in EVERY biome — a
    // wetland bayou still runs even though its ponds sit glassy. Open
    // water gets contour surf except in the stillwater biomes
    // (wetland/desert), where flat water stays calm on purpose.
    {
      int gx = 0, gy = 0;
      if (x > 0 && x < W - 1) gx = (int)w.height[y][x-1] - (int)w.height[y][x+1];
      if (y > 0 && y < H - 1) gy = (int)w.height[y-1][x] - (int)w.height[y+1][x];
      // >=2 catches the gentle ~0.7/cell ramps of carved through-rivers,
      // not just steep mountain streams.
      bool river = (std::abs(gx) + std::abs(gy) >= 2) && d <= 3;
      bool stillBiome = (w.biome == WETLAND || w.biome == DESERT);

      if (river) {
        float fx = (float)((gx > 0) - (gx < 0));
        float fy = (float)((gy > 0) - (gy < 0));
        float ph = 0.8f * ((float)x * fx + (float)y * fy) - animT * 2.8f;
        float ripple = std::sin(ph + 0.7f * std::sin(ph * 0.37f + (float)(h & 7u)));
        int lift = (int)(ripple * 14.f);
        r += lift / 2; g += lift; b += lift;
        if (ripple > 0.92f) { r += 50; g += 55; b += 50; }  // whitewater glints
      } else if (!stillBiome && d >= 4) {
        // Deep ocean: the shared swell field (see pixelviewSwell), crests
        // sharpened, whitecaps on the strongest sets.
        float grp;
        float swell = pixelviewSwell(w, x, y, animT, h, &grp);
        float crest = swell * std::fabs(swell);  // sharpen up, soften down
        int lift = (int)(crest * 15.f);
        r += lift / 2; g += lift; b += (int)(lift * 1.1f);
        if (crest > 0.60f) {
          float f = (crest - 0.60f) / 0.40f * 0.55f;
          r = (int)(r + (205 - r) * f);
          g = (int)(g + (222 - g) * f);
          b = (int)(b + (238 - b) * f);
        }
      } else if (!stillBiome) {
        // Shallows: the SAME swell field rolls in from the deep (so sets
        // cross the depth boundary seamlessly) and hands over to contour
        // surf as the water thins — breaks pulse when a set arrives.
        float grp;
        float swell = pixelviewSwell(w, x, y, animT, h, &grp);
        float shorePh = (float)w.height[y][x] * 0.55f - animT * 3.0f;
        float crest = std::sin(shorePh + 0.5f * std::sin(shorePh * 0.31f + (float)(h & 7u)));
        float mixS = 0.85f - 0.25f * (float)(d - 1);  // d1: surfy, d3: swelly
        float ripple = crest * mixS + swell * (1.f - mixS);

        int lift = (int)(ripple * 16.f);
        r += lift / 2; g += lift; b += lift;
        crest *= (0.55f + 0.45f * grp);  // breaks ride the arriving sets

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
        } else if (nearShore && crest > 0.05f && crest <= 0.55f) {
          // Spray: dissolving cloud of bright pixels trailing the break —
          // the Surf Sandbox signature. Scatter pattern drifts with time.
          uint32_t sp = hash3((uint32_t)x, (uint32_t)y,
                              (uint32_t)(animT * 6.0f) * 2654435761u);
          if ((sp % 6u) == 0u) { r = 205; g = 228; b = 240; }
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

  // Fireflies: sparse warm motes drifting through non-winter nights.
  if (dl.level < 0.30f && season != WINTER && d == 0 && e == ' ' &&
      (w.biome == MEADOW || w.biome == WETLAND || w.biome == TROPICAL)) {
    uint32_t fh = hash3((uint32_t)x, (uint32_t)y, 0xF12EF1u ^ w.worldSeed);
    float p = pixelviewMote(fh, animT, 6.0f, 1600u, 2.0f) * displayBrightness();
    rr += 200.f * p; gg += 215.f * p; bb += 90.f * p;
  }

  // Alpine aurora: slow green/violet curtains wash over the night.
  if (w.biome == ALPINE && dl.level < 0.25f) {
    float aph = (float)x * 0.05f + (float)y * 0.02f;
    float a1 = std::sin(aph + animT * 0.50f);  a1 = a1 > 0.f ? a1 * a1 : 0.f;
    float a2 = std::sin(aph * 0.7f - animT * 0.33f + 2.1f);
    a2 = a2 > 0.f ? a2 * a2 : 0.f;
    float br = displayBrightness();
    rr += 14.f * a2 * br;
    gg += (26.f * a1 + 10.f * a2) * br;
    bb += (16.f * a1 + 22.f * a2) * br;
  }

  // Desert night: starlight twinkle, and now and then a shooting star.
  if (w.biome == DESERT && dl.level < 0.25f) {
    float br = displayBrightness();
    uint32_t st = hash3((uint32_t)x, (uint32_t)y, 0xDE5E27u ^ w.worldSeed);
    {
      float p = pixelviewMote(st, animT, 5.0f, 1900u, 3.0f) * br;
      rr += 175.f * p; gg += 190.f * p; bb += 220.f * p;
    }
    uint32_t sep = (uint32_t)(animT / 40.0f);
    uint32_t se = hash3(sep, 0x57A2u, w.worldSeed);
    float t0 = (float)sep * 40.0f + (float)(se % 860u) / 25.0f;
    float tt = animT - t0;
    if (tt > 0.f && tt < 1.2f) {
      float px = (float)((se >> 8) & 127u) / 127.f * (float)W + tt * 90.f;
      float py = (float)((se >> 16) & 63u) / 63.f * (float)H * 0.5f + tt * 38.f;
      float ddx = (float)x - px, ddy = (float)y - py;
      float dist2 = ddx * ddx + ddy * ddy;
      if (dist2 < 2.5f) { rr += 230.f * br; gg += 235.f * br; bb += 240.f * br; }
      else {
        float back = (ddx * -90.f + ddy * -38.f) / 97.7f;
        if (back > 0.f && back < 7.f && dist2 - back * back < 1.8f) {
          float fade = (1.f - back / 7.f) * 0.8f * br;
          rr += 200.f * fade; gg += 205.f * fade; bb += 215.f * fade;
        }
      }
    }
  }

  // Alien night: sparse bioluminescent spores drift slowly upward.
  if (w.biome == ALIEN && dl.level < 0.35f && d == 0) {
    uint32_t yy = (uint32_t)((float)y + animT * 1.4f);
    uint32_t sp2 = hash3((uint32_t)x, yy, 0xA11E17u ^ w.worldSeed);
    if ((sp2 % 1500u) == 0u) {
      float p = (0.55f + 0.45f * std::sin(animT * 1.6f + (float)(sp2 & 63u))) *
                displayBrightness();
      if (sp2 & 64u) { rr += 90.f * p; gg += 200.f * p; bb += 210.f * p; }
      else           { rr += 200.f * p; gg += 80.f * p; bb += 220.f * p; }
    }
  }

  // Day life, one signature per biome: butterflies, dragonfly glints,
  // heat shimmer, snow glitter, prismatic motes.
  if (dl.level > 0.85f) {
    float br = displayBrightness();
    if ((w.biome == MEADOW || w.biome == TROPICAL) && d == 0 && e == ' ' &&
        season != WINTER) {
      uint32_t bh = hash3((uint32_t)x, (uint32_t)y, 0xB77E12u ^ w.worldSeed);
      float p = pixelviewMote(bh, animT, 5.0f, 3200u, 3.5f) * br;
      if (p > 0.f) {
        if (w.biome == MEADOW) {
          switch ((bh >> 8) % 3u) {  // cabbage white, gold, monarch
            case 0:  rr += 190.f * p; gg += 190.f * p; bb += 180.f * p; break;
            case 1:  rr += 205.f * p; gg += 175.f * p; bb += 60.f * p; break;
            default: rr += 215.f * p; gg += 120.f * p; bb += 45.f * p; break;
          }
        } else {
          switch ((bh >> 8) % 3u) {  // morpho, orchid, sulphur
            case 0:  rr += 65.f * p;  gg += 160.f * p; bb += 210.f * p; break;
            case 1:  rr += 205.f * p; gg += 65.f * p;  bb += 150.f * p; break;
            default: rr += 205.f * p; gg += 180.f * p; bb += 50.f * p; break;
          }
        }
      }
    }
    if (w.biome == WETLAND && d == 0) {
      bool nearWater = (x > 0 && w.water[y][x-1] > 0) ||
                       (x < W-1 && w.water[y][x+1] > 0) ||
                       (y > 0 && w.water[y-1][x] > 0) ||
                       (y < H-1 && w.water[y+1][x] > 0);
      if (nearWater) {
        uint32_t dh = hash3((uint32_t)x, (uint32_t)y, 0xD2A60Fu ^ w.worldSeed);
        float p = pixelviewMote(dh, animT, 3.0f, 2400u, 5.0f) * br;
        rr += 60.f * p; gg += 205.f * p; bb += 195.f * p;
      }
    }
    if (w.biome == DESERT && dl.level > 0.95f) {
      float sh2 = std::sin((float)y * 0.35f + (float)x * 0.07f + animT * 1.6f) *
                  std::sin((float)x * 0.22f - animT * 1.1f);
      float amp = 3.2f * br;  // barely-there midday waver
      rr += sh2 * amp; gg += sh2 * amp * 0.9f; bb += sh2 * amp * 0.7f;
    }
    if (w.biome == ALPINE && d == 0) {
      uint32_t gh = hash3((uint32_t)x, (uint32_t)y, 0x911770u ^ w.worldSeed);
      float p = pixelviewMote(gh, animT, 2.5f, 3600u, 5.0f) * br;
      p = p * p;  // sharp glints
      rr += 200.f * p; gg += 205.f * p; bb += 215.f * p;
    }
    if (w.biome == ALIEN && d == 0) {
      uint32_t ph2 = hash3((uint32_t)x, (uint32_t)y, 0xA71E20u ^ w.worldSeed);
      float p = pixelviewMote(ph2, animT, 7.0f, 3000u, 1.0f) * br;
      if (p > 0.f) {
        float base = animT * 1.3f + (float)(ph2 & 63u);
        rr += (60.f + 60.f * std::sin(base)) * p;
        gg += (60.f + 60.f * std::sin(base + 2.09f)) * p;
        bb += (60.f + 60.f * std::sin(base + 4.19f)) * p;
      }
    }
  }

  // Island cast: the ship on her endless circuit (lantern-lit at night),
  // seabirds skimming the shore by day, an occasional whale, and — rarely
  // — the serpent's humps arcing through the deep.
  if (w.island) {
    PixelviewCast& C = pixelviewCast(animT);
    float br = displayBrightness();
    float fx = (float)x, fy = (float)y;
    if (C.serpentUp) {
      for (int i = 0; i < 6; i += 2) {
        float dxs = fx - C.serpX[i], dys = fy - C.serpY[i];
        if (dxs * dxs + dys * dys < (i == 0 ? 2.2f : 1.4f)) {
          rr = 26.f * br; gg = 62.f * br; bb = 52.f * br;
          if (i == 0 && dl.level < 0.35f) {  // her eye, by moonlight
            rr = 200.f * br; gg = 40.f * br; bb = 30.f * br;
          }
        }
      }
    }
    if (C.whaleUp) {
      float dxw = fx - C.whaleX, dyw = fy - C.whaleY;
      if ((dxw * dxw) / 9.f + (dyw * dyw) / 2.8f < 1.f) {
        rr = 42.f * br; gg = 52.f * br; bb = 74.f * br;
      }
      if (C.whaleAge < 4.f) {  // the spout, fading
        float sx2 = fx - C.whaleX, sy2 = fy - (C.whaleY - 2.6f);
        if (sx2 * sx2 + sy2 * sy2 < 1.2f) {
          float fd = (1.f - C.whaleAge / 4.f) * br;
          rr = 215.f * fd + rr * (1.f - fd);
          gg = 228.f * fd + gg * (1.f - fd);
          bb = 240.f * fd + bb * (1.f - fd);
        }
      }
    }
    {
      float dxs = fx - C.shipX, dys = fy - C.shipY;
      if (dxs * dxs + dys * dys < 2.4f) {  // hull
        rr = 108.f * br; gg = 76.f * br; bb = 48.f * br;
      }
      float sailX = C.shipX - std::sin(C.shipAng) * 1.5f;
      float sailY = C.shipY + std::cos(C.shipAng) * 1.5f;
      float dxl = fx - sailX, dyl = fy - sailY;
      if (dxl * dxl + dyl * dyl < 1.1f) {
        if (dl.level > 0.35f) {  // sail by day
          rr = 232.f * br; gg = 232.f * br; bb = 226.f * br;
        } else {  // lantern by night
          float p = (0.75f + 0.25f * std::sin(animT * 2.3f)) * br;
          rr = 255.f * p; gg = 185.f * p; bb = 90.f * p;
        }
      }
    }
    if (dl.level > 0.5f) {
      for (int i = 0; i < 3; ++i) {
        float dxb = fx - C.birdX[i], dyb = fy - C.birdY[i];
        if (dxb * dxb + dyb * dyb < 0.9f) {
          float flap = 0.75f + 0.25f * std::sin(animT * 7.f + (float)i * 2.1f);
          rr = 238.f * flap * br; gg = 240.f * flap * br; bb = 244.f * flap * br;
        }
      }
    }
  }

  // Storm lightning: single-tick global flashes (the sim's strikes were
  // invisible at 1px/cell — the whole sky flickering sells the storm).
  if (w.weather.state == STORM && (hash3((uint32_t)tick, 99u, 7u) % 19u) == 0u) {
    rr = rr * 1.5f + 70.f; gg = gg * 1.5f + 70.f; bb = bb * 1.4f + 60.f;
  }

  // User contrast (live, from the kiosk remote).
  float ck = displayContrast();
  if (ck != 1.0f) {
    rr = (rr - 128.f) * ck + 128.f;
    gg = (gg - 128.f) * ck + 128.f;
    bb = (bb - 128.f) * ck + 128.f;
  }
  return PixelviewRGB{clampU8((int)rr), clampU8((int)gg), clampU8((int)bb)};
}
