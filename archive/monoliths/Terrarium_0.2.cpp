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
// Terrarium 0.2 (SDL2) - Dense DF-ish vibe
// - Water depth (0..7) + cheap flow
// - More biome variance: flowers, bugs, birds, multiple trees
// - Black negative space (no '.' glyph)
// - True fullscreen fill (no letterboxing)
// Controls:
//   SPACE = pause/unpause
//   .     = step one tick (when paused)
//   [ ]   = sim speed down/up
//   r     = reset
//   ESC   = quit
// ============================================================

// ---------------- World size ----------------
static constexpr int W = 200;
static constexpr int H = 112;

// ---------------- Pace ----------------
static constexpr int DEFAULT_TPS = 4;

// ---------------- Chaos knobs ----------------
static constexpr int EVENT_CHANCE = 1500;   // 1 in N ticks
static constexpr int EVENT_INTENSITY = 260; // ops per event

// ---------------- Life caps (scaled-ish) ----------------
static constexpr int MAX_BUGS_BASE  = 220;
static constexpr int MAX_BIRDS_BASE = 60;

// ---------------- Types ----------------
using Grid = std::vector<std::string>;
using Water = std::vector<std::vector<uint8_t>>; // 0..7 depth

static inline bool inBounds(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

struct Rng {
  std::mt19937 rng;
  explicit Rng(uint32_t seed) : rng(seed) {}
  int i(int a, int b) { std::uniform_int_distribution<int> d(a, b); return d(rng); }
  bool oneIn(int n) { return n > 0 && i(1, n) == 1; }
  double u01() { std::uniform_real_distribution<double> d(0.0, 1.0); return d(rng); }
};

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

static bool isVegetation(char c) {
  // Terrain vegetation only (not entities)
  return (c == ',' || c == '"' || c == '#' || c == 'T' || c == 'Y' || c == 'f' || c == '+' || c == 'm');
}

static bool isTree(char c) { return (c == 'T' || c == 'Y'); }

// ---------------- Layers ----------------
struct World {
  Grid terrain;   // '.' empty, ',' grass, '"' tallgrass, '#' shrub, 'T'/'Y' trees, 'm' mushrooms, 'f'/'+' flowers, '*' fire, 'x' ash
  Grid entities;  // ' ' none, 'b' bug, 'v' bird, 'o' herbivore, 'W' predator, 'A' alien
  Water water;    // 0..7 depth
};

// ---------------- Seed ----------------
static void seedWorld(World& w, Rng& r) {
  w.terrain.assign(H, std::string(W, '.'));
  w.entities.assign(H, std::string(W, ' '));
  w.water.assign(H, std::vector<uint8_t>(W, 0));

  // Ponds (depth 3..7)
  int ponds = std::max(4, (W * H) / 9000);
  for (int p = 0; p < ponds; ++p) {
    int marginX = std::max(12, W / 18);
    int marginY = std::max(8,  H / 18);
    int cx = r.i(marginX, W - 1 - marginX);
    int cy = r.i(marginY, H - 1 - marginY);
    int rad = r.i(5, 12);

    for (int y = cy - rad; y <= cy + rad; ++y)
      for (int x = cx - rad; x <= cx + rad; ++x) {
        if (!inBounds(x, y)) continue;
        int dx = x - cx, dy = y - cy;
        int d2 = dx*dx + dy*dy;
        if (d2 <= rad*rad + r.i(-4, 4)) {
          uint8_t depth = (uint8_t)std::clamp(7 - (d2 / std::max(1, rad)), 3, 7);
          w.water[y][x] = std::max<uint8_t>(w.water[y][x], depth);
        }
      }
  }

  // Grass near water
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      if (w.water[y][x] > 0) continue;
      if (countNeighborsWater(w.water, x, y) > 0 && r.oneIn(2)) w.terrain[y][x] = ',';
    }

  // Starter shrubs + trees
  int starters = (W * H) / 700;
  for (int k = 0; k < starters; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && w.terrain[y][x] == ',' && r.oneIn(3)) w.terrain[y][x] = '#';
  }
  for (int k = 0; k < starters / 2; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && w.terrain[y][x] == '#' && r.oneIn(2)) w.terrain[y][x] = (r.oneIn(2) ? 'T' : 'Y');
  }

  // Starter mushrooms + flowers
  int mushSeeds = (W * H) / 600;
  int flowerSeeds = (W * H) / 900;
  for (int k = 0; k < mushSeeds; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && (w.terrain[y][x] == '.' || w.terrain[y][x] == ',') && countNeighborsWater(w.water, x, y) > 0 && r.oneIn(2))
      w.terrain[y][x] = 'm';
  }
  for (int k = 0; k < flowerSeeds; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] == 0 && (w.terrain[y][x] == ',' || w.terrain[y][x] == '"') && r.oneIn(2))
      w.terrain[y][x] = (r.oneIn(2) ? 'f' : '+');
  }
}

// ---------------- Water symbols (depth->glyph) ----------------
// 0 means no water; we render black empty background if terrain is '.'
static char waterGlyph(uint8_t d) {
  // shallow -> flood
  switch (d) {
    case 1: return ','; // damp/shallow
    case 2: return '-';
    case 3: return '~';
    case 4: return '=';
    case 5: return '#';
    case 6: return '%';
    default: return '@'; // 7+
  }
}

// ---------------- Chaos events ----------------
static void chaosRain(World& w, Rng& r, std::string& banner) {
  banner = "Rain: water rises; mushrooms + flowers bloom";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] < 7 && r.oneIn(2)) w.water[y][x]++;

    // bloom around wet edges
    if (w.water[y][x] == 0 && countNeighborsWater(w.water, x, y) > 0) {
      char &t = w.terrain[y][x];
      if ((t == '.' || t == ',') && r.oneIn(6)) t = 'm';
      if ((t == ',' || t == '"') && r.oneIn(10)) t = (r.oneIn(2) ? 'f' : '+');
    }
  }
  // rare rainbow shimmer line (purely visual symbol '=' on terrain where dry)
  if (r.oneIn(4)) {
    int yy = r.i(4, H - 5);
    for (int x = 0; x < W; ++x) {
      if (w.water[yy][x] == 0 && (w.terrain[yy][x] == '.' || w.terrain[yy][x] == ',')) {
        // we reuse '+' as a "spark" sometimes; keep it subtle
        if (r.oneIn(5)) w.terrain[yy][x] = '+';
      }
    }
  }
}

static void chaosDrought(World& w, Rng& r, std::string& banner) {
  banner = "Drought: water falls; flowers wither";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] > 0 && r.oneIn(2)) w.water[y][x]--;

    char &t = w.terrain[y][x];
    if ((t == 'm' || t == 'f' || t == '+') && r.oneIn(2)) t = (r.oneIn(3) ? '.' : ',');
    if ((t == ',' || t == '"') && r.oneIn(10)) t = '.';
  }
}

static void chaosLightning(World& w, Rng& r, std::string& banner) {
  banner = "Lightning: fires ignite";
  int cx = r.i(0, W - 1), cy = r.i(0, H - 1);
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = cx + r.i(-22, 22), y = cy + r.i(-12, 12);
    if (!inBounds(x, y)) continue;
    if (w.water[y][x] > 0) continue;
    if (isVegetation(w.terrain[y][x]) && r.oneIn(2)) w.terrain[y][x] = '*';
  }
}

static void chaosBlight(World& w, Rng& r, std::string& banner) {
  banner = "Blight: vegetation collapses";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] > 0) continue;
    char &t = w.terrain[y][x];
    if (isVegetation(t) && r.oneIn(2)) t = (r.oneIn(3) ? 'x' : '.');
  }
}

static void chaosAlien(World& w, Rng& r, std::string& banner) {
  banner = "Alien: reality flexes";
  // place one alien entity on dry land
  for (int tries = 0; tries < 900; ++tries) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (w.water[y][x] > 0) continue;
    if (w.terrain[y][x] == '*') continue;
    w.entities[y][x] = 'A';
    break;
  }
  // weird pulses: water + sudden growth + occasional fire
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (r.oneIn(8)) w.water[y][x] = (uint8_t)r.i(0, 7);

    char &t = w.terrain[y][x];
    if (w.water[y][x] == 0) {
      if (t == '.' && r.oneIn(3)) t = ',';
      else if (t == ',' && r.oneIn(4)) t = (r.oneIn(2) ? '"' : (r.oneIn(2) ? 'f' : '+'));
      else if (t == '"' && r.oneIn(6)) t = '#';
      else if (t == '#' && r.oneIn(8)) t = (r.oneIn(2) ? 'T' : 'Y');
      else if (isTree(t) && r.oneIn(18)) t = '*';
    }
  }
}

static void maybeChaos(World& w, Rng& r, std::string& banner) {
  if (!r.oneIn(EVENT_CHANCE)) { banner = "calm"; return; }
  int type = r.i(1, 6);
  if (type == 1) chaosRain(w, r, banner);
  else if (type == 2) chaosDrought(w, r, banner);
  else if (type == 3) chaosLightning(w, r, banner);
  else if (type == 4) chaosBlight(w, r, banner);
  else chaosAlien(w, r, banner);
}

// ---------------- Water flow (cheap + pretty) ----------------
// We diffuse / flow 1 unit at a time from higher -> lower neighbors.
// Adds a slight downward bias so it "feels" like flow.
static void stepWater(World& w, Rng& r) {
  Water next = w.water;

  // a small number of micro-moves per tick to keep it cheap but alive
  int moves = (W * H) / 2; // ~11k on 200x112
  for (int k = 0; k < moves; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    uint8_t d = next[y][x];
    if (d == 0) continue;

    // candidate neighbors (with downward bias)
    int bestNx = x, bestNy = y;
    int bestScore = 1e9;

    auto consider = [&](int nx, int ny, int scoreBias) {
      if (!inBounds(nx, ny)) return;
      uint8_t nd = next[ny][nx];
      // prefer lower depth
      int score = (int)nd * 10 + scoreBias;
      if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
    };

    // Downward-ish bias (y+1 favored)
    consider(x, y + 1, 0);
    consider(x - 1, y + 1, 1);
    consider(x + 1, y + 1, 1);
    consider(x - 1, y, 2);
    consider(x + 1, y, 2);
    consider(x, y - 1, 3);

    if (bestNx == x && bestNy == y) continue;

    uint8_t nd = next[bestNy][bestNx];

    // Move 1 unit if it helps equalize
    if (nd + 1 < d) {
      next[y][x]--;
      next[bestNy][bestNx]++;
    } else if (r.oneIn(10) && nd < d) {
      // occasional "sloshing" move
      next[y][x]--;
      next[bestNy][bestNx]++;
    }
  }

  // Clamp
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      next[y][x] = (uint8_t)std::min<int>(7, next[y][x]);

  w.water.swap(next);
}

// ---------------- Terrain ecology ----------------
static void stepTerrain(World& w, Rng& r) {
  Grid next = w.terrain;

  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    // Water overrides ecology (but terrain can exist "under" it)
    if (w.water[y][x] > 0) {
      // fire cannot persist under water
      if (w.terrain[y][x] == '*') next[y][x] = 'x';
      continue;
    }

    char c = w.terrain[y][x];

    // Fire/ash
    if (c == '*') {
      // burn out to ash
      next[y][x] = (r.oneIn(3) ? 'x' : '*');
      continue;
    }
    if (c == 'x') {
      // ash recovers: near water -> grass, otherwise slowly to empty
      if (countNeighborsWater(w.water, x, y) > 0 && r.oneIn(5)) next[y][x] = ',';
      else if (r.oneIn(30)) next[y][x] = '.';
      continue;
    }

    // ignition spread
    if (isVegetation(c) && countNeighborsChar(w.terrain, x, y, '*') > 0 && r.oneIn(3)) {
      next[y][x] = '*';
      continue;
    }

    // local wetness
    int wet = countNeighborsWater(w.water, x, y);
    int g  = countNeighborsChar(w.terrain, x, y, ',');
    int tg = countNeighborsChar(w.terrain, x, y, '"');
    int sh = countNeighborsChar(w.terrain, x, y, '#');
    int t1 = countNeighborsChar(w.terrain, x, y, 'T');
    int t2 = countNeighborsChar(w.terrain, x, y, 'Y');
    int trees = t1 + t2;
    int flo = countNeighborsChar(w.terrain, x, y, 'f') + countNeighborsChar(w.terrain, x, y, '+');

    // Negative space '.' => black emptiness; allow colonization if near life/water
    if (c == '.') {
      int fert = wet * 3 + g + tg + flo;
      if (fert > 0 && r.u01() < (0.0035 * fert)) next[y][x] = ',';
      // occasional mushroom near damp shade
      if (wet > 0 && trees > 0 && r.oneIn(180)) next[y][x] = 'm';
      continue;
    }

    // Grass
    if (c == ',') {
      // tall grass / flowers appear
      if ((g + tg) >= 4 && r.oneIn(90)) next[y][x] = '"';
      if (wet > 0 && r.oneIn(170)) next[y][x] = (r.oneIn(2) ? 'f' : '+');
      // grass can die if isolated & dry
      if ((wet + g + tg) == 0 && r.oneIn(70)) next[y][x] = '.';
      continue;
    }

    // Tall grass
    if (c == '"') {
      if ((g + tg) >= 5 && r.oneIn(130)) next[y][x] = '#';
      if (wet > 0 && r.oneIn(220)) next[y][x] = (r.oneIn(2) ? 'f' : '+');
      if (wet == 0 && r.oneIn(120)) next[y][x] = ',';
      continue;
    }

    // Shrub
    if (c == '#') {
      if ((sh + trees) >= 3 && wet > 0 && r.oneIn(260)) next[y][x] = (r.oneIn(2) ? 'T' : 'Y');
      if (wet == 0 && r.oneIn(160)) next[y][x] = '"';
      // mushrooms near shrubs + damp
      if (wet > 0 && r.oneIn(260)) next[y][x] = 'm';
      continue;
    }

    // Trees (variants)
    if (c == 'T' || c == 'Y') {
      // under-tree mushrooms + flowers sometimes
      if (wet > 0 && r.oneIn(220)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx, ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == '.' || w.terrain[ny][nx] == ','))
          next[ny][nx] = (r.oneIn(2) ? 'm' : (r.oneIn(2) ? 'f' : '+'));
      }
      // tree dieback if very dry
      if (wet == 0 && r.oneIn(1200)) next[y][x] = '#';
      continue;
    }

    // Mushrooms
    if (c == 'm') {
      if (wet == 0 && trees == 0 && r.oneIn(30)) next[y][x] = '.';
      // spread in damp shade
      if ((wet + trees) >= 2 && r.oneIn(45)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx, ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == '.' || w.terrain[ny][nx] == ','))
          next[ny][nx] = 'm';
      }
      continue;
    }

    // Flowers
    if (c == 'f' || c == '+') {
      // fade over time
      if (r.oneIn(120)) next[y][x] = (r.oneIn(2) ? ',' : '.');
      // bloom spreads near wet grasslands
      if (wet > 0 && (g + tg) >= 3 && r.oneIn(180)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx, ny) && w.water[ny][nx] == 0 && (w.terrain[ny][nx] == ',' || w.terrain[ny][nx] == '"'))
          next[ny][nx] = (r.oneIn(2) ? 'f' : '+');
      }
      continue;
    }
  }

  w.terrain.swap(next);
}

// ---------------- Entities (bugs + birds + existing creatures) ----------------
static void stepEntities(World& w, Rng& r) {
  Grid cur = w.entities;
  Grid out = cur;

  auto countEnt = [&](char c) {
    int total = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (cur[y][x] == c) total++;
    return total;
  };

  const int MAX_BUGS  = std::max(MAX_BUGS_BASE,  (W * H) / 120);
  const int MAX_BIRDS = std::max(MAX_BIRDS_BASE, (W * H) / 420);

  int bugs  = countEnt('b');
  int birds = countEnt('v');

  // Spawn bugs near flowers (alive feeling)
  if (bugs < MAX_BUGS && r.oneIn(2)) {
    for (int tries = 0; tries < 200; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (w.water[y][x] > 0) continue;
      if (cur[y][x] != ' ') continue;
      if (w.terrain[y][x] == 'f' || w.terrain[y][x] == '+') { out[y][x] = 'b'; break; }
      if ((w.terrain[y][x] == ',' || w.terrain[y][x] == '"') && (countNeighborsChar(w.terrain, x, y, 'f') + countNeighborsChar(w.terrain, x, y, '+') > 0) && r.oneIn(8)) {
        out[y][x] = 'b'; break;
      }
    }
  }

  // Spawn birds if many bugs
  if (birds < MAX_BIRDS && bugs > MAX_BUGS / 6 && r.oneIn(12)) {
    for (int tries = 0; tries < 200; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (w.water[y][x] > 0) continue;
      if (cur[y][x] == ' ') { out[y][x] = 'v'; break; }
    }
  }

  // Helper: can occupy cell
  auto canMoveTo = [&](int nx, int ny) {
    if (!inBounds(nx, ny)) return false;
    if (w.water[ny][nx] > 0) return false;
    if (w.terrain[ny][nx] == '*') return false;
    return true;
  };

  // Move pass
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    char e = cur[y][x];
    if (e == ' ') continue;
    if (out[y][x] != e) continue; // already moved over

    auto moveTo = [&](int nx, int ny) {
      out[ny][nx] = e;
      out[y][x] = ' ';
    };

    // BUGS: drift toward flowers
    if (e == 'b') {
      // die sometimes (keeps motion from saturating)
      if (r.oneIn(350)) { out[y][x] = ' '; continue; }

      int bestNx = x, bestNy = y;
      int bestScore = 999999;

      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          int nx = x + dx, ny = y + dy;
          if (!canMoveTo(nx, ny)) continue;
          if (cur[ny][nx] != ' ') continue;

          // Prefer flowers, then grass/tallgrass, avoid empty
          int score = 0;
          char t = w.terrain[ny][nx];
          if (t == 'f' || t == '+') score -= 100;
          else if (t == '"' ) score -= 20;
          else if (t == ',' ) score -= 10;
          else if (t == '.') score += 10;

          // slight randomness
          score += r.i(0, 12);

          if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
        }

      if (bestNx != x || bestNy != y) moveTo(bestNx, bestNy);
      continue;
    }

    // BIRDS: hunt bugs
    if (e == 'v') {
      if (r.oneIn(600)) { out[y][x] = ' '; continue; }

      // immediate eat if bug adjacent
      bool ate = false;
      for (int dy = -1; dy <= 1 && !ate; ++dy)
        for (int dx = -1; dx <= 1 && !ate; ++dx) {
          if (dx == 0 && dy == 0) continue;
          int nx = x + dx, ny = y + dy;
          if (!inBounds(nx, ny)) continue;
          if (cur[ny][nx] == 'b' && canMoveTo(nx, ny)) {
            out[ny][nx] = 'v';
            out[y][x] = ' ';
            ate = true;
          }
        }
      if (ate) continue;

      // otherwise drift toward areas with bugs
      int bestNx = x, bestNy = y;
      int bestScore = 999999;

      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          int nx = x + dx, ny = y + dy;
          if (!canMoveTo(nx, ny)) continue;
          if (cur[ny][nx] != ' ') continue;

          // Count nearby bugs around candidate
          int bugLocal = 0;
          for (int oy = -2; oy <= 2; ++oy)
            for (int ox = -2; ox <= 2; ++ox) {
              int ax = nx + ox, ay = ny + oy;
              if (inBounds(ax, ay) && cur[ay][ax] == 'b') bugLocal++;
            }

          int score = -bugLocal * 25 + r.i(0, 18);
          if (score < bestScore) { bestScore = score; bestNx = nx; bestNy = ny; }
        }

      if (bestNx != x || bestNy != y) moveTo(bestNx, bestNy);
      continue;
    }

    // Aliens: wander and warp terrain/water
    if (e == 'A') {
      if (r.oneIn(1200)) { out[y][x] = ' '; continue; }

      int dx = r.i(-1, 1), dy = r.i(-1, 1);
      int nx = x + dx, ny = y + dy;
      if (canMoveTo(nx, ny) && cur[ny][nx] == ' ') {
        // leave behind strange residue
        if (w.water[y][x] == 0) {
          char &t = w.terrain[y][x];
          if (t == '.' && r.oneIn(2)) t = ',';
          else if (t == ',' && r.oneIn(3)) t = (r.oneIn(2) ? 'm' : '+');
          else if (isVegetation(t) && r.oneIn(12)) t = '*';
        }
        // warp water sometimes
        if (r.oneIn(30)) w.water[y][x] = (uint8_t)r.i(0, 7);
        moveTo(nx, ny);
      }
      continue;
    }

    // Other creatures from older versions (optional)
    // 'o' herbivore, 'W' predator: keep very simple if present
    if (e == 'o' || e == 'W') {
      int dx = r.i(-1, 1), dy = r.i(-1, 1);
      int nx = x + dx, ny = y + dy;
      if (canMoveTo(nx, ny) && cur[ny][nx] == ' ') {
        moveTo(nx, ny);
      }
      continue;
    }
  }

  w.entities.swap(out);
}

// ---------------- Full step ----------------
static void step(World& w, Rng& r, std::string& banner) {
  maybeChaos(w, r, banner);
  stepWater(w, r);
  stepTerrain(w, r);
  stepEntities(w, r);
}

// ---------------- Rendering ----------------
// True fullscreen fill: each cell maps to exact pixel ranges using integer division.
// Glyph textures are 8x8 and scaled to cell rectangles at draw time.

static inline void setColor(SDL_Renderer* rr, uint8_t R, uint8_t G, uint8_t B) {
  SDL_SetRenderDrawColor(rr, R, G, B, 255);
}

struct Layout {
  int screenW = 0, screenH = 0;
  int hudH = 0;
  int simHpx = 0; // screenH - hudH
};

static Layout computeLayout(SDL_Renderer* ren) {
  Layout L;
  SDL_GetRendererOutputSize(ren, &L.screenW, &L.screenH);
  L.hudH = std::max(40, L.screenH / 18);
  L.simHpx = L.screenH - L.hudH;
  return L;
}

// --- tiny 8x8 glyphs ---
// Each glyph is 8 bytes, MSB=left pixel
static const uint8_t* glyph8(char c) {
  static const uint8_t BLANK[8]  = {0,0,0,0,0,0,0,0};

  static const uint8_t COMMA[8]  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10};
  static const uint8_t DASH[8]   = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00};
  static const uint8_t WAVE[8]   = {0x00,0x00,0x52,0x2A,0x15,0x0A,0x00,0x00};
  static const uint8_t EQ[8]     = {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00};
  static const uint8_t HASH[8]   = {0x00,0x24,0x7E,0x24,0x24,0x7E,0x24,0x00};
  static const uint8_t PCT[8]    = {0x00,0x62,0x64,0x08,0x10,0x26,0x46,0x00};
  static const uint8_t AT[8]     = {0x00,0x3C,0x42,0x5A,0x5A,0x40,0x3C,0x00};

  static const uint8_t GRASS[8]  = {0x00,0x10,0x10,0x00,0x28,0x28,0x00,0x00}; // ','
  static const uint8_t TGRASS[8] = {0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00}; // '"'

  static const uint8_t TREE1[8]  = {0x10,0x38,0x54,0x10,0x10,0x10,0x38,0x00}; // T-ish
  static const uint8_t TREE2[8]  = {0x10,0x38,0x54,0x10,0x10,0x28,0x44,0x00}; // Y-ish

  static const uint8_t MUSH[8]   = {0x00,0x3C,0x7E,0x7E,0x18,0x18,0x3C,0x00};
  static const uint8_t FLOW1[8]  = {0x10,0x54,0x38,0x7C,0x38,0x54,0x10,0x00}; // '+'
  static const uint8_t FLOW2[8]  = {0x00,0x10,0x38,0x7C,0x38,0x10,0x00,0x00}; // 'f' stylized
  static const uint8_t BUG[8]    = {0x00,0x18,0x3C,0x5A,0x3C,0x18,0x00,0x00}; // 'b'
  static const uint8_t BIRD[8]   = {0x00,0x00,0x42,0x24,0x18,0x00,0x00,0x00}; // 'v' stylized
  static const uint8_t STAR[8]   = {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00};
  static const uint8_t EX[8]     = {0x00,0x42,0x24,0x18,0x18,0x24,0x42,0x00};
  static const uint8_t AYY[8]    = {0x00,0x18,0x24,0x42,0x7E,0x42,0x42,0x00};

  // Note: '.' is black empty space; we return BLANK and also skip drawing for '.'
  switch (c) {
    // Water depth glyphs
    case ',': return COMMA; // also used for grass; rendering layer decides
    case '-': return DASH;
    case '~': return WAVE;
    case '=': return EQ;
    case '#': return HASH;
    case '%': return PCT;
    case '@': return AT;

    // Terrain
    case '"': return TGRASS;
    case 'T': return TREE1;
    case 'Y': return TREE2;
    case 'm': return MUSH;
    case '+': return FLOW1;
    case 'f': return FLOW2;
    case '*': return STAR;
    case 'x': return EX;

    // Entities
    case 'b': return BUG;
    case 'v': return BIRD;
    case 'A': return AYY;

    default:  return BLANK;
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

    // Clear transparent
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
        if (on) px[x] = 0xE0FFFFFF; // white glyph w/ alpha
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

// Background palette by “final rendered char”
static void setBg(SDL_Renderer* ren, char c, uint8_t waterDepth /*0..7*/, char terrain, char entity) {
  // Entities get strong colors
  if (entity != ' ') {
    switch (entity) {
      case 'b': setColor(ren, 170, 160, 120); return;
      case 'v': setColor(ren, 190, 190, 220); return;
      case 'A': setColor(ren, 220, 220, 120); return;
      case 'o': setColor(ren, 220, 220, 220); return;
      case 'W': setColor(ren, 200, 205, 240); return;
      default:  setColor(ren, 120, 120, 120); return;
    }
  }

  // Water gradient
  if (waterDepth > 0) {
    // deeper -> darker/stronger
    int d = (int)waterDepth;
    uint8_t R = (uint8_t)std::clamp(18 + d * 2, 0, 255);
    uint8_t G = (uint8_t)std::clamp(28 + d * 3, 0, 255);
    uint8_t B = (uint8_t)std::clamp(70 + d * 10, 0, 255);
    setColor(ren, R, G, B);
    return;
  }

  // Terrain colors
  switch (terrain) {
    case '.': setColor(ren, 0, 0, 0); return;          // true empty black
    case ',': setColor(ren, 34, 70, 34); return;        // grass
    case '"': setColor(ren, 52, 96, 52); return;        // tall grass
    case '#': setColor(ren, 44, 62, 44); return;        // shrub
    case 'T': setColor(ren, 26, 50, 26); return;        // tree type
    case 'Y': setColor(ren, 24, 46, 24); return;        // tree type
    case 'm': setColor(ren, 90, 72, 42); return;        // mushroom
    case 'f': setColor(ren, 88, 50, 90); return;        // flower
    case '+': setColor(ren, 110, 100, 40); return;      // flower/spark
    case '*': setColor(ren, 150, 60, 30); return;       // fire
    case 'x': setColor(ren, 60, 50, 45); return;        // ash
    default:  setColor(ren, 18, 18, 18); return;
  }
}

// Render symbol selection:
// Priority: entity > water > terrain. Empty '.' draws black with no glyph.
static inline char renderCharAt(const World& w, int x, int y) {
  char e = w.entities[y][x];
  if (e != ' ') return e;
  uint8_t d = w.water[y][x];
  if (d > 0) return waterGlyph(d);
  return w.terrain[y][x];
}

static void render(SDL_Renderer* ren, const Layout& L, const World& w, GlyphCache& gc) {
  // Clear full screen
  setColor(ren, 0, 0, 0);
  SDL_RenderClear(ren);

  // Each cell fills exact pixel range using integer division (no gaps, no bars)
  // x0 = x * screenW / W, x1 = (x+1) * screenW / W
  // y0 = y * simHpx / H,  y1 = (y+1) * simHpx / H
  for (int y = 0; y < H; ++y) {
    int y0 = (y * L.simHpx) / H;
    int y1 = ((y + 1) * L.simHpx) / H;
    int hpx = std::max(1, y1 - y0);

    for (int x = 0; x < W; ++x) {
      int x0 = (x * L.screenW) / W;
      int x1 = ((x + 1) * L.screenW) / W;
      int wpx = std::max(1, x1 - x0);

      SDL_Rect rc{ x0, y0, wpx, hpx };

      char terrain = w.terrain[y][x];
      char entity  = w.entities[y][x];
      uint8_t depth = w.water[y][x];
      char c = renderCharAt(w, x, y);

      setBg(ren, c, depth, terrain, entity);
      SDL_RenderFillRect(ren, &rc);

      // Skip glyph for empty black space
      if (c == '.' && depth == 0 && entity == ' ') continue;

      SDL_Texture* gt = gc.get(ren, c);
      if (!gt) continue;

      SDL_RenderCopy(ren, gt, nullptr, &rc);
    }
  }

  // HUD strip
  SDL_Rect hud{0, L.simHpx, L.screenW, L.hudH};
  setColor(ren, 8, 8, 8);
  SDL_RenderFillRect(ren, &hud);

  SDL_RenderPresent(ren);
}

int main(int, char**) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  SDL_Window* win = SDL_CreateWindow(
    "Terrarium 0.2",
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
          case SDLK_PERIOD: if (paused) { step(world, r, banner); tick++; } break;
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
      step(world, r, banner);
      tick++;
    }

    std::string title =
      std::string("Terrarium 0.2 | ") + std::to_string(W) + "x" + std::to_string(H) +
      " | tick " + std::to_string(tick) +
      " | " + (paused ? "PAUSED" : ("tps " + std::to_string(tps))) +
      " | " + banner +
      " | SPACE pause  . step  [ ] speed  r reset  ESC quit";
    SDL_SetWindowTitle(win, title.c_str());

    render(ren, layout, world, gc);

    SDL_Delay(6);
  }

  gc.destroy();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
