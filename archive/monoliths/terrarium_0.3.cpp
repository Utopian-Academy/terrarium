#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// Terrarium 0.3 (SDL2) - Seasonal, windy, colorful, esoteric
// Layers:
//   terrain: '.', ',', '"', '#', 'T','Y', 'm', 'f','+','&', ';', ':', '^', '*','x'
//   entities:' ' none, 'b' bug, 'v' bird, 'F' firefly, 'H' yeti, 'A' alien
//   water depth: 0..7
// Render priority: entity > rainbow overlay > water > terrain
//
// Controls:
//   SPACE pause/unpause
//   .     step (paused)
//   [ ]   speed down/up
//   r     reset
//   ESC   quit
// ============================================================

static constexpr int W = 200;
static constexpr int H = 112;

static constexpr int DEFAULT_TPS = 4;

// Events
static constexpr int EVENT_CHANCE = 1300;     // lower = more chaos
static constexpr int EVENT_INTENSITY = 320;

// Seasons
static constexpr int SEASON_TICKS = 1800;     // ~ 7.5 minutes at 4 tps
// Day/night-ish (for fireflies glow feel; just a phase)
static constexpr int DAY_TICKS = 900;

// Wind
static constexpr int WIND_CHANGE_TICKS = 240; // how often wind can shift
static constexpr int MAX_WIND = 4;            // 0..4 strength

using Grid = std::vector<std::string>;
using Water = std::vector<std::vector<uint8_t>>;
using Overlay = std::vector<std::string>; // render-only overlay; ' ' = none

static inline bool inBounds(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

struct Rng {
  std::mt19937 rng;
  explicit Rng(uint32_t seed) : rng(seed) {}
  int i(int a, int b) { std::uniform_int_distribution<int> d(a, b); return d(rng); }
  bool oneIn(int n) { return n > 0 && i(1, n) == 1; }
  double u01() { std::uniform_real_distribution<double> d(0.0, 1.0); return d(rng); }
};

static inline uint32_t hash2(uint32_t x, uint32_t y, uint32_t salt) {
  uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA6Bu ^ salt * 0xC2B2AE35u;
  h ^= (h >> 16);
  h *= 0x7FEB352Du;
  h ^= (h >> 15);
  h *= 0x846CA68Bu;
  h ^= (h >> 16);
  return h;
}

static int countNeighborsChar(const Grid& g, int x, int y, char c) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx, ny = y + dy;
      if (inBounds(nx, ny) && g[ny][nx] == c) n++;
    }
  return n;
}

static int countNeighborsWater(const Water& w, int x, int y) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx, ny = y + dy;
      if (inBounds(nx, ny) && w[ny][nx] > 0) n++;
    }
  return n;
}

static bool isTree(char c) { return c == 'T' || c == 'Y'; }
static bool isVeg(char c) {
  return (c == ',' || c == '"' || c == '#' || isTree(c) || c == 'm' || c == 'f' || c == '+' || c == '&' || c == ';' || c == ':' );
}
static bool blocksEntity(char terrain, uint8_t waterDepth) {
  if (waterDepth > 0) return true;
  if (terrain == '*') return true; // fire
  return false;
}

struct Wind {
  // dir as dx,dy in {-1,0,1} excluding 0,0; strength 0..MAX_WIND
  int dx = 0, dy = 0;
  int strength = 0;
};

struct World {
  Grid terrain;
  Grid entities;
  Water water;
  Overlay overlay; // rainbow etc
  Wind wind;
};

// Water depth glyphs (DF-ish; ASCII safe)
static char waterGlyph(uint8_t d) {
  switch (d) {
    case 1: return ','; // shallow
    case 2: return '-';
    case 3: return '~';
    case 4: return '=';
    case 5: return '#';
    case 6: return '%';
    default: return '@'; // 7
  }
}

// ---------------- Seeding ----------------
static void seedWorld(World& w, Rng& r) {
  w.terrain.assign(H, std::string(W, '.'));
  w.entities.assign(H, std::string(W, ' '));
  w.overlay.assign(H, std::string(W, ' '));
  w.water.assign(H, std::vector<uint8_t>(W, 0));
  w.wind = Wind{0, 0, 0};

  // Ponds
  int ponds = std::max(4, (W * H) / 9000);
  for (int p = 0; p < ponds; ++p) {
    int marginX = std::max(12, W / 18);
    int marginY = std::max(8,  H / 18);
    int cx = r.i(marginX, W - 1 - marginX);
    int cy = r.i(marginY, H - 1 - marginY);
    int rad = r.i(5, 13);

    for (int y = cy - rad; y <= cy + rad; ++y)
      for (int x = cx - rad; x <= cx + rad; ++x) {
        if (!inBounds(x, y)) continue;
        int dx = x - cx, dy = y - cy;
        int d2 = dx*dx + dy*dy;
        if (d2 <= rad*rad + r.i(-4, 4)) {
          uint8_t depth = (uint8_t)std::clamp(7 - (d2 / std::max(1, rad)), 2, 7);
          w.water[y][x] = std::max<uint8_t>(w.water[y][x], depth);
        }
      }
  }

  // Grass near water; reeds at shoreline; stones scattered
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    if (w.water[y][x] > 0) continue;
    int wet = countNeighborsWater(w.water, x, y);
    if (wet > 0 && r.oneIn(2)) w.terrain[y][x] = ',';
    if (wet > 0 && r.oneIn(10)) w.terrain[y][x] = ':'; // reeds
    if (wet == 0 && r.oneIn(420)) w.terrain[y][x] = '^'; // stones
  }

  // Shrubs/trees/ferns
  for (int k = 0; k < (W * H) / 700; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && w.terrain[y][x] == ',' && r.oneIn(3)) w.terrain[y][x] = '#';
  }
  for (int k = 0; k < (W * H) / 2300; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && (w.terrain[y][x] == '#' || w.terrain[y][x] == ',') && r.oneIn(3))
      w.terrain[y][x] = (r.oneIn(2) ? 'T' : 'Y');
  }
  for (int k = 0; k < (W * H) / 900; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && (w.terrain[y][x] == ',' || w.terrain[y][x] == '#') && r.oneIn(3))
      w.terrain[y][x] = ';'; // ferns
  }

  // Mushrooms + flowers
  for (int k = 0; k < (W * H) / 700; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && (w.terrain[y][x] == '.' || w.terrain[y][x] == ',') && countNeighborsWater(w.water, x, y) > 0 && r.oneIn(2))
      w.terrain[y][x] = 'm';
  }
  for (int k = 0; k < (W * H) / 900; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && (w.terrain[y][x] == ',' || w.terrain[y][x] == '"' || w.terrain[y][x] == ';') && r.oneIn(2))
      w.terrain[y][x] = (r.oneIn(3) ? '&' : (r.oneIn(2) ? 'f' : '+'));
  }
}

// ---------------- Seasons ----------------
enum Season { SPRING=0, SUMMER=1, AUTUMN=2, WINTER=3 };

static inline Season seasonAt(int tick) {
  int s = (tick / SEASON_TICKS) % 4;
  return (Season)s;
}
static inline float seasonLerp(int tick) {
  // 0..1 progress through current season
  int t = tick % SEASON_TICKS;
  return (float)t / (float)SEASON_TICKS;
}

// A simple tint function: base rgb -> season-tinted rgb
static inline void applySeasonTint(uint8_t& r, uint8_t& g, uint8_t& b, Season s, float p) {
  // p used to smooth transition within season; also blends toward next season
  auto clampU8 = [](int v){ return (uint8_t)std::clamp(v, 0, 255); };

  // Define target offsets per season
  // (these are subtle; we rely on per-tile color variety too)
  struct Off { int dr,dg,db; };
  Off offA, offB;

  auto offsFor = [&](Season ss)->Off{
    switch (ss) {
      case SPRING: return Off{ -4, +18, -4 };
      case SUMMER: return Off{ +6, +10, +0 };
      case AUTUMN: return Off{ +18, -2, -8 };
      case WINTER: return Off{ -10, -6, +10 };
      default:     return Off{0,0,0};
    }
  };

  Season s2 = (Season)((s + 1) % 4);
  offA = offsFor(s);
  offB = offsFor(s2);

  float t = p; // 0..1 toward next season

  int rr = (int)r + (int)(offA.dr * (1.0f - t) + offB.dr * t);
  int gg = (int)g + (int)(offA.dg * (1.0f - t) + offB.dg * t);
  int bb = (int)b + (int)(offA.db * (1.0f - t) + offB.db * t);

  // Winter desaturation
  if (s == WINTER || s2 == WINTER) {
    int gray = (rr + gg + bb) / 3;
    float wgt = (s == WINTER) ? (0.35f + 0.35f*t) : (0.35f*(1.0f-t));
    rr = (int)(rr*(1.0f-wgt) + gray*wgt);
    gg = (int)(gg*(1.0f-wgt) + gray*wgt);
    bb = (int)(bb*(1.0f-wgt) + gray*wgt);
  }

  r = clampU8(rr); g = clampU8(gg); b = clampU8(bb);
}

// ---------------- Wind evolution ----------------
static void updateWind(World& w, Rng& r, int tick) {
  if (tick % WIND_CHANGE_TICKS != 0) return;

  // small drift in wind; sometimes calm, sometimes gusty
  if (r.oneIn(5)) { w.wind.strength = 0; w.wind.dx = 0; w.wind.dy = 0; return; }

  w.wind.strength = std::clamp(w.wind.strength + r.i(-1, 2), 0, MAX_WIND);

  // pick direction occasionally
  if (w.wind.strength == 0) { w.wind.dx = 0; w.wind.dy = 0; return; }

  if (r.oneIn(2) || (w.wind.dx == 0 && w.wind.dy == 0)) {
    int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
    int k = r.i(0,7);
    w.wind.dx = dirs[k][0];
    w.wind.dy = dirs[k][1];
  }
}

// ---------------- Big rainbow overlay ----------------
static void clearOverlay(World& w) {
  for (int y = 0; y < H; ++y) std::fill(w.overlay[y].begin(), w.overlay[y].end(), ' ');
}

static void spawnRainbow(World& w, Rng& r) {
  clearOverlay(w);

  // Big arc across screen
  int cx = W / 2 + r.i(-W/10, W/10);
  int cy = H + r.i(H/6, H/3);           // center below horizon so arc shows
  int R  = std::min(W, H) + r.i(10, 40);

  // thickness
  int thick = 2 + r.i(0, 2);

  // use multiple symbols for shimmer
  const char bands[] = {'=', '-', '~', '+'}; // pseudo bands
  int nb = (int)(sizeof(bands)/sizeof(bands[0]));

  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    int dx = x - cx, dy = y - cy;
    int d2 = dx*dx + dy*dy;
    int r0 = R;
    int r1 = R - thick;

    if (d2 <= r0*r0 && d2 >= r1*r1) {
      // band by angle-ish
      int band = (x + y) % nb;
      w.overlay[y][x] = bands[band];
    }
  }
}

// ---------------- Chaos events ----------------
static void chaosRain(World& w, Rng& r, std::string& banner) {
  banner = "Rain: water rises; bloom; chance of rainbow";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] < 7 && r.oneIn(2)) w.water[y][x]++;

    if (w.water[y][x] == 0 && countNeighborsWater(w.water, x, y) > 0) {
      char &t = w.terrain[y][x];
      if ((t == '.' || t == ',') && r.oneIn(8)) t = 'm';
      if ((t == ',' || t == '"' || t == ';') && r.oneIn(14)) t = (r.oneIn(3) ? '&' : (r.oneIn(2) ? 'f' : '+'));
      if (t == '.' && r.oneIn(18)) t = ':'; // reeds after rain
    }
  }

  // chance to spawn a big rainbow overlay after rain
  if (r.oneIn(3)) spawnRainbow(w, r);
}

static void chaosStormLightning(World& w, Rng& r, std::string& banner) {
  banner = "Storm: lightning strikes (windy)";
  // gust up a bit
  w.wind.strength = std::min(MAX_WIND, w.wind.strength + 1);

  int strikes = 2 + r.i(0, 4);
  for (int s = 0; s < strikes; ++s) {
    int cx = r.i(0, W - 1), cy = r.i(0, H - 1);
    for (int k = 0; k < EVENT_INTENSITY / 3; ++k) {
      int x = cx + r.i(-18, 18), y = cy + r.i(-10, 10);
      if (!inBounds(x, y)) continue;
      if (w.water[y][x] > 0) continue;
      if (isVeg(w.terrain[y][x]) && r.oneIn(2)) w.terrain[y][x] = '*';
    }
  }
}

static void chaosDrought(World& w, Rng& r, std::string& banner) {
  banner = "Drought: water falls; flowers fade";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] > 0 && r.oneIn(2)) w.water[y][x]--;
    char &t = w.terrain[y][x];
    if ((t == 'm' || t == 'f' || t == '+' || t == '&') && r.oneIn(3)) t = (r.oneIn(2) ? ',' : '.');
    if (t == ':' && r.oneIn(5)) t = '.';
  }
  // rainbow fades
  if (r.oneIn(2)) clearOverlay(w);
}

static void chaosBlight(World& w, Rng& r, std::string& banner) {
  banner = "Blight: vegetation collapses";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] > 0) continue;
    char &t = w.terrain[y][x];
    if (isVeg(t) && r.oneIn(2)) t = (r.oneIn(3) ? 'x' : '.');
  }
}

static void chaosAlien(World& w, Rng& r, std::string& banner) {
  banner = "Alien: reality flexes";
  for (int tries = 0; tries < 900; ++tries) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] > 0) continue;
    if (w.entities[y][x] == ' ') { w.entities[y][x] = 'A'; break; }
  }
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (r.oneIn(10)) w.water[y][x] = (uint8_t)r.i(0, 7);
    if (w.water[y][x] == 0) {
      char &t = w.terrain[y][x];
      if (t == '.' && r.oneIn(3)) t = ',';
      else if (t == ',' && r.oneIn(4)) t = (r.oneIn(2) ? '"' : (r.oneIn(2) ? ';' : ':'));
      else if (t == '"' && r.oneIn(6)) t = '#';
      else if (t == '#' && r.oneIn(8)) t = (r.oneIn(2) ? 'T' : 'Y');
      else if (isTree(t) && r.oneIn(18)) t = '*';
    }
  }
}

static void maybeChaos(World& w, Rng& r, std::string& banner, Season s) {
  if (!r.oneIn(EVENT_CHANCE)) { banner = "calm"; return; }

  // seasonal bias: spring rain, summer storms, autumn mushrooms, winter calm
  int roll = r.i(1, 100);
  if (s == SPRING) {
    if (roll <= 55) chaosRain(w, r, banner);
    else if (roll <= 70) chaosBlight(w, r, banner);
    else chaosAlien(w, r, banner);
  } else if (s == SUMMER) {
    if (roll <= 40) chaosRain(w, r, banner);
    else if (roll <= 70) chaosStormLightning(w, r, banner);
    else if (roll <= 85) chaosDrought(w, r, banner);
    else chaosAlien(w, r, banner);
  } else if (s == AUTUMN) {
    if (roll <= 45) chaosRain(w, r, banner);
    else if (roll <= 70) chaosBlight(w, r, banner);
    else chaosDrought(w, r, banner);
  } else { // WINTER
    if (roll <= 25) chaosStormLightning(w, r, banner);
    else if (roll <= 45) chaosDrought(w, r, banner);
    else banner = "winter hush";
  }
}

// ---------------- Water flow (windy slosh) ----------------
static void stepWater(World& w, Rng& r) {
  Water next = w.water;

  int baseMoves = (W * H) / 2;               // baseline motion
  int windMoves = (W * H) / 6 * w.wind.strength; // extra sideways motion
  int moves = baseMoves + windMoves;

  for (int k = 0; k < moves; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    uint8_t d = next[y][x];
    if (d == 0) continue;

    // pick a direction biased by gravity (down) + wind (dx,dy)
    int dirs[6][2] = {
      {0, 1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}, {0, -1}
    };

    // choose target
    int bestNx = x, bestNy = y;
    int bestScore = 999999;

    for (int i = 0; i < 6; ++i) {
      int nx = x + dirs[i][0];
      int ny = y + dirs[i][1];
      if (!inBounds(nx, ny)) continue;

      int score = (int)next[ny][nx] * 10 + i;

      // wind bias: prefer moving along wind direction when windy
      if (w.wind.strength > 0) {
        int wx = w.wind.dx, wy = w.wind.dy;
        int dot = dirs[i][0]*wx + dirs[i][1]*wy; // -2..2
        score -= dot * (2 + w.wind.strength);    // reduce score -> prefer
      }

      // tiny jitter
      score += r.i(0, 4);

      if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
    }

    if (bestNx == x && bestNy == y) continue;

    uint8_t nd = next[bestNy][bestNx];

    if (nd + 1 < d) {
      next[y][x]--;
      next[bestNy][bestNx]++;
    } else if (w.wind.strength >= 3 && r.oneIn(8) && nd < d) {
      // gust slosh
      next[y][x]--;
      next[bestNy][bestNx]++;
    } else if (r.oneIn(12) && nd < d) {
      next[y][x]--;
      next[bestNy][bestNx]++;
    }
  }

  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      next[y][x] = (uint8_t)std::min<int>(7, next[y][x]);

  w.water.swap(next);
}

// ---------------- Terrain ecology (more plants) ----------------
static void stepTerrain(World& w, Rng& r, Season s) {
  Grid next = w.terrain;

  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    // water suppresses fire
    if (w.water[y][x] > 0) {
      if (w.terrain[y][x] == '*') next[y][x] = 'x';
      continue;
    }

    char c = w.terrain[y][x];

    // Fire/ash
    if (c == '*') { next[y][x] = (r.oneIn(3) ? 'x' : '*'); continue; }
    if (c == 'x') {
      int wet = countNeighborsWater(w.water, x, y);
      if (wet > 0 && r.oneIn(6)) next[y][x] = ',';
      else if (r.oneIn(35)) next[y][x] = '.';
      continue;
    }

    // fire spread (wind-influenced)
    if (isVeg(c)) {
      bool nearFire = false;
      for (int dy = -1; dy <= 1 && !nearFire; ++dy)
        for (int dx = -1; dx <= 1 && !nearFire; ++dx) {
          if (dx==0 && dy==0) continue;
          int nx = x+dx, ny=y+dy;
          if (!inBounds(nx,ny)) continue;
          if (w.terrain[ny][nx] == '*') {
            // downwind helps ignition
            int dot = dx*w.wind.dx + dy*w.wind.dy;
            int ignite = 5;
            if (w.wind.strength > 0) ignite += std::max(0, dot) * w.wind.strength;
            if (r.oneIn(std::max(2, 10 - ignite))) nearFire = true;
          }
        }
      if (nearFire) { next[y][x] = '*'; continue; }
    }

    int wet = countNeighborsWater(w.water, x, y);
    int g  = countNeighborsChar(w.terrain, x, y, ',');
    int tg = countNeighborsChar(w.terrain, x, y, '"');
    int sh = countNeighborsChar(w.terrain, x, y, '#');
    int trees = countNeighborsChar(w.terrain, x, y, 'T') + countNeighborsChar(w.terrain, x, y, 'Y');
    int flo = countNeighborsChar(w.terrain, x, y, 'f') + countNeighborsChar(w.terrain, x, y, '+') + countNeighborsChar(w.terrain, x, y, '&');
    int fern = countNeighborsChar(w.terrain, x, y, ';');
    int reeds = countNeighborsChar(w.terrain, x, y, ':');

    // seasonal multipliers
    int springBoost = (s == SPRING) ? 2 : 1;
    int autumnMush  = (s == AUTUMN) ? 2 : 1;
    int winterSlow  = (s == WINTER) ? 2 : 1;

    // Empty black space
    if (c == '.') {
      int fert = wet * 3 + g + tg + flo + fern;
      if (fert > 0 && r.u01() < (0.0032f * fert) / (float)winterSlow) next[y][x] = ',';

      // shoreline reeds
      if (wet > 0 && r.oneIn(20 * winterSlow)) next[y][x] = ':';

      // stones persist
      if (r.oneIn(5000)) next[y][x] = '^';

      continue;
    }

    // Stones
    if (c == '^') {
      // mossy reclaim
      if (wet > 0 && r.oneIn(220)) next[y][x] = ',';
      continue;
    }

    // Reeds
    if (c == ':') {
      if (wet == 0 && r.oneIn(18 * winterSlow)) next[y][x] = '.';
      if (wet > 0 && r.oneIn(40)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx,ny) && w.water[ny][nx] == 0 && w.terrain[ny][nx] == '.') next[ny][nx] = ':';
      }
      continue;
    }

    // Ferns
    if (c == ';') {
      if (wet == 0 && r.oneIn(30 * winterSlow)) next[y][x] = ',';
      if (wet > 0 && r.oneIn(55)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx,ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == ',' || w.terrain[ny][nx] == '.')) next[ny][nx] = ';';
      }
      continue;
    }

    // Grass
    if (c == ',') {
      if ((g + tg) >= 4 && r.oneIn(90 * winterSlow)) next[y][x] = '"';
      if (wet > 0 && r.oneIn(180 / springBoost)) next[y][x] = (r.oneIn(3) ? '&' : (r.oneIn(2) ? 'f' : '+'));
      if (wet > 0 && r.oneIn(260)) next[y][x] = ';';
      if ((wet + g + tg + reeds) == 0 && r.oneIn(75 * winterSlow)) next[y][x] = '.';
      continue;
    }

    // Tall grass
    if (c == '"') {
      if ((g + tg) >= 5 && r.oneIn(140 * winterSlow)) next[y][x] = '#';
      if (wet > 0 && r.oneIn(220 / springBoost)) next[y][x] = (r.oneIn(3) ? '&' : (r.oneIn(2) ? 'f' : '+'));
      if (wet == 0 && r.oneIn(120 * winterSlow)) next[y][x] = ',';
      continue;
    }

    // Shrub
    if (c == '#') {
      if ((sh + trees) >= 3 && wet > 0 && r.oneIn(280 * winterSlow)) next[y][x] = (r.oneIn(2) ? 'T' : 'Y');
      if (wet == 0 && r.oneIn(170 * winterSlow)) next[y][x] = '"';
      if (wet > 0 && r.oneIn(240 / autumnMush)) next[y][x] = 'm';
      continue;
    }

    // Trees
    if (c == 'T' || c == 'Y') {
      // drop mushrooms/flowers/ferns nearby
      if (wet > 0 && r.oneIn(230)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx,ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == '.' || w.terrain[ny][nx] == ',')) {
          int pick = r.i(1, 6);
          if (pick <= 2) next[ny][nx] = 'm';
          else if (pick <= 4) next[ny][nx] = ';';
          else next[ny][nx] = (r.oneIn(3) ? '&' : (r.oneIn(2) ? 'f' : '+'));
        }
      }
      if (wet == 0 && r.oneIn(1400 * winterSlow)) next[y][x] = '#';
      continue;
    }

    // Mushrooms
    if (c == 'm') {
      if (wet == 0 && trees == 0 && r.oneIn(28 * winterSlow)) next[y][x] = '.';
      if ((wet + trees) >= 2 && r.oneIn(45 / autumnMush)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx,ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == '.' || w.terrain[ny][nx] == ',')) next[ny][nx] = 'm';
      }
      continue;
    }

    // Flowers (f,+,& big bloom)
    if (c == 'f' || c == '+' || c == '&') {
      int fade = 160;
      if (s == WINTER) fade = 60;
      if (s == SPRING) fade = 220;
      if (r.oneIn(fade)) next[y][x] = (r.oneIn(2) ? ',' : '.');

      if (wet > 0 && (g + tg + fern) >= 3 && r.oneIn(170 / springBoost)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx,ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == ',' || w.terrain[ny][nx] == '"' || w.terrain[ny][nx] == ';')) {
          int pick = r.i(1, 6);
          next[ny][nx] = (pick <= 2) ? '&' : ((pick <= 4) ? 'f' : '+');
        }
      }
      continue;
    }
  }

  w.terrain.swap(next);
}

// ---------------- Entities (bugs, birds, fireflies, yeti) ----------------
static void stepEntities(World& w, Rng& r, Season s, int tick) {
  Grid cur = w.entities;
  Grid out = cur;

  auto countEnt = [&](char c) {
    int total = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (cur[y][x] == c) total++;
    return total;
  };

  int bugs = countEnt('b');
  int birds = countEnt('v');
  int fireflies = countEnt('F');
  int yetis = countEnt('H');

  // spawn bugs near flowers
  int bugCap = (W * H) / 80;
  if (bugs < bugCap && r.oneIn(2)) {
    for (int tries = 0; tries < 200; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (cur[y][x] != ' ') continue;
      if (blocksEntity(w.terrain[y][x], w.water[y][x])) continue;
      char t = w.terrain[y][x];
      if (t == 'f' || t == '+' || t == '&') { out[y][x] = 'b'; break; }
      if ((t == ',' || t == '"' || t == ';') && (countNeighborsChar(w.terrain, x, y, 'f') + countNeighborsChar(w.terrain, x, y, '+') + countNeighborsChar(w.terrain, x, y, '&') > 0) && r.oneIn(8)) {
        out[y][x] = 'b'; break;
      }
    }
  }

  // spawn birds if many bugs
  int birdCap = (W * H) / 240;
  if (birds < birdCap && bugs > bugCap/6 && r.oneIn(10)) {
    for (int tries = 0; tries < 200; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (cur[y][x] == ' ' && !blocksEntity(w.terrain[y][x], w.water[y][x])) { out[y][x] = 'v'; break; }
    }
  }

  // fireflies: summer + "night" phase
  bool nightish = ((tick / (DAY_TICKS/2)) % 2) == 1;
  int fireflyCap = (W * H) / 260;
  if (s == SUMMER && nightish && fireflies < fireflyCap && r.oneIn(5)) {
    for (int tries = 0; tries < 220; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (cur[y][x] != ' ') continue;
      if (blocksEntity(w.terrain[y][x], w.water[y][x])) continue;
      if (w.terrain[y][x] == ',' || w.terrain[y][x] == '"' || w.terrain[y][x] == ';' || w.terrain[y][x] == 'f' || w.terrain[y][x] == '+') {
        out[y][x] = 'F'; break;
      }
    }
  }

  // yeti: winter rare
  int yetiCap = (W * H) / 2500;
  if (s == WINTER && yetis < yetiCap && r.oneIn(40)) {
    for (int tries = 0; tries < 260; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (cur[y][x] == ' ' && !blocksEntity(w.terrain[y][x], w.water[y][x]) && (w.terrain[y][x] == '.' || w.terrain[y][x] == '^')) {
        out[y][x] = 'H'; break;
      }
    }
  }

  auto canMoveTo = [&](int nx, int ny) {
    if (!inBounds(nx, ny)) return false;
    if (cur[ny][nx] != ' ') return false;
    if (blocksEntity(w.terrain[ny][nx], w.water[ny][nx])) return false;
    return true;
  };

  // move pass
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    char e = cur[y][x];
    if (e == ' ') continue;
    if (out[y][x] != e) continue;

    auto moveTo = [&](int nx, int ny) {
      out[ny][nx] = e;
      out[y][x] = ' ';
    };

    if (e == 'b') {
      if (r.oneIn(420)) { out[y][x] = ' '; continue; }
      int bestNx = x, bestNy = y, bestScore = 999999;
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        if (dx==0 && dy==0) continue;
        int nx = x+dx, ny = y+dy;
        if (!canMoveTo(nx, ny)) continue;
        int score = r.i(0, 18);
        char t = w.terrain[ny][nx];
        if (t == '&') score -= 90;
        else if (t == 'f' || t == '+') score -= 70;
        else if (t == ';') score -= 25;
        else if (t == '"') score -= 18;
        else if (t == ',') score -= 10;
        else if (t == '.') score += 10;
        if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
      }
      if (bestNx != x || bestNy != y) moveTo(bestNx, bestNy);
      continue;
    }

    if (e == 'v') {
      if (r.oneIn(700)) { out[y][x] = ' '; continue; }

      // Eat adjacent bug
      bool ate = false;
      for (int dy = -1; dy <= 1 && !ate; ++dy) for (int dx = -1; dx <= 1 && !ate; ++dx) {
        if (dx==0 && dy==0) continue;
        int nx = x+dx, ny=y+dy;
        if (!inBounds(nx,ny)) continue;
        if (cur[ny][nx] == 'b' && !blocksEntity(w.terrain[ny][nx], w.water[ny][nx])) {
          out[ny][nx] = 'v'; out[y][x] = ' '; ate = true;
        }
      }
      if (ate) continue;

      // Drift toward bug clusters
      int bestNx = x, bestNy = y, bestScore = 999999;
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        if (dx==0 && dy==0) continue;
        int nx = x+dx, ny = y+dy;
        if (!canMoveTo(nx, ny)) continue;
        int bugLocal = 0;
        for (int oy = -2; oy <= 2; ++oy) for (int ox = -2; ox <= 2; ++ox) {
          int ax = nx+ox, ay = ny+oy;
          if (inBounds(ax,ay) && cur[ay][ax] == 'b') bugLocal++;
        }
        int score = -bugLocal * 22 + r.i(0, 22);
        if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
      }
      if (bestNx != x || bestNy != y) moveTo(bestNx, bestNy);
      continue;
    }

    if (e == 'F') {
      // fireflies drift lightly and blink out
      if (!nightish || r.oneIn(220)) { out[y][x] = ' '; continue; }
      int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
      if (canMoveTo(nx, ny)) moveTo(nx, ny);
      continue;
    }

    if (e == 'H') {
      // yeti lumbers slowly, dislikes vegetation
      if (s != WINTER || r.oneIn(900)) { out[y][x] = ' '; continue; }
      int bestNx = x, bestNy = y, bestScore = 999999;
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        if (dx==0 && dy==0) continue;
        int nx = x+dx, ny = y+dy;
        if (!canMoveTo(nx, ny)) continue;
        int score = r.i(0, 25);
        char t = w.terrain[ny][nx];
        if (t == '.') score -= 10;
        if (t == '^') score -= 8;
        if (isVeg(t)) score += 25;
        if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
      }
      if (bestNx != x || bestNy != y) moveTo(bestNx, bestNy);
      continue;
    }

    if (e == 'A') {
      if (r.oneIn(1200)) { out[y][x] = ' '; continue; }
      int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
      if (canMoveTo(nx, ny)) {
        // warp behind
        if (w.water[y][x] == 0 && r.oneIn(8)) w.terrain[y][x] = (r.oneIn(2) ? 'm' : '&');
        if (r.oneIn(25)) w.water[y][x] = (uint8_t)r.i(0,7);
        moveTo(nx, ny);
      }
      continue;
    }
  }

  w.entities.swap(out);
}

// ---------------- Step ----------------
static void step(World& w, Rng& r, std::string& banner, int tick) {
  clearOverlay(w); // overlay is ephemeral unless spawned
  Season s = seasonAt(tick);
  updateWind(w, r, tick);
  maybeChaos(w, r, banner, s);
  stepWater(w, r);
  stepTerrain(w, r, s);
  stepEntities(w, r, s, tick);
}

// ---------------- Rendering (fullscreen fill) ----------------
static inline void setColor(SDL_Renderer* rr, uint8_t R, uint8_t G, uint8_t B) {
  SDL_SetRenderDrawColor(rr, R, G, B, 255);
}

struct Layout {
  int screenW = 0, screenH = 0;
  int hudH = 0;
  int simHpx = 0;
};

static Layout computeLayout(SDL_Renderer* ren) {
  Layout L;
  SDL_GetRendererOutputSize(ren, &L.screenW, &L.screenH);
  L.hudH = std::max(40, L.screenH / 18);
  L.simHpx = L.screenH - L.hudH;
  return L;
}

// 8x8 glyphs for symbols we actually use
static const uint8_t* glyph8(char c) {
  static const uint8_t BLANK[8]  = {0,0,0,0,0,0,0,0};

  static const uint8_t COMMA[8]  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10};
  static const uint8_t DASH[8]   = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00};
  static const uint8_t WAVE[8]   = {0x00,0x00,0x52,0x2A,0x15,0x0A,0x00,0x00};
  static const uint8_t EQ[8]     = {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00};
  static const uint8_t HASH[8]   = {0x00,0x24,0x7E,0x24,0x24,0x7E,0x24,0x00};
  static const uint8_t PCT[8]    = {0x00,0x62,0x64,0x08,0x10,0x26,0x46,0x00};
  static const uint8_t AT[8]     = {0x00,0x3C,0x42,0x5A,0x5A,0x40,0x3C,0x00};

  static const uint8_t TGRASS[8] = {0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00}; // "
  static const uint8_t TREE1[8]  = {0x10,0x38,0x54,0x10,0x10,0x10,0x38,0x00}; // T
  static const uint8_t TREE2[8]  = {0x10,0x38,0x54,0x10,0x10,0x28,0x44,0x00}; // Y
  static const uint8_t MUSH[8]   = {0x00,0x3C,0x7E,0x7E,0x18,0x18,0x3C,0x00}; // m

  static const uint8_t FLOW1[8]  = {0x10,0x54,0x38,0x7C,0x38,0x54,0x10,0x00}; // +
  static const uint8_t FLOW2[8]  = {0x00,0x10,0x38,0x7C,0x38,0x10,0x00,0x00}; // f
  static const uint8_t BIGF[8]   = {0x28,0x7C,0xFE,0x7C,0xFE,0x7C,0x28,0x00}; // &
  static const uint8_t FERN[8]   = {0x10,0x38,0x10,0x38,0x10,0x28,0x44,0x00}; // ;
  static const uint8_t REED[8]   = {0x10,0x10,0x10,0x10,0x28,0x28,0x00,0x00}; // :
  static const uint8_t STONE[8]  = {0x00,0x18,0x3C,0x7E,0x7E,0x3C,0x18,0x00}; // ^
  static const uint8_t STAR[8]   = {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00}; // *
  static const uint8_t EX[8]     = {0x00,0x42,0x24,0x18,0x18,0x24,0x42,0x00}; // x

  static const uint8_t BUG[8]    = {0x00,0x18,0x3C,0x5A,0x3C,0x18,0x00,0x00}; // b
  static const uint8_t BIRD[8]   = {0x00,0x00,0x42,0x24,0x18,0x00,0x00,0x00}; // v
  static const uint8_t GLOW[8]   = {0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00}; // F
  static const uint8_t YETI[8]   = {0x3C,0x7E,0xDB,0xFF,0xFF,0xDB,0x7E,0x3C}; // H
  static const uint8_t AYY[8]    = {0x00,0x18,0x24,0x42,0x7E,0x42,0x42,0x00}; // A

  switch (c) {
    // water glyphs
    case ',': return COMMA;
    case '-': return DASH;
    case '~': return WAVE;
    case '=': return EQ;
    case '#': return HASH;
    case '%': return PCT;
    case '@': return AT;

    // terrain
    case '"': return TGRASS;
    case 'T': return TREE1;
    case 'Y': return TREE2;
    case 'm': return MUSH;
    case '+': return FLOW1;
    case 'f': return FLOW2;
    case '&': return BIGF;
    case ';': return FERN;
    case ':': return REED;
    case '^': return STONE;
    case '*': return STAR;
    case 'x': return EX;

    // entities
    case 'b': return BUG;
    case 'v': return BIRD;
    case 'F': return GLOW;
    case 'H': return YETI;
    case 'A': return AYY;

    default: return BLANK;
  }
}

struct GlyphCache {
  std::unordered_map<char, SDL_Texture*> tex;

  void destroy() {
    for (auto& kv : tex) SDL_DestroyTexture(kv.second);
    tex.clear();
  }

  SDL_Texture* makeGlyph(SDL_Renderer* ren, char c) {
    SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 8, 8);
    if (!t) return nullptr;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(t, nullptr, &pixels, &pitch) != 0) {
      SDL_DestroyTexture(t);
      return nullptr;
    }

    for (int y = 0; y < 8; ++y) {
      uint32_t* px = (uint32_t*)((uint8_t*)pixels + y * pitch);
      for (int x = 0; x < 8; ++x) px[x] = 0x00000000;
    }

    const uint8_t* g = glyph8(c);
    for (int y = 0; y < 8; ++y) {
      uint32_t* px = (uint32_t*)((uint8_t*)pixels + y * pitch);
      uint8_t bits = g[y];
      for (int x = 0; x < 8; ++x) {
        bool on = (bits & (0x80u >> x)) != 0;
        if (on) px[x] = 0xE0FFFFFF;
      }
    }

    SDL_UnlockTexture(t);
    return t;
  }

  SDL_Texture* get(SDL_Renderer* ren, char c) {
    auto it = tex.find(c);
    if (it != tex.end()) return it->second;
    SDL_Texture* t = makeGlyph(ren, c);
    if (t) tex[c] = t;
    return t;
  }
};

// Priority: entity > overlay > water > terrain
static inline char renderCharAt(const World& w, int x, int y) {
  char e = w.entities[y][x];
  if (e != ' ') return e;
  char o = w.overlay[y][x];
  if (o != ' ') return o;
  uint8_t d = w.water[y][x];
  if (d > 0) return waterGlyph(d);
  return w.terrain[y][x];
}

// Color system with lots of variance + season tint
struct RGB { uint8_t r,g,b; };

static RGB baseBgFor(const World& w, int x, int y, int tick, Season s, float sp) {
  uint32_t h = hash2((uint32_t)x, (uint32_t)y, (uint32_t)(tick/6));
  auto jitter = [&](int base, int amt)->uint8_t {
    int j = (int)(h & 0x7) - 3;
    int v = base + j * amt;
    return (uint8_t)std::clamp(v, 0, 255);
  };

  char e = w.entities[y][x];
  if (e != ' ') {
    RGB c;
    switch (e) {
      case 'b': c = {160, 150, 110}; break;
      case 'v': c = {180, 180, 220}; break;
      case 'F': c = {40, 30, 10}; break;   // firefly background dark
      case 'H': c = {25, 25, 30}; break;   // yeti on cold dark
      case 'A': c = {60, 60, 20}; break;
      default:  c = {80, 80, 80}; break;
    }
    applySeasonTint(c.r,c.g,c.b,s,sp);
    return c;
  }

  uint8_t d = w.water[y][x];
  if (d > 0) {
    RGB c = { (uint8_t)(18 + d*2), (uint8_t)(28 + d*3), (uint8_t)(70 + d*11) };
    applySeasonTint(c.r,c.g,c.b,s,sp);
    return c;
  }

  char t = w.terrain[y][x];
  RGB c;
  switch (t) {
    case '.': c = {0,0,0}; break;
    case ',': c = { (uint8_t)jitter(28,2), (uint8_t)jitter(78,3), (uint8_t)jitter(28,2) }; break;
    case '"': c = { (uint8_t)jitter(34,2), (uint8_t)jitter(100,3), (uint8_t)jitter(34,2) }; break;
    case '#': c = { (uint8_t)jitter(34,2), (uint8_t)jitter(58,2), (uint8_t)jitter(34,2) }; break;
    case 'T': c = { (uint8_t)jitter(18,1), (uint8_t)jitter(52,2), (uint8_t)jitter(18,1) }; break;
    case 'Y': c = { (uint8_t)jitter(16,1), (uint8_t)jitter(48,2), (uint8_t)jitter(16,1) }; break;
    case 'm': c = { (uint8_t)jitter(92,3), (uint8_t)jitter(70,2), (uint8_t)jitter(42,2) }; break;
    case 'f': c = { (uint8_t)jitter(70,2), (uint8_t)jitter(56,2), (uint8_t)jitter(90,3) }; break;
    case '+': c = { (uint8_t)jitter(110,2), (uint8_t)jitter(94,2), (uint8_t)jitter(44,2) }; break;
    case '&': c = { (uint8_t)jitter(100,3), (uint8_t)jitter(70,3), (uint8_t)jitter(110,3) }; break;
    case ';': c = { (uint8_t)jitter(30,2), (uint8_t)jitter(86,3), (uint8_t)jitter(40,2) }; break;
    case ':': c = { (uint8_t)jitter(42,2), (uint8_t)jitter(92,3), (uint8_t)jitter(58,2) }; break;
    case '^': c = { (uint8_t)jitter(40,2), (uint8_t)jitter(40,2), (uint8_t)jitter(44,2) }; break;
    case '*': c = { (uint8_t)jitter(150,3), (uint8_t)jitter(60,2), (uint8_t)jitter(30,2) }; break;
    case 'x': c = { (uint8_t)jitter(60,2), (uint8_t)jitter(52,2), (uint8_t)jitter(45,2) }; break;
    default:  c = {18,18,18}; break;
  }

  applySeasonTint(c.r,c.g,c.b,s,sp);
  return c;
}

static RGB fgForChar(char c, Season s, float sp, int tick) {
  // vivid foregrounds; season can slightly tint
  RGB fg = {235,235,235};
  switch (c) {
    case 'm': fg = {245, 235, 210}; break;
    case 'f': fg = {255, 200, 255}; break;
    case '+': fg = {255, 240, 190}; break;
    case '&': fg = {255, 220, 255}; break;
    case ';': fg = {210, 255, 210}; break;
    case ':': fg = {220, 255, 240}; break;
    case '^': fg = {200, 200, 210}; break;

    case ',': fg = {190, 235, 190}; break; // grass or shallow water; ok
    case '"': fg = {210, 250, 210}; break;
    case 'T': fg = {180, 240, 180}; break;
    case 'Y': fg = {180, 240, 200}; break;

    case '~': fg = {180, 220, 255}; break;
    case '=': fg = {200, 235, 255}; break;
    case '#': fg = {220, 240, 255}; break;
    case '%': fg = {230, 250, 255}; break;
    case '@': fg = {240, 255, 255}; break;

    case 'b': fg = {250, 250, 240}; break;
    case 'v': fg = {230, 230, 255}; break;

    case 'F': {
      // firefly glow pulses with day/night
      bool nightish = ((tick / (DAY_TICKS/2)) % 2) == 1;
      fg = nightish ? RGB{255, 255, 140} : RGB{200, 200, 120};
    } break;

    case 'H': fg = {240, 240, 255}; break;
    case 'A': fg = {255, 255, 200}; break;

    case '*': fg = {255, 220, 180}; break;
    case 'x': fg = {200, 190, 190}; break;

    default: break;
  }

  // Overlay rainbow symbols: boost saturation
  if (c == '-' || c == '=' || c == '~') {
    // keep as is
  }

  applySeasonTint(fg.r, fg.g, fg.b, s, sp);
  return fg;
}

static void render(SDL_Renderer* ren, const Layout& L, const World& w, GlyphCache& gc, int tick) {
  Season s = seasonAt(tick);
  float sp = seasonLerp(tick);

  setColor(ren, 0, 0, 0);
  SDL_RenderClear(ren);

  for (int y = 0; y < H; ++y) {
    int y0 = (y * L.simHpx) / H;
    int y1 = ((y + 1) * L.simHpx) / H;
    int hpx = std::max(1, y1 - y0);

    for (int x = 0; x < W; ++x) {
      int x0 = (x * L.screenW) / W;
      int x1 = ((x + 1) * L.screenW) / W;
      int wpx = std::max(1, x1 - x0);

      SDL_Rect rc{ x0, y0, wpx, hpx };

      RGB bg = baseBgFor(w, x, y, tick, s, sp);
      setColor(ren, bg.r, bg.g, bg.b);
      SDL_RenderFillRect(ren, &rc);

      char c = renderCharAt(w, x, y);

      // skip glyph for empty black
      if (c == '.' && w.water[y][x] == 0 && w.entities[y][x] == ' ' && w.overlay[y][x] == ' ') continue;

      SDL_Texture* gt = gc.get(ren, c);
      if (!gt) continue;

      RGB fg = fgForChar(c, s, sp, tick);

      // wind makes grass "shimmer" by alternating color slightly (render-only)
      if ((w.terrain[y][x] == ',' || w.terrain[y][x] == '"') && w.wind.strength > 0) {
        uint32_t h = hash2((uint32_t)x, (uint32_t)y, (uint32_t)(tick/3));
        int wob = (int)(h & 1);
        if (wob) {
          fg.g = (uint8_t)std::min<int>(255, fg.g + 10);
        }
      }

      SDL_SetTextureColorMod(gt, fg.r, fg.g, fg.b);
      SDL_RenderCopy(ren, gt, nullptr, &rc);
    }
  }

  // HUD strip
  SDL_Rect hud{0, L.simHpx, L.screenW, L.hudH};
  setColor(ren, 6, 6, 6);
  SDL_RenderFillRect(ren, &hud);

  SDL_RenderPresent(ren);
}

// ---------------- Main loop ----------------
int main(int, char**) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  SDL_Window* win = SDL_CreateWindow(
    "Terrarium 0.3",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    0, 0,
    SDL_WINDOW_FULLSCREEN_DESKTOP
  );
  if (!win) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) {
    std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  uint32_t seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
  Rng r(seed);

  World world;
  seedWorld(world, r);

  GlyphCache gc;
  Layout layout = computeLayout(ren);

  bool running = true;
  bool paused = false;
  int tps = DEFAULT_TPS;
  int tick = 0;
  std::string banner = "calm";

  auto last = std::chrono::steady_clock::now();

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;

      if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.sym) {
          case SDLK_ESCAPE: running = false; break;
          case SDLK_SPACE:  paused = !paused; break;
          case SDLK_PERIOD: if (paused) { step(world, r, banner, tick); tick++; } break;
          case SDLK_r:      seedWorld(world, r); tick = 0; banner = "reset"; break;
          case SDLK_LEFTBRACKET:  if (tps > 1) tps--; break;
          case SDLK_RIGHTBRACKET: if (tps < 30) tps++; break;
          default: break;
        }
      }

      if (e.type == SDL_WINDOWEVENT &&
          (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
           e.window.event == SDL_WINDOWEVENT_RESIZED ||
           e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) {
        layout = computeLayout(ren);
      }
    }

    auto now = std::chrono::steady_clock::now();
    auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    const int msPerTick = 1000 / std::max(1, tps);

    if (!paused && dtMs >= msPerTick) {
      last = now;
      step(world, r, banner, tick);
      tick++;
    }

    Season s = seasonAt(tick);
    const char* sname = (s==SPRING? "spring" : s==SUMMER? "summer" : s==AUTUMN? "autumn" : "winter");
    std::string title =
      std::string("Terrarium 0.3 | ") + std::to_string(W) + "x" + std::to_string(H) +
      " | tick " + std::to_string(tick) +
      " | " + (paused ? "PAUSED" : ("tps " + std::to_string(tps))) +
      " | " + sname +
      " | wind " + std::to_string(world.wind.strength) +
      " | " + banner +
      " | SPACE pause  . step  [ ] speed  r reset  ESC quit";
    SDL_SetWindowTitle(win, title.c_str());

    render(ren, layout, world, gc, tick);

    SDL_Delay(6);
  }

  gc.destroy();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
