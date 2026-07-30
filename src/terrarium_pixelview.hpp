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

// ---- Traffic ----
// Cars are agents, not a pattern. The first version derived them per cell
// from a closed form (lane parity, phase, speed): that could not turn, could
// not vary within a lane, and vanished wherever a cell had road on both axes
// — which is every intersection AND every cell of a three-wide avenue. So
// they drive now: each car holds a position and a heading, follows the road,
// picks a way at junctions, and gets rasterised into an occupancy grid once
// per frame so the per-cell lookup stays O(1).
struct CityCar {
  float x = 0.f, y = 0.f;
  int8_t dx = 1, dy = 0;
  float speed = 6.f;
  float wait = 0.f;      // held at a junction
  uint8_t hue = 0;
};
struct CityTraffic {
  float t = -1e9f;
  uint32_t seed = 0xFFFFFFFFu;
  std::vector<CityCar> cars;
  std::vector<uint8_t> cell;   // 0 empty, else 1 + car index (capped)
  std::vector<uint8_t> dirOf;  // 0 none, 1 = heading +, 2 = heading -
};

inline bool pixelviewIsRoad(const World& w, int x, int y) {
  if (!inBounds(x, y)) return false;
  char c = w.terrain[y][x];
  return c == CITY_ROAD || c == CITY_BRIDGE;
}

inline const CityTraffic& pixelviewTraffic(const World& w, float animT) {
  static CityTraffic slot[2];
  static int rr2 = 0;
  CityTraffic* T = nullptr;
  for (int i = 0; i < 2; ++i) if (slot[i].seed == w.worldSeed) T = &slot[i];
  if (!T) { T = &slot[rr2]; rr2 ^= 1; *T = CityTraffic{}; T->seed = w.worldSeed; }
  if (T->t == animT) return *T;

  float dt = (T->t < -1e8f) ? 0.f : std::min(0.35f, animT - T->t);
  bool first = T->cars.empty();
  T->t = animT;
  if (T->cell.empty()) { T->cell.assign((size_t)W * H, 0); T->dirOf.assign((size_t)W * H, 0); }

  uint32_t rs = w.worldSeed ^ 0x7A4F1Cu;
  auto rnd = [&]() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; };

  if (first) {
    int want = std::max(8, (W * H) / 620);
    for (int tries = 0; tries < want * 60 && (int)T->cars.size() < want; ++tries) {
      int x = (int)(rnd() % (uint32_t)W), y = (int)(rnd() % (uint32_t)H);
      if (!pixelviewIsRoad(w, x, y)) continue;
      // A heading the road actually goes in.
      const int dxs[4] = {1, -1, 0, 0}, dys[4] = {0, 0, 1, -1};
      int pick = -1;
      for (int k = 0; k < 4; ++k) {
        int kk = (int)((rnd() + (uint32_t)k) % 4u);
        if (pixelviewIsRoad(w, x + dxs[kk], y + dys[kk])) { pick = kk; break; }
      }
      if (pick < 0) continue;
      CityCar c;
      c.x = (float)x + 0.5f; c.y = (float)y + 0.5f;
      c.dx = (int8_t)dxs[pick]; c.dy = (int8_t)dys[pick];
      c.speed = 3.2f + (float)(rnd() % 1000u) * 0.0075f;   // 3.2 .. 10.7 cells/s
      c.hue = (uint8_t)(rnd() & 255u);
      T->cars.push_back(c);
    }
  }

  std::fill(T->cell.begin(), T->cell.end(), (uint8_t)0);
  std::fill(T->dirOf.begin(), T->dirOf.end(), (uint8_t)0);

  for (size_t i = 0; i < T->cars.size(); ++i) {
    CityCar& c = T->cars[i];
    if (c.wait > 0.f) {
      c.wait -= dt;
    } else {
      int cx = (int)c.x, cy = (int)c.y;
      float step = c.speed * dt;
      // Advance in short hops so a fast car cannot tunnel through a junction.
      while (step > 0.f) {
        float hop = std::min(step, 0.45f);
        step -= hop;
        c.x += (float)c.dx * hop;
        c.y += (float)c.dy * hop;
        int nx = (int)c.x, ny = (int)c.y;
        if (nx == cx && ny == cy) continue;
        cx = nx; cy = ny;
        // Entered a new cell: is there still road ahead, and is this a junction?
        bool ahead = pixelviewIsRoad(w, nx + c.dx, ny + c.dy);
        int lx = -c.dy, ly = c.dx;                  // the two perpendiculars
        bool left = pixelviewIsRoad(w, nx + lx, ny + ly);
        bool right = pixelviewIsRoad(w, nx - lx, ny - ly);
        bool junction = (left || right);
        uint32_t roll = rnd();
        if (!ahead || (junction && (roll % 100u) < 16u)) {
          // Turn. Prefer a turn that exists; if the road dead-ends, come about.
          bool goLeft = left && (!right || (roll & 0x100u));
          if (goLeft)       { c.dx = (int8_t)lx; c.dy = (int8_t)ly; }
          else if (right)   { c.dx = (int8_t)-lx; c.dy = (int8_t)-ly; }
          else if (!ahead)  { c.dx = (int8_t)-c.dx; c.dy = (int8_t)-c.dy; }
          c.x = (float)nx + 0.5f; c.y = (float)ny + 0.5f;
          if ((roll >> 12) % 5u == 0u) c.wait = 0.25f + (float)((roll >> 16) % 90u) * 0.01f;
          break;
        }
        if (!pixelviewIsRoad(w, nx, ny)) {          // shoved off the network
          c.dx = (int8_t)-c.dx; c.dy = (int8_t)-c.dy;
          c.x = (float)cx + 0.5f; c.y = (float)cy + 0.5f;
          break;
        }
      }
    }
    int ix = std::clamp((int)c.x, 0, W - 1), iy = std::clamp((int)c.y, 0, H - 1);
    T->cell[(size_t)iy * W + ix] = (uint8_t)(1u + (i & 0x7Fu));
    T->dirOf[(size_t)iy * W + ix] = (uint8_t)((c.dx + c.dy > 0) ? 1u : 2u);
  }
  return *T;
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

// ---- The thing in the alien world that notices you ----
// Rarely, something very large rises at the edge of the world, looks slowly
// around, and goes back down. It reads as a silhouette — the world dims and
// hazes behind it — and only the eyes are lit, which is what makes it
// unpleasant. Same schedule-by-epoch trick as the whale and the serpent.
struct AlienHead {
  float t = -1e9f;
  bool up = false;
  int side = -1;                 // -1 leans in from the left rim, +1 the right
  float cx = 0.f, cy = 0.f;      // centre of the cranium, in cells
  float rx = 1.f, ry = 1.f;
  float tilt = 0.f;              // head cocked toward the middle
  float rise = 0.f;              // 0 hidden .. 1 fully leaning in
  float gaze = 0.f;              // -1 .. +1, where it is looking
  float blink = 1.f;             // 1 open, 0 shut
  float handX = 0.f, handY = 0.f;  // the hand that holds the rim
  float age = 0.f;
};

inline const AlienHead& pixelviewAlienHead(const World& w, float animT) {
  static AlienHead A;
  if (A.t == animT) return A;
  A.t = animT;
  // Lugia-rare: one epoch in eleven, a quarter-hour apart, for half a minute.
  // That is about one appearance per three hours the alien world is up —
  // and it is one voyage stop in nine, so most days you will not see it.
  const float kEpoch = 900.f, kDwell = 30.f;
  uint32_t ep = (uint32_t)(animT / kEpoch);
  uint32_t hh = hash3(ep, w.worldSeed, 0x8EAD5u);
  float age = animT - (float)ep * kEpoch;
  A.up = ((hh % 11u) == 0u) && age < kDwell;
#ifdef TERRA_FORCE_HEAD
  A.up = true; age = std::fmod(animT, kDwell * 2.f) * 0.5f + 6.f;
#endif
  if (!A.up) return A;
  A.age = age;

  // Rise, hold, withdraw.
  float rise;
  if (age < 5.f)                 rise = age / 5.f;
  else if (age < kDwell - 6.f)   rise = 1.f;
  else                           rise = std::max(0.f, (kDwell - age) / 6.f);
  rise = rise * rise * (3.f - 2.f * rise);
  A.rise = rise;

  A.side = ((hh >> 20) & 1u) ? 1 : -1;
  A.rx = (float)W * (0.15f + 0.04f * (float)((hh >> 4) & 15u) / 15.f);
  A.ry = A.rx * 1.35f;
  // It leans in around the rim: off the panel entirely when hidden, head and
  // one shoulder inside the circle when fully out, the rest of it still
  // beyond the edge (the circle mask does the cropping for us).
  float inX  = (float)W * (A.side < 0 ? 0.26f : 0.74f);
  float outX = (A.side < 0) ? -A.rx * 1.6f : (float)W + A.rx * 1.6f;
  A.cx = outX + (inX - outX) * rise;
  A.cy = (float)H * (0.40f + 0.03f * std::sin(age * 0.35f));   // it shifts a little
  A.tilt = (float)-A.side * 0.26f * rise;                      // cocked inward
  // A hand comes round the rim below the head to steady itself.
  {
    float cc = (float)W * 0.5f - 0.5f, R = (float)W * 0.5f;
    float baseAng = (A.side < 0) ? 3.14159f : 0.f;
    float ang = baseAng + (float)-A.side * (0.62f + 0.06f * std::sin(age * 0.5f));
    float rr3 = R * (0.995f - 0.16f * rise);
    A.handX = cc + rr3 * std::cos(ang);
    A.handY = cc + rr3 * std::sin(ang);
  }

  // It looks around in long sweeps, with the odd slow blink.
  A.gaze = std::sin(age * 0.42f) * 0.75f + 0.25f * std::sin(age * 0.17f + 1.3f);
  float bph = std::sin(age * 0.9f + (float)(hh & 31u));
  A.blink = (bph > 0.972f) ? 0.12f : 1.f;
  return A;
}

// ---------------------------------------------------------------------
// Biome identity
// ---------------------------------------------------------------------
// The plant glyphs are shared, so for a long time meadow, wetland, alpine
// and tropical differed only in how MUCH of each grew — four variations on
// one green. Each terrestrial biome now grades its own land: a whole-palette
// tint applied after the per-glyph colour, which keeps the plant families
// while making the biome recognisable from across the room.
//
// (ALIEN and CITY don't come through here — they replace the palette
// outright.)
inline void pixelviewBiomeGrade(Biome bi, int& r, int& g, int& b) {
  float fr = (float)r, fg = (float)g, fb = (float)b;
  switch (bi) {
    case MEADOW:
      // Sunlit hay: warm yellow-greens, the blue pulled right out.
      fr *= 1.12f; fg *= 1.08f; fb *= 0.76f;
      fr += 8.f; fg += 7.f;
      break;
    case WETLAND:
      // Peat and shade: deep blue-greens, everything a stop darker.
      fr *= 0.76f; fg *= 0.98f; fb *= 1.12f;
      fb += 10.f; fg += 4.f;
      break;
    case ALPINE: {
      // Thin air: sage and lichen, desaturated toward blue-grey rock.
      float grey = 0.30f * fr + 0.59f * fg + 0.11f * fb;
      fr = fr * 0.58f + grey * 0.42f;
      fg = fg * 0.62f + grey * 0.38f;
      fb = fb * 0.58f + grey * 0.42f;
      fr *= 0.94f; fb *= 1.14f;
      fb += 12.f;
      break;
    }
    case TROPICAL:
      // Rainforest: saturated jade, dark under the canopy.
      fr *= 0.78f; fg *= 1.12f; fb *= 0.88f;
      fg += 6.f;
      break;
    case DESERT:
      // Everything bakes: ochre and rose, greens only in the cactus.
      fr *= 1.10f; fg *= 1.00f; fb *= 0.78f;
      fr += 8.f; fg += 3.f;
      break;
    default:
      return;
  }
  r = (int)fr; g = (int)fg; b = (int)fb;
}

// Bare ground differs as much as the planting does: pale tan under a meadow,
// black peat in a bog, grey scree on a mountain.
inline void pixelviewBiomeSoil(Biome bi, int j, int& r, int& g, int& b) {
  switch (bi) {
    case MEADOW:   r = 68 + j / 2; g = 64 + j / 2; b = 42; break;  // dry olive
    case WETLAND:  r = 48 + j / 2; g = 46 + j / 2; b = 38; break;  // peat
    case ALPINE:   r = 92 + j / 2; g = 96 + j / 2; b = 104; break; // scree
    case TROPICAL: r = 52 + j / 2; g = 40 + j / 2; b = 30; break;  // dark loam
    case DESERT:   r = 178 + j / 2; g = 148 + j / 2; b = 104; break;
    default:       r = 26 + j / 2; g = 22 + j / 2; b = 18; break;
  }
}

// ...and so does the water. One blue ramp served every biome; a tannin bog,
// a glacial tarn and a coral shallow are not the same colour of water.
inline void pixelviewBiomeWater(Biome bi, int depth, float& fr, float& fg,
                                float& fb) {
  switch (bi) {
    case WETLAND: {   // tannin: brown-green, and you cannot see into it
      float t = 0.72f;
      fr = fr * (1.f - t) + (44.f + 5.f * (float)depth) * t;
      fg = fg * (1.f - t) + (62.f + 3.f * (float)depth) * t;
      fb = fb * (1.f - t) + (40.f + 2.f * (float)depth) * t;
      break;
    }
    case ALPINE:      // glacial: pale milky cyan in the shallows
      fr *= 0.92f; fg *= 1.14f; fb *= 1.10f;
      fr += 18.f; fg += 26.f; fb += 14.f;
      break;
    case TROPICAL:    // reef turquoise
      fr *= 0.80f; fg *= 1.20f; fb *= 1.06f;
      fg += 14.f;
      break;
    case DESERT:      // an oasis is jade, not ocean
      fr *= 0.90f; fg *= 1.16f; fb *= 0.86f;
      fg += 10.f;
      break;
    case MEADOW:      // a clear pond takes the sky
      fg += 6.f; fb += 10.f;
      break;
    default:
      break;
  }
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
    {  // each biome's water is its own colour, before any motion is added
      float fr2 = (float)r, fg2 = (float)g, fb2 = (float)b;
      pixelviewBiomeWater(w.biome, dd, fr2, fg2, fb2);
      r = (int)fr2; g = (int)fg2; b = (int)fb2;
    }
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
        else pixelviewBiomeSoil(w.biome, j, r, g, b);            // earth
        break;
    }
    if (t == KELP_GLYPH) { r = 24; g = 140 + j; b = 110; }
    // Whole-palette biome grade over the foliage, soil and rock. The
    // accents stay true: the wildflower distribution and the fly-agaric caps
    // are chosen colours, and grading them turned white petals yellow and
    // every desert bloom orange.
    bool accent = (t == 'f' || t == '+' || t == '&' || t == '!' || t == 'm' ||
                   t == '$' || t == '*' || t == 'V' || t == 'C');
    if (!accent && w.biome != CITY) pixelviewBiomeGrade(w.biome, r, g, b);
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

    // Traffic: driven agents, looked up from this frame's occupancy grid.
    if (t == CITY_ROAD || t == CITY_BRIDGE) {
      const CityTraffic& T = pixelviewTraffic(w, animT);
      uint8_t occ = T.cell[(size_t)y * W + x];
      if (occ) {
        uint32_t ch = hash3((uint32_t)occ, w.worldSeed, 0xCA25u);
        bool heading = (T.dirOf[(size_t)y * W + x] == 1u);
        uint32_t kind = ch % 100u;
        if (night > 0.35f) {
          // Lamps, not paintwork: what you see of a car at night.
          if (kind < 4u) {                       // emergency, running blue
            float f = std::sin(animT * 9.f + (float)(ch & 31u)) > 0.f ? 1.f : 0.25f;
            rr = 60.f * br * f; gg = 110.f * br * f; bb = 255.f * br * f;
          } else if (kind < 12u) {               // a taxi's roof sign
            rr = 255.f * br; gg = 196.f * br; bb = 70.f * br;
          } else if (heading) {
            if (kind < 56u) { rr = 255.f * br; gg = 238.f * br; bb = 200.f * br; }
            else            { rr = 226.f * br; gg = 238.f * br; bb = 255.f * br; }  // xenon
          } else {
            if (kind < 60u) { rr = 240.f * br; gg = 52.f * br;  bb = 40.f * br; }
            else            { rr = 245.f * br; gg = 120.f * br; bb = 40.f * br; }   // amber
          }
        } else {
          switch (kind % 11u) {                  // daylight paintwork
            case 0:  rr = 232.f; gg = 236.f; bb = 240.f; break;  // white
            case 1:  rr = 44.f;  gg = 46.f;  bb = 54.f;  break;  // black
            case 2:  rr = 198.f; gg = 58.f;  bb = 52.f;  break;  // red
            case 3:  rr = 62.f;  gg = 104.f; bb = 188.f; break;  // blue
            case 4:  rr = 246.f; gg = 202.f; bb = 58.f;  break;  // taxi yellow
            case 5:  rr = 132.f; gg = 140.f; bb = 146.f; break;  // silver
            case 6:  rr = 54.f;  gg = 132.f; bb = 96.f;  break;  // green
            case 7:  rr = 236.f; gg = 138.f; bb = 62.f;  break;  // orange bus
            case 8:  rr = 88.f;  gg = 176.f; bb = 186.f; break;  // teal
            case 9:  rr = 176.f; gg = 84.f;  bb = 152.f; break;  // plum
            default: rr = 214.f; gg = 208.f; bb = 190.f; break;  // cream van
          }
          rr *= br; gg *= br; bb *= br;
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

    // ...and, rarely, something leans in around the edge of the world to see
    // what is in here. The circle mask crops whatever is still outside the
    // rim, which is what sells it as peering round a corner.
    const AlienHead& A = pixelviewAlienHead(w, animT);
    if (A.up && A.rise > 0.01f) {
      const float fxx = (float)x, fyy = (float)y;
      // Cheap reject: everything below is confined to this box.
      float bx0 = std::min(A.cx, A.handX) - A.rx * 2.6f;
      float bx1 = std::max(A.cx, A.handX) + A.rx * 2.6f;
      float by0 = A.cy - A.ry * 1.8f;
      float by1 = std::max(A.cy + A.ry * 3.4f, A.handY + A.rx * 1.2f);
      if (fxx > bx0 && fxx < bx1 && fyy > by0 && fyy < by1) {
        auto ellipse = [&](float cx2, float cy2, float rx2, float ry2, float rot) {
          float dx2 = fxx - cx2, dy2 = fyy - cy2;
          float c = std::cos(rot), s2 = std::sin(rot);
          float a2 = dx2 * c + dy2 * s2, b2 = -dx2 * s2 + dy2 * c;
          return (a2 * a2) / (rx2 * rx2) + (b2 * b2) / (ry2 * ry2);
        };
        auto capsule = [&](float x0, float y0, float x1, float y1, float rad) {
          float vx = x1 - x0, vy = y1 - y0;
          float len2 = vx * vx + vy * vy;
          float tt = (len2 > 0.f) ? ((fxx - x0) * vx + (fyy - y0) * vy) / len2 : 0.f;
          tt = std::clamp(tt, 0.f, 1.f);
          float px2 = x0 + vx * tt - fxx, py2 = y0 + vy * tt - fyy;
          return (px2 * px2 + py2 * py2) / (rad * rad);
        };

        // --- head, in its own tilted frame ---
        float ct = std::cos(A.tilt), st = std::sin(A.tilt);
        float hx = (fxx - A.cx) * ct + (fyy - A.cy) * st;
        float hy = -(fxx - A.cx) * st + (fyy - A.cy) * ct;
        float v = hy / A.ry;
        float taper = 1.f - 0.62f * std::max(0.f, v);
        float u = hx / (A.rx * std::max(0.18f, taper));
        float dHead = u * u + v * v;

        // --- shoulder / torso, still mostly beyond the rim ---
        float shX = A.cx - (float)A.side * A.rx * 0.85f;
        float shY = A.cy + A.ry * 1.30f;
        float dBody = ellipse(shX, shY, A.rx * 1.75f, A.ry * 1.15f,
                              (float)-A.side * 0.35f);
        // --- neck ---
        float dNeck = capsule(A.cx, A.cy + A.ry * 0.72f, shX, shY, A.rx * 0.30f);
        // --- arm reaching along the rim to the hand ---
        float elbowX = (shX + A.handX) * 0.5f + (float)A.side * A.rx * 0.30f;
        float elbowY = (shY + A.handY) * 0.5f + A.ry * 0.55f;
        float dArm = std::min(capsule(shX, shY, elbowX, elbowY, A.rx * 0.24f),
                              capsule(elbowX, elbowY, A.handX, A.handY, A.rx * 0.19f));
        // --- hand: palm plus long fingers curling in over the edge ---
        float inwX = (float)W * 0.5f - A.handX, inwY = (float)H * 0.5f - A.handY;
        float inwL = std::sqrt(inwX * inwX + inwY * inwY);
        if (inwL > 0.001f) { inwX /= inwL; inwY /= inwL; }
        float tanX = -inwY, tanY = inwX;
        float dHand = ellipse(A.handX, A.handY, A.rx * 0.34f, A.rx * 0.26f, 0.f);
        for (int fgr = 0; fgr < 4; ++fgr) {
          float off = ((float)fgr - 1.5f) * A.rx * 0.17f;
          float bxf = A.handX + tanX * off, byf = A.handY + tanY * off;
          float tipLen = A.rx * (0.52f - 0.06f * std::fabs((float)fgr - 1.5f));
          float txf = bxf + inwX * tipLen, tyf = byf + inwY * tipLen;
          dHand = std::min(dHand, capsule(bxf, byf, txf, tyf, A.rx * 0.072f));
        }

        // Head only, for now. A full figure (shoulder, arm, a hand round the
        // rim) is computed above but reads as one huge blob at this scale:
        // the torso lands well inside the circle instead of being cropped by
        // it. Left in place, out of the silhouette, until it earns its keep.
        (void)dBody; (void)dNeck; (void)dArm; (void)dHand;
        float dAny = dHead;

        // Atmosphere: it displaces the air it leans through.
        if (dAny >= 1.f && dAny < 2.0f) {
          float haze = (1.f - (dAny - 1.f) / 1.0f) * 0.32f * A.rise;
          rr = rr * (1.f - haze) + 26.f * haze;
          gg = gg * (1.f - haze) + 30.f * haze;
          bb = bb * (1.f - haze) + 44.f * haze;
        }
        if (dAny < 1.f) {
          // Near-black, with a faint rim where the world's glow catches it.
          float rim = std::max(0.f, (dAny - 0.70f) / 0.30f);
          float bR = 12.f + 42.f * rim, bG = 14.f + 50.f * rim, bB = 22.f + 70.f * rim;
          rr = rr * (1.f - A.rise) + bR * A.rise;
          gg = gg * (1.f - A.rise) + bG * A.rise;
          bb = bb * (1.f - A.rise) + bB * A.rise;
        }

        // Eyes last, so nothing paints over them.
        if (dHead < 1.2f) {
          for (int sd = -1; sd <= 1; sd += 2) {
            float ex = (0.44f * (float)sd + A.gaze * 0.10f) * A.rx;
            float ey = -0.10f * A.ry;
            float dx2 = hx - ex, dy2 = hy - ey;
            float sl = 0.42f * (float)sd;
            float rxq = dx2 * std::cos(sl) - dy2 * std::sin(sl);
            float ryq = dx2 * std::sin(sl) + dy2 * std::cos(sl);
            float exr = A.rx * 0.30f, eyr = A.ry * 0.13f * A.blink;
            float q = (rxq * rxq) / (exr * exr) + (ryq * ryq) / (eyr * eyr);
            if (q < 1.f) {
              float core = 1.f - q;
              float pulse = 0.72f + 0.28f * std::sin(animT * 2.1f);
              float p = A.rise * pulse * br * (0.35f + 0.65f * core);
              rr = rr * (1.f - p) + 120.f * p;
              gg = gg * (1.f - p) + 246.f * p;
              bb = bb * (1.f - p) + 238.f * p;
              if (q < 0.22f) { rr += 60.f * p; gg += 20.f * p; bb += 40.f * p; }
            }
          }
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

  // Offshore cast: the ship on her endless circuit (lantern-lit at night),
  // seabirds skimming the shore by day, an occasional whale, and — rarely
  // — the serpent's humps arcing through the deep. Island mode and the open
  // OCEAN biome both get it.
  if (hasOpenSea(w)) {
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

  // Brightness lift (live). A 256-entry table, rebuilt only when the setting
  // moves, keeps this off the per-cell pow() path — it runs on a Pi Zero.
  {
    float lift = displayLift();
    if (lift > 1.001f) {
      static float cachedLift = -1.f;
      static float lut[256];
      if (cachedLift != lift) {
        cachedLift = lift;
        for (int i = 0; i < 256; ++i)
          lut[i] = 255.f * (1.f - std::pow(1.f - (float)i / 255.f, lift));
      }
      rr = lut[clampU8((int)rr)];
      gg = lut[clampU8((int)gg)];
      bb = lut[clampU8((int)bb)];
    }
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
