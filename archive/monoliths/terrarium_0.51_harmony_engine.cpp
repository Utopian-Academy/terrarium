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

// --- Audio voice count (melodic channels 0..NUM_VOICES-1) ---
static constexpr int NUM_VOICES = 3;

// clamp to [-1, +1]
static inline float clamp11f(float v){ return v<-1.f?-1.f:(v>1.f?1.f:v); }


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

static int countNeighborsChar(const Grid& g, int x, int y, char c) {
  int n = 0;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (dx==0 && dy==0) continue;
    int nx=x+dx, ny=y+dy;
    if (inBounds(nx,ny) && g[ny][nx]==c) n++;
  }
  return n;
}
// ===== Shared state for new features (zoom/pan, ripples, alea weights) =====
struct World; struct Rng;
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
static std::vector<StepEvent> g_stepEvents;

static int g_zoom = 1;
static int g_inspectIdx = -1;       // selected agent index
static bool g_followInspect = false; // camera follows selected agent when true

static int g_camX = 0;
static int g_camY = 0;
static std::vector<Ripple> g_ripples;
static AleaWeights g_alea;

static constexpr int MOD_N = 50;
static const char* g_modName[MOD_N] = {
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
static float g_modVal[MOD_N] = {0};

// --- Mod Matrix ---
enum ModDest : int {
  DEST_NONE=0,
  DEST_CC11_EXPR=1,
  DEST_CC74_BRIGHT=2,
  DEST_PAN=3,
  DEST_PORTA_V0=4,
  DEST_PORTA_V1=5,
  DEST_PORTA_V2=6,
};

static inline const char* modDestName(int d){
  switch(d){
    case DEST_CC11_EXPR: return "CC11 Expr";
    case DEST_CC74_BRIGHT: return "CC74 Bright";
    case DEST_PAN: return "CC10 Pan";
    case DEST_PORTA_V0: return "Porta V0";
    case DEST_PORTA_V1: return "Porta V1";
    case DEST_PORTA_V2: return "Porta V2";
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
};
static constexpr int MOD_SLOTS=12;
static ModMap g_modMap[MOD_SLOTS];
static int g_g_modScroll=0; static int g_g_mmSel=0; static int g_g_mmField=0;


// Mod-driven targets
static float g_cc11Expr=1.0f;
static float g_cc74Bright=0.5f;
static float g_pan01=0.5f;
static float g_porta01[NUM_VOICES]={0.f,0.f,0.f};

static inline float smooth1(float cur,float tgt,float s){
  float a=std::clamp(s,0.0f,0.98f);
  return cur*a + tgt*(1.0f-a);
}

static void applyModMatrix(){
  g_cc11Expr=1.0f; g_cc74Bright=0.5f; g_pan01=0.5f;
  for(int v=0; v<NUM_VOICES; ++v) g_porta01[v]=0.0f;

  for(int i=0;i<MOD_SLOTS;++i){
    auto& mm=g_modMap[i];
    if(!mm.enabled || mm.dest==DEST_NONE) continue;
    int src=std::clamp(mm.src,0,MOD_N-1);
    float x=g_modVal[src]; // [-1..1]
    float target=x*mm.amt;
    mm.state=smooth1(mm.state,target,mm.smooth);
    float v=mm.state;

    switch(mm.dest){
      case DEST_CC11_EXPR: g_cc11Expr=std::clamp(1.0f+0.7f*v,0.0f,1.0f); break;
      case DEST_CC74_BRIGHT: g_cc74Bright=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PAN: g_pan01=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V0: g_porta01[0]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V1: g_porta01[1]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V2: g_porta01[2]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      default: break;
    }
  }
}


static void updateModPool(World& w, int tick, int viewW, int viewH);

static inline void clampCameraToWorld();
static inline void applyRippleChaos(World& w, Rng& r, int tick);
static inline char renderCharAtBase(const World& w, int x, int y, int tick);



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

enum Species : uint8_t {
  SPEC_WANDERER=0,
  SPEC_SHELLBACK=1,   // "turtle-ish" abstract
  SPEC_SWARMER=2,
  SPEC_ENGINEER=3,
  SPEC_PARASITE=4,
  SPEC_PACKHUNTER=5,
  SPEC_MYSTIC=6,
  SPEC_TRICKSTER=7,
  SPEC_COUNT=8
};




static inline const char* speciesName(uint8_t s){
  switch(s){
    case SPEC_WANDERER:  return "WANDERER";
    case SPEC_SHELLBACK: return "SHELLBACK";
    case SPEC_SWARMER:   return "SWARMER";
    case SPEC_ENGINEER:  return "ENGINEER";
    case SPEC_PARASITE:  return "PARASITE";
    case SPEC_PACKHUNTER:return "PACKHUNTER";
    case SPEC_MYSTIC:    return "MYSTIC";
    case SPEC_TRICKSTER: return "TRICKSTER";
  }
  return "???";
}

static inline uint8_t pickBiomeSpecies(Biome b, Rng& r){
  // Abstract distributions per biome for variety (no new glyphs required).
  // We bias different "personalities" rather than different sprites.
  float u = r.u01();
  switch(b){
    case WETLAND:
      if(u<0.18f) return SPEC_SHELLBACK;
      if(u<0.32f) return SPEC_SWARMER;
      if(u<0.44f) return SPEC_PARASITE;
      if(u<0.56f) return SPEC_ENGINEER;
      if(u<0.70f) return SPEC_MYSTIC;
      if(u<0.82f) return SPEC_TRICKSTER;
      return SPEC_WANDERER;
    case DESERT:
      if(u<0.22f) return SPEC_ENGINEER;
      if(u<0.40f) return SPEC_TRICKSTER;
      if(u<0.54f) return SPEC_PACKHUNTER;
      if(u<0.68f) return SPEC_MYSTIC;
      if(u<0.80f) return SPEC_PARASITE;
      return SPEC_WANDERER;
    case TROPICAL:
      if(u<0.22f) return SPEC_SWARMER;
      if(u<0.40f) return SPEC_PARASITE;
      if(u<0.54f) return SPEC_ENGINEER;
      if(u<0.68f) return SPEC_TRICKSTER;
      if(u<0.80f) return SPEC_MYSTIC;
      return SPEC_WANDERER;
    case ALPINE:
      if(u<0.20f) return SPEC_PACKHUNTER;
      if(u<0.38f) return SPEC_MYSTIC;
      if(u<0.54f) return SPEC_ENGINEER;
      if(u<0.68f) return SPEC_TRICKSTER;
      if(u<0.80f) return SPEC_SWARMER;
      return SPEC_WANDERER;
    case ALIEN:
      if(u<0.25f) return SPEC_MYSTIC;
      if(u<0.45f) return SPEC_TRICKSTER;
      if(u<0.60f) return SPEC_PARASITE;
      if(u<0.74f) return SPEC_ENGINEER;
      if(u<0.86f) return SPEC_SWARMER;
      return SPEC_WANDERER;
    case MEADOW:
    default:
      if(u<0.18f) return SPEC_SWARMER;
      if(u<0.32f) return SPEC_ENGINEER;
      if(u<0.44f) return SPEC_TRICKSTER;
      if(u<0.56f) return SPEC_MYSTIC;
      if(u<0.68f) return SPEC_PACKHUNTER;
      if(u<0.78f) return SPEC_PARASITE;
      return SPEC_WANDERER;
  }
}

// Display glyphs: animated, biome-tinted per abstract species (does not affect behavior glyphs).
static inline char speciesDisplayGlyph(uint8_t spec, Biome b, int tick, bool legendA, bool legendB){
  int ph = (tick/5)&1; // 2-frame animation
  if(legendA) return ph? 'Y' : 'y';  // "Legendary" couple (A)
  if(legendB) return ph? 'Z' : 'z';  // "Legendary" couple (B)
  // Per-biome flavor: same species feels different in each biome.
  switch(b){
    case WETLAND:
      switch(spec){
        case SPEC_SHELLBACK: return ph? 'o':'O';
        case SPEC_SWARMER:   return ph? '~':'^';
        case SPEC_ENGINEER:  return ph? '#':'H';
        case SPEC_PARASITE:  return ph? ':':'!';
        case SPEC_PACKHUNTER:return ph? 'V':'v';
        case SPEC_MYSTIC:    return ph? '*':'+';
        case SPEC_TRICKSTER: return ph? '?':'%';
        default:             return ph? '.':',';
      }
    case DESERT:
      switch(spec){
        case SPEC_SHELLBACK: return ph? 'u':'U';
        case SPEC_SWARMER:   return ph? '`':'\'';
        case SPEC_ENGINEER:  return ph? 'T':'t';
        case SPEC_PARASITE:  return ph? ';':':';
        case SPEC_PACKHUNTER:return ph? 'A':'a';
        case SPEC_MYSTIC:    return ph? '*':'x';
        case SPEC_TRICKSTER: return ph? '$':'&';
        default:             return ph? '.':',';
      }
    case ALIEN:
      switch(spec){
        case SPEC_SHELLBACK: return ph? '0':'@';
        case SPEC_SWARMER:   return ph? '=':'-';
        case SPEC_ENGINEER:  return ph? 'M':'W';
        case SPEC_PARASITE:  return ph? 'i':'I';
        case SPEC_PACKHUNTER:return ph? 'K':'k';
        case SPEC_MYSTIC:    return ph? '*':'o';
        case SPEC_TRICKSTER: return ph? '?':'/';
        default:             return ph? '.':',';
      }
    case TROPICAL:
      switch(spec){
        case SPEC_SHELLBACK: return ph? 'o':'O';
        case SPEC_SWARMER:   return ph? '"':'~';
        case SPEC_ENGINEER:  return ph? 'n':'N';
        case SPEC_PARASITE:  return ph? ':':'!';
        case SPEC_PACKHUNTER:return ph? 'v':'V';
        case SPEC_MYSTIC:    return ph? '+':'*';
        case SPEC_TRICKSTER: return ph? '%':'?';
        default:             return ph? '.':',';
      }
    case MEADOW:
    default:
      switch(spec){
        case SPEC_SHELLBACK: return ph? 'o':'O';
        case SPEC_SWARMER:   return ph? ',':'.';
        case SPEC_ENGINEER:  return ph? 'h':'H';
        case SPEC_PARASITE:  return ph? ':':'!';
        case SPEC_PACKHUNTER:return ph? 'v':'V';
        case SPEC_MYSTIC:    return ph? '+':'*';
        case SPEC_TRICKSTER: return ph? '%':'?';
        default:             return ph? '.':',';
      }
  }
}

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


static inline BiomeWeights lerpBiomeWeights(const BiomeWeights& a, const BiomeWeights& b, float t){
  BiomeWeights o;
  o.pondDensity     = a.pondDensity     + (b.pondDensity     - a.pondDensity)     * t;
  o.stoneChance     = a.stoneChance     + (b.stoneChance     - a.stoneChance)     * t;
  o.reedChance      = a.reedChance      + (b.reedChance      - a.reedChance)      * t;
  o.fernChance      = a.fernChance      + (b.fernChance      - a.fernChance)      * t;
  o.flowerChance    = a.flowerChance    + (b.flowerChance    - a.flowerChance)    * t;
  o.bigFlowerChance = a.bigFlowerChance + (b.bigFlowerChance - a.bigFlowerChance) * t;
  o.treeChance      = a.treeChance      + (b.treeChance      - a.treeChance)      * t;
  o.mushChance      = a.mushChance      + (b.mushChance      - a.mushChance)      * t;
  o.growRate        = a.growRate        + (b.growRate        - a.growRate)        * t;
  o.bloomRate       = a.bloomRate       + (b.bloomRate       - a.bloomRate)       * t;
  o.fireRate        = a.fireRate        + (b.fireRate        - a.fireRate)        * t;
  o.alienRate       = a.alienRate       + (b.alienRate       - a.alienRate)       * t;
  return o;
}

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
  // Biome morph (non-destructive transition)
  bool biomeMorphActive = false;
  float biomeMorphT = 0.0f; // 0..1
  BiomeWeights bwFrom{};
  BiomeWeights bwTo{};
  BiomeWeights bw;
  // Hydrology stability
  std::vector<std::vector<uint8_t>> moist; // 0..255 groundwater/moisture reserve
  std::vector<std::pair<int,int>> springs; // persistent water sources
  std::vector<Agent> agents; // persistent fauna agents (overlay on entities grid)

};
// --- Mod pool update (moved here so World/Species are defined) ---
static void updateModPool(World& w, int tick, int viewW, int viewH){
  // Sample the current camera viewport to keep modulation "what you see is what you hear".
  int x0=g_camX, y0=g_camY;
  int x1=std::min(W, g_camX+viewW);
  int y1=std::min(H, g_camY+viewH);
  int area = std::max(1, (x1-x0)*(y1-y0));

  int waterC=0, plantC=0, overC=0;
  int agentsV=0, panicC=0;
  float stressSum=0, hungerSum=0, thirstSum=0, fatSum=0, healthSum=0;
  float speedSum=0;
  int stressHi=0;

  // Species-derived silly counters
  int shellHi=0, shellN=0;
  int swarmN=0;
  int parasiteN=0;
  int engineerN=0;
  int mysticN=0;
  int trickN=0;
  int packN=0;

  // Quick tile sampling (stride for speed)
  int sx = std::max(1, (x1-x0)/64);
  int sy = std::max(1, (y1-y0)/36);
  int samp=0;
  for(int y=y0; y<y1; y+=sy){
    for(int x=x0; x<x1; x+=sx){
      ++samp;
      if (w.water[y][x]>0) ++waterC;
      if (isEdiblePlant(w.terrain[y][x])) ++plantC;
      if (w.overlay[y][x] != ' ') ++overC;
    }
  }
  float waterFrac = (samp>0)? (float)waterC/samp : 0.f;
  float plantFrac = (samp>0)? (float)plantC/samp : 0.f;
  float overFrac  = (samp>0)? (float)overC/samp  : 0.f;

  // Agent stats within view
  for (auto &a: w.agents){
    if (a.x<x0 || a.x>=x1 || a.y<y0 || a.y>=y1) continue;
    agentsV++;
    stressSum += a.stress;
    hungerSum += a.hunger;
    thirstSum += a.thirst;
    fatSum    += a.fatigue;
    healthSum += a.health;
    if (a.stress > 0.75f) stressHi++;
    if (a.flags & 1) panicC++;

    // speed proxy: flee/wander/drink intents influence audible motion
    float sp = 0.0f;
    if (a.intent==INTENT_FLEE) sp = 1.0f;
    else if (a.intent==INTENT_DRINK || a.intent==INTENT_FORAGE || a.intent==INTENT_HUNT) sp = 0.7f;
    else sp = 0.35f;
    sp *= (1.0f - 0.65f*a.fatigue);
    speedSum += sp;

    switch(a.species){
      case SPEC_SHELLBACK: shellN++; if (a.stress>0.70f) shellHi++; break;
      case SPEC_SWARMER: swarmN++; break;
      case SPEC_PARASITE: parasiteN++; break;
      case SPEC_ENGINEER: engineerN++; break;
      case SPEC_MYSTIC: mysticN++; break;
      case SPEC_TRICKSTER: trickN++; break;
      case SPEC_PACKHUNTER: packN++; break;
      default: break;
    }
  }
  float invA = (agentsV>0)? (1.0f/agentsV) : 0.f;
  float stressMean = stressSum*invA;
  float hungerMean = hungerSum*invA;
  float thirstMean = thirstSum*invA;
  float fatMean    = fatSum*invA;
  float healthMean = healthSum*invA;
  float agentSpeed = speedSum*invA;

  // Predator pressure in view
  int preds=0, prey=0;
  for (auto &a: w.agents){
    if (a.x<x0 || a.x>=x1 || a.y<y0 || a.y>=y1) continue;
    if (isPredator(a.glyph)) preds++; else prey++;
  }
  float predPressure = (prey>0)? (float)preds/(float)prey : (preds? 4.f:0.f);

  // Ripple energy near view
  float rippleE=0.f;
  for (auto &rp: g_ripples){
    // approximate: more recent + closer to view center
    float cx=float(rp.cx), cy=float(rp.cy);
    float vx=float((x0+x1)*0.5f), vy=float((y0+y1)*0.5f);
    float dx=cx-vx, dy=cy-vy;
    float dist = std::sqrt(dx*dx+dy*dy);
    rippleE += (rp.amp) * std::exp(-dist/40.0f) * (1.0f - std::min(1.0f, rp.t/3.0f));
  }
  rippleE = std::min(3.0f, rippleE);

  // Flux: compare against previous values (store in tail of g_modVal)
  static float prevWater=0, prevPlant=0, prevStress=0, prevHunger=0, prevThirst=0, prevFat=0, prevHealth=0, prevPanic=0;
  float waterFlux = std::fabs(waterFrac - prevWater);
  float plantFlux = std::fabs(plantFrac - prevPlant);
  float stressFlux= std::fabs(stressMean - prevStress);
  float hungerFlux= std::fabs(hungerMean - prevHunger);
  float thirstFlux= std::fabs(thirstMean - prevThirst);
  float fatFlux   = std::fabs(fatMean - prevFat);
  float healthFlux= std::fabs(healthMean - prevHealth);
  float panicFlux = std::fabs((float)panicC*invA - prevPanic);

  prevWater=waterFrac; prevPlant=plantFrac; prevStress=stressMean; prevHunger=hungerMean; prevThirst=thirstMean; prevFat=fatMean; prevHealth=healthMean; prevPanic=(float)panicC*invA;

  // Fill mod array (0..1-ish)
  auto clamp01f=[](float v){ return v<0.f?0.f:(v>1.f?1.f:v); };
  g_modVal[0]=clamp01f(waterFrac);
  g_modVal[1]=clamp01f(plantFrac);
  g_modVal[2]=clamp01f(overFrac);
  g_modVal[3]=clamp01f((float)agentsV/60.f);
  g_modVal[4]=clamp01f(agentSpeed);

  g_modVal[5]=clamp01f(stressMean);
  g_modVal[6]=clamp01f((float)stressHi/ std::max(1.f,(float)agentsV));
  g_modVal[7]=clamp01f((float)panicC/ std::max(1.f,(float)agentsV));
  g_modVal[8]=clamp01f(hungerMean);
  g_modVal[9]=clamp01f(thirstMean);

  g_modVal[10]=clamp01f(fatMean);
  g_modVal[11]=clamp01f(healthMean);
  g_modVal[12]=clamp01f(std::min(2.0f,predPressure)/2.0f);

  // Birth/death pulses: use existing counters if any, else fake from flux
  g_modVal[13]=clamp01f(plantFlux*3.0f);
  g_modVal[14]=clamp01f(stressFlux*3.0f);

  g_modVal[15]=clamp01f(rippleE/3.0f);
  float windMag = std::sqrt((float)w.wind.dx*w.wind.dx + (float)w.wind.dy*w.wind.dy) * (float)w.wind.strength/8.0f;
  g_modVal[16]=clamp01f(windMag);
  g_modVal[17]=clamp01f(seasonLerp(tick));
  g_modVal[18]=clamp01f(w.cloudOpacity);
  g_modVal[19]=clamp01f((float)w.weather.state/4.0f);

  g_modVal[20]=clamp01f((shellN>0)? (float)shellHi/shellN : 0.f);
  // swarm cohesion: more swarmers + less speed => higher cohesion (cute but useful)
  g_modVal[21]=clamp01f((float)swarmN/50.f * (1.0f - agentSpeed));
  g_modVal[22]=clamp01f((float)parasiteN/40.f * (0.3f + stressMean));
  g_modVal[23]=clamp01f((float)engineerN/40.f * (0.2f + waterFrac));
  g_modVal[24]=clamp01f((float)mysticN/40.f * (0.5f + overFrac));

  g_modVal[25]=clamp01f((float)trickN/40.f * (0.5f + rippleE/3.0f));
  g_modVal[26]=clamp01f((float)packN/40.f * (0.4f + predPressure*0.25f));
  g_modVal[27]=clamp01f(plantFlux*4.0f);
  g_modVal[28]=clamp01f(waterFlux*4.0f);
  g_modVal[29]=clamp01f(stressFlux*4.0f);

  g_modVal[30]=clamp01f(hungerFlux*4.0f);
  g_modVal[31]=clamp01f(thirstFlux*4.0f);
  g_modVal[32]=clamp01f(fatFlux*4.0f);
  g_modVal[33]=clamp01f(healthFlux*4.0f);
  g_modVal[34]=clamp01f(panicFlux*4.0f);

  // Oddities: silly mixtures, designed to wiggle
  float o0 = (g_modVal[20]*g_modVal[15]);                  // shellback stress * ripples
  float o1 = (g_modVal[12]* (1.0f-g_modVal[1]));          // pred pressure * low plants
  float o2 = (g_modVal[8]*g_modVal[9]);                   // hunger * thirst
  float o3 = std::fabs(g_modVal[16]-g_modVal[19]);         // wind vs rain mismatch
  float o4 = g_modVal[21] * (0.3f + g_modVal[2]);          // swarm cohesion * overlay
  float o5 = g_modVal[5] * (0.5f + g_modVal[28]);          // stress * water flux
  float o6 = g_modVal[11] * (1.0f - g_modVal[10]);         // health vs fatigue
  float o7 = g_modVal[24] * (0.2f + g_modVal[17]);         // mystic flux * season
  float o8 = g_modVal[25] * (0.2f + g_modVal[4]);          // trickster mischief * speed
  float o9 = (g_modVal[0]+g_modVal[1])*0.5f;               // wet+green
  float o10= std::fabs(g_modVal[0]-g_modVal[1]);           // wet vs green contrast
  float o11= g_modVal[7] * (0.3f + g_modVal[12]);          // panic * pred pressure
  float o12= g_modVal[18] * (0.2f + g_modVal[2]);          // clouds * overlay
  float o13= g_modVal[13] * (0.2f + g_modVal[29]);         // birthpulse * stress flux
  float o14= (float)((tick/37)%11)/10.0f;                  // weird slow sawtooth

  float odd[15]={o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14};

// --- Bipolar + spiky modulation ---
static float prev[MOD_N] = {0};
for(int i=0;i<15;++i) g_modVal[35+i]=clamp01f(odd[i]);

for(int i=0;i<MOD_N;++i){
  float v = g_modVal[i]*2.0f - 1.0f;      // 0..1 -> -1..+1
  float dv = v - prev[i];
  prev[i] = v;
  float sp = v + 0.85f*dv + ((float)((tick + i*131) % 97) / 96.0f - 0.5f) * 0.06f;
  g_modVal[i] = clamp11f(sp);
}



}
// --- end mod pool update ---


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
      chance = (int)std::clamp((float)chance / std::max(0.10f, g_alea.rainChance * g_alea.chaos), 1.0f, 20000.0f);
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
    chance = (int)std::clamp((float)chance / std::max(0.10f, g_alea.spawnChance * g_alea.chaos), 1.0f, 20000.0f);
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
  w.biomeMorphActive = false;
  w.biomeMorphT = 0.0f;
  w.bwFrom = w.bw;
  w.bwTo = w.bw;

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
  int nextId=0;
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      char g = w.entities[y][x];
      if(g=='.' || g==' ') continue;
      Agent a; a.id=nextId++; a.x=x; a.y=y; a.glyph=g; a.species = pickBiomeSpecies(w.biome, r);
      // randomize initial needs a bit
      a.hunger = r.u01()*0.5f;
      a.thirst = r.u01()*0.5f;
      a.stress = r.u01()*0.2f;
      a.health = 0.8f + r.u01()*0.2f;
      w.agents.push_back(a);
    }
  }
}


static inline bool cellPassableForAgent(const World& w, int x, int y){
  if(!inBounds(x,y)) return false;
  char t = w.terrain[y][x];
  if(t=='#') return false;
  return true;
}

static void ensureLegendaryCouple(World& w, Rng& r){
  bool haveA=false, haveB=false;
  for(const auto& a: w.agents){
    if(a.flags & AGF_LEGEND_A) haveA=true;
    if(a.flags & AGF_LEGEND_B) haveB=true;
  }
  auto spawnLegend = [&](uint8_t flag){
    // Try near camera center-ish, else random.
    int cx = W/2, cy = H/2;
    for(int tries=0; tries<4000; ++tries){
      int x = (tries<500) ? clampi(cx + (int)r.irange(-20,20), 0, W-1) : r.irange(0,W-1);
      int y = (tries<500) ? clampi(cy + (int)r.irange(-12,12), 0, H-1) : r.irange(0,H-1);
      if(!cellPassableForAgent(w,x,y)) continue;
      // don't spawn on another agent
      bool occ=false;
      for(const auto& a: w.agents){ if(a.x==x && a.y==y) { occ=true; break; } }
      if(occ) continue;

      Agent a;
      a.id = (int)w.agents.size() ? (w.agents.back().id + 1) : 999999;
      a.x=x; a.y=y;
      a.glyph = (flag==AGF_LEGEND_A)? 'Y':'Z'; // behavior glyph (kept distinct)
      a.species = pickBiomeSpecies(w.biome, r);
      a.hunger = r.u01()*0.2f;
      a.thirst = r.u01()*0.2f;
      a.stress = r.u01()*0.1f;
      a.fatigue = r.u01()*0.2f;
      a.health = 1.0f;
      a.flags |= flag;
      w.agents.push_back(a);
      return;
    }
  };

  if(!haveA) spawnLegend(AGF_LEGEND_A);
  if(!haveB) spawnLegend(AGF_LEGEND_B);
}
static void agentsWriteToGrid(World& w, int tick){
  // clear entities grid
  for(int y=0;y<H;++y) for(int x=0;x<W;++x) w.entities[y][x]=' ';
  for(auto &a: w.agents){
    if(!inBounds(a.x,a.y)) continue;
    w.entities[a.y][a.x]=speciesDisplayGlyph(a.species, w.biome, tick, (a.flags&AGF_LEGEND_A)!=0, (a.flags&AGF_LEGEND_B)!=0);
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

      // Species quirks (abstract "species" that vary per biome)
      // These are designed to create wacky interactions + interesting modulation signals.
      if (a.species == SPEC_PARASITE) {
        // Parasites raise nearby stress and steal a bit of hydration/energy.
        for (auto &b : w.agents) {
          if (&b==&a) continue;
          int d = std::abs(b.x-a.x) + std::abs(b.y-a.y);
          if (d<=1) {
            b.stress = clamp01(b.stress + 0.10f*dt);
            b.thirst = clamp01(b.thirst + 0.04f*dt);
            a.hunger = clamp01(a.hunger - 0.03f*dt);
          }
        }
      } else if (a.species == SPEC_ENGINEER) {
        // Engineers occasionally tweak terrain: seed plants or shallow water.
        if (r.oneIn(30)) {
          if (w.water[a.y][a.x]>0 && w.water[a.y][a.x]<3) {
            // turn shallow water into algae/plant hint
            if (isEdiblePlant(w.terrain[a.y][a.x])==false && r.oneIn(2)) w.terrain[a.y][a.x] = '"';
          } else if (w.water[a.y][a.x]==0 && r.oneIn(3)) {
            // sometimes dig a tiny puddle
            w.water[a.y][a.x] = 1;
          }
          a.stress = clamp01(a.stress - 0.04f*dt);
        }
      } else if (a.species == SPEC_SWARMER) {
        // Swarmers like being near others of their kind: calm when clustered, stress when isolated.
        int near=0;
        for (auto &b : w.agents) {
          if (&b==&a) continue;
          if (b.species != SPEC_SWARMER) continue;
          int d = std::abs(b.x-a.x) + std::abs(b.y-a.y);
          if (d<=4) near++;
        }
        if (near>=3) a.stress = clamp01(a.stress - 0.06f*dt);
        else a.stress = clamp01(a.stress + 0.03f*dt);
      } else if (a.species == SPEC_SHELLBACK) {
        // "Shellbacks" are calm but react strongly to ripples/chaos.
        if (!g_ripples.empty()) a.stress = clamp01(a.stress + 0.03f*dt);
        a.fatigue = clamp01(a.fatigue - 0.02f*dt);
      } else if (a.species == SPEC_MYSTIC) {
        // Mystics fluctuate: occasionally spike mood in either direction.
        if (r.oneIn(40)) {
          float j = (r.u01()<0.5f) ? -0.25f : 0.25f;
          a.stress = clamp01(a.stress + j);
          a.hunger = clamp01(a.hunger + 0.10f*(r.u01()-0.5f));
        }
      } else if (a.species == SPEC_TRICKSTER) {
        // Tricksters chase click-ripples; when close to a ripple origin they get euphoric.
        if (!g_ripples.empty()) {
          const Ripple& rp = g_ripples.back();
          int d = std::abs(a.x - rp.cx) + std::abs(a.y - rp.cy);
          if (d<6) {
            a.stress = clamp01(a.stress - 0.08f*dt);
            a.hunger = clamp01(a.hunger + 0.02f*dt); // "forget to eat"
          }
        }
      } else if (a.species == SPEC_PACKHUNTER) {
        // Packhunters get bold in groups and increase predator pressure.
        int pack=0;
        for (auto &b : w.agents) {
          if (b.species!=SPEC_PACKHUNTER) continue;
          int d=std::abs(b.x-a.x)+std::abs(b.y-a.y);
          if (d<=5) pack++;
        }
        if (pack>=3) a.stress = clamp01(a.stress - 0.05f*dt);
      }

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
      if (isLegendary(a)) { a.health = std::max(a.health, 0.12f); a.stress = clamp01(a.stress * 0.985f); }

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

            // Derive mood from strongest drive (DF-ish but cheap)
      a.mood = MOOD_CALM;
      float best = a.stress;
      if (a.thirst > best) { best = a.thirst; a.mood = MOOD_THIRSTY; }
      if (a.hunger > best) { best = a.hunger; a.mood = MOOD_HUNGRY; }
      if (a.stress > 0.70f) a.mood = MOOD_FEARFUL;
      if (a.health < 0.25f && a.stress > 0.6f) a.mood = MOOD_ENRAGED;
      if (a.hunger < 0.25f && a.thirst < 0.25f && a.stress < 0.20f) a.mood = MOOD_EUPHORIC;

// Movement: very simple, but driven by needs and panic.
      int ox=a.x, oy=a.y;
      int nx=a.x, ny=a.y;

      if (a.flags & 1) {
        a.intent = INTENT_FLEE;
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
        a.intent = INTENT_DRINK;
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
        a.intent = INTENT_FORAGE;
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
        a.intent = INTENT_WANDER;
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

      // Fatigue: movement costs, calm recovers
      if (a.x!=ox || a.y!=oy) a.fatigue = clamp01(a.fatigue + 0.08f * dt);
      else a.fatigue = clamp01(a.fatigue - 0.04f * dt);

      if (a.x!=ox || a.y!=oy) {
        StepEvent ev; ev.x=a.x; ev.y=a.y; ev.dx=a.x-ox; ev.dy=a.y-oy; ev.glyph=a.glyph;
        ev.strength = (std::abs(ev.dx)+std::abs(ev.dy) > 1) ? 2.f : 1.f;
        g_stepEvents.push_back(ev);
      }
    }

    // Cull dead agents (and clear their glyph)
    w.agents.erase(std::remove_if(w.agents.begin(), w.agents.end(),
      [&](const Agent& a){ return a.health <= 0.01f; }), w.agents.end());
  }

  agentsWriteToGrid(w, tick);
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
  // keep the Legendary Couple in play
  ensureLegendaryCouple(w, r);

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
  applyRippleChaos(w, r, tick);
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
  // Click ripples: sample from a displaced cell to create a literal glyph-wave.
  int dx = 0, dy = 0;
  for (const auto& r : g_ripples) {
    float rx = float(x - r.cx);
    float ry = float(y - r.cy);
    float dist = std::sqrt(rx*rx + ry*ry);
    float ring = r.speed * r.t;
    float d = std::fabs(dist - ring);
    if (d < r.width) {
      float s = (1.0f - d / r.width) * r.amp;
      float inv = (dist > 0.001f) ? (1.0f / dist) : 0.0f;
      dx += int(std::lround(rx * inv * s));
      dy += int(std::lround(ry * inv * s));
    }
  }
  int sx = clampi(x + dx, 0, W-1);
  int sy = clampi(y + dy, 0, H-1);
  return renderCharAtBase(w, sx, sy, tick);
}

static inline char renderCharAtBase(const World& w, int x, int y, int tick) {
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



// (zoom/pan + ripples + alea weights globals declared near top)

static inline float smoothstep01(float t){
  t = std::clamp(t, 0.0f, 1.0f);
  return t*t*(3.0f-2.0f*t);
}

static inline void clampCameraToWorld() {
  int viewW = std::max(1, W / std::max(1, g_zoom));
  int viewH = std::max(1, H / std::max(1, g_zoom));
  g_camX = clampi(g_camX, 0, std::max(0, W - viewW));
  g_camY = clampi(g_camY, 0, std::max(0, H - viewH));
}

static inline void updateRipples(float dt) {
  for (auto &r : g_ripples) r.t += dt;
  g_ripples.erase(std::remove_if(g_ripples.begin(), g_ripples.end(),
    [](const Ripple& r){ return r.t > 3.0f; }), g_ripples.end());
}


static inline void applyRippleChaos(World& w, Rng& r, int tick) {
  if (g_ripples.empty()) return;
  // light touch: perturb a few cells around each ripple ring
  for (const auto& rp : g_ripples) {
    float ring = rp.speed * rp.t;

    // Push fauna/agents on the shock-ring (actual simulation chaos, not just visual)
    {
      const float p = rp.chaos * g_alea.chaos;
      for (auto &a : w.agents) {
        float rx = float(a.x - rp.cx);
        float ry = float(a.y - rp.cy);
        float dist = std::sqrt(rx*rx + ry*ry);
        float d = std::fabs(dist - ring);
        if (d > rp.width) continue;

        float strength = (1.0f - d / std::max(0.001f, rp.width)) * rp.amp; // 0..amp
        if (dist < 0.001f) { rx = 1.f; ry = 0.f; dist = 1.f; }
        float nx = rx / dist, ny = ry / dist;
        int push = (strength > 1.6f) ? 2 : 1;
        int nxCell = clampi(a.x + (int)std::lround(nx * push), 0, W-1);
        int nyCell = clampi(a.y + (int)std::lround(ny * push), 0, H-1);

        // Basic passability: non-aquatic agents avoid deep water.
        if (!isAquatic(a.glyph) && w.water[nyCell][nxCell] > 5) {
          // try sideways jitter
          if (std::fabs(nx) > std::fabs(ny)) nyCell = clampi(nyCell + (r.oneIn(2)?1:-1), 0, H-1);
          else nxCell = clampi(nxCell + (r.oneIn(2)?1:-1), 0, W-1);
        }

        a.x = nxCell;
        a.y = nyCell;

        // Status chaos
        a.stress = clamp01(a.stress + 0.10f * strength * p);
        if (r.u01() < 0.10f * p) a.flags |= 1; // panic bit
        if (r.u01() < 0.08f * p) {
          a.hunger = clamp01(a.hunger + 0.05f * p);
          a.thirst = clamp01(a.thirst + 0.05f * p);
        }
      }
    }
    int samples = 10; // cheap
    for (int i=0;i<samples;i++){
      float ang = (float)(r.u01() * 6.2831853);
      float rad = ring + (r.u01()*2.f - 1.f) * rp.width;
      int x = rp.cx + (int)std::lround(std::cos(ang) * rad);
      int y = rp.cy + (int)std::lround(std::sin(ang) * rad);
      if (!inBounds(x,y)) continue;

      float p = rp.chaos * g_alea.chaos;
      if (r.u01() < 0.25f * p) {
        // splash / drain
        int d = (int)w.water[y][x];
        d += (r.oneIn(2)? 1 : -1);
        w.water[y][x] = (uint8_t)clampi(d, 0, 9);
      }
      if (r.u01() < 0.18f * p) {
        // scramble overlay (visual chaos)
        static const char ov[] = {'~','`','*','+',';','"',':','.'};
        w.overlay[y][x] = ov[r.irange(0,(int)(sizeof(ov)-1))];
      }
      if (r.u01() < 0.08f * p) {
        // nudge terrain locally
        char &t = w.terrain[y][x];
        if (t=='.') t=',';
        else if (t==',') t='"';
        else if (t=='"') t=';';
        else if (t==';') t='.';
      }
    }
  }
  (void)tick;
}

// Per-voice pitch + program settings (3 melodic voices + drums on ch9)
struct VoiceSettings {
  int minNote = 36;   // C2
  int maxNote = 84;   // C6
  int program = 10;   // Music Box default
  int bankMSB = 0;
  int bankLSB = 0;
  int transpose = 0;  // semitones
  float velMul = 1.0f;
};
static VoiceSettings g_voice[NUM_VOICES] = {
  {40, 88, 10, 0,0, 0, 1.0f}, // voice 0
  {40, 88, 12, 0,0, 0, 0.9f}, // voice 1
  {28, 72, 11, 0,0, 0, 0.8f}, // bass-ish voice 2
};

// Per-voice mixer (CC7 = fader, CC11 = animated expression)
static int   g_lastCC5[NUM_VOICES] = {-1,-1,-1};
static int   g_lastCC65[NUM_VOICES] = {-1,-1,-1};
static int   g_lastCC11[NUM_VOICES] = {-1,-1,-1};
static int   g_lastCC74[NUM_VOICES] = {-1,-1,-1};
static int   g_lastCC10[NUM_VOICES] = {-1,-1,-1};
static float g_voiceFader[NUM_VOICES] = {1.f, 1.f, 1.f}; // 0..2 (1.0 = unity, >1 adds headroom via velocity gain)
static float g_voiceAuto[NUM_VOICES]  = {1.f, 1.f, 1.f}; // 0..1
static bool  g_voiceMute[NUM_VOICES]  = {false, false, false};

static float g_drumsFader = 1.f; // ch9
static float g_drumsAuto  = 1.f;
static bool  g_drumsMute  = false;

// Animated (screen-driven) multipliers for note gating; multiplied into g_alea.noteLen/holdChance (user knobs)
static float g_noteLenAutoMul = 1.0f;
static float g_holdChanceAutoMul = 1.0f;

// Solo state for mixer page (-1 = none, 0..NUM_VOICES-1 voices, NUM_VOICES = drums)
static int   g_soloRow = -1;
static float g_savedVoiceFader[NUM_VOICES] = {1.f,1.f,1.f};
static float g_savedDrumsFader = 1.f;
static bool  g_savedVoiceMute[NUM_VOICES] = {false,false,false};
static bool  g_savedDrumsMute = false;


// --- Note gate / note-off scheduling (prevents stuck/overlong notes) ---




static bool g_voiceProgManual[NUM_VOICES] = {false,false,false};

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
static inline int cc127f(float x){
  if (x < 0.f) x = 0.f;
  if (x > 1.f) x = 1.f;
  return (int)std::lround(x * 127.f);
}

static inline void applyVoiceMixer(SynthOut& synth){
  if (!synth.enabled) return;
  for (int v=0; v<NUM_VOICES; ++v){
    int ch = v;
    float f = g_voiceMute[v] ? 0.f : g_voiceFader[v];
    float e = g_voiceMute[v] ? 0.f : g_voiceAuto[v];
    synth.cc(ch, 7,  cc127f(std::min(f, 1.f))); // Channel Volume
    synth.cc(ch, 11, cc127f(e)); // Expression (automation-friendly)
    // Portamento (MIDI): CC65 on/off, CC5 time
    float pn = std::clamp(g_porta01[v], 0.f, 1.f);
    int cc65 = (pn > 0.02f) ? 127 : 0;
    int cc5  = (int)std::lround(pn * 127.f);
    if (g_lastCC65[v] != cc65) { synth.cc(ch, 65, cc65); g_lastCC65[v]=cc65; }
    if (g_lastCC5[v]  != cc5 ) { synth.cc(ch, 5,  cc5 ); g_lastCC5[v]=cc5; }
  }
  // Drums on GM channel 9
  {
    int ch = 9;
    float f = g_drumsMute ? 0.f : g_drumsFader;
    float e = g_drumsMute ? 0.f : g_drumsAuto;
    synth.cc(ch, 7,  cc127f(std::min(f, 1.f)));
    synth.cc(ch, 11, cc127f(e));
  }
}


// ===== Animated audio coupling (viewport-driven automation) =====
struct ViewAudioMetrics {
  float water01 = 0.f;
  float plant01 = 0.f;
  float overlay01 = 0.f;
  float agent01 = 0.f;
  float agentSpeed01 = 0.f;
  float ripple01 = 0.f;
  float motion01 = 0.f;
  float centroidX01 = 0.5f; // 0..1
};

// Cheap sampler: measures what's visible (camera + zoom) so audio matches what you're looking at.
static inline ViewAudioMetrics computeViewAudioMetrics(const World& w) {
  ViewAudioMetrics out{};
  const int viewW = std::max(1, W / std::max(1, g_zoom));
  const int viewH = std::max(1, H / std::max(1, g_zoom));
  const int x0 = clampi(g_camX, 0, std::max(0, W - viewW));
  const int y0 = clampi(g_camY, 0, std::max(0, H - viewH));

  // Sample at a stride so it stays cheap even when zoomed out.
  const int stride = (g_zoom <= 1) ? 2 : 1; // when zoomed in, sample more densely
  int samples = 0;
  int water = 0, plant = 0, ov = 0, ent = 0;

  for (int sy = 0; sy < viewH; sy += stride) {
    int y = y0 + sy; if (y < 0 || y >= H) continue;
    for (int sx = 0; sx < viewW; sx += stride) {
      int x = x0 + sx; if (x < 0 || x >= W) continue;
      samples++;

      if (w.water[y][x] > 0) water++;
      char t = w.terrain[y][x];
      if (t==',' || t=='"' || t==';' || t=='f' || t=='F' || t=='p' || t=='y' || t=='Y') plant++;
      if (w.overlay[y][x] != ' ') ov++;
      if (w.entities[y][x] != ' ') ent++;
    }
  }
  if (samples > 0) {
    out.water01 = (float)water / (float)samples;
    out.plant01 = (float)plant / (float)samples;
    out.overlay01 = (float)ov / (float)samples;
    out.agent01 = std::clamp((float)ent / (float)samples * 1.8f, 0.f, 1.f); // boost a little
  }

  // Agent motion + centroid within view (uses real agent list, not entities grid)
  int agentsHere = 0;
  float cx = 0.f;
  float speedAccum = 0.f;
  static std::vector<std::pair<int,int>> s_prevPos; // sized lazily
  if (s_prevPos.size() != w.agents.size()) s_prevPos.assign(w.agents.size(), { -9999, -9999 });

  for (size_t i=0;i<w.agents.size();++i) {
    const auto& a = w.agents[i];
    if (a.x < x0 || a.x >= x0+viewW || a.y < y0 || a.y >= y0+viewH) continue;
    agentsHere++;
    cx += (float)(a.x - x0) / (float)std::max(1, viewW-1);

    auto [px,py] = s_prevPos[i];
    if (px != -9999) {
      speedAccum += (float)(std::abs(a.x - px) + std::abs(a.y - py));
    }
    s_prevPos[i] = {a.x,a.y};
  }
  if (agentsHere > 0) {
    out.centroidX01 = std::clamp(cx / (float)agentsHere, 0.f, 1.f);
    float avgStep = speedAccum / (float)agentsHere; // tiles per tick
    out.agentSpeed01 = std::clamp(avgStep / 1.2f, 0.f, 1.f);
    out.agent01 = std::clamp(out.agent01 + std::min(1.f, agentsHere / 18.f) * 0.5f, 0.f, 1.f);
  }

  // Ripples intersecting the view (gives a strong "I did that" audible response)
  float rippleEnergy = 0.f;
  for (const auto& rp : g_ripples) {
    if (rp.cx >= x0 && rp.cx < x0+viewW && rp.cy >= y0 && rp.cy < y0+viewH) rippleEnergy += 1.f;
    else {
      // If ring radius could pass through view, give a little weight
      float ring = rp.speed * rp.t;
      // distance from ripple center to view box (approx)
      float dx = 0.f;
      if (rp.cx < x0) dx = (float)(x0 - rp.cx);
      else if (rp.cx > x0+viewW-1) dx = (float)(rp.cx - (x0+viewW-1));
      float dy = 0.f;
      if (rp.cy < y0) dy = (float)(y0 - rp.cy);
      else if (rp.cy > y0+viewH-1) dy = (float)(rp.cy - (y0+viewH-1));
      float dist = std::sqrt(dx*dx + dy*dy);
      if (std::fabs(dist - ring) < rp.width * 2.5f) rippleEnergy += 0.5f;
    }
  }
  out.ripple01 = std::clamp(rippleEnergy / 3.f, 0.f, 1.f);

  // Motion = change in key fractions over time (very stable and correlates with visual "activity")
  static ViewAudioMetrics s_prev{};
  float dm = 0.f;
  dm += std::fabs(out.water01  - s_prev.water01);
  dm += std::fabs(out.plant01  - s_prev.plant01);
  dm += std::fabs(out.overlay01- s_prev.overlay01);
  dm += std::fabs(out.agent01  - s_prev.agent01);
  dm += 0.6f * std::fabs(out.agentSpeed01 - s_prev.agentSpeed01);
  dm += 0.9f * std::fabs(out.ripple01 - s_prev.ripple01);
  out.motion01 = std::clamp(dm * 2.4f, 0.f, 1.f);
  s_prev = out;

  return out;
}

// Applies animated expression (CC11) and subtle pan (CC10) so sound follows what's happening on screen.
// CC7 stays your manual fader (the "mixer"). CC11 is automation (movement / activity).
static inline void applyAnimatedAutomation(SynthOut& synth, const World& w, int tick) {
  if (!synth.enabled) return;

  const ViewAudioMetrics m = computeViewAudioMetrics(w);

  // Global "animation amount" - driven by motion and ripples.
  const float anim = std::clamp(0.25f + 0.65f*m.motion01 + 0.35f*m.ripple01, 0.f, 1.f);

  // Voice-specific expression curves:
  // v0: agents (motion + agent speed)
  // v1: plants/overlay shimmer
  // v2: water/bass movement
  g_voiceAuto[0] = std::clamp(0.45f + 0.55f*(0.55f*m.agent01 + 0.45f*m.agentSpeed01) + 0.35f*m.ripple01, 0.f, 1.f);
  g_voiceAuto[1] = std::clamp(0.40f + 0.60f*(0.55f*m.plant01 + 0.45f*m.overlay01) + 0.25f*m.motion01, 0.f, 1.f);
  g_voiceAuto[2] = std::clamp(0.35f + 0.65f*(0.75f*m.water01 + 0.25f*m.motion01), 0.f, 1.f);

  // Drums: follow motion + rain (when it's raining, keep a steady floor)
  const float rain01 = std::clamp((float)w.weather.state / 4.f, 0.f, 1.f);
  g_drumsAuto = std::clamp(0.25f + 0.55f*m.motion01 + 0.35f*m.ripple01 + 0.25f*rain01, 0.f, 1.f);

  // Also tighten note lengths when the scene is busy so it feels more "animated"
  // (uses your existing noteLen/holdChance knobs as the base)
  const float busy = std::clamp(0.15f + 0.85f*anim, 0.f, 1.f);
  // When busy, we want shorter notes: effective multiplier goes down (but keep user's intent).
  g_noteLenAutoMul = std::clamp((1.20f - 0.55f*busy), 0.45f, 1.35f);
  g_holdChanceAutoMul = std::clamp((1.15f - 0.85f*busy), 0.30f, 1.25f);

  // Pan (subtle): center of activity in the view
#ifdef USE_FLUIDSYNTH
  if ((tick & 3) == 0) { // CC throttling
    int pan = (int)std::lround(std::clamp(m.centroidX01, 0.f, 1.f) * 127.f);
    for (int v=0; v<NUM_VOICES; ++v) fluid_synth_cc(synth.synth, v, 10, pan);
    fluid_synth_cc(synth.synth, 9, 10, pan);
  }
#endif

  // Push mixer CCs occasionally (expression changes over time)
  if ((tick & 1) == 0) applyVoiceMixer(synth);
}





// ---- Scheduled note-off gate system (moved below SynthOut definition) ----
struct ActiveNote {
  int note = -1;
  int offTick = 0;
  bool on = false;
};
static ActiveNote g_activeNotes[16]; // indexed by MIDI channel

static inline int pickNoteDurationTicks(Rng& r) {
  // Base range in ticks; scaled by aleatoric controls.
  // At 60 TPS, 12 ticks ~ 200ms.
  int baseMin = 8;
  int baseMax = 42;
  float t = r.u01();
  int dur = (int)std::lround(baseMin + (baseMax - baseMin) * t);
  dur = (int)std::lround((float)dur * std::max(0.10f, g_alea.noteLen) * std::max(0.10f, g_noteLenAutoMul));
  if (r.u01() < std::clamp(g_alea.holdChance * g_holdChanceAutoMul, 0.0f, 1.0f)) dur *= 2;
  // Hard safety cap so nothing rings forever even if something goes wrong.
  return clampi(dur, 2, 240);
}

static inline void serviceScheduledNoteOffs(SynthOut& synth, int tick) {
  // Called every tick; sends note-offs when due.
  for (int ch=0; ch<16; ++ch) {
    if (g_activeNotes[ch].on && tick >= g_activeNotes[ch].offTick) {
      synth.noteOff(ch, g_activeNotes[ch].note, 0);
      g_activeNotes[ch].on = false;
      g_activeNotes[ch].note = -1;
    }
  }
}

static inline void gatedNoteOn(SynthOut& synth, Rng& r, int ch, int note, int vel, int tick, int durTicksOverride=-1) {
  // Turn off any currently held note on this channel to prevent stacking.
  if (g_activeNotes[ch].on) {
    synth.noteOff(ch, g_activeNotes[ch].note, 0);
    g_activeNotes[ch].on = false;
  }
  // Apply mixer headroom: CC7 caps at 1.0, so we scale velocity for fader > 1.
  float fader = 1.f;
  if (ch >= 0 && ch < NUM_VOICES) fader = g_voiceMute[ch] ? 0.f : g_voiceFader[ch];
  else if (ch == 9) fader = g_drumsMute ? 0.f : g_drumsFader;
  vel = (int)std::lround((float)vel * std::clamp(fader, 0.f, 2.f));
  vel = std::clamp(vel, 1, 127);
  synth.noteOn(ch, note, vel);
  int dur = (durTicksOverride > 0) ? durTicksOverride : pickNoteDurationTicks(r);
  g_activeNotes[ch].note = note;
  g_activeNotes[ch].offTick = tick + dur;
  g_activeNotes[ch].on = true;
}



// ---- Harmonic engine (chord progressions + arpeggios) ----
// This layer makes note generation musically coherent: voices pull from a shared chord/voicing
// which evolves with simulation metrics (stress/water/pred_pressure/mystic_flux/etc).

enum HarmonyStyle : uint8_t { HARM_DIATONIC=0, HARM_CINEMATIC=1, HARM_MODAL=2, HARM_FREE=3 };

enum ChordType : uint8_t {
  CH_MAJ7=0, CH_MIN9=1, CH_DOM7ALT=2, CH_SUS2=3, CH_SUS4=4, CH_QUARTAL=5, CH_DIM=6, CH_WHOLETONE=7, CH_SYMDIM=8
};

struct HarmonicState {
  int rootPc = 0;        // 0..11
  int degree = 0;        // 0..6 (scale degree, if diatonic-ish)
  ChordType chord = CH_MAJ7;
  int inversion = 0;     // 0..3
  float tension01 = 0.f; // 0..1
  HarmonyStyle style = HARM_DIATONIC;
  int lastChangeTick = 0;
};

// Global harmonic state
static HarmonicState g_harm;

// Simple chord templates as pitch-class offsets (may include extensions > 11; we mod 12 later)
static inline void chordPcs(ChordType ct, int* out, int& n){
  n=0;
  switch(ct){
    case CH_MAJ7:      { int a[]={0,4,7,11,14}; for(int x: a) out[n++]=x; } break; // add9
    case CH_MIN9:      { int a[]={0,3,7,10,14}; for(int x: a) out[n++]=x; } break;
    case CH_DOM7ALT:   { int a[]={0,4,7,10,13,17}; for(int x: a) out[n++]=x; } break; // b9 #11
    case CH_SUS2:      { int a[]={0,2,7,10,14}; for(int x: a) out[n++]=x; } break;
    case CH_SUS4:      { int a[]={0,5,7,10,14}; for(int x: a) out[n++]=x; } break;
    case CH_QUARTAL:   { int a[]={0,5,10,15,19}; for(int x: a) out[n++]=x; } break;
    case CH_DIM:       { int a[]={0,3,6,9,12,15}; for(int x: a) out[n++]=x; } break;
    case CH_WHOLETONE: { int a[]={0,2,4,6,8,10}; for(int x: a) out[n++]=x; } break;
    case CH_SYMDIM:    { int a[]={0,3,6,9,1,4,7,10}; for(int x: a) out[n++]=x; } break;
    default:           { int a[]={0,4,7,10}; for(int x: a) out[n++]=x; } break;
  }
}

static inline HarmonyStyle styleForBiome(Biome b){
  // "E": all styles depending on biome (and can be overridden by mystic/trickster signals)
  switch(b){
    case BIOME_MEADOW:   return HARM_DIATONIC;
    case BIOME_TROPICAL: return HARM_CINEMATIC;
    case BIOME_WETLAND:  return HARM_MODAL;
    case BIOME_ALPINE:   return HARM_MODAL;
    case BIOME_ALIEN:    return HARM_FREE;
    default:             return HARM_DIATONIC;
  }
}

static inline int wrapPc(int x){ x%=12; if(x<0) x+=12; return x; }

// Choose next chord based on sim metrics (spiky bipolar g_modVal)
static inline void updateHarmonyFromSim(const World& w, Rng& r, int tick, int rootKeyMidi, ScaleType st){
  const int barTicks = 96; // harmonic rate (slow enough to feel like progression)
  if (tick - g_harm.lastChangeTick < barTicks) return;

  // Read a few macro signals from mod pool (bipolar [-1,+1])
  float stress = std::fabs(g_modVal[5]);          // stress_mean
  float pred   = std::fabs(g_modVal[12]);         // pred_pressure
  float water  = std::fabs(g_modVal[0]);          // water_view
  float mystic = std::fabs(g_modVal[24]);         // mystic_flux
  float trick  = std::fabs(g_modVal[25]);         // trickster_mischief
  float ripple = std::fabs(g_modVal[15]);         // ripple_energy
  float tension = std::clamp(0.25f*stress + 0.25f*pred + 0.20f*mystic + 0.15f*trick + 0.15f*ripple, 0.f, 1.f);
  g_harm.tension01 = tension;

  HarmonyStyle base = styleForBiome((Biome)w.biome);
  // Let "mystic/trickster" push towards free-jazz
  if (mystic > 0.55f || trick > 0.60f) base = HARM_FREE;
  else if (water > 0.55f && base != HARM_FREE) base = HARM_MODAL;
  g_harm.style = base;

  int rootPcBase = wrapPc(rootKeyMidi);
  if (g_harm.lastChangeTick == 0){
    g_harm.rootPc = rootPcBase;
  }

  // Root motion: fifths/seconds for diatonic/cinematic, tritones & symmetric for free.
  int step=0;
  float u=r.u01();
  if (g_harm.style == HARM_FREE){
    // Bigger leaps, symmetric motion
    int choices[] = {6, 3, -3, 1, -1, 5, -5};
    step = choices[(int)std::floor(u*7.0f)];
  } else if (g_harm.style == HARM_MODAL){
    int choices[] = {0, 2, -2, 5, -5, 7, -7};
    step = choices[(int)std::floor(u*7.0f)];
  } else {
    int choices[] = {0, 2, -2, 5, -5, 7, -7, 9, -9}; // includes relative-ish move
    step = choices[(int)std::floor(u*9.0f)];
  }
  // Tension nudges towards more motion
  if (tension > 0.7f && r.u01() < 0.5f) step += (r.u01()<0.5f)?1:-1;

  g_harm.rootPc = wrapPc(g_harm.rootPc + step);

  // Chord type selection by style + tension
  float p = r.u01();
  if (g_harm.style == HARM_DIATONIC){
    g_harm.chord = (p < 0.45f) ? CH_MAJ7 : (p < 0.85f ? CH_MIN9 : CH_SUS2);
  } else if (g_harm.style == HARM_CINEMATIC){
    g_harm.chord = (p < 0.30f) ? CH_SUS4 : (p < 0.65f ? CH_MAJ7 : (p < 0.90f ? CH_MIN9 : CH_QUARTAL));
  } else if (g_harm.style == HARM_MODAL){
    g_harm.chord = (p < 0.35f) ? CH_SUS2 : (p < 0.70f ? CH_SUS4 : CH_QUARTAL);
  } else { // FREE
    g_harm.chord = (p < 0.20f) ? CH_WHOLETONE : (p < 0.45f ? CH_SYMDIM : (p < 0.70f ? CH_DOM7ALT : CH_DIM));
  }

  // Inversion / voicing movement: more tension => more inversion changes
  if (r.u01() < (0.15f + 0.60f*tension)) g_harm.inversion = (g_harm.inversion + 1 + (int)(r.u01()*2.0f)) % 4;

  g_harm.lastChangeTick = tick;
}

// Build a sorted list of chord pitch-classes for current state (0..11)
static inline int buildChordPcList(int pcsOut[12]){
  int tmp[12]; int n=0;
  chordPcs(g_harm.chord, tmp, n);
  for(int i=0;i<n;i++) pcsOut[i]=wrapPc(g_harm.rootPc + tmp[i]);
  // Sort and unique (small n)
  for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(pcsOut[j]<pcsOut[i]) std::swap(pcsOut[i],pcsOut[j]);
  int m=0;
  for(int i=0;i<n;i++){ if(i==0 || pcsOut[i]!=pcsOut[i-1]) pcsOut[m++]=pcsOut[i]; }
  return m;
}

// Per-voice arp state
struct ArpState { int step=0; int dir=1; uint8_t mode=0; int lastNote=-1; };
static ArpState g_arp[NUM_VOICES];

// Pick next midi note for voice v from chord tones, with pattern differences per voice.
static inline int pickArpNoteForVoice(int v, const World& w, Rng& r, int tick, int rootKeyMidi){
  int pcs[12]; int n=buildChordPcList(pcs);
  if(n<=0) return rootKeyMidi;

  // Different "clock divisions" per voice so they don't all line up.
  int div = (v==0)?5:((v==1)?7:4);
  if ((tick % div) != 0) return g_arp[v].lastNote>=0 ? g_arp[v].lastNote : rootKeyMidi;

  // Mode per style: 0 up,1 down,2 pingpong,3 random-walk
  uint8_t mode = 0;
  if (g_harm.style==HARM_MODAL) mode = (v==2)?0:2;
  else if (g_harm.style==HARM_CINEMATIC) mode = (v==0)?2:0;
  else if (g_harm.style==HARM_FREE) mode = (v==0)?3:((v==1)?3:0);
  g_arp[v].mode = mode;

  int idx = g_arp[v].step;
  if (mode==0){ idx = g_arp[v].step % n; g_arp[v].step++; }
  else if (mode==1){ idx = (n-1) - (g_arp[v].step % n); g_arp[v].step++; }
  else if (mode==2){
    // pingpong
    idx = g_arp[v].step;
    if (idx>=n-1){ g_arp[v].dir=-1; }
    if (idx<=0){ g_arp[v].dir=+1; }
    idx = std::clamp(idx,0,n-1);
    g_arp[v].step += g_arp[v].dir;
  } else {
    // random walk inside chord
    int jump = (r.u01()<0.7f)?1:2;
    g_arp[v].step += (r.u01()<0.5f? -jump: jump);
    if (g_arp[v].step<0) g_arp[v].step += n*4;
    idx = g_arp[v].step % n;
  }

  // Register mapping: bass lower, others mid/high; tension raises register a bit
  int baseOct = (v==2)?2: (v==1?3:4);
  if (g_harm.style==HARM_FREE && v!=2) baseOct += 1;
  int octLift = (int)std::lround(g_harm.tension01 * ((v==2)?1.0f:2.0f));
  int midi = 12*(baseOct+octLift) + pcs[idx];

  // Add color tones occasionally for FREE/tense moments
  if (g_harm.style==HARM_FREE && r.u01() < (0.20f + 0.30f*g_harm.tension01)){
    midi += (r.u01()<0.5f)?1:-1;
  }
  g_arp[v].lastNote = midi;
  return midi;
}

// ---- Built-in synth music driver ----
// Called whenever the simulation advances a tick (realtime or single-step).
static inline void synthTickMusic(SynthOut& synth, const World& world, Rng& r, int tick,
                                 int& heldNote, int& heldNote2, int& heldNote3, int rootKey, ScaleType scaleType,
                                 const std::vector<MidiParam>& params)
{
  if (!synth.enabled) return;
  // Always service scheduled note-offs first (prevents stuck/overlong notes)
  serviceScheduledNoteOffs(synth, tick);

  // Per-tick CC automation (expression/brightness/pan/portamento) driven by MODMAP
  // CC7 faders are set in applyVoiceMixer (menu changes); CC11 is animated here.
  for (int v=0; v<NUM_VOICES; ++v){
    int ch=v;
    float expr = std::clamp(g_voiceMute[v]?0.f:(g_voiceAuto[v]*g_cc11Expr), 0.f, 1.f);
    int cc11 = cc127f(expr);
    if (g_lastCC11[v] != cc11) { synth.cc(ch, 11, cc11); g_lastCC11[v]=cc11; }

    int cc74 = cc127f(g_cc74Bright);
    if (g_lastCC74[v] != cc74) { synth.cc(ch, 74, cc74); g_lastCC74[v]=cc74; }

    int cc10 = (int)std::lround(std::clamp(g_pan01,0.f,1.f)*127.f);
    if (g_lastCC10[v] != cc10) { synth.cc(ch, 10, cc10); g_lastCC10[v]=cc10; }

    float pn = std::clamp(g_porta01[v], 0.f, 1.f);
    int cc65 = (pn > 0.02f) ? 127 : 0;
    int cc5  = (int)std::lround(pn * 127.f);
    if (g_lastCC65[v] != cc65) { synth.cc(ch, 65, cc65); g_lastCC65[v]=cc65; }
    if (g_lastCC5[v]  != cc5 ) { synth.cc(ch, 5,  cc5 ); g_lastCC5[v]=cc5; }
  }

  // Apply per-voice programs (cached) for melodic channels 0..NUM_VOICES-1
#ifdef USE_FLUIDSYNTH
  {
    static int lastProg[NUM_VOICES] = {-1,-1,-1};
    static int lastMSB[NUM_VOICES]  = {-1,-1,-1};
    static int lastLSB[NUM_VOICES]  = {-1,-1,-1};
    for (int v=0; v<NUM_VOICES; ++v) {
      int ch = v;
      if (g_voice[v].bankMSB != lastMSB[v]) { fluid_synth_cc(synth.synth, ch, 0, g_voice[v].bankMSB); lastMSB[v]=g_voice[v].bankMSB; }
      if (g_voice[v].bankLSB != lastLSB[v]) { fluid_synth_cc(synth.synth, ch, 32, g_voice[v].bankLSB); lastLSB[v]=g_voice[v].bankLSB; }
      if (g_voice[v].program != lastProg[v]) { fluid_synth_program_change(synth.synth, ch, g_voice[v].program); lastProg[v]=g_voice[v].program; }
    }
  }
#endif
  // Viewport-driven expression/pan automation so audio follows on-screen motion
  applyAnimatedAutomation(synth, world, tick);


  // --- Instrument selection (bell/chime/light palette) ---
// Default timbre now varies by biome (each biome gets its own “instrument palette”),
// while still allowing an explicit override via MIDI param "Instr" (0..1).
  static int currentProgram = -1;
  static int nextAutoChangeTick = 0;

  auto chooseFromPaletteGlobal = [&](float x01)->int {
    // Full GM program range 0..127 so you can access *all* instruments in your SF2.
    // x01 chooses a program deterministically from the range.
    int idx = (int)std::floor(std::clamp(x01, 0.0f, 0.9999f) * 128.0f);
    idx = std::clamp(idx, 0, 127);
    return idx;
  };

  auto chooseFromBiomePalette = [&](Biome b, float x01)->int {
    // Still give each biome a "personality" by rotating the program space.
    int idx = (int)std::floor(std::clamp(x01, 0.0f, 0.9999f) * 128.0f);
    idx = std::clamp(idx, 0, 127);
    int shift = (int(b) * 17) % 128; // co-prime-ish rotation
    return (idx + shift) & 127;
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
// --- Micro-events: trigger notes on agent steps so sound matches motion ---
if (!g_stepEvents.empty()) {
  // Use existing automation energy as a guide for density/shortness
  float energy = std::max(g_voiceAuto[0], std::max(g_voiceAuto[1], g_voiceAuto[2]));
  int maxEv = (energy > 0.75f) ? 12 : (energy > 0.45f ? 8 : 5);
  // Scale interval tables
  // Update harmonic progression state from simulation (E: biome-dependent styles + free-jazz pushes)
  updateHarmonyFromSim(world, r, tick, rootKeyUsed, scaleUsed);

  static const int MAJ[]  = {0,2,4,5,7,9,11};
  static const int MINR[] = {0,2,3,5,7,8,10};
  static const int PENT[] = {0,3,5,7,10};
  static const int DOR[]  = {0,2,3,5,7,9,10};
  static const int LYD[]  = {0,2,4,6,7,9,11};
  static const int WHOLE[]= {0,2,4,6,8,10};
  auto pickInterval = [&](int deg)->int{
    switch (scaleUsed) {
      case SCALE_MAJOR:      return MAJ[deg % 7];
      case SCALE_MINOR:      return MINR[deg % 7];
      case SCALE_PENTATONIC: return PENT[deg % 5];
      case SCALE_DORIAN:     return DOR[deg % 7];
      case SCALE_LYDIAN:     return LYD[deg % 7];
      case SCALE_WHOLE:      return WHOLE[deg % 6];
      case SCALE_CHROMATIC:  default: return (deg % 12);
    }
  };
  int used = 0;
  for (int i=0; i<(int)g_stepEvents.size() && used < maxEv; ++i) {
    const StepEvent &ev = g_stepEvents[i];
    // Thin events a bit when calm
    if (energy < 0.35f && (r.u32() % 3) != 0) continue;
    // Distribute step-events across voices so they don't all hit together.
    // Base "family" from glyph type, then hashed into the available melodic channels.
    int family = 0;
    if (isPredator(ev.glyph)) family = 1;
    else if (isAquatic(ev.glyph) || isBird(ev.glyph)) family = 2;
    uint32_t h = (uint32_t)(ev.x*73856093u) ^ (uint32_t)(ev.y*19349663u) ^ (uint32_t)(tick*83492791u) ^ (uint32_t)(family*2654435761u);
    int ch = (int)(h % (uint32_t)NUM_VOICES);

    // Per-voice rhythmic phase: each channel has a different subdivision/offset.
    static const int divs[3] = {4,5,7};
    static const int offs[3] = {0,2,3};
    if (((tick + offs[ch]) % divs[ch]) != 0) continue;
    // Map movement + position to pitch
    int deg = (ev.x + ev.y + tick) & 255;
    deg += (ev.dx>0) - (ev.dx<0);
    deg += 2*((ev.dy<0) - (ev.dy>0)); // up => higher
    int interval = pickInterval(deg);
    // Octave: top of screen slightly higher
    int oct = 3 + (int)std::lround((float)(H-1-ev.y) / std::max(1.f, (float)H) * 2.f); // 3..5
    int note = 12*oct + (rootKeyUsed % 12) + interval;
    // Velocity from step strength + energy
    int stepMag = std::abs(ev.dx) + std::abs(ev.dy);
    float v01 = std::clamp(0.25f + 0.35f*(float)stepMag + 0.40f*energy, 0.f, 1.f);
    int vel = (int)std::lround(20 + v01*95);
    // Short, snappy gates for animation (busy => shorter)
    int dur = (energy > 0.7f) ? 4 : (energy > 0.45f ? 6 : 8);
    // Route through per-voice range clamp inside gatedNoteOn path
    gatedNoteOn(synth, r, ch, note, vel, tick, dur);
    // Occasional drum accents on big moves / panic
    if (!g_drumsMute && (ev.strength > 1.5f) && (r.u01() < (0.20f + 0.25f*energy))) {
      int drum = (r.u01() < 0.6f) ? 36 : 38; // kick/snare
      gatedNoteOn(synth, r, 9, drum, (int)std::lround(55 + 40*energy), tick, 3);
    }
    used++;
  }
  g_stepEvents.clear();
}

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

int interval = pat[phrasePos % 9];
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

  {
    auto &vs = g_voice[0];
    int note = heldNote + vs.transpose;
    note = clampi(note, vs.minNote, vs.maxNote);
    int vel = (int)std::lround((float)v1 * vs.velMul);
    vel = clampi(vel, 1, 127);
    heldNote = note;
    gatedNoteOn(synth, r, 0, heldNote, vel, tick);
}
  // Bass companion (voice 2): sparse, lower octave
  if (heldNote3 < 0 || tick >= holdUntil1) {
    int bn = heldNote - 12;
    auto &vs = g_voice[2];
    bn = clampi(bn + vs.transpose, vs.minNote, vs.maxNote);
    int bvel = clampi((int)std::lround(0.75f * (float)v1 * vs.velMul), 1, 110);
    heldNote3 = bn;
    gatedNoteOn(synth, r, 2, heldNote3, bvel, tick);
// release a bit sooner than melody
    // reuse holdUntil1 window as a guide
  }


// Optional harmony note
if (r.u01() < harmP) {
  heldNote2 = std::clamp(n2, 36, 96);
  lastHeld2 = heldNote2;
  holdUntil2 = tick + hold2;
  int v2 = std::clamp(v1 - (10 + (int)std::lround(8.0f*activity)), 18, 78);
    {
    auto &vs = g_voice[1];
    int note = heldNote2 + vs.transpose;
    note = clampi(note, vs.minNote, vs.maxNote);
    int vel = clampi((int)std::lround((float)v2 * vs.velMul), 1, 127);
    heldNote2 = note;
    gatedNoteOn(synth, r, 1, heldNote2, vel, tick);
}

}

// Percussion accents: now less repetitive; driven by rain/wind + a little randomness.
// Keep it magical and sparse.
if (world.weather.state == STORM && r.u01() < (0.08f + 0.12f*wind01)) gatedNoteOn(synth, r, 9, 80, 70, tick, 4);
// mute triangle-ish
if (world.weather.state == RAIN  && r.u01() < (0.05f + 0.08f*rain01)) gatedNoteOn(synth, r, 9, 81, 55, tick, 4);
// open triangle-ish

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

  int viewW = std::max(1, W / std::max(1, g_zoom));
  int viewH = std::max(1, H / std::max(1, g_zoom));
  clampCameraToWorld();

  updateModPool(w, tick, viewW, viewH);

  applyModMatrix();
for (int y=0; y<viewH; ++y) {
    int wy = g_camY + y;
        int y0 = (y * L.simHpx) / viewH;
        int y1 = ((y+1) * L.simHpx) / viewH;
    int hpx = std::max(1, y1 - y0);

    for (int x=0; x<viewW; ++x) {
      int wx = g_camX + x;
      int x0 = (x * L.screenW) / viewW;
      int x1 = ((x+1) * L.screenW) / viewW;
      int wpx = std::max(1, x1 - x0);

      SDL_Rect rc{ x0, y0, wpx, hpx };

      RGB bg = baseBgFor(w, wx, wy, tick, s, sp);
      uint8_t cloud = sampleCloud(w.clouds, wx, wy);
      // Biome-tune clouds: tropical clouds should be lighter/smaller coverage.
      cloud = (uint8_t)std::min<int>(255, (int)(cloud * w.cloudOpacity));
      if (w.biome==TROPICAL) cloud = (uint8_t)std::max<int>(0, (int)cloud - 35);
      if (w.biome==DESERT)   cloud = (uint8_t)std::max<int>(0, (int)cloud - 25);

      applyCloudShadow(bg, cloud);

      setColor(ren, bg.r, bg.g, bg.b);
      SDL_RenderFillRect(ren, &rc);

      char c = renderCharAt(w, wx, wy, tick);

      if (c=='.' && w.water[wy][wx]==0 && w.entities[wy][wx]==' ' && w.overlay[wy][wx]==' ') {
        applyCloudLayer(ren, rc, cloud);
        continue;
      }

      if (w.entities[wy][wx]==' ' && w.overlay[wy][wx]==' ' && w.water[wy][wx]==0) {
        uint32_t h = hash3((uint32_t)wx, (uint32_t)wy, (uint32_t)(tick/6));
        c = terrainGlyphVariant(c, h, s, w.weather);
      }

      SDL_Texture* gt = gcWorld.get(ren, (unsigned char)c);
      if (gt) {
        RGB fg = fgForChar(w, c, s, sp, tick, x, y);

        if ((w.terrain[wy][wx]==',' || w.terrain[wy][wx]=='"') && w.wind.strength>0) {
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
    static const char* EN4="VOICES";
    switch (p % 7) {
      case 0: return EN0; // keep MIDI in Latin
      case 1: return (uiLang==UI_KATA) ? KATA_MIZU  : EN1;
      case 2: return (uiLang==UI_KATA) ? KATA_SPAWN : EN2;
      case 3: return (uiLang==UI_KATA) ? KATA_OTO   : EN3;
      case 4: return EN4;
      case 5: return "MIXER";
      case 6: return "INSPECT";
      case 7: return "MODS";
      case 8: return "MODMAP";
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

  // Flavor: The Legendary Couple (always present)
  int legA=0, legB=0;
  for (const auto& a : w.agents){ if(a.flags & AGF_LEGEND_A) legA++; if(a.flags & AGF_LEGEND_B) legB++; }
  snprintf(buf, sizeof(buf), "Legendary Couple: %s%s  (click ripples to stir fate)",
           (legA? "y/Y ":"(missing A) "),
           (legB? "z/Z":"(missing B)"));
  drawString(ren, gcText, tx, ty, buf, 240,210,140, 230, L.scale);
  ty += 10*L.scale;

  
  if ((menuPage%9)==0) {
    drawString(ren, gcText, tx, ty, "MIDI/CC (UP/DOWN select, +/- edit, O MIDI, C clock, V clock src, K key, S scale, M menu)", 180,180,180, 220, L.scale);
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
  } else if ((menuPage%9)==1) {
    // CHAOS / aleatoric weights
    drawString(ren, gcText, tx, ty, "CHAOS WEIGHTS (UP/DOWN select, +/- adjust)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;

    struct Row { const char* name; float* v; float lo; float hi; };
    Row rows[] = {
      {"Chaos",       &g_alea.chaos,       0.0f, 2.0f},
      {"RainChance",  &g_alea.rainChance,  0.0f, 2.0f},
      {"SpawnChance", &g_alea.spawnChance, 0.0f, 2.0f},
      {"Mutation",    &g_alea.mutationRate,0.0f, 2.0f},
      {"Drift",       &g_alea.drift,       0.0f, 2.0f},
      {"NoteLen",    &g_alea.noteLen,     0.10f, 2.50f},
      {"HoldChance", &g_alea.holdChance,  0.00f, 0.50f},
    };
    int n = (int)(sizeof(rows)/sizeof(rows[0]));
    int sel = clampi(menuSel, 0, n-1);
    for (int i=0;i<n;i++){
      int rr = (i==sel)?255:200;
      int gg = (i==sel)?240:200;
      int bb = (i==sel)?210:200;
      snprintf(buf, sizeof(buf), "%s  %.2f", rows[i].name, *rows[i].v);
      drawString(ren, gcText, tx, ty + i*9*L.scale, buf, (uint8_t)rr,(uint8_t)gg,(uint8_t)bb, 230, L.scale);
      int bx = tx + 240*L.scale;
      int by = ty + i*9*L.scale;
      int barW = 200*L.scale;
      int barH = 6*L.scale;
      SDL_Rect bar{ bx, by + 2*L.scale, barW, barH };
      SDL_SetRenderDrawColor(ren, 60,60,60, 200);
      SDL_RenderFillRect(ren, &bar);
      float t = (*rows[i].v - rows[i].lo) / (rows[i].hi - rows[i].lo + 1e-6f);
      SDL_Rect fill = bar;
      fill.w = (int)(barW * clamp01(t));
      SDL_SetRenderDrawColor(ren, rr,gg,bb, 200);
      SDL_RenderFillRect(ren, &fill);
    }

    snprintf(buf, sizeof(buf), "Water tiles:%d  shallow:%d  deep:%d  (Scroll=zoom, click=ripple, WASD pan when zoomed)", waterTiles, shallow, deep);
    drawString(ren, gcText, tx, ty + n*9*L.scale + 2*L.scale, buf, 180,190,200, 220, L.scale);

  } else if ((menuPage%9)==2) {
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
  } else if ((menuPage%9)==3) {
    snprintf(buf, sizeof(buf), "SoundFont: %s", sf2Path.empty() ? "(none)" : sf2Path.c_str());
    drawString(ren, gcText, tx, ty, buf, 220,220,220, 230, L.scale);
    ty += 10*L.scale;
    drawString(ren, gcText, tx, ty, "Use: --synth --sf2 <path.sf2> --gain <0..2>  (compile with -DUSE_FLUIDSYNTH)", 180,180,180, 220, L.scale);
  } else if ((menuPage%9)==4) { // page 4: VOICES
    drawString(ren, gcText, tx, ty, "VOICES (UP/DOWN select, +/- adjust: Prog/Min/Max)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;

    int rows = NUM_VOICES * 3; // prog,min,max per voice
    int sel = clampi(menuSel, 0, rows-1);

    auto gmName = [&](int p)->const char*{
      static const char* GM[128] = {
        "Acoustic Grand Piano",
        "Bright Acoustic Piano",
        "Electric Grand Piano",
        "Honky-tonk Piano",
        "Electric Piano 1",
        "Electric Piano 2",
        "Harpsichord",
        "Clavi",
        "Celesta",
        "Glockenspiel",
        "Music Box",
        "Vibraphone",
        "Marimba",
        "Xylophone",
        "Tubular Bells",
        "Dulcimer",
        "Drawbar Organ",
        "Percussive Organ",
        "Rock Organ",
        "Church Organ",
        "Reed Organ",
        "Accordion",
        "Harmonica",
        "Tango Accordion",
        "Acoustic Guitar (nylon)",
        "Acoustic Guitar (steel)",
        "Electric Guitar (jazz)",
        "Electric Guitar (clean)",
        "Electric Guitar (muted)",
        "Overdriven Guitar",
        "Distortion Guitar",
        "Guitar harmonics",
        "Acoustic Bass",
        "Electric Bass (finger)",
        "Electric Bass (pick)",
        "Fretless Bass",
        "Slap Bass 1",
        "Slap Bass 2",
        "Synth Bass 1",
        "Synth Bass 2",
        "Violin",
        "Viola",
        "Cello",
        "Contrabass",
        "Tremolo Strings",
        "Pizzicato Strings",
        "Orchestral Harp",
        "Timpani",
        "String Ensemble 1",
        "String Ensemble 2",
        "Synth Strings 1",
        "Synth Strings 2",
        "Choir Aahs",
        "Voice Oohs",
        "Synth Choir",
        "Orchestra Hit",
        "Trumpet",
        "Trombone",
        "Tuba",
        "Muted Trumpet",
        "French Horn",
        "Brass Section",
        "Synth Brass 1",
        "Synth Brass 2",
        "Soprano Sax",
        "Alto Sax",
        "Tenor Sax",
        "Baritone Sax",
        "Oboe",
        "English Horn",
        "Bassoon",
        "Clarinet",
        "Piccolo",
        "Flute",
        "Recorder",
        "Pan Flute",
        "Blown Bottle",
        "Shakuhachi",
        "Whistle",
        "Ocarina",
        "Lead 1 (square)",
        "Lead 2 (sawtooth)",
        "Lead 3 (calliope)",
        "Lead 4 (chiff)",
        "Lead 5 (charang)",
        "Lead 6 (voice)",
        "Lead 7 (fifths)",
        "Lead 8 (bass+lead)",
        "Pad 1 (new age)",
        "Pad 2 (warm)",
        "Pad 3 (polysynth)",
        "Pad 4 (choir)",
        "Pad 5 (bowed)",
        "Pad 6 (metallic)",
        "Pad 7 (halo)",
        "Pad 8 (sweep)",
        "FX 1 (rain)",
        "FX 2 (soundtrack)",
        "FX 3 (crystal)",
        "FX 4 (atmosphere)",
        "FX 5 (brightness)",
        "FX 6 (goblins)",
        "FX 7 (echoes)",
        "FX 8 (sci-fi)",
        "Sitar",
        "Banjo",
        "Shamisen",
        "Koto",
        "Kalimba",
        "Bag pipe",
        "Fiddle",
        "Shanai",
        "Tinkle Bell",
        "Agogo",
        "Steel Drums",
        "Woodblock",
        "Taiko Drum",
        "Melodic Tom",
        "Synth Drum",
        "Reverse Cymbal",
        "Guitar Fret Noise",
        "Breath Noise",
        "Seashore",
        "Bird Tweet",
        "Telephone Ring",
        "Helicopter",
        "Applause",
        "Gunshot"
      };
      if (p<0 || p>127) return "";
      return GM[p];
    };

    for (int v=0; v<NUM_VOICES; ++v) {
      int base = v*3;
      // Program
      {
        int i = base+0;
        int rr=(i==sel)?255:200, gg=(i==sel)?240:200, bb=(i==sel)?210:200;
        snprintf(buf,sizeof(buf),"V%d Program: %d %s", v, g_voice[v].program, gmName(g_voice[v].program));
        drawString(ren,gcText,tx, ty + i*9*L.scale, buf, rr,gg,bb,230,L.scale);
      }
      // Min
      {
        int i = base+1;
        int rr=(i==sel)?255:200, gg=(i==sel)?240:200, bb=(i==sel)?210:200;
        snprintf(buf,sizeof(buf),"V%d MinNote: %d", v, g_voice[v].minNote);
        drawString(ren,gcText,tx, ty + i*9*L.scale, buf, rr,gg,bb,230,L.scale);
      }
      // Max
      {
        int i = base+2;
        int rr=(i==sel)?255:200, gg=(i==sel)?240:200, bb=(i==sel)?210:200;
        snprintf(buf,sizeof(buf),"V%d MaxNote: %d", v, g_voice[v].maxNote);
        drawString(ren,gcText,tx, ty + i*9*L.scale, buf, rr,gg,bb,230,L.scale);
      }
    }
    drawString(ren, gcText, tx, ty + rows*9*L.scale + 2*L.scale, "Drums are on ch9 (weather-triggered).", 180,180,180, 220, L.scale);
  } else if ((menuPage%9)==5) { // page 5: MIXER
    drawString(ren, gcText, tx, ty, "MIXER (UP/DOWN select, +/- level, M mute, S solo)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;

    int rows = NUM_VOICES + 1; // voices + drums
    int sel = clampi(menuSel, 0, rows-1);

    for (int v=0; v<NUM_VOICES; ++v) {
      int i = v;
      int rr=(i==sel)?255:200, gg=(i==sel)?240:200, bb=(i==sel)?210:200;
      float lvl = g_voiceMute[v] ? 0.f : g_voiceFader[v];
      snprintf(buf, sizeof(buf), "V%d Level: %.2f%s%s", v, lvl, g_voiceMute[v]?" (MUTE)":"", (g_soloRow==v)?" (SOLO)":"");
      drawString(ren, gcText, tx, ty + i*9*L.scale, buf, rr,gg,bb, 230, L.scale);

      int bx = tx + 240*L.scale;
      int by = ty + i*9*L.scale;
      int barW = 200*L.scale;
      int barH = 6*L.scale;
      SDL_Rect bar{ bx, by + 2*L.scale, barW, barH };
      SDL_SetRenderDrawColor(ren, 60,60,60, 200);
      SDL_RenderFillRect(ren, &bar);
      SDL_Rect fill = bar;
      fill.w = (int)(barW * clamp01(lvl));
      SDL_SetRenderDrawColor(ren, rr,gg,bb, 200);
      SDL_RenderFillRect(ren, &fill);
    }
    // Drums row (index NUM_VOICES)
    {
      int i = NUM_VOICES;
      int rr=(i==sel)?255:200, gg=(i==sel)?240:200, bb=(i==sel)?210:200;
      float lvl = g_drumsMute ? 0.f : g_drumsFader;
      snprintf(buf, sizeof(buf), "DRUMS Level: %.2f%s%s", lvl, g_drumsMute?" (MUTE)":"", (g_soloRow==NUM_VOICES)?" (SOLO)":"");
      drawString(ren, gcText, tx, ty + i*9*L.scale, buf, rr,gg,bb, 230, L.scale);

      int bx = tx + 240*L.scale;
      int by = ty + i*9*L.scale;
      int barW = 200*L.scale;
      int barH = 6*L.scale;
      SDL_Rect bar{ bx, by + 2*L.scale, barW, barH };
      SDL_SetRenderDrawColor(ren, 60,60,60, 200);
      SDL_RenderFillRect(ren, &bar);
      SDL_Rect fill = bar;
      fill.w = (int)(barW * clamp01(lvl));
      SDL_SetRenderDrawColor(ren, rr,gg,bb, 200);
      SDL_RenderFillRect(ren, &fill);
    }

    drawString(ren, gcText, tx, ty + rows*9*L.scale + 2*L.scale, "Tip: use solo to tune mappings, then blend voices live.", 180,180,180, 220, L.scale);

  } else if ((menuPage%9)==6) { // page 6: INSPECT (DF-ish statblocks)
    drawString(ren, gcText, tx, ty, "INSPECT (click an agent, or UP/DOWN select; F follow)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;
    int nA = (int)w.agents.size();
    if (nA <= 0) {
      drawString(ren, gcText, tx, ty, "(no agents)", 200,200,200, 230, L.scale);
    } else {
      int sel = clampi(g_inspectIdx, 0, nA-1);
      g_inspectIdx = sel;
      const Agent& a = w.agents[sel];
      char hdr[256];
      snprintf(hdr, sizeof(hdr), "#%d '%c'  (%d,%d)  %s", sel, a.glyph, a.x, a.y, speciesName(a.species));
      drawString(ren, gcText, tx, ty, hdr, 240,240,210, 230, L.scale);
      ty += 10*L.scale;
      char st[256];
      snprintf(st, sizeof(st), "hp %3d  stress %3d  hunger %3d  thirst %3d  fatigue %3d",
               (int)std::lround(100.f*a.health), (int)std::lround(100.f*a.stress),
               (int)std::lround(100.f*a.hunger), (int)std::lround(100.f*a.thirst),
               (int)std::lround(100.f*a.fatigue));
      drawString(ren, gcText, tx, ty, st, 210,210,220, 230, L.scale);
      ty += 10*L.scale;
      // roster list
      int maxRows = 14;
      int start = std::max(0, sel - maxRows/2);
      start = clampi(start, 0, std::max(0, nA - maxRows));
      for (int i=0; i<maxRows && (start+i)<nA; ++i) {
        int idx = start+i;
        const Agent& q = w.agents[idx];
        int rr = (idx==sel)?255:180;
        int gg = (idx==sel)?240:180;
        int bb = (idx==sel)?210:180;
        char buf[256];
        snprintf(buf,sizeof(buf),"%4d '%c' (%3d,%3d) %s st%3d hu%3d th%3d",
                 idx,q.glyph,q.x,q.y, speciesName(q.species),
                 (int)std::lround(100.f*q.stress), (int)std::lround(100.f*q.hunger), (int)std::lround(100.f*q.thirst));
        drawString(ren, gcText, tx, ty + i*9*L.scale, buf, rr,gg,bb, 230, L.scale);
      }
    }

  } else if ((menuPage%9)==7) { // page 7: MODS
    drawString(ren, gcText, tx, ty, "MODS (50 spiky bipolar signals)  (UP/DOWN scroll)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;
    g_g_modScroll = std::clamp(g_g_modScroll, 0, std::max(0, MOD_N-14));
    for (int i=0;i<14;++i) {
      int mi = g_g_modScroll + i;
      char ln[128];
      snprintf(ln, sizeof(ln), "%2d %-14s %+.3f", mi, g_modName[mi], g_modVal[mi]);
      drawString(ren, gcText, tx, ty + i*10*L.scale, ln, 210,210,220, 230, L.scale);
    }

  } else if ((menuPage%9)==8) { // page 8: MODMAP
    drawString(ren, gcText, tx, ty, "MODMAP (UP/DOWN slot, LEFT/RIGHT field, +/- edit, E enable)", 180,180,180, 220, L.scale);
    ty += 10*L.scale;
    g_g_mmSel = clampi(g_g_mmSel, 0, MOD_SLOTS-1);
    g_g_mmField = clampi(g_g_mmField, 0, 3);
    for (int i=0;i<MOD_SLOTS;++i) {
      const ModMap& mm = g_modMap[i];
      char ln[196];
      snprintf(ln, sizeof(ln), "%2d %c src:%02d %-12s amt:%+0.2f sm:%0.2f",
               i, mm.enabled?'*':' ', mm.src, modDestName(mm.dest), mm.amt, mm.smooth);
      drawString(ren, gcText, tx, ty + i*10*L.scale, ln, (i==g_g_mmSel)?255:200, (i==g_g_mmSel)?255:200, 220, 230, L.scale);
      if (i==g_g_mmSel) {
        int caretX = tx + (g_g_mmField==0?30:(g_g_mmField==1?92:(g_g_mmField==2?170:230)))*L.scale/2;
        drawString(ren, gcText, caretX, ty + i*10*L.scale, "^", 255,220,120, 230, L.scale);
      }
    }
    drawString(ren, gcText, tx, ty + MOD_SLOTS*10*L.scale + 4*L.scale,
               "Map MODS -> CC11/CC74/Pan/Porta. Values are bipolar & spiky.", 150,200,255, 220, L.scale);
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
  // Initialize mixer CCs (per-voice faders/expression)
  applyVoiceMixer(synth);


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
int heldNote3 = -1;
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
case SDLK_TAB: if (showMenu) menuPage = (menuPage + 1) % 9; break;
case SDLK_f: if (showMenu && (menuPage%9)==6) { g_followInspect = !g_followInspect; } break;
          
case SDLK_F2: uiLang = (uiLang==UI_EN) ? UI_KATA : UI_EN; break;
          case SDLK_b: {
            // Cycle biomes with a short fade-out/fade-in.
            world.targetBiome = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
            // Start a non-destructive biome morph (keeps the world, morphs parameters + palette).
            world.biomeMorphActive = true;
            world.biomeMorphT = 0.0f;
            world.bwFrom = world.bw;
            world.bwTo = weightsFor(world.targetBiome);
            // Switch visual palette immediately; terrain will adapt gradually via bw morph.
            world.biome = world.targetBiome;
            initClouds(world.clouds, r, world.biome);
            world.biomeFadeDir = 0;
            world.biomeFade = 0.0f;
          } break;
          case SDLK_SPACE: paused=!paused; break;
          case SDLK_w: if (g_zoom>1 && !showMenu) { g_camY -= 2; clampCameraToWorld(); break; } /* else fallthrough */
          case SDLK_a: if (g_zoom>1 && !showMenu) { g_camX -= 2; clampCameraToWorld(); break; } /* else fallthrough */
          case SDLK_d: if (g_zoom>1 && !showMenu) { g_camX += 2; clampCameraToWorld(); break; } /* else fallthrough */

          case SDLK_PERIOD:
            if (paused) { 
              step(world, r, banner, tick); 
              tick++; 
              // Music follows sim ticks (also in realtime loop below)
              synthTickMusic(synth, world, r, tick, heldNote, heldNote2, heldNote3, rootKey, scaleType, params);
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
          
case SDLK_m:
          if (showMenu && (menuPage%9)==5) {
            int rows = NUM_VOICES + 1;
            int sel = clampi(menuSel, 0, rows-1);
            if (sel < NUM_VOICES) g_voiceMute[sel] = !g_voiceMute[sel];
            else g_drumsMute = !g_drumsMute;
            applyVoiceMixer(synth);
          } else {
            showMenu = !showMenu;
          }
          break;
case SDLK_o:
  midi.enabled = !midi.enabled;
  if (midi.enabled) midi.sendStart(); else midi.sendStop();
  break;
case SDLK_c: midiClockOut = !midiClockOut; break;
case SDLK_v: useSimClock = !useSimClock; break;
case SDLK_UP: {
            int maxSel = 0;
            if (showMenu) {
              int pg = (menuPage%9);
              if (pg==0) maxSel = (int)params.size();
              else if (pg==1) maxSel = 7;
              else if (pg==4) maxSel = NUM_VOICES*3;
              else if (pg==5) maxSel = NUM_VOICES+1;
              else if (pg==6) maxSel = (int)world.agents.size();
              else if (pg==7) maxSel = MOD_N;
              else maxSel = 1;
            } else maxSel = (int)params.size();
            if (maxSel<1) maxSel=1;
            menuSel = (menuSel + maxSel - 1) % maxSel;
          } break;

case SDLK_DOWN: {
            int maxSel = 0;
            if (showMenu) {
              int pg = (menuPage%9);
              if (pg==0) maxSel = (int)params.size();
              else if (pg==1) maxSel = 7;
              else if (pg==4) maxSel = NUM_VOICES*3;
              else if (pg==5) maxSel = NUM_VOICES+1;
              else if (pg==6) maxSel = (int)world.agents.size();
              else if (pg==7) maxSel = MOD_N;
              else maxSel = 1;
            } else maxSel = (int)params.size();
            if (maxSel<1) maxSel=1;
            menuSel = (menuSel + 1) % maxSel;
          } break;

case SDLK_MINUS:
case SDLK_KP_MINUS: {
  if (showMenu && (menuPage%9)==1) {
    float* rows[] = { &g_alea.chaos, &g_alea.rainChance, &g_alea.spawnChance, &g_alea.mutationRate, &g_alea.drift, &g_alea.noteLen, &g_alea.holdChance };
    int sel = clampi(menuSel,0,6);
    float lo[] = {0.0f,0.0f,0.0f,0.0f,0.0f,0.10f,0.0f};
    float hi[] = {2.0f,2.0f,2.0f,2.0f,2.0f,2.50f,0.50f};
    *rows[sel] = std::clamp(*rows[sel] - 0.05f, lo[sel], hi[sel]);
  } else if (showMenu && (menuPage%9)==4) {
    int sel = clampi(menuSel,0,NUM_VOICES*3-1);
    int v = sel/3; int f = sel%3;
    if (f==0) { g_voice[v].program = std::max(0, g_voice[v].program - 1); g_voiceProgManual[v]=true; }
    if (f==1) { g_voice[v].minNote = std::max(0, g_voice[v].minNote - 1); }
    if (f==2) { g_voice[v].maxNote = std::max(0, g_voice[v].maxNote - 1); }
    if (g_voice[v].maxNote < g_voice[v].minNote) g_voice[v].maxNote = g_voice[v].minNote;
  } else if (showMenu && (menuPage%9)==5) {
    int rows = NUM_VOICES + 1;
    int sel = clampi(menuSel, 0, rows-1);
    if (sel < NUM_VOICES) {
      g_voiceFader[sel] = std::clamp(g_voiceFader[sel] - 0.05f, 0.0f, 1.0f);
      if (g_voiceFader[sel] <= 0.0001f) g_voiceMute[sel] = true;
    } else {
      g_drumsFader = std::clamp(g_drumsFader - 0.05f, 0.0f, 1.0f);
      if (g_drumsFader <= 0.0001f) g_drumsMute = true;
    }
    applyVoiceMixer(synth);
  } else {
    int sel = clampi(menuSel,0,(int)params.size()-1);
    params[sel].weight = std::max(0.f, params[sel].weight - 0.05f);
  }
} break;
case SDLK_EQUALS:
case SDLK_KP_PLUS: {
  if (showMenu && (menuPage%9)==1) {
    float* rows[] = { &g_alea.chaos, &g_alea.rainChance, &g_alea.spawnChance, &g_alea.mutationRate, &g_alea.drift, &g_alea.noteLen, &g_alea.holdChance };
    int sel = clampi(menuSel,0,6);
    float lo[] = {0.0f,0.0f,0.0f,0.0f,0.0f,0.10f,0.0f};
    float hi[] = {2.0f,2.0f,2.0f,2.0f,2.0f,2.50f,0.50f};
    *rows[sel] = std::clamp(*rows[sel] + 0.05f, lo[sel], hi[sel]);
  } else if (showMenu && (menuPage%9)==4) {
    int sel = clampi(menuSel,0,NUM_VOICES*3-1);
    int v = sel/3; int f = sel%3;
    if (f==0) { g_voice[v].program = std::min(127, g_voice[v].program + 1); g_voiceProgManual[v]=true; }
    if (f==1) { g_voice[v].minNote = std::min(127, g_voice[v].minNote + 1); }
    if (f==2) { g_voice[v].maxNote = std::min(127, g_voice[v].maxNote + 1); }
    if (g_voice[v].maxNote < g_voice[v].minNote) g_voice[v].maxNote = g_voice[v].minNote;
  } else if (showMenu && (menuPage%9)==5) {
    int rows = NUM_VOICES + 1;
    int sel = clampi(menuSel, 0, rows-1);
    if (sel < NUM_VOICES) {
      g_voiceMute[sel] = false;
      g_voiceFader[sel] = std::clamp(g_voiceFader[sel] + 0.05f, 0.0f, 1.0f);
    } else {
      g_drumsMute = false;
      g_drumsFader = std::clamp(g_drumsFader + 0.05f, 0.0f, 1.0f);
    }
    applyVoiceMixer(synth);
  } else {
    int sel = clampi(menuSel,0,(int)params.size()-1);
    params[sel].weight = std::min(2.f, params[sel].weight + 0.05f);
  }
} break;
case SDLK_k: rootKey = (rootKey + 1) % 12; break;
case SDLK_s:
          if (showMenu && (menuPage%9)==5) {
            // SOLO toggle on mixer page
            int rows = NUM_VOICES + 1;
            int sel = clampi(menuSel, 0, rows-1);
            if (g_soloRow == sel) {
              // clear solo: restore
              g_soloRow = -1;
              for (int v=0; v<NUM_VOICES; ++v){ g_voiceFader[v]=g_savedVoiceFader[v]; g_voiceMute[v]=g_savedVoiceMute[v]; }
              g_drumsFader = g_savedDrumsFader; g_drumsMute = g_savedDrumsMute;
            } else {
              // set solo: save then mute others
              g_soloRow = sel;
              for (int v=0; v<NUM_VOICES; ++v){ g_savedVoiceFader[v]=g_voiceFader[v]; g_savedVoiceMute[v]=g_voiceMute[v]; }
              g_savedDrumsFader = g_drumsFader; g_savedDrumsMute = g_drumsMute;

              for (int v=0; v<NUM_VOICES; ++v){ if (v!=sel) g_voiceMute[v]=true; else g_voiceMute[v]=false; }
              if (sel!=NUM_VOICES) { g_drumsMute = true; } else { g_drumsMute = false; }
            }
            applyVoiceMixer(synth);
            break;
          }
          if (g_zoom>1 && !showMenu) { g_camY += 2; clampCameraToWorld(); break; }
          scaleType = (ScaleType)(((int)scaleType + 1) % 6); break;
default: break;
        }
      }
      if (e.type == SDL_WINDOWEVENT &&
          (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
           e.window.event == SDL_WINDOWEVENT_RESIZED ||
           e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) {
        layout = computeLayout(ren);
      }
    
      if (e.type == SDL_MOUSEWHEEL) {
        if (e.wheel.y > 0) g_zoom = std::min(4, g_zoom + 1);
        if (e.wheel.y < 0) g_zoom = std::max(1, g_zoom - 1);
        clampCameraToWorld();
      }
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        // map click to world cell in current view
        int mx = e.button.x;
        int my = e.button.y;
        // Only if inside sim viewport (ignore HUD area)
        int simHpx = layout.simHpx;
        if (my >= 0 && my < simHpx) {
          int viewW = std::max(1, W / std::max(1, g_zoom));
          int viewH = std::max(1, H / std::max(1, g_zoom));
          int sx = (int)((int64_t)mx * viewW / std::max(1, layout.screenW));
          int sy = (int)((int64_t)my * viewH / std::max(1, simHpx));
                    int wx = clampi(g_camX + sx, 0, W-1);
          int wy = clampi(g_camY + sy, 0, H-1);

          // Select agent if clicked on one (DF-ish inspect). We still spawn the ripple.
          g_inspectIdx = -1;
          for (int i=0;i<(int)world.agents.size();++i){
            if (world.agents[i].x==wx && world.agents[i].y==wy) { g_inspectIdx=i; break; }
          }

          Ripple rp; rp.cx=wx; rp.cy=wy;
          rp.amp = 3.0f + 5.0f * (float)r.u01();
          rp.speed = 16.f + 18.f * (float)r.u01();
          rp.width = 2.0f + 2.5f * (float)r.u01();
          rp.chaos = 0.5f + 0.8f * (float)r.u01();
          g_ripples.push_back(rp);
        }
      }

    }

    auto now = std::chrono::steady_clock::now();
    auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    updateRipples((float)dtMs/1000.0f);
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

      // Follow selected agent (if enabled)
      if (g_followInspect && g_inspectIdx >= 0 && g_inspectIdx < (int)world.agents.size()) {
        int viewW = std::max(1, W / std::max(1, g_zoom));
        int viewH = std::max(1, H / std::max(1, g_zoom));
        const Agent& a = world.agents[g_inspectIdx];
        g_camX = clampi(a.x - viewW/2, 0, std::max(0, W - viewW));
        g_camY = clampi(a.y - viewH/2, 0, std::max(0, H - viewH));
      }

      synthTickMusic(synth, world, r, tick, heldNote, heldNote2, heldNote3, rootKey, scaleType, params);
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