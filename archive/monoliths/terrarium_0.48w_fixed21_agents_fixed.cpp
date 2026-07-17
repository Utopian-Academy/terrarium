// terrarium_0.42_cloudfix.cpp
// SDL2 ASCII-glyph terrarium with: seasons, wind, water depth (DF-ish), vivid palette,
// biome presets (meadow, wetland, alpine, alien, tropical), clouds + cloud shadows,
// weather (clear/overcast/rain/storm), rain overlay, lightning, big rainbow,
// lots of plant variance, more animals, and multi-tile "big" entities (2x2 deer, 3x3 ancient tree).
//
// Build (Linux):
//   sudo apt install -y g++ libsdl2-dev
//   g++ -O2 -std=c++17 terrarium_0.34.cpp -o terrarium_0.34 `sdl2-config --cflags --libs`
//
// Run examples:
//   ./terrarium_0.34 --biome meadow
//   ./terrarium_0.34 --biome tropical
//   ./terrarium_0.34 --windowed --biome tropical
//   ./terrarium_0.34 --fullscreen --biome meadow
//
// Controls:
//   SPACE pause/unpause
//   .     step (when paused)
//   [ ]   slower/faster
//   r     reseed
//   F11   toggle fullscreen
//   ESC   quit
//
// Notes:
// - ASCII-only (no unicode) for portability and Termux-friendly compilation if needed.
// - Clouds are a low-res field (CW x CH), scrolled by wind; shadows darken the w.
// - Rain is overlay + modest water increase; storms add lightning and stronger wind.
// - Big creatures are "stamped" at render time from anchor entities (simple, robust).

#include <SDL.h>
#ifdef _WIN32
  #include <windows.h>
  #include <mmsystem.h>
  #pragma comment(lib, "winmm.lib")
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

static constexpr int W = 200;
static constexpr int H = 112;

static constexpr int CW = 96;
static constexpr int CH = 54;

static constexpr int DEFAULT_TPS = 6;

static constexpr int SEASON_TICKS = 900; // faster season cycle so it's visible (~30-90s depending on TPS)
static constexpr int DAY_TICKS    = 900;

static constexpr int WIND_CHANGE_TICKS = 220;
static constexpr int MAX_WIND = 5;

enum WeatherState { CLEAR=0, OVERCAST=1, RAIN=2, STORM=3 };

static constexpr float VIVID_SAT = 1.18f;  // slightly richer (bg is black)
static constexpr float VIVID_VAL = 1.08f;  // a touch brighter

static constexpr int BUG_CAP_BASE      = (W * H) / 75;
static constexpr int BIRD_CAP_BASE     = (W * H) / 260;
static constexpr int SCORPION_CAP = (W * H) / 1600;
static constexpr int DRAGONFLY_CAP= (W * H) / 1100;
static constexpr int CRAB_CAP     = (W * H) / 1400;
static constexpr int JELLY_CAP    = (W * H) / 1500;
static constexpr int CRAWLER_CAP  = (W * H) / 1700;
static constexpr int ORB_CAP      = (W * H) / 2600;

static constexpr int FISH_CAP     = (W * H) / 1600;
static constexpr int CRANE_CAP    = (W * H) / 3800;

static constexpr int RABBIT_CAP_BASE   = (W * H) / 420;
static constexpr int SNAKE_CAP_BASE    = (W * H) / 1200;
static constexpr int FIREFLY_CAP_BASE  = (W * H) / 320;
static constexpr int DEER_CAP_BASE     = (W * H) / 2400;
static constexpr int OWL_CAP_BASE      = (W * H) / 2200;
static constexpr int YETI_CAP_BASE     = (W * H) / 3200;

using Grid = std::vector<std::string>;
using Water = std::vector<std::vector<uint8_t>>;

static inline bool inBounds(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

struct Rng {
  std::mt19937 rng;
  Rng(uint32_t seed=0xC0FFEEu) : rng(seed) {}
  uint32_t u32() { return rng(); }
  int irange(int a,int b){ std::uniform_int_distribution<int> d(a,b); return d(rng); }
  float u01(){ std::uniform_real_distribution<float> d(0.f,1.f); return d(rng); }
  bool oneIn(int n){ std::uniform_int_distribution<int> d(1,n); return d(rng)==1; }
  int i(int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    std::uniform_int_distribution<int> d(lo, hi);
    return d(rng);
  }

};

struct Agent {
  int x=0, y=0;
  char glyph='?';
  float hunger=0.0f;   // 0..1
  float thirst=0.0f;   // 0..1
  float stress=0.0f;   // 0..1
  float health=1.0f;   // 0..1
  uint8_t flags=0;     // bit0:panic
};

static inline bool isAquatic(char g){ return g=='>'||g=='<'||g=='W'||g=='S'||g=='D'; }
static inline bool isHerbivore(char g){ return g=='r'||g=='D'||g=='K'; }
static inline bool isPredator(char g){ return g=='n'||g=='H'||g=='S'||g=='K'; } // dinos can be predators-ish
static inline bool isBird(char g){ return g=='v'||g=='O'||g=='C'; }

static inline bool isEdiblePlant(char t){
  switch(t){
    case ',': case '"': case ';': case 'f': case 'F': case 'p': case 'y': case 'Y': return true;
    default: return false;
  }
}
static inline char grazed(char t){
  switch(t){
    case '"': return ',';
    case ';': return ',';
    case 'f': return ',';
    case ',': return '.';
    default: return t;
  }
}
static inline uint32_t hash3(uint32_t x, uint32_t y, uint32_t salt) {
  uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA6Bu ^ salt * 0xC2B2AE35u;
  h ^= (h >> 16); h *= 0x7FEB352Du; h ^= (h >> 15); h *= 0x846CA68Bu; h ^= (h >> 16);
  return h;
}

static inline uint8_t clampU8(int v){ return (uint8_t)std::clamp(v, 0, 255); }

static int countNeighborsChar(const Grid& g, int x, int y, char c) {
  int n = 0;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (dx==0 && dy==0) continue;
    int nx=x+dx, ny=y+dy;
    if (inBounds(nx,ny) && g[ny][nx]==c) n++;
  }
  return n;
}

static int countNeighborsWater(const Water& w, int x, int y) {
  int n = 0;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (dx==0 && dy==0) continue;
    int nx=x+dx, ny=y+dy;
    if (inBounds(nx,ny) && w[ny][nx]>0) n++;
  }
  return n;
}

enum Season { SPRING=0, SUMMER=1, AUTUMN=2, WINTER=3 };
static inline Season seasonAt(int tick) { return (Season)((tick / SEASON_TICKS) % 4); }
static inline float seasonLerp(int tick) { return float(tick % SEASON_TICKS) / float(SEASON_TICKS); }
static inline bool nightish(int tick) { return ((tick / (DAY_TICKS/2)) % 2) == 1; }

// ---------------- Biomes ----------------
static constexpr int BIOME_COUNT = 6;

enum Biome { MEADOW=0, WETLAND=1, ALPINE=2, ALIEN=3, TROPICAL=4, DESERT=5 };

static const char* biomeName(Biome b) {
  switch (b) {
    case MEADOW: return "meadow";
    case WETLAND:return "wetland";
    case ALPINE: return "alpine";
    case ALIEN:  return "alien";
    case TROPICAL: return "tropical";
    case DESERT: return "desert";
  }
  return "meadow";
}

struct BiomeWeights {
  float pondDensity;
  float stoneChance;
  float reedChance;
  float fernChance;
  float flowerChance;
  float bigFlowerChance;
  float treeChance;
  float mushChance;

  float growRate;
  float bloomRate;
  float fireRate;
  float alienRate;
};

static BiomeWeights weightsFor(Biome b) {
  switch (b) {
    // pondDensity, stoneChance, reedChance, fernChance, flowerChance, bigFlowerChance, treeChance, mushChance,
    // growRate, bloomRate, fireRate, alienRate
    case MEADOW:  // drier + fewer "mud faces" + fewer flowers (more distinct from wetland)
      return {0.35f, 1.0f, 0.55f, 0.85f, 0.55f, 0.55f, 1.0f, 0.90f, 1.0f, 0.85f, 0.90f, 0.70f};
    case WETLAND: // wetter + reed-heavy
      return {1.70f, 0.7f, 1.55f, 1.05f, 0.95f, 0.90f, 0.85f, 1.15f, 1.0f, 1.15f, 0.80f, 0.70f};
    case ALPINE:  // much rockier, fewer ponds, fewer flowers
      return {0.18f, 1.85f, 0.35f, 0.45f, 0.35f, 0.35f, 0.60f, 0.55f, 0.70f, 0.55f, 1.05f, 0.80f};
    case ALIEN:
      return {1.05f, 1.0f, 0.90f, 0.90f, 1.25f, 1.35f, 1.05f, 1.15f, 1.05f, 1.45f, 0.85f, 1.60f};
    case TROPICAL:
      return {1.30f, 0.6f, 1.35f, 1.25f, 1.35f, 1.15f, 1.20f, 1.05f, 1.15f, 1.45f, 0.75f, 1.00f};
    case DESERT:
      return {0.05f, 1.10f, 0.05f, 0.10f, 0.10f, 0.10f, 0.20f, 0.10f, 0.35f, 0.15f, 1.45f, 0.40f};
  }
  return weightsFor(MEADOW);
}

// ---------------- Wind / Weather / Clouds ----------------
struct Wind { int dx=0, dy=0; int strength=0; };
struct Weather {
  WeatherState state = CLEAR;
  float rainStrength = 0.f;
  int timer = 0;
  bool lastTickWasRaining = false;
};
struct Clouds {
  std::vector<uint8_t> field;
  float offX=0.f, offY=0.f;
  Clouds() : field(CW*CH, 0) {}
};

struct World {
  uint32_t worldSeed = 0xC0FFEEu;
  Grid terrain;
  Grid entities;
  Water water;
  std::vector<std::vector<uint8_t>> height;
  Grid overlay;
  Wind wind;
  Weather weather;
  Clouds clouds;
  float cloudOpacity = 1.0f;
  Biome biome;
  Biome targetBiome = MEADOW;
  float biomeFade = 0.0f;
  int biomeFadeDir = 0;
  BiomeWeights bw;
  // Hydrology stability
  std::vector<std::vector<uint8_t>> moist; // 0..255 groundwater/moisture reserve
  std::vector<std::pair<int,int>> springs; // persistent water sources
  std::vector<Agent> agents; // persistent fauna agents (overlay on entities grid)

};

// ---------------- Procedural species variety (visual-only) ----------------
static inline uint32_t biomeSalt(Biome b) {
  switch (b) {
    case MEADOW:   return 0xA17C3u;
    case WETLAND:  return 0x55D1Bu;
    case ALPINE:   return 0xC0FFEu;
    case DESERT:   return 0xD3A5Eu;
    case TROPICAL: return 0x7A0F1u;
    case ALIEN:    return 0xA11E1u;
    default:       return 0xBEEFu;
  }
}

static inline uint32_t speciesSeed2(const World& w, int x, int y) {
  return hash3((uint32_t)x, (uint32_t)y, w.worldSeed ^ biomeSalt(w.biome));
}

static inline int speciesVariant2(const World& w, int x, int y, int n) {
  if (n <= 1) return 0;
  return (int)(speciesSeed2(w,x,y) % (uint32_t)n);
}

// ---------------- Helpers ----------------
static char waterGlyph(uint8_t d) {
  d = (uint8_t)std::min<int>(7, d);
  if (!d) return '.';
  return (char)('0' + d); // '1'..'7'
}


static inline char waterFlowGlyph(const World& w, int x, int y, int tick) {
  uint8_t d0 = w.water[y][x];
  if (!d0) return '.';
  int d = std::min<int>(7, d0);

  int bestDx = 0, bestDy = 0;
  int here = (int)w.height[y][x] + (int)w.water[y][x]*8;
  int bestDrop = 0;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (!dx && !dy) continue;
    int nx=x+dx, ny=y+dy;
    if (!inBounds(nx,ny)) continue;
    int there = (int)w.height[ny][nx] + (int)w.water[ny][nx]*8;
    int drop = here - there;
    if (drop > bestDrop) { bestDrop = drop; bestDx = dx; bestDy = dy; }
  }

  if (bestDrop < 2) return (char)('0' + d);

  int cat = 0; // 0 horiz, 1 vert, 2 diag
  if (bestDx != 0 && bestDy != 0) cat = 2;
  else if (bestDy != 0) cat = 1;

  int base = (cat==0 ? 0x01 : (cat==1 ? 0x08 : 0x0F));
  int phase = ((tick/6) + x + y) & 1;
  if (d >= 4 && phase) d = std::max(1, d-1);
  return (char)(base + (d-1));
}


// Visual helper glyphs (ASCII-safe)
static constexpr char FOAM_GLYPH = '=';   // shoreline/crest foam
static constexpr char LILYPAD_GLYPH = 'l';// lily pads (visual)
static constexpr char KELP_GLYPH = 'u';   // underwater plants (terrain visual + fish cover)

static inline bool isWaterVisualGlyph(unsigned char c) {
  // depth digits and custom wave/flow bitmaps live in low control-code range.
  if (c >= (unsigned char)'1' && c <= (unsigned char)'7') return true;
  if (c == (unsigned char)'0') return true; // used for d>=? and calm water
  if (c == (unsigned char)FOAM_GLYPH) return true;
  if (c < 0x20u && c != 0u) return true; // custom flow glyphs 0x01..0x15
  return false;
}

static inline bool isTree(char c) { return c=='T' || c=='Y' || c=='P'; }
static inline bool isVeg(char c) {
  return (c==','||c=='"'||c=='#'||c=='m'||c=='f'||c=='+'||c=='&'||c==';'||c==':'||c=='$'||c=='!'||isTree(c));
}
static inline bool blocksEntity(char terrain, uint8_t waterDepth) {
  if (waterDepth > 0) return true;
  if (terrain == '*' || terrain == 'B' || terrain == 'M' || terrain == '^' || terrain=='X' || terrain=='c') return true;
  return false;
}

static void initClouds(Clouds& c, Rng& r, Biome b) {
  float base = (b==WETLAND) ? 110.f : (b==ALPINE ? 80.f : 95.f);
  if (b==ALIEN) base = 105.f;
  if (b==TROPICAL) base = 92.f;
  for (int y=0; y<CH; ++y) for (int x=0; x<CW; ++x) {
    float n = r.u01();
    int v = int(base + (n-0.5f)*120.f);
    c.field[y*CW + x] = clampU8(v);
  }
  c.offX = r.u01() * CW;
  c.offY = r.u01() * CH;
}

static void blurClouds(Clouds& c) {
  std::vector<uint8_t> tmp = c.field;
  for (int y=0; y<CH; ++y) for (int x=0; x<CW; ++x) {
    int acc=0, cnt=0;
    for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
      int nx = (x+dx+CW)%CW, ny=(y+dy+CH)%CH;
      acc += tmp[ny*CW+nx];
      cnt++;
    }
    c.field[y*CW+x] = (uint8_t)(acc/cnt);
  }
}

static void evolveClouds(Clouds& c, Rng& r, const Wind& w, const Weather& we, int tick) {
  float speed = 0.010f + 0.008f * (float)w.strength;
  if (we.state == STORM) speed *= 1.6f;
  c.offX += speed * (float)w.dx;
  c.offY += speed * (float)w.dy;
  if (c.offX < 0) c.offX += CW; if (c.offX >= CW) c.offX -= CW;
  if (c.offY < 0) c.offY += CH; if (c.offY >= CH) c.offY -= CH;

  if (tick % 9 == 0) {
    for (int i=0; i< (CW*CH)/18; ++i) {
      int x = r.i(0, CW-1), y = r.i(0, CH-1);
      int idx = y*CW+x;
      int v = (int)c.field[idx] + r.i(-6, 6);
      if (we.state == OVERCAST) v += 3;
      if (we.state == CLEAR) v -= 2;
      if (we.state == RAIN) v += 4;
      if (we.state == STORM) v += 6;
      c.field[idx] = clampU8(v);
    }
  }
  if (tick % 23 == 0) blurClouds(c);
}

static uint8_t sampleCloud(const Clouds& c, int x, int y) {
  // Bilinear sample for smoother/smaller-looking cloud shapes.
  float fx = ((float)x / (float)W) * (float)CW + c.offX;
  float fy = ((float)y / (float)H) * (float)CH + c.offY;
  int x0 = (int)floorf(fx) % CW; if (x0 < 0) x0 += CW;
  int y0 = (int)floorf(fy) % CH; if (y0 < 0) y0 += CH;
  int x1 = (x0 + 1) % CW;
  int y1 = (y0 + 1) % CH;
  float tx = fx - floorf(fx);
  float ty = fy - floorf(fy);
  int a = c.field[y0*CW + x0];
  int b = c.field[y0*CW + x1];
  int d = c.field[y1*CW + x0];
  int e = c.field[y1*CW + x1];
  float top = a + (b - a) * tx;
  float bot = d + (e - d) * tx;
  float v = top + (bot - top) * ty;
  if (v < 0.f) v = 0.f;
  if (v > 255.f) v = 255.f;
  return (uint8_t)(v + 0.5f);
}

static float avgCloud(const Clouds& c) {
  long acc=0;
  for (auto v: c.field) acc += v;
  return (float)acc / (float)(CW*CH);
}

// overlays
static void clearOverlay(World& w) {
  for (int y=0; y<H; ++y) std::fill(w.overlay[y].begin(), w.overlay[y].end(), ' ');
}

static void spawnRainbow(World& w, Rng& r) {
  int cx = W/2 + r.i(-W/10, W/10);
  int cy = H + r.i(H/6, H/3);
  int R  = std::min(W, H) + r.i(10, 60);
  int thick = 3 + r.i(0, 3);
  const char bands[] = {'=', '-', '~', '+', '!'};
  int nb = (int)(sizeof(bands)/sizeof(bands[0]));
  for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
    int dx=x-cx, dy=y-cy;
    int d2=dx*dx + dy*dy;
    int r0=R, r1=R-thick;
    if (d2 <= r0*r0 && d2 >= r1*r1) {
      int band = (x + 2*y) % nb;
      w.overlay[y][x] = bands[band];
    }
  }
}

static void applyRainOverlay(World& w, int tick) {
  if (w.weather.rainStrength <= 0.01f) return;
  int strength = (int)(w.weather.rainStrength * 10.f);
  int drops = (W * H) / std::max(8, 30 - strength*2);

  char streak = '|';
  if (w.wind.strength > 0) {
    if (w.wind.dx > 0) streak = '/';
    else if (w.wind.dx < 0) streak = '\\';
    else streak = '|';
  }

  for (int i=0; i<drops; ++i) {
    int x = (int)(hash3(i, tick, 1337) % W);
    int y = (int)(hash3(i, tick, 7331) % H);
    if (w.overlay[y][x] == ' ') w.overlay[y][x] = streak;
  }
}

static void updateWind(World& w, Rng& r, int tick) {
  if (tick % WIND_CHANGE_TICKS != 0) return;

  int target = 0;
  if (w.weather.state == CLEAR) target = r.i(0, 2);
  if (w.weather.state == OVERCAST) target = r.i(1, 3);
  if (w.weather.state == RAIN) target = r.i(2, 4);
  if (w.weather.state == STORM) target = r.i(3, MAX_WIND);

  if (r.oneIn(8)) { w.wind.strength = 0; w.wind.dx=0; w.wind.dy=0; return; }
  w.wind.strength = std::clamp(w.wind.strength + r.i(-1, 2), 0, MAX_WIND);
  if (w.wind.strength < target && r.oneIn(2)) w.wind.strength++;
  if (w.wind.strength > target && r.oneIn(3)) w.wind.strength--;

  if (w.wind.strength == 0) { w.wind.dx=0; w.wind.dy=0; return; }

  if (r.oneIn(2) || (w.wind.dx==0 && w.wind.dy==0)) {
    int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
    int k = r.i(0,7);
    w.wind.dx = dirs[k][0];
    w.wind.dy = dirs[k][1];
  }
}

static void updateWeather(World& w, Rng& r, int tick) {
  w.weather.timer++;
  Season s = seasonAt(tick);

  // Biome-driven precipitation tendencies (higher => more rain; lower => drier).
  auto biomeRaininess = [&](Biome b)->float{
    switch (b) {
      case WETLAND:  return 1.55f;
      case TROPICAL: return 1.35f;
      case MEADOW:   return 0.45f;
      case ALPINE:   return 0.22f;
      case ALIEN:    return 0.95f;
      case DESERT:   return 0.08f;
      default:       return 0.80f;
    }
  };
  float raininess = biomeRaininess(w.biome);

  float cloudAvg = avgCloud(w.clouds);
  bool cloudy = cloudAvg > 120.f;
  bool veryCloudy = cloudAvg > 150.f;

  auto toState = [&](WeatherState ns){
    w.weather.state = ns;
    w.weather.timer = 0;
  };

  switch (w.weather.state) {
    case CLEAR: {
      w.weather.rainStrength = std::max(0.f, w.weather.rainStrength - 0.02f);
      if (cloudy && r.oneIn(6)) toState(OVERCAST);
      int chance = (s==SUMMER? 220 : s==SPRING? 170 : s==AUTUMN? 190 : 260);
      // Apply biome raininess: lower raininess => higher chance value (rarer transitions)
      chance = (int)std::clamp((float)chance / std::max(0.05f, raininess), 80.0f, 12000.0f);
      if (w.biome==TROPICAL) chance = std::max(120, chance-60);
      if (w.biome==DESERT) chance = std::max(4000, chance+2800); // far rarer rain in desert
      if (r.oneIn(chance)) toState(OVERCAST);
    } break;

    case OVERCAST: {
      w.weather.rainStrength = std::min(0.35f, w.weather.rainStrength + 0.01f);
      if (veryCloudy && w.weather.timer > 80 && r.oneIn(3)) toState(RAIN);
      if (!cloudy && w.weather.timer > 120 && r.oneIn(4)) toState(CLEAR);
      if (w.weather.timer > 600 && r.oneIn(3)) toState(CLEAR);
    } break;

    case RAIN: {
      float maxRain = (w.biome==ALPINE? 0.50f : (w.biome==MEADOW? 0.55f : (w.biome==DESERT? 0.18f : 1.0f)));
      w.weather.rainStrength = std::min(maxRain, w.weather.rainStrength + 0.02f);
      int stormChance = (s==SUMMER ? 180 : 420);
      stormChance = (int)std::clamp((float)stormChance / std::max(0.05f, raininess), 90.0f, 25000.0f);
      if (w.biome==TROPICAL) stormChance = std::max(120, stormChance-80);
      if (w.biome==DESERT) stormChance = std::max(8000, stormChance+6000); // basically no storms
      if (w.weather.timer > 120 && r.oneIn(stormChance)) toState(STORM);
      if (w.weather.timer > 350 && r.oneIn(6)) toState(OVERCAST);
      if (w.weather.timer > 900) toState(OVERCAST);
    } break;

    case STORM: {
      float maxRain = (w.biome==ALPINE? 0.60f : (w.biome==MEADOW? 0.65f : (w.biome==DESERT? 0.22f : 1.0f)));
      w.weather.rainStrength = std::min(maxRain, w.weather.rainStrength + 0.03f);
      if (w.weather.timer > 160 && r.oneIn(4)) toState(RAIN);
      if (w.weather.timer > 420) toState(RAIN);
    } break;
  }

  bool isRainingNow = (w.weather.state == RAIN || w.weather.state == STORM);
  if (w.weather.lastTickWasRaining && !isRainingNow) {
    float cavg = cloudAvg;
    int chance = (cavg < 120.f) ? 2 : (cavg < 140.f ? 3 : 5);
    if (w.biome==TROPICAL) chance = std::max(1, chance-1);
    if (w.biome==DESERT) chance = std::max(1, chance+12); // desert tends to stay clear
    if (r.oneIn(chance)) spawnRainbow(w, r);
  }
  w.weather.lastTickWasRaining = isRainingNow;
}

// ---------------- Seeding world ----------------

static void genHeight(World& w, uint32_t seed) {
  auto noise = [&](int x,int y,int s)->uint8_t{
    uint32_t h = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(seed + (uint32_t)s*1013u));
    return (uint8_t)(h & 255u);
  };

  for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
    int n1 = noise(x/6,  y/6,  1);
    int n2 = noise(x/18, y/18, 2);
    int n3 = noise(x/44, y/44, 3);
    int v  = (n1*6 + n2*3 + n3*2) / 11;

    int ridge = (int)(120 - std::abs(y - H/2)) / 2;
    v = std::clamp(v + ridge, 0, 255);

    w.height[y][x] = (uint8_t)v;
  }

  for (int pass=0; pass<2; ++pass) {
    auto tmp = w.height;
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
      int acc=0,cnt=0;
      for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
        int nx=x+dx, ny=y+dy;
        if (!inBounds(nx,ny)) continue;
        acc += tmp[ny][nx];
        cnt++;
      }
      w.height[y][x] = (uint8_t)(acc/cnt);
    }
  }
}

static void seedWorld(World& w, Rng& r, Biome biome) {
  
  w.worldSeed = r.u32();
w.biome = biome;
  w.targetBiome = biome;
  w.biomeFade = 0.0f;
  w.biomeFadeDir = 0;
  w.bw = weightsFor(biome);

  w.terrain.assign(H, std::string(W, '.'));
  w.entities.assign(H, std::string(W, ' '));
  w.overlay.assign(H, std::string(W, ' '));
  w.water.assign(H, std::vector<uint8_t>(W, 0));
  w.height.assign(H, std::vector<uint8_t>(W, 0));
  w.moist.assign(H, std::vector<uint8_t>(W, 0));
  w.springs.clear();

  w.wind = Wind{0,0,0};
  w.weather = Weather{};
  w.clouds = Clouds{};
  initClouds(w.clouds, r, biome);
  w.cloudOpacity = 1.0f;
  if (biome==TROPICAL) w.cloudOpacity = 0.40f;
  if (biome==DESERT)   w.cloudOpacity = 0.35f;
  if (biome==ALIEN)    w.cloudOpacity = 0.75f;
  genHeight(w, (uint32_t)r.i(0, 0x7fffffff));
  // Biome-specific base terrain + sea level shaping
  if (biome == DESERT) {
    // Mostly sand, very sparse vegetation; water only in small oases.
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
      w.terrain[y][x] = 's';
      w.water[y][x] = 0;
    }
    // Carve a few oases: shallow water + reeds + palms
    int oases = 2 + (r.oneIn(2)?1:0);
    for (int o=0;o<oases;++o) {
      int cx=r.i(W/6, W-1-W/6), cy=r.i(H/6, H-1-H/6);
      int rad=r.i(10, 22);
      for (int y=cy-rad; y<=cy+rad; ++y) for (int x=cx-rad; x<=cx+rad; ++x) {
        if (!inBounds(x,y)) continue;
        int dx=x-cx, dy=y-cy;
        if (dx*dx+dy*dy > rad*rad) continue;
        int d2 = dx*dx+dy*dy;
        if (d2 < (rad*rad)/3) w.water[y][x] = (uint8_t)std::max<int>(w.water[y][x], 3);
        else if (d2 < (rad*rad)*2/3) w.water[y][x] = (uint8_t)std::max<int>(w.water[y][x], 2);
        else w.water[y][x] = (uint8_t)std::max<int>(w.water[y][x], 1);
      }
    }
  } else if (biome == TROPICAL) {
    // Ocean-heavy: low altitude becomes sea; high altitude becomes islands.
    for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
      uint8_t alt = w.height[y][x];
      if (alt < 120) { w.water[y][x] = 5; w.terrain[y][x] = '.'; }
      else if (alt < 150) { w.water[y][x] = 3; w.terrain[y][x] = '.'; }
      else {
        w.water[y][x] = 0;
        w.terrain[y][x] = (alt > 210) ? '^' : '.';
      }
    }
  }


  int basePonds = std::max(4, (W * H) / 9000);
  int ponds = std::max(2, (int)(basePonds * w.bw.pondDensity));
  for (int p=0; p<ponds; ++p) {
    int marginX = std::max(12, W/18);
    int marginY = std::max(8,  H/18);
    int cx = r.i(marginX, W-1-marginX);
    int cy = r.i(marginY, H-1-marginY);
    int rad = r.i(5, 14);

    for (int y=cy-rad; y<=cy+rad; ++y) for (int x=cx-rad; x<=cx+rad; ++x) {
      if (!inBounds(x,y)) continue;
      int dx=x-cx, dy=y-cy;
      int d2 = dx*dx + dy*dy;
      if (d2 <= rad*rad + r.i(-5,5)) {
        uint8_t depth = (uint8_t)std::clamp(7 - (d2 / std::max(1,rad)), 2, 7);
        w.water[y][x] = std::max<uint8_t>(w.water[y][x], depth);
      }
    }
  }

  // height-driven cliffs / boulders / mountain ridges
    // Gentle springs in some ponds/lakes so water doesn't vanish over time.
  // (Helps preserve those nice seed-born ponds even if weather stays dry.)
  {
    int target = std::max(2, ponds);
    int added = 0;
    for (int tries=0; tries<6000 && added<target; ++tries) {
      int x = r.i(0, W-1), y = r.i(0, H-1);
      if (w.water[y][x] >= 5) { w.springs.emplace_back(x,y); ++added; }
    }
  }

for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
    if (w.water[y][x] > 0) continue;
    uint8_t alt = w.height[y][x];
    if (alt > 245) { w.terrain[y][x] = 'M'; if (biome==ALPINE) w.terrain[y][x] = '*'; continue; }
    if (alt > 232 && (biome==ALPINE ? r.oneIn(1) : r.oneIn(2))) w.terrain[y][x] = '^';
    if (alt > 238 && (biome==ALPINE ? r.oneIn(2) : r.oneIn(3))) w.terrain[y][x] = 'B';
  }

  for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
    if (w.water[y][x] > 0) continue;
    uint8_t alt = w.height[y][x];
    // altGrow is currently not used in seedWorld; kept for potential future tuning.
    // float altGrow = 1.0f;
    // if (alt > 220) altGrow *= 0.45f;
    // else if (alt > 200) altGrow *= 0.65f;
    // else if (alt < 80) altGrow *= 1.25f;

    int wet = countNeighborsWater(w.water, x, y);
    if (wet > 0 && r.oneIn(2)) w.terrain[y][x] = ',';
    if (wet > 0 && r.u01() < 0.08f * w.bw.reedChance) w.terrain[y][x] = ':';
  }

  for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
    if (w.water[y][x] > 0) continue;
    if (r.u01() < 0.0016f * w.bw.stoneChance) w.terrain[y][x] = '^';
  }

  // Mud tends to form near water edges (adds earth tones) — but keep it biome-appropriate.
// Meadow should not become "mud-face fields".
  for (int k=0; k< (W*H)/520; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]>0) continue;
    int wet = countNeighborsWater(w.water, x, y);
    if (wet==0) continue;

    // Wetlands make mud readily; meadows rarely; alpine almost never.
    int denomEdge = 5;
    int denomGrass = 7;
    if (biome==WETLAND) { denomEdge = 3; denomGrass = 5; }
    if (biome==TROPICAL){ denomEdge = 4; denomGrass = 6; }
    if (biome==MEADOW)  { denomEdge = 18; denomGrass = 24; }
    if (biome==ALPINE)  { denomEdge = 40; denomGrass = 55; }
    if (biome==DESERT)  { denomEdge = 70; denomGrass = 90; }

    if (wet>0 && r.oneIn(denomEdge) && w.terrain[y][x]=='.') w.terrain[y][x]='d';
    if (wet>1 && r.oneIn(denomGrass) && (w.terrain[y][x]==','||w.terrain[y][x]=='"')) w.terrain[y][x]='d';
  }

  // Extra boulders (earthy accents)
  for (int k=0; k< (W*H)/9000; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]>0) continue;
    if (w.terrain[y][x]=='.' || w.terrain[y][x]=='^') w.terrain[y][x]='B';
  }
  // DESERT cactus scatter
  if (biome == DESERT) {
    for (int k=0; k< (W*H)/180; ++k) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.water[y][x]>0) continue;
      if (w.terrain[y][x]=='s' && r.oneIn(3)) w.terrain[y][x]='c';
    }
  }


  int starters = (W * H) / 700;
  for (int k=0; k<starters; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]==0 && w.terrain[y][x]==',' && r.oneIn(3)) w.terrain[y][x] = '#';
  }
  for (int k=0; k< (W*H)/900; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]==0 && (w.terrain[y][x]==','||w.terrain[y][x]=='#') && r.u01() < 0.20f*w.bw.fernChance) w.terrain[y][x] = ';';
  }
  for (int k=0; k< (W*H)/2200; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]==0 && (w.terrain[y][x]=='#' || w.terrain[y][x]==',') && r.u01() < 0.35f*w.bw.treeChance)
      w.terrain[y][x] = (w.biome==TROPICAL ? (r.oneIn(2)?'P':(r.oneIn(2)?'T':'Y')) : (r.oneIn(2) ? 'T' : 'Y'));
  }

  for (int k=0; k< (W*H)/700; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]==0 && (w.terrain[y][x]=='.' || w.terrain[y][x]==',') && countNeighborsWater(w.water, x, y)>0 && r.u01() < 0.40f*w.bw.mushChance)
      w.terrain[y][x] = 'm';
  }
  for (int k=0; k< (W*H)/900; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]==0 && (w.terrain[y][x]==',' || w.terrain[y][x]=='"' || w.terrain[y][x]==';') && r.u01() < 0.30f*w.bw.flowerChance) {
      float t = r.u01();
      if (t < 0.12f*w.bw.bigFlowerChance) w.terrain[y][x] = '&';
      else w.terrain[y][x] = (r.oneIn(2) ? 'f' : '+');
    }
  }

  if (biome != ALPINE) {
    for (int k=0; k< (W*H)/2400; ++k) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.water[y][x]==0 && (w.terrain[y][x]=='#' || w.terrain[y][x]==';') && r.oneIn(2)) w.terrain[y][x] = '$';
    }
  }

  for (int k=0; k< (W*H)/9000; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]==0 && (w.terrain[y][x]==',' || w.terrain[y][x]=='"') && r.oneIn(2)) w.terrain[y][x] = '!';
  }

  // ---- Hydrology: initialize groundwater moisture + persistent springs ----
  {
    float moistScale = 1.0f;
    if (biome==WETLAND)  moistScale = 1.35f;
    if (biome==DESERT)   moistScale = 0.35f;
    if (biome==TROPICAL) moistScale = 1.15f;
    if (biome==ALPINE)   moistScale = 0.60f;
    if (biome==ALIEN)    moistScale = 0.90f;

    for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
      uint8_t alt = w.height[y][x];
      int base = 120 - (int)alt/2; // lowlands = wetter
      if (base < 0) base = 0;
      int m = (int)(base * moistScale) + r.i(0, 18);
      if (m > 180) m = 180;
      w.moist[y][x] = (uint8_t)m;
    }

    int springCount = 2;
    if (biome==WETLAND) springCount = 3;
    if (biome==DESERT)  springCount = 1;
    if (biome==ALPINE)  springCount = 1;
    if (biome==TROPICAL) springCount = 2;
    if (biome==ALIEN)   springCount = 2;

    auto tooClose = [&](int x,int y){
      for (auto &p : w.springs) {
        int dx = p.first - x, dy = p.second - y;
        if (dx*dx + dy*dy < 70*70) return true;
      }
      return false;
    };

    for (int sidx=0; sidx<springCount; ++sidx) {
      int bestX = W/2, bestY = H/2;
      int bestScore = 1e9;

      for (int tries=0; tries<2600; ++tries) {
        int x = r.i(10, W-11);
        int y = r.i(10, H-11);
        if (tooClose(x,y)) continue;

        uint8_t alt = w.height[y][x];
        // Avoid very high peaks; prefer low basins, but allow midlands for rivers.
        if (biome==ALPINE && alt > 200) continue;
        if (biome!=ALPINE && alt > 210) continue;

        int nmin = 255;
        for (int oy=-1; oy<=1; ++oy) for (int ox=-1; ox<=1; ++ox) {
          if (ox==0 && oy==0) continue;
          uint8_t a2 = w.height[y+oy][x+ox];
          if (a2 < nmin) nmin = a2;
        }

        // Score: low altitude + "basin-ness" bonus.
        int basinBonus = (nmin - (int)alt);
        int score = (int)alt*3 - basinBonus*6 + r.i(0, 30);

        if (biome==DESERT) score += (int)alt; // deserts: prefer the lowest of the low
        if (score < bestScore) { bestScore = score; bestX = x; bestY = y; }
      }

      w.springs.push_back({bestX, bestY});

      // Seed a stable pool at the source + wetter soil around it.
      w.water[bestY][bestX] = (uint8_t)std::max<int>(w.water[bestY][bestX], 6);
      for (int oy=-3; oy<=3; ++oy) for (int ox=-3; ox<=3; ++ox) {
        int x = bestX + ox, y = bestY + oy;
        if (!inBounds(x,y)) continue;
        int dist2 = ox*ox + oy*oy;
        if (dist2 <= 2) w.water[y][x] = (uint8_t)std::max<int>(w.water[y][x], 4);
        int add = (dist2<=4) ? 80 : 35;
        int mm = (int)w.moist[y][x] + add;
        if (mm > 255) mm = 255;
        w.moist[y][x] = (uint8_t)mm;
      }
    }
  }

}

// ---------------- Water flow ----------------
static void stepWater(World& w, Rng& r) {
  Water next = w.water;

  // Persistent springs: keep rivers/lakes alive long-term.
  if (!w.springs.empty()) {
    for (auto &p : w.springs) {
      int sx=p.first, sy=p.second;
      int add = 2;
      if (w.biome==DESERT) add = 1;
      if (w.weather.state==STORM) add = 3;
      if (w.weather.state==RAIN)  add = 2;
      int v = (int)next[sy][sx] + add;
      if (v > 7) v = 7;
      next[sy][sx] = (uint8_t)v;

      // Gentle seep around springs (helps a visible outflow form).
      for (int oy=-1; oy<=1; ++oy) for (int ox=-1; ox<=1; ++ox) {
        if (!ox && !oy) continue;
        int x=sx+ox, y=sy+oy;
        if (!inBounds(x,y)) continue;
        if (r.oneIn(18) && next[y][x] < 5) next[y][x]++;
      }
    }
  }

  // Groundwater recharge: dry tiles with stored moisture can re-wet, especially in basins.
  for (int i=0; i<(W*H)/80; ++i) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (next[y][x] != 0) continue;
    uint8_t m = w.moist[y][x];
    if (m < 20) continue;
    uint8_t alt = w.height[y][x];
    // Basin-ish: lower than neighbors
    int nmin = 255;
    for (int oy=-1; oy<=1; ++oy) for (int ox=-1; ox<=1; ++ox) {
      if (!ox && !oy) continue;
      int nx=x+ox, ny=y+oy;
      if (!inBounds(nx,ny)) continue;
      nmin = std::min<int>(nmin, (int)w.height[ny][nx]);
    }
    bool basin = ((int)alt + 2 <= nmin);
    int denom = basin ? 180 : 420;
    if (w.biome==DESERT) denom *= 2;
    if (w.biome==WETLAND) denom = std::max(80, denom/2);
    if (r.oneIn(denom)) {
      next[y][x] = 1;
      w.moist[y][x] = (uint8_t)std::max<int>(0, (int)m - 18);
    }
  }

  int baseMoves = (W*H)/2;
  int windMoves = (W*H)/8 * w.wind.strength;
  int rainMoves = (w.weather.state==RAIN||w.weather.state==STORM) ? (W*H)/10 : 0;
  int moves = baseMoves + windMoves + rainMoves;

  if (w.weather.rainStrength > 0.05f) {
    int hitsBase = (int)((W*H)/420 * w.weather.rainStrength); // less global flooding
    float rainTileMul = 1.0f;
    if (w.biome==MEADOW) rainTileMul = 0.55f;
    if (w.biome==ALPINE) rainTileMul = 0.35f;
    if (w.biome==WETLAND) rainTileMul = 1.10f;
    if (w.biome==TROPICAL) rainTileMul = 1.05f;
    if (w.biome==DESERT) rainTileMul = 0.08f;
    int hits = (int)std::max(0.0f, hitsBase * rainTileMul);
    for (int i=0; i<hits; ++i) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      int wetN = countNeighborsWater(next, x, y);
      if (wetN == 0 && !r.oneIn(7)) continue;
      if (next[y][x] < 7 && r.oneIn(2)) next[y][x]++;
    }
  }

  
  // Moisture slowly decays (prevents infinite buildup).
  for (int i=0; i<(W*H)/60; ++i) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    uint8_t &m = w.moist[y][x];
    if (m==0) continue;
    if (r.oneIn(10)) m--; // very gentle
  }

for (int k=0; k<moves; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    uint8_t d = next[y][x];
    if (d==0) continue;

    int dirs[6][2] = {{0,1},{-1,1},{1,1},{-1,0},{1,0},{0,-1}};
    int bestNx=x, bestNy=y, bestScore=999999;

    for (int i=0; i<6; ++i) {
      int nx=x+dirs[i][0], ny=y+dirs[i][1];
      if (!inBounds(nx,ny)) continue;
      int score = (int)next[ny][nx]*10 + i;

      if (w.wind.strength>0) {
        int dot = dirs[i][0]*w.wind.dx + dirs[i][1]*w.wind.dy;
        score -= dot * (2 + w.wind.strength);
      }
      score += r.i(0,4);

      if (score < bestScore) { bestScore=score; bestNx=nx; bestNy=ny; }
    }

    if (bestNx==x && bestNy==y) continue;
    uint8_t nd = next[bestNy][bestNx];

    if (nd + 1 < d) { next[y][x]--; next[bestNy][bestNx]++; }
    else if (w.wind.strength>=3 && r.oneIn(7) && nd < d) { next[y][x]--; next[bestNy][bestNx]++; }
    else if (r.oneIn(12) && nd < d) { next[y][x]--; next[bestNy][bestNx]++; }
  }

  for (int y=0;y<H;++y) for (int x=0;x<W;++x) next[y][x] = (uint8_t)std::min<int>(7, next[y][x]);
  w.water.swap(next);
}


static void waterSinks(World& w, Rng& r, Season s) {
  // Prevent long-term "oceanification" by adding sinks.
  float evap = 0.00006f; // evaporation (very gentle so ponds/lakes persist)
  if (s == SUMMER) evap *= 1.6f;
  if (s == WINTER) evap *= 0.55f;

  if (w.weather.state == CLEAR)    evap *= 1.35f;
  if (w.weather.state == OVERCAST) evap *= 0.95f;
  if (w.weather.state == RAIN)     evap *= 0.45f;
  if (w.weather.state == STORM)    evap *= 0.35f;

  evap *= (1.0f + 0.10f * (float)w.wind.strength);

  float infil = 0.00012f; // infiltration (gentle; shallow puddles fade, lakes stay)
  if (w.biome == TROPICAL) infil *= 0.85f;
  if (w.biome == DESERT) infil *= 1.85f;
  if (s == SUMMER) infil *= 1.15f;
  if (s == WINTER) infil *= 0.75f;

  float edgeDrain = 0.0016f; // lower edge drainage
  if (w.biome == WETLAND) edgeDrain *= 0.60f;
  if (w.biome == TROPICAL) edgeDrain *= 0.70f;

  for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
    uint8_t &d = w.water[y][x];
    if (d == 0) continue;


    // Basins/lowlands retain water better.
    uint8_t alt = w.height[y][x];
    int nmin = 255;
    for (int oy=-1; oy<=1; ++oy) for (int ox=-1; ox<=1; ++ox) {
      if (!ox && !oy) continue;
      int nx=x+ox, ny=y+oy;
      if (!inBounds(nx,ny)) continue;
      nmin = std::min<int>(nmin, (int)w.height[ny][nx]);
    }
    bool basin = ((int)alt + 2 <= nmin);
    float retain = 1.0f;
    if (alt < 90) retain *= 0.75f;
    if (basin) retain *= 0.35f;
    if (d >= 4) retain *= 0.55f; // deep water rarely infiltrates/evaps away

    // Infiltration for shallow puddles on porous ground/vegetation.
    if (d <= 2) {
      char t = w.terrain[y][x];
      bool porous =
        (t=='.' || t==',' || t=='"' || t==';' || t=='#' || t==':' || t=='d' ||
         t=='m' || t=='f' || t=='+' || t=='&' || t=='!' || t=='$' || isTree(t));
      if (porous && r.u01() < infil*retain) {
        d--; 
        int mm = (int)w.moist[y][x] + 22;
        if (mm > 255) mm = 255;
        w.moist[y][x] = (uint8_t)mm;
        continue;
      }
    }

    // Evaporation.
    if (r.u01() < evap*retain) {
      d--; 
      int mm = (int)w.moist[y][x] + 6;
      if (mm > 255) mm = 255;
      w.moist[y][x] = (uint8_t)mm;
      continue;
    }

    // Edge drainage.
    bool nearEdge = (x < 2 || y < 2 || x > W-3 || y > H-3);
    if (nearEdge && d <= 3 && r.u01() < edgeDrain*retain) {
      d--; 
      continue;
    }
  }
}



// ---------------- Terrain ecology ----------------
static void stepTerrain(World& w, Rng& r, Season s, int tick) {
  Grid next = w.terrain;

  float springBoost = (s==SPRING) ? 1.35f : 1.0f;
  float autumnMush  = (s==AUTUMN) ? 1.35f : 1.0f;
  float winterSlow  = (s==WINTER) ? 1.55f : 1.0f;

  float rainBoost = (w.weather.state==RAIN || w.weather.state==STORM) ? (1.0f + 0.7f*w.weather.rainStrength) : 1.0f;

  for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
    if (w.water[y][x] > 0) {
      if (w.terrain[y][x] == '*') next[y][x] = 'x';

      // Underwater flora (fish cover): very sparse kelp in shallow water, wind+rain help it.
      if (w.water[y][x] <= 2 && (w.terrain[y][x]=='.' || w.terrain[y][x]==',' || w.terrain[y][x]==';')) {
        int boost = (w.biome==WETLAND || w.biome==TROPICAL) ? 1 : 0;
        boost += (w.weather.state==RAIN || w.weather.state==STORM) ? 1 : 0;
        int chance = 520 - 80*boost; // base very rare
        uint32_t hh = hash3((uint32_t)(x + tick/17), (uint32_t)(y - tick/23), 0x4B454C50u);
        if (chance > 60 && (hh % (uint32_t)chance)==0u) next[y][x] = KELP_GLYPH;
      }
      continue;
    }

    char c = w.terrain[y][x];

    // altitude drives ecology (mountains sparser, valleys richer)
    uint8_t alt = w.height[y][x];
    float altGrow = 1.0f;
    if (alt > 220) altGrow *= 0.45f;
    else if (alt > 200) altGrow *= 0.65f;
    else if (alt < 80) altGrow *= 1.25f;

    if (c=='*') { next[y][x] = (r.oneIn(3) ? 'x' : '*'); continue; }
    if (c=='x') {
      int wet = countNeighborsWater(w.water, x, y);
      if (wet>0 && r.oneIn(6)) next[y][x]=',';
      else if (r.oneIn((int)(35*winterSlow))) next[y][x]='.';
      continue;
    }

    if (isVeg(c)) {
      bool ignite = false;
      for (int dy=-1; dy<=1 && !ignite; ++dy) for (int dx=-1; dx<=1 && !ignite; ++dx) {
        if (dx==0 && dy==0) continue;
        int nx=x+dx, ny=y+dy;
        if (!inBounds(nx,ny)) continue;
        if (w.terrain[ny][nx]=='*') {
          int dot = dx*w.wind.dx + dy*w.wind.dy;
          int boost = 0;
          if (w.wind.strength>0) boost += std::max(0, dot)*w.wind.strength;
          if (w.weather.state==STORM) boost += 2;
          int denom = std::max(2, (int)(10 - boost));
          // tropical slightly more fire-prone during storms
          if (w.biome==TROPICAL && w.weather.state==STORM) denom = std::max(2, denom-1);
          if (r.oneIn(denom)) ignite = true;
        }
      }
      if (ignite) { next[y][x]='*'; continue; }
    }

    int wet = countNeighborsWater(w.water, x, y);
    int g   = countNeighborsChar(w.terrain, x, y, ',');
    int tg  = countNeighborsChar(w.terrain, x, y, '"');
    int sh  = countNeighborsChar(w.terrain, x, y, '#');
    int tr  = countNeighborsChar(w.terrain, x, y, 'T') + countNeighborsChar(w.terrain, x, y, 'Y') + countNeighborsChar(w.terrain, x, y, 'P');
    int flo = countNeighborsChar(w.terrain, x, y, 'f') + countNeighborsChar(w.terrain, x, y, '+') +
              countNeighborsChar(w.terrain, x, y, '&') + countNeighborsChar(w.terrain, x, y, '!') +
              countNeighborsChar(w.terrain, x, y, '$');
    int fern= countNeighborsChar(w.terrain, x, y, ';');
    int reeds=countNeighborsChar(w.terrain, x, y, ':');

    if (c=='.') {
      int fert = wet*3 + g + tg + flo + fern;
      float p = 0.0032f * fert * w.bw.growRate * springBoost * rainBoost * altGrow / winterSlow;
      if (w.biome==TROPICAL) p *= 1.25f;
      if (fert>0 && r.u01() < p) next[y][x] = ',';
      if (wet>0 && r.u01() < 0.05f * w.bw.reedChance * rainBoost / winterSlow) next[y][x] = ':';
      // (disabled) rockification over time tended to turn the whole world into cliffs.
      continue;
    }

    if (c=='^') {
      if (wet>0 && r.oneIn((int)(220 / (w.bw.growRate*rainBoost)))) next[y][x] = ',';
      continue;
    }

    if (c==':') {
      if (wet==0 && r.oneIn((int)(18*winterSlow))) next[y][x]='.';
      if (wet>0 && r.oneIn(40)) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && w.terrain[ny][nx]=='.') next[ny][nx]=':';
      }
      continue;
    }

    if (c==';') {
      if (wet==0 && r.oneIn((int)(30*winterSlow))) next[y][x]=',';
      if (wet>0 && r.oneIn((int)(55 / (springBoost*rainBoost)))) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && (w.terrain[ny][nx]==',' || w.terrain[ny][nx]=='.')) next[ny][nx]=';';
      }
      continue;
    }

    if (c=='d') { // mud dries back into soil/grass when not persistently wet
      if (wet==0) {
        int mm = (int)w.moist[y][x];
        bool dryAir = (w.weather.state==CLEAR || w.weather.state==OVERCAST);
        int denom = 140; // base drying rate
        if (w.biome==MEADOW) denom = 65;
        if (w.biome==ALPINE) denom = 55;
        if (w.biome==WETLAND) denom = 190;
        if (w.biome==TROPICAL) denom = 150;
        if (w.biome==DESERT) denom = 28;
        if (mm < 50 && dryAir && r.oneIn(std::max(10, (int)(denom*winterSlow)))) next[y][x] = (r.oneIn(3)?',':'.');
      }
      // If mud is right at the water edge, allow it to spread slightly in wetlands only.
      if (wet>1 && w.biome==WETLAND && r.oneIn(90)) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && w.terrain[ny][nx]=='.') next[ny][nx]='d';
      }
      continue;
    }

    if (c=='$') {
      if (s==SUMMER && wet>0 && r.oneIn(180)) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && (w.terrain[ny][nx]==',' || w.terrain[ny][nx]==';')) next[ny][nx]='$';
      }
      if (s==WINTER && r.oneIn(80)) next[y][x]='#';
      continue;
    }

    if (c==',') {
      if ((g+tg)>=4 && r.oneIn((int)(90*winterSlow))) next[y][x]='"';
      float flowerScale = (alt > 200) ? 0.35f : 1.0f;
      if (wet>0 && r.u01() < (0.005f * springBoost * rainBoost * w.bw.bloomRate * flowerScale)) {
        float t = r.u01();
        if (t < 0.05f * w.bw.bigFlowerChance) next[y][x] = '!';
        else if (t < 0.16f * w.bw.bigFlowerChance) next[y][x] = '&';
        else next[y][x] = (r.oneIn(2) ? 'f' : '+');
      }
      if (wet>0 && r.oneIn((int)(260 / (springBoost*rainBoost)))) next[y][x]=';';
      if ((wet+g+tg+reeds)==0 && r.oneIn((int)(75*winterSlow))) next[y][x]='.';
      continue;
    }

    if (c=='"') {
      if ((g+tg)>=5 && r.oneIn((int)(140*winterSlow))) next[y][x]='#';
      if (wet>0 && r.u01() < (0.004f * springBoost * rainBoost * w.bw.bloomRate)) {
        float t=r.u01();
        if (t < 0.10f*w.bw.bigFlowerChance) next[y][x]='&';
        else next[y][x] = (r.oneIn(2)?'f':'+');
      }
      if (wet==0 && r.oneIn((int)(120*winterSlow))) next[y][x]=',';
      continue;
    }

    if (c=='#') {
      if ((sh+tr)>=3 && wet>0 && r.oneIn((int)(280*winterSlow)))
        next[y][x]=(w.biome==TROPICAL ? (r.oneIn(2)?'P':(r.oneIn(2)?'T':'Y')) : (r.oneIn(2)?'T':'Y'));
      if (wet==0 && r.oneIn((int)(170*winterSlow))) next[y][x]='"';
      if (wet>0 && r.u01() < 0.006f * autumnMush * rainBoost * w.bw.mushChance) next[y][x]='m';
      if (s==SUMMER && r.oneIn(220) && w.biome!=ALPINE) next[y][x]='$';
      continue;
    }

    if (isTree(c)) {
      if (wet>0 && r.oneIn((int)(230 / (rainBoost)))) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && (w.terrain[ny][nx]=='.'||w.terrain[ny][nx]==',')) {
          int pick = r.i(1, 10);
          if (pick <= 3) next[ny][nx]='m';
          else if (pick <= 5) next[ny][nx]=';';
          else if (pick <= 7) next[ny][nx]='$';
          else if (pick == 8) next[ny][nx]='!';
          else next[ny][nx]=(r.oneIn(2)?'f':'+');
        }
      }
      if (wet==0 && r.oneIn((int)(1400*winterSlow))) next[y][x]='#';
      continue;
    }

    if (c=='m') {
      if (wet==0 && tr==0 && r.oneIn((int)(28*winterSlow))) next[y][x]='.';
      if ((wet+tr)>=2 && r.u01() < 0.02f * autumnMush * rainBoost * w.bw.mushChance) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && (w.terrain[ny][nx]=='.'||w.terrain[ny][nx]==',')) next[ny][nx]='m';
      }
      continue;
    }

    if (c=='f' || c=='+' || c=='&' || c=='!' ) {
      int fade = 170;
      if (s==WINTER) fade = 70;
      if (s==SPRING) fade = 230;
      if (w.weather.state==STORM) fade = std::max(40, fade-30);
      if (w.biome==TROPICAL) fade = (int)(fade * 1.25f);
      if (r.oneIn((int)(fade*winterSlow))) next[y][x] = (r.oneIn(2)?',':'.');

      if (wet>0 && (g+tg+fern)>=3 && r.u01() < 0.0045f * springBoost * rainBoost * w.bw.bloomRate) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && (w.terrain[ny][nx]==','||w.terrain[ny][nx]=='"'||w.terrain[ny][nx]==';')) {
          float t=r.u01();
          if (t < 0.06f*w.bw.bigFlowerChance) next[ny][nx]='!';
          else if (t < 0.18f*w.bw.bigFlowerChance) next[ny][nx]='&';
          else next[ny][nx]=(r.oneIn(2)?'f':'+');
        }
      }
      continue;
    }
  }

  w.terrain.swap(next);
}

// ---------------- Lightning ----------------
static void lightning(World& w, Rng& r, std::string& banner) {
  banner = "STORM: lightning!";
  int strikes = 2 + r.i(0, 4);
  for (int s=0; s<strikes; ++s) {
    int cx=r.i(0,W-1), cy=r.i(0,H-1);
    for (int k=0; k<260; ++k) {
      int x=cx+r.i(-20,20), y=cy+r.i(-12,12);
      if (!inBounds(x,y)) continue;
      if (w.water[y][x] > 0) continue;
      if (isVeg(w.terrain[y][x]) && r.oneIn(2)) w.terrain[y][x]='*';
    }
  }
}

// ---------------- Chaos ----------------
static void chaosAlien(World& w, Rng& r, std::string& banner) {
  banner = "Alien: reality flexes";
  for (int tries=0; tries<800; ++tries) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (w.water[y][x]>0) continue;
    if (w.entities[y][x]==' ') { w.entities[y][x]='A'; break; }
  }
  for (int k=0; k<340; ++k) {
    int x=r.i(0,W-1), y=r.i(0,H-1);
    if (r.oneIn(9)) w.water[y][x] = (uint8_t)r.i(0,7);
    if (w.water[y][x]==0) {
      char &t = w.terrain[y][x];
      if (t=='.' && r.oneIn(3)) t=',';
      else if (t==',' && r.oneIn(3)) t=(r.oneIn(2)?'"':(r.oneIn(2)?';':':'));
      else if (t=='"' && r.oneIn(5)) t='#';
      else if (t=='#' && r.oneIn(6)) t=(r.oneIn(2)?'T':'Y');
      else if (isTree(t) && r.oneIn(18)) t='*';
      else if (t==',' && r.oneIn(20)) t='!';
    }
  }
}

static void maybeChaos(World& w, Rng& r, std::string& banner, Season s) {
  int base = (w.biome==ALIEN) ? 900 : (w.biome==TROPICAL ? 1100 : 1400);
  if (!r.oneIn(base)) { banner = "calm"; return; }

  int roll = r.i(1, 100);
  if (s==SPRING) {
    if (roll <= 55) banner = "spring bloom";
    else if (roll <= 65) chaosAlien(w,r,banner);
    else banner = "fresh wind";
  } else if (s==SUMMER) {
    if (roll <= 45) banner = "summer heat";
    else if (roll <= 70) chaosAlien(w,r,banner);
    else banner = "wild gusts";
  } else if (s==AUTUMN) {
    if (roll <= 35) banner = "spore drift";
    else if (roll <= 55) chaosAlien(w,r,banner);
    else banner = "autumn hush";
  } else {
    if (roll <= 20) chaosAlien(w,r,banner);
    else banner = "winter hush";
  }
}

// ---------------- Entities ----------------

static void agentsInitFromGrid(World& w, Rng& r){
  w.agents.clear();
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      char g = w.entities[y][x];
      if(g=='.' || g==' ') continue;
      Agent a; a.x=x; a.y=y; a.glyph=g;
      // randomize initial needs a bit
      a.hunger = r.u01()*0.5f;
      a.thirst = r.u01()*0.5f;
      a.stress = r.u01()*0.2f;
      a.health = 0.8f + r.u01()*0.2f;
      w.agents.push_back(a);
    }
  }
}

static void agentsWriteToGrid(World& w){
  // clear entities grid
  for(int y=0;y<H;++y) for(int x=0;x<W;++x) w.entities[y][x]='.';
  for(auto &a: w.agents){
    if(!inBounds(a.x,a.y)) continue;
    w.entities[a.y][a.x]=a.glyph;
  }
}

static inline float clamp01(float v){ return v<0.f?0.f:(v>1.f?1.f:v); }

static inline float waterNearby01(const World& w,int x,int y){
  int wet=0, tot=0;
  for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
    if(dx==0 && dy==0) continue;
    int nx=x+dx, ny=y+dy;
    if(!inBounds(nx,ny)) continue;
    tot++;
    if(w.water[ny][nx]>0) wet++;
  }
  return tot? (float)wet/(float)tot : 0.f;
}

static inline int nearestPredatorDist(const World& w,int x,int y,int radius){
  int best=9999;
  for(auto &p: w.agents){
    if(!isPredator(p.glyph)) continue;
    int dx=p.x-x, dy=p.y-y;
    int d=std::abs(dx)+std::abs(dy);
    if(d<best) best=d;
  }
  return best;
}

static inline void moveRandom(Rng& r,int &x,int &y){
  static const int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
  int k=r.irange(0,7);
  x+=dirs[k][0]; y+=dirs[k][1];
}
static void stepEntities(World& w, Rng& r, Season s, int tick) {
  // One-time migration: if agents list is empty, seed it from the existing entity glyph grid.
  if (w.agents.empty()) agentsInitFromGrid(w, r);

  // Update at a lower rate than the main sim tick to keep things cheap.
  // 10Hz-ish: every 6 ticks at 60 TPS, but still works at other TPS.
  bool doUpdate = (tick % 6) == 0;

  if (doUpdate) {
    for (auto &a : w.agents) {
      // Baseline needs
      float dt = 0.1f; // ~100ms
      float biomeThirst = (w.biome==DESERT) ? 1.6f : (w.biome==TROPICAL?0.9f:1.0f);
      float biomeHunger = (w.biome==ALPINE) ? 1.15f : 1.0f;

      a.hunger = clamp01(a.hunger + (0.020f*biomeHunger) * dt);
      a.thirst = clamp01(a.thirst + (0.030f*biomeThirst) * dt);

      // Stress decays
      a.stress = clamp01(a.stress - 0.050f * dt);

      // Threat / chase
      int pd = nearestPredatorDist(w, a.x, a.y, 8);
      if (!isPredator(a.glyph) && pd <= 6) {
        float threat = (6 - pd) / 6.0f;
        a.stress = clamp01(a.stress + (0.45f * threat) * dt);
        if (a.stress > 0.75f) a.flags |= 1;
      } else {
        a.flags &= ~1;
      }

      // Drink if near water (non-aquatic). Aquatic counts as always hydrated.
      if (!isAquatic(a.glyph)) {
        float wet01 = waterNearby01(w, a.x, a.y);
        if (wet01 > 0.3f) a.thirst = clamp01(a.thirst - (0.25f*wet01) * dt);
      } else {
        a.thirst = clamp01(a.thirst - 0.40f * dt);
      }

      // Eat if on edible plant and herbivore-ish
      char &tile = w.terrain[a.y][a.x];
      if (isHerbivore(a.glyph) && isEdiblePlant(tile)) {
        a.hunger = clamp01(a.hunger - 0.30f * dt);
        tile = grazed(tile);
        a.stress = clamp01(a.stress - 0.05f * dt);
      }

      // Health: starve/dehydrate/panic costs
      float harm = 0.0f;
      if (a.hunger > 0.92f) harm += (a.hunger - 0.92f) * 0.6f;
      if (a.thirst > 0.90f) harm += (a.thirst - 0.90f) * 0.9f;
      if ((a.flags & 1) && a.stress > 0.8f) harm += 0.15f * (a.stress - 0.8f);
      a.health = clamp01(a.health - harm * dt);

      // Predators can damage nearby prey
      if (isPredator(a.glyph)) {
        for (auto &prey : w.agents) {
          if (&prey==&a) continue;
          if (isPredator(prey.glyph)) continue;
          int d = std::abs(prey.x - a.x) + std::abs(prey.y - a.y);
          if (d==1 && r.oneIn(8)) {
            prey.health = clamp01(prey.health - 0.25f);
            prey.stress = clamp01(prey.stress + 0.6f);
            prey.flags |= 1;
          }
        }
      }

      // Simple recovery if calm & fed
      if (a.hunger < 0.35f && a.thirst < 0.35f && a.stress < 0.35f) {
        a.health = clamp01(a.health + 0.06f * dt);
      }

      // Movement: very simple, but driven by needs and panic.
      int nx=a.x, ny=a.y;

      if (a.flags & 1) {
        // flee: move away from nearest predator (Manhattan)
        int bestDx=0, bestDy=0, bestScore=-999;
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
          if(dx==0 && dy==0) continue;
          int tx=a.x+dx, ty=a.y+dy;
          if(!inBounds(tx,ty)) continue;
          // avoid deep water if land
          if(!isAquatic(a.glyph) && w.water[ty][tx]>2) continue;
          int d = nearestPredatorDist(w, tx, ty, 8);
          int score = d;
          if(score>bestScore){ bestScore=score; bestDx=dx; bestDy=dy; }
        }
        nx=a.x+bestDx; ny=a.y+bestDy;
      } else if (a.thirst > 0.65f && !isAquatic(a.glyph)) {
        // drift toward water
        int bestDx=0,bestDy=0; float best=waterNearby01(w,a.x,a.y);
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
          if(dx==0 && dy==0) continue;
          int tx=a.x+dx, ty=a.y+dy;
          if(!inBounds(tx,ty)) continue;
          float w01=waterNearby01(w,tx,ty);
          if(w01>best){ best=w01; bestDx=dx; bestDy=dy; }
        }
        nx=a.x+bestDx; ny=a.y+bestDy;
        if(best<0.34f && r.oneIn(3)) moveRandom(r,nx,ny);
      } else if (a.hunger > 0.65f && isHerbivore(a.glyph)) {
        // drift toward plants
        int bestDx=0,bestDy=0; int bestScore=-999;
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
          if(dx==0 && dy==0) continue;
          int tx=a.x+dx, ty=a.y+dy;
          if(!inBounds(tx,ty)) continue;
          if(!isAquatic(a.glyph) && w.water[ty][tx]>2) continue;
          int score = isEdiblePlant(w.terrain[ty][tx]) ? 2 : 0;
          score += (w.water[ty][tx]==0)?1:0;
          if(score>bestScore){ bestScore=score; bestDx=dx; bestDy=dy; }
        }
        nx=a.x+bestDx; ny=a.y+bestDy;
        if(bestScore<=0 && r.oneIn(3)) moveRandom(r,nx,ny);
      } else {
        // wander
        if (r.oneIn(3)) moveRandom(r,nx,ny);
      }

      // Aquatic: keep in water
      if (isAquatic(a.glyph)) {
        if (!inBounds(nx,ny) || w.water[ny][nx]==0) { nx=a.x; ny=a.y; }
      } else {
        if (!inBounds(nx,ny)) { nx=a.x; ny=a.y; }
      }

      a.x=nx; a.y=ny;
    }

    // Cull dead agents (and clear their glyph)
    w.agents.erase(std::remove_if(w.agents.begin(), w.agents.end(),
      [&](const Agent& a){ return a.health <= 0.01f; }), w.agents.end());
  }

  agentsWriteToGrid(w);
}

// big ancient tree anchors 'Q'
static void maybeSpawnAncientTree(World& w, Rng& r) {
  if (!r.oneIn(2200)) return;
  for (int tries=0; tries<500; ++tries) {
    int x=r.i(1, W-2), y=r.i(1, H-2);
    if (w.water[y][x] > 0) continue;
    if (w.entities[y][x] != ' ') continue;
    int trees = countNeighborsChar(w.terrain, x, y, 'T') + countNeighborsChar(w.terrain, x, y, 'Y') + countNeighborsChar(w.terrain, x, y, 'P');
    if (trees < 2) continue;
    w.entities[y][x] = 'Q';
    break;
  }
}

// ---------------- Step ----------------
static void step(World& w, Rng& r, std::string& banner, int tick) {
  clearOverlay(w);

  Season s = seasonAt(tick);

  evolveClouds(w.clouds, r, w.wind, w.weather, tick);
  updateWeather(w, r, tick);
  updateWind(w, r, tick);

  if (w.weather.state == STORM && r.oneIn(35)) lightning(w, r, banner);
  maybeChaos(w, r, banner, s);

  stepWater(w, r);
  waterSinks(w, r, s);
  stepTerrain(w, r, s, tick);
  stepEntities(w, r, s, tick);
  maybeSpawnAncientTree(w, r);

  applyRainOverlay(w, tick);
}

// ---------------- Rendering ----------------
static inline void setColor(SDL_Renderer* rr, uint8_t R, uint8_t G, uint8_t B, uint8_t A=255) {
  SDL_SetRenderDrawColor(rr, R, G, B, A);
}

struct Layout {
  int scale = 2; // UI/font scale
 int screenW=0, screenH=0; int hudH=0; int simHpx=0; };

static Layout computeLayout(SDL_Renderer* ren) {
  Layout L;
  SDL_GetRendererOutputSize(ren, &L.screenW, &L.screenH);
  L.hudH = std::max(40, L.screenH/18);
  L.simHpx = L.screenH - L.hudH;
  return L;
}

// tiny 8x8 glyphs
static const uint8_t* glyph8_world(char c) {
  static const uint8_t BLANK[8]  = {0,0,0,0,0,0,0,0};

  static const uint8_t COMMA[8]  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10};
  static const uint8_t DASH[8]   = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00};
  static const uint8_t WAVE[8]   = {0x00,0x00,0x52,0x2A,0x15,0x0A,0x00,0x00};
  static const uint8_t EQ[8]     = {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00};
  static const uint8_t HASH[8]   = {0x00,0x24,0x7E,0x24,0x24,0x7E,0x24,0x00};
  static const uint8_t PCT[8]    = {0x00,0x62,0x64,0x08,0x10,0x26,0x46,0x00};
  static const uint8_t AT[8]     = {0x00,0x3C,0x42,0x5A,0x5A,0x40,0x3C,0x00};

  static const uint8_t TGRASS[8] = {0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00}; // "
  static const uint8_t SHRUB[8]  = {0x00,0x24,0x7E,0x24,0x24,0x7E,0x24,0x00}; // #
  static const uint8_t TREE1[8]  = {0x10,0x38,0x54,0x10,0x10,0x10,0x38,0x00}; // T
  static const uint8_t TREE2[8]  = {0x10,0x38,0x54,0x10,0x10,0x28,0x44,0x00}; // Y
  static const uint8_t PALM[8]   = {0x10,0x54,0x38,0x10,0x10,0x10,0x38,0x00}; // P
  static const uint8_t MUSH[8]   = {0x00,0x3C,0x7E,0x7E,0x18,0x18,0x3C,0x00}; // m
  static const uint8_t FLOW1[8]  = {0x10,0x54,0x38,0x7C,0x38,0x54,0x10,0x00}; // +
  static const uint8_t FLOW2[8]  = {0x00,0x10,0x38,0x7C,0x38,0x10,0x00,0x00}; // f
  static const uint8_t BIGF[8]   = {0x28,0x7C,0xFE,0x7C,0xFE,0x7C,0x28,0x00}; // &
  static const uint8_t SUPERB[8] = {0x10,0x7C,0xFE,0x7C,0xFE,0x7C,0x10,0x00}; // !
  static const uint8_t FERN[8]   = {0x10,0x38,0x10,0x38,0x10,0x28,0x44,0x00}; // ;
  static const uint8_t REED[8]   = {0x10,0x10,0x10,0x10,0x28,0x28,0x00,0x00}; // :
  static const uint8_t STONE[8]  = {0x00,0x18,0x3C,0x7E,0x7E,0x3C,0x18,0x00}; // ^
  static const uint8_t FRUIT[8]  = {0x18,0x24,0x42,0x5A,0x7E,0x24,0x18,0x00}; // $
  static const uint8_t STAR[8]   = {0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00}; // *
  static const uint8_t EX[8]     = {0x00,0x42,0x24,0x18,0x18,0x24,0x42,0x00}; // x
  static const uint8_t MUD[8]    = {0x00,0x00,0x3C,0x66,0x5A,0x66,0x3C,0x00}; // d
  static const uint8_t BOUL[8]   = {0x00,0x3C,0x7E,0xDB,0xFF,0xE7,0x7E,0x3C}; // B
  static const uint8_t MOUN[8]   = {0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00}; // M
  static const uint8_t LILY[8]   = {0x00,0x38,0x7C,0xFE,0xEE,0x7C,0x38,0x00}; // l
  static const uint8_t SAND[8]   = {0x00,0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00}; // s
  static const uint8_t CACT[8]   = {0x18,0x18,0x5A,0x7E,0x5A,0x18,0x18,0x00}; // c
  static const uint8_t LIZ[8] = {0x00,0x18,0x3C,0x66,0x3C,0x18,0x66,0x00}; // L
  static const uint8_t CAM[8] = {0x00,0x3C,0x66,0x7E,0x5A,0x66,0x24,0x00}; // C
  static const uint8_t DOLP[8] = {0x00,0x1C,0x3E,0x7C,0x3E,0x1C,0x08,0x00}; // D
  static const uint8_t WHAL[8] = {0x00,0x3C,0x7E,0xDB,0xFF,0x7E,0x3C,0x00}; // W
  static const uint8_t SEAM[8] = {0x18,0x3C,0x7E,0xDB,0x7E,0x3C,0x5A,0x00}; // S
  static const uint8_t DINO[8] = {0x00,0x1C,0x3E,0x3F,0x1E,0x3E,0x2A,0x22}; // K (dinosaur)
  static const uint8_t MONO[8]   = {0x18,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x18}; // X
  static const uint8_t EYE[8]    = {0x00,0x3C,0x42,0xA5,0x81,0xA5,0x42,0x3C}; // E

// Extra fauna glyphs (distinct archetypes) + legendary wanderers
static const uint8_t SCOR1[8] = {0x00,0x10,0x38,0x54,0x38,0x10,0x28,0x00}; // \x1E scorpion
static const uint8_t DRGN1[8] = {0x00,0x24,0x18,0x7E,0x18,0x24,0x42,0x00}; // \x1F dragonfly
static const uint8_t CRAB1[8] = {0x00,0x24,0x7E,0x3C,0x3C,0x7E,0x24,0x00}; // \x20 crab
static const uint8_t JELY1[8] = {0x00,0x3C,0x7E,0x7E,0x3C,0x24,0x24,0x00}; // \x21 jellyfish
static const uint8_t CRAW1[8] = {0x00,0x3C,0x5A,0x3C,0x5A,0x3C,0x24,0x00}; // \x22 crawler
static const uint8_t ORB1 [8] = {0x00,0x18,0x3C,0x7E,0x7E,0x3C,0x18,0x00}; // \x23 orb
static const uint8_t HIM1 [8] = {0x00,0x18,0x18,0x3C,0x5A,0x18,0x24,0x00}; // \x19
static const uint8_t HER1 [8] = {0x00,0x18,0x18,0x3C,0x7E,0x18,0x3C,0x00}; // \x1A


  // Water depth glyphs (ASCII digits '1'..'7' but drawn as waves)
  static const uint8_t WAT1[8] = {0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00}; // 1 (spark)
  static const uint8_t WAT2[8] = {0x00,0x00,0x10,0x00,0x04,0x00,0x00,0x00}; // 2
  static const uint8_t WAT3[8] = {0x00,0x00,0x28,0x00,0x10,0x00,0x00,0x00}; // 3
  static const uint8_t WAT4[8] = {0x00,0x00,0x28,0x00,0x28,0x00,0x00,0x00}; // 4
  static const uint8_t WAT5[8] = {0x00,0x44,0x28,0x00,0x44,0x28,0x00,0x00}; // 5
  static const uint8_t WAT6[8] = {0x00,0x44,0x28,0x00,0x44,0x28,0x00,0x44}; // 6
  static const uint8_t WAT7[8] = {0x44,0x28,0x00,0x44,0x28,0x00,0x44,0x28}; // 7 (dense waves)


  // Water flow-direction glyphs (non-printable chars so they don't collide with gameplay glyphs)
  // Horizontal (\x01..\x07), Vertical (\x08..\x0E), Diagonal (\x0F..\x15)
  static const uint8_t W1H[8] = {0x00,0x00,0x00,0x38,0x00,0x00,0x00,0x00};
  static const uint8_t W2H[8] = {0x00,0x00,0x38,0x00,0x1C,0x00,0x00,0x00};
  static const uint8_t W3H[8] = {0x00,0x00,0x38,0x00,0x38,0x00,0x00,0x00};
  static const uint8_t W4H[8] = {0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x00};
  static const uint8_t W5H[8] = {0x00,0x7C,0x00,0x38,0x00,0x7C,0x00,0x00};
  static const uint8_t W6H[8] = {0x00,0x7C,0x00,0x7C,0x00,0x7C,0x00,0x00};
  static const uint8_t W7H[8] = {0x7C,0x00,0x7C,0x00,0x7C,0x00,0x7C,0x00};

  static const uint8_t W1V[8] = {0x00,0x00,0x10,0x10,0x10,0x00,0x00,0x00};
  static const uint8_t W2V[8] = {0x00,0x10,0x10,0x00,0x10,0x10,0x00,0x00};
  static const uint8_t W3V[8] = {0x10,0x10,0x00,0x10,0x10,0x00,0x10,0x10};
  static const uint8_t W4V[8] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00};
  static const uint8_t W5V[8] = {0x1C,0x1C,0x00,0x1C,0x1C,0x00,0x1C,0x1C};
  static const uint8_t W6V[8] = {0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00};
  static const uint8_t W7V[8] = {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C};

  static const uint8_t W1D[8] = {0x00,0x00,0x40,0x03,0x10,0x00,0x00,0x00};
  static const uint8_t W2D[8] = {0x00,0x40,0x03,0x10,0x08,0x00,0x00,0x00};
  static const uint8_t W3D[8] = {0x40,0x03,0x10,0x08,0x04,0x02,0x00,0x00};
  static const uint8_t W4D[8] = {0x40,0x03,0x10,0x08,0x10,0x03,0x40,0x00};
  static const uint8_t W5D[8] = {0x44,0x05,0x11,0x08,0x11,0x05,0x44,0x00};
  static const uint8_t W6D[8] = {0x66,0x33,0x19,0x0C,0x19,0x33,0x66,0x00};
  static const uint8_t W7D[8] = {0x77,0x3B,0x1D,0x0E,0x1D,0x3B,0x77,0x00};

  static const uint8_t BUG[8]    = {0x00,0x18,0x3C,0x5A,0x3C,0x18,0x00,0x00}; // b
  static const uint8_t BIRD[8]   = {0x00,0x00,0x42,0x24,0x18,0x00,0x00,0x00}; // v
  static const uint8_t RAB[8]    = {0x18,0x3C,0x66,0x42,0x42,0x66,0x24,0x00}; // r
  static const uint8_t SNAKE[8]  = {0x00,0x7E,0x40,0x7E,0x02,0x7E,0x00,0x00}; // s
  static const uint8_t GLOW[8]   = {0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00}; // F
  static const uint8_t OWL[8]    = {0x3C,0x7E,0xDB,0xFF,0xBD,0xDB,0x7E,0x3C}; // O
  static const uint8_t YETI[8]   = {0x3C,0x7E,0xFF,0xDB,0xFF,0xDB,0x7E,0x3C}; // H
  static const uint8_t AYY[8]    = {0x00,0x18,0x24,0x42,0x7E,0x42,0x42,0x00}; // A

  static const uint8_t SLASH[8]  = {0x02,0x04,0x08,0x10,0x03,0x40,0x80,0x00};
  static const uint8_t BSLASH[8] = {0x80,0x40,0x03,0x10,0x08,0x04,0x02,0x00};
  static const uint8_t PIPE[8]   = {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18};

  switch (c) {
    case ',': return COMMA;
    case '-': return DASH;
    case '~': return WAVE;
    case '=': return EQ;
    case '#': return HASH;
    case '%': return PCT;
    case '@': return AT;

    case '"': return TGRASS;
    case 'T': return TREE1;
    case 'Y': return TREE2;
    case 'P': return PALM;
    case 'm': return MUSH;
    case '+': return FLOW1;
    case 'f': return FLOW2;
    case '&': return BIGF;
    case '!': return SUPERB;
    case ';': return FERN;
    case ':': return REED;
    case '^': return STONE;
    case '$': return FRUIT;
    case '*': return STAR;
    case 'x': return EX;
    case 'd': return MUD;
    case 'B': return BOUL;
    case 'M': return MOUN;
    case 'l': return LILY;
    case '`': return SAND;
    case 'c': return CACT;
    case 'L': return LIZ;
    case 'C': return CAM;
    case 'D': return DOLP;
    case 'W': return WHAL;
    case 'K': return DINO;
    case 'S': return SEAM;
    case 'X': return MONO;
    case 'E': return EYE;
    case '1': return WAT1;
    case '2': return WAT2;
    case '3': return WAT3;
    case '4': return WAT4;
    case '5': return WAT5;
    case '6': return WAT6;
    case '7': return WAT7;
    case '\x01': return W1H; case '\x02': return W2H; case '\x03': return W3H; case '\x04': return W4H; case '\x05': return W5H; case '\x06': return W6H; case '\x07': return W7H;
    case '\x08': return W1V; case '\x09': return W2V; case '\x0A': return W3V; case '\x0B': return W4V; case '\x0C': return W5V; case '\x0D': return W6V; case '\x0E': return W7V;
    case '\x0F': return W1D; case '\x10': return W2D; case '\x11': return W3D; case '\x12': return W4D; case '\x13': return W5D; case '\x14': return W6D; case '\x15': return W7D;

    case 'b': return BUG;
    case 'v': return BIRD;
    case 'r': return RAB;
    case 'n': return SNAKE;
    case 's': return SAND;
    case 'F': return GLOW;
    case 'O': return OWL;
    case 'H': return YETI;
    case 'A': return AYY;

    case '/': return SLASH;
    case '\\': return BSLASH;
    case '|': return PIPE;

        case '\x19': return HIM1;
    case '\x1A': return HER1;
    case '\x16': return SCOR1;
    case '\x17': return DRGN1;
    case '\x18': return CRAB1;
    case '\x1B': return JELY1;
    case '\x1C': return CRAW1;
    case '\x1D': return ORB1;
    default: return BLANK;
  }
}


// UI/text font: simple 5x7 uppercase (ASCII). This is intentionally separate from the world glyphs
// so menu text stays readable even when letters are used for creatures/terrain in the sim.
static inline const uint8_t* glyph8_text(unsigned char c) {
  static const uint8_t BLANK[8] = {0,0,0,0,0,0,0,0};
  // Map lowercase -> uppercase for UI readability.
  if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');

  // Helper: 5-bit row (bits4..0) -> centered into an 8-bit row (bits6..2)
  #define R(x) (uint8_t)((x) << 2)

  // Common punctuation
  static const uint8_t SPACE[8] = {0,0,0,0,0,0,0,0};
  static const uint8_t DOT[8]   = {0,0,0,0,0,0, R(0b00100), 0};
  static const uint8_t COLON[8] = {0, R(0b00100), 0,0, R(0b00100), 0,0,0};
  static const uint8_t DASH[8]  = {0,0,0, R(0b11111), 0,0,0,0};
  static const uint8_t PLUS[8]  = {0,0, R(0b00100), R(0b11111), R(0b00100), 0,0,0};
  static const uint8_t SLASH[8] = {R(0b00001), R(0b00010), R(0b00100), R(0b01000), R(0b10000),0,0,0};
  static const uint8_t PCT[8]   = {R(0b11001), R(0b11010), R(0b00100), R(0b01000), R(0b10110), 0,0,0};
  static const uint8_t LBR[8]   = {R(0b00110), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00110), 0};
  static const uint8_t RBR[8]   = {R(0b01100), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b01100), 0};
  static const uint8_t LP[8]    = {R(0b00010), R(0b00100), R(0b01000), R(0b01000), R(0b01000), R(0b00100), R(0b00010), 0};
  static const uint8_t RP[8]    = {R(0b01000), R(0b00100), R(0b00010), R(0b00010), R(0b00010), R(0b00100), R(0b01000), 0};
  static const uint8_t EQ[8]    = {0,0, R(0b11111),0, R(0b11111),0,0,0};
  static const uint8_t COMMA[8] = {0,0,0,0,0, R(0b00100), R(0b00100), R(0b01000)};
  static const uint8_t QUOTE[8] = {R(0b00100), R(0b00100),0,0,0,0,0,0};
  static const uint8_t EXCL[8]  = {R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00100),0, R(0b00100),0};

  // Digits 0-9
  static const uint8_t D0[8] = {R(0b01110),R(0b10001),R(0b10011),R(0b10101),R(0b11001),R(0b10001),R(0b01110),0};
  static const uint8_t D1[8] = {R(0b00100),R(0b01100),R(0b00100),R(0b00100),R(0b00100),R(0b00100),R(0b01110),0};
  static const uint8_t D2[8] = {R(0b01110),R(0b10001),R(0b00001),R(0b00010),R(0b00100),R(0b01000),R(0b11111),0};
  static const uint8_t D3[8] = {R(0b11110),R(0b00001),R(0b00001),R(0b01110),R(0b00001),R(0b00001),R(0b11110),0};
  static const uint8_t D4[8] = {R(0b00010),R(0b00110),R(0b01010),R(0b10010),R(0b11111),R(0b00010),R(0b00010),0};
  static const uint8_t D5[8] = {R(0b11111),R(0b10000),R(0b10000),R(0b11110),R(0b00001),R(0b00001),R(0b11110),0};
  static const uint8_t D6[8] = {R(0b01110),R(0b10000),R(0b10000),R(0b11110),R(0b10001),R(0b10001),R(0b01110),0};
  static const uint8_t D7[8] = {R(0b11111),R(0b00001),R(0b00010),R(0b00100),R(0b01000),R(0b01000),R(0b01000),0};
  static const uint8_t D8[8] = {R(0b01110),R(0b10001),R(0b10001),R(0b01110),R(0b10001),R(0b10001),R(0b01110),0};
  static const uint8_t D9[8] = {R(0b01110),R(0b10001),R(0b10001),R(0b01111),R(0b00001),R(0b00001),R(0b01110),0};

  // Letters A-Z (5x7)
  static const uint8_t A[8] = {R(0b01110),R(0b10001),R(0b10001),R(0b11111),R(0b10001),R(0b10001),R(0b10001),0};
  static const uint8_t B[8] = {R(0b11110),R(0b10001),R(0b10001),R(0b11110),R(0b10001),R(0b10001),R(0b11110),0};
  static const uint8_t C[8] = {R(0b01110),R(0b10001),R(0b10000),R(0b10000),R(0b10000),R(0b10001),R(0b01110),0};
  static const uint8_t D[8] = {R(0b11110),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b11110),0};
  static const uint8_t E[8] = {R(0b11111),R(0b10000),R(0b10000),R(0b11110),R(0b10000),R(0b10000),R(0b11111),0};
  static const uint8_t F[8] = {R(0b11111),R(0b10000),R(0b10000),R(0b11110),R(0b10000),R(0b10000),R(0b10000),0};
  static const uint8_t G[8] = {R(0b01110),R(0b10001),R(0b10000),R(0b10111),R(0b10001),R(0b10001),R(0b01110),0};
  static const uint8_t H[8] = {R(0b10001),R(0b10001),R(0b10001),R(0b11111),R(0b10001),R(0b10001),R(0b10001),0};
  static const uint8_t I[8] = {R(0b01110),R(0b00100),R(0b00100),R(0b00100),R(0b00100),R(0b00100),R(0b01110),0};
  static const uint8_t J[8] = {R(0b00111),R(0b00010),R(0b00010),R(0b00010),R(0b10010),R(0b10010),R(0b01100),0};
  static const uint8_t K[8] = {R(0b10001),R(0b10010),R(0b10100),R(0b11000),R(0b10100),R(0b10010),R(0b10001),0};
  static const uint8_t L[8] = {R(0b10000),R(0b10000),R(0b10000),R(0b10000),R(0b10000),R(0b10000),R(0b11111),0};
  static const uint8_t M[8] = {R(0b10001),R(0b11011),R(0b10101),R(0b10101),R(0b10001),R(0b10001),R(0b10001),0};
  static const uint8_t N[8] = {R(0b10001),R(0b11001),R(0b10101),R(0b10011),R(0b10001),R(0b10001),R(0b10001),0};
  static const uint8_t O[8] = {R(0b01110),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b01110),0};
  static const uint8_t P[8] = {R(0b11110),R(0b10001),R(0b10001),R(0b11110),R(0b10000),R(0b10000),R(0b10000),0};
  static const uint8_t Q[8] = {R(0b01110),R(0b10001),R(0b10001),R(0b10001),R(0b10101),R(0b10010),R(0b01101),0};
  static const uint8_t Rr[8]= {R(0b11110),R(0b10001),R(0b10001),R(0b11110),R(0b10100),R(0b10010),R(0b10001),0};
  static const uint8_t S[8] = {R(0b01111),R(0b10000),R(0b10000),R(0b01110),R(0b00001),R(0b00001),R(0b11110),0};
  static const uint8_t T[8] = {R(0b11111),R(0b00100),R(0b00100),R(0b00100),R(0b00100),R(0b00100),R(0b00100),0};
  static const uint8_t U[8] = {R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b01110),0};
  static const uint8_t V[8] = {R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b10001),R(0b01010),R(0b00100),0};
  static const uint8_t W[8] = {R(0b10001),R(0b10001),R(0b10001),R(0b10101),R(0b10101),R(0b10101),R(0b01010),0};
  static const uint8_t X[8] = {R(0b10001),R(0b10001),R(0b01010),R(0b00100),R(0b01010),R(0b10001),R(0b10001),0};
  static const uint8_t Y[8] = {R(0b10001),R(0b10001),R(0b01010),R(0b00100),R(0b00100),R(0b00100),R(0b00100),0};
  static const uint8_t Z[8] = {R(0b11111),R(0b00001),R(0b00010),R(0b00100),R(0b01000),R(0b10000),R(0b11111),0};

  
  // --- Minimal Katakana glyphs for UI (single-byte codes 0x80..0x8A) ---
  // These are *not* UTF-8. They are internal glyph codes used by the menu renderer when UI_KATA is enabled.
  // Codepoints:
  //   0x80 ミ  0x81 ズ  0x82 オ  0x83 ト  0x84 ス  0x85 ポ  0x86 ン  0x87 ー  0x88 メ  0x89 ニ  0x8A ュ
  static const uint8_t K_MI[8]   = {0x00,0x7C,0x10,0x7C,0x10,0x7C,0x00,0x00};
  static const uint8_t K_ZU[8]   = {0x00,0x44,0x7C,0x08,0x10,0x20,0x7C,0x00}; // ス + ゛
  static const uint8_t K_O[8]    = {0x00,0x7C,0x10,0x7C,0x12,0x14,0x18,0x00};
  static const uint8_t K_TO[8]   = {0x00,0x10,0x10,0x10,0x10,0x10,0x7C,0x00};
  static const uint8_t K_SU[8]   = {0x00,0x7C,0x08,0x10,0x20,0x20,0x7C,0x00};
  static const uint8_t K_PO[8]   = {0x00,0x7C,0x10,0x7C,0x10,0x28,0x44,0x40}; // rough ポ (ホ + ○)
  static const uint8_t K_N[8]    = {0x00,0x40,0x20,0x10,0x08,0x08,0x70,0x00};
  static const uint8_t K_LONG[8] = {0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00};
  static const uint8_t K_ME[8]   = {0x00,0x44,0x28,0x10,0x28,0x44,0x00,0x00};
  static const uint8_t K_NI[8]   = {0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x00};
  static const uint8_t K_SYU[8]  = {0x00,0x00,0x50,0x10,0x7C,0x00,0x00,0x00};

switch (c) {
    case ' ': return SPACE;
    case '.': return DOT;
    case ':': return COLON;
    case '-': return DASH;
    case '+': return PLUS;
    case '/': return SLASH;
    case '%': return PCT;
    case '[': return LBR;
    case ']': return RBR;
    case '(': return LP;
    case ')': return RP;
    case '=': return EQ;
    case ',': return COMMA;
    case '"': return QUOTE;
    case '!': return EXCL;

    case '0': return D0; case '1': return D1; case '2': return D2; case '3': return D3; case '4': return D4;
    case '5': return D5; case '6': return D6; case '7': return D7; case '8': return D8; case '9': return D9;

    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E;
    case 'F': return F; case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J;
    case 'K': return K; case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
    case 'P': return P; case 'Q': return Q; case 'R': return Rr; case 'S': return S; case 'T': return T;
    case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X; case 'Y': return Y;
    case 'Z': return Z;
    // Katakana internal glyph codes
    case 0x80: return K_MI;
    case 0x81: return K_ZU;
    case 0x82: return K_O;
    case 0x83: return K_TO;
    case 0x84: return K_SU;
    case 0x85: return K_PO;
    case 0x86: return K_N;
    case 0x87: return K_LONG;
    case 0x88: return K_ME;
    case 0x89: return K_NI;
    case 0x8A: return K_SYU;
    default: return BLANK;
  }

  #undef R
}


struct GlyphCache {
  std::unordered_map<unsigned char, SDL_Texture*> tex;
  bool textMode = false;

  void destroy() {
    for (auto& kv : tex) SDL_DestroyTexture(kv.second);
    tex.clear();
  }

  SDL_Texture* makeGlyph(SDL_Renderer* ren, unsigned char c) {
    SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 8, 8);
    if (!t) return nullptr;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);

    void* pixels=nullptr; int pitch=0;
    if (SDL_LockTexture(t, nullptr, &pixels, &pitch) != 0) {
      SDL_DestroyTexture(t);
      return nullptr;
    }

    for (int y=0; y<8; ++y) {
      uint32_t* px = (uint32_t*)((uint8_t*)pixels + y*pitch);
      for (int x=0; x<8; ++x) px[x] = 0x00000000;
    }

    const uint8_t* g = textMode ? glyph8_text((unsigned char)c) : glyph8_world((char)c);
    for (int y=0; y<8; ++y) {
      uint32_t* px = (uint32_t*)((uint8_t*)pixels + y*pitch);
      uint8_t bits = g[y];
      for (int x=0; x<8; ++x) {
        bool on = (bits & (0x80u >> x)) != 0;
        if (on) px[x] = 0xE0FFFFFF;
      }
    }

    SDL_UnlockTexture(t);
    return t;
  }

  SDL_Texture* get(SDL_Renderer* ren, unsigned char c) {
    auto it = tex.find(c);
    if (it != tex.end()) return it->second;
    SDL_Texture* t = makeGlyph(ren, c);
    if (t) tex[c] = t;
    return t;
  }
};

struct RGB { uint8_t r,g,b; };

static inline void applySeasonTint(uint8_t& r, uint8_t& g, uint8_t& b, Season s, float p) {
  struct Off { int dr,dg,db; };
  auto offsFor = [&](Season ss)->Off{
    switch (ss) {
      case SPRING: return Off{-4, +18, -4};
      case SUMMER: return Off{+8, +10, +0};
      case AUTUMN: return Off{+22, -2, -10};
      case WINTER: return Off{-12, -8, +12};
    }
    return Off{0,0,0};
  };
  Season s2 = (Season)((s+1)%4);
  Off a = offsFor(s), b2 = offsFor(s2);

  float t=p;
  int rr = int(r) + int(a.dr*(1.f-t) + b2.dr*t);
  int gg = int(g) + int(a.dg*(1.f-t) + b2.dg*t);
  int bb = int(b) + int(a.db*(1.f-t) + b2.db*t);

  if (s==WINTER || s2==WINTER) {
    int gray = (rr+gg+bb)/3;
    float wgt = (s==WINTER) ? (0.35f + 0.35f*t) : (0.35f*(1.f-t));
    rr = int(rr*(1.f-wgt) + gray*wgt);
    gg = int(gg*(1.f-wgt) + gray*wgt);
    bb = int(bb*(1.f-wgt) + gray*wgt);
  }

  r = clampU8(rr); g=clampU8(gg); b=clampU8(bb);
}

static inline void vividify(uint8_t& R, uint8_t& G, uint8_t& B) {
  float r = R/255.f, g = G/255.f, b = B/255.f;
  float L = (r+g+b)/3.f;
  r = L + (r-L)*VIVID_SAT;
  g = L + (g-L)*VIVID_SAT;
  b = L + (b-L)*VIVID_SAT;
  r *= VIVID_VAL; g *= VIVID_VAL; b *= VIVID_VAL;
  R = clampU8((int)(r*255.f));
  G = clampU8((int)(g*255.f));
  B = clampU8((int)(b*255.f));
}

static inline void pastelWash(uint8_t& R, uint8_t& G, uint8_t& B, float amt) {
  const int wr = 248, wg = 246, wb = 240;
  R = clampU8((int)(R*(1.f-amt) + wr*amt));
  G = clampU8((int)(G*(1.f-amt) + wg*amt));
  B = clampU8((int)(B*(1.f-amt) + wb*amt));
}

static inline char terrainGlyphVariant(char t, uint32_t h, Season s, const Weather& we) {
  uint32_t k = h & 7u;
  if (t=='f' || t=='+' || t=='&' || t=='!') {
    if ((s==SPRING || we.state==RAIN || we.state==STORM) && k==0) return '!';
    if (k==1) return '&';
    if (k==2) return '+';
    return t;
  }
  return t;
}

static inline char renderCharAt(const World& w, int x, int y, int tick) {
  char e = w.entities[y][x];
  if (e != ' ') {
    if (e=='D') return 'r';
    if (e=='Q') return 'T';
    return e;
  }

  for (int ay=y-1; ay<=y; ++ay) for (int ax=x-1; ax<=x; ++ax) {
    if (!inBounds(ax,ay)) continue;
    if (w.entities[ay][ax]=='D') {
      int dx = x-ax, dy = y-ay;
      if (dx>=0 && dx<2 && dy>=0 && dy<2) {
        if ((dx^dy)==0) return 'r';
        return 'v';
      }
    }
  }

  for (int ay=y-1; ay<=y+1; ++ay) for (int ax=x-1; ax<=x+1; ++ax) {
    if (!inBounds(ax,ay)) continue;
    if (w.entities[ay][ax]=='Q') {
      int dx=x-ax, dy=y-ay;
      if (dx>=-1 && dx<=1 && dy>=-1 && dy<=1) {
        return ((dx+dy)&1) ? 'Y' : 'T';
      }
    }
  }

  char o = w.overlay[y][x];
  if (o != ' ') return o;

  uint8_t d = w.water[y][x];
  if (d>0) {
// shallow-water lily pads (visual only) — now *dynamic* and wind/wave-driven, and MUCH rarer.
// We offset a static hash field by wind + a small wave phase so pads "drift" without needing a particle sim.
if (d <= 2 && w.entities[y][x]==' ' && w.overlay[y][x]==' ') {
  int driftX = (tick/18) * w.wind.dx;
  int driftY = (tick/18) * w.wind.dy;
  int wave   = (tick/9) & 3; // tiny oscillation so it doesn't look locked to the grid
  uint32_t hh = hash3((uint32_t)(x + driftX + wave), (uint32_t)(y + driftY - wave), 555u);
  // density: ~1/73 in shallow water (was ~1/13)
  if ((hh % 73u)==0u) return LILYPAD_GLYPH;
  // occasional foam fleck riding waves in shallows
  if ((hh % 997u)==0u) return FOAM_GLYPH;
}

// Foam: shorelines + windy crests + fast-flow edges
if (d > 0) {
  bool shore=false;
  for (int dy=-1; dy<=1 && !shore; ++dy) for (int dx=-1; dx<=1 && !shore; ++dx) {
    if (!dx && !dy) continue;
    int nx=x+dx, ny=y+dy;
    if (!inBounds(nx,ny)) continue;
    if (w.water[ny][nx]==0) shore=true;
  }
  // wind makes more foam; also add a little phase animation
  int foamChance = shore ? (90 - 10*std::clamp(w.wind.strength,0,5)) : 320;
  // On open water, strong wind should visibly create moving crests.
  if (!shore && w.wind.strength>=2) foamChance = std::max(45, foamChance - 55*w.wind.strength);
  if (!shore && d>=4) foamChance = std::max(38, foamChance - 18);
  int phase = ((tick/5) + x*3 + y*7) & 7;
  if (shore && phase==0) foamChance = std::max(18, foamChance-28);
  uint32_t hh = hash3((uint32_t)(x + tick/7), (uint32_t)(y - tick/11), 0xF0A1u);
  if ((hh % (uint32_t)foamChance)==0u) return FOAM_GLYPH;
}
  // Strong wind: make surface waves more obvious, not just shoreline foam.
  if (w.wind.strength >= 3 && d >= 3) {
    uint32_t wh = hash3((uint32_t)(x + tick/3), (uint32_t)(y + tick/5), 0x57415645u);
    if ((wh % 3u)==0u) {
      int ph = ((tick/4) + x + y) % 3;
      return (char)('5' + ph); // denser wave glyphs
    }
  }
  return waterFlowGlyph(w, x, y, tick); // use wave/flow glyphs (custom low-ASCII)
  }

  return w.terrain[y][x];
}

static RGB baseBgFor(const World& /*w*/, int /*x*/, int /*y*/, int /*tick*/, Season /*s*/, float /*sp*/) {
  // 0.33: background is always pure black; all color lives in glyphs.
  return RGB{0,0,0};
}

static RGB fgForChar(const World& w, char c, Season s, float sp, int tick, int x, int y) {
  // 0.33: All colors are on glyphs; background stays black.
  // sp = cloud shadow strength (0..~0.6). We dim fg under shadow.
  uint32_t h = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(tick/12));
  auto pick = [&](const std::initializer_list<RGB>& pal)->RGB {
    size_t n = pal.size();
    size_t idx = (size_t)((h ^ (h>>11) ^ (h>>21)) % (uint32_t)n);
    auto it = pal.begin();
    std::advance(it, idx);
    return *it;
  };
  auto jitter = [&](RGB c0, int amt)->RGB {
    int jr = (int)((h>>8)&7) - 3;
    int jg = (int)((h>>11)&7) - 3;
    int jb = (int)((h>>14)&7) - 3;
    c0.r = clampU8((int)c0.r + jr*amt);
    c0.g = clampU8((int)c0.g + jg*amt);
    c0.b = clampU8((int)c0.b + jb*amt);
    return c0;
  };

  RGB fg{235,235,235};

  // Empty space: invisible on black
  if (c=='.' || c==' ') fg = {0,0,0};

  // Water depth glyphs from waterGlyph(): ASCII-safe digits '1'..'7'
  else if ((unsigned char)c >= 0x01 && (unsigned char)c <= 0x15) {
    unsigned char uc = (unsigned char)c;
    int d = 1;
    if (uc <= 0x07) d = 1 + (uc - 0x01);
    else if (uc <= 0x0E) d = 1 + (uc - 0x08);
    else d = 1 + (uc - 0x0F);
    uint32_t wh = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(tick/5));
    int shimmer = (int)((wh & 3u)) - 1;
    int r = 34 - d*2;
    int g = 130 + d*3 + shimmer*2;
    int b = 210 + d*6 + shimmer*3;
    int dark = d * 7;
    r = std::max(0, r - dark);
    g = std::max(0, g - dark/2);
    b = std::max(0, b - dark/3);
    fg = { clampU8(r), clampU8(g), clampU8(b) };
  }
  else if ((c>='1' && c<='7') || isWaterVisualGlyph((unsigned char)c)) {
    // Aquamarine glow: brighter, more visible water.
    int d = 0;
    if (inBounds(x,y)) d = std::min<int>(7, (int)w.water[y][x]);
    if (d <= 0) d = (c>='1' && c<='7') ? (int)(c-'0') : 2;

    // base aquamarine ramp
    int r = 16 + d*2;
    int g = 150 + d*8;
    int b = 190 + d*9;

    // wind + storm slightly brightens crests
    int windBoost = std::clamp(w.wind.strength,0,5) * 4;
    if (w.weather.state==STORM) windBoost += 6;
    if (c == FOAM_GLYPH) { return {255,255,255}; }
    r += windBoost; g += windBoost; b += windBoost;

    fg = { clampU8(r), clampU8(g), clampU8(b) };
  }

  // Plants
  else if (c==',' ) { // short grass
    if (w.biome==ALPINE)      fg = jitter(pick({ {80,140,120},{70,130,140},{92,160,150},{86,150,168} }), 5); // cooler, sparser
    else if (w.biome==WETLAND)fg = jitter(pick({ {60,170,120},{52,156,120},{70,190,140},{78,206,152} }), 6); // teal greens
    else if (w.biome==DESERT) fg = jitter(pick({ {150,170,110},{170,190,120},{140,160,96} }), 4); // scrubby
    else                      fg = jitter(pick({ {88,180,110},{68,156,96},{58,140,92},{96,196,132},{78,170,120} }), 6);
  }
  else if (c=='"') { // tall grass
    if (w.biome==ALPINE)       fg = jitter(pick({ {64,120,140},{58,110,150},{74,130,160} }), 5);
    else if (w.biome==WETLAND) fg = jitter(pick({ {50,160,112},{44,146,108},{62,182,132} }), 6);
    else if (w.biome==DESERT)  fg = jitter(pick({ {170,170,120},{190,180,130},{160,160,110} }), 4);
    else                       fg = jitter(pick({ {66,170,98},{52,152,86},{44,136,78},{74,186,122} }), 6);
  }
  else if (c==';') { // shrubs
    if (w.biome==ALPINE)       fg = jitter(pick({ {70,120,130},{62,110,140},{82,132,150} }), 4);
    else if (w.biome==WETLAND) fg = jitter(pick({ {40,126,96},{34,116,92},{52,150,118} }), 5);
    else if (w.biome==DESERT)  fg = jitter(pick({ {140,150,96},{120,132,86},{160,170,110} }), 4);
    else                       fg = jitter(pick({ {52,146,86},{42,126,78},{36,116,72},{62,160,96} }), 5);
  }
  else if (c=='#') { // reeds
    if (w.biome==WETLAND) fg = jitter(pick({ {72,210,150},{62,190,140},{90,230,170} }), 6);
    else if (w.biome==MEADOW) fg = jitter(pick({ {72,180,118},{62,164,110},{82,196,130} }), 5);
    else if (w.biome==ALPINE) fg = jitter(pick({ {90,170,180},{80,160,190},{110,190,205} }), 4);
    else fg = jitter(pick({ {72,190,120},{62,170,112},{82,206,132} }), 5);
  }
  else if (c==':') { // moss terrain
    fg = jitter(pick({ {54,160,100},{44,140,90},{64,178,110} }), 5);
  }
  else if (c=='m') { // mushrooms
    fg = jitter(pick({ {230,210,190},{210,180,220},{255,150,180},{200,245,255},{255,240,170} }), 4);
  }
  else if (c=='d') { // mud/dirt
    fg = jitter(pick({ {110,70,42},{92,58,36},{138,92,58},{120,78,48} }), 3);
  }
  else if (c=='B' || c=='^' || c=='M') { // rocks
    if (c=='M') fg = jitter(pick({ {140,160,185},{120,140,170},{165,185,210} }), 3);
    else if (c=='^') fg = jitter(pick({ {160,160,175},{140,140,160},{185,185,205} }), 3);
    else fg = jitter(pick({ {170,170,178},{152,152,160},{190,190,198} }), 3);
  }
  else if (c=='l') { // lily pads
    if (w.biome==WETLAND) fg = jitter(pick({ {72,210,150},{64,190,140},{86,230,170} }), 5);
    else if (w.biome==MEADOW) fg = jitter(pick({ {92,210,140},{72,190,120},{110,230,160} }), 4);
    else fg = jitter(pick({ {70,190,150},{60,170,140},{92,210,170} }), 4);
  }
  else if (c=='T' || c=='t') { // bark
    fg = jitter(pick({ {140,98,66},{120,82,56},{165,120,84} }), 3);
  }
  else if (c=='F' || c=='P') { // foliage/fronds
    fg = jitter(pick({ {60,190,120},{46,170,110},{70,210,140} }), 5);
  }
  else if (c=='f' || c=='+' || c=='&' || c=='!') { // flowers
    if (w.biome==TROPICAL) {
      fg = jitter(pick({ {255,80,120},{255,140,60},{255,220,60},{140,220,255},{190,120,255} }), 7);
    } else if (w.biome==ALPINE) {
      // Alpine: pale, cold blossoms (edelweiss-ish + tiny pinks)
      fg = jitter(pick({ {245,245,245},{220,235,255},{255,220,240},{210,255,250},{235,235,210} }), 4);
    } else if (w.biome==WETLAND) {
      // Wetland: purples/blues + softer whites
      fg = jitter(pick({ {180,150,255},{140,200,255},{200,170,255},{170,220,255},{240,240,255} }), 5);
    } else if (w.biome==DESERT) {
      // Desert: warm yellows/oranges
      fg = jitter(pick({ {255,220,120},{255,190,90},{255,160,80},{245,235,180} }), 4);
    } else if (w.biome==ALIEN) {
      fg = jitter(pick({ {120,255,180},{170,255,120},{255,110,220},{110,220,255},{255,255,120} }), 7);
    } else {
      // Meadow/default
      fg = jitter(pick({ {255,160,190},{255,220,120},{200,170,255},{160,220,255},{255,190,140},{255,120,150},{245,245,245},{210,255,160} }), 6);
    }
    if (c=='+') { fg.r = clampU8((int)fg.r + 12); fg.g = clampU8((int)fg.g + 8); }
    if (c=='&') { fg.b = clampU8((int)fg.b + 14); }
    if (c=='!') { fg.r = clampU8((int)fg.r + 16); fg.b = clampU8((int)fg.b + 10); }
  }

  
// Water flora & visuals
else if (c==LILYPAD_GLYPH) fg = jitter(pick({ {48,168,92},{56,186,104},{38,150,84} }), 6);
else if (c==KELP_GLYPH)    fg = jitter(pick({ {28,160,120},{22,132,106},{34,186,142} }), 6);

// Creatures / special effects
  else if (c=='r') fg = {255,245,220};
  else if (c=='b') fg = {220,255,180};
  else if (c=='v') fg = {210,210,255};
else if (c=='>' || c=='<') fg = {160, 240, 255}; // fish
else if (c=='C') fg = {220, 220, 230}; // crane
  else if (c=='H') fg = {230,245,255};
  else if (c=='A') fg = {170,255,220};
  else if (c=='D') fg = {160, 220, 255}; // dolphin
  else if (c=='W') fg = {190, 210, 240}; // whale
  else if (c=='K') fg = {170, 200, 120}; // dinosaur
  else if (c=='S') fg = {120, 255, 210}; // sea 
  else if (c=='L') fg = {220, 255, 180}; // lizard
  else if (c=='C') fg = {210, 180, 120}; // camel
  else if (c=='c') fg = {120, 220, 150}; // cactus
  else if (c=='`') fg = {230, 210, 150}; // sand
  else if (c=='X') fg = {180, 120, 255}; // monolith
  else if (c=='E') fg = {255, 110, 140}; // watcher eye

  else if (c=='R') fg = {255,200,120};
  else if (c=='*') fg = {255,255,255};
  else if (c=='o') fg = {255,210,120};
  else if (c=='x') fg = {255,120,120};
  else if (c=='n') fg = {180, 255, 180}; // snake

  // Seasonal tint (subtle)
  if (s == AUTUMN) { fg.r = clampU8((int)fg.r + 10); fg.g = clampU8((int)fg.g - 2); }
  else if (s == WINTER) { fg.r = clampU8((int)fg.r + 6); fg.g = clampU8((int)fg.g + 6); fg.b = clampU8((int)fg.b + 10); }

  // Cloud shadows dim the glyph (bg stays black)
  float dim = std::clamp(1.0f - sp, 0.45f, 1.0f);
  dim *= (1.0f - w.biomeFade);
  fg.r = (uint8_t)(fg.r * dim);
  fg.g = (uint8_t)(fg.g * dim);
  fg.b = (uint8_t)(fg.b * dim);

  vividify(fg.r, fg.g, fg.b);
  return fg;
}
static inline void applyCloudShadow(RGB& bg, uint8_t cloudVal) {
  float c = cloudVal / 255.f;
  float shadow = 1.0f - c * 0.42f;
  bg.r = clampU8((int)(bg.r * shadow));
  bg.g = clampU8((int)(bg.g * shadow));
  bg.b = clampU8((int)(bg.b * shadow));
}

static inline void applyCloudLayer(SDL_Renderer* ren, const SDL_Rect& rc, uint8_t cloudVal) {
  if (cloudVal < 120) return;
  float c = (cloudVal - 120) / 135.f;
  c = std::clamp(c, 0.f, 1.f);
  uint8_t alpha = (uint8_t)(c * 60);
  setColor(ren, 180, 190, 210, alpha);
  SDL_RenderFillRect(ren, &rc);
}


// ===== MIDI + menu plumbing (0.48w) =====
enum ScaleType { SCALE_CHROMATIC=0, SCALE_MAJOR=1, SCALE_MINOR=2, SCALE_PENTATONIC=3, SCALE_DORIAN=4, SCALE_LYDIAN=5, SCALE_WHOLE=6 };
enum UiLang { UI_EN=0, UI_KATA=1 }; // UI language: English or Katakana


static inline int quantizeNoteToScale(int midiNote, int root /*0=C*/, ScaleType st) {
  static const int major[7]  = {0,2,4,5,7,9,11};
  static const int minor[7]  = {0,2,3,5,7,8,10};
  static const int pent[5]   = {0,2,4,7,9};
  static const int dorian[7] = {0,2,3,5,7,9,10};
  static const int lydian[7] = {0,2,4,6,7,9,11};
  static const int whole[6]  = {0,2,4,6,8,10};
  const int n = std::clamp(midiNote, 0, 127);
  if (st==SCALE_CHROMATIC) return n;
  int pc = (n - root) % 12; if (pc<0) pc += 12;
  auto snap = [&](const int* arr, int count){
    int best = arr[0], bestd = 99;
    for (int i=0;i<count;i++){
      int d = std::abs(arr[i]-pc);
      d = std::min(d, 12-d);
      if (d<bestd){ bestd=d; best=arr[i]; }
    }
    return best;
  };
  int targetPc = pc;
  if (st==SCALE_MAJOR) targetPc = snap(major,7);
  else if (st==SCALE_MINOR) targetPc = snap(minor,7);
  else if (st==SCALE_PENTATONIC) targetPc = snap(pent,5);
  else if (st==SCALE_DORIAN) targetPc = snap(dorian,7);
  else if (st==SCALE_LYDIAN) targetPc = snap(lydian,7);
  else if (st==SCALE_WHOLE)  targetPc = snap(whole,6);
  int out = n + (targetPc - pc);
  if (out < 0) out += 12;
  if (out > 127) out -= 12;
  return std::clamp(out,0,127);
}


struct MidiParam { const char* name; int cc; float weight; float value01; float lastSent01; };
struct MidiEvent { int note=60; int vel=90; int durTicks=24; int startTick=0; };

// Linux: no-op MIDI backend (Windows will use WinMM later).
struct MidiOut {
  bool enabled=false;
  bool open(int /*deviceIndex*/) { enabled=false; return false; }
  void close() {}
  void sendCC(int /*ch*/, int /*cc*/, int /*val*/) {}
  void sendNoteOn(int /*ch*/, int /*note*/, int /*vel*/) {}
  void sendNoteOff(int /*ch*/, int /*note*/, int /*vel*/=0) {}
  void sendClock() {}
  void sendStart() {}
  void sendStop() {}
};



// ---------------- Optional built-in synth (FluidSynth + SoundFont) ----------------
// Enable by compiling with: -DUSE_FLUIDSYNTH and linking to fluidsynth.
// Linux example:
//   sudo apt install -y libfluidsynth-dev
//   g++ -O2 -std=c++17 terrarium_0.48w_fixed6.cpp -o terrarium `sdl2-config --cflags --libs` -lfluidsynth -DUSE_FLUIDSYNTH
//
// Runtime flags:
//   --synth            enable built-in synth
//   --sf2 <path.sf2>   SoundFont to load (default tries a few common paths)
//   --gain <0..2>      synth gain (default 0.7)

#ifdef USE_FLUIDSYNTH
  #include <fluidsynth.h>
#endif

struct SynthOut {
  bool enabled=false;
  int sampleRate=48000;

#ifdef USE_FLUIDSYNTH
fluid_settings_t* settings=nullptr;
fluid_synth_t* synth=nullptr;
fluid_audio_driver_t* adriver=nullptr;
int sfid=-1;
float gain=0.7f;
std::string audioDriver="";   // pipewire/pulseaudio/alsa/jack/...
std::string audioDevice="";   // optional device name

bool open(const std::string& sf2Path, float g,
          const std::string& driverStr,
          const std::string& deviceStr) {
  gain = g;
  audioDriver = driverStr;
  audioDevice = deviceStr;

  settings = new_fluid_settings();
  if (!settings) return false;

  fluid_settings_setnum(settings, "synth.gain", (double)gain);
  fluid_settings_setnum(settings, "synth.sample-rate", (double)sampleRate);

  // FX (set via settings to avoid deprecated synth API calls)
  fluid_settings_setint(settings, "synth.reverb.active", 1);
  fluid_settings_setnum(settings, "synth.reverb.room-size", 0.90);
  fluid_settings_setnum(settings, "synth.reverb.damp",      0.30);
  fluid_settings_setnum(settings, "synth.reverb.width",     100.0);
  fluid_settings_setnum(settings, "synth.reverb.level",     0.70);

  fluid_settings_setint(settings, "synth.chorus.active", 1);
  fluid_settings_setint(settings, "synth.chorus.nr",     4);
  fluid_settings_setnum(settings, "synth.chorus.level",  1.10);
  fluid_settings_setnum(settings, "synth.chorus.speed",  0.20);
  fluid_settings_setnum(settings, "synth.chorus.depth",  9.0);
  fluid_settings_setint(settings, "synth.chorus.type",   FLUID_CHORUS_MOD_SINE);

  // Keep polyphony modest for delicate bell/chime patches
  fluid_settings_setint(settings, "synth.polyphony", 16);

  // Prefer explicit driver if provided
  if (!audioDriver.empty()) {
    fluid_settings_setstr(settings, "audio.driver", audioDriver.c_str());
    // Device names are backend-specific; set the common ones if we can.
    if (!audioDevice.empty()) {
      if (audioDriver=="alsa") {
        fluid_settings_setstr(settings, "audio.alsa.device", audioDevice.c_str());
      } else if (audioDriver=="pulseaudio") {
        fluid_settings_setstr(settings, "audio.pulseaudio.device", audioDevice.c_str());
      } else if (audioDriver=="jack") {
        fluid_settings_setstr(settings, "audio.jack.id", audioDevice.c_str());
      }
      // (pipewire typically doesn't require device selection)
    }
  }

  synth = new_fluid_synth(settings);
  if (!synth) return false;

  sfid = fluid_synth_sfload(synth, sf2Path.c_str(), 1);
  if (sfid < 0) return false;

  // Program 0 on ch0 for melodic; ch9 drums if present.
  fluid_synth_program_change(synth, 0, 10); // Music Box
  fluid_synth_program_change(synth, 9, 0);

  // Create FluidSynth audio driver (more reliable than going through SDL audio on Linux).
  adriver = new_fluid_audio_driver(settings, synth);
  if (!adriver) return false;

  enabled = true;
  return true;
}

void close() {
  if (adriver) { delete_fluid_audio_driver(adriver); adriver=nullptr; }
  if (synth) { delete_fluid_synth(synth); synth=nullptr; }
  if (settings) { delete_fluid_settings(settings); settings=nullptr; }
  enabled=false;
}

void noteOn(int ch,int note,int vel){ if(enabled) fluid_synth_noteon(synth,ch,note,vel); }
void noteOff(int ch,int note,int vel=0){ if(enabled) fluid_synth_noteoff(synth,ch,note); (void)vel; }
void cc(int ch,int cc,int val){ if(enabled) fluid_synth_cc(synth,ch,cc,val); }

#else
  bool open(const std::string&, float){ enabled=false; return false; }
  void close() {}
  void noteOn(int,int,int) {}
  void noteOff(int,int,int=0) {}
  void cc(int,int,int) {}
#endif
};

// ---- Built-in synth music driver ----
// Called whenever the simulation advances a tick (realtime or single-step).
static inline void synthTickMusic(SynthOut& synth, const World& world, Rng& r, int tick,
                                 int& heldNote, int& heldNote2, int rootKey, ScaleType scaleType,
                                 const std::vector<MidiParam>& params)
{
  if (!synth.enabled) return;

  // --- Instrument selection (bell/chime/light palette) ---
// Default timbre now varies by biome (each biome gets its own “instrument palette”),
// while still allowing an explicit override via MIDI param "Instr" (0..1).
  static int currentProgram = -1;
  static int nextAutoChangeTick = 0;

  auto chooseFromPaletteGlobal = [&](float x01)->int {
    // Curated global “magical marimba / musicbox / chime” palette (GM-ish; depends on SF2)
    // 10 Music Box, 12 Marimba, 11 Vibraphone, 14 Tubular Bells, 98 Crystal, 102 Echo Drops
    static const int pal[] = { 10, 12, 11, 14, 98, 102 };
    const int n = (int)(sizeof(pal)/sizeof(pal[0]));
    int idx = (int)std::floor(std::clamp(x01, 0.0f, 0.9999f) * n);
    idx = std::clamp(idx, 0, n-1);
    return pal[idx];
  };

  auto chooseFromBiomePalette = [&](Biome b, float x01)->int {
    // Biome-specific timbre sets (still bell/chime/light), to give each biome an identity.
    // (GM-ish program numbers; exact sound depends on the chosen SF2.)
    const int* pal = nullptr;
    int n = 0;
    static const int pal_meadow[]  = { 10, 12, 11, 98, 15, 102 }; // music box / marimba / vibes / crystal / dulcimer / echo
    static const int pal_wetland[] = { 11, 98, 102, 10, 99, 14 }; // wetter shimmer + echo
    static const int pal_alpine[]  = { 14, 98, 10, 102, 11 };     // bells/crystal/air
    static const int pal_trop[]    = { 12, 11, 10, 15, 98 };      // brighter wood/metal
    static const int pal_desert[]  = { 15, 10, 98, 11 };          // sparse, dry, delicate
    static const int pal_alien[]   = { 98, 102, 100, 101, 11 };   // uncanny padlets/echo
    switch (b) {
      case MEADOW:   pal = pal_meadow;  n = (int)(sizeof(pal_meadow)/sizeof(pal_meadow[0])); break;
      case WETLAND:  pal = pal_wetland; n = (int)(sizeof(pal_wetland)/sizeof(pal_wetland[0])); break;
      case ALPINE:   pal = pal_alpine;  n = (int)(sizeof(pal_alpine)/sizeof(pal_alpine[0])); break;
      case TROPICAL: pal = pal_trop;    n = (int)(sizeof(pal_trop)/sizeof(pal_trop[0])); break;
      case DESERT:   pal = pal_desert;  n = (int)(sizeof(pal_desert)/sizeof(pal_desert[0])); break;
      case ALIEN:    pal = pal_alien;   n = (int)(sizeof(pal_alien)/sizeof(pal_alien[0])); break;
      default:       pal = pal_meadow;  n = (int)(sizeof(pal_meadow)/sizeof(pal_meadow[0])); break;
    }
    int idx = (int)std::floor(std::clamp(x01, 0.0f, 0.9999f) * (float)n);
    idx = std::clamp(idx, 0, n-1);
    return pal[idx];
  };

  auto getParam01 = [&](const char* name, float def01)->float{
    for (const auto& p : params) if (p.name && std::strcmp(p.name, name)==0) return std::clamp(p.value01, 0.0f, 1.0f);
    return def01;
  };

  // If the Instr knob has moved from near-zero, treat it as an explicit selection (global palette).
  float instr01 = getParam01("Instr", 0.0f);
  bool instrExplicit = instr01 > 0.01f;

  auto applyProgram = [&](int prog){
#ifdef USE_FLUIDSYNTH
    if (prog != currentProgram) {
      fluid_synth_program_change(synth.synth, 0, prog);
      currentProgram = prog;
    }
#else
    (void)prog;
#endif
  };

  // Auto-switch rarely based on sim state, but only when not explicitly overridden.
  if (!instrExplicit && tick >= nextAutoChangeTick) {
    // Spread changes out; slightly more likely during storms or when big sea life is around.
    int waterTiles = 0, whales = 0;
    for (int yy = 0; yy < H; ++yy) for (int xx = 0; xx < W; ++xx) {
      if (world.water[yy][xx] > 0) waterTiles++;
      if (world.entities[yy][xx] == 'W') whales++;
    }
    float w01 = (float)waterTiles / (float)(W*H);

    int baseInterval = 60 * 8; // ~8 seconds at 60 TPS (rough)
    int jitter = r.irange(-180, 420);
    int boost = (world.weather.state==STORM ? -180 : 0) + (whales>0 ? -120 : 0) + (w01>0.60f ? -90 : 0);
    nextAutoChangeTick = tick + std::max(240, baseInterval + jitter + boost);

    // Pick within the current biome palette: wetter -> more “Crystal/Echo”, drier -> Music Box/Marimba/Dulcimer.
    float t = std::clamp((w01 - 0.20f) / 0.65f, 0.0f, 1.0f);
    float pick = std::clamp(t + (r.u01()*0.40f - 0.20f), 0.0f, 0.9999f);
    int prog = chooseFromBiomePalette((Biome)world.biome, pick);
    applyProgram(prog);
  }

  if (instrExplicit) {
    // Explicit override uses the global palette for predictable knob behavior across biomes.
    int prog = chooseFromPaletteGlobal(instr01);
    applyProgram(prog);
  } else if (currentProgram < 0) {
    // Default per-biome starting timbre (slightly biased toward “music box” end).
    int prog = chooseFromBiomePalette((Biome)world.biome, 0.12f);
    applyProgram(prog);
  }
// --- Note generator ---
// More life: variable rhythm + parameter-driven density/contour.
static int nextTick = 0;
static float center = 62.0f;
static int lastHeld1 = -1, lastHeld2 = -1;
static int holdUntil1 = 0, holdUntil2 = 0;

// Grab modulation params (already 0..1 and weight-scaled in the UI loop).
float water01 = getParam01("Water", 0.0f);
float rain01  = getParam01("Rain",  0.0f);
float wind01  = getParam01("Wind",  0.0f);
float flora01 = getParam01("Flora", 0.0f);
float fauna01 = getParam01("Fauna", 0.0f);


// Auto key/scale modulation (sim-derived).
float autoKey01   = getParam01("AutoKey",   1.0f);
float autoScale01 = getParam01("AutoScale", 1.0f);

// Sample the world to derive an abstract “dominant plant” + diversity + lifecycle phase.
// This acts like a mod/melody engine: each piece comes from a different sim source.
int sampleStep2 = 8;
int counts[256] = {0};
int plantCount = 0;
int moistSum = 0, moistN = 0;
for (int yy=0; yy<H; yy+=sampleStep2) for (int xx=0; xx<W; xx+=sampleStep2) {
  unsigned char t = (unsigned char)world.terrain[yy][xx];
  // treat these as “plants/biota” for dominance purposes
  if (t==','||t=='"'||t==';'||t=='m'||t=='f'||t=='+'||t=='&'||t=='$'||t=='#'||t=='T'||t=='Y'||t=='P'||t==KELP_GLYPH) {
    counts[t]++; plantCount++;
  }
  moistSum += (int)world.moist[yy][xx];
  moistN++;
}
int dom = 0, domC = 0;
for (int i=0;i<256;i++){ if (counts[i]>domC){ domC=counts[i]; dom=i; } }

// Diversity (Shannon-ish, normalized)
float Hdiv = 0.f;
if (plantCount>0){
  for (int i=0;i<256;i++){
    if (!counts[i]) continue;
    float p = (float)counts[i]/(float)plantCount;
    Hdiv += -p * std::log(std::max(p, 1e-6f));
  }
  // normalize by log(N) where N is number of categories seen (approx)
  int cats=0; for(int i=0;i<256;i++) if(counts[i]) cats++;
  if (cats>1) Hdiv /= std::log((float)cats);
}
float diversity01 = std::clamp(Hdiv, 0.0f, 1.0f);

// “Lifecycle phase”: slow LFO driven by season + moisture drift.
float moist01 = moistN? ((float)moistSum/(float)moistN)/255.f : 0.f;
float seasonPhase = (float)(tick % (SEASON_TICKS*4)) / (float)(SEASON_TICKS*4); // 0..1 over full year
float lifePhase01 = std::fmod(seasonPhase + moist01*0.35f + (dom * 0.001f), 1.0f);

// Map dominant plant + biome to a musical root (circle-of-fifths-ish) and a mode.
auto hash32 = [&](uint32_t x)->uint32_t{
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x;
};
uint32_t h = hash32((uint32_t)dom * 131u + (uint32_t)world.biome * 911u + world.worldSeed);
int rootAuto = (int)(h % 12); // 0=C
// Seasonal drift: winter darker, summer brighter (small, not constant climbing)
if (seasonAt(tick) == WINTER) rootAuto = (rootAuto + 10) % 12; // -2
else if (seasonAt(tick) == SPRING) rootAuto = (rootAuto + 0) % 12;
else if (seasonAt(tick) == SUMMER) rootAuto = (rootAuto + 2) % 12;
else if (seasonAt(tick) == AUTUMN)   rootAuto = (rootAuto + 0) % 12;

// Mode decision: diversity + lifecycle + water decide “brightness”.
float brightness = 0.45f*diversity01 + 0.35f*(1.0f-lifePhase01) + 0.20f*(1.0f-water01);
// CCs can modulate brightness: wind brightens, rain darkens.
brightness = std::clamp(brightness + 0.18f*wind01 - 0.18f*rain01, 0.0f, 1.0f);

ScaleType scaleAuto = SCALE_PENTATONIC;
if (brightness > 0.82f) scaleAuto = SCALE_LYDIAN;
else if (brightness > 0.62f) scaleAuto = SCALE_MAJOR;
else if (brightness > 0.42f) scaleAuto = SCALE_DORIAN;
else scaleAuto = SCALE_MINOR;
// If it's very watery, prefer pentatonic (chime-friendly) regardless.
if (water01 > 0.70f && brightness > 0.35f) scaleAuto = SCALE_PENTATONIC;

// Blend manual key/scale with sim-derived key/scale.
int rootKeyUsed = rootKey;
ScaleType scaleUsed = scaleType;
if (autoKey01 > 0.01f){
  // blend by stepping toward rootAuto slowly; avoid sudden key jumps.
  static int rootSmooth = rootKey;
  // small chance to move toward new root; more likely on phrase boundaries (approx by life phase)
  if (r.u01() < (0.05f + 0.10f*autoKey01) * (0.35f + 0.65f*diversity01)) {
    int diff = (rootAuto - rootSmooth + 12) % 12;
    if (diff == 0) {}
    else if (diff > 6) rootSmooth = (rootSmooth + 11) % 12; // step down
    else rootSmooth = (rootSmooth + 1) % 12; // step up
  }
  rootKeyUsed = rootSmooth;
}
if (autoScale01 > 0.01f){
  // allow mode flips only occasionally, and bias toward pentatonic for chimes.
  static ScaleType modeSmooth = scaleType;
  if (modeSmooth != scaleAuto && r.u01() < 0.03f + 0.08f*autoScale01) modeSmooth = scaleAuto;
  scaleUsed = modeSmooth;
}


// Release held notes when their hold time expires.
if (heldNote >= 0 && tick >= holdUntil1) { synth.noteOff(0, heldNote, 0); heldNote = -1; }
if (heldNote2>= 0 && tick >= holdUntil2) { synth.noteOff(0, heldNote2,0); heldNote2= -1; }

// Not time to trigger a new note yet.
if (tick < nextTick) return;

// Compute activity (how busy rhythm should be).
// Wind/rain make it more active; fauna adds aleatoric jitter; flora smooths it.
float activity = std::clamp(0.45f*wind01 + 0.45f*rain01 + 0.30f*fauna01 - 0.20f*flora01, 0.0f, 1.0f);

// Choose next inter-onset interval in ticks.
// We use BIOME-SPECIFIC motif “grammars” (phrase patterns) that evolve with sim state.
// Each biome has a distinct rhythmic feel:
//   MEADOW  : music-box ostinati (repeating cells + occasional sparkle)
//   WETLAND : slow shimmer / gentle polyrhythm (longer gaps, occasional syncopation)
//   ALPINE  : sparse bell punctures (long rests, high register)
//   TROPICAL: lively syncopation (shorter intervals, more motion)
//   DESERT  : minimal / breathy (very sparse, occasional accented hits)
//   ALIEN   : uncanny cycles (odd subdivisions, rare “broken” meters)
static int phraseLen = 6;
static int phrasePos = 0;
static int pat[8] = { 18, 18, 24, 12, 18, 24, 12, 30 };

auto regenPattern = [&](){
  // Choose a motif family based on biome + activity + lifecycle.
  // We do NOT hard-lock the tempo; we bias the distribution.
  int n = 0;
  const int* base = nullptr;

  // Helper for selecting a base pool.
  auto pickPool = [&](const int* a, int an, const int* b, int bn, float t)->const int*{
    // t ~0 chooses a, t ~1 chooses b
    return (r.u01() < t) ? b : a;
  };

  // Base pools (tick intervals). 12~ fast, 24~ moderate, 36~ slow.
  static const int MEA_A[] = { 12, 12, 18, 18, 24 };
  static const int MEA_B[] = {  9, 12, 15, 18, 21 }; // more sparkle
  static const int WET_A[] = { 18, 24, 30, 36 };
  static const int WET_B[] = { 15, 18, 24, 27, 30, 33 }; // gentle polyrhythm
  static const int ALP_A[] = { 24, 30, 36, 42, 48 };
  static const int ALP_B[] = { 18, 24, 30, 36, 54 }; // occasional shorter puncture
  static const int TRO_A[] = {  9, 12, 12, 15, 18 };
  static const int TRO_B[] = {  6,  9, 12, 15, 18, 21 }; // lively
  static const int DES_A[] = { 30, 36, 42, 48, 60 };
  static const int DES_B[] = { 24, 30, 36, 54, 72 };
  static const int ALI_A[] = {  7, 11, 13, 17, 19, 23 };
  static const int ALI_B[] = {  9, 12, 15, 18, 21, 27 };

  float fastBias = std::clamp(activity * (0.65f + 0.35f*wind01), 0.0f, 1.0f);
  float weirdBias = std::clamp(0.10f + 0.45f*fauna01 + 0.25f*(0.5f-std::fabs(lifePhase01-0.5f))*2.0f, 0.0f, 1.0f);

  switch (world.biome) {
    case MEADOW: {
      // More diversity -> more sparkle / shorter notes.
      base = pickPool(MEA_A, (int)(sizeof(MEA_A)/sizeof(MEA_A[0])),
                      MEA_B, (int)(sizeof(MEA_B)/sizeof(MEA_B[0])),
                      std::clamp(0.20f + 0.50f*diversity01 + 0.20f*fastBias, 0.0f, 1.0f));
      break;
    }
    case WETLAND: {
      base = pickPool(WET_A, (int)(sizeof(WET_A)/sizeof(WET_A[0])),
                      WET_B, (int)(sizeof(WET_B)/sizeof(WET_B[0])),
                      std::clamp(0.25f + 0.35f*diversity01 + 0.20f*fastBias, 0.0f, 1.0f));
      break;
    }
    case ALPINE: {
      base = pickPool(ALP_A, (int)(sizeof(ALP_A)/sizeof(ALP_A[0])),
                      ALP_B, (int)(sizeof(ALP_B)/sizeof(ALP_B[0])),
                      std::clamp(0.10f + 0.25f*fastBias, 0.0f, 1.0f));
      break;
    }
    case TROPICAL: {
      base = pickPool(TRO_A, (int)(sizeof(TRO_A)/sizeof(TRO_A[0])),
                      TRO_B, (int)(sizeof(TRO_B)/sizeof(TRO_B[0])),
                      std::clamp(0.35f + 0.45f*diversity01 + 0.25f*fastBias, 0.0f, 1.0f));
      break;
    }
    case DESERT: {
      base = pickPool(DES_A, (int)(sizeof(DES_A)/sizeof(DES_A[0])),
                      DES_B, (int)(sizeof(DES_B)/sizeof(DES_B[0])),
                      std::clamp(0.20f + 0.25f*fastBias, 0.0f, 1.0f));
      break;
    }
    case ALIEN: default: {
      // Odd meters show up more when fauna is high or lifecycle is near mid-year.
      base = pickPool(ALI_B, (int)(sizeof(ALI_B)/sizeof(ALI_B[0])),
                      ALI_A, (int)(sizeof(ALI_A)/sizeof(ALI_A[0])),
                      weirdBias);
      break;
    }
  }

  // Determine pool size n by checking biome (since base points at one of the statics).
  // (This looks verbose but avoids dynamic allocations and keeps everything portable.)
  if (base==MEA_A) n=(int)(sizeof(MEA_A)/sizeof(MEA_A[0]));
  else if (base==MEA_B) n=(int)(sizeof(MEA_B)/sizeof(MEA_B[0]));
  else if (base==WET_A) n=(int)(sizeof(WET_A)/sizeof(WET_A[0]));
  else if (base==WET_B) n=(int)(sizeof(WET_B)/sizeof(WET_B[0]));
  else if (base==ALP_A) n=(int)(sizeof(ALP_A)/sizeof(ALP_A[0]));
  else if (base==ALP_B) n=(int)(sizeof(ALP_B)/sizeof(ALP_B[0]));
  else if (base==TRO_A) n=(int)(sizeof(TRO_A)/sizeof(TRO_A[0]));
  else if (base==TRO_B) n=(int)(sizeof(TRO_B)/sizeof(TRO_B[0]));
  else if (base==DES_A) n=(int)(sizeof(DES_A)/sizeof(DES_A[0]));
  else if (base==DES_B) n=(int)(sizeof(DES_B)/sizeof(DES_B[0]));
  else if (base==ALI_A) n=(int)(sizeof(ALI_A)/sizeof(ALI_A[0]));
  else n=(int)(sizeof(ALI_B)/sizeof(ALI_B[0]));

  // Phrase length: biome-dependent.
  int minL=4,maxL=8;
  switch(world.biome){
    case ALPINE: minL=3; maxL=6; break; // sparse, short phrases
    case DESERT: minL=3; maxL=5; break;
    case WETLAND: minL=5; maxL=8; break;
    case TROPICAL: minL=6; maxL=8; break;
    case ALIEN: minL=5; maxL=8; break;
    case MEADOW: default: minL=5; maxL=8; break;
  }
  phraseLen = std::clamp(minL + (int)std::lround((maxL-minL) * (0.35f + 0.55f*diversity01)) + r.irange(-1,1), minL, maxL);

  // Build the pattern with repetition + a few surprises.
  // Repetition amount depends on biome: meadow repeats more, alien repeats less.
  float repeatP = 0.55f;
  if (world.biome==MEADOW) repeatP = 0.70f;
  else if (world.biome==WETLAND) repeatP = 0.60f;
  else if (world.biome==ALPINE) repeatP = 0.50f;
  else if (world.biome==TROPICAL) repeatP = 0.45f;
  else if (world.biome==DESERT) repeatP = 0.50f;
  else if (world.biome==ALIEN) repeatP = 0.35f;

  for (int i=0;i<8;i++){
    int v = base[r.irange(0, n-1)];

    // Syncopation / odd accents.
    float sync = 0.08f + 0.22f*fauna01 + 0.18f*wind01 + 0.18f*(0.5f-std::fabs(lifePhase01-0.5f))*2.0f;
    if (world.biome==TROPICAL) sync += 0.12f;
    if (world.biome==WETLAND) sync += 0.06f;
    if (world.biome==ALPINE)  sync -= 0.04f;
    if (world.biome==DESERT)  sync -= 0.02f;
    if (world.biome==ALIEN)   sync += 0.16f;

    if (r.u01() < sync) v = base[r.irange(0, n-1)];

    // Occasional “rest cell” (extend gap) in calmer states or sparse biomes.
    float restP = 0.06f*(1.0f-activity);
    if (world.biome==ALPINE) restP += 0.10f;
    if (world.biome==DESERT) restP += 0.12f;
    if (world.biome==WETLAND) restP += 0.04f;
    if (r.u01() < restP) v += 12;

    // Encourage repetition: copy a previous cell.
    if (i>0 && r.u01() < repeatP) v = pat[i-1];

    pat[i] = std::clamp(v, 6, 96);
  }

  // Stronger downbeat at phrase start (except alien).
  if (world.biome != ALIEN) pat[0] = std::clamp(pat[0] + 6, 6, 96);
};

// Regenerate pattern on phrase boundaries or when sim phase shifts.
if (phrasePos==0 && (tick==0 || r.u01() < 0.30f*diversity01 + 0.08f)) regenPattern();

int interval = pat[phrasePos % 8];
phrasePos = (phrasePos + 1) % phraseLen;

// Small timing jitter: swing driven by fauna + wind.
int jitter = (int)std::lround((r.u01()*2.f-1.f) * (1.f + 5.f*fauna01 + 2.f*wind01));
interval = std::clamp(interval + jitter, 6, 48);
nextTick = tick + interval;

// (Re)compute coarse world metrics cheaply to give melody a real “state”.
// Note: we intentionally avoid full-grid scans here.
int sampleStep = 6;
int waterTiles = 0, plants = 0, predators = 0, whales = 0, total = 0;
for (int yy=0; yy<H; yy+=sampleStep) for (int xx=0; xx<W; xx+=sampleStep) {
  total++;
  if (world.water[yy][xx] > 0) waterTiles++;
  char t = world.terrain[yy][xx];
  if (t==','||t=='"'||t==';'||t=='m'||t=='f'||t=='+'||t=='&'||t=='$'||t=='#'||t=='T'||t=='Y'||t=='P'||t==KELP_GLYPH) plants++;
  char e = world.entities[yy][xx];
  if (e=='n' || e=='S' || e=='K' || e=='H') predators++;
  if (e=='W') whales++;
}
float w01 = total? (float)waterTiles/(float)total : 0.f;
float p01 = total? (float)plants/(float)total : 0.f;

// Target pitch: centered, not monotonic; modulated by parameters + weather.
// We also apply a biome “register” bias so each biome tends to live in a different tessitura.
float target = 60.0f
             + (w01 - 0.30f) * 9.0f
             + (p01 - 0.18f) * 5.0f
             + (wind01 - 0.20f) * 4.0f
             - (rain01) * 2.0f; // rain pulls slightly downward (more “hollow”)
if (world.weather.state == STORM) target += 2.5f;

// Biome register bias (subtle; the motif rhythm is the bigger identity).
int regBias = 0;
switch (world.biome) {
  case MEADOW:   regBias = +2; break;  // music box sits a bit higher
  case WETLAND:  regBias = -1; break;  // darker shimmer
  case ALPINE:   regBias = +9; break;  // airy bells
  case TROPICAL: regBias = +4; break;  // lively higher motion
  case DESERT:   regBias = -6; break;  // sparse low tones
  case ALIEN:    regBias = +0; break;  // uncanny center
}
target += (float)regBias;

// Smooth center with a rate that increases with activity (more restless).
float a = 0.010f + 0.030f*activity;
center = center*(1.0f-a) + target*a;

// Melodic contour: biome-specific leap grammar.
// fauna increases willingness to leap; flora smooths toward stepwise motion.
int leap = 0;
{
  // Base leap pools (semitones). These are “raw” before scale quantization.
  static const int MEA[] = { -3,-2,-1,0,1,2,3,5,7 };
  static const int WET[] = { -5,-3,-2,0,2,3,5,7 };
  static const int ALP[] = { -12,-7,-5,-3,0,3,5,7,12 };
  static const int TRO[] = { -7,-5,-3,-2,0,2,3,5,7,9,12 };
  static const int DES[] = { -5,-3,-2,0,2,3,5,7 };
  static const int ALI[] = { -11,-6,-1,0,1,6,11,13,-13 };

  const int* pool = MEA; int n = (int)(sizeof(MEA)/sizeof(MEA[0]));
  switch(world.biome){
    case WETLAND: pool=WET; n=(int)(sizeof(WET)/sizeof(WET[0])); break;
    case ALPINE:  pool=ALP; n=(int)(sizeof(ALP)/sizeof(ALP[0])); break;
    case TROPICAL:pool=TRO; n=(int)(sizeof(TRO)/sizeof(TRO[0])); break;
    case DESERT:  pool=DES; n=(int)(sizeof(DES)/sizeof(DES[0])); break;
    case ALIEN:   pool=ALI; n=(int)(sizeof(ALI)/sizeof(ALI[0])); break;
    case MEADOW: default: break;
  }

  // Stepwise bias: more flora -> pick closer-to-zero leaps.
  // More fauna -> allow far leaps more often.
  float farP = std::clamp(0.10f + 0.55f*fauna01 - 0.25f*flora01, 0.02f, 0.80f);
  if (world.biome==ALPINE) farP *= 0.75f; // alpine is sparse; let leaps be meaningful but not constant
  if (world.biome==TROPICAL) farP *= 1.15f;
  if (world.biome==ALIEN) farP *= 1.25f;

  int tries = 6;
  int chosen = 0;
  for(int k=0;k<tries;k++){
    int cand = pool[r.irange(0,n-1)];
    bool far = std::abs(cand) >= 7;
    if (far == (r.u01() < farP)) { chosen = cand; break; }
    chosen = cand;
  }
  leap = chosen;
}

// Pick primary note around center with controlled spread.
int spread = 3 + (int)std::lround(5.0f*activity);
switch (world.biome) {
  case MEADOW:   spread += 1; break;
  case WETLAND:  spread += 0; break;
  case ALPINE:   spread += 1; break;
  case TROPICAL: spread += 2; break;
  case DESERT:   spread -= 1; break;
  case ALIEN:    spread += 3; break;
}
spread = std::clamp(spread, 1, 12);

int raw1 = (int)std::lround(center) + r.irange(-spread, spread) + leap;

// Occasional octave sparkle: wind-chimes, biome-shaped.
float sparkleP = 0.04f + 0.12f*wind01 + 0.05f*diversity01;
switch (world.biome) {
  case MEADOW:   sparkleP += 0.02f; break;
  case WETLAND:  sparkleP -= 0.01f; break;
  case ALPINE:   sparkleP += 0.05f; break;
  case TROPICAL: sparkleP += 0.03f; break;
  case DESERT:   sparkleP -= 0.03f; break;
  case ALIEN:    sparkleP += 0.01f; break;
}
sparkleP = std::clamp(sparkleP, 0.0f, 0.30f);

if (r.u01() < sparkleP) raw1 += 12;
// Rare 2-octave glint in alpine storms (magical bell flare).
if (world.biome==ALPINE && world.weather.state==STORM && r.u01() < 0.03f) raw1 += 12;
// Alien can also dip downward into a “shadow octave”.
if (world.biome==ALIEN && r.u01() < 0.04f*(0.4f+fauna01)) raw1 -= 12;

// Harmony probability: biome-shaped.
float harmP = std::clamp(0.25f + 0.55f*flora01 - 0.25f*(predators>0?1.0f:0.0f), 0.02f, 0.90f);
switch (world.biome) {
  case MEADOW:   harmP += 0.10f; break;
  case WETLAND:  harmP += 0.06f; break;
  case ALPINE:   harmP -= 0.12f; break;
  case TROPICAL: harmP += 0.04f; break;
  case DESERT:   harmP -= 0.18f; break;
  case ALIEN:    harmP += (r.u01()<0.5f ? -0.08f : 0.08f); break;
}
harmP = std::clamp(harmP, 0.02f, 0.90f);

int raw2 = raw1 + (r.oneIn(2) ? 7 : 4) + r.irange(-1, 1);
if (predators > 0 && r.oneIn(2)) raw2 -= 5;

int n1 = quantizeNoteToScale(raw1, rootKeyUsed, scaleUsed);
int n2 = quantizeNoteToScale(raw2, rootKeyUsed, scaleUsed);

// Hold time (note duration) varies with rhythm; longer in calm.
int holdBase = std::clamp((int)std::lround(interval * (activity<0.4f ? 1.8f : 1.2f)), 10, 80);
int hold1 = holdBase + r.irange(-4, 10);
int hold2 = holdBase + r.irange(-6, 8);

// Prevent immediate same-note retrigger clicks: if same as last and still very recent, nudge.
if (lastHeld1 == n1 && (tick - (holdUntil1-hold1)) < 12) n1 = quantizeNoteToScale(n1 + (r.oneIn(2)?2:-2), rootKeyUsed, scaleUsed);

heldNote  = std::clamp(n1, 36, 96);
lastHeld1 = heldNote;
holdUntil1 = tick + hold1;

// Velocity: delicate; wind adds sparkle; rain softens; storms push intensity.
int v1 = 38
       + (int)std::lround(22.0f*flora01)
       + (int)std::lround(18.0f*wind01)
       - (int)std::lround(10.0f*rain01);
if (world.weather.state == STORM) v1 += 14;
v1 = std::clamp(v1, 25, 92);

synth.noteOn(0, heldNote, v1);

// Optional harmony note
if (r.u01() < harmP) {
  heldNote2 = std::clamp(n2, 36, 96);
  lastHeld2 = heldNote2;
  holdUntil2 = tick + hold2;
  int v2 = std::clamp(v1 - (10 + (int)std::lround(8.0f*activity)), 18, 78);
  synth.noteOn(0, heldNote2, v2);
}

// Percussion accents: now less repetitive; driven by rain/wind + a little randomness.
// Keep it magical and sparse.
if (world.weather.state == STORM && r.u01() < (0.08f + 0.12f*wind01)) synth.noteOn(9, 80, 70); // mute triangle-ish
if (world.weather.state == RAIN  && r.u01() < (0.05f + 0.08f*rain01)) synth.noteOn(9, 81, 55); // open triangle-ish

}


static inline std::string defaultSf2Path() {
#ifdef _WIN32
  return "";
#else
  // Try a few common distro paths.
  const char* candidates[] = {
    "/usr/share/sounds/sf2/FluidR3_GM.sf2",
    "/usr/share/soundfonts/FluidR3_GM.sf2",
    "/usr/share/sounds/sf2/TimGM6mb.sf2",
    "/usr/share/sounds/sf2/GeneralUser_GS.sf2"
  };
  for (auto* p : candidates) {
    FILE* f = std::fopen(p, "rb");
    if (f) { std::fclose(f); return std::string(p); }
  }
  return std::string();
#endif
}



static void drawString(SDL_Renderer* ren, GlyphCache& gc, int x, int y, const std::string& s,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale);
// ===== end MIDI + menu plumbing =====

static void render(SDL_Renderer* ren, const Layout& L, World& w, GlyphCache& gcWorld, GlyphCache& gcText, int tick, bool showMenu, int menuPage, const std::vector<MidiParam>& params, int menuSel, bool midiEnabled, bool midiClockOut, bool useSimClock, int rootKey, ScaleType scaleType, bool synthEnabled, const std::string& sf2Path, UiLang uiLang) {
  Season s = seasonAt(tick);
  float sp = seasonLerp(tick);
        sp *= w.cloudOpacity;

  setColor(ren, 0,0,0);
  SDL_RenderClear(ren);

  for (int y=0; y<H; ++y) {
    int y0 = (y * L.simHpx) / H;
    int y1 = ((y+1) * L.simHpx) / H;
    int hpx = std::max(1, y1 - y0);

    for (int x=0; x<W; ++x) {
      int x0 = (x * L.screenW) / W;
      int x1 = ((x+1) * L.screenW) / W;
      int wpx = std::max(1, x1 - x0);

      SDL_Rect rc{ x0, y0, wpx, hpx };

      RGB bg = baseBgFor(w, x, y, tick, s, sp);
      uint8_t cloud = sampleCloud(w.clouds, x, y);
      // Biome-tune clouds: tropical clouds should be lighter/smaller coverage.
      cloud = (uint8_t)std::min<int>(255, (int)(cloud * w.cloudOpacity));
      if (w.biome==TROPICAL) cloud = (uint8_t)std::max<int>(0, (int)cloud - 35);
      if (w.biome==DESERT)   cloud = (uint8_t)std::max<int>(0, (int)cloud - 25);

      applyCloudShadow(bg, cloud);

      setColor(ren, bg.r, bg.g, bg.b);
      SDL_RenderFillRect(ren, &rc);

      char c = renderCharAt(w, x, y, tick);

      if (c=='.' && w.water[y][x]==0 && w.entities[y][x]==' ' && w.overlay[y][x]==' ') {
        applyCloudLayer(ren, rc, cloud);
        continue;
      }

      if (w.entities[y][x]==' ' && w.overlay[y][x]==' ' && w.water[y][x]==0) {
        uint32_t h = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(tick/6));
        c = terrainGlyphVariant(c, h, s, w.weather);
      }

      SDL_Texture* gt = gcWorld.get(ren, (unsigned char)c);
      if (gt) {
        RGB fg = fgForChar(w, c, s, sp, tick, x, y);

        if ((w.terrain[y][x]==',' || w.terrain[y][x]=='"') && w.wind.strength>0) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(tick/3));
          if (h & 1u) {
            fg.g = clampU8((int)fg.g + 20);
            fg.r = clampU8((int)fg.r + 5);
          }
        }

        if ((c=='/'||c=='\\'||c=='|') && (w.weather.state==STORM)) {
          fg.r = clampU8(fg.r + 30);
          fg.g = clampU8(fg.g + 30);
          fg.b = clampU8(fg.b + 30);
        }

        SDL_SetTextureColorMod(gt, fg.r, fg.g, fg.b);
        SDL_RenderCopy(ren, gt, nullptr, &rc);
      }

      applyCloudLayer(ren, rc, cloud);
    }
  }

  SDL_Rect hud{0, L.simHpx, L.screenW, L.hudH};
  setColor(ren, 8,8,10);
  SDL_RenderFillRect(ren, &hud);

  
// ----- Menu overlay (semi-transparent) -----
if (showMenu) {
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  SDL_Rect panel{ 8, 8, L.screenW - 16, 8*L.scale*11 };
  SDL_SetRenderDrawColor(ren, 0, 0, 0, 170);
  SDL_RenderFillRect(ren, &panel);

  int tx = 12;
  int ty = 12;
  auto seasonName = [&](Season s)->const char*{
    switch(s){ case SPRING: return "SPRING"; case SUMMER: return "SUMMER"; case AUTUMN: return "AUTUMN"; case WINTER: return "WINTER"; }
    return "?";
  };
  auto biomeName = [&](Biome b)->const char*{
    switch(b){ case MEADOW: return "MEADOW"; case WETLAND: return "WETLAND"; case DESERT: return "DESERT"; case TROPICAL: return "TROPICAL"; case ALPINE: return "ALPINE"; case ALIEN: return "ALIEN"; }
    return "?";
  };
  auto pageName = [&](int p)->const char*{
    static const char* EN0="MIDI";
    static const char* EN1="WATER";
    static const char* EN2="SPAWNS";
    static const char* EN3="AUDIO";
    static const char KATA_MIZU[]   = { (char)0x80, (char)0x81, 0 };                 // ミズ
    static const char KATA_SPAWN[]  = { (char)0x84, (char)0x85, (char)0x87, (char)0x86, 0 }; // スポーン
    static const char KATA_OTO[]    = { (char)0x82, (char)0x83, 0 };                 // オト
    switch (p & 3) {
      case 0: return EN0; // keep MIDI in Latin
      case 1: return (uiLang==UI_KATA) ? KATA_MIZU  : EN1;
      case 2: return (uiLang==UI_KATA) ? KATA_SPAWN : EN2;
      case 3: return (uiLang==UI_KATA) ? KATA_OTO   : EN3;
    }
    return "?";
  };

  // quick stats
  int waterTiles=0, shallow=0, deep=0;
  for (int yy=0; yy<H; ++yy) for (int xx=0; xx<W; ++xx) {
    uint8_t d = w.water[yy][xx];
    if (d>0) { waterTiles++; if (d<=2) shallow++; if (d>=5) deep++; }
  }

  char buf[512];
  snprintf(buf, sizeof(buf),
           "[TAB] Page:%s   Biome:%s   Season:%s   Wind:(%d,%d) s=%d   Weather:%d   Water:%d%%   Synth:%s",
           pageName(menuPage), biomeName(w.biome), seasonName(s),
           w.wind.dx, w.wind.dy, w.wind.strength, (int)w.weather.state,
           (int)(100.0f * (float)waterTiles / (float)(W*H)),
           synthEnabled ? "ON" : "OFF");
  drawString(ren, gcText, tx, ty, buf, 240,240,240, 230, L.scale);
  ty += 10*L.scale;

  if ((menuPage&3)==0) {
    drawString(ren, gcText, tx, ty, "MIDI/CC (UP/DOWN select, +/- weight, O MIDI, C clock, V clock src, K key, S scale, M menu)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;

    for (int i=0; i<(int)params.size(); ++i) {
      const auto& p = params[i];
      int rr = (i==menuSel)?255:200;
      int gg = (i==menuSel)?240:200;
      int bb = (i==menuSel)?210:200;
      snprintf(buf, sizeof(buf), "%s  CC:%d  w:%.2f  v:%.2f", p.name, p.cc, p.weight, p.value01);
      drawString(ren, gcText, tx, ty + i*9*L.scale, buf, (uint8_t)rr,(uint8_t)gg,(uint8_t)bb, 230, L.scale);
      int bx = tx + 240*L.scale;
      int by = ty + i*9*L.scale;
      int barW = 200*L.scale;
      int barH = 6*L.scale;
      SDL_Rect bar{ bx, by + 2*L.scale, barW, barH };
      SDL_SetRenderDrawColor(ren, 60,60,60, 200);
      SDL_RenderFillRect(ren, &bar);
      SDL_Rect fill = bar;
      fill.w = (int)(barW * clamp01(p.value01));
      SDL_SetRenderDrawColor(ren, rr,gg,bb, 200);
      SDL_RenderFillRect(ren, &fill);
    }
  } else if ((menuPage&3)==1) {
    snprintf(buf, sizeof(buf), "Water tiles: %d  shallow(<=2): %d  deep(>=5): %d", waterTiles, shallow, deep);
    drawString(ren, gcText, tx, ty, buf, 200,220,240, 230, L.scale);
    ty += 10*L.scale;
    drawString(ren, gcText, tx, ty, "Tip: wind now drives lily pad drift + increases foam on shores/crests.", 180,190,200, 220, L.scale);
  } else if ((menuPage&3)==2) {
    // very lightweight counts
    int whales=0, dinos=0, yeti=0, fish=0, cranes=0;
    for (int yy=0; yy<H; ++yy) for (int xx=0; xx<W; ++xx) {
      char e = w.entities[yy][xx];
      if (e=='W') whales++;
      if (e=='K') dinos++;
      if (e=='H') yeti++;
      if (e=='>'||e=='<') fish++;
      if (e=='C') cranes++;
    }
    snprintf(buf, sizeof(buf), "Whales(W):%d  Fish(</>):%d  Cranes(C):%d  Dinos(K):%d  Yetis(H):%d", whales, fish, cranes, dinos, yeti);
    drawString(ren, gcText, tx, ty, buf, 220,220,220, 230, L.scale);
    ty += 10*L.scale;
    drawString(ren, gcText, tx, ty, "Legendary couple: glyphs 0x19 and 0x1A (ultra rare, never despawn).", 180,180,180, 220, L.scale);
  } else {
    snprintf(buf, sizeof(buf), "SoundFont: %s", sf2Path.empty() ? "(none)" : sf2Path.c_str());
    drawString(ren, gcText, tx, ty, buf, 220,220,220, 230, L.scale);
    ty += 10*L.scale;
    drawString(ren, gcText, tx, ty, "Use: --synth --sf2 <path.sf2> --gain <0..2>  (compile with -DUSE_FLUIDSYNTH)", 180,180,180, 220, L.scale);
  }
}
SDL_RenderPresent(ren);
}

// ---------------- CLI ----------------
static Biome parseBiome(int argc, char** argv) {
  for (int i=1; i<argc; ++i) {
    if (std::strcmp(argv[i], "--biome")==0 && i+1<argc) {
      std::string s = argv[i+1];
      if (s=="meadow") return MEADOW;
      if (s=="wetland") return WETLAND;
      if (s=="alpine") return ALPINE;
      if (s=="alien") return ALIEN;
      if (s=="tropical") return TROPICAL;
    }
  }
  return MEADOW;
}

static const char* weatherName(WeatherState st) {
  switch (st) {
    case CLEAR: return "clear";
    case OVERCAST: return "overcast";
    case RAIN: return "rain";
    case STORM: return "storm";
  }
  return "clear";
}
static const char* seasonName(Season s) {
  switch (s) {
    case SPRING: return "spring";
    case SUMMER: return "summer";
    case AUTUMN: return "autumn";
    case WINTER: return "winter";
  }
  return "spring";
}

int main(int argc, char** argv) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  bool startFullscreen = true;
  UiLang uiLang = UI_EN;

// Menu pages (TAB cycles when menu open)
int menuPage = 0;

// Built-in synth (FluidSynth + SoundFont) – optional.
SynthOut synth;
std::string sf2Path = defaultSf2Path();
bool wantSynth = false;
float synthGain = 0.7f;
std::string synthAudioDriver = ""; // e.g. pipewire/pulseaudio/alsa/jack
std::string synthAudioDevice = ""; // optional backend-specific device

// Parse CLI args
for (int i=1; i<argc; ++i) {
  if (std::strcmp(argv[i], "--windowed")==0) { startFullscreen = false; }
  else if (std::strcmp(argv[i], "--fullscreen")==0) { startFullscreen = true; }
  else if (std::strcmp(argv[i], "--synth")==0) { wantSynth = true; }
  else if (std::strcmp(argv[i], "--sf2")==0 && i+1<argc) { sf2Path = argv[++i]; wantSynth = true; }
  else if (std::strcmp(argv[i], "--gain")==0 && i+1<argc) { synthGain = (float)std::atof(argv[++i]); wantSynth = true; }
  else if (std::strcmp(argv[i], "--audio-driver")==0 && i+1<argc) { synthAudioDriver = argv[++i]; wantSynth = true; }
  else if (std::strcmp(argv[i], "--audio-device")==0 && i+1<argc) { synthAudioDevice = argv[++i]; wantSynth = true; }
  else if (std::strcmp(argv[i], "--lang")==0 && i+1<argc) { std::string s=argv[++i]; if (s=="kata"||s=="katakana"||s=="ja"||s=="jp") uiLang=UI_KATA; else uiLang=UI_EN; }
}

Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
  if (startFullscreen) wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

  SDL_Window* win = SDL_CreateWindow(
    "Terrarium 0.42 (fixed4)",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    1280, 720,
    wflags
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

  Biome biome = parseBiome(argc, argv);

  World world;
  seedWorld(world, r, biome);

  GlyphCache gcWorld;
  GlyphCache gcText; gcText.textMode = true;
  Layout layout = computeLayout(ren);

  if (wantSynth) {
    if (sf2Path.empty()) {
      std::cerr << "[synth] No SoundFont found. Provide --sf2 path/to/file.sf2\n";
    } else {
      if (!synth.open(sf2Path, std::clamp(synthGain, 0.0f, 2.0f), synthAudioDriver, synthAudioDevice)) {
        std::cerr << "[synth] Failed to start synth (check --sf2 path and audio driver). Try: --audio-driver pipewire (or pulseaudio/alsa).\n";
      }
    }
  }

  bool running=true, paused=false;
  int tps=DEFAULT_TPS;
  int tick=0;
  std::string banner="calm";

// ----- 0.48w UI + MIDI mapping -----
bool showMenu = true;          // toggle with M
int menuSel = 0;               // selected parameter
bool midiClockOut = false;     // toggle with C
bool useSimClock = true;       // V toggles (external clock-in stub for now)
int rootKey = 0;               // 0=C, 1=C#, ...
ScaleType scaleType = SCALE_PENTATONIC;
uint32_t lastClockMs = 0;
uint32_t lastParamSendMs = 0;
const uint32_t PARAM_SEND_INTERVAL_MS = 50; // 20 Hz

int heldNote = -1;
int heldNote2 = -1;
MidiOut midi;
midi.open(0);
midi.enabled = false; // toggle with O

// CC assignments (feel free to change):
// 20 water, 21 rain, 22 wind, 23 season, 24 biome, 25 flora density, 26 fauna density
std::vector<MidiParam> params = {
  {"Water", 20, 1.0f, 0.0f, -1.0f},
  {"Rain",  21, 1.0f, 0.0f, -1.0f},
  {"Wind",  22, 1.0f, 0.0f, -1.0f},
  {"Season",23, 0.6f, 0.0f, -1.0f},
  {"Biome", 24, 0.6f, 0.0f, -1.0f},
  {"Flora", 25, 1.0f, 0.0f, -1.0f},
  {"Fauna", 26, 1.0f, 0.0f, -1.0f},
  {"Instr", 27, 1.0f, 0.0f, -1.0f}, // 0..1 selects from a bell/chime palette
  {"AutoKey", 28, 1.0f, 1.0f, -1.0f}, // 0..1 blend sim-derived key (root)
  {"AutoScale",29, 1.0f, 1.0f, -1.0f}, // 0..1 blend sim-derived scale/mode
};

// A tiny note scheduler (for rare "big life" events etc.)
std::vector<MidiEvent> noteQueue;

  auto last = std::chrono::steady_clock::now();

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running=false;
      if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.sym) {
          case SDLK_ESCAPE: running=false; break;
case SDLK_TAB: if (showMenu) menuPage = (menuPage + 1) & 3; break;
          case SDLK_F2: uiLang = (uiLang==UI_EN) ? UI_KATA : UI_EN; break;
          case SDLK_b: {
            // Cycle biomes with a short fade-out/fade-in.
            world.targetBiome = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
            world.biomeFadeDir = +1;
          } break;
          case SDLK_SPACE: paused=!paused; break;
          case SDLK_PERIOD:
            if (paused) { 
              step(world, r, banner, tick); 
              tick++; 
              // Music follows sim ticks (also in realtime loop below)
              synthTickMusic(synth, world, r, tick, heldNote, heldNote2, rootKey, scaleType, params);
            }
            break;
          case SDLK_LEFTBRACKET: if (tps>1) tps--; break;
          case SDLK_RIGHTBRACKET: if (tps<30) tps++; break;
          case SDLK_r:
            seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
            r = Rng(seed);
            seedWorld(world, r, biome);
            tick=0; banner="reset";
            break;
          case SDLK_F11: {
            Uint32 flags = SDL_GetWindowFlags(win);
            bool fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            layout = computeLayout(ren);
          } break;
          
case SDLK_m: showMenu = !showMenu; break;
case SDLK_o:
  midi.enabled = !midi.enabled;
  if (midi.enabled) midi.sendStart(); else midi.sendStop();
  break;
case SDLK_c: midiClockOut = !midiClockOut; break;
case SDLK_v: useSimClock = !useSimClock; break;
case SDLK_UP:    menuSel = (menuSel + (int)params.size() - 1) % (int)params.size(); break;
case SDLK_DOWN:  menuSel = (menuSel + 1) % (int)params.size(); break;
case SDLK_MINUS:
case SDLK_KP_MINUS:
  params[menuSel].weight = std::max(0.f, params[menuSel].weight - 0.05f);
  break;
case SDLK_EQUALS:
case SDLK_KP_PLUS:
  params[menuSel].weight = std::min(2.f, params[menuSel].weight + 0.05f);
  break;
case SDLK_k: rootKey = (rootKey + 1) % 12; break;
case SDLK_s: scaleType = (ScaleType)(((int)scaleType + 1) % 5); break;
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

    // Biome transitions: animate fade, then actually reseed the world into the new biome.
    // (Without this, hitting 'B' would only change the label/palette, not the terrain rules.)
    if (world.biomeFadeDir != 0) {
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

    if (!paused && dtMs >= msPerTick) {
      last = now;
      step(world, r, banner, tick);
      tick++;
      synthTickMusic(synth, world, r, tick, heldNote, heldNote2, rootKey, scaleType, params);
    }

    Season s = seasonAt(tick);
    std::string title =
      std::string("Terrarium 0.42 (fixed4) | biome ") + biomeName(biome) +
      " | " + std::to_string(W) + "x" + std::to_string(H) +
      " | tick " + std::to_string(tick) +
      " | " + (paused ? "PAUSED" : ("tps " + std::to_string(tps))) +
      " | " + seasonName(s) +
      " | weather " + weatherName(world.weather.state) +
      " (" + std::to_string((int)(world.weather.rainStrength*100)) + "%)" +
      " | wind " + std::to_string(world.wind.strength) +
      " | " + banner +
      " | SPACE pause  . step  [ ] speed  r reset  F11 fullscreen  ESC quit";
    SDL_SetWindowTitle(win, title.c_str());

    
// ----- Compute menu metrics + MIDI params -----
// Values are normalized 0..1 and then scaled by each parameter's weight.
// Keeping this cheap: we sample a coarse grid for density rather than full scan.
float windMag = std::min(1.f, (std::abs((float)world.wind.dx) + std::abs((float)world.wind.dy)) / 6.f);
float rain01  = clamp01(world.weather.rainStrength);
// average water sample
int sampleStep = 4;
int waterSum = 0, waterN = 0;
int floraCount = 0, faunaCount = 0, cellN = 0;
for(int y=0; y<H; y+=sampleStep){
  for(int x=0; x<W; x+=sampleStep){
    waterSum += (int)world.water[y][x];
    waterN++;
    char t = world.terrain[y][x];
    char e = world.entities[y][x];
    if(t!='.' && t!='^' && t!=' ' ) floraCount++;
    if(e!=' ') faunaCount++;
    cellN++;
  }
}
float water01 = clamp01((waterN? (float)waterSum/(float)waterN : 0.f) / 8.f);
float flora01 = clamp01(cellN? (float)floraCount/(float)cellN : 0.f);
float fauna01 = clamp01(cellN? (float)faunaCount/(float)cellN : 0.f);
Season sNow = seasonAt(tick);
float season01 = (float)((int)sNow) / 3.f;
float biome01  = (float)((int)world.biome)  / 5.f;

for(auto &p : params){
  if      (std::string(p.name)=="Water")  p.value01 = water01;
  else if (std::string(p.name)=="Rain")   p.value01 = rain01;
  else if (std::string(p.name)=="Wind")   p.value01 = windMag;
  else if (std::string(p.name)=="Season") p.value01 = season01;
  else if (std::string(p.name)=="Biome")  p.value01 = biome01;
  else if (std::string(p.name)=="Flora")  p.value01 = flora01;
  else if (std::string(p.name)=="Fauna")  p.value01 = fauna01;
  p.value01 = clamp01(p.value01 * p.weight);
}

// Send MIDI CCs at a modest rate.
uint32_t nowMs = SDL_GetTicks();
if (midi.enabled && (nowMs - lastParamSendMs) >= PARAM_SEND_INTERVAL_MS) {
  lastParamSendMs = nowMs;
  for(auto &p : params){
    if(p.cc<0) continue;
    if(p.lastSent01 < 0.f || std::abs(p.lastSent01 - p.value01) > 0.01f){
      int v = (int)std::lround(p.value01 * 127.f);
      midi.sendCC(0, p.cc, v);
      p.lastSent01 = p.value01;
    }
  }
}

// Optional MIDI clock out (24 ppqn). We derive tempo from sim speed.
if (midi.enabled && midiClockOut && useSimClock) {
  // Rough tempo mapping: more wind/rain => faster.
  float bpm = 90.f + 60.f * (0.5f*windMag + 0.5f*rain01);
  float msPerClock = (60000.f / bpm) / 24.f;
  if (lastClockMs==0) lastClockMs = nowMs;
  while ((nowMs - lastClockMs) >= (uint32_t)msPerClock) {
    midi.sendClock();
    lastClockMs += (uint32_t)msPerClock;
  }
}
render(ren, layout, world, gcWorld, gcText, tick, showMenu, menuPage, params, menuSel, midi.enabled, midiClockOut, useSimClock, rootKey, scaleType, synth.enabled, sf2Path, uiLang);
    SDL_Delay(6);
  }

  gcWorld.destroy();
  gcText.destroy();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
    synth.close();
SDL_Quit();
  return 0;
}

void drawString(SDL_Renderer* ren, GlyphCache& gc, int px, int py, const std::string& s, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale) {
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  int x = px;
  for(char c: s){
    if(c=='\n'){ py += 8*scale; x = px; continue; }
    SDL_Texture* tex = gc.get(ren, (unsigned char)c);
    if(!tex){ x += 8*scale; continue; }
    SDL_SetTextureColorMod(tex, r, g, b);
    SDL_SetTextureAlphaMod(tex, a);
    SDL_Rect dst{ x, py, 8*scale, 8*scale };
    SDL_RenderCopy(ren, tex, nullptr, &dst);
    x += 8*scale;
  }
}

