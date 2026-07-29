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
  // A sea current, not a bullseye: one coherent directional flow across
  // the whole ocean (radial island waves read as a clock face). Wind sets
  // the heading; it veers slowly (~10 min) so the sea never goes static.
  float wa = (w.wind.dx == 0 && w.wind.dy == 0)
                 ? 0.7f
                 : std::atan2((float)w.wind.dy, (float)w.wind.dx);
  wa += 0.5f * std::sin(animT * 0.009f);
  float ca = std::cos(wa), sa = std::sin(wa);
  float base = (float)x * ca + (float)y * sa;
  float ang = 0.13f * ((float)y * ca - (float)x * sa);
  float s1 = std::sin(0.42f * base + animT * 1.9f +
                      1.3f * std::sin(ang * 3.f + animT * 0.20f));
  float s2 = std::sin(0.23f * base + animT * 1.15f +
                      1.7f * std::sin(ang * 5.f - animT * 0.13f));
  float s3 = std::sin(0.70f * base + animT * 2.6f + (float)(h & 7u) * 0.22f);
  float grp = 0.6f + 0.4f * std::sin(0.06f * base + animT * 0.45f +
                                     0.8f * std::sin(ang * 2.f));
  if (grpOut) *grpOut = grp;
  // Wave energy follows the wind (live mode: the real wind).
  float energy = 0.70f + 0.10f * (float)w.wind.strength;
  return (0.55f * s1 + 0.30f * s2 + 0.15f * s3) * grp * energy;
}

// Island cast (ship, seabirds, whale): positions computed once per frame.
struct PixelviewCast {
  float t = -1e9f;
  float birdX[3], birdY[3];
  bool shipUp = false;
  float shipX, shipY, shipAng;
  bool pirateUp = false;
  float pirX, pirY, pirAng;
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
    // The ship comes and goes: every ~4 minutes she enters from beyond the
    // edge, sails a straight passage that clears the island, and exits.
    {
      uint32_t sep = (uint32_t)(animT / 240.f);
      uint32_t shh = hash3(sep, 0x5A11u, 0xB0A7u);
      float age = animT - (float)sep * 240.f;
      C.shipUp = age < 110.f;
      if (C.shipUp) {
        float th = (float)(shh & 1023u) / 1023.f * 6.283f;
        float side = ((shh >> 12) & 1u) ? 1.f : -1.f;
        float tx2 = cc + R * 0.82f * std::cos(th);
        float ty2 = cc + R * 0.82f * std::sin(th);
        float dirx = -std::sin(th) * side, diry = std::cos(th) * side;
        float span = R * 1.35f;
        C.shipX = tx2 + dirx * (-span + age * (2.f * span / 110.f));
        C.shipY = ty2 + diry * (-span + age * (2.f * span / 110.f));
        C.shipAng = std::atan2(diry, dirx);
      }
    }
    // The pirate ship: rarer, on her own schedule, running dark.
    {
      uint32_t pep = (uint32_t)(animT / 560.f);
      uint32_t phh = hash3(pep, 0x9147Eu, 0xB1AC4u);
      float age = animT - (float)pep * 560.f;
      C.pirateUp = ((phh % 2u) == 0u) && age < 100.f;
      if (C.pirateUp) {
        float th = (float)(phh & 1023u) / 1023.f * 6.283f;
        float side = ((phh >> 12) & 1u) ? 1.f : -1.f;
        float tx2 = cc + R * 0.78f * std::cos(th);
        float ty2 = cc + R * 0.78f * std::sin(th);
        float dirx = -std::sin(th) * side, diry = std::cos(th) * side;
        float span = R * 1.35f;
        C.pirX = tx2 + dirx * (-span + age * (2.f * span / 100.f));
        C.pirY = ty2 + diry * (-span + age * (2.f * span / 100.f));
        C.pirAng = std::atan2(diry, dirx);
      }
    }
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

// ---------------------------------------------------------------------
// City
// ---------------------------------------------------------------------

// Where the harbour is, and which way it runs. Derived once per world from
// the water itself (second moments -> principal axis) so the boats know
// where to sail without the renderer being told anything about worldgen.
struct CityHarbour {
  uint32_t seed = 0xFFFFFFFFu;
  bool valid = false;
  float cx = 0.f, cy = 0.f;    // centroid of the water
  float ax = 1.f, ay = 0.f;    // long axis, unit
  float halfLen = 0.f;         // extent along the long axis
  float halfWid = 0.f;
  int berthN = 0;              // moorings along the quays
  float berthX[6] = {0}, berthY[6] = {0};
};

inline const CityHarbour& pixelviewHarbour(const World& w) {
  // Two slots: a voyage crosses from one world to the next, and both are
  // rendered on the same frame.
  static CityHarbour slot[2];
  static int next = 0;
  for (int i = 0; i < 2; ++i)
    if (slot[i].seed == w.worldSeed) return slot[i];

  CityHarbour h;
  h.seed = w.worldSeed;
  double sx = 0, sy = 0, n = 0;
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
    if (w.water[y][x] >= 3) { sx += x; sy += y; n += 1; }
  if (n > 40) {
    h.valid = true;
    h.cx = (float)(sx / n); h.cy = (float)(sy / n);
    double mxx = 0, myy = 0, mxy = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
      if (w.water[y][x] < 3) continue;
      double dx = x - h.cx, dy = y - h.cy;
      mxx += dx * dx; myy += dy * dy; mxy += dx * dy;
    }
    mxx /= n; myy /= n; mxy /= n;
    double th = 0.5 * std::atan2(2.0 * mxy, mxx - myy);
    h.ax = (float)std::cos(th); h.ay = (float)std::sin(th);
    double e1 = 0.5 * (mxx + myy) + 0.5 * std::sqrt((mxx - myy) * (mxx - myy) + 4 * mxy * mxy);
    double e2 = 0.5 * (mxx + myy) - 0.5 * std::sqrt((mxx - myy) * (mxx - myy) + 4 * mxy * mxy);
    h.halfLen = (float)(2.0 * std::sqrt(std::max(1.0, e1)));
    h.halfWid = (float)(2.0 * std::sqrt(std::max(1.0, e2)));
    // Moorings: deep-ish water that still has a quay within reach.
    for (int tries = 0; tries < 4000 && h.berthN < 6; ++tries) {
      uint32_t q = hash3((uint32_t)tries, w.worldSeed, 0xB0A7u);
      int x = (int)(q % (uint32_t)W), y = (int)((q >> 11) % (uint32_t)H);
      if (w.water[y][x] < 2 || w.water[y][x] > 4) continue;
      bool nearQuay = false;
      for (int oy = -2; oy <= 2 && !nearQuay; ++oy)
        for (int ox = -2; ox <= 2; ++ox) {
          int nx = x + ox, ny = y + oy;
          if (inBounds(nx, ny) && w.water[ny][nx] == 0) { nearQuay = true; break; }
        }
      if (!nearQuay) continue;
      bool tooClose = false;
      for (int i = 0; i < h.berthN; ++i) {
        float dx = h.berthX[i] - (float)x, dy = h.berthY[i] - (float)y;
        if (dx * dx + dy * dy < 100.f) { tooClose = true; break; }
      }
      if (tooClose) continue;
      h.berthX[h.berthN] = (float)x; h.berthY[h.berthN] = (float)y;
      ++h.berthN;
    }
  }
  slot[next] = h;
  next ^= 1;
  return slot[next ^ 1];
}

// The harbour traffic, positioned once per frame: a ferry working her route,
// an occasional freighter standing in or out, and the moored boats nodding
// at their berths.
struct CityBoats {
  float t = -1e9f;
  uint32_t seed = 0;
  bool ferryUp = false;
  float ferX = 0, ferY = 0, ferDx = 1, ferDy = 0;
  bool shipUp = false;
  float shpX = 0, shpY = 0, shpDx = 1, shpDy = 0;
  int mooredN = 0;
  float mooX[6] = {0}, mooY[6] = {0};
};

inline const CityBoats& pixelviewCityBoats(const World& w, float animT) {
  static CityBoats B;
  const CityHarbour& h = pixelviewHarbour(w);
  if (B.t == animT && B.seed == w.worldSeed) return B;
  B.t = animT; B.seed = w.worldSeed;
  B.ferryUp = B.shipUp = false; B.mooredN = 0;
  if (!h.valid) return B;

  // Ferry: a shuttle up and down the harbour, turning at each end.
  {
    const float period = 84.f;
    float ph = std::fmod(animT, period) / period;      // 0..1
    float tri = (ph < 0.5f) ? (ph * 2.f) : (2.f - ph * 2.f);   // 0..1..0
    float dir = (ph < 0.5f) ? 1.f : -1.f;
    float s = (tri * 2.f - 1.f) * h.halfLen * 0.86f;
    float cross = 0.35f * h.halfWid * std::sin(animT * 0.05f);
    B.ferX = h.cx + h.ax * s - h.ay * cross;
    B.ferY = h.cy + h.ay * s + h.ax * cross;
    B.ferDx = h.ax * dir; B.ferDy = h.ay * dir;
    B.ferryUp = true;
  }
  // Freighter: in on one epoch, gone the next.
  {
    uint32_t ep = (uint32_t)(animT / 200.f);
    uint32_t hh = hash3(ep, w.worldSeed, 0x5417u);
    float age = animT - (float)ep * 200.f;
    if ((hh % 3u) == 0u && age < 150.f) {
      float dir = (hh & 0x10000u) ? 1.f : -1.f;
      float s = (age / 150.f * 2.f - 1.f) * h.halfLen * 1.25f * dir;
      float cross = ((float)((hh >> 4) & 255u) / 255.f - 0.5f) * h.halfWid * 0.7f;
      B.shpX = h.cx + h.ax * s - h.ay * cross;
      B.shpY = h.cy + h.ay * s + h.ax * cross;
      B.shpDx = h.ax * dir; B.shpDy = h.ay * dir;
      B.shipUp = true;
    }
  }
  // Moored boats nod on the swell.
  for (int i = 0; i < h.berthN; ++i) {
    float bob = 0.45f * std::sin(animT * 0.9f + (float)i * 1.7f);
    B.mooX[i] = h.berthX[i] + bob * 0.4f;
    B.mooY[i] = h.berthY[i] + bob;
    ++B.mooredN;
  }
  return B;
}

// Roof palette. Seen from above a city is roofs, not facades: tar and
// gravel, painted membrane, rusted steel, weathered copper — with the pastel
// plaster only showing on the parapets that catch the light.
inline void pixelviewCityRoof(uint32_t fam, int storeys, int& r, int& g, int& b) {
  // Painted metal, mostly — the blue and green roofs you see over any
  // Japanese city, with terracotta and mustard between them and only the
  // occasional stretch of plain tar. The city has to carry colour by day,
  // when the neon is off.
  switch (fam % 14u) {
    case 0:  r = 48;  g = 122; b = 178; break;  // cobalt metal
    case 1:  r = 40;  g = 148; b = 132; break;  // sea green metal
    case 2:  r = 196; g = 92;  b = 62;  break;  // terracotta
    case 3:  r = 214; g = 168; b = 58;  break;  // mustard
    case 4:  r = 72;  g = 96;  b = 168; break;  // deep blue
    case 5:  r = 226; g = 118; b = 132; break;  // coral
    case 6:  r = 232; g = 158; b = 120; break;  // peach membrane
    case 7:  r = 238; g = 212; b = 176; break;  // cream
    case 8:  r = 84;  g = 80;  b = 88;  break;  // tar
    case 9:  r = 158; g = 74;  b = 130; break;  // plum
    case 10: r = 96;  g = 186; b = 196; break;  // pale teal
    case 11: r = 190; g = 150; b = 108; break;  // sand
    case 12: r = 130; g = 128; b = 136; break;  // galvanised
    default: r = 208; g = 96;  b = 88;  break;  // red oxide
  }
  // Tall stock reads cooler and more mechanical, but never goes grey.
  float t = std::min(1.f, (float)storeys / 26.f) * 0.38f;
  r = (int)(r * (1.f - t) + 120 * t);
  g = (int)(g * (1.f - t) + 138 * t);
  b = (int)(b * (1.f - t) + 160 * t);
}

// Is this cell part of the same building as its neighbour? Buildings are
// uniform in height, which is what lets the renderer find their outlines.
inline bool pixelviewSameBuilding(const World& w, int x, int y, int nx, int ny) {
  if (!inBounds(nx, ny)) return false;
  if (!isCityBuilding(w.terrain[ny][nx])) return false;
  return w.height[ny][nx] == w.height[y][x];
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
  bool liveSnow = (g_weatherMode == 1 && liveWeatherNow().snowing);
  bool snowy = liveSnow ||
               (season == WINTER && w.biome != TROPICAL && w.biome != DESERT);

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
        // The whole wave body carries color (Surf Sandbox bands): troughs
        // sink toward deep navy, crests lift toward teal...
        float t01 = crest * 0.5f + 0.5f;
        r = (int)((float)r * 0.75f + t01 * 26.f);
        g = (int)((float)g * 0.80f + t01 * 52.f);
        b = (int)((float)b * 0.85f + t01 * 55.f);
        // ...an aqua face rides just below each crest...
        if (crest > 0.25f && crest <= 0.62f) {
          float f = (crest - 0.25f) / 0.37f * 0.45f;
          r += (int)(20.f * f); g += (int)(70.f * f); b += (int)(60.f * f);
        }
        // ...and whitecaps top the strongest sets.
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
        if (w.biome == TROPICAL) {
          // Lush island blooms: hibiscus, plumeria, bird-of-paradise,
          // orchid, bougainvillea.
          switch ((h >> 5) % 5u) {
            case 0:  r = 255; g = 95 + j; b = 125; break;
            case 1:  r = 255; g = 225 + j; b = 150; break;
            case 2:  r = 255; g = 150 + j; b = 45; break;
            case 3:  r = 230; g = 105 + j; b = 220; break;
            default: r = 255; g = 125 + j; b = 175; break;
          }
        } else {
          // Wildflower mix per-cell: poppy, marigold, orchid, white,
          // cornflower.
          switch ((h >> 5) % 5u) {
            case 0:  r = 225; g = 70 + j; b = 55; break;
            case 1:  r = 255; g = 215 + j; b = 90; break;
            case 2:  r = 175; g = 120 + j; b = 235; break;
            case 3:  r = 248; g = 246; b = 238; break;
            default: r = 120; g = 190 + j; b = 250; break;
          }
        }
        break;
      // ---- City ----
      case CITY_ROAD: {
        r = 34 + j / 2; g = 36 + j / 2; b = 46 + j / 2;
        // Wet asphalt goes darker and glossier when it rains.
        float wet = w.weather.rainStrength;
        if (wet > 0.02f) {
          r = (int)(r * (1.f - 0.30f * wet)); g = (int)(g * (1.f - 0.30f * wet));
          b = (int)(b * (1.f - 0.18f * wet));
        }
        break;
      }
      case CITY_WALK:  r = 78 + j; g = 76 + j; b = 84 + j; break;
      case CITY_QUAY:  r = 96 + j; g = 90 + j; b = 82 + j; break;
      case CITY_BRIDGE:r = 112 + j; g = 106 + j; b = 116 + j; break;
      case CITY_LOT:   r = 62 + j; g = 57 + j; b = 50 + j; break;
      case CITY_LOW: case CITY_MID: case CITY_TOWER: case CITY_GLASS: {
        int storeys = std::max(0, ((int)w.height[y][x] - CITY_BASE_H) / CITY_STOREY);
        // One family per building, keyed off height ALONE: every cell of a
        // building shares its height, and worldgen gives each lot its own
        // storey count. (Mixing a coarse position into this key split single
        // roofs across two colours — the city came out as confetti.)
        uint32_t famh = hash3((uint32_t)w.height[y][x], w.worldSeed, 0x0F00Fu);
        if (t == CITY_GLASS) {   // a glazed atrium roof, banded by its bays
          float band = 0.5f + 0.5f * std::sin((float)(x + y) * 1.9f);
          r = 92 + (int)(30 * band); g = 118 + (int)(34 * band);
          b = 140 + (int)(38 * band);
        } else {
          pixelviewCityRoof(famh, storeys, r, g, b);
        }
        r += j / 2; g += j / 2; b += j / 2;

        // Parapet: the lip of the roof catches the light, which is what
        // gives a block its outline from above.
        bool edge = !pixelviewSameBuilding(w, x, y, x - 1, y) ||
                    !pixelviewSameBuilding(w, x, y, x + 1, y) ||
                    !pixelviewSameBuilding(w, x, y, x, y - 1) ||
                    !pixelviewSameBuilding(w, x, y, x, y + 1);
        if (edge) { r += 42; g += 40; b += 38; }

        // Rooftop plant: tanks, ducts, stair heads, the odd garden or pad.
        uint32_t rh = hash3((uint32_t)x, (uint32_t)y, famh ^ 0x0F007u);
        uint32_t kind = rh % 100u;
        if (!edge) {   // sparse: plant should punctuate a roof, not pepper it
          if (kind < 4u) { r = 118; g = 112; b = 104; }          // AC plant
          else if (kind < 6u) { r = 96; g = 72; b = 52; }        // water tank
          else if (kind < 8u && storeys >= 4) { r = 150; g = 150; b = 156; }   // stair head
          else if (kind < 11u && storeys <= 6) { r = 62; g = 116; b = 64; }    // roof garden
        }
        break;
      }
      case CITY_NEON: {
        // The sign's own colour; the glow is added after the light grade.
        switch ((h >> 7) % 5u) {
          case 0:  r = 236; g = 60;  b = 150; break;  // magenta
          case 1:  r = 70;  g = 220; b = 226; break;  // cyan
          case 2:  r = 255; g = 156; b = 60;  break;  // amber
          case 3:  r = 150; g = 240; b = 110; break;  // lime
          default: r = 240; g = 84;  b = 80;  break;  // red
        }
        break;
      }
      case 'V': r = 46; g = 38 + j / 2; b = 40; break;  // basalt vent
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

  // The alien world is not Earth with odd colours: its whole biology is
  // bioluminescent, so the ground itself carries slow chromatic tides and
  // the flora glows in daylight instead of merely reflecting it. This
  // overrides the terrestrial hue bands wholesale for ALIEN.
  if (tintable && w.biome == ALIEN) {
    // Chromatic tide: a slow wave crossing the world, so the palette drifts
    // through violet, teal and chartreuse rather than sitting still.
    float tide = std::sin((float)x * 0.045f + (float)y * 0.031f + animT * 0.11f);
    float tide2 = std::sin((float)x * 0.017f - (float)y * 0.023f - animT * 0.07f);
    float shift = 0.5f + 0.5f * tide;
    uint32_t fam = (h >> 6) & 3u;
    switch (t) {
      case ',': case '"': case ';': case ':': {   // filament turf
        r = 26 + (int)(38.f * shift) + j;
        g = 88 + (int)(52.f * (1.f - shift)) + j;
        b = 96 + (int)(58.f * shift);
        if (fam == 0) { r += 20; b += 18; }        // violet strain
        break;
      }
      case '#': {                                  // bladder-shrub
        r = 62 + (int)(48.f * shift) + j;
        g = 42 + (int)(36.f * (1.f - shift));
        b = 104 + (int)(42.f * shift);
        break;
      }
      case 'T': case 'Y': case 'P': {              // spires, cyan at the tips
        r = 38 + (int)(30.f * shift);
        g = 28 + (int)(72.f * (0.35f + 0.65f * (1.f - shift))) + j;
        b = 92 + (int)(62.f * shift);
        break;
      }
      case 'm': {                                  // glow pods
        float pulse = 0.55f + 0.45f * std::sin(animT * 1.9f + (float)(h & 31u));
        r = (int)(70.f + 120.f * pulse);
        g = (int)(210.f + 40.f * pulse);
        b = (int)(180.f + 60.f * pulse);
        break;
      }
      case 'f': case '+': case '&': case '!': {    // luminous blooms
        float pulse = 0.5f + 0.5f * std::sin(animT * 1.3f + (float)(h & 63u));
        switch ((h >> 5) % 4u) {
          case 0:  r = (int)(90 + 90 * pulse);  g = 250; b = 230; break;  // cyan
          case 1:  r = 240; g = (int)(70 + 60 * pulse);  b = 250; break;  // magenta
          case 2:  r = (int)(180 + 60 * pulse); g = 255; b = 90;  break;  // chartreuse
          default: r = 150; g = (int)(120 + 90 * pulse); b = 255; break;  // violet
        }
        break;
      }
      case '$': r = 250; g = 230; b = 120; break;  // amber sap
      case 'c': r = 90;  g = 230; b = 190; break;
      case 'd': case 'e': case 'g':                // regolith / flesh ground
        r = 74 + (int)(20.f * tide2) + j / 2;
        g = 52 + (int)(16.f * tide) + j / 2;
        b = 86 + (int)(26.f * tide2);
        break;
      case '^': case 'B': r = 104; g = 96; b = 128; break;
      case 'M': r = 150; g = 140; b = 178; break;
      case 's': r = 168; g = 150; b = 190; break;
      case '.': default:
        if (displayBgMode() == 1) { r = g = b = 0; }
        else {
          r = 34 + (int)(14.f * tide2) + j / 2;
          g = 24 + j / 2;
          b = 44 + (int)(18.f * tide) + j / 2;
        }
        break;
    }
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
                     t == ':' || t == 's' ||
                     isCityPaved(t) || t == CITY_LOT);  // snow lies on streets
      if (ground) {
        uint32_t sh = hash3((uint32_t)x, (uint32_t)y, 0x534E4F57u);
        float cover = 0.10f + 0.22f * seasonLerp(tick);
        if ((float)(sh & 1023u) / 1023.0f < cover) { r = 226; g = 232; b = 244; }
      }
      // Roofs take snow too, but blended: a hard white speckle over the
      // city's painted metal read as static rather than settled snow.
      if (w.biome == CITY && isCityBuilding(t)) {
        uint32_t sh = hash3((uint32_t)x, (uint32_t)y, 0x524F4F46u);
        float lay = (0.35f + 0.45f * seasonLerp(tick)) *
                    (0.55f + 0.45f * (float)(sh & 255u) / 255.f);
        r = (int)(r + (232 - r) * lay);
        g = (int)(g + (238 - g) * lay);
        b = (int)(b + (248 - b) * lay);
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

  // ---- The city after dark (and its traffic, which never stops) ----
  if (w.biome == CITY) {
    const float br = displayBrightness();
    const float night = std::clamp(1.0f - dl.level / 0.60f, 0.0f, 1.0f);
    const float fx = (float)x, fy = (float)y;

    // Shadows. From above, a city is legible because its towers lie across
    // everything north-west of them — this is what puts the skyline into a
    // top-down view at all.
    if (dl.level > 0.20f) {
      int hHere = (int)w.height[y][x];
      float dark = 0.f;
      for (int k = 1; k <= 4; ++k) {
        int nx = x - k, ny = y - k;
        if (!inBounds(nx, ny)) break;
        if (!isCityBuilding(w.terrain[ny][nx])) continue;
        int over = (int)w.height[ny][nx] - hHere;
        if (over <= 0) continue;
        // A storey throws about a cell of shadow at this sun angle.
        float reach = (float)over / (float)CITY_STOREY;
        if (reach >= (float)k)
          dark = std::max(dark, 0.42f * (1.f - (float)(k - 1) * 0.18f));
      }
      if (dark > 0.f) {
        float f = dark * dl.level;
        rr *= (1.f - f); gg *= (1.f - f * 0.94f); bb *= (1.f - f * 0.82f);
      }
    }

    // Golden hour over a harbour city: the whole thing goes rose.
    if (dl.warm > 0.f) {
      float ww = dl.warm * br;
      rr += 34.f * ww; gg += 9.f * ww; bb += 20.f * ww;
    }

    // Lit windows: each cell is a floor's worth of glass that switches on
    // its own slow clock, so the towers shimmer instead of strobing.
    if (isCityBuilding(t) && night > 0.02f) {
      uint32_t wh = hash3((uint32_t)x, (uint32_t)y, 0x5711DEu ^ w.worldSeed);
      float lifeSec = 50.f + (float)(wh % 120u);
      uint32_t ep = (uint32_t)(animT / lifeSec + (float)(wh % 977u) * 0.0011f);
      uint32_t st = hash3(wh, ep * 2654435761u, 0x1417u);
      float density = (t == CITY_LOW) ? 0.26f
                    : (t == CITY_GLASS) ? 0.30f : 0.38f;
      density *= 0.45f + 0.55f * night;     // the offices empty overnight
      // Occupancy per building (height keys a building, as with its roof):
      // some blocks blaze, some are dark for the night. Uniform density made
      // every tower an identical smear of scattered lights.
      uint32_t bh = hash3((uint32_t)w.height[y][x], w.worldSeed, 0x0CC0u);
      density *= 0.20f + 1.35f * (float)(bh & 255u) / 255.f;
      // The perimeter is where the windows are; a roof is mostly roof.
      bool onEdge = !pixelviewSameBuilding(w, x, y, x - 1, y) ||
                    !pixelviewSameBuilding(w, x, y, x + 1, y) ||
                    !pixelviewSameBuilding(w, x, y, x, y - 1) ||
                    !pixelviewSameBuilding(w, x, y, x, y + 1);
      density *= onEdge ? 1.6f : 0.35f;
      if ((float)(st & 1023u) / 1023.f < density) {
        float p = night * br;
        switch ((st >> 12) % 8u) {
          case 0:  rr += 120.f * p; gg += 170.f * p; bb += 235.f * p; break;  // a TV
          case 1:  rr += 235.f * p; gg += 232.f * p; bb += 215.f * p; break;  // office white
          default: rr += 245.f * p; gg += 205.f * p; bb += 140.f * p; break;  // lamplight
        }
      }
    }

    // Signage, street lamps and their spill. One 3x3 pass gathers both.
    if (night > 0.02f && (isCityPaved(t) || isCityBuilding(t) ||
                          t == CITY_NEON || t == CITY_LOT)) {
      float glowR = 0.f, glowG = 0.f, glowB = 0.f;
      for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
        int nx = x + ox, ny = y + oy;
        if (!inBounds(nx, ny)) continue;
        char n = w.terrain[ny][nx];
        bool self = (ox == 0 && oy == 0);
        float fall = self ? 1.0f : (ox && oy ? 0.28f : 0.42f);
        if (n == CITY_NEON) {
          uint32_t nh = hash3((uint32_t)nx, (uint32_t)ny, w.worldSeed);
          float pulse = 0.72f + 0.28f * std::sin(animT * (1.1f + (float)(nh & 7u) * 0.3f) +
                                                 (float)(nh & 63u));
          // A tube on the blink: rare, brief, and it comes back.
          if (((nh >> 9) % 7u) == 0u &&
              std::sin(animT * 6.0f + (float)(nh & 31u)) > 0.93f) pulse *= 0.25f;
          float a = pulse * fall * night;
          switch ((nh >> 7) % 5u) {
            case 0:  glowR += 236.f * a; glowG += 60.f * a;  glowB += 150.f * a; break;
            case 1:  glowR += 70.f * a;  glowG += 220.f * a; glowB += 226.f * a; break;
            case 2:  glowR += 255.f * a; glowG += 156.f * a; glowB += 60.f * a;  break;
            case 3:  glowR += 150.f * a; glowG += 240.f * a; glowB += 110.f * a; break;
            default: glowR += 240.f * a; glowG += 84.f * a;  glowB += 80.f * a;  break;
          }
        } else if (n == CITY_WALK || n == CITY_BRIDGE) {
          // Street lamps at regular intervals along the pavement.
          if ((hash3((uint32_t)nx, (uint32_t)ny, 0x1A47Fu) % 19u) == 0u) {
            float a = fall * night * (0.85f + 0.15f * std::sin(animT * 0.7f + (float)nx));
            glowR += 210.f * a; glowG += 160.f * a; glowB += 85.f * a;
          }
        }
      }
      rr += glowR * 0.80f * br; gg += glowG * 0.80f * br; bb += glowB * 0.80f * br;
    }

    // Aviation lights: a slow red blink on the tallest towers.
    if (isCityBuilding(t) && night > 0.15f &&
        (int)w.height[y][x] >= CITY_BASE_H + 20 * CITY_STOREY) {
      uint32_t ah = hash3((uint32_t)w.height[y][x], w.worldSeed, 0xA71Au);
      if ((hash3((uint32_t)x, (uint32_t)y, ah) % 23u) == 0u) {
        float blink = std::sin(animT * 1.7f + (float)(ah & 63u));
        if (blink > 0.55f) {
          float p = night * br * (blink - 0.55f) / 0.45f;
          rr += 235.f * p; gg += 40.f * p; bb += 30.f * p;
        }
      }
    }

    // Traffic. Lanes alternate direction, each with its own spacing and
    // speed, so the streams read as flow rather than a marching pattern.
    if (t == CITY_ROAD || t == CITY_BRIDGE) {
      bool horiz = (x > 0 && (w.terrain[y][x-1] == t)) ||
                   (x < W - 1 && (w.terrain[y][x+1] == t));
      bool vert  = (y > 0 && (w.terrain[y-1][x] == t)) ||
                   (y < H - 1 && (w.terrain[y+1][x] == t));
      if (horiz != vert) {                       // never at a junction
        int lane = horiz ? y : x;
        float dir = (lane & 1) ? 1.f : -1.f;
        float s = horiz ? fx : fy;
        uint32_t lh = hash3((uint32_t)lane, w.worldSeed, 0x7A4F1Cu);
        bool quiet = (night > 0.7f) && ((lh % 3u) == 0u);   // small hours
        if (!quiet) {
          float spacing = 9.f + (float)(lh % 15u);
          float speed = 5.0f + (float)((lh >> 5) % 8u);
          float u = (s - dir * speed * animT) / spacing +
                    (float)(lh % 100u) * 0.01f;
          float cell = std::floor(u);
          float f = u - cell;
          if (f < 1.5f / spacing) {
            uint32_t ch = hash3((uint32_t)lane, (uint32_t)(int)cell, w.worldSeed);
            float p = br;
            if (night > 0.35f) {
              // Headlights coming, tail lights going.
              bool coming = (dir > 0.f) == (horiz ? true : true);
              if (((ch >> 3) & 1u) ^ (coming ? 0u : 1u)) {
                rr = 255.f * p; gg = 240.f * p; bb = 205.f * p;
              } else {
                rr = 235.f * p; gg = 60.f * p; bb = 45.f * p;
              }
            } else {
              switch (ch % 6u) {                 // daylight paintwork
                case 0:  rr = 220.f; gg = 225.f; bb = 230.f; break;
                case 1:  rr = 40.f;  gg = 44.f;  bb = 52.f;  break;
                case 2:  rr = 190.f; gg = 60.f;  bb = 55.f;  break;
                case 3:  rr = 70.f;  gg = 110.f; bb = 180.f; break;
                case 4:  rr = 200.f; gg = 190.f; bb = 90.f;  break;
                default: rr = 120.f; gg = 130.f; bb = 135.f; break;
              }
              rr *= br; gg *= br; bb *= br;
            }
          }
        }
      }
    }

    // Harbour: the skyline falls into the water, wobbling.
    if (d > 0 && night > 0.05f) {
      float wob = 1.4f * std::sin(fy * 0.8f + animT * 1.7f);
      for (int k = 2; k <= 8; k += 3) {
        int sxr = (int)(fx + wob), syr = y - k;
        if (!inBounds(sxr, syr)) continue;
        char src = w.terrain[syr][sxr];
        float a = night * br * (0.30f - 0.03f * (float)k);
        if (src == CITY_NEON) {
          uint32_t nh = hash3((uint32_t)sxr, (uint32_t)syr, w.worldSeed);
          switch ((nh >> 7) % 5u) {
            case 0:  rr += 190.f * a; gg += 50.f * a;  bb += 120.f * a; break;
            case 1:  rr += 55.f * a;  gg += 175.f * a; bb += 180.f * a; break;
            case 2:  rr += 205.f * a; gg += 125.f * a; bb += 50.f * a;  break;
            case 3:  rr += 120.f * a; gg += 190.f * a; bb += 90.f * a;  break;
            default: rr += 190.f * a; gg += 70.f * a;  bb += 65.f * a;  break;
          }
        } else if (isCityBuilding(src)) {
          uint32_t wh2 = hash3((uint32_t)sxr, (uint32_t)syr, 0x5711DEu ^ w.worldSeed);
          if ((wh2 % 5u) == 0u) {
            rr += 170.f * a; gg += 145.f * a; bb += 100.f * a;
          }
        }
      }
    }

    // Boats. The ferry works her route whatever the weather; a freighter
    // stands in and out; the moored ones nod at the quay.
    if (d > 0) {
      const CityBoats& B = pixelviewCityBoats(w, animT);
      auto hull = [&](float bx, float by, float dx2, float dy2, float len,
                      float wid) {
        float px = fx - bx, py = fy - by;
        float along = px * dx2 + py * dy2;
        float across = -px * dy2 + py * dx2;
        return (along * along) / (len * len) + (across * across) / (wid * wid);
      };
      if (B.ferryUp) {
        float q = hull(B.ferX, B.ferY, B.ferDx, B.ferDy, 2.6f, 1.15f);
        if (q < 1.f) {
          rr = 232.f * br; gg = 234.f * br; bb = 238.f * br;   // white hull
          if (q < 0.28f && night > 0.3f) {                     // lit saloon
            rr = 255.f * br; gg = 226.f * br; bb = 160.f * br;
          }
        } else if (q < 2.6f) {
          // Wake: foam trailing astern.
          float px = fx - B.ferX, py = fy - B.ferY;
          float along = px * B.ferDx + py * B.ferDy;
          if (along < 0.f) {
            uint32_t fh = hash3((uint32_t)x, (uint32_t)y,
                                (uint32_t)(animT * 5.f) * 2654435761u);
            if ((fh % 3u) == 0u) {
              rr = rr * 0.4f + 205.f * br * 0.6f;
              gg = gg * 0.4f + 225.f * br * 0.6f;
              bb = bb * 0.4f + 240.f * br * 0.6f;
            }
          }
        }
        if (night > 0.35f) {   // nav lights: red to port, green to starboard
          float px = fx - B.ferX, py = fy - B.ferY;
          float across = -px * B.ferDy + py * B.ferDx;
          float along = px * B.ferDx + py * B.ferDy;
          if (along > 1.6f && along < 3.0f && std::fabs(across) > 0.6f &&
              std::fabs(across) < 1.8f) {
            if (across > 0.f) { rr = 60.f * br; gg = 240.f * br; bb = 90.f * br; }
            else              { rr = 245.f * br; gg = 50.f * br; bb = 50.f * br; }
          }
        }
      }
      if (B.shipUp) {
        float q = hull(B.shpX, B.shpY, B.shpDx, B.shpDy, 6.5f, 1.9f);
        if (q < 1.f) {
          float px = fx - B.shpX, py = fy - B.shpY;
          float along = px * B.shpDx + py * B.shpDy;
          if (along > 2.0f) {                    // stacked containers
            uint32_t ch = hash3((uint32_t)x, (uint32_t)y, w.worldSeed ^ 0xC0A7u);
            switch (ch % 4u) {
              case 0:  rr = 170.f * br; gg = 55.f * br;  bb = 48.f * br; break;
              case 1:  rr = 45.f * br;  gg = 95.f * br;  bb = 150.f * br; break;
              case 2:  rr = 60.f * br;  gg = 130.f * br; bb = 90.f * br; break;
              default: rr = 190.f * br; gg = 150.f * br; bb = 60.f * br; break;
            }
          } else {
            rr = 48.f * br; gg = 52.f * br; bb = 62.f * br;   // hull and house
            if (night > 0.3f && along < -3.5f) {
              rr = 250.f * br; gg = 220.f * br; bb = 160.f * br;
            }
          }
        }
      }
      for (int i = 0; i < B.mooredN; ++i) {
        float dxm = fx - B.mooX[i], dym = fy - B.mooY[i];
        if (dxm * dxm + dym * dym * 2.2f < 1.6f) {
          rr = 208.f * br; gg = 206.f * br; bb = 196.f * br;
          if (night > 0.4f && ((hash3((uint32_t)i, 0x11u, w.worldSeed) & 1u) == 0u)) {
            rr = 250.f * br; gg = 210.f * br; bb = 120.f * br;
          }
        }
      }
    }
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

  // Alien biology is its own light source: the blooms and pods do not go
  // dark at dusk, they take over. Emission is added after the day/night
  // grade and scaled by how dark it has got, so the world inverts from a
  // strange daylight into a glowing one.
  if (w.biome == ALIEN) {
    const float br = displayBrightness();
    const float dark = std::clamp(1.0f - dl.level, 0.0f, 1.0f);
    if (e == ' ' && d == 0) {
      float emit = 0.f;
      switch (t) {
        case 'f': case '+': case '&': case '!': emit = 0.85f; break;
        case 'm': emit = 0.70f; break;
        case 'T': case 'Y': case 'P': emit = 0.18f; break;
        case '#': emit = 0.10f; break;
        case ',': case '"': case ';': case ':': emit = 0.04f; break;
        default: break;
      }
      if (emit > 0.f) {
        float pulse = 0.62f + 0.38f * std::sin(animT * 1.5f + (float)(h & 63u));
        float p = emit * pulse * (0.25f + 0.75f * dark) * br;
        rr += (float)r * p * 0.55f; gg += (float)g * p * 0.55f; bb += (float)b * p * 0.55f;
      }
    }
    // Ammonia sea: the water is not Earth-blue.
    if (d > 0) {
      float vio = 0.5f + 0.5f * std::sin((float)x * 0.03f - (float)y * 0.02f + animT * 0.09f);
      float nr = rr * 0.72f + bb * 0.30f * vio + 18.f * vio;
      float ng = gg * 0.94f + 16.f * (1.f - vio);
      float nb = bb * 0.88f + gg * 0.16f;
      rr = nr; gg = ng; bb = nb;
    }
    // A bioluminescent tide sweeps the whole world every half minute or so.
    {
      float band = std::sin(((float)x + (float)y) * 0.06f - animT * 0.55f);
      if (band > 0.92f) {
        float p = (band - 0.92f) / 0.08f * 0.22f * br;
        rr += 40.f * p; gg += 150.f * p; bb += 130.f * p;
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
    if (w.biome == DESERT && d == 0) {
      // Tumbleweed: rolls with the wind, bouncing, one crossing at a time.
      float twSpan = (float)W * 1.4f;
      uint32_t twe = (uint32_t)(animT / 45.f);
      uint32_t twh = hash3(twe, 0x70B1Eu, w.worldSeed);
      float age = animT - (float)twe * 45.f;
      if ((twh % 2u) == 0u && age < 34.f) {
        float wx3 = (w.wind.dx == 0 && w.wind.dy == 0) ? 1.f : (float)w.wind.dx;
        float wy3 = (float)w.wind.dy * 0.4f;
        float n = std::sqrt(wx3 * wx3 + wy3 * wy3);
        float sx0 = ((twh >> 8) & 1023u) / 1023.f * (float)W;
        float sy0 = ((twh >> 18) & 1023u) / 1023.f * (float)H;
        float px2 = sx0 + wx3 / n * (age * twSpan / 34.f - twSpan * 0.5f);
        float py2 = sy0 + wy3 / n * (age * twSpan / 34.f - twSpan * 0.5f) -
                    2.2f * std::fabs(std::sin(age * 2.2f));
        px2 = px2 - std::floor(px2 / (float)W) * (float)W;  // wrap
        float ddx2 = (float)x - px2;
        if (ddx2 > (float)W * 0.5f) ddx2 -= (float)W;
        if (ddx2 < -(float)W * 0.5f) ddx2 += (float)W;
        float ddy2 = (float)y - py2;
        if (ddx2 * ddx2 + ddy2 * ddy2 < 1.2f) {
          rr = 185.f * br; gg = 155.f * br; bb = 105.f * br;
        }
      }
      // Dust devil: occasional wandering spiral of lifted sand.
      uint32_t dde = (uint32_t)(animT / 90.f);
      uint32_t ddh = hash3(dde, 0xD05Eu, w.worldSeed);
      float dage = animT - (float)dde * 90.f;
      if ((ddh % 3u) == 0u && dage < 20.f) {
        float dxc = ((ddh >> 6) & 1023u) / 1023.f * (float)W * 0.7f + (float)W * 0.15f +
                    6.f * std::sin(dage * 0.7f);
        float dyc = ((ddh >> 16) & 1023u) / 1023.f * (float)H * 0.7f + (float)H * 0.15f +
                    6.f * std::cos(dage * 0.5f);
        float ex = (float)x - dxc, ey = (float)y - dyc;
        float dist2 = ex * ex + ey * ey;
        if (dist2 < 6.5f) {
          float swirl = std::sin(std::atan2(ey, ex) * 3.f + animT * 8.f -
                                 std::sqrt(dist2) * 1.8f);
          float f = (1.f - dist2 / 6.5f) * (0.5f + 0.5f * swirl) * 0.5f * br;
          rr += 120.f * f; gg += 105.f * f; bb += 75.f * f;
        }
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
    if (C.shipUp && d > 0) {
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
    if (C.pirateUp && d > 0) {
      float dxp = fx - C.pirX, dyp = fy - C.pirY;
      if (dxp * dxp + dyp * dyp < 2.4f) {  // black hull
        rr = 38.f * br; gg = 34.f * br; bb = 36.f * br;
      }
      float sailX = C.pirX - std::sin(C.pirAng) * 1.5f;
      float sailY = C.pirY + std::cos(C.pirAng) * 1.5f;
      float dxl = fx - sailX, dyl = fy - sailY;
      if (dxl * dxl + dyl * dyl < 1.1f) {
        if (dl.level > 0.35f) {  // dark sails by day
          rr = 72.f * br; gg = 66.f * br; bb = 78.f * br;
        } else {  // she runs dark at night — barely a silhouette
          rr = 30.f * br; gg = 28.f * br; bb = 34.f * br;
        }
      }
      // red pennant above the sail
      float pnX = sailX - std::sin(C.pirAng) * 1.4f;
      float pnY = sailY + std::cos(C.pirAng) * 1.4f;
      float dxn = fx - pnX, dyn = fy - pnY;
      if (dxn * dxn + dyn * dyn < 0.5f) {
        rr = 190.f * br; gg = 35.f * br; bb = 30.f * br;
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

  // Volcano: the crater breathes a faint glow when dormant (strongest at
  // night); during an eruption the vent floods with lava light, embers
  // scatter downwind, and smoke hazes the slopes.
  if (w.ventX >= 0) {
    float br = displayBrightness();
    float vdx = (float)x - (float)w.ventX, vdy = (float)y - (float)w.ventY;
    float vd2 = vdx * vdx + vdy * vdy;
    bool erupting = (w.eruptEnd > tick);
    if (!erupting && vd2 < 3.5f) {
      float breathe = 0.5f + 0.5f * std::sin(animT * 0.8f);
      float glow = (0.25f + 0.45f * (1.f - dl.level)) * breathe *
                   (1.f - vd2 / 3.5f) * br;
      rr += 160.f * glow; gg += 55.f * glow; bb += 15.f * glow;
    }
    if (erupting) {
      float fury = std::min(1.f, (float)(w.eruptEnd - tick) / 120.f);
      if (vd2 < 9.f) {  // lava light
        float f = (1.f - vd2 / 9.f) * (0.65f + 0.35f * std::sin(animT * 7.f)) * br;
        rr += 255.f * f * fury; gg += 130.f * f * fury; bb += 25.f * f * fury;
      }
      if (vd2 < 240.f && d == 0) {  // embers drifting downwind
        float wdot = vdx * (float)w.wind.dx + vdy * (float)w.wind.dy;
        uint32_t eh = hash3((uint32_t)x, (uint32_t)y, 0xE38E5u ^ w.worldSeed);
        float em = pixelviewMote(eh, animT, 2.2f, wdot > 0.f ? 240u : 700u, 6.f);
        em *= fury * br;
        rr += 230.f * em; gg += 120.f * em; bb += 25.f * em;
        // smoke haze thickens downwind of the vent
        if (wdot > 0.f) {
          float haze = std::min(1.f, wdot / 16.f) * (1.f - vd2 / 240.f) * 0.35f * fury;
          rr = rr * (1.f - haze) + 70.f * haze * br;
          gg = gg * (1.f - haze) + 66.f * haze * br;
          bb = bb * (1.f - haze) + 68.f * haze * br;
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
