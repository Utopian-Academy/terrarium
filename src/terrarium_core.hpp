#pragma once

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

// NOTE: deliberately no SDL include here — the sim core is platform-free so
// it can be reused by the plugin build (see plugin/).
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX  // keep std::min/std::max usable
  #endif
  #include <windows.h>
  #include <mmsystem.h>
  #pragma comment(lib, "winmm.lib")
  // windef.h defines legacy near/far as empty macros; the sim uses them as
  // ordinary identifiers.
  #undef near
  #undef far
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

// --- Audio voice count (melodic channels 0..NUM_VOICES-1) ---
static constexpr int NUM_VOICES = 3;

// clamp to [-1, +1]
static inline float clamp11f(float v){ return v<-1.f?-1.f:(v>1.f?1.f:v); }


// World size in cells. Overridable per-target (the pico build uses 200x200).
#ifndef TERRA_W
#define TERRA_W 200
#endif
#ifndef TERRA_H
#define TERRA_H 112
#endif
static constexpr int W = TERRA_W;
static constexpr int H = TERRA_H;

static constexpr int CW = 96;
static constexpr int CH = 54;

static constexpr int DEFAULT_TPS = 6;

static constexpr int SEASON_TICKS = 900; // faster season cycle so it's visible (~30-90s depending on TPS)
static constexpr int DAY_TICKS    = 900;

static constexpr int WIND_CHANGE_TICKS = 220;
static constexpr int MAX_WIND = 5;

enum WeatherState { CLEAR=0, OVERCAST=1, RAIN=2, STORM=3 };

static constexpr float VIVID_SAT = 1.32f;  // rich, life-evoking (bg is black)
static constexpr float VIVID_VAL = 1.10f;  // a touch brighter

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

// Fast xorshift64* RNG. The sim calls this millions of times per second in
// the water/terrain passes; std::mt19937 + per-call distribution objects were
// the single largest cost per tick (and far too slow for a Pi Zero build).
struct Rng {
  uint64_t state;
  Rng(uint32_t seed=0xC0FFEEu) {
    state = (uint64_t)seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
    if (state == 0) state = 0xC0FFEEull;
    u32(); u32();  // scramble away from the seed
  }
  uint32_t u32() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return (uint32_t)((state * 0x2545F4914F6CDD1Dull) >> 32);
  }
  // Inclusive [a,b] like the old uniform_int_distribution behavior.
  int irange(int a,int b){
    if (b < a) std::swap(a, b);
    uint32_t span = (uint32_t)(b - a) + 1u;
    return a + (int)(((uint64_t)u32() * (uint64_t)span) >> 32);
  }
  float u01(){ return (float)(u32() >> 8) * (1.0f / 16777216.0f); }
  bool oneIn(int n){ return n <= 1 ? true : irange(1, n) == 1; }
  int i(int lo, int hi) { return irange(lo, hi); }
};

enum Mood : uint8_t { MOOD_CALM=0, MOOD_CURIOUS, MOOD_HUNGRY, MOOD_THIRSTY, MOOD_FEARFUL, MOOD_ENRAGED, MOOD_EUPHORIC };
enum Intent : uint8_t { INTENT_WANDER=0, INTENT_FORAGE, INTENT_DRINK, INTENT_FLEE, INTENT_HUNT, INTENT_REST };

struct Agent {
  int id=0;          // stable-ish id (index at spawn)
  int x=0, y=0;
  char glyph='?';

  uint8_t species=0; // abstract species id

  // Vitals (0..1 unless noted)
  float hunger=0.0f;   // 0..1 (1 = starving)
  float thirst=0.0f;   // 0..1 (1 = parched)
  float fatigue=0.0f;  // 0..1 (1 = exhausted)
  float stress=0.0f;   // 0..1
  float health=1.0f;   // 0..1

  Mood mood = MOOD_CALM;
  Intent intent = INTENT_WANDER;

  uint8_t flags=0;     // bit0:panic
};
// Agent flags
static constexpr uint8_t AGF_PANIC   = 1u<<0;
static constexpr uint8_t AGF_LEGEND_A= 1u<<1;
static constexpr uint8_t AGF_LEGEND_B= 1u<<2;

static inline bool isLegendary(const Agent& a){ return (a.flags & (AGF_LEGEND_A|AGF_LEGEND_B))!=0; }


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
static inline int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }

inline int countNeighborsChar(const Grid& g, int x, int y, char c) {
  int n = 0;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (dx==0 && dy==0) continue;
    int nx=x+dx, ny=y+dy;
    if (inBounds(nx,ny) && g[ny][nx]==c) n++;
  }
  return n;
}

enum Season { SPRING=0, SUMMER=1, AUTUMN=2, WINTER=3 };

inline constexpr int BIOME_COUNT = 6;

enum Biome { MEADOW=0, WETLAND=1, ALPINE=2, ALIEN=3, TROPICAL=4, DESERT=5 };

enum Species : uint8_t {
  SPEC_WANDERER=0,
  SPEC_SHELLBACK=1,
  SPEC_SWARMER=2,
  SPEC_ENGINEER=3,
  SPEC_PARASITE=4,
  SPEC_PACKHUNTER=5,
  SPEC_MYSTIC=6,
  SPEC_TRICKSTER=7,
  SPEC_COUNT=8
};

struct BiomeWeights {
  float pondDensity = 0.0f;
  float stoneChance = 0.0f;
  float reedChance = 0.0f;
  float fernChance = 0.0f;
  float flowerChance = 0.0f;
  float bigFlowerChance = 0.0f;
  float treeChance = 0.0f;
  float mushChance = 0.0f;
  float growRate = 0.0f;
  float bloomRate = 0.0f;
  float fireRate = 0.0f;
  float alienRate = 0.0f;
};

struct Wind {
  int dx = 0;
  int dy = 0;
  int strength = 0;
};

struct Weather {
  WeatherState state = CLEAR;
  float rainStrength = 0.0f;
  int timer = 0;
  bool lastTickWasRaining = false;
};

struct Clouds {
  std::vector<uint8_t> field;
  float offX = 0.0f;
  float offY = 0.0f;

  Clouds() : field(CW * CH, 0) {}
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
  Biome biome = MEADOW;
  Biome targetBiome = MEADOW;
  float biomeFade = 0.0f;
  int biomeFadeDir = 0;
  bool biomeMorphActive = false;
  float biomeMorphT = 0.0f;
  BiomeWeights bwFrom{};
  BiomeWeights bwTo{};
  BiomeWeights bw{};
  std::vector<std::vector<uint8_t>> moist;
  std::vector<std::pair<int,int>> springs;
  std::vector<Agent> agents;
};

inline constexpr char FOAM_GLYPH = '=';
inline constexpr char LILYPAD_GLYPH = 'l';
inline constexpr char KELP_GLYPH = 'u';

// ===== Shared state for new features (zoom/pan, ripples, alea weights) =====
struct Ripple {
  int cx=0, cy=0;
  float t=0.f;
  float amp=3.f;      // displacement amplitude in cells
  float speed=18.f;   // ring speed (cells/sec)
  float width=2.5f;   // ring thickness (cells)
  float chaos=1.f;    // chaos injection multiplier
};
struct AleaWeights {
  float rainChance=1.f;
  float spawnChance=1.f;
  float mutationRate=1.f;
  float drift=1.f;
  float chaos=1.f;
  // music note-off / duration control
  float noteLen=1.f;        // duration multiplier (0.25..2.0 typical)
  float holdChance=0.06f;   // chance to stretch a note longer
};

struct StepEvent {
  int x=0, y=0;
  int dx=0, dy=0;  // movement delta
  char glyph='?';
  float strength=1.f; // 1..2 (for shove etc)
};
inline std::vector<StepEvent> g_stepEvents;

inline int g_zoom = 1;
inline int g_inspectIdx = -1;       // selected agent index
inline bool g_followInspect = false; // camera follows selected agent when true

inline int g_camX = 0;
inline int g_camY = 0;
inline std::vector<Ripple> g_ripples;
inline AleaWeights g_alea;

static constexpr int MOD_N = 50;
inline const char* g_modName[MOD_N] = {
  "water_view", "plants_view", "overlay_view", "agents_view", "agent_speed",
  "stress_mean", "stress_hi", "panic_count", "hunger_mean", "thirst_mean",
  "fatigue_mean", "health_mean", "pred_pressure", "birth_pulse", "death_pulse",
  "ripple_energy", "wind_mag", "season_pos", "cloud_opacity", "raininess",
  "shellback_stress", "swarm_cohesion", "parasite_aura", "engineer_work", "mystic_flux",
  "trickster_mischief", "pack_density", "plant_flux", "water_flux", "stress_flux",
  "hunger_flux", "thirst_flux", "fatigue_flux", "health_flux", "panic_flux",
  "oddity_0", "oddity_1", "oddity_2", "oddity_3", "oddity_4",
  "oddity_5", "oddity_6", "oddity_7", "oddity_8", "oddity_9",
  "oddity_10", "oddity_11", "oddity_12", "oddity_13", "oddity_14"
};
inline float g_modVal[MOD_N] = {0};

// --- Mod Matrix ---
enum ModDest : int {
  DEST_NONE=0,
  DEST_CC11_EXPR=1,
  DEST_CC74_BRIGHT=2,
  DEST_PAN=3,
  DEST_PORTA_V0=4,
  DEST_PORTA_V1=5,
  DEST_PORTA_V2=6,
  DEST_MIDI_CC=7,   // arbitrary CC on the external MIDI port (slot's cc field)
  DEST_COUNT=8
};

static inline const char* modDestName(int d){
  switch(d){
    case DEST_CC11_EXPR: return "CC11 Expr";
    case DEST_CC74_BRIGHT: return "CC74 Bright";
    case DEST_PAN: return "CC10 Pan";
    case DEST_PORTA_V0: return "Porta V0";
    case DEST_PORTA_V1: return "Porta V1";
    case DEST_PORTA_V2: return "Porta V2";
    case DEST_MIDI_CC: return "MIDI CC";
    default: return "None";
  }
}

struct ModMap {
  int src=0;
  int dest=DEST_NONE;
  float amt=0.0f;     // -2..2
  float smooth=0.20f; // 0..0.98
  float state=0.0f;
  bool enabled=false;
  int cc=1;           // DEST_MIDI_CC only: which controller to drive (0..127)
};
static constexpr int MOD_SLOTS=12;
inline ModMap g_modMap[MOD_SLOTS];

// True when any enabled mod-matrix slot targets `dest`. Used to give the
// matrix full authority over a destination: the built-in animated automation
// must yield instead of overwriting the matrix's CC values.
inline bool modMapControls(int dest) {
  for (int i = 0; i < MOD_SLOTS; ++i) {
    if (g_modMap[i].enabled && g_modMap[i].dest == dest) return true;
  }
  return false;
}
inline int g_g_modScroll = 0;
inline int g_g_mmSel = 0;
inline int g_g_mmField = 0;


// Mod-driven targets
inline float g_cc11Expr = 1.0f;
inline float g_cc74Bright = 0.5f;
inline float g_pan01 = 0.5f;
inline float g_porta01[NUM_VOICES] = {0.f, 0.f, 0.f};
// Per-slot outputs for DEST_MIDI_CC (0..1); -1 = slot inactive this pass.
inline float g_modCC01[MOD_SLOTS] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};



// Shared engine functions implemented in terrarium_core.cpp.
void applyModMatrix();
void updateModPool(World& w, int tick, int viewW, int viewH);
Season seasonAt(int tick);
float seasonLerp(int tick);
bool nightish(int tick);
const char* weatherName(WeatherState state);
const char* seasonName(Season season);
const char* speciesName(uint8_t s);
const char* biomeName(Biome b);
BiomeWeights weightsFor(Biome b);
BiomeWeights lerpBiomeWeights(const BiomeWeights& a, const BiomeWeights& b, float t);
void initClouds(Clouds& c, Rng& r, Biome b);
float clamp01(float v);
char waterFlowGlyph(const World& w, int x, int y, int tick);
bool isWaterVisualGlyph(unsigned char c);
bool isTree(char c);
bool isVeg(char c);
int speciesVariant2(const World& w, int x, int y, int n);
void seedWorld(World& w, Rng& r, Biome biome);
void step(World& w, Rng& r, std::string& banner, int tick);
