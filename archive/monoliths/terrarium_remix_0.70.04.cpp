// terrarium_remix.cpp
// Fresh rebuild: biome ecology + full(er) fluid sim + event-driven MIDI (4 voices + drums).
// ASCII-first rendering, DF-ish vibe with Noita-inspired color/glow.
// Menu overlay toggled with M. Screen-adaptive layout.

#include <SDL.h>
#ifdef _WIN32
  #include <windows.h>
  #include <mmsystem.h>
  #pragma comment(lib, "winmm.lib")
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <utility>
#include <tuple>
#include <cmath>
#include <thread>
#include <unordered_map>

// ===== Config =====
static constexpr const char* VERSION = "0.70.04";
static constexpr int W = 200;
static constexpr int H = 112;
static constexpr int DEFAULT_TPS = 6;
static constexpr int MAX_AGENTS = 100;
static constexpr int START_AGENTS = 6;
static constexpr int MAX_THREADS = 8;

static constexpr int SEASON_TICKS = 1200;
static constexpr int DAY_TICKS = 1000;

// Fluid sim
static constexpr float MAX_WATER = 6.0f;      // max water height per cell
static constexpr float FLOW_RATE = 0.25f;     // flow speed
static constexpr float EVAP_RATE = 0.0006f;   // evaporation per tick
static constexpr float RAIN_RATE = 0.015f;    // rain intensity
static constexpr float SEDIMENT_ERODE_RATE = 0.010f;
static constexpr float SEDIMENT_DEPOSIT_RATE = 0.006f;
static constexpr float DETRITUS_DECAY_RATE = 0.028f;
static constexpr float DETRITUS_DIFFUSE_RATE = 0.120f;
static constexpr float COASTAL_SURGE_RATE = 0.065f;

// ===== Types =====
using Grid = std::vector<std::string>;
using WaterF = std::vector<std::vector<float>>;

struct Rng {
  std::mt19937 rng;
  Rng(uint32_t seed=0xC0FFEEu) : rng(seed) {}
  uint32_t u32() { return rng(); }
  int i(int lo,int hi){ std::uniform_int_distribution<int> d(lo,hi); return d(rng); }
  float u01(){ std::uniform_real_distribution<float> d(0.f,1.f); return d(rng); }
  bool oneIn(int n){ std::uniform_int_distribution<int> d(1,n); return d(rng)==1; }
};

static inline uint32_t hash3(uint32_t x, uint32_t y, uint32_t salt) {
  uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA6Bu ^ salt * 0xC2B2AE35u;
  h ^= (h >> 16); h *= 0x7FEB352Du; h ^= (h >> 15); h *= 0x846CA68Bu; h ^= (h >> 16);
  return h;
}
static inline int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static inline bool inBounds(int x,int y){ return x>=0 && x<W && y>=0 && y<H; }
static inline float clamp01(float v){ return v<0.f?0.f:(v>1.f?1.f:v); }
static inline float clamp11(float v){ return v<-1.f?-1.f:(v>1.f?1.f:v); }
static inline float softClipGain(float g){
  float gg = std::clamp(g, 0.0f, 3.0f);
  return 2.0f * std::tanh(gg * 0.5f);
}

// ===== Shared sim controls =====
static int g_zoom = 1;
static int g_camX = 0;
static int g_camY = 0;
static float g_camFX = 0.f;
static float g_camFY = 0.f;
static float g_zoomF = 1.f;
static int g_threads = 1;
static std::string g_audioDriver = "sdl2"; // sdl2 handles PipeWire reconnection; override with --audio-driver
static std::string g_audioDevice;

static inline int zoomViewW() { return std::max(8, W / std::max(1, g_zoom)); }
static inline int zoomViewH() { return std::max(8, H / std::max(1, g_zoom)); }
static inline int zoomViewWFor(float z) { return std::max(8, (int)std::lround((float)W / std::max(1.f, z))); }
static inline int zoomViewHFor(float z) { return std::max(8, (int)std::lround((float)H / std::max(1.f, z))); }
static inline void clampCameraToZoom() {
  int vw = zoomViewW();
  int vh = zoomViewH();
  g_camX = clampi(g_camX, 0, std::max(0, W - vw));
  g_camY = clampi(g_camY, 0, std::max(0, H - vh));
}
static inline void clampCameraFToZoom(float& cx, float& cy, float z) {
  int vw = zoomViewWFor(z);
  int vh = zoomViewHFor(z);
  cx = std::clamp(cx, 0.f, (float)std::max(0, W - vw));
  cy = std::clamp(cy, 0.f, (float)std::max(0, H - vh));
}

template <typename Fn>
static void parallelForRange(int count, int threads, Fn&& fn) {
  if (count <= 0) return;
  if (threads <= 1 || count == 1) { fn(0, count, 0); return; }
  int n = std::min(threads, count);
  int chunk = (count + n - 1) / n;
  std::vector<std::thread> ts;
  ts.reserve(n - 1);
  for (int t = 0; t < n - 1; ++t) {
    int begin = t * chunk;
    int end = std::min(count, begin + chunk);
    ts.emplace_back([=, &fn]{ fn(begin, end, t); });
  }
  int begin = (n - 1) * chunk;
  int end = std::min(count, begin + chunk);
  fn(begin, end, n - 1);
  for (auto& th : ts) th.join();
}

struct Ripple {
  int cx=0, cy=0;
  float t=0.f;
  float amp=3.f;
  float speed=18.f;
  float width=2.5f;
  float chaos=1.f;
  int mode=0;
  uint32_t seed=0;
};
static std::vector<Ripple> g_ripples;

struct Whirlpool {
  int x=0, y=0;
  float t=0.f;
  float life=6.f;
  float radius=5.f;
};
static std::vector<Whirlpool> g_whirlpools;

struct AleaWeights {
  float rainChance=1.f;
  float spawnChance=1.f;
  float mutationRate=1.f;
  float drift=1.f;
  float chaos=1.f;
};
static AleaWeights g_alea;

// ===== Mod matrix =====
static constexpr int MOD_N = 78;
static const char* g_modName[MOD_N] = {
  "water_view", "plants_view", "overlay_view", "agents_view", "agent_speed",
  "stress_mean", "stress_hi", "panic_count", "hunger_mean", "thirst_mean",
  "fatigue_mean", "health_mean", "pred_pressure", "birth_pulse", "death_pulse",
  "ripple_energy", "wind_mag", "season_pos", "cloud_opacity", "raininess",
  "shellback_stress", "swarm_cohesion", "parasite_aura", "engineer_work", "mystic_flux",
  "trickster_mischief", "pack_density", "plant_flux", "water_flux", "stress_flux",
  "hunger_flux", "thirst_flux", "fatigue_flux", "health_flux", "panic_flux",
  "emotion_mean", "bold_mean", "social_mean", "curious_mean", "aggro_mean",
  "oddity_0", "oddity_1", "oddity_2", "oddity_3", "oddity_4",
  "oddity_5", "oddity_6", "oddity_7", "oddity_8", "oddity_9",
  "biodiversity", "pred_prey_ratio", "avg_dist_water", "water_turbulence", "plant_diversity",
  "plant_var", "fauna_var", "school_cohesion", "rest_ratio", "hunt_rate",
  "forage_rate", "cloud_cover", "wind_var", "rain_cycle", "fire_activity",
  "aquatic_ratio", "land_ratio", "mean_altitude", "edge_activity", "calmness",
  "fertility_mean", "fertility_flux", "mist_strength", "heat_shimmer", "snow_density",
  "migration_bias", "grazing_impact", "soil_enrich"
};
static float g_modVal[MOD_N] = {0.f};
static int g_voiceProg[4] = {0, 48, 32, 89};
static bool g_voiceProgDirty[4] = {true, true, true, true};
static int g_progLastTick[16] = {0};
static int g_voiceVol[4] = {100, 100, 100, 100};
static int g_drumProg = 0;
static int g_drumBank = 128;
static int g_drumVol = 0;
static int g_drum2Vol = 0;
static int g_drum3Vol = 0;
static float g_masterGain = 0.55f;
static float g_tempoMult = 1.0f;
static bool g_musicKeyManual = false;
static int g_musicRootManual = 0;      // 0..11 (C..B)
static int g_musicScaleManual = 1;     // ScaleType value (default major)
static std::string g_sf2Path;
static bool g_wantSynth = false;
static bool g_ghibliPalette = false;
static bool g_showHotMods = false;
static bool g_hoverInspect = true;
static bool g_inspectPinned = false;
static bool g_inspectPinnedIsBig = false;
static int g_inspectPinnedAgentId = -1;
static int g_inspectPinnedBigX = -1;
static int g_inspectPinnedBigY = -1;
static char g_inspectPinnedBigGlyph = ' ';
static bool g_userMixerTouched = false;
static std::vector<std::string> g_sf2Presets;
static std::unordered_map<int, std::string> g_sf2PresetName;
static bool g_showAudioDebug = false;
static int g_shapeShiftTimer = 0;
static int g_chaosStormTimer = 0;
static int g_panicFloodTimer = 0;
static float g_windGust = 0.f;
static int g_windGustTimer = 0;
static int g_paletteSel = 0;
static int g_paletteChan = 0;
static bool g_paletteHSV = false;
static float g_gradeContrast = 1.0f;
static float g_gradeSat = 1.0f;
static float g_gradeLift = 0.0f;
static float g_modActivity[MOD_N] = {0.f};
static int g_modHot[6] = {0,1,2,3,4,5};
static int g_trigTimer = 0;
static int g_trigDur = 0;
static int g_trigType = 0;

enum ModDest : int {
  DEST_NONE=0,
  DEST_CC11_EXPR=1,
  DEST_CC74_BRIGHT=2,
  DEST_PAN=3,
  DEST_PORTA_V0=4,
  DEST_PORTA_V1=5,
  DEST_PORTA_V2=6,
  DEST_PORTA_V3=7,
  DEST_TEMPO=8,
};

static inline const char* modDestName(int d){
  switch(d){
    case DEST_CC11_EXPR: return "CC11 Expr";
    case DEST_CC74_BRIGHT: return "CC74 Bright";
    case DEST_PAN: return "CC10 Pan";
    case DEST_PORTA_V0: return "Porta V0";
    case DEST_PORTA_V1: return "Porta V1";
    case DEST_PORTA_V2: return "Porta V2";
    case DEST_PORTA_V3: return "Porta V3";
    case DEST_TEMPO: return "Tempo";
    default: return "None";
  }
}

struct ModMap {
  int src=0;
  int dest=DEST_NONE;
  float amt=0.0f;
  float smooth=0.20f;
  float state=0.0f;
  bool enabled=false;
};
static constexpr int MOD_SLOTS=12;
static ModMap g_modMap[MOD_SLOTS];

enum Biome : int;
static void applyBiomeModPreset(Biome b, bool applyPrograms);
static int g_g_modScroll=0; static int g_g_mmSel=0; static int g_g_mmField=0;

static float g_cc11Expr=1.0f;
static float g_cc74Bright=0.5f;
static float g_pan01=0.5f;
static float g_porta01[4] = {0.f,0.f,0.f,0.f};

static inline float smooth1(float cur,float tgt,float s){
  float a=std::clamp(s,0.0f,0.98f);
  return cur*(1.f-a) + tgt*a;
}

static void applyModMatrix(){
  g_cc11Expr=1.0f; g_cc74Bright=0.5f; g_pan01=0.5f;
  for(int v=0; v<4; ++v) g_porta01[v]=0.0f;
  g_tempoMult = 1.0f;
  for(int i=0;i<MOD_SLOTS;++i){
    auto& mm = g_modMap[i];
    if(!mm.enabled || mm.dest==DEST_NONE) continue;
    int src=std::clamp(mm.src,0,MOD_N-1);
    float x = g_modVal[src];
    float target = x * mm.amt;
    mm.state = smooth1(mm.state, target, mm.smooth);
    float v = mm.state;
    switch(mm.dest){
      case DEST_CC11_EXPR: g_cc11Expr = std::clamp(1.0f+0.7f*v,0.0f,1.0f); break;
      case DEST_CC74_BRIGHT: g_cc74Bright = std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PAN: g_pan01 = std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V0: g_porta01[0]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V1: g_porta01[1]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V2: g_porta01[2]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_PORTA_V3: g_porta01[3]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      case DEST_TEMPO: g_tempoMult = std::clamp(1.0f + 0.8f*v, 0.5f, 2.0f); break;
    }
  }
}

enum Season { SPRING=0, SUMMER=1, AUTUMN=2, WINTER=3 };
static inline Season seasonAt(int tick){ return (Season)((tick / SEASON_TICKS) % 4); }
static inline float seasonLerp(int tick){ return float(tick % SEASON_TICKS) / float(SEASON_TICKS); }
static inline bool nightish(int tick){ return ((tick / (DAY_TICKS/2)) % 2) == 1; }


enum WeatherState { CLEAR=0, OVERCAST=1, RAIN=2, STORM=3 };

enum Biome : int { MEADOW=0, WETLAND=1, ALPINE=2, DESERT=3, TROPICAL=4, TAIGA=5, ALIEN=6 };
static constexpr int BIOME_COUNT = 7;

struct RGB { uint8_t r,g,b; };
struct World;
static inline char waterGlyph(float w);
static inline char waterFlowGlyph(const World& w, int x, int y, int tick);
static inline int countChar(const Grid& g, char c){
  int n=0;
  for (const auto& row : g) for (char ch : row) if (ch==c) ++n;
  return n;
}

// ===== Species =====
struct SpeciesDef {
  const char* name;
  char glyph;
  bool aquatic;
  bool herbivore;
  bool carnivore;
  bool schooling;
  bool pack;
  float speed;
  float hungerRate;
  float thirstRate;
  float reproduce;
};

enum SpeciesId : uint8_t {
  SP_RABBIT=0, SP_DEER, SP_GOAT,
  SP_FISH, SP_CRAB, SP_FROG,
  SP_WOLF, SP_BEAR, SP_EEL,
  SP_BIRD, SP_FOX, SP_BOAR,
  SP_TURTLE, SP_HAWK, SP_SNAKE,
  SP_ALIEN1, SP_ALIEN2,
  SP_COUNT
};

static const SpeciesDef g_species[SP_COUNT] = {
  {"RABBIT", 'r', false, true,  false, false, false, 1.2f, 0.015f, 0.020f, 0.010f},
  {"DEER",   'd', false, true,  false, false, false, 1.0f, 0.012f, 0.018f, 0.008f},
  {"GOAT",   'g', false, true,  false, false, false, 1.0f, 0.012f, 0.018f, 0.007f},
  {"FISH",   'f', true,  true,  false, true,  false, 1.1f, 0.010f, 0.010f, 0.010f},
  {"CRAB",   'c', true,  true,  false, false, false, 0.8f, 0.010f, 0.010f, 0.006f},
  {"FROG",   'p', true,  true,  false, false, false, 0.9f, 0.010f, 0.012f, 0.006f},
  {"WOLF",   'w', false, false, true,  false, true,  1.4f, 0.020f, 0.020f, 0.004f},
  {"BEAR",   'b', false, false, true,  false, false, 0.9f, 0.018f, 0.018f, 0.003f},
  {"EEL",    'e', true,  false, true,  false, false, 1.2f, 0.020f, 0.015f, 0.004f},
  {"BIRD",   'v', false, true,  false, false, false, 1.6f, 0.010f, 0.020f, 0.006f},
  {"FOX",    'x', false, false, true,  false, true,  1.3f, 0.018f, 0.018f, 0.004f},
  {"BOAR",   'o', false, true,  false, false, false, 0.9f, 0.014f, 0.018f, 0.006f},
  {"TURTLE", 'u', true,  true,  false, false, false, 0.7f, 0.009f, 0.010f, 0.004f},
  {"HAWK",   'h', false, false, true,  false, false, 1.8f, 0.015f, 0.020f, 0.005f},
  {"SNAKE",  's', false, false, true,  false, false, 1.1f, 0.016f, 0.018f, 0.004f},
  {"ALIEN1", 'A', false, false, true,  false, false, 1.2f, 0.015f, 0.015f, 0.006f},
  {"ALIEN2", 'Z', true,  true,  false, true,  false, 1.0f, 0.010f, 0.010f, 0.006f},
};

static inline RGB boostColor(RGB c, float sat, float bright){
  float r = c.r/255.f, g = c.g/255.f, b = c.b/255.f;
  float maxc = std::max(r, std::max(g,b));
  float minc = std::min(r, std::min(g,b));
  float l = (maxc + minc) * 0.5f;
  float s = (maxc==minc)?0.f: (maxc - minc) / (1.f - std::fabs(2.f*l - 1.f));
  s = std::clamp(s * sat, 0.f, 1.f);
  float c2 = (1.f - std::fabs(2.f*l - 1.f)) * s;
  float h = 0.f;
  if (maxc != minc) {
    if (maxc == r) h = fmodf((g-b)/(maxc-minc), 6.f);
    else if (maxc == g) h = ((b-r)/(maxc-minc)) + 2.f;
    else h = ((r-g)/(maxc-minc)) + 4.f;
    h *= 60.f;
    if (h < 0.f) h += 360.f;
  }
  float x = c2 * (1.f - std::fabs(fmodf(h/60.f,2.f)-1.f));
  float m = l - c2*0.5f;
  float rr=0, gg=0, bb=0;
  if (h < 60) { rr=c2; gg=x; bb=0; }
  else if (h < 120) { rr=x; gg=c2; bb=0; }
  else if (h < 180) { rr=0; gg=c2; bb=x; }
  else if (h < 240) { rr=0; gg=x; bb=c2; }
  else if (h < 300) { rr=x; gg=0; bb=c2; }
  else { rr=c2; gg=0; bb=x; }
  rr = (rr + m) * bright;
  gg = (gg + m) * bright;
  bb = (bb + m) * bright;
  return { (uint8_t)clampi((int)std::lround(rr*255.f),0,255),
           (uint8_t)clampi((int)std::lround(gg*255.f),0,255),
           (uint8_t)clampi((int)std::lround(bb*255.f),0,255) };
}

struct BigDef {
  char glyph;
  int w;
  int h;
  bool aquatic;
};

struct BigPool {
  const BigDef* defs;
  int count;
};

static const BigDef g_big_meadow[]  = { {'M',2,2,false}, {'B',2,2,false} };
static const BigDef g_big_wetland[] = { {'W',3,2,true},  {'C',2,1,false} };
static const BigDef g_big_alpine[]  = { {'G',2,2,false}, {'Y',2,2,false} };
static const BigDef g_big_desert[]  = { {'D',2,2,false}, {'S',2,2,false} };
static const BigDef g_big_tropical[]= { {'H',2,2,true},  {'T',2,2,false}, {'K',3,2,true}, {'R',2,3,true} };
static const BigDef g_big_taiga[]   = { {'E',2,2,false}, {'B',2,2,false} };
static const BigDef g_big_alien[]   = { {'X',3,2,false}, {'Q',2,2,true} };

static const BigPool g_bigPools[BIOME_COUNT] = {
  {g_big_meadow,  (int)(sizeof(g_big_meadow)/sizeof(g_big_meadow[0]))},
  {g_big_wetland, (int)(sizeof(g_big_wetland)/sizeof(g_big_wetland[0]))},
  {g_big_alpine,  (int)(sizeof(g_big_alpine)/sizeof(g_big_alpine[0]))},
  {g_big_desert,  (int)(sizeof(g_big_desert)/sizeof(g_big_desert[0]))},
  {g_big_tropical,(int)(sizeof(g_big_tropical)/sizeof(g_big_tropical[0]))},
  {g_big_taiga,   (int)(sizeof(g_big_taiga)/sizeof(g_big_taiga[0]))},
  {g_big_alien,   (int)(sizeof(g_big_alien)/sizeof(g_big_alien[0]))},
};

struct BiomeDef {
  const char* name;
  RGB waterDeep;
  RGB waterShallow;
  RGB foam;
  RGB soil;
  RGB grass;
  RGB tree;
  RGB flower;
  RGB rock;
  RGB sky;
  std::vector<SpeciesId> herb;
  std::vector<SpeciesId> carn;
  std::vector<SpeciesId> aqua;
};

static BiomeDef g_biomesEdit[BIOME_COUNT];
static bool g_biomesEditInit = false;

struct BiomeTuning {
  float humidityBase;
  float pressureBase;
  float rainBias;
  float evapMult;
  float droughtMult;
  float plantGrowthMult;
  float flowerMult;
  float fertRain;
  float fertClear;
  float fireMult;
  float waterPlantMult;
  float windBias;
  float agentMetab;
};

static const BiomeDef g_biomes[BIOME_COUNT] = {
  {"MEADOW", {20,120,200},{60,190,235},{240,250,255},{110,90,70},{90,220,130},{100,190,130},{235,165,200},{170,170,185},{20,20,28},
   {SP_RABBIT,SP_DEER,SP_BOAR},{SP_WOLF,SP_FOX,SP_HAWK},{SP_FISH,SP_TURTLE}},
  {"WETLAND",{16,110,180},{50,200,240},{235,250,255},{95,85,70},{70,210,170},{90,180,150},{195,235,210},{140,150,160},{20,20,28},
   {SP_FROG,SP_CRAB,SP_TURTLE},{SP_EEL,SP_HAWK},{SP_FISH,SP_CRAB,SP_FROG,SP_EEL,SP_TURTLE}},
  {"ALPINE", {18,90,170},{60,160,220},{230,245,255},{95,90,85},{120,210,180},{110,180,170},{220,210,230},{190,200,220},{20,20,28},
   {SP_GOAT},{SP_BEAR,SP_HAWK,SP_WOLF},{SP_FISH}},
  {"DESERT", {30,95,160},{75,160,210},{235,245,255},{175,135,85},{200,200,120},{170,180,120},{245,200,150},{200,180,140},{20,20,28},
   {SP_RABBIT,SP_BOAR},{SP_WOLF,SP_FOX,SP_SNAKE},{SP_FISH}},
  {"TROPICAL",{16,110,185},{60,195,230},{235,245,250},{95,70,45},{70,230,150},{90,200,160},{245,150,180},{150,150,170},{20,20,28},
   {SP_DEER,SP_RABBIT,SP_BOAR},{SP_WOLF,SP_FOX,SP_SNAKE,SP_HAWK},{SP_FISH,SP_CRAB,SP_TURTLE,SP_EEL,SP_FROG}},
  {"TAIGA",  {18,90,170},{60,150,210},{230,245,255},{90,80,80},{85,190,120},{80,160,130},{220,180,200},{160,160,180},{20,20,28},
   {SP_DEER,SP_BOAR},{SP_WOLF,SP_BEAR,SP_FOX},{SP_FISH}},
  {"ALIEN",  {70,40,120},{120,70,180},{240,220,255},{75,30,75},{100,60,150},{140,90,190},{230,100,240},{170,100,190},{12,8,18},
   {SP_ALIEN1},{SP_ALIEN1,SP_HAWK},{SP_ALIEN2}},
};

static const BiomeTuning g_biomeTune[BIOME_COUNT] = {
  // humidity, pressure, rainBias, evap, drought, growth, flower, fertRain, fertClear, fire, waterPlant, wind, agentMetab
  {0.45f, 0.55f, 0.1f, 0.95f, 1.0f, 1.0f, 1.0f, 0.0015f, 0.00045f, 1.0f, 1.0f, 1.0f, 1.0f}, // MEADOW
  {0.75f, 0.50f, 0.5f, 0.85f, 0.8f, 1.25f, 1.1f, 0.0020f, 0.00035f, 0.7f, 1.4f, 0.9f, 0.95f}, // WETLAND
  {0.40f, 0.60f, 0.05f, 1.05f, 1.2f, 0.7f, 0.7f, 0.0010f, 0.00030f, 1.1f, 0.7f, 1.2f, 1.05f}, // ALPINE
  {0.20f, 0.65f, -0.3f, 1.35f, 2.0f, 0.45f, 0.4f, 0.0006f, 0.00020f, 1.5f, 0.5f, 1.4f, 1.1f}, // DESERT
  {0.70f, 0.48f, 0.35f, 0.90f, 0.9f, 1.35f, 1.2f, 0.0022f, 0.00040f, 0.9f, 1.5f, 0.9f, 1.0f}, // TROPICAL
  {0.35f, 0.58f, 0.0f, 1.05f, 1.15f, 0.85f, 0.7f, 0.0011f, 0.00028f, 1.0f, 0.8f, 1.1f, 1.05f}, // TAIGA
  {0.55f, 0.50f, 0.15f, 0.95f, 1.0f, 1.05f, 1.3f, 0.0016f, 0.00040f, 1.2f, 1.0f, 1.0f, 1.0f}, // ALIEN
};

static inline void enforceDefaultDrumMute(){
  if (!g_userMixerTouched) {
    g_drumVol = 0;
    g_drum2Vol = 0;
    g_drum3Vol = 0;
  }
}

static void applyBiomeModPreset(Biome b, bool applyPrograms){
  ModMap preset[MOD_SLOTS]{};
  auto set = [&](int i, int src, int dest, float amt, float smooth){
    if (i<0 || i>=MOD_SLOTS) return;
    preset[i].src=src; preset[i].dest=dest; preset[i].amt=amt; preset[i].smooth=smooth; preset[i].enabled=true;
  };
  set(0, 0, DEST_CC74_BRIGHT, 0.8f, 0.7f);
  set(1, 1, DEST_CC11_EXPR, 0.8f, 0.7f);
  set(2, 16, DEST_PAN, 0.6f, 0.6f);
  set(3, 19, DEST_TEMPO, 0.6f, 0.6f);
  set(4, 35, DEST_PORTA_V0, 0.5f, 0.6f);
  set(5, 37, DEST_PORTA_V1, 0.5f, 0.6f);
  set(6, 39, DEST_PORTA_V2, 0.5f, 0.6f);
  set(7, 15, DEST_PORTA_V3, 0.5f, 0.6f);
  if (b==ALIEN) {
    preset[3].amt = 1.2f;
    preset[0].amt = 1.1f;
  } else if (b==DESERT) {
    preset[3].amt = 0.4f;
  } else if (b==TROPICAL) {
    preset[3].amt = 0.8f;
  }
  for (int i=0;i<MOD_SLOTS;++i) g_modMap[i]=preset[i];

  if (applyPrograms) {
    // biome musical defaults
    struct VoicePreset { int v0,v1,v2,v3; int dvol; int dprog; };
    static const VoicePreset vp[BIOME_COUNT] = {
      {0, 48, 32, 89, 80, 0},   // MEADOW: piano/strings/bass/pad
      {12, 13, 33, 89, 70, 0},  // WETLAND: marimba/xylophone/bass/pad
      {46, 52, 32, 90, 60, 0},  // ALPINE: harp/choir/bass/pad
      {24, 25, 32, 89, 55, 0},  // DESERT: nylon/guitar/bass/pad
      {114, 73, 32, 89, 90, 0}, // TROPICAL: steel drums/flute/bass/pad
      {41, 48, 32, 89, 65, 0},  // TAIGA: violin/strings/bass/pad
      {81, 80, 32, 89, 75, 0},  // ALIEN: synth lead/lead/bass/pad
    };
    const VoicePreset& p = vp[b];
    g_voiceProg[0]=p.v0; g_voiceProg[1]=p.v1; g_voiceProg[2]=p.v2; g_voiceProg[3]=p.v3;
    for (int i=0;i<4;++i) g_voiceProgDirty[i] = true;
    g_drumProg = p.dprog;
  }
  enforceDefaultDrumMute();
}

static inline void initBiomeEdits(){
  if (g_biomesEditInit) return;
  for (int i=0;i<BIOME_COUNT;++i) g_biomesEdit[i] = g_biomes[i];
  g_biomesEditInit = true;
}

static inline RGB& biomeColorRef(BiomeDef& b, int idx){
  switch(idx){
    case 0: return b.waterDeep;
    case 1: return b.waterShallow;
    case 2: return b.foam;
    case 3: return b.soil;
    case 4: return b.grass;
    case 5: return b.tree;
    case 6: return b.flower;
    case 7: return b.rock;
    default: return b.sky;
  }
}

static inline const char* biomeColorName(int idx){
  static const char* names[] = {
    "water_deep","water_shallow","foam","soil","grass","tree","flower","rock","sky"
  };
  return names[clampi(idx, 0, 8)];
}

static inline void rgbToHsv(const RGB& c, float& h, float& s, float& v){
  float rf = c.r / 255.f;
  float gf = c.g / 255.f;
  float bf = c.b / 255.f;
  float maxv = std::max(rf, std::max(gf, bf));
  float minv = std::min(rf, std::min(gf, bf));
  v = maxv;
  float d = maxv - minv;
  s = (maxv <= 0.f) ? 0.f : (d / maxv);
  if (d <= 1e-6f) { h = 0.f; return; }
  if (maxv == rf) h = 60.f * std::fmod(((gf - bf) / d), 6.f);
  else if (maxv == gf) h = 60.f * (((bf - rf) / d) + 2.f);
  else h = 60.f * (((rf - gf) / d) + 4.f);
  if (h < 0.f) h += 360.f;
}

static inline RGB hsvToRgb(float h, float s, float v){
  h = std::fmod(h, 360.f); if (h < 0.f) h += 360.f;
  s = std::clamp(s, 0.f, 1.f);
  v = std::clamp(v, 0.f, 1.f);
  float c = v * s;
  float x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
  float m = v - c;
  float r=0,g=0,b=0;
  if (h < 60.f)      { r=c; g=x; b=0; }
  else if (h < 120.f){ r=x; g=c; b=0; }
  else if (h < 180.f){ r=0; g=c; b=x; }
  else if (h < 240.f){ r=0; g=x; b=c; }
  else if (h < 300.f){ r=x; g=0; b=c; }
  else               { r=c; g=0; b=x; }
  RGB out;
  out.r = (uint8_t)clampi((int)std::lround((r + m) * 255.f), 0, 255);
  out.g = (uint8_t)clampi((int)std::lround((g + m) * 255.f), 0, 255);
  out.b = (uint8_t)clampi((int)std::lround((b + m) * 255.f), 0, 255);
  return out;
}

static inline const char* presetName(int bank, int prog){
  int key = bank * 128 + prog;
  auto it = g_sf2PresetName.find(key);
  if (it != g_sf2PresetName.end()) return it->second.c_str();
  return "";
}

static void savePaletteFile(const std::string& path){
  std::filesystem::create_directories("/home/user/terrarium/palettes");
  std::ofstream out(path);
  if (!out){ std::fprintf(stderr, "terrarium: could not write palette '%s'\n", path.c_str()); return; }
  out << "grade " << g_gradeContrast << " " << g_gradeSat << " " << g_gradeLift << "\n";
  for (int i=0;i<BIOME_COUNT;++i){
    const BiomeDef& b = g_biomesEdit[i];
    out << "biome " << g_biomes[i].name << " ";
    const RGB cols[] = {b.waterDeep,b.waterShallow,b.foam,b.soil,b.grass,b.tree,b.flower,b.rock,b.sky};
    for (int k=0;k<9;++k){
      out << (int)cols[k].r << " " << (int)cols[k].g << " " << (int)cols[k].b << " ";
    }
    out << "\n";
  }
}

static void loadPaletteFile(const std::string& path){
  std::ifstream in(path);
  if (!in){ std::fprintf(stderr, "terrarium: could not read palette '%s'\n", path.c_str()); return; }
  std::string tok;
  while (in >> tok) {
    if (tok == "grade") {
      in >> g_gradeContrast >> g_gradeSat >> g_gradeLift;
    } else if (tok == "biome") {
      std::string name; in >> name;
      int idx = -1;
      for (int i=0;i<BIOME_COUNT;++i) if (name == g_biomes[i].name) { idx = i; break; }
      if (idx < 0) { int skip; for(int k=0;k<27;++k) in >> skip; continue; }
      BiomeDef& b = g_biomesEdit[idx];
      RGB* cols[] = {&b.waterDeep,&b.waterShallow,&b.foam,&b.soil,&b.grass,&b.tree,&b.flower,&b.rock,&b.sky};
      for (int k=0;k<9;++k){
        int r,g,bl; in >> r >> g >> bl;
        cols[k]->r = (uint8_t)clampi(r,0,255);
        cols[k]->g = (uint8_t)clampi(g,0,255);
        cols[k]->b = (uint8_t)clampi(bl,0,255);
      }
    }
  }
}

struct Weather {
  WeatherState state=CLEAR;
  float humidity=0.4f;
  float pressure=0.5f;
  float rainStrength=0.f;
  float cloudOpacity=0.6f;
  int timer=0;
};

struct Wind { int dx=0, dy=0; int strength=0; };

static inline char terrainGlyphVariant(char t, uint32_t h, Season s, const Weather& we) {
  uint32_t k = h & 7u;
  if (t=='f' || t=='+' || t=='&' || t=='!') {
    if ((s==SPRING || we.state==RAIN || we.state==STORM) && k==0) return '!';
    if (k==1) return '&';
    if (k==2) return '+';
    return t;
  }
  if (t=='d' || t=='e' || t=='g') {
    switch (h & 3u) {
      case 0: return 'd';
      case 1: return 'e';
      case 2: return 'g';
      default:return 'd';
    }
  }
  return t;
}

struct Agent {
  int id=0;
  int x=0,y=0;
  SpeciesId sp=SP_RABBIT;
  int goalX=0, goalY=0;
  int goalTTL=0;
  float hunger=0.f;
  float thirst=0.f;
  float stress=0.f;
  float fatigue=0.f;
  float emotion=0.6f;
  float bold=0.5f;
  float social=0.5f;
  float curious=0.5f;
  float aggro=0.5f;
  float health=1.f;
  bool panic=false;
  int age=0;
  int maxAge=2000;
};

struct Event {
  enum Type {EV_BIRTH, EV_DEATH, EV_EAT, EV_DRINK, EV_STORM, EV_LIGHTNING, EV_FIRE, EV_RAIN, EV_FLOW} type;
  int x=0,y=0;
  float mag=1.f;
};

struct Cloud {
  float x=0.f, y=0.f;
  float vx=0.f, vy=0.f;
  float size=6.f;
  int life=0;
};

struct BigCreature {
  int x=0,y=0;
  int w=2,h=2;
  char glyph='M';
  bool aquatic=false;
  int moveCooldown=0;
};

struct World {
  Biome biome=MEADOW;
  Biome targetBiome=MEADOW;
  bool biomeMorphActive=false;
  float biomeMorphT=0.f; // 0..1
  Grid terrain;      // '.', ',', '"', ';', 'T', 'Y', '#', 'd', '^', 's', 'c', '*', 'x'
  Grid entities;     // big creatures (anchors)
  Grid overlay;      // rain, fire, etc.
  WaterF water;      // water height (0..MAX_WATER)
  WaterF waterBase;  // persistent basins to prevent full drain
  WaterF waterNext;  // reused buffer for water sim
  WaterF oceanDelta; // tropical current buffer
  std::vector<std::vector<uint8_t>> coreLand;  // tropical hard islands
  std::vector<std::vector<uint8_t>> shoreLand; // tropical soft shores
  std::vector<WaterF> waterDeltas; // per-thread delta buffers (row-sliced)
  std::vector<int> waterDeltaY0;
  std::vector<int> waterDeltaY1;
  int waterDeltaThreads=0;
  std::vector<std::vector<float>> fertility; // 0..1
  std::vector<std::vector<float>> sediment;  // suspended/shore sediment
  std::vector<std::vector<float>> detritus;  // dead biomass -> nutrients
  std::vector<std::vector<uint8_t>> height; // 0..255
  std::vector<std::vector<uint8_t>> moist;  // 0..255
  Weather weather;
  Wind wind;
  std::vector<Agent> agents;
  std::vector<BigCreature> bigs;
  std::vector<Cloud> clouds;
  std::vector<Event> events;
  std::vector<std::pair<int,int>> prevPos;
  uint32_t seed=0;
};

// ===== MIDI (event-driven) =====
// Minimal MIDI out: Linux no-op, WinMM for Windows.
struct MidiOut {
  bool enabled=false;
#ifdef _WIN32
  HMIDIOUT dev = nullptr;
  bool open(int deviceIndex=0){
    if (midiOutOpen(&dev, deviceIndex, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR){
      enabled = true; return true;
    }
    return false;
  }
  void close(){ if(dev){ midiOutClose(dev); dev=nullptr; } enabled=false; }
  void sendShort(uint8_t status, uint8_t data1, uint8_t data2){
    if(!enabled) return;
    DWORD msg = status | (data1<<8) | (data2<<16);
    midiOutShortMsg(dev, msg);
  }
  void sendNoteOn(int ch,int note,int vel){ sendShort(0x90 | (ch&0x0F), note, vel); }
  void sendNoteOff(int ch,int note,int vel=0){ sendShort(0x80 | (ch&0x0F), note, vel); }
  void sendCC(int ch,int cc,int val){ sendShort(0xB0 | (ch&0x0F), cc, val); }
  void sendProgramChange(int ch,int prog){ sendShort(0xC0 | (ch&0x0F), prog, 0); }
#else
  bool open(int /*deviceIndex*/=0){ enabled=false; return false; }
  void close(){}
  void sendNoteOn(int,int,int){}
  void sendNoteOff(int,int,int=0){}
  void sendCC(int,int,int){}
  void sendProgramChange(int,int){}
#endif
};

// FluidSynth built-in synth (optional)
#ifdef USE_FLUIDSYNTH
  #include <fluidsynth.h>
#endif
struct SynthOut {
  bool enabled=false;
#ifdef USE_FLUIDSYNTH
  fluid_settings_t* settings=nullptr;
  fluid_synth_t* synth=nullptr;
  fluid_audio_driver_t* adriver=nullptr;
  int sfid=-1;
  float gain=0.7f;
  bool open(const std::string& sf2, float g, const std::string& driver, const std::string& device){
    gain=g;
    settings = new_fluid_settings();
    if(!settings) return false;
    if (!driver.empty()) {
      fluid_settings_setstr(settings, "audio.driver", driver.c_str());
    }
    if (!device.empty()) {
      if (driver=="pulseaudio" || driver=="pulse") {
        fluid_settings_setstr(settings, "audio.pulseaudio.device", device.c_str());
      } else if (driver=="alsa") {
        fluid_settings_setstr(settings, "audio.alsa.device", device.c_str());
      } else {
        fluid_settings_setstr(settings, "audio.device", device.c_str());
      }
    }
    fluid_settings_setnum(settings, "synth.gain", (double)gain);
    // Match PipeWire's native rate — avoids resampling on modern Linux desktops.
    // PipeWire runs at 48000 Hz / 1024-sample quantum; mismatching causes underruns.
    fluid_settings_setnum(settings, "synth.sample-rate", 48000.0);
    // Realtime priority — requires user in 'audio' group:
    //   sudo usermod -aG audio $USER
    //   echo '@audio - rtprio 95' | sudo tee /etc/security/limits.d/99-audio-rtprio.conf
    fluid_settings_setint(settings, "audio.realtime-prio", 80);
    // 1024-sample periods @ 48kHz = ~21ms each. 6 periods = ~128ms total ring buffer.
    // Without RT priority this larger buffer absorbs preemptions from simulation threads.
    // With RT priority (setcap/audio group) you can safely reduce periods back to 3.
    fluid_settings_setint(settings, "audio.period-size", 1024);
    fluid_settings_setint(settings, "audio.periods", 6);
    // 16 voices matches terrarium_0.50.6 — keeps DSP load low and voice stealing natural.
    // 96 was too heavy; the synth was starving the audio thread.
    fluid_settings_setint(settings, "synth.polyphony", 16);
    fluid_settings_setint(settings, "synth.cpu-cores", 1);
    // Reverb/chorus settings ported directly from terrarium_0.50.6 — they worked there
    // because low polyphony leaves plenty of CPU headroom for the effects.
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
    synth = new_fluid_synth(settings);
    if(!synth) return false;
    sfid = fluid_synth_sfload(synth, sf2.c_str(), 1);
    if (sfid < 0) return false;
    adriver = new_fluid_audio_driver(settings, synth);
    if(!adriver) return false;
    enabled=true; return true;
  }
  void close(){
    if(adriver){ delete_fluid_audio_driver(adriver); adriver=nullptr; }
    if(synth){ delete_fluid_synth(synth); synth=nullptr; }
    if(settings){ delete_fluid_settings(settings); settings=nullptr; }
    enabled=false;
  }
  void noteOn(int ch,int note,int vel){ if(enabled) fluid_synth_noteon(synth,ch,note,vel); }
  void noteOff(int ch,int note,int vel=0){ if(enabled) fluid_synth_noteoff(synth,ch,note); (void)vel; }
  void cc(int ch,int cc,int val){ if(enabled) fluid_synth_cc(synth,ch,cc,val); }
  void programChange(int ch,int prog){ if(enabled) fluid_synth_program_change(synth,ch,prog); }
  void bankSelect(int ch,int bank){ if(enabled) fluid_synth_bank_select(synth,ch,bank); }
  void setGain(float g){ if(enabled){ gain=g; fluid_synth_set_gain(synth, g); } }
  void allNotesOff(){
    if(!enabled) return;
    for(int ch=0; ch<16; ++ch) fluid_synth_all_notes_off(synth, ch);
  }
  void listPresets(std::vector<std::string>& out){
    out.clear();
    g_sf2PresetName.clear();
    if (!enabled || !synth) return;
    fluid_sfont_t* sfont = fluid_synth_get_sfont(synth, 0);
    if (!sfont) return;
    fluid_sfont_iteration_start(sfont);
    fluid_preset_t* preset = nullptr;
    while ((preset = fluid_sfont_iteration_next(sfont)) != nullptr) {
      const char* name = fluid_preset_get_name(preset);
      int bank = fluid_preset_get_banknum(preset);
      int prog = fluid_preset_get_num(preset);
      if (!name) name = "?";
      char line[256];
      std::snprintf(line, sizeof(line), "[%03d:%03d] %s", bank, prog, name);
      out.push_back(line);
      int key = bank * 128 + prog;
      if (!g_sf2PresetName.count(key)) g_sf2PresetName[key] = name;
    }
    std::sort(out.begin(), out.end());
  }
#else
  bool open(const std::string&, float, const std::string&, const std::string&){ enabled=false; return false; }
  void close(){}
  void noteOn(int,int,int){}
  void noteOff(int,int,int=0){}
  void cc(int,int,int){}
  void programChange(int,int){}
  void bankSelect(int,int){}
  void setGain(float){}
  void listPresets(std::vector<std::string>& out){ out.clear(); g_sf2PresetName.clear(); }
#endif
};

// ===== Glyphs =====
static inline const uint8_t* glyph8_text(unsigned char c);
static const uint8_t* glyph8_world(unsigned char c){
  static const uint8_t BLANK[8]  = {0,0,0,0,0,0,0,0};
  static const uint8_t DOT[8]    = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00};
  static const uint8_t COMMA[8]  = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x10};
  static const uint8_t TGRASS[8] = {0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t SHRUB[8]  = {0x00,0x24,0x7E,0x24,0x24,0x7E,0x24,0x00};
  static const uint8_t TREE1[8]  = {0x10,0x38,0x54,0x10,0x10,0x10,0x38,0x00};
  static const uint8_t TREE2[8]  = {0x10,0x38,0x54,0x10,0x10,0x28,0x44,0x00};
  static const uint8_t ROCK[8]   = {0x00,0x18,0x3C,0x7E,0x7E,0x3C,0x18,0x00};
  static const uint8_t MUD[8]    = {0x00,0x00,0x3A,0x5C,0x2E,0x74,0x5C,0x2E};
  static const uint8_t MUD1[8]   = {0x00,0x00,0x3C,0x6A,0x5C,0x3A,0x6C,0x00};
  static const uint8_t MUD2[8]   = {0x00,0x00,0x2C,0x5A,0x3C,0x66,0x5A,0x00};
  static const uint8_t FIRE[8]   = {0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00,0x00};
  static const uint8_t ASH[8]    = {0x00,0x00,0x10,0x28,0x10,0x28,0x00,0x00};
  static const uint8_t LILYP[8]  = {0x00,0x00,0x18,0x3C,0x7E,0x3C,0x18,0x00}; // m
  static const uint8_t APLANT[8] = {0x00,0x18,0x24,0x5A,0x24,0x18,0x00,0x00}; // a
  static const uint8_t TUMBLE[8] = {0x00,0x3C,0x42,0x5A,0x66,0x42,0x3C,0x00}; // t
  static const uint8_t LICHEN[8] = {0x00,0x18,0x3C,0x18,0x3C,0x18,0x00,0x00}; // l
  static const uint8_t VINE[8]   = {0x20,0x10,0x08,0x04,0x02,0x01,0x02,0x04}; // n
  static const uint8_t SPORE[8]  = {0x00,0x10,0x28,0x44,0x28,0x10,0x00,0x00}; // q
  static const uint8_t FLOW1[8]  = {0x10,0x54,0x38,0x7C,0x38,0x54,0x10,0x00}; // +
  static const uint8_t FLOW2[8]  = {0x00,0x10,0x38,0x7C,0x38,0x10,0x00,0x00}; // f
  static const uint8_t BIGF[8]   = {0x28,0x7C,0xFE,0x7C,0xFE,0x7C,0x28,0x00}; // &
  static const uint8_t SUPERB[8] = {0x10,0x7C,0xFE,0x7C,0xFE,0x7C,0x10,0x00}; // !

  static const uint8_t W1[8]     = {0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00};
  static const uint8_t W2[8]     = {0x00,0x00,0x10,0x00,0x04,0x00,0x00,0x00};
  static const uint8_t W3[8]     = {0x00,0x00,0x28,0x00,0x10,0x00,0x00,0x00};
  static const uint8_t W4[8]     = {0x00,0x00,0x28,0x00,0x28,0x00,0x00,0x00};
  static const uint8_t W5[8]     = {0x00,0x44,0x28,0x00,0x44,0x28,0x00,0x00};
  static const uint8_t W6[8]     = {0x00,0x44,0x28,0x00,0x44,0x28,0x00,0x44};
  static const uint8_t W7[8]     = {0x44,0x28,0x00,0x44,0x28,0x00,0x44,0x28};
  static const uint8_t W1H[8]    = {0x00,0x00,0x00,0x38,0x00,0x00,0x00,0x00};
  static const uint8_t W2H[8]    = {0x00,0x00,0x38,0x00,0x1C,0x00,0x00,0x00};
  static const uint8_t W3H[8]    = {0x00,0x00,0x38,0x00,0x38,0x00,0x00,0x00};
  static const uint8_t W4H[8]    = {0x00,0x38,0x00,0x38,0x00,0x38,0x00,0x00};
  static const uint8_t W5H[8]    = {0x00,0x7C,0x00,0x38,0x00,0x7C,0x00,0x00};
  static const uint8_t W6H[8]    = {0x00,0x7C,0x00,0x7C,0x00,0x7C,0x00,0x00};
  static const uint8_t W7H[8]    = {0x7C,0x00,0x7C,0x00,0x7C,0x00,0x7C,0x00};
  static const uint8_t W1V[8]    = {0x00,0x00,0x10,0x10,0x10,0x00,0x00,0x00};
  static const uint8_t W2V[8]    = {0x00,0x10,0x10,0x00,0x10,0x10,0x00,0x00};
  static const uint8_t W3V[8]    = {0x10,0x10,0x00,0x10,0x10,0x00,0x10,0x10};
  static const uint8_t W4V[8]    = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00};
  static const uint8_t W5V[8]    = {0x1C,0x1C,0x00,0x1C,0x1C,0x00,0x1C,0x1C};
  static const uint8_t W6V[8]    = {0x3C,0x00,0x3C,0x00,0x3C,0x00,0x3C,0x00};
  static const uint8_t W7V[8]    = {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C};
  static const uint8_t W1D[8]    = {0x00,0x00,0x40,0x03,0x10,0x00,0x00,0x00};
  static const uint8_t W2D[8]    = {0x00,0x40,0x03,0x10,0x08,0x00,0x00,0x00};
  static const uint8_t W3D[8]    = {0x40,0x03,0x10,0x08,0x04,0x02,0x00,0x00};
  static const uint8_t W4D[8]    = {0x40,0x03,0x10,0x08,0x10,0x03,0x40,0x00};
  static const uint8_t W5D[8]    = {0x44,0x05,0x11,0x08,0x11,0x05,0x44,0x00};
  static const uint8_t W6D[8]    = {0x66,0x33,0x19,0x0C,0x19,0x33,0x66,0x00};
  static const uint8_t W7D[8]    = {0x77,0x3B,0x1D,0x0E,0x1D,0x3B,0x77,0x00};

  if (c==' ') return BLANK;
  if (c=='.') return DOT;
  if (c==',') return COMMA;
  if (c=='"') return TGRASS;
  if (c==';') return SHRUB;
  if (c=='T') return TREE1;
  if (c=='Y') return TREE2;
  if (c=='^') return ROCK;
  if (c=='d') return MUD;
  if (c=='e') return MUD1;
  if (c=='g') return MUD2;
  if (c=='*') return FIRE;
  if (c=='x') return ASH;
  if (c=='m') return LILYP;
  if (c=='a') return APLANT;
  if (c=='t') return TUMBLE;
  if (c=='l') return LICHEN;
  if (c=='n') return VINE;
  if (c=='q') return SPORE;
  if (c=='+') return FLOW1;
  if (c=='f') return FLOW2;
  if (c=='&') return BIGF;
  if (c=='!') return SUPERB;
  if (c=='1') return W1;
  if (c=='2') return W2;
  if (c=='3') return W3;
  if (c=='4') return W4;
  if (c=='5') return W5;
  if (c=='6') return W6;
  if (c=='7') return W7;
  switch (c) {
    case '\x01': return W1H; case '\x02': return W2H; case '\x03': return W3H; case '\x04': return W4H; case '\x05': return W5H; case '\x06': return W6H; case '\x07': return W7H;
    case '\x08': return W1V; case '\x09': return W2V; case '\x0A': return W3V; case '\x0B': return W4V; case '\x0C': return W5V; case '\x0D': return W6V; case '\x0E': return W7V;
    case '\x0F': return W1D; case '\x10': return W2D; case '\x11': return W3D; case '\x12': return W4D; case '\x13': return W5D; case '\x14': return W6D; case '\x15': return W7D;
  }

  if (c>='a' && c<='z') return glyph8_text((unsigned char)(c - 'a' + 'A'));
  if (c>='A' && c<='Z') return glyph8_text(c);
  return DOT;
}

// UI/text font: simple 5x7 uppercase (ASCII) for menus.
static inline const uint8_t* glyph8_text(unsigned char c) {
  static const uint8_t BLANK[8] = {0,0,0,0,0,0,0,0};
  if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
  #define R(x) (uint8_t)((x) << 2)
  static const uint8_t SPACE[8] = {0,0,0,0,0,0,0,0};
  static const uint8_t DOT[8]   = {0,0,0,0,0,0, R(0b00100), 0};
  static const uint8_t COLON[8] = {0, R(0b00100), 0,0, R(0b00100), 0,0,0};
  static const uint8_t DASH[8]  = {0,0,0, R(0b11111), 0,0,0,0};
  static const uint8_t PLUS[8]  = {0,0, R(0b00100), R(0b11111), R(0b00100), 0,0,0};
  static const uint8_t SLASH[8] = {R(0b00001), R(0b00010), R(0b00100), R(0b01000), R(0b10000),0,0,0};
  static const uint8_t PCT[8]   = {R(0b11001), R(0b11010), R(0b00100), R(0b01000), R(0b10110), 0,0,0};
  static const uint8_t LBR[8]   = {R(0b00110), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00110), 0};
  static const uint8_t RBR[8]   = {R(0b01100), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b01100), 0};
  static const uint8_t EQ[8]    = {0,0, R(0b11111),0, R(0b11111),0,0,0};
  static const uint8_t COMMA[8] = {0,0,0,0,0, R(0b00100), R(0b00100), R(0b01000)};
  static const uint8_t QUOTE[8] = {R(0b00100), R(0b00100),0,0,0,0,0,0};
  static const uint8_t EXCL[8]  = {R(0b00100), R(0b00100), R(0b00100), R(0b00100), R(0b00100),0, R(0b00100),0};

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

  switch(c){
    case ' ': return SPACE;
    case '.': return DOT;
    case ':': return COLON;
    case '-': return DASH;
    case '+': return PLUS;
    case '/': return SLASH;
    case '%': return PCT;
    case '[': return LBR;
    case ']': return RBR;
    case '=': return EQ;
    case ',': return COMMA;
    case '"': return QUOTE;
    case '!': return EXCL;
    case '0': return D0; case '1': return D1; case '2': return D2; case '3': return D3; case '4': return D4;
    case '5': return D5; case '6': return D6; case '7': return D7; case '8': return D8; case '9': return D9;
    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E; case 'F': return F;
    case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J; case 'K': return K; case 'L': return L;
    case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P; case 'Q': return Q; case 'R': return Rr;
    case 'S': return S; case 'T': return T; case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X;
    case 'Y': return Y; case 'Z': return Z;
  }
  return BLANK;
}
#undef R

struct GlyphCache {
  std::array<SDL_Texture*, 256> tex{};
  bool textMode=false;
  ~GlyphCache(){ for (auto* t: tex) if(t) SDL_DestroyTexture(t); }
  SDL_Texture* get(SDL_Renderer* ren, unsigned char c){
    if (tex[c]) return tex[c];
    const uint8_t* g = textMode ? glyph8_text(c) : glyph8_world(c);
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_RGBA32);
    if(!surf) return nullptr;
    uint32_t* px = (uint32_t*)surf->pixels;
    for(int y=0;y<8;++y){
      for(int x=0;x<8;++x){
        bool on = (g[y] >> (7-x)) & 1;
        px[y*8 + x] = on ? 0xFFFFFFFFu : 0x00000000u;
      }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, surf);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surf);
    tex[c]=t;
    return t;
  }
};

static void drawString(SDL_Renderer* ren, GlyphCache& gc, int px, int py, const std::string& s,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale) {
  int x = px, y = py;
  for (char c : s) {
    if (c=='\n'){ y += 8*scale; x = px; continue; }
    SDL_Texture* tex = gc.get(ren, (unsigned char)c);
    if (!tex){ x += 8*scale; continue; }
    SDL_SetTextureColorMod(tex, r, g, b);
    SDL_SetTextureAlphaMod(tex, a);
    SDL_Rect rc{ x, y, 8*scale, 8*scale };
    SDL_RenderCopy(ren, tex, nullptr, &rc);
    x += 8*scale;
  }
}

struct Layout { int screenW=0, screenH=0; int simHpx=0; int hudH=0; };
static Layout computeLayout(SDL_Renderer* ren){
  Layout L; SDL_GetRendererOutputSize(ren, &L.screenW, &L.screenH);
  L.hudH = std::max(40, L.screenH/18);
  L.simHpx = L.screenH; // sim fills screen; HUD overlays when menu open
  return L;
}
static inline void setColor(SDL_Renderer* rr, uint8_t R, uint8_t G, uint8_t B, uint8_t A=255){ SDL_SetRenderDrawColor(rr,R,G,B,A); }

// ===== Ecology helpers =====
static inline bool isTree(char c){ return c=='T' || c=='Y'; }
static inline bool isVeg(char c){ return (c==','||c=='"'||c==';'||c=='#'||c=='m'||c=='a'||c=='f'||c=='+'||c=='&'||c=='!'||c=='t'||c=='l'||c=='n'||c=='q'||isTree(c)); }
static inline bool isEdiblePlant(char c){ return (c==','||c=='"'||c==';'||c=='#'||c=='f'||c=='+'||c=='&'||c=='!'); }
static inline bool isFlower(char c){ return (c=='f'||c=='+'||c=='&'||c=='!'); }

static inline int nearbyTreeCount(const World& w, int x, int y, int r){
  int c = 0;
  for (int dy=-r; dy<=r; ++dy){
    for (int dx=-r; dx<=r; ++dx){
      if (!dx && !dy) continue;
      int nx = x + dx, ny = y + dy;
      if (!inBounds(nx,ny)) continue;
      if (isTree(w.terrain[ny][nx])) c++;
    }
  }
  return c;
}

static inline bool nearShore(const World& w, int x, int y){
  for (int dy=-1; dy<=1; ++dy){
    for (int dx=-1; dx<=1; ++dx){
      if (!dx && !dy) continue;
      int nx = x + dx, ny = y + dy;
      if (!inBounds(nx,ny)) continue;
      bool a = w.water[y][x] > 0.2f;
      bool b = w.water[ny][nx] > 0.2f;
      if (a != b) return true;
    }
  }
  return false;
}

static inline bool speciesIs(const SpeciesDef& sd, const char* name){
  return sd.name && name && std::strcmp(sd.name, name)==0;
}

static inline float distToNearestWhirlpool(int x, int y){
  float best = 1e9f;
  for (const auto& wh : g_whirlpools){
    float dx = float(x - wh.x);
    float dy = float(y - wh.y);
    float d = std::sqrt(dx*dx + dy*dy) - wh.radius;
    if (d < best) best = d;
  }
  return best;
}

static inline float habitatSuitability(const World& w, const SpeciesDef& sd, int x, int y, int tick){
  if (!inBounds(x,y)) return 0.f;
  float water = w.water[y][x];
  float fert = std::clamp(w.fertility[y][x], 0.f, 1.f);
  float moist = std::clamp((float)w.moist[y][x] / 255.f, 0.f, 1.f);
  float alt = std::clamp((float)w.height[y][x] / 255.f, 0.f, 1.f);
  float landDry = 1.0f - std::clamp(water / 0.6f, 0.f, 1.f);
  float det = std::clamp(w.detritus[y][x], 0.f, 1.f);
  float s = 0.15f;
  bool shore = nearShore(w, x, y);
  Season season = seasonAt(tick);

  if (sd.aquatic) {
    if (water <= 0.2f) return 0.f;
    float depth = std::clamp(water / 3.0f, 0.f, 1.f);
    s += 0.45f * depth + 0.15f * moist + 0.15f * fert + 0.08f * det;
    if (shore) s += 0.08f;
    if (speciesIs(sd, "CRAB")) {
      s += shore ? 0.16f : -0.08f;
    } else if (speciesIs(sd, "TURTLE")) {
      if (water < 2.2f) s += 0.08f;
      if (season==SUMMER) s += 0.05f;
    } else if (speciesIs(sd, "EEL")) {
      if (water > 1.8f) s += 0.12f;
      if (shore) s -= 0.05f;
    } else if (speciesIs(sd, "FROG")) {
      s += shore ? 0.15f : -0.06f;
    } else if (sd.schooling) {
      s += 0.08f * std::clamp((water - 0.8f) / 1.4f, 0.f, 1.f);
    }
  } else {
    if (water > 0.6f) return 0.02f;
    float dry = landDry;
    s += 0.30f * dry + 0.25f * fert + 0.18f * moist + 0.10f * det;
    if (sd.herbivore && isEdiblePlant(w.terrain[y][x])) s += 0.20f;
    if (sd.carnivore) s += 0.08f * std::min(1.f, (float)nearbyTreeCount(w, x, y, 2) / 6.f);
    if (speciesIs(sd, "GOAT")) {
      s += 0.12f * alt;
    } else if (speciesIs(sd, "DEER")) {
      s += 0.08f * (1.0f - alt);
    } else if (speciesIs(sd, "RABBIT")) {
      if (shore) s -= 0.05f;
      if (fert > 0.55f) s += 0.08f;
    } else if (speciesIs(sd, "HAWK")) {
      s += 0.14f * alt;
    } else if (speciesIs(sd, "BOAR")) {
      s += 0.10f * moist;
    }
    if (season==WINTER) s -= 0.10f * alt;
    if (season==SUMMER) s += 0.06f * alt;
  }

  if (w.weather.state==STORM && !sd.aquatic) s -= 0.08f;
  if (w.weather.state==STORM && sd.aquatic) s += 0.03f;
  if (w.biome==DESERT && !sd.aquatic) s *= (0.75f + 0.30f*landDry);
  if (w.biome==TROPICAL && shore && sd.aquatic) s += 0.05f;
  return std::clamp(s, 0.f, 1.f);
}

static inline void updateRipples(float dt) {
  for (auto &r : g_ripples) r.t += dt;
  g_ripples.erase(std::remove_if(g_ripples.begin(), g_ripples.end(),
    [](const Ripple& r){ return r.t > 3.0f; }), g_ripples.end());
}

static inline void updateWhirlpools(float dt) {
  for (auto &w : g_whirlpools) w.t += dt;
  g_whirlpools.erase(std::remove_if(g_whirlpools.begin(), g_whirlpools.end(),
    [](const Whirlpool& w){ return w.t > w.life; }), g_whirlpools.end());
}

static inline char flowerForBiome(Biome b, Rng& r, uint32_t h);
static void triggerChaos(World& w, Rng& r, int cx, int cy){
  int variant = r.i(0,99);
  int type = variant % 10;
  int rad = 3 + (variant % 6);
  if (type==0) {
    // swirl/rotate terrain
    for (int i=0;i<rad; ++i){
      for (int y=cy-rad; y<=cy+rad; ++y){
        for (int x=cx-rad; x<=cx+rad; ++x){
          if (!inBounds(x,y)) continue;
          int nx = cx + (y - cy);
          int ny = cy - (x - cx);
          if (!inBounds(nx,ny)) continue;
          std::swap(w.terrain[y][x], w.terrain[ny][nx]);
        }
      }
    }
  } else if (type==1) {
    // displacement shuffle
    for (int y=cy-rad; y<=cy+rad; ++y){
      for (int x=cx-rad; x<=cx+rad; ++x){
        if (!inBounds(x,y)) continue;
        int nx = clampi(x + r.i(-2,2), 0, W-1);
        int ny = clampi(y + r.i(-2,2), 0, H-1);
        std::swap(w.terrain[y][x], w.terrain[ny][nx]);
      }
    }
  } else if (type==2) {
    // sinkhole + glyph spew
    for (int y=cy-rad; y<=cy+rad; ++y){
      for (int x=cx-rad; x<=cx+rad; ++x){
        if (!inBounds(x,y)) continue;
        if (r.oneIn(2)) w.terrain[y][x]='.';
        if (r.oneIn(3)) w.terrain[y][x]=',';
        if (r.oneIn(5)) w.terrain[y][x]='"';
        if (r.oneIn(8)) w.terrain[y][x]='f';
      }
    }
  } else if (type==3) {
    // bloom eruption
    for (int y=cy-rad; y<=cy+rad; ++y){
      for (int x=cx-rad; x<=cx+rad; ++x){
        if (!inBounds(x,y)) continue;
        if (w.water[y][x] > 0.2f) continue;
        if (r.oneIn(2)) w.terrain[y][x]=',';
        if (r.oneIn(4)) w.terrain[y][x]=';';
        if (r.oneIn(5)) w.terrain[y][x]=flowerForBiome(w.biome, r, w.seed);
      }
    }
  } else if (type==4) {
    // cat invasion (predators)
    int count = 3 + (variant % 5);
    for (int i=0;i<count; ++i){
      Agent a;
      a.id=(int)w.agents.size()+1;
      a.x=clampi(cx + r.i(-4,4),0,W-1);
      a.y=clampi(cy + r.i(-4,4),0,H-1);
      a.sp=SP_FOX;
      a.hunger=0.2f; a.thirst=0.2f; a.health=1.f;
      a.bold=0.8f; a.social=0.2f; a.curious=0.6f; a.aggro=0.9f;
      a.age=0; a.maxAge=200 + r.i(0,200);
      w.agents.push_back(a);
    }
  } else if (type==5) {
    // panic flood: animals run to water
    g_panicFloodTimer = 120;
  } else if (type==6) {
    // shapeshift
    g_shapeShiftTimer = 200;
  } else if (type==7) {
    // instant storm
    w.weather.state = STORM;
    w.weather.humidity = 0.9f;
    w.weather.pressure = 0.2f;
    w.weather.rainStrength = 1.0f;
    g_chaosStormTimer = 120;
  } else if (type==8) {
    // water burst
    for (int y=cy-rad; y<=cy+rad; ++y){
      for (int x=cx-rad; x<=cx+rad; ++x){
        if (!inBounds(x,y)) continue;
        w.water[y][x] = std::min(MAX_WATER, w.water[y][x] + 2.0f);
      }
    }
  } else {
    // glyph scramble
    const char opts[] = {'.',',','"',';','T','Y','#','d','f','+','&','!','m','a'};
    for (int y=cy-rad; y<=cy+rad; ++y){
      for (int x=cx-rad; x<=cx+rad; ++x){
        if (!inBounds(x,y)) continue;
        if (r.oneIn(2)) w.terrain[y][x] = opts[r.i(0,(int)(sizeof(opts)-1))];
      }
    }
  }
}

static void updateClouds(World& w, Rng& r){
  // drift
  for (auto &c : w.clouds){
    c.x += c.vx;
    c.y += c.vy;
    c.life--;
  }
  w.clouds.erase(std::remove_if(w.clouds.begin(), w.clouds.end(),
    [](const Cloud& c){ return c.life <= 0 || c.x < -20.f || c.x > W+20.f || c.y < -20.f || c.y > H+20.f; }), w.clouds.end());

  // occasional spawn
  if (r.oneIn(140)) {
    Cloud c;
    c.size = (float)r.i(4,10);
    c.y = (float)r.i(2, H/3);
    bool left = r.oneIn(2);
    c.x = left ? -c.size : (float)(W + c.size);
    float base = 0.03f + 0.05f * r.u01();
    c.vx = left ? base : -base;
    c.vy = (r.u01()-0.5f) * 0.02f;
    c.life = r.i(800, 1600);
    w.clouds.push_back(c);
  }

  // paint overlay
  for (int y=0;y<H;++y) std::fill(w.overlay[y].begin(), w.overlay[y].end(), ' ');
  for (const auto& c : w.clouds){
    int cx = (int)std::lround(c.x);
    int cy = (int)std::lround(c.y);
    int rad = (int)std::lround(c.size);
    for (int yy=cy-rad; yy<=cy+rad; ++yy){
      for (int xx=cx-rad; xx<=cx+rad; ++xx){
        if (!inBounds(xx,yy)) continue;
        int dx = xx - cx;
        int dy = yy - cy;
        if (dx*dx + dy*dy > rad*rad) continue;
        w.overlay[yy][xx] = 'o';
      }
    }
  }
}

static inline char flowerForBiome(Biome b, Rng& r, uint32_t h){
  (void)h;
  switch(b){
    case MEADOW:  return r.oneIn(3)?'f':'+';
    case WETLAND: return r.oneIn(2)?'&':'+';
    case ALPINE:  return r.oneIn(2)?'!':'&';
    case DESERT:  return r.oneIn(3)?'+':'f';
    case TROPICAL:return r.oneIn(2)?'f':'&';
    case TAIGA:   return r.oneIn(2)?'&':'f';
    case ALIEN:   return r.oneIn(2)?'!':'&';
    default: return 'f';
  }
}

static inline char flowerFromHash(Biome b, uint32_t h){
  (void)b;
  switch (h & 3u){
    case 0: return 'f';
    case 1: return '+';
    case 2: return '&';
    default:return '!';
  }
}

static inline bool canPlaceBigAt(const World& w, const BigDef& def, int x, int y){
  if (x < 0 || y < 0 || x + def.w > W || y + def.h > H) return false;
  for (int yy=y; yy<y+def.h; ++yy){
    for (int xx=x; xx<x+def.w; ++xx){
      if (def.aquatic && w.water[yy][xx] <= 0.2f) return false;
      if (!def.aquatic && w.water[yy][xx] > 0.2f) return false;
    }
  }
  return true;
}

static inline void stampBig(World& w, const BigCreature& b, char fill){
  for (int yy=b.y; yy<b.y+b.h; ++yy){
    for (int xx=b.x; xx<b.x+b.w; ++xx){
      if (!inBounds(xx,yy)) continue;
      w.entities[yy][xx] = fill;
    }
  }
}

static inline BigCreature makeBig(const BigDef& def, int x, int y, Rng& r){
  BigCreature b; b.x=x; b.y=y; b.w=def.w; b.h=def.h; b.glyph=def.glyph; b.aquatic=def.aquatic;
  b.moveCooldown = r.i(20, 80);
  return b;
}

static void spawnBigCreatureEdge(World& w, Rng& r){
  const BigPool& pool = g_bigPools[w.biome];
  if (pool.count <= 0) return;
  const BigDef& def = pool.defs[r.i(0, pool.count-1)];
  int tries=0;
  while(tries++ < 200){
    int side = r.i(0,3);
    int x = (side==0?0: side==1?W-def.w:r.i(0,W-def.w));
    int y = (side==2?0: side==3?H-def.h:r.i(0,H-def.h));
    if (!canPlaceBigAt(w, def, x, y)) continue;
    BigCreature b = makeBig(def, x, y, r);
    w.bigs.push_back(b);
    stampBig(w, b, b.glyph);
    return;
  }
}

static void stepBigCreatures(World& w, Rng& r){
  int maxB = (w.biome==TROPICAL) ? 8 : 6;
  if ((int)w.bigs.size() < maxB && r.oneIn(500)) {
    spawnBigCreatureEdge(w, r);
  }

  for (auto &b : w.bigs){
    if (b.moveCooldown > 0) { b.moveCooldown--; continue; }
    b.moveCooldown = r.i(15, 70);
    int dx = r.i(-1,1);
    int dy = r.i(-1,1);
    if (dx==0 && dy==0) continue;
    int nx = b.x + dx;
    int ny = b.y + dy;
    BigDef def{b.glyph, b.w, b.h, b.aquatic};
    if (!canPlaceBigAt(w, def, nx, ny)) continue;
    if (!b.aquatic) {
      for (int yy=b.y; yy<b.y+b.h; ++yy){
        for (int xx=b.x; xx<b.x+b.w; ++xx){
          if (!inBounds(xx,yy)) continue;
          w.detritus[yy][xx] = std::min(1.4f, w.detritus[yy][xx] + 0.010f);
          w.fertility[yy][xx] = std::max(0.05f, w.fertility[yy][xx] - 0.003f);
        }
      }
    }
    stampBig(w, b, ' ');
    b.x = nx; b.y = ny;
    stampBig(w, b, b.glyph);
  }
}

static inline char renderCharAtBase(const World& w, int x, int y, int tick){
  (void)tick;
  if (w.entities[y][x] != ' ') return w.entities[y][x];
  if (w.water[y][x] > 0.2f && (w.terrain[y][x] == 'm' || w.terrain[y][x] == 'a')) return w.terrain[y][x];
  if (w.water[y][x] > 0.2f) {
    for (const auto& wh : g_whirlpools){
      float dx = float(x - wh.x);
      float dy = float(y - wh.y);
      float dist = std::sqrt(dx*dx + dy*dy);
      if (dist <= wh.radius) {
        int phase = (tick/3 + x + y) & 6;
        return (char)(0x0F + phase);
      }
    }
    bool shore=false;
    for(int dy=-1;dy<=1 && !shore;++dy) for(int dx=-1;dx<=1 && !shore;++dx){
      if(!dx && !dy) continue;
      int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
      if (w.water[ny][nx] <= 0.2f) shore=true;
    }
    if (shore || (w.wind.strength>=2 && ((hash3(x,y,tick/5) % (uint32_t)(20 - 3*w.wind.strength))==0u))) return '=';
    return waterFlowGlyph(w, x, y, tick);
  }
  return w.terrain[y][x];
}

static inline char renderCharAt(const World& w, int x, int y, int tick){
  // chaos visuals from ripples
  for (const auto& r : g_ripples){
    float rx = float(x - r.cx);
    float ry = float(y - r.cy);
    float dist = std::sqrt(rx*rx + ry*ry);
    float ring = r.speed * r.t;
    float d = std::fabs(dist - ring);
    if (d < r.width + 1.0f) {
      uint32_t h = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(r.seed + tick));
      if (r.mode==3) {
        const char opts[] = {'.',',','"',';','T','Y','#','d','f','+','&','!','m','a','t','l','n','q'};
        return opts[h % (uint32_t)(sizeof(opts))];
      } else if (r.mode==4) {
        return flowerFromHash(w.biome, h);
      } else if (r.mode==5) {
        return (h & 1u) ? '^' : 'd';
      } else if (r.mode==6) {
        return (h & 1u) ? '!' : '&';
      } else if (r.mode==7) {
        return (h & 1u) ? 'x' : '*';
      } else if (r.mode==8) {
        return (h & 1u) ? 'm' : 'a';
      } else if (r.mode==9) {
        return (h & 1u) ? 't' : 'n';
      }
    }
  }
  // Agents are not displaced by ripples.
  for (const auto& a : w.agents){
    if (a.x==x && a.y==y) {
      if (g_shapeShiftTimer > 0) {
        const char morphs[] = {'r','d','g','f','c','p','w','b','e','v','x','o','u','h','s','A','Z'};
        uint32_t h = hash3((uint32_t)a.x,(uint32_t)a.y,(uint32_t)(tick + a.id*31));
        return morphs[h % (uint32_t)(sizeof(morphs))];
      }
      return g_species[a.sp].glyph;
    }
  }
  int dx=0, dy=0;
  for (const auto& r : g_ripples){
    float rx = float(x - r.cx);
    float ry = float(y - r.cy);
    float dist = std::sqrt(rx*rx + ry*ry);
    float ring = r.speed * r.t;
    float d = std::fabs(dist - ring);
    if (d < r.width) {
      float s = (1.0f - d / r.width) * r.amp;
      float inv = (dist > 0.001f) ? (1.0f / dist) : 0.0f;
      if (r.mode==1) {
        // spiral
        float ang = std::atan2(ry, rx) + r.t*1.2f;
        dx += (int)std::lround(std::cos(ang) * s);
        dy += (int)std::lround(std::sin(ang) * s);
      } else if (r.mode==2) {
        // jittered shock
        uint32_t h = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(r.seed + tick));
        dx += (int)((int)(h & 3u) - 1) * (int)std::lround(s);
        dy += (int)((int)((h>>2)&3u) - 1) * (int)std::lround(s);
      } else {
        dx += (int)std::lround(rx * inv * s);
        dy += (int)std::lround(ry * inv * s);
      }
    }
  }
  int sx = clampi(x+dx,0,W-1);
  int sy = clampi(y+dy,0,H-1);
  return renderCharAtBase(w, sx, sy, tick);
}

static inline const Agent* hoveredAgentAt(const World& w, int wx, int wy){
  const Agent* best = nullptr;
  int bestD = 999;
  for (const auto& a : w.agents) {
    int d = std::abs(a.x - wx) + std::abs(a.y - wy);
    if (d < bestD && d <= 1) {
      best = &a;
      bestD = d;
      if (d == 0) break;
    }
  }
  return best;
}

static inline const BigCreature* hoveredBigAt(const World& w, int wx, int wy){
  for (const auto& b : w.bigs) {
    if (wx >= b.x && wx < b.x + b.w && wy >= b.y && wy < b.y + b.h) return &b;
  }
  return nullptr;
}

static inline void clearInspectPin(){
  g_inspectPinned = false;
  g_inspectPinnedIsBig = false;
  g_inspectPinnedAgentId = -1;
  g_inspectPinnedBigX = -1;
  g_inspectPinnedBigY = -1;
  g_inspectPinnedBigGlyph = ' ';
}

static inline const char* bigCreatureName(char g){
  switch(g){
    case 'M': return "MAMMOTH";
    case 'B': return "BEHEMOTH";
    case 'W': return "WHALE";
    case 'C': return "CROCODILE";
    case 'G': return "GOLEM";
    case 'Y': return "YAK LORD";
    case 'D': return "DUNE TITAN";
    case 'S': return "SAND WYRM";
    case 'H': return "HYDRA";
    case 'T': return "TREANT";
    case 'K': return "KRAKEN";
    case 'R': return "RAY GIANT";
    case 'E': return "ELK KING";
    case 'X': return "XENOBEAST";
    case 'Q': return "QUASAR EEL";
    default: return "COLOSSUS";
  }
}

// ===== World generation =====
static void genHeight(World& w, Rng& r){
  w.height.assign(H, std::vector<uint8_t>(W, 0));
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      uint32_t h = hash3((uint32_t)x,(uint32_t)y,r.u32());
      int v = (int)(h % 256u);
      w.height[y][x] = (uint8_t)v;
    }
  }
}

static void seedWorld(World& w, Rng& r, Biome biome){
  w.seed = r.u32();
  w.biome = biome;
  w.targetBiome = biome;
  w.biomeMorphActive = false;
  w.biomeMorphT = 0.f;
  w.terrain.assign(H, std::string(W, '.'));
  w.entities.assign(H, std::string(W, ' '));
  w.water.assign(H, std::vector<float>(W, 0.f));
  w.waterBase.assign(H, std::vector<float>(W, 0.f));
  w.waterNext.assign(H, std::vector<float>(W, 0.f));
  w.oceanDelta.assign(H, std::vector<float>(W, 0.f));
  w.coreLand.assign(H, std::vector<uint8_t>(W, 0));
  w.shoreLand.assign(H, std::vector<uint8_t>(W, 0));
  w.waterDeltas.clear();
  w.waterDeltaY0.clear();
  w.waterDeltaY1.clear();
  w.waterDeltaThreads = 0;
  w.fertility.assign(H, std::vector<float>(W, 0.6f));
  w.sediment.assign(H, std::vector<float>(W, 0.02f));
  w.detritus.assign(H, std::vector<float>(W, 0.02f));
  w.overlay.assign(H, std::string(W, ' '));
  w.moist.assign(H, std::vector<uint8_t>(W, 80));
  w.agents.clear();
  w.agents.reserve(MAX_AGENTS);
  w.prevPos.clear();
  g_whirlpools.clear();
  w.bigs.clear();
  w.events.clear();
  genHeight(w, r);
  applyBiomeModPreset(biome, false);
  w.weather.humidity = g_biomeTune[biome].humidityBase;
  w.weather.pressure = g_biomeTune[biome].pressureBase;

  // base terrain + fertility
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      uint8_t alt = w.height[y][x];
      if (biome==DESERT) w.terrain[y][x]='s';
      if (alt > 240) w.terrain[y][x]='^';
      float f = 0.55f + (float)(alt)/255.f * 0.15f;
      if (biome==DESERT) f *= 0.55f;
      if (biome==WETLAND) f *= 1.15f;
      w.fertility[y][x] = std::clamp(f, 0.1f, 1.0f);
      if (biome==DESERT) w.sediment[y][x] = 0.01f;
      if (biome==WETLAND || biome==TROPICAL) w.sediment[y][x] = 0.04f;
      if (isVeg(w.terrain[y][x])) w.detritus[y][x] = 0.03f;
    }
  }

  if (biome==TROPICAL) {
    const int seaLevel = 160;
    // start with a broad ocean
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        int alt = (int)w.height[y][x];
        if (alt < seaLevel) {
          float depth = std::clamp((seaLevel - alt) / 25.0f, 0.5f, MAX_WATER);
          w.water[y][x] = depth;
          w.fertility[y][x] = std::max(0.25f, w.fertility[y][x] * 0.6f);
        } else {
          w.terrain[y][x] = '.';
          w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + 0.15f);
        }
      }
    }
    // carve islands
    int islandN = std::max(6, (W*H)/2800);
    for (int i=0; i<islandN; ++i){
      int cx=r.i(12,W-13), cy=r.i(10,H-11);
      int rad=r.i(6,18);
      for(int y=cy-rad; y<=cy+rad; ++y){
        for(int x=cx-rad; x<=cx+rad; ++x){
          if(!inBounds(x,y)) continue;
          float dx = (float)(x - cx);
          float dy = (float)(y - cy);
          float d = std::sqrt(dx*dx + dy*dy);
          if (d > rad) continue;
          float k = 1.0f - (d / (float)rad);
          if (k > 0.2f) {
            w.water[y][x] = 0.f;
            w.terrain[y][x] = '.';
            w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + 0.25f*k);
            if (k > 0.45f) w.coreLand[y][x] = 1;
            else if (k > 0.2f) w.shoreLand[y][x] = 1;
          } else {
            if (w.water[y][x] > 0.f) w.terrain[y][x] = 's';
          }
        }
      }
    }
    // expand soft shore ring
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        if (!w.coreLand[y][x]) continue;
        for(int dy=-2; dy<=2; ++dy) for(int dx=-2; dx<=2; ++dx){
          int nx=x+dx, ny=y+dy;
          if(!inBounds(nx,ny)) continue;
          if (!w.coreLand[ny][nx]) w.shoreLand[ny][nx] = 1;
        }
      }
    }
    // add reef patches (shallow water) for wave breaks + lagoons
    int reefN = std::max(5, (W*H)/4200);
    for (int i=0; i<reefN; ++i){
      int cx=r.i(6,W-7), cy=r.i(6,H-7);
      int rad=r.i(3,7);
      for(int y=cy-rad; y<=cy+rad; ++y){
        for(int x=cx-rad; x<=cx+rad; ++x){
          if(!inBounds(x,y)) continue;
          float dx = (float)(x - cx);
          float dy = (float)(y - cy);
          float d = std::sqrt(dx*dx + dy*dy);
          if (d > rad) continue;
          if (w.coreLand[y][x]) continue;
          if (w.water[y][x] <= 0.2f) continue;
          w.water[y][x] = std::min(w.water[y][x], 1.2f);
          w.waterBase[y][x] = std::min(w.waterBase[y][x], 0.9f);
        }
      }
    }
  }

  // ponds
  if (biome!=TROPICAL) {
    int ponds = std::max(4, (W*H)/9000);
    for(int p=0;p<ponds;++p){
      int cx=r.i(10,W-11), cy=r.i(8,H-9);
      int rad=r.i(6,14);
      for(int y=cy-rad;y<=cy+rad;++y) for(int x=cx-rad;x<=cx+rad;++x){
        if(!inBounds(x,y)) continue;
        int dx=x-cx, dy=y-cy; int d2=dx*dx+dy*dy;
        if(d2>rad*rad) continue;
        float depth = std::max(1.f, 6.f - (float)d2/std::max(1,rad));
        w.water[y][x] = std::max(w.water[y][x], depth);
      }
    }
  }
  w.waterBase = w.water;

  // initial flora
  for(int y=0;y<H;++y) for(int x=0;x<W;++x){
    if (w.water[y][x]>0) continue;
    uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
    if (h % 29u == 0u) w.terrain[y][x] = ',';
    if (h % 97u == 0u) w.terrain[y][x] = '"';
    if (h % 211u == 0u) w.terrain[y][x] = ';';
    if (h % 379u == 0u) w.terrain[y][x] = (r.oneIn(2)?'T':'Y');
    if (h % 521u == 0u) w.terrain[y][x] = flowerForBiome(biome, r, h);
  }

  // initial big creatures
  if (r.oneIn(2)) spawnBigCreatureEdge(w, r);
  if (r.oneIn(4)) spawnBigCreatureEdge(w, r);

  // seed starting agents
  for(int i=0;i<START_AGENTS;++i){
    SpeciesId sp = g_biomes[biome].herb[r.i(0,(int)g_biomes[biome].herb.size()-1)];
    int tries=0;
    while(tries++<400){
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (g_species[sp].aquatic && w.water[y][x] <= 0.2f) continue;
      if (!g_species[sp].aquatic && w.water[y][x] > 0.4f) continue;
      float suit = habitatSuitability(w, g_species[sp], x, y, 0);
      if (suit < 0.24f && !r.oneIn(12)) continue;
      Agent a; a.id = (int)w.agents.size()+1; a.x=x; a.y=y; a.sp=sp;
      a.goalX = x; a.goalY = y; a.goalTTL = 0;
      a.hunger=r.u01()*0.2f; a.thirst=r.u01()*0.2f; a.health=1.0f;
      a.fatigue=r.u01()*0.2f; a.stress=r.u01()*0.2f; a.emotion=0.6f;
      a.bold=0.3f+0.7f*r.u01();
      a.social=0.3f+0.7f*r.u01();
      a.curious=0.3f+0.7f*r.u01();
      a.aggro=0.2f+0.8f*r.u01();
      a.age = r.i(0,400);
      a.maxAge = 1400 + r.i(0,800);
      w.agents.push_back(a);
      break;
    }
  }
}

// ===== Weather =====
static void updateWeather(World& w, Rng& r){
  w.weather.timer++;
  float humid = w.weather.humidity;
  float press = w.weather.pressure;
  const BiomeTuning& bt = g_biomeTune[w.biome];
  // humidity drift
  humid = clamp01(humid + (r.u01()-0.5f)*0.02f + (bt.humidityBase - humid)*0.04f);
  press = clamp01(press + (r.u01()-0.5f)*0.01f + (bt.pressureBase - press)*0.03f);

  float rainT = std::clamp(0.55f - 0.15f*bt.rainBias, 0.35f, 0.70f);
  float stormT = std::clamp(0.70f - 0.20f*bt.rainBias, 0.50f, 0.85f);
  if (humid > stormT && press < 0.45f) w.weather.state = (r.oneIn(4)?STORM:RAIN);
  else if (humid > rainT) w.weather.state = OVERCAST;
  else w.weather.state = CLEAR;

  w.weather.humidity = humid;
  w.weather.pressure = press;
  float rainBoost = std::clamp(0.7f + 0.6f*bt.rainBias, 0.4f, 1.3f);
  w.weather.rainStrength = (w.weather.state==RAIN || w.weather.state==STORM) ? (0.4f + 0.6f*humid) * rainBoost : 0.f;
  w.weather.cloudOpacity = std::clamp(0.25f + humid*0.85f, 0.2f, 1.0f);

  if (w.weather.state==STORM && r.oneIn(120)) {
    Event ev; ev.type=Event::EV_LIGHTNING; ev.mag=1.0f; w.events.push_back(ev);
  }
}

static void updateWind(World& w, Rng& r){
  const BiomeTuning& bt = g_biomeTune[w.biome];
  int roll = (int)std::lround(50.f / std::max(0.4f, bt.windBias));
  roll = std::clamp(roll, 20, 70);
  if (r.oneIn(roll)) {
    int t = r.i(0,7);
    w.wind.dx = (t%3)-1;
    w.wind.dy = ((t/3)%3)-1;
    w.wind.strength = std::min(5, r.i(0,5) + (int)std::lround(bt.windBias-1.0f));
  }
  if (w.weather.state==STORM) w.wind.strength = std::max(w.wind.strength, 3);

  if (g_windGustTimer > 0) {
    g_windGustTimer--;
    g_windGust = std::max(0.f, g_windGust * 0.97f);
  } else if (r.oneIn(std::max(18, roll/2))) {
    g_windGustTimer = r.i(20, 80);
    g_windGust = 0.6f + r.u01() * 1.4f;
  }
  if (w.weather.state==STORM) g_windGust = std::max(g_windGust, 1.0f);
}

// ===== Water simulation =====
static void ensureWaterBuffers(World& w, int simThreads){
  if ((int)w.waterNext.size() != H || (w.waterNext.empty() || (int)w.waterNext[0].size() != W)) {
    w.waterNext.assign(H, std::vector<float>(W, 0.f));
  }
  if (w.waterDeltaThreads != simThreads || (int)w.waterDeltas.size() != simThreads) {
    w.waterDeltas.clear();
    w.waterDeltaY0.clear();
    w.waterDeltaY1.clear();
    w.waterDeltas.resize(simThreads);
    w.waterDeltaY0.resize(simThreads);
    w.waterDeltaY1.resize(simThreads);
    int chunk = (H + simThreads - 1) / simThreads;
    for (int t=0; t<simThreads; ++t){
      int y0 = t * chunk;
      int y1 = std::min(H, y0 + chunk);
      w.waterDeltaY0[t] = y0;
      w.waterDeltaY1[t] = y1;
      int rows = std::max(0, y1 - y0);
      w.waterDeltas[t].assign(rows, std::vector<float>(W, 0.f));
    }
    w.waterDeltaThreads = simThreads;
  } else {
    int chunk = (H + simThreads - 1) / simThreads;
    bool reinit = false;
    for (int t=0; t<simThreads; ++t){
      int y0 = t * chunk;
      int y1 = std::min(H, y0 + chunk);
      int rows = std::max(0, y1 - y0);
      if (w.waterDeltaY0[t] != y0 || w.waterDeltaY1[t] != y1) { reinit = true; break; }
      if ((int)w.waterDeltas[t].size() != rows) { reinit = true; break; }
      if (rows > 0 && (int)w.waterDeltas[t][0].size() != W) { reinit = true; break; }
    }
    if (reinit) {
      w.waterDeltas.clear();
      w.waterDeltaY0.clear();
      w.waterDeltaY1.clear();
      w.waterDeltas.resize(simThreads);
      w.waterDeltaY0.resize(simThreads);
      w.waterDeltaY1.resize(simThreads);
      for (int t=0; t<simThreads; ++t){
        int y0 = t * chunk;
        int y1 = std::min(H, y0 + chunk);
        w.waterDeltaY0[t] = y0;
        w.waterDeltaY1[t] = y1;
        int rows = std::max(0, y1 - y0);
        w.waterDeltas[t].assign(rows, std::vector<float>(W, 0.f));
      }
    } else {
      for (int t=0; t<simThreads; ++t){
        auto& d = w.waterDeltas[t];
        for (int y=0; y<(int)d.size(); ++y){
          std::fill(d[y].begin(), d[y].end(), 0.f);
        }
      }
    }
  }
}

static void stepWater(World& w, Rng& r){
  const float maxMass = MAX_WATER;
  const BiomeTuning& bt = g_biomeTune[w.biome];

  const int threads = std::max(1, std::min(g_threads, MAX_THREADS));
  const int simThreads = std::min(threads, H);
  ensureWaterBuffers(w, simThreads);
  WaterF& next = w.waterNext;
  std::vector<WaterF>& deltas = w.waterDeltas;
  int chunk = (H + simThreads - 1) / simThreads;
  std::vector<std::vector<std::tuple<int,int,float>>> spills(simThreads);

  parallelForRange(H, simThreads, [&](int y0, int y1, int tid){
    auto& d = deltas[tid];
    int baseY = w.waterDeltaY0[tid];
    for(int y=y0;y<y1;++y){
      int ly = y - baseY;
      for(int x=0;x<W;++x){
        float here = w.water[y][x];
        if (here <= 0.0001f) continue;

        // flow to 4 neighbors
        float remaining = here;
        const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        for(int i=0;i<4;++i){
          int nx=x+dirs[i][0], ny=y+dirs[i][1];
          if(!inBounds(nx,ny)) continue;
          float nwater = w.water[ny][nx];
          float diff = (here - nwater) / 4.f;
          if (diff <= 0.001f) continue;
          float flow = std::min(diff * FLOW_RATE, remaining);
          if (flow <= 0.f) continue;
          remaining -= flow;
          d[ly][x] -= flow;
          int lny = ny - baseY;
          if (lny >= 0 && lny < (int)d.size()) {
            d[lny][nx] += flow;
          } else {
            spills[tid].push_back({ny, nx, flow});
          }
        }
      }
    }
  });

  for (int t=0; t<simThreads; ++t){
    for (const auto& sp : spills[t]){
      int ny = std::get<0>(sp);
      int nx = std::get<1>(sp);
      float flow = std::get<2>(sp);
      int owner = std::clamp(ny / chunk, 0, simThreads-1);
      int lny = ny - w.waterDeltaY0[owner];
      // if boundary rounding placed us in the wrong chunk, try neighbours
      if (lny < 0 && owner > 0) { owner--; lny = ny - w.waterDeltaY0[owner]; }
      else if (lny >= (int)deltas[owner].size() && owner < simThreads-1) { owner++; lny = ny - w.waterDeltaY0[owner]; }
      if (lny >= 0 && lny < (int)deltas[owner].size()) {
        deltas[owner][lny][nx] += flow;
      }
    }
  }

  parallelForRange(H, simThreads, [&](int y0, int y1, int tid){
    (void)tid;
    for(int y=y0;y<y1;++y){
      for(int x=0;x<W;++x){
        float v = w.water[y][x];
        for (int t=0; t<simThreads; ++t) {
          int dy0 = w.waterDeltaY0[t];
          int dy1 = w.waterDeltaY1[t];
          if (y < dy0 || y >= dy1) continue;
          v += deltas[t][y - dy0][x];
        }
        next[y][x] = std::clamp(v, 0.f, maxMass);
      }
    }
  });

  // ripple influence on water height (more consequence)
  for (const auto& rp : g_ripples){
    // only some ripple modes physically disturb water (rarer)
    if (!(rp.mode==0 || rp.mode==1)) continue;
    int cx = rp.cx, cy = rp.cy;
    int rad = (int)std::lround(rp.speed * rp.t);
    for(int y=cy-rad-2; y<=cy+rad+2; ++y){
      for(int x=cx-rad-2; x<=cx+rad+2; ++x){
        if(!inBounds(x,y)) continue;
        float dx = (float)(x - cx);
        float dy = (float)(y - cy);
        float dist = std::sqrt(dx*dx + dy*dy);
        float ring = rp.speed * rp.t;
        float d = std::fabs(dist - ring);
        if (d < rp.width+0.5f) {
          float s = (1.0f - d / (rp.width+0.5f)) * rp.amp * 0.18f;
          next[y][x] = std::clamp(next[y][x] + s, 0.f, maxMass);
        }
      }
    }
  }

  // rainfall
  if (w.weather.state==RAIN || w.weather.state==STORM) {
    int hits = (int)((W*H) * w.weather.rainStrength * 0.02f);
    for(int i=0;i<hits;++i){
      int x=0,y=0;
      if (!w.clouds.empty()) {
        const auto& c = w.clouds[r.i(0, (int)w.clouds.size()-1)];
        int rad = std::max(2, (int)std::lround(c.size));
        bool placed=false;
        for (int tries=0; tries<4 && !placed; ++tries){
          int rx = r.i(-rad, rad);
          int ry = r.i(-rad, rad);
          if (rx*rx + ry*ry > rad*rad) continue;
          x = clampi((int)std::lround(c.x) + rx, 0, W-1);
          y = clampi((int)std::lround(c.y) + ry, 0, H-1);
          placed=true;
        }
        if (!placed) { x=r.i(0,W-1); y=r.i(0,H-1); }
      } else {
        x=r.i(0,W-1); y=r.i(0,H-1);
      }
      next[y][x] = std::min(maxMass, next[y][x] + RAIN_RATE);
    }
    Event ev; ev.type=Event::EV_RAIN; ev.mag=w.weather.rainStrength; w.events.push_back(ev);
  }

  // seep back towards base basins
  for(int y=0;y<H;++y) for(int x=0;x<W;++x){
    float base = w.waterBase[y][x];
    if (base > 0.f && next[y][x] < base) {
      next[y][x] = std::min(base, next[y][x] + 0.02f);
    }
  }

  // tropical hard land + soft shore retention
  if (w.biome==TROPICAL) {
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        if (w.coreLand[y][x]) {
          next[y][x] = 0.f;
        } else if (w.shoreLand[y][x]) {
          if (next[y][x] < 0.9f) next[y][x] = 0.f;
        }
      }
    }
  }

  if (w.biome==TROPICAL && r.oneIn(400)) {
    int tries=0;
    while(tries++<200){
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (next[y][x] > 1.0f) {
        Whirlpool wh; wh.x=x; wh.y=y; wh.t=0.f; wh.life=6.f + r.u01()*4.f; wh.radius=4.5f + r.u01()*3.5f;
        g_whirlpools.push_back(wh);
        if ((int)g_whirlpools.size() > 6) g_whirlpools.erase(g_whirlpools.begin());
        break;
      }
    }
  }

  // whirlpool sinks (tropical)
  if (w.biome==TROPICAL && !g_whirlpools.empty()) {
    for (const auto& wh : g_whirlpools){
      int rad = (int)std::ceil(wh.radius);
      float removed = 0.f;
      for(int y=wh.y-rad; y<=wh.y+rad; ++y){
        for(int x=wh.x-rad; x<=wh.x+rad; ++x){
          if(!inBounds(x,y)) continue;
          float dx = float(x - wh.x);
          float dy = float(y - wh.y);
          float dist = std::sqrt(dx*dx + dy*dy);
          if (dist > wh.radius) continue;
          float k = (1.0f - dist / wh.radius);
          float sink = 0.02f * k;
          float take = std::min(sink, next[y][x]);
          next[y][x] -= take;
          removed += take;
        }
      }
      if (removed > 0.f) {
        // push outward ring
        for(int y=wh.y-rad; y<=wh.y+rad; ++y){
          for(int x=wh.x-rad; x<=wh.x+rad; ++x){
            if(!inBounds(x,y)) continue;
            float dx = float(x - wh.x);
            float dy = float(y - wh.y);
            float dist = std::sqrt(dx*dx + dy*dy);
            if (dist < wh.radius*0.6f || dist > wh.radius*1.05f) continue;
            next[y][x] = std::min(maxMass, next[y][x] + removed * 0.0012f);
          }
        }
      }
    }
  }

  // tropical tides (slow global shore breathing)
  if (w.biome==TROPICAL) {
    float tide = 0.35f * std::sin((float)w.weather.timer * 0.01f);
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        if (w.coreLand[y][x]) continue;
        if (w.shoreLand[y][x]) {
          next[y][x] = std::clamp(next[y][x] + tide * 0.15f, 0.f, maxMass);
        } else {
          next[y][x] = std::clamp(next[y][x] + tide * 0.03f, 0.f, maxMass);
        }
      }
    }
  }
  // wetland bog pulse (subtle breathing water)
  if (w.biome==WETLAND) {
    float pulse = 0.18f * std::sin((float)w.weather.timer * 0.02f);
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        if (next[y][x] > 0.2f && next[y][x] < 2.0f) {
          next[y][x] = std::clamp(next[y][x] + pulse * 0.08f, 0.f, maxMass);
        }
      }
    }
  }

  // tropical ocean current (gentle, continuous drift)
  if (w.biome==TROPICAL) {
    for(int y=0;y<H;++y) std::fill(w.oceanDelta[y].begin(), w.oceanDelta[y].end(), 0.f);
    float phase = (float)(w.weather.timer % 1000) / 1000.f;
    float vx = std::cos(phase * 6.2831853f);
    float vy = std::sin(phase * 5.026548f);
    if (w.wind.strength > 0) {
      vx = 0.6f*vx + 0.4f*(float)w.wind.dx;
      vy = 0.6f*vy + 0.4f*(float)w.wind.dy;
    }
    int cdx = (vx > 0.2f) ? 1 : (vx < -0.2f ? -1 : 0);
    int cdy = (vy > 0.2f) ? 1 : (vy < -0.2f ? -1 : 0);
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        float wv = next[y][x];
        if (wv <= 0.2f) continue;
        int nx = clampi(x + cdx, 0, W-1);
        int ny = clampi(y + cdy, 0, H-1);
        if (nx==x && ny==y) continue;
        float flow = wv * 0.02f;
        w.oceanDelta[y][x] -= flow;
        w.oceanDelta[ny][nx] += flow;
      }
    }
    for(int y=0;y<H;++y){
      for(int x=0;x<W;++x){
        next[y][x] = std::clamp(next[y][x] + w.oceanDelta[y][x], 0.f, maxMass);
      }
    }
  }

  // wind-driven coastal surge / backwash for less "grid-perfect" shore motion
  if (w.wind.strength > 0 && (w.wind.dx != 0 || w.wind.dy != 0)) {
    int wx = w.wind.dx;
    int wy = w.wind.dy;
    int px = -wy, py = wx;
    float surf = COASTAL_SURGE_RATE * (0.70f + 0.22f * (float)w.wind.strength);
    if (w.weather.state==STORM) surf *= 1.35f;
    if (w.biome==TROPICAL) surf *= 1.25f;

    for (int y=1; y<H-1; ++y){
      for (int x=1; x<W-1; ++x){
        if (next[y][x] <= 0.2f) continue;
        int upx = x - wx, upy = y - wy;
        int dnx = x + wx, dny = y + wy;
        if (!inBounds(upx,upy) || !inBounds(dnx,dny)) continue;
        if (next[upy][upx] <= 0.2f) continue;
        if (next[dny][dnx] > 0.2f) continue;
        float push = std::min(std::max(0.f, next[upy][upx] - 0.2f), surf);
        if (push <= 0.f) continue;

        next[upy][upx] = std::max(0.f, next[upy][upx] - push);
        next[y][x] = std::min(maxMass, next[y][x] + push * 0.78f);
        w.sediment[y][x] = std::min(1.4f, w.sediment[y][x] + push * 0.35f);

        // lateral swash adds curled shore motion, especially in tropical surf.
        int sx1 = x + px, sy1 = y + py;
        int sx2 = x - px, sy2 = y - py;
        float side = push * ((w.biome==TROPICAL)?0.20f:0.12f);
        if (inBounds(sx1,sy1) && next[sy1][sx1] > 0.2f) {
          next[sy1][sx1] = std::min(maxMass, next[sy1][sx1] + side);
        }
        if (inBounds(sx2,sy2) && next[sy2][sx2] > 0.2f) {
          next[sy2][sx2] = std::min(maxMass, next[sy2][sx2] + side*0.8f);
        }
      }
    }
  }

  // evaporation (less aggressive, moisture-aware)
  for(int y=0;y<H;++y) for(int x=0;x<W;++x){
    float m = (float)w.moist[y][x] / 255.f;
    float evap = EVAP_RATE*0.35f*(1.2f - 0.7f*m)*bt.evapMult;
    if (next[y][x] > 0.f) next[y][x] = std::max(0.f, next[y][x] - evap);
  }

  // sediment transport + shoreline shaping
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      float waterHere = next[y][x];
      float sed = std::clamp(w.sediment[y][x], 0.f, 1.4f);
      if (waterHere <= 0.05f) {
        float settle = std::min(sed, SEDIMENT_DEPOSIT_RATE * 0.6f);
        sed -= settle;
        if (settle > 0.f) {
          w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + settle * 0.35f);
          w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + settle * 0.08f);
        }
        w.sediment[y][x] = sed;
        continue;
      }

      float grad = 0.f;
      int wetN = 0;
      bool shore = false;
      const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
      for (int i=0; i<4; ++i){
        int nx = x + dirs[i][0];
        int ny = y + dirs[i][1];
        if (!inBounds(nx,ny)) continue;
        float nw = next[ny][nx];
        if (nw > 0.2f) {
          wetN++;
          grad += std::fabs(waterHere - nw);
        } else {
          shore = true;
        }
      }

      float wave = 0.f;
      if (w.biome==TROPICAL) {
        float tidePulse = 0.5f + 0.5f * std::sin((float)w.weather.timer * 0.021f + x*0.09f + y*0.07f);
        wave = tidePulse * (0.18f + 0.06f * (float)w.wind.strength);
      } else {
        wave = 0.03f * (float)w.wind.strength;
      }
      float flowEnergy = ((wetN>0)?(grad / (float)wetN):0.f) + wave;
      float erode = SEDIMENT_ERODE_RATE * flowEnergy;
      if (shore) erode *= 1.35f;
      if (w.weather.state==STORM) erode *= 1.25f;

      float fertTake = std::min(std::max(0.f, w.fertility[y][x] - 0.05f), erode * 0.22f);
      w.fertility[y][x] = std::max(0.05f, w.fertility[y][x] - fertTake);
      sed += erode + fertTake * 1.5f;

      float calm = std::max(0.f, 0.30f - flowEnergy);
      float deposit = std::min(sed, SEDIMENT_DEPOSIT_RATE * (0.6f + calm * 3.0f));
      if (shore) deposit *= 1.8f;
      if (w.biome==TROPICAL && w.shoreLand[y][x]) deposit *= 1.8f;
      sed -= deposit;

      if (deposit > 0.f) {
        w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + deposit * 0.26f);
        w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + deposit * 0.10f);
        if (shore) {
          float shoal = deposit * ((w.biome==TROPICAL) ? 7.0f : 2.5f);
          next[y][x] = std::max(0.f, next[y][x] - shoal);
        }
        if (w.biome==TROPICAL && !w.coreLand[y][x] && shore && next[y][x] < 0.12f && deposit > 0.0025f) {
          w.shoreLand[y][x] = 1;
          if (w.terrain[y][x] == 's' || w.terrain[y][x] == '.') w.terrain[y][x] = '.';
        }
      }
      if (w.biome==TROPICAL && w.shoreLand[y][x]) {
        w.waterBase[y][x] = std::min(w.waterBase[y][x], 0.55f);
        if (next[y][x] < 1.05f) next[y][x] = 0.f;
      }

      w.sediment[y][x] = std::clamp(sed, 0.f, 1.4f);
    }
  }

  // update soil moisture
  for(int y=0;y<H;++y) for(int x=0;x<W;++x){
    float waterN = next[y][x];
    float target = clamp01((waterN / maxMass) + w.weather.humidity * 0.25f);
    float m = (float)w.moist[y][x] / 255.f;
    float rate = (waterN > 0.2f) ? 0.08f : 0.03f;
    m = m*(1.0f - rate) + target*rate;
    w.moist[y][x] = (uint8_t)clampi((int)std::lround(m*255.f), 0, 255);
  }

  w.water.swap(next);
}

static void stepNutrientCycle(World& w){
  WaterF detNext = w.detritus;
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      float det = std::clamp(w.detritus[y][x], 0.f, 1.4f);
      float moist = (float)w.moist[y][x] / 255.f;
      float water = std::clamp(w.water[y][x] / MAX_WATER, 0.f, 1.f);
      char t = w.terrain[y][x];

      if (isVeg(t) && w.water[y][x] <= 0.3f) det += 0.0008f;
      if (t=='x') det += 0.0022f;

      float decay = det * (0.006f + DETRITUS_DECAY_RATE * (0.45f + 0.55f*moist) * (0.8f + 0.4f*water));
      det = std::max(0.f, det - decay);

      float fertGain = decay * (0.35f + 0.65f*moist);
      if (w.water[y][x] > 1.2f) fertGain *= 0.65f;
      w.fertility[y][x] = std::clamp(w.fertility[y][x] + fertGain, 0.05f, 1.0f);

      if (w.water[y][x] <= 0.2f && moist < 0.25f && det < 0.015f && !isVeg(t)) {
        w.fertility[y][x] = std::max(0.05f, w.fertility[y][x] - 0.0007f);
      }

      detNext[y][x] = std::clamp(det, 0.f, 1.4f);
    }
  }

  WaterF detSmooth = detNext;
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      float sum = detNext[y][x];
      int n = 1;
      const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
      for (int i=0; i<4; ++i){
        int nx = x + dirs[i][0];
        int ny = y + dirs[i][1];
        if (!inBounds(nx,ny)) continue;
        sum += detNext[ny][nx];
        n++;
      }
      float avg = sum / (float)n;
      float water = std::clamp(w.water[y][x] / MAX_WATER, 0.f, 1.f);
      float mix = DETRITUS_DIFFUSE_RATE * (0.18f + 0.62f*water);
      detSmooth[y][x] = std::clamp(detNext[y][x] + (avg - detNext[y][x]) * mix, 0.f, 1.4f);
    }
  }

  w.detritus.swap(detSmooth);
}

static void spawnEcoAgent(World& w, Rng& r, SpeciesId sp, int x, int y, int tick){
  if ((int)w.agents.size() >= MAX_AGENTS) return;
  if (!inBounds(x,y)) return;
  const SpeciesDef& sd = g_species[sp];
  if (sd.aquatic && w.water[y][x] <= 0.2f) return;
  if (!sd.aquatic && w.water[y][x] > 0.4f) return;
  float suit = habitatSuitability(w, sd, x, y, tick);
  if (suit < 0.35f && !r.oneIn(8)) return;
  Agent a;
  a.id = (int)w.agents.size() + 1;
  a.x = x; a.y = y; a.sp = sp;
  a.goalX = x; a.goalY = y; a.goalTTL = 0;
  a.hunger = r.u01()*0.25f;
  a.thirst = r.u01()*0.25f;
  a.fatigue = r.u01()*0.2f;
  a.stress = r.u01()*0.2f;
  a.health = 0.85f + r.u01()*0.15f;
  a.emotion = 0.6f;
  a.bold = 0.3f + 0.7f*r.u01();
  a.social = 0.3f + 0.7f*r.u01();
  a.curious = 0.3f + 0.7f*r.u01();
  a.aggro = 0.2f + 0.8f*r.u01();
  a.age = r.i(0,250);
  a.maxAge = 1400 + r.i(0,800);
  w.agents.push_back(a);
}

static void stepBiomeEcoEvents(World& w, Rng& r, int tick){
  Season s = seasonAt(tick);
  if (w.biome==TROPICAL) {
    // Deep-water upwelling: nutrient bloom, water plants, and fish pulses.
    if (r.oneIn(220)) {
      int cx=0, cy=0;
      bool found=false;
      for (int tries=0; tries<200 && !found; ++tries){
        int x = r.i(0, W-1), y = r.i(0, H-1);
        if (w.water[y][x] > 1.8f && !nearShore(w, x, y)) { cx=x; cy=y; found=true; }
      }
      if (found) {
        int rad = r.i(3,7);
        for (int y=cy-rad; y<=cy+rad; ++y){
          for (int x=cx-rad; x<=cx+rad; ++x){
            if (!inBounds(x,y)) continue;
            float dx = (float)(x-cx), dy = (float)(y-cy);
            float d = std::sqrt(dx*dx + dy*dy);
            if (d > (float)rad) continue;
            float k = 1.0f - d / (float)rad;
            if (w.water[y][x] <= 0.2f) continue;
            w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.08f*k);
            w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + 0.05f*k);
            w.sediment[y][x] = std::min(1.4f, w.sediment[y][x] + 0.02f*k);
            if (w.water[y][x] < 2.4f && (w.terrain[y][x]==' ' || w.terrain[y][x]=='.' || w.terrain[y][x]==',')) {
              if (r.oneIn(4)) w.terrain[y][x] = r.oneIn(2) ? 'a' : 'm';
            }
          }
        }
        spawnEcoAgent(w, r, SP_FISH, cx, cy, tick);
        if (r.oneIn(2)) spawnEcoAgent(w, r, SP_CRAB, clampi(cx+r.i(-2,2),0,W-1), clampi(cy+r.i(-2,2),0,H-1), tick);
        Ripple rp; rp.cx=cx; rp.cy=cy; rp.t=0.f; rp.amp=2.2f; rp.speed=10.f; rp.width=2.2f; rp.mode=1; rp.seed=w.seed ^ (uint32_t)tick;
        g_ripples.push_back(rp);
      }
    }
    // Reef nursery pulse near shore
    if (r.oneIn(260)) {
      for (int tries=0; tries<120; ++tries){
        int x = r.i(1, W-2), y = r.i(1, H-2);
        if (!nearShore(w,x,y) || w.water[y][x] <= 0.2f || w.water[y][x] > 1.9f) continue;
        for (int dy=-2; dy<=2; ++dy){
          for (int dx=-2; dx<=2; ++dx){
            int nx=x+dx, ny=y+dy;
            if (!inBounds(nx,ny) || w.water[ny][nx] <= 0.2f) continue;
            if (r.oneIn(4)) w.terrain[ny][nx] = (r.oneIn(2)?'m':'a');
            w.detritus[ny][nx] = std::min(1.4f, w.detritus[ny][nx] + 0.02f);
          }
        }
        break;
      }
    }
  } else if (w.biome==DESERT) {
    // Wind-driven dust squalls.
    if (s==SUMMER && r.oneIn(220)) {
      int x = r.i(0,W-1), y = r.i(0,H-1);
      int dx = (w.wind.dx==0 && w.wind.dy==0) ? (r.oneIn(2)?1:-1) : w.wind.dx;
      int dy = (w.wind.dx==0 && w.wind.dy==0) ? 0 : w.wind.dy;
      for (int i=0; i<110; ++i){
        if (!inBounds(x,y)) break;
        if (w.water[y][x] <= 0.2f) {
          w.moist[y][x] = (uint8_t)clampi((int)w.moist[y][x] - 10, 0, 255);
          w.fertility[y][x] = std::max(0.05f, w.fertility[y][x] - 0.010f);
          w.sediment[y][x] = std::min(1.4f, w.sediment[y][x] + 0.05f);
          if (w.terrain[y][x]=='.' && r.oneIn(3)) w.terrain[y][x]='s';
          if (w.terrain[y][x]==',' && r.oneIn(2)) w.terrain[y][x]='.';
        }
        x += dx + r.i(-1,1);
        y += dy + r.i(-1,1);
      }
    }
  } else if (w.biome==ALPINE) {
    // Spring snowmelt rivulets.
    if (s==SPRING && r.oneIn(240)) {
      int sx=0, sy=0;
      bool found=false;
      for (int tries=0; tries<180 && !found; ++tries){
        int x=r.i(1,W-2), y=r.i(1,H-2);
        if (w.height[y][x] > 220 && w.water[y][x] < 0.3f) { sx=x; sy=y; found=true; }
      }
      if (found) {
        int x=sx, y=sy;
        for (int step=0; step<32; ++step){
          w.water[y][x] = std::min(MAX_WATER, w.water[y][x] + 0.26f);
          w.waterBase[y][x] = std::max(w.waterBase[y][x], 0.18f);
          w.moist[y][x] = (uint8_t)clampi((int)w.moist[y][x] + 10, 0, 255);
          int bx=x, by=y;
          int bestAlt = (int)w.height[y][x];
          for (int dy=-1; dy<=1; ++dy){
            for (int dx=-1; dx<=1; ++dx){
              if (!dx && !dy) continue;
              int nx=x+dx, ny=y+dy;
              if (!inBounds(nx,ny)) continue;
              int alt = (int)w.height[ny][nx];
              if (alt < bestAlt) { bestAlt = alt; bx=nx; by=ny; }
            }
          }
          if (bx==x && by==y) break;
          x=bx; y=by;
        }
      }
    }
  } else if (w.biome==WETLAND) {
    if (r.oneIn(250)) {
      int cx=r.i(2,W-3), cy=r.i(2,H-3);
      if (w.water[cy][cx] > 0.2f && w.water[cy][cx] < 1.8f) {
        int rad=r.i(2,4);
        for(int y=cy-rad; y<=cy+rad; ++y){
          for(int x=cx-rad; x<=cx+rad; ++x){
            if(!inBounds(x,y) || w.water[y][x] <= 0.2f) continue;
            if (r.oneIn(3)) w.terrain[y][x] = 'a';
            if (r.oneIn(5)) w.terrain[y][x] = 'm';
            w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.03f);
          }
        }
      }
    }
  } else if (w.biome==TAIGA) {
    if (r.oneIn(300)) {
      int x=r.i(1,W-2), y=r.i(1,H-2);
      if (isTree(w.terrain[y][x])) {
        for (int dy=-3; dy<=3; ++dy){
          for (int dx=-3; dx<=3; ++dx){
            int nx=x+dx, ny=y+dy;
            if (!inBounds(nx,ny) || w.water[ny][nx] > 0.2f) continue;
            if (w.terrain[ny][nx]=='.' && r.oneIn(4)) w.terrain[ny][nx]='l';
            w.fertility[ny][nx] = std::min(1.0f, w.fertility[ny][nx] + 0.015f);
          }
        }
      }
    }
  } else if (w.biome==MEADOW) {
    if (s==SPRING && r.oneIn(220)) {
      int cx=r.i(2,W-3), cy=r.i(2,H-3);
      int rad=r.i(3,6);
      for (int y=cy-rad; y<=cy+rad; ++y){
        for (int x=cx-rad; x<=cx+rad; ++x){
          if (!inBounds(x,y) || w.water[y][x] > 0.2f) continue;
          if (w.terrain[y][x]=='.' && r.oneIn(3)) w.terrain[y][x]=',';
          if (w.terrain[y][x]==',' && r.oneIn(4)) w.terrain[y][x]=flowerForBiome(w.biome, r, hash3((uint32_t)x,(uint32_t)y,w.seed));
          w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + 0.01f);
        }
      }
      spawnEcoAgent(w, r, SP_RABBIT, cx, cy, tick);
    }
  } else if (w.biome==ALIEN) {
    if (r.oneIn(240)) {
      int cx=r.i(2,W-3), cy=r.i(2,H-3);
      int rad=r.i(2,5);
      for (int y=cy-rad; y<=cy+rad; ++y){
        for (int x=cx-rad; x<=cx+rad; ++x){
          if (!inBounds(x,y) || w.water[y][x] > 0.2f) continue;
          if (r.oneIn(3)) w.terrain[y][x]='q';
          w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.02f);
        }
      }
      Ripple rp; rp.cx=cx; rp.cy=cy; rp.t=0.f; rp.amp=2.0f; rp.speed=14.f; rp.width=2.5f; rp.mode=3; rp.seed=w.seed ^ ((uint32_t)tick*13u);
      g_ripples.push_back(rp);
      if ((int)w.agents.size() < MAX_AGENTS && r.oneIn(2)) spawnEcoAgent(w, r, SP_ALIEN1, cx, cy, tick);
    }
  }
}

// ===== Terrain + plants =====
static void stepTerrain(World& w, Rng& r, Season s){
  Grid next = w.terrain;
  const BiomeTuning& bt = g_biomeTune[w.biome];
  float drought = (w.weather.state==CLEAR && s==SUMMER) ? 1.5f : 1.0f;
  drought *= bt.droughtMult;
  float growthMul = bt.plantGrowthMult;
  float flowerMul = bt.flowerMult;

  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      if (w.water[y][x] > 0.2f) continue;
      char c = w.terrain[y][x];
      float fert = w.fertility[y][x];
      float moist = (float)w.moist[y][x] / 255.f;
      // rain -> mud near water
      if ((w.weather.state==RAIN || w.weather.state==STORM) && c=='.') {
        bool wet=false;
        for(int dy=-1;dy<=1 && !wet;++dy) for(int dx=-1;dx<=1 && !wet;++dx){
          if(!dx && !dy) continue;
          int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
          if (w.water[ny][nx] > 0.2f) wet=true;
        }
        if (wet && r.oneIn(12)) next[y][x]='d';
      }
      // drought kills
      if (c==',' || c=='"' || c==';' || c=='#' || isFlower(c)) {
        float dryMul = (1.1f - moist);
        if (r.u01() < 0.0008f * drought * (1.2f - fert) * dryMul) next[y][x]='.';
      }

      // growth
      if (c=='.') {
        if (r.u01() < (0.0015f / drought) * (0.6f + fert) * (0.4f + moist) * growthMul) next[y][x]=',';
        if (w.biome==TROPICAL && w.shoreLand[y][x] && r.oneIn(40)) next[y][x]='.';
      } else if (c=='d') {
        if (w.weather.state==CLEAR && r.oneIn(40)) next[y][x]='.';
      } else if (c==',') {
        if (r.u01() < 0.0012f * (0.5f + moist) * growthMul) next[y][x]='"';
        if (r.u01() < 0.0007f * (0.5f + moist) * growthMul) next[y][x]=';';
        float flowerChance = 0.0004f;
        if (w.biome==MEADOW) flowerChance=0.0010f;
        else if (w.biome==TROPICAL) flowerChance=0.0008f;
        else if (w.biome==WETLAND) flowerChance=0.0006f;
        else if (w.biome==ALPINE) flowerChance=0.0005f;
        else if (w.biome==TAIGA) flowerChance=0.0004f;
        else if (w.biome==DESERT) flowerChance=0.0002f;
        else if (w.biome==ALIEN) flowerChance=0.0012f;
        if (r.u01() < (flowerChance / drought) * (0.5f + fert) * (0.6f + moist) * flowerMul) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
          next[y][x] = flowerForBiome(w.biome, r, h);
        }
      } else if (c=='"') {
        if (r.u01() < 0.0009f * (0.6f + fert) * (0.6f + moist) * growthMul) next[y][x]='#';
        if (r.u01() < 0.0003f / drought * (0.6f + fert) * (0.6f + moist) * flowerMul) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
          next[y][x] = flowerForBiome(w.biome, r, h);
        }
      } else if (c==';') {
        if (r.u01() < 0.0002f / drought * (0.6f + fert) * (0.6f + moist) * flowerMul) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
          next[y][x] = flowerForBiome(w.biome, r, h);
        }
      }
      // fertility recovery
      if (c=='.' && w.weather.state==RAIN) w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + bt.fertRain);
      if (c=='.' && w.weather.state==CLEAR) w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + bt.fertClear);

      char out = next[y][x];
      if (isVeg(c) && !isVeg(out)) {
        float biomass = isTree(c) ? 0.06f : (isFlower(c) ? 0.045f : 0.03f);
        w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + biomass);
      }
      if (!isVeg(c) && isVeg(out)) {
        w.fertility[y][x] = std::max(0.05f, w.fertility[y][x] - 0.006f);
      }
      if (out=='d' && w.detritus[y][x] > 0.12f && r.oneIn(35)) {
        next[y][x] = ',';
      }
    }
  }
  w.terrain.swap(next);
}

static void stepWaterPlants(World& w, Rng& r){
  Grid next = w.terrain;
  const BiomeTuning& bt = g_biomeTune[w.biome];
  bool wet = (w.biome==WETLAND || w.biome==TROPICAL);
  int lilyCount = countChar(w.terrain, 'm');
  int lilyCap = wet ? (int)((W*H)/280 * bt.waterPlantMult) : (int)((W*H)/900 * bt.waterPlantMult);
  int aplantCount = countChar(w.terrain, 'a');
  int aplantCap = wet ? (int)((W*H)/260 * bt.waterPlantMult) : (int)((W*H)/800 * bt.waterPlantMult);
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      float water = w.water[y][x];
      if (water <= 0.2f) {
        if (next[y][x]=='m' || next[y][x]=='a') {
          next[y][x]=' ';
          w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.025f);
        }
        continue;
      }
      if (water > 3.5f && (next[y][x]=='m' || next[y][x]=='a')) {
        next[y][x]=' ';
        w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.020f);
        continue;
      }

      if (lilyCount < lilyCap && (wet || r.oneIn((int)std::max(600.f, 1200.f / bt.waterPlantMult)))) {
        if (water < 2.6f && (next[y][x]==' ' || next[y][x]=='.')) {
          if (r.oneIn(300)) { next[y][x]='m'; lilyCount++; }
        }
      }
      if (aplantCount < aplantCap && wet) {
        if (water < 2.2f && (next[y][x]==' ' || next[y][x]=='.')) {
          if (r.oneIn(260)) { next[y][x]='a'; aplantCount++; }
        }
      }
      if (next[y][x]=='m') {
        int nx=x, ny=y;
        if (w.wind.strength>=1 && r.oneIn(6)) {
          nx = clampi(x + w.wind.dx, 0, W-1);
          ny = clampi(y + w.wind.dy, 0, H-1);
        } else if (r.oneIn(10)) {
          // drift toward nearby lower water level
          float best = water;
          int bnx=x, bny=y;
          for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
            if (!dx && !dy) continue;
            int tx=x+dx, ty=y+dy; if(!inBounds(tx,ty)) continue;
            float wv = w.water[ty][tx];
            if (wv > 0.2f && wv < best) { best = wv; bnx=tx; bny=ty; }
          }
          nx=bnx; ny=bny;
        }
        if ((nx!=x || ny!=y) && w.water[ny][nx] > 0.2f && next[ny][nx] != 'm') {
          next[ny][nx] = 'm';
          next[y][x] = ' ';
        }
      }
      if (next[y][x]=='a') {
        int nx=x, ny=y;
        if (w.wind.strength>=1 && r.oneIn(5)) {
          nx = clampi(x + w.wind.dx, 0, W-1);
          ny = clampi(y + w.wind.dy, 0, H-1);
        } else if (r.oneIn(8)) {
          float best = water;
          int bnx=x, bny=y;
          for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
            if (!dx && !dy) continue;
            int tx=x+dx, ty=y+dy; if(!inBounds(tx,ty)) continue;
            float wv = w.water[ty][tx];
            if (wv > 0.2f && wv < best) { best = wv; bnx=tx; bny=ty; }
          }
          nx=bnx; ny=bny;
        }
        if ((nx!=x || ny!=y) && w.water[ny][nx] > 0.2f && next[ny][nx] != 'a') {
          next[ny][nx] = 'a';
          next[y][x] = ' ';
        }
      }
    }
  }
  w.terrain.swap(next);
}

static void stepBiomeSpecials(World& w, Rng& r){
  Grid next = w.terrain;
  if (w.biome==DESERT) {
    int tumbleCount = countChar(w.terrain, 't');
    int cap = (W*H)/500;
    if (tumbleCount < cap && r.oneIn(200)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.water[y][x] <= 0.2f && (w.terrain[y][x]=='.' || w.terrain[y][x]=='s')) next[y][x]='t';
    }
    for(int y=0;y<H;++y) for(int x=0;x<W;++x){
      if (w.terrain[y][x]=='t' && w.wind.strength>=1 && r.oneIn(3)) {
        int nx = clampi(x + w.wind.dx, 0, W-1);
        int ny = clampi(y + w.wind.dy, 0, H-1);
        if (w.water[ny][nx] <= 0.2f && next[ny][nx] != 't') { next[ny][nx]='t'; next[y][x]='.'; }
      }
      if (w.terrain[y][x]=='.' && w.wind.strength>=2 && r.oneIn(400)) {
        next[y][x] = 's';
      }
    }
  } else if (w.biome==ALPINE) {
    if (r.oneIn(600)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='^') next[y][x]='l';
    }
    if (seasonAt(w.weather.timer)==WINTER && r.oneIn(500)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='^') next[y][x]='.';
    }
  } else if (w.biome==TROPICAL) {
    if (r.oneIn(350)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]==';' || w.terrain[y][x]=='"') next[y][x]='n';
    }
    if (r.oneIn(260)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='.') {
        bool shore=false;
        for(int dy=-1;dy<=1 && !shore;++dy) for(int dx=-1;dx<=1 && !shore;++dx){
          if(!dx && !dy) continue;
          int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
          if (w.water[ny][nx] > 0.2f) shore=true;
        }
        if (shore) next[y][x] = r.oneIn(2) ? 'Y' : 'T';
      }
    }
  } else if (w.biome==TAIGA) {
    if (r.oneIn(700)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='T' || w.terrain[y][x]=='Y') next[y][x]='l';
    }
    if (r.oneIn(500)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='.') {
        bool nearTree=false;
        for(int dy=-2;dy<=2 && !nearTree;++dy) for(int dx=-2;dx<=2 && !nearTree;++dx){
          int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
          if (isTree(w.terrain[ny][nx])) nearTree=true;
        }
        if (nearTree) next[y][x]='l';
      }
    }
  } else if (w.biome==ALIEN) {
    if (r.oneIn(400)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='.') next[y][x]='q';
    }
  } else if (w.biome==WETLAND) {
    if (r.oneIn(260)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]==',' || w.terrain[y][x]=='"') next[y][x]='#';
    }
  } else if (w.biome==MEADOW) {
    if (r.oneIn(300)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]==',') {
        uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
        next[y][x] = flowerForBiome(w.biome, r, h);
      }
    }
  }
  w.terrain.swap(next);
}

// ===== Fire =====
static void stepFire(World& w, Rng& r){
  Grid next = w.terrain;
  const BiomeTuning& bt = g_biomeTune[w.biome];
  float dry = std::clamp(1.0f - w.weather.humidity, 0.0f, 1.0f);
  float wind = 1.0f + 0.15f * (float)w.wind.strength;
  float spreadChance = std::clamp(0.18f + dry*0.35f, 0.05f, 0.7f) * wind * bt.fireMult;
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      if (w.terrain[y][x]=='*') {
        // burn neighbors
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
          if (!dx && !dy) continue;
          int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
          if (isVeg(w.terrain[ny][nx]) && r.u01() < spreadChance) {
            next[ny][nx]='*';
            w.detritus[ny][nx] = std::min(1.4f, w.detritus[ny][nx] + 0.010f);
          }
        }
        // turn to ash
        if (r.oneIn(3)) {
          next[y][x]='x';
          w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.040f);
          w.fertility[y][x] = std::min(1.0f, w.fertility[y][x] + 0.015f);
        }
        if (r.oneIn(4)) { Event ev; ev.type=Event::EV_FIRE; ev.x=x; ev.y=y; ev.mag=1.f; w.events.push_back(ev); }
      }
      if (w.terrain[y][x]=='x' && r.oneIn(40)) {
        next[y][x]='.';
        w.detritus[y][x] = std::min(1.4f, w.detritus[y][x] + 0.008f);
      }
    }
  }
  w.terrain.swap(next);
}

// ===== Agents =====
static void stepAgents(World& w, Rng& r, int tick){
  const BiomeTuning& bt = g_biomeTune[w.biome];
  // immigration
  if ((int)w.agents.size() < MAX_AGENTS) {
    int denom = (int)std::max(20.f, 120.f / std::max(0.1f, g_alea.spawnChance));
    if (r.oneIn(denom)) {
    bool aquatic = (w.biome==TROPICAL) ? (r.i(0,9) < 6) : r.oneIn(2);
    SpeciesId sp;
    if (aquatic) sp = g_biomes[w.biome].aqua[r.i(0,(int)g_biomes[w.biome].aqua.size()-1)];
    else sp = g_biomes[w.biome].herb[r.i(0,(int)g_biomes[w.biome].herb.size()-1)];
    int tries=0;
    while(tries++<200){
      int side = r.i(0,3);
      int x = (side==0?0: side==1?W-1:r.i(0,W-1));
      int y = (side==2?0: side==3?H-1:r.i(0,H-1));
      if (g_species[sp].aquatic && w.water[y][x] <= 0.2f) continue;
      if (!g_species[sp].aquatic && w.water[y][x] > 0.4f) continue;
      float suit = habitatSuitability(w, g_species[sp], x, y, tick);
      if (suit < 0.28f && !r.oneIn(10)) continue;
      Agent a; a.id=(int)w.agents.size()+1; a.x=x; a.y=y; a.sp=sp; a.health=1.f;
      a.goalX = x; a.goalY = y; a.goalTTL = 0;
      a.hunger=r.u01()*0.3f; a.thirst=r.u01()*0.3f; a.fatigue=r.u01()*0.3f; a.stress=r.u01()*0.2f; a.emotion=0.6f;
      a.bold=0.3f+0.7f*r.u01();
      a.social=0.3f+0.7f*r.u01();
      a.curious=0.3f+0.7f*r.u01();
      a.aggro=0.2f+0.8f*r.u01();
      a.age = r.i(0,300);
      a.maxAge = 1400 + r.i(0,800);
      w.agents.push_back(a);
      break;
    }
    }
  }

  struct AgentView {
    int x=0, y=0;
    SpeciesId sp=SP_FISH;
    bool carnivore=false;
    bool herbivore=false;
    bool aquatic=false;
    bool schooling=false;
    bool pack=false;
  };
  struct AgentSense {
    int predDist=999;
    int schoolCx=0, schoolCy=0, schoolN=1;
    int packCx=0, packCy=0, packN=1;
  };

  std::vector<AgentView> views;
  views.reserve(w.agents.size());
  for (const auto& a : w.agents){
    const SpeciesDef &sd = g_species[a.sp];
    AgentView v;
    v.x = a.x; v.y = a.y; v.sp = a.sp;
    v.carnivore = sd.carnivore;
    v.herbivore = sd.herbivore;
    v.aquatic = sd.aquatic;
    v.schooling = sd.schooling;
    v.pack = sd.pack;
    views.push_back(v);
  }

  const int BUCKET = 4;
  const int BW = (W + BUCKET - 1) / BUCKET;
  const int BH = (H + BUCKET - 1) / BUCKET;
  auto bucketIndex = [&](int x,int y){ return (y/BUCKET)*BW + (x/BUCKET); };
  auto buildBuckets = [&](const std::vector<AgentView>& v){
    std::vector<std::vector<int>> buckets(BW*BH);
    for (int i=0;i<(int)v.size();++i){
      buckets[bucketIndex(v[i].x, v[i].y)].push_back(i);
    }
    return buckets;
  };
  auto bucketsViews = buildBuckets(views);
  auto forEachNearby = [&](const std::vector<AgentView>& v, const std::vector<std::vector<int>>& buckets,
                           int x,int y,int radius, const auto& fn){
    int minx = std::max(0, x - radius);
    int maxx = std::min(W-1, x + radius);
    int miny = std::max(0, y - radius);
    int maxy = std::min(H-1, y + radius);
    int minbx = minx / BUCKET;
    int maxbx = maxx / BUCKET;
    int minby = miny / BUCKET;
    int maxby = maxy / BUCKET;
    for (int by=minby; by<=maxby; ++by){
      for (int bx=minbx; bx<=maxbx; ++bx){
        const auto& b = buckets[by*BW + bx];
        for (int idx : b){
          const auto& o = v[idx];
          int d = std::abs(o.x - x) + std::abs(o.y - y);
          if (d <= radius) fn(idx);
        }
      }
    }
  };

  std::vector<AgentSense> sense(views.size());
  if (!views.empty()) {
    const int threads = std::max(1, std::min(g_threads, MAX_THREADS));
    const int n = (int)views.size();
    const int agentThreads = std::min(threads, n);
    parallelForRange(n, agentThreads, [&](int i0, int i1, int tid){
      (void)tid;
      for (int i=i0;i<i1;++i){
        const auto& a = views[i];
        AgentSense s;
        // predator distance (only need within 4 for stress)
        if (!a.carnivore) {
          int best = 999;
          forEachNearby(views, bucketsViews, a.x, a.y, 4, [&](int idx){
            const auto& p = views[idx];
            if (!p.carnivore) return;
            int d = std::abs(p.x-a.x)+std::abs(p.y-a.y);
            if (d < best) best = d;
          });
          s.predDist = best;
        }
        // schooling center
        if (a.schooling) {
          int cx=a.x, cy=a.y, nsum=1;
          forEachNearby(views, bucketsViews, a.x, a.y, 4, [&](int idx){
            if (idx==i) return;
            const auto& o = views[idx];
            if (o.sp != a.sp) return;
            cx += o.x; cy += o.y; nsum++;
          });
          s.schoolCx = cx / nsum;
          s.schoolCy = cy / nsum;
          s.schoolN = nsum;
        } else {
          s.schoolCx = a.x; s.schoolCy = a.y; s.schoolN = 1;
        }
        // pack center
        if (a.pack) {
          int cx=a.x, cy=a.y, nsum=1;
          forEachNearby(views, bucketsViews, a.x, a.y, 5, [&](int idx){
            if (idx==i) return;
            const auto& o = views[idx];
            if (o.sp != a.sp) return;
            cx += o.x; cy += o.y; nsum++;
          });
          s.packCx = cx / nsum;
          s.packCy = cy / nsum;
          s.packN = nsum;
        } else {
          s.packCx = a.x; s.packCy = a.y; s.packN = 1;
        }
        sense[i] = s;
      }
    });
  }

  std::vector<Agent> newborns;
  newborns.reserve(8);
  const size_t agentN = w.agents.size();
  for (size_t ai=0; ai<agentN; ++ai){
    auto &a = w.agents[ai];
    const SpeciesDef &sd = g_species[a.sp];
    a.age++;
    a.hunger = clamp01(a.hunger + sd.hungerRate * bt.agentMetab);
    a.thirst = clamp01(a.thirst + sd.thirstRate * bt.agentMetab);
    a.stress = clamp01(a.stress + 0.01f * (a.hunger + a.thirst));
    a.fatigue = clamp01(a.fatigue + 0.004f);

    // drink
    if (a.thirst > 0.7f) {
      float waterHere = w.water[a.y][a.x];
      if (sd.aquatic && waterHere > 0.1f) {
        a.thirst = std::max(0.f, a.thirst - 0.5f);
        a.stress = clamp01(a.stress - 0.06f);
        Event ev; ev.type=Event::EV_DRINK; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
      } else if (!sd.aquatic && waterHere > 0.2f) {
        a.thirst = std::max(0.f, a.thirst - 0.4f);
        a.stress = clamp01(a.stress - 0.05f);
        Event ev; ev.type=Event::EV_DRINK; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
      }
    }

    // eat
    if (sd.herbivore && isEdiblePlant(w.terrain[a.y][a.x])) {
      a.hunger = std::max(0.f, a.hunger - 0.4f);
      w.terrain[a.y][a.x] = '.';
      w.fertility[a.y][a.x] = std::max(0.f, w.fertility[a.y][a.x] - 0.02f);
      w.detritus[a.y][a.x] = std::min(1.4f, w.detritus[a.y][a.x] + 0.018f);
      a.stress = clamp01(a.stress - 0.08f);
      Event ev; ev.type=Event::EV_EAT; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
    }

    // predator proximity stress + fear
    int predDist = 999;
    if (!sd.carnivore && ai < sense.size()) {
      predDist = sense[ai].predDist;
      if (predDist <= 4) {
        float fear = (4 - predDist) / 4.0f;
        float fearAdj = fear * (1.2f - a.bold);
        a.stress = clamp01(a.stress + 0.06f * fearAdj);
      }
    }

    // update emotion from state
    float emoTarget = clamp01(0.6f*(1.0f-a.stress) + 0.2f*(1.0f-a.hunger) + 0.2f*(1.0f-a.thirst));
    a.emotion = smooth1(a.emotion, emoTarget, 0.05f);

    // schooling cohesion (fish/alien2)
    int schoolCx = a.x, schoolCy = a.y, schoolN = 1;
    if (sd.schooling && ai < sense.size()) {
      schoolCx = sense[ai].schoolCx;
      schoolCy = sense[ai].schoolCy;
      schoolN = sense[ai].schoolN;
    }

    // pack cohesion for pack predators
    int packCx = a.x, packCy = a.y, packN = 1;
    if (sd.pack && ai < sense.size()) {
      packCx = sense[ai].packCx;
      packCy = sense[ai].packCy;
      packN = sense[ai].packN;
    }

    // occasional speed burst / hunt sprint
    float sprintChance = 0.02f + 0.08f * a.aggro + 0.05f * (a.stress);
    bool sprint = r.u01() < sprintChance;

    if (a.goalTTL > 0) a.goalTTL--;

    auto countSameNear = [&](int nx, int ny, int radius){
      int count = 0;
      forEachNearby(views, bucketsViews, nx, ny, radius, [&](int idx){
        if (idx==(int)ai) return;
        if (views[idx].sp == a.sp) {
          count++;
        }
      });
      return count;
    };

    float suitHere = habitatSuitability(w, sd, a.x, a.y, tick);
    if (suitHere < 0.18f) a.stress = clamp01(a.stress + 0.02f);

    // occasional long-range migration scan
    if (a.goalTTL <= 2 || !inBounds(a.goalX, a.goalY) || (a.x==a.goalX && a.y==a.goalY)) {
      float bestSuit = suitHere;
      int bestGX = a.x, bestGY = a.y;
      int samples = sd.aquatic ? 16 : 12;
      int span = sd.aquatic ? 42 : 34;
      for (int s=0; s<samples; ++s){
        int tx = clampi(a.x + r.i(-span, span), 0, W-1);
        int ty = clampi(a.y + r.i(-span, span), 0, H-1);
        if (!sd.aquatic && w.water[ty][tx] > 0.4f) continue;
        if (sd.aquatic && w.water[ty][tx] <= 0.2f) continue;
        float hs = habitatSuitability(w, sd, tx, ty, tick);
        if (hs > bestSuit + 0.07f) {
          bestSuit = hs;
          bestGX = tx;
          bestGY = ty;
        }
      }
      if (bestGX != a.x || bestGY != a.y) {
        a.goalX = bestGX;
        a.goalY = bestGY;
        a.goalTTL = 20 + r.i(0,28);
      }
    }

    // move toward target (allow stay to rest)
    int bestDx=0,bestDy=0;
    float bestScore=-1e9f;
    for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
      int nx=a.x+dx, ny=a.y+dy; if(!inBounds(nx,ny)) continue;
      if (!sd.aquatic && w.water[ny][nx] > 0.4f) continue;
      if (sd.aquatic && w.water[ny][nx] <= 0.2f) continue;
      float score=0.f;
      bool staying = (dx==0 && dy==0);
      if (sd.herbivore && isEdiblePlant(w.terrain[ny][nx])) score += 3.f;
      if (sd.carnivore) {
        int preyNear = 0;
        forEachNearby(views, bucketsViews, nx, ny, 2, [&](int idx){
          if (idx==(int)ai) return;
          if (!views[idx].herbivore) return;
          preyNear++;
        });
        if (preyNear > 0) {
          score += (2.0f + a.aggro + (sd.pack?1.0f:0.0f)) * (float)preyNear;
        }
      }
      // habitat suitability gradient
      float suitNext = habitatSuitability(w, sd, nx, ny, tick);
      score += (suitNext - suitHere) * 8.0f;
      if (suitNext > 0.70f) score += 1.5f;
      if (suitNext < 0.12f) score -= 2.0f;
      if (sd.aquatic) {
        float detN = std::clamp(w.detritus[ny][nx], 0.f, 1.4f);
        float fertN = std::clamp(w.fertility[ny][nx], 0.f, 1.f);
        score += detN * 2.0f + fertN * 0.9f;
        if (w.terrain[ny][nx]=='m' || w.terrain[ny][nx]=='a') {
          score += sd.herbivore ? 1.8f : 0.5f;
        }
        if (!g_whirlpools.empty()) {
          float wh = distToNearestWhirlpool(nx, ny);
          if (speciesIs(sd, "EEL")) {
            if (wh > 0.3f && wh < 2.2f) score += 1.4f;
            if (wh < -0.2f) score -= 2.0f;
          } else {
            if (wh < 0.f) score -= 4.0f;
            else if (wh < 2.0f) score -= 1.5f;
          }
        }
      } else if (w.weather.state==STORM && nearShore(w, nx, ny)) {
        score -= 0.8f;
      }

      // seasonal migration: winter -> lower alt, summer -> higher alt
      if (!sd.aquatic) {
        float altHere = w.height[a.y][a.x] / 255.f;
        float altNext = w.height[ny][nx] / 255.f;
        if (seasonAt(tick)==WINTER && altNext < altHere) score += 1.2f;
        if (seasonAt(tick)==SUMMER && altNext > altHere) score += 1.2f;
      }

      // thirst seeking
      if (a.thirst > 0.6f) {
        if (w.water[ny][nx] > 0.2f) score += 2.f;
      }
      if (g_panicFloodTimer > 0 && !sd.aquatic) {
        if (w.water[ny][nx] > 0.2f) score += 4.f;
      }
      // rest when fatigued
      if (a.fatigue > 0.7f && staying) score += 3.f;
      if (a.fatigue > 0.8f && !staying) score -= 2.f;
      if (sd.schooling) {
        int dcur = std::abs(a.x-schoolCx)+std::abs(a.y-schoolCy);
        int dnext = std::abs(nx-schoolCx)+std::abs(ny-schoolCy);
        if (dnext < dcur) score += 2.2f;
        if (dnext > dcur) score -= 1.0f;
      }
      if (sd.pack) {
        int dcur = std::abs(a.x-packCx)+std::abs(a.y-packCy);
        int dnext = std::abs(nx-packCx)+std::abs(ny-packCy);
        if (dnext < dcur) score += 2.0f;
        if (dnext > dcur) score -= 1.0f;
      }
      // goal memory to reduce jitter
      if (a.goalTTL > 0) {
        int dcur = std::abs(a.x-a.goalX) + std::abs(a.y-a.goalY);
        int dnext = std::abs(nx-a.goalX) + std::abs(ny-a.goalY);
        score += (float)(dcur - dnext) * 1.4f;
      }
      // avoid overcrowding
      int sameN = countSameNear(nx, ny, 1);
      if (sameN >= 3) score -= (float)(sameN - 2);
      // social/curious personality
      if (!sd.schooling && a.social > 0.6f) {
        score += (a.social-0.6f) * 2.0f;
      }
      uint32_t k = hash3((uint32_t)nx,(uint32_t)ny,(uint32_t)w.seed);
      if (a.curious > 0.6f && (k & 7u)==0u) score += 1.f;
      score += (float)r.i(0,2);
      if (score>bestScore){ bestScore=score; bestDx=dx; bestDy=dy; }
    }
    if (a.goalTTL <= 0 || !inBounds(a.goalX, a.goalY) || (a.x==a.goalX && a.y==a.goalY)) {
      a.goalX = clampi(a.x + bestDx, 0, W-1);
      a.goalY = clampi(a.y + bestDy, 0, H-1);
      a.goalTTL = 6 + r.i(0,6);
    }

    int stepMul = sprint ? 2 : 1;
    int oldX = a.x, oldY = a.y;
    auto canEnter = [&](int x,int y){
      if (!inBounds(x,y)) return false;
      if (!sd.aquatic && w.water[y][x] > 0.4f) return false;
      if (sd.aquatic && w.water[y][x] <= 0.2f) return false;
      return true;
    };
    int nx = a.x, ny = a.y;
    for (int s=0; s<stepMul; ++s){
      if (bestDx==0 && bestDy==0) break;
      int tx = nx + bestDx;
      int ty = ny + bestDy;
      if (!canEnter(tx, ty)) break;
      nx = tx; ny = ty;
    }
    a.x = nx; a.y = ny;
    if (a.x==oldX && a.y==oldY) a.fatigue = clamp01(a.fatigue - 0.02f);
    else a.fatigue = clamp01(a.fatigue + 0.01f);

    // health
    float harm = 0.f;
    if (a.hunger>0.9f) harm += (a.hunger-0.9f);
    if (a.thirst>0.9f) harm += (a.thirst-0.9f)*1.2f;
    if (a.age > a.maxAge) harm += 0.4f;
    a.health = clamp01(a.health - harm*0.05f);
    a.panic = (a.stress > 0.75f);
    if (a.health < 0.1f) {
      Event ev; ev.type=Event::EV_DEATH; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
      w.fertility[a.y][a.x] = std::min(1.0f, w.fertility[a.y][a.x] + 0.08f);
      w.detritus[a.y][a.x] = std::min(1.4f, w.detritus[a.y][a.x] + 0.12f);
    }

    // reproduction
    float seasonBoost = (seasonAt(tick) == SPRING) ? 1.2f : 1.0f;
    if (a.health>0.8f && a.hunger<0.4f && r.u01() < sd.reproduce*0.15f*seasonBoost) {
      if ((int)w.agents.size() < MAX_AGENTS) {
        Agent child=a; child.id=(int)w.agents.size() + (int)newborns.size() + 1; child.hunger=0.2f; child.thirst=0.2f; child.health=0.7f;
        child.fatigue=0.2f; child.stress=0.2f; child.emotion=0.6f;
        float mut = 0.08f;
        child.bold = clamp01(a.bold + (r.u01()-0.5f)*mut);
        child.social = clamp01(a.social + (r.u01()-0.5f)*mut);
        child.curious = clamp01(a.curious + (r.u01()-0.5f)*mut);
        child.aggro = clamp01(a.aggro + (r.u01()-0.5f)*mut);
        child.age = 0;
        child.maxAge = a.maxAge + r.i(-200,200);
        child.goalX = child.x; child.goalY = child.y; child.goalTTL = 0;
        newborns.push_back(child);
        Event ev; ev.type=Event::EV_BIRTH; ev.x=child.x; ev.y=child.y; ev.mag=1.f; w.events.push_back(ev);
      }
    }
  }

  // apply newborns after iteration to avoid invalidating references
  if (!newborns.empty()) {
    size_t space = (w.agents.size() < (size_t)MAX_AGENTS) ? (size_t)MAX_AGENTS - w.agents.size() : 0;
    size_t addN = std::min(space, newborns.size());
    w.agents.insert(w.agents.end(), newborns.begin(), newborns.begin() + (int)addN);
  }

  // post-move predation using updated positions
  if (!w.agents.empty()) {
    std::vector<AgentView> liveViews;
    liveViews.reserve(w.agents.size());
    for (const auto& a : w.agents){
      const SpeciesDef &sd = g_species[a.sp];
      AgentView v;
      v.x = a.x; v.y = a.y; v.sp = a.sp;
      v.carnivore = sd.carnivore;
      v.herbivore = sd.herbivore;
      v.aquatic = sd.aquatic;
      v.schooling = sd.schooling;
      v.pack = sd.pack;
      liveViews.push_back(v);
    }
    auto liveBuckets = buildBuckets(liveViews);
    auto forEachLiveNearby = [&](int x,int y,int radius, const auto& fn){
      forEachNearby(liveViews, liveBuckets, x, y, radius, fn);
    };

    for (size_t i=0; i<w.agents.size(); ++i){
      auto &a = w.agents[i];
      const SpeciesDef &sd = g_species[a.sp];
      if (a.sp == SP_BIRD && a.hunger > 0.4f) {
        bool ate=false;
        forEachLiveNearby(a.x, a.y, 1, [&](int idx){
          if (ate) return;
          if (liveViews[idx].sp != SP_FISH) return;
          auto &p = w.agents[idx];
          if (std::abs(p.x-a.x)+std::abs(p.y-a.y) != 1) return;
          if (w.water[p.y][p.x] > 0.2f && r.oneIn(4)) {
            p.health -= 0.6f;
            a.hunger = std::max(0.f, a.hunger - 0.5f);
            Event ev; ev.type=Event::EV_EAT; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
            ate = true;
          }
        });
      }
      if (sd.carnivore) {
        forEachLiveNearby(a.x, a.y, 0, [&](int idx){
          if (idx==(int)i) return;
          if (!g_species[w.agents[idx].sp].herbivore) return;
          if (r.oneIn(3)) {
            w.agents[idx].health -= 0.5f;
            a.hunger = std::max(0.f, a.hunger - 0.5f);
            a.stress = clamp01(a.stress - 0.12f);
            Event ev; ev.type=Event::EV_EAT; ev.x=a.x; ev.y=a.y; ev.mag=2.f; w.events.push_back(ev);
          }
        });
      }
    }
  }

  // cull dead
  w.agents.erase(std::remove_if(w.agents.begin(), w.agents.end(),
    [](const Agent& a){ return a.health <= 0.05f; }), w.agents.end());
}

static void updateModPool(World& w, int tick){
  int waterC=0, plantC=0, overC=0;
  int agentsV=0, panicC=0;
  float stressSum=0, hungerSum=0, thirstSum=0, fatSum=0, healthSum=0;
  float emoSum=0, boldSum=0, socialSum=0, curiousSum=0, aggroSum=0;
  float speedSum=0;
  float speedBurstSum=0.f;
  int stressHi=0;
  int uniqueSpecies=0;
  bool speciesSeen[SP_COUNT] = {false};
  int schoolingCount=0;
  float schoolDistSum=0.f;
  int restCount=0;
  int aquaticCount=0, landCount=0;
  float distWaterSum=0.f;
  int fireCells=0;
  float meanAlt=0.f;
  float meanAlt2=0.f;
  float plantVarSum=0.f;
  float faunaVarSum=0.f;
  float edgeAct=0.f;
  int huntEvents=0, forageEvents=0;
  float fertSum=0.f;
  float detSum=0.f;
  float sedSum=0.f;
  float fertFlux=0.f;
  float mistStrength=0.f;
  float heatShimmer=0.f;
  float snowDensity=0.f;
  float migBias=0.f;
  float grazingImpact=0.f;
  float soilEnrich=0.f;

  const int threads = std::max(1, std::min(g_threads, MAX_THREADS));

  struct GridAcc {
    int waterC=0, plantC=0, overC=0, fireCells=0;
    float meanAlt=0.f, meanAlt2=0.f, fertSum=0.f, detSum=0.f, sedSum=0.f;
  };
  int gridThreads = std::min(threads, H);
  std::vector<GridAcc> gacc(gridThreads);
  parallelForRange(H, gridThreads, [&](int y0, int y1, int tid){
    GridAcc acc;
    for(int y=y0;y<y1;++y){
      for(int x=0;x<W;++x){
        if (w.water[y][x] > 0.2f) ++acc.waterC;
        if (isVeg(w.terrain[y][x])) ++acc.plantC;
        if (w.terrain[y][x] == '*') ++acc.fireCells;
        if (w.overlay[y][x] != ' ') ++acc.overC;
        float alt = (float)w.height[y][x] / 255.f;
        acc.meanAlt += alt;
        acc.meanAlt2 += alt*alt;
        acc.fertSum += w.fertility[y][x];
        acc.detSum += std::clamp(w.detritus[y][x], 0.f, 1.4f);
        acc.sedSum += std::clamp(w.sediment[y][x], 0.f, 1.4f);
      }
    }
    gacc[tid] = acc;
  });
  for (const auto& acc : gacc) {
    waterC += acc.waterC;
    plantC += acc.plantC;
    overC += acc.overC;
    fireCells += acc.fireCells;
    meanAlt += acc.meanAlt;
    meanAlt2 += acc.meanAlt2;
    fertSum += acc.fertSum;
    detSum += acc.detSum;
    sedSum += acc.sedSum;
  }
  float waterFrac = (float)waterC / (float)(W*H);
  float plantFrac = (float)plantC / (float)(W*H);
  float overFrac  = (float)overC  / (float)(W*H);

  if (w.prevPos.size() != w.agents.size()) w.prevPos.assign(w.agents.size(), {-9999,-9999});

  int predators=0, prey=0;
  int crabN=0, eelN=0, fishN=0, birdN=0, alienN=0;

  struct AgentAcc {
    int agentsV=0, panicC=0, stressHi=0;
    int predators=0, prey=0;
    int crabN=0, eelN=0, fishN=0, birdN=0, alienN=0;
    int restCount=0;
    int aquaticCount=0, landCount=0;
    float stressSum=0, hungerSum=0, thirstSum=0, fatSum=0, healthSum=0;
    float emoSum=0, boldSum=0, socialSum=0, curiousSum=0, aggroSum=0;
    float speedSum=0, speedBurstSum=0;
    float distWaterSum=0;
    float schoolDistSum=0;
    int schoolingCount=0;
    float edgeAct=0;
    bool speciesSeen[SP_COUNT] = {false};
  };

  const int agentN = (int)w.agents.size();
  if (agentN > 0) {
    const int BUCKET = 4;
    const int BW = (W + BUCKET - 1) / BUCKET;
    const int BH = (H + BUCKET - 1) / BUCKET;
    auto bucketIndex = [&](int x,int y){ return (y/BUCKET)*BW + (x/BUCKET); };
    std::vector<std::vector<int>> buckets(BW*BH);
    for (int i=0; i<agentN; ++i){
      buckets[bucketIndex(w.agents[i].x, w.agents[i].y)].push_back(i);
    }
    auto forEachNearbyAgent = [&](int x,int y,int radius, const auto& fn){
      int minx = std::max(0, x - radius);
      int maxx = std::min(W-1, x + radius);
      int miny = std::max(0, y - radius);
      int maxy = std::min(H-1, y + radius);
      int minbx = minx / BUCKET;
      int maxbx = maxx / BUCKET;
      int minby = miny / BUCKET;
      int maxby = maxy / BUCKET;
      for (int by=minby; by<=maxby; ++by){
        for (int bx=minbx; bx<=maxbx; ++bx){
          const auto& b = buckets[by*BW + bx];
          for (int idx : b){
            const auto& o = w.agents[idx];
            int d = std::abs(o.x - x) + std::abs(o.y - y);
            if (d <= radius) fn(idx);
          }
        }
      }
    };

    int agentThreads = std::min(threads, agentN);
    std::vector<AgentAcc> aacc(agentThreads);
    parallelForRange(agentN, agentThreads, [&](int i0, int i1, int tid){
      AgentAcc acc;
      for (int i=i0;i<i1;++i){
        const auto& a = w.agents[i];
        acc.agentsV++;
        acc.stressSum += a.stress;
        acc.hungerSum += a.hunger;
        acc.thirstSum += a.thirst;
        acc.fatSum    += a.fatigue;
        acc.healthSum += a.health;
        acc.emoSum    += a.emotion;
        acc.boldSum   += a.bold;
        acc.socialSum += a.social;
        acc.curiousSum+= a.curious;
        acc.aggroSum  += a.aggro;
        if (a.stress > 0.75f) acc.stressHi++;
        if (a.panic) acc.panicC++;

        if (w.prevPos[i].first!=-9999) {
          int dx=a.x - w.prevPos[i].first;
          int dy=a.y - w.prevPos[i].second;
          float sp = std::sqrt(float(dx*dx + dy*dy));
          acc.speedSum += sp;
          if (sp > 1.2f) acc.speedBurstSum += 1.f;
          if (dx==0 && dy==0) acc.restCount++;
        }
        w.prevPos[i] = {a.x,a.y};

        if (g_species[a.sp].carnivore) acc.predators++; else acc.prey++;
        if (a.sp==SP_CRAB) acc.crabN++;
        if (a.sp==SP_EEL) acc.eelN++;
        if (a.sp==SP_FISH) acc.fishN++;
        if (a.sp==SP_BIRD) acc.birdN++;
        if (a.sp==SP_ALIEN1 || a.sp==SP_ALIEN2) acc.alienN++;
        acc.speciesSeen[a.sp] = true;
        if (g_species[a.sp].aquatic) acc.aquaticCount++; else acc.landCount++;

        if (a.x<2 || a.y<2 || a.x>W-3 || a.y>H-3) acc.edgeAct += 1.f;

        // approximate distance to water (0..4)
        int bestD = 4;
        for(int dy=-4; dy<=4; ++dy){
          for(int dx=-4; dx<=4; ++dx){
            int nx=a.x+dx, ny=a.y+dy; if(!inBounds(nx,ny)) continue;
            if (w.water[ny][nx] > 0.2f) {
              int d = std::abs(dx) + std::abs(dy);
              if (d < bestD) bestD = d;
            }
          }
        }
        acc.distWaterSum += (float)bestD / 4.f;

        // schooling cohesion
        if (g_species[a.sp].schooling) {
          int nearest=99;
          forEachNearbyAgent(a.x, a.y, 8, [&](int idx){
            if (idx==i) return;
            const auto& o = w.agents[idx];
            if (o.sp!=a.sp) return;
            int d = std::abs(o.x-a.x)+std::abs(o.y-a.y);
            if (d < nearest) nearest = d;
          });
          if (nearest < 99) { acc.schoolDistSum += (float)nearest; acc.schoolingCount++; }
        }
      }
      aacc[tid] = acc;
    });

    for (const auto& acc : aacc) {
      agentsV += acc.agentsV;
      panicC += acc.panicC;
      stressHi += acc.stressHi;
      stressSum += acc.stressSum;
      hungerSum += acc.hungerSum;
      thirstSum += acc.thirstSum;
      fatSum += acc.fatSum;
      healthSum += acc.healthSum;
      emoSum += acc.emoSum;
      boldSum += acc.boldSum;
      socialSum += acc.socialSum;
      curiousSum += acc.curiousSum;
      aggroSum += acc.aggroSum;
      speedSum += acc.speedSum;
      speedBurstSum += acc.speedBurstSum;
      restCount += acc.restCount;
      aquaticCount += acc.aquaticCount;
      landCount += acc.landCount;
      distWaterSum += acc.distWaterSum;
      schoolDistSum += acc.schoolDistSum;
      schoolingCount += acc.schoolingCount;
      edgeAct += acc.edgeAct;
      predators += acc.predators;
      prey += acc.prey;
      crabN += acc.crabN;
      eelN += acc.eelN;
      fishN += acc.fishN;
      birdN += acc.birdN;
      alienN += acc.alienN;
      for (int s=0; s<SP_COUNT; ++s) if (acc.speciesSeen[s]) speciesSeen[s]=true;
    }
  }
  for (int s=0; s<SP_COUNT; ++s) if (speciesSeen[s]) uniqueSpecies++;

  float invA = (agentsV>0)? (1.0f/agentsV) : 0.f;
  float stressMean = stressSum*invA;
  float hungerMean = hungerSum*invA;
  float thirstMean = thirstSum*invA;
  float fatMean    = fatSum*invA;
  float healthMean = healthSum*invA;
  float emoMean = emoSum*invA;
  float boldMean = boldSum*invA;
  float socialMean = socialSum*invA;
  float curiousMean = curiousSum*invA;
  float aggroMean = aggroSum*invA;
  float agentSpeed = speedSum*invA;
  float burstRatio = (agentsV>0)? (speedBurstSum/(float)agentsV) : 0.f;
  float predPressure = (prey>0)? (float)predators/(float)prey : (predators? 4.f:0.f);

  float p = plantFrac;
  plantVarSum = p*(1.f-p);
  float f = (agentsV>0)? (float)agentsV / (float)(W*H) : 0.f;
  faunaVarSum = f*(1.f-f);

  edgeAct *= invA;

  for (const auto& ev : w.events){
    if (ev.type==Event::EV_EAT && ev.mag>1.2f) huntEvents++;
    if (ev.type==Event::EV_EAT && ev.mag<=1.2f) forageEvents++;
    if (ev.type==Event::EV_EAT) grazingImpact += 1.f;
    if (ev.type==Event::EV_DEATH) soilEnrich += 1.f;
  }

  float rippleE=0.f;
  for (auto &rp: g_ripples){
    rippleE += rp.amp * std::exp(-rp.t/3.0f);
  }
  rippleE = std::min(3.0f, rippleE);

  static float prevWater=0, prevPlant=0, prevStress=0, prevHunger=0, prevThirst=0, prevFat=0, prevHealth=0, prevPanic=0;
  float waterFlux = std::fabs(waterFrac - prevWater);
  float plantFlux = std::fabs(plantFrac - prevPlant);
  float stressFlux= std::fabs(stressMean - prevStress);
  float hungerFlux= std::fabs(hungerMean - prevHunger);
  float thirstFlux= std::fabs(thirstMean - prevThirst);
  float fatFlux   = std::fabs(fatMean - prevFat);
  float healthFlux= std::fabs(healthMean - prevHealth);
  float panicFlux = std::fabs((float)panicC*invA - prevPanic);
  prevWater=waterFrac; prevPlant=plantFrac; prevStress=stressMean; prevHunger=hungerMean; prevThirst=thirstMean;
  prevFat=fatMean; prevHealth=healthMean; prevPanic=(float)panicC*invA;

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
  g_modVal[13]=clamp01f((float)std::count_if(w.events.begin(), w.events.end(), [](const Event& e){ return e.type==Event::EV_BIRTH; }));
  g_modVal[14]=clamp01f((float)std::count_if(w.events.begin(), w.events.end(), [](const Event& e){ return e.type==Event::EV_DEATH; }));
  g_modVal[15]=clamp01f(rippleE/3.0f);
  g_modVal[16]=clamp01f((float)w.wind.strength/5.f);
  g_modVal[17]=clamp01f(seasonLerp(tick));
  g_modVal[18]=clamp01f(w.weather.cloudOpacity);
  g_modVal[19]=clamp01f(w.weather.rainStrength);
  g_modVal[20]=clamp01f((crabN>0)? (float)crabN/40.f : 0.f);
  g_modVal[21]=clamp01f((float)fishN/50.f * (1.0f - agentSpeed));
  g_modVal[22]=clamp01f((float)eelN/40.f * (0.3f + stressMean));
  g_modVal[23]=clamp01f((float)birdN/40.f * (0.2f + waterFrac));
  g_modVal[24]=clamp01f((float)alienN/40.f * (0.5f + overFrac));
  g_modVal[25]=clamp01f((float)alienN/40.f * (0.5f + rippleE/3.0f));
  g_modVal[26]=clamp01f((float)predators/40.f * (0.4f + predPressure*0.25f));
  g_modVal[27]=clamp01f(plantFlux*4.0f);
  g_modVal[28]=clamp01f(waterFlux*4.0f);
  g_modVal[29]=clamp01f(stressFlux*4.0f);
  g_modVal[30]=clamp01f(hungerFlux*4.0f);
  g_modVal[31]=clamp01f(thirstFlux*4.0f);
  g_modVal[32]=clamp01f(fatFlux*4.0f);
  g_modVal[33]=clamp01f(healthFlux*4.0f);
  g_modVal[34]=clamp01f(panicFlux*4.0f);

  g_modVal[35]=clamp01f(emoMean);
  g_modVal[36]=clamp01f(boldMean);
  g_modVal[37]=clamp01f(socialMean);
  g_modVal[38]=clamp01f(curiousMean);
  g_modVal[39]=clamp01f(aggroMean);

  float o0 = (g_modVal[20]*g_modVal[15]);
  float o1 = (g_modVal[12]*(1.0f-g_modVal[1]));
  float o2 = (g_modVal[8]*g_modVal[9]);
  float o3 = std::fabs(g_modVal[16]-g_modVal[19]);
  float o4 = g_modVal[21] * (0.3f + g_modVal[2]);
  float o5 = g_modVal[5] * (0.5f + g_modVal[28]);
  float o6 = g_modVal[11] * (1.0f - g_modVal[10]);
  float o7 = g_modVal[24] * (0.2f + g_modVal[17]);
  float detMeanForOdd = detSum / (float)(W*H);
  float sedMeanForOdd = sedSum / (float)(W*H);
  float o8 = g_modVal[25] * (0.2f + g_modVal[4]) + std::clamp(detMeanForOdd*0.25f, 0.f, 1.f);
  float surfPulse = std::clamp(0.5f*g_modVal[16] + 0.5f*g_modVal[19] + 0.2f*sedMeanForOdd, 0.f, 1.f);
  float o9 = 0.40f*(float)((tick/37)%11)/10.0f + 0.60f*surfPulse;
  float odd[10]={o0,o1,o2,o3,o4,o5,o6,o7,o8,o9};
  for(int i=0;i<10;++i) g_modVal[40+i]=clamp01f(odd[i]);

  g_modVal[50] = clamp01f((float)uniqueSpecies / (float)SP_COUNT);
  g_modVal[51] = clamp01f(std::min(2.0f,predPressure)/2.0f);
  g_modVal[52] = clamp01f(distWaterSum*invA);
  g_modVal[53] = clamp01f(waterFlux*6.0f);
  {
    int types=0;
    bool hasComma=false, hasTall=false, hasShrub=false, hasReed=false, hasLily=false, hasTree=false, hasFlower=false;
    for (const auto& row : w.terrain){
      if (!hasComma && row.find(',')!=std::string::npos) hasComma=true;
      if (!hasTall && row.find('"')!=std::string::npos) hasTall=true;
      if (!hasShrub && row.find(';')!=std::string::npos) hasShrub=true;
      if (!hasReed && row.find('#')!=std::string::npos) hasReed=true;
      if (!hasLily && row.find('m')!=std::string::npos) hasLily=true;
      if (!hasTree && (row.find('T')!=std::string::npos || row.find('Y')!=std::string::npos)) hasTree=true;
      if (!hasFlower && (row.find('f')!=std::string::npos || row.find('+')!=std::string::npos || row.find('&')!=std::string::npos || row.find('!')!=std::string::npos)) hasFlower=true;
    }
    types += hasComma; types += hasTall; types += hasShrub; types += hasReed; types += hasLily; types += hasTree; types += hasFlower;
    g_modVal[54] = clamp01f((float)types / 7.f);
  }
  g_modVal[55] = clamp01f(plantVarSum*4.0f);
  g_modVal[56] = clamp01f(faunaVarSum*50.0f);
  g_modVal[57] = clamp01f((schoolingCount>0)? (schoolDistSum/(float)schoolingCount)/6.0f : 0.f);
  g_modVal[58] = clamp01f((agentsV>0)? (float)restCount/(float)agentsV : 0.f);
  g_modVal[59] = clamp01f((predators>0)? (float)huntEvents/(float)predators : 0.f);
  g_modVal[60] = clamp01f((prey>0)? (float)forageEvents/(float)prey : 0.f);
  g_modVal[61] = clamp01f(overFrac);
  g_modVal[62] = clamp01f(std::fabs(g_modVal[16]-g_modVal[19]));
  g_modVal[63] = clamp01f((float)(w.weather.timer % 200) / 200.f);
  g_modVal[64] = clamp01f((float)fireCells / (float)(W*H) * 30.0f);
  g_modVal[65] = clamp01f((agentsV>0)? (float)aquaticCount/(float)agentsV : 0.f);
  g_modVal[66] = clamp01f((agentsV>0)? (float)landCount/(float)agentsV : 0.f);
  float altMean = meanAlt / (float)(W*H);
  g_modVal[67] = clamp01f(altMean);
  g_modVal[68] = clamp01f(edgeAct);
  g_modVal[69] = clamp01f(1.0f - stressMean);

  // new ecological/music signals
  static float prevFert=0.f;
  float fertMean = fertSum / (float)(W*H);
  fertFlux = std::fabs(fertMean - prevFert);
  prevFert = fertMean;
  if (w.biome==WETLAND) mistStrength = std::clamp(w.weather.humidity,0.f,1.f);
  if (w.biome==DESERT) heatShimmer = (w.weather.state==CLEAR) ? 1.0f : 0.2f;
  if ((w.biome==ALPINE || w.biome==TAIGA) && seasonAt(tick)==WINTER) snowDensity = 0.8f;
  {
    float seasonMig = (seasonAt(tick)==WINTER || seasonAt(tick)==SUMMER) ? 0.6f : 0.3f;
    float goalDist = 0.f;
    float longGoal = 0.f;
    for (const auto& a : w.agents){
      int d = std::abs(a.x-a.goalX) + std::abs(a.y-a.goalY);
      goalDist += (float)d;
      if (a.goalTTL > 10 || d > 10) longGoal += 1.f;
    }
    float liveMig = 0.f;
    if (!w.agents.empty()) {
      float meanD = goalDist / (float)w.agents.size();
      liveMig = std::clamp(meanD / 18.f, 0.f, 1.f) * 0.7f
              + std::clamp(longGoal / (float)w.agents.size(), 0.f, 1.f) * 0.3f;
    }
    migBias = std::clamp(0.35f*seasonMig + 0.65f*liveMig, 0.f, 1.f);
  }

  g_modVal[70] = clamp01f(fertMean);
  g_modVal[71] = clamp01f(fertFlux*6.0f);
  g_modVal[72] = clamp01f(mistStrength);
  g_modVal[73] = clamp01f(heatShimmer);
  g_modVal[74] = clamp01f(snowDensity);
  g_modVal[75] = clamp01f(migBias);
  g_modVal[76] = clamp01f((agentsV>0)? (grazingImpact/(float)agentsV) : 0.f);
  g_modVal[77] = clamp01f((agentsV>0)? (soilEnrich/(float)agentsV) : 0.f);

  // inject burst ratio into oddities for musical flourishes
  g_modVal[40] = clamp01f(std::max(g_modVal[40], burstRatio));

  static float prev[MOD_N] = {0};
  for(int i=0;i<MOD_N;++i){
    float v = g_modVal[i]*2.0f - 1.0f;
    float dv = v - prev[i];
    prev[i] = v;
    float sp = v + 0.85f*dv + ((float)((tick + i*131) % 97) / 96.0f - 0.5f) * 0.06f;
    g_modVal[i] = clamp11(sp);
    g_modActivity[i] = g_modActivity[i]*0.9f + std::fabs(dv)*0.1f;
  }

  // track most active mods (for trigger selection) — proper top-6 via partial sort
  int idx[MOD_N];
  for (int i=0;i<MOD_N;++i) idx[i]=i;
  std::partial_sort(idx, idx+6, idx+MOD_N,
    [](int a, int b){ return g_modActivity[a] > g_modActivity[b]; });
  for (int k=0;k<6;++k) g_modHot[k]=idx[k];
}

// ===== MIDI event mapping =====
enum ScaleType { SCALE_CHROMATIC=0, SCALE_MAJOR=1, SCALE_MINOR=2, SCALE_PENTATONIC=3, SCALE_DORIAN=4, SCALE_LYDIAN=5, SCALE_WHOLE=6 };

static inline const char* scaleName(ScaleType st){
  switch(st){
    case SCALE_CHROMATIC: return "CHROMATIC";
    case SCALE_MAJOR: return "MAJOR";
    case SCALE_MINOR: return "MINOR";
    case SCALE_PENTATONIC: return "PENTATONIC";
    case SCALE_DORIAN: return "DORIAN";
    case SCALE_LYDIAN: return "LYDIAN";
    case SCALE_WHOLE: return "WHOLE";
    default: return "MAJOR";
  }
}

static inline const char* rootName(int root){
  static const char* names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
  return names[clampi(root, 0, 11)];
}

static inline int quantizeNoteToScale(int midiNote, int root, ScaleType st) {
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

struct ActiveNote { int note=-1; int offTick=0; bool on=false; };
static ActiveNote g_activeNotes[16];
struct PendingOff { int ch=0; int note=0; int offTick=0; };
static std::vector<PendingOff> g_pendingOffs;

static inline void serviceNoteOffs(SynthOut& synth, MidiOut& midi, int tick){
  for (int ch=0; ch<16; ++ch){
    if (g_activeNotes[ch].on && tick >= g_activeNotes[ch].offTick){
      int note = g_activeNotes[ch].note;
      synth.noteOff(ch, note, 0);
      midi.sendNoteOff(ch, note, 0);
      g_activeNotes[ch].on=false;
    }
  }
  if (!g_pendingOffs.empty()) {
    size_t wr = 0;
    for (size_t i=0; i<g_pendingOffs.size(); ++i){
      const PendingOff& p = g_pendingOffs[i];
      if (tick >= p.offTick) {
        synth.noteOff(p.ch, p.note, 0);
        midi.sendNoteOff(p.ch, p.note, 0);
      } else {
        g_pendingOffs[wr++] = p;
      }
    }
    g_pendingOffs.resize(wr);
  }
}

static inline void sendDrumProgram(SynthOut& synth, MidiOut& midi){
  int bank = clampi(g_drumBank, 0, 127);
  int prog = clampi(g_drumProg, 0, 127);
  synth.bankSelect(9, bank);
  synth.programChange(9, prog);
  midi.sendCC(9, 0, (uint8_t)bank);
  midi.sendCC(9, 32, 0);
  midi.sendProgramChange(9, (uint8_t)prog);
}

static inline void applyVoiceProgramNow(SynthOut& synth, MidiOut& midi, int ch, int tick){
  if (ch < 0 || ch >= 4) return;
  synth.cc(ch, 123, 0); midi.sendCC(ch,123,0);
  synth.cc(ch, 121, 0); midi.sendCC(ch,121,0);
  synth.cc(ch, 0, 0); midi.sendCC(ch,0,0);
  synth.cc(ch, 32, 0); midi.sendCC(ch,32,0);
  int prog = clampi(g_voiceProg[ch], 0, 127);
  g_voiceProg[ch] = prog;
  synth.programChange(ch, prog);
  midi.sendProgramChange(ch, (uint8_t)prog);
  g_voiceProgDirty[ch] = false;
  g_progLastTick[ch] = tick;
}

static inline void resetAudio(SynthOut& synth, MidiOut& midi){
  if (!g_wantSynth || g_sf2Path.empty()) return;
  // Silence everything and drain pending state before teardown to avoid
  // hung notes or corrupt voice state carrying over into the new driver.
  synth.allNotesOff();
  for(int ch=0; ch<16; ++ch){ midi.sendCC(ch, 123, 0); }
  for(int ch=0; ch<16; ++ch) g_activeNotes[ch] = ActiveNote{};
  g_pendingOffs.clear();
  SDL_Delay(40); // brief drain — one FluidSynth period at 44.1kHz/2048
  synth.close();
  if (synth.open(g_sf2Path, g_masterGain, g_audioDriver, g_audioDevice)) {
    synth.listPresets(g_sf2Presets);
    for (int i=0;i<4;++i) g_voiceProgDirty[i] = true;
    sendDrumProgram(synth, midi);
    enforceDefaultDrumMute();
  }
}

static void saveSnapshot(const World& w, int tps){
  std::filesystem::create_directories("/home/user/terrarium/snapshots");
  auto ts = std::chrono::system_clock::now().time_since_epoch();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ts).count();
  std::string path = "/home/user/terrarium/snapshots/terrarium_snapshot_" + std::to_string(ms) + ".txt";
  std::ofstream out(path);
  if (!out) return;
  out << "seed " << w.seed << "\n";
  out << "biome " << g_biomes[w.biome].name << "\n";
  out << "tps " << tps << "\n";
  out << "alea " << g_alea.rainChance << " " << g_alea.spawnChance << " " << g_alea.drift << " " << g_alea.chaos << "\n";
  out << "voices ";
  for (int i=0;i<4;++i) out << g_voiceProg[i] << " ";
  out << "\nvol ";
  for (int i=0;i<4;++i) out << g_voiceVol[i] << " ";
  out << "\ndrums " << g_drumProg << " " << g_drumBank << " " << g_drumVol << " " << g_drum2Vol << " " << g_drum3Vol << "\n";
  out << "master " << g_masterGain << " tempo " << g_tempoMult << "\n";
  out << "modmap\n";
  for (int i=0;i<MOD_SLOTS;++i){
    out << i << " " << g_modMap[i].enabled << " " << g_modMap[i].src << " " << g_modMap[i].dest << " " << g_modMap[i].amt << " " << g_modMap[i].smooth << "\n";
  }
}

static void exportMidiStub(){
  std::filesystem::create_directories("/home/user/terrarium/snapshots");
  std::string path = "/home/user/terrarium/snapshots/midi_export_stub.txt";
  std::ofstream out(path);
  if (!out) return;
  out << "MIDI export is not implemented yet. This is a stub.\n";
}

// Maximum note duration in ticks (~10s at 20 TPS) — prevents stuck notes if
// dur is miscalculated or a note-off is ever skipped.
static constexpr int MAX_NOTE_TICKS = 200;

static inline void gatedNoteOn(SynthOut& synth, MidiOut& midi, int ch, int note, int vel, int tick, int dur){
  if (g_activeNotes[ch].on) {
    synth.noteOff(ch, g_activeNotes[ch].note, 0);
    midi.sendNoteOff(ch, g_activeNotes[ch].note, 0);
  }
  synth.noteOn(ch, note, vel);
  midi.sendNoteOn(ch, note, vel);
  g_activeNotes[ch].note = note;
  g_activeNotes[ch].offTick = tick + std::clamp(dur, 1, MAX_NOTE_TICKS);
  g_activeNotes[ch].on = true;
}

static inline void queuedNoteOn(SynthOut& synth, MidiOut& midi, int ch, int note, int vel, int tick, int dur){
  synth.noteOn(ch, note, vel);
  midi.sendNoteOn(ch, note, vel);
  PendingOff off;
  off.ch = ch;
  off.note = note;
  off.offTick = tick + std::clamp(dur, 1, MAX_NOTE_TICKS);
  g_pendingOffs.push_back(off);
}

static inline void drumHit(SynthOut& synth, MidiOut& midi, int note, int baseVel, int vol){
  if (vol <= 0) return;
  int vel = clampi((baseVel * clampi(vol,0,127)) / 127, 1, 127);
  synth.noteOn(9, note, vel);
  midi.sendNoteOn(9, note, vel);
}

static inline void buildChord(std::vector<int>& out, int rootNote, ScaleType st, int chordType){
  out.clear();
  // chordType: 0=maj,1=min,2=7,3=maj7,4=min7,5=sus2,6=sus4,7=add9
  static const int maj[3]  = {0,4,7};
  static const int min[3]  = {0,3,7};
  static const int dom7[4] = {0,4,7,10};
  static const int maj7[4] = {0,4,7,11};
  static const int min7[4] = {0,3,7,10};
  static const int sus2[3] = {0,2,7};
  static const int sus4[3] = {0,5,7};
  static const int add9[4] = {0,4,7,14};
  const int* arr = maj; int n=3;
  switch(chordType){
    case 1: arr=min; n=3; break;
    case 2: arr=dom7; n=4; break;
    case 3: arr=maj7; n=4; break;
    case 4: arr=min7; n=4; break;
    case 5: arr=sus2; n=3; break;
    case 6: arr=sus4; n=3; break;
    case 7: arr=add9; n=4; break;
  }
  for (int i=0;i<n;++i){
    int note = rootNote + arr[i];
    out.push_back(note);
  }
}

struct MusicState {
  int nextChordTick=0;
  int chordIndex=0;
  int chordType=0;
  int arpStep=0;
  int phraseBurst=0;
  int phrasePattern=0;
  int phraseStep=0;
  int motifLen=0;
  int motifPos=0;
  int motif[8] = {0};
  int motifHold=0;
};
static MusicState g_music;

static void synthTickMusic(const World& w, SynthOut& synth, MidiOut& midi, int tick){
  serviceNoteOffs(synth, midi, tick);
  enforceDefaultDrumMute();

  // auto-trigger envelopes from most active mods
  if (g_trigTimer > 0) g_trigTimer--;
  else {
    for (int k=0;k<6;++k){
      int mi = g_modHot[k];
      float v = std::fabs(g_modVal[mi]);
      if (v > 0.6f && ((tick + mi*7) % 11)==0) {
        g_trigType = (tick + mi*13) % 3;
        g_trigDur = 6 + ((tick + mi*5) % 18);
        g_trigTimer = g_trigDur;
        break;
      }
    }
  }
  float trigEnv = (g_trigTimer>0 && g_trigDur>0) ? (float)g_trigTimer/(float)g_trigDur : 0.f;

  // derive root + scale from mod matrix for livelier movement
  int root = (int)std::lround((g_modVal[17]+1.f) * 6.f); // 0..12
  root = std::clamp(root, 0, 11);
  static const ScaleType biomeScale[BIOME_COUNT] = {
    SCALE_MAJOR,     // MEADOW
    SCALE_DORIAN,    // WETLAND
    SCALE_LYDIAN,    // ALPINE
    SCALE_MINOR,     // DESERT
    SCALE_MAJOR,     // TROPICAL
    SCALE_MINOR,     // TAIGA
    SCALE_WHOLE      // ALIEN
  };
  ScaleType scale = biomeScale[w.biome];
  if (g_modVal[19] > 0.3f) scale = SCALE_DORIAN;
  if (g_modVal[0] > 0.5f) scale = SCALE_LYDIAN;
  if (g_modVal[1] < -0.2f) scale = SCALE_MINOR;
  if (g_musicKeyManual) {
    root = clampi(g_musicRootManual, 0, 11);
    scale = (ScaleType)clampi(g_musicScaleManual, (int)SCALE_CHROMATIC, (int)SCALE_WHOLE);
  }
  float emo = std::clamp(g_modVal[35], -1.f, 1.f);
  float bold = std::clamp(g_modVal[36], -1.f, 1.f);
  float social = std::clamp(g_modVal[37], -1.f, 1.f);
  float curious = std::clamp(g_modVal[38], -1.f, 1.f);
  float aggro = std::clamp(g_modVal[39], -1.f, 1.f);

  if (w.biome == ALIEN) {
    aggro = std::clamp(aggro + 0.35f, -1.f, 1.f);
    curious = std::clamp(curious + 0.25f, -1.f, 1.f);
    emo = std::clamp(emo - 0.25f, -1.f, 1.f);
  }

  // chord progression selection
  static const int progA[] = {0,3,4,3}; // I-IV-V-IV
  static const int progB[] = {5,3,0,4}; // vi-IV-I-V
  static const int progC[] = {0,5,3,4}; // I-vi-IV-V
  const int* prog = progA;
  int psel = (int)std::lround((g_modVal[24]+1.f) * 1.5f);
  if (psel==1) prog = progB;
  else if (psel>=2) prog = progC;

  // Coltrane changes flavor (cycle major thirds)
  static const int progCol[] = {0,4,8,4}; // C -> E -> Ab -> E
  float stable = std::clamp(g_modVal[70], -1.f, 1.f); // fertility_mean
  if (stable > 0.2f && g_modVal[29] < 0.0f) {
    prog = progCol;
  }

  static const int biomeChordBase[BIOME_COUNT] = {48, 44, 46, 56, 40, 50, 38};
  static const int biomeArpBase[BIOME_COUNT]   = {8,  7,  8,  10, 6,  9,  5};
  int chordInterval = (int)std::lround((biomeChordBase[w.biome] - 6.0f*aggro) / g_tempoMult);
  int arpInterval = (int)std::lround((biomeArpBase[w.biome] - 2.0f*aggro) / g_tempoMult);
  // biome rhythm signatures
  if (w.biome==TROPICAL) {
    float swing = 0.5f + 0.5f*std::sin((float)tick*0.13f);
    arpInterval = std::max(3, (int)std::lround(arpInterval * (0.85f + 0.3f*swing)));
  } else if (w.biome==ALPINE) {
    arpInterval = std::max(4, arpInterval + 2);
  } else if (w.biome==DESERT) {
    arpInterval = std::max(3, arpInterval - 1);
  } else if (w.biome==WETLAND) {
    arpInterval = std::max(3, arpInterval + ((tick/12)&1));
  }
  if (trigEnv > 0.7f) arpInterval = std::max(2, arpInterval-1);
  if (g_modVal[16] > 0.2f) arpInterval = 6;
  if (g_modVal[19] > 0.4f) arpInterval = 5;
  chordInterval = std::max(16, chordInterval);
  arpInterval = std::max(4, arpInterval); // floor at 4 ticks — prevents voice pile-up if tempoMult spikes

  // phrase bursts driven by interaction intensity
  float burstSrc = std::clamp(g_modVal[40], -1.f, 1.f);
  if (burstSrc > 0.35f && g_music.phraseBurst==0) {
    int var = (int)std::lround((g_modVal[50]+1.f) * 3.0f); // biodiversity -> 0..6
    g_music.phrasePattern = std::clamp(var, 0, 6);
    g_music.phraseBurst = 8 + (int)std::lround(std::fabs(burstSrc)*6.0f);
    g_music.phraseStep = 0;
  }

  if (tick >= g_music.nextChordTick){
    g_music.chordIndex = (g_music.chordIndex + 1) % 4;
    g_music.chordType = (g_modVal[15] > 0.2f) ? 3 : (g_modVal[5] > 0.3f ? 4 : 0);
  if (aggro > 0.3f) g_music.chordType = 2; // dominant 7 for edge
  // consonance bias when stable
  if (stable > 0.4f) g_music.chordType = 0;
    g_music.nextChordTick = tick + chordInterval;

    // update motif memory occasionally
    if (g_music.motifHold <= 0 || g_modVal[50] > 0.6f) {
      g_music.motifLen = 4 + (int)std::lround(std::clamp(g_modVal[50], -1.f, 1.f) * 4.0f);
      g_music.motifLen = std::clamp(g_music.motifLen, 4, 8);
      for (int i=0;i<g_music.motifLen;++i){
        int step = (i * 2 + (int)std::lround(curious*2.f)) % 12;
        g_music.motif[i] = step + (i%3==0?0: (aggro>0.f?1:-1));
      }
      g_music.motifPos = 0;
      g_music.motifHold = 16 + (int)std::lround(std::fabs(emo)*12.0f);
    } else {
      g_music.motifHold--;
    }

    // chord pad + bass
    int degree = prog[g_music.chordIndex];
    int rootNote = 48 + degree*2; // rough diatonic-ish
    rootNote = quantizeNoteToScale(rootNote, root, scale);
    std::vector<int> chord;
    buildChord(chord, rootNote, scale, g_music.chordType);

    // voice 1: chord pad
    for (int n : chord){
      int note = quantizeNoteToScale(n+12, root, scale);
      queuedNoteOn(synth, midi, 1, note, 70, tick, chordInterval-2);
    }
    // voice 2: bass
    gatedNoteOn(synth, midi, 2, rootNote-12, 80, tick, chordInterval-4);
  }

  int arpJitter = (int)std::lround(std::clamp(g_modVal[58], -1.f, 1.f) * 2.0f);
  int arpTick = tick + arpJitter;
  if ((arpTick % arpInterval)==0){
    int degree = prog[g_music.chordIndex];
    int rootNote = 60 + degree*2;
    rootNote = quantizeNoteToScale(rootNote, root, scale);
    std::vector<int> chord;
    buildChord(chord, rootNote, scale, g_music.chordType);
    if (chord.empty()) return;

    int idx = g_music.arpStep++ % (int)chord.size();
    int note = chord[idx];
    // voice 0 melody driven by oddities
    int mel = note + (int)std::lround(g_modVal[8]*4.f + curious*3.f);
    if (g_music.motifLen > 0 && (g_music.arpStep % 2)==0) {
      int mstep = g_music.motif[g_music.motifPos++ % g_music.motifLen];
      mel = 60 + mstep + (int)std::lround(root/2);
    }
    mel = quantizeNoteToScale(mel, root, scale);
    if (aggro > 0.4f && stable < 0.3f && (g_music.arpStep % 3)==0) mel += (aggro>0.f ? 1 : -1); // chromatic passing
    int vel = 60 + (int)std::lround(std::clamp(g_modVal[4], -1.f, 1.f)*20.f + 20.f + trigEnv*25.f);
    gatedNoteOn(synth, midi, 0, mel, std::clamp(vel,30,110), tick, arpInterval-1);

    // trigger-based flourish (short grace note)
    if (trigEnv > 0.6f && g_trigType==1) {
      int grace = mel + ((tick % 2)==0 ? 2 : -2);
      grace = quantizeNoteToScale(grace, root, scale);
      gatedNoteOn(synth, midi, 0, grace, std::clamp(vel+10,40,120), tick, std::max(1, arpInterval/3));
    }

    // voice 3 shimmer tied to ripple energy
    int shimmer = note + 12 + (int)std::lround(g_modVal[15]*6.f + social*2.f);
    shimmer = quantizeNoteToScale(shimmer, root, scale);
    int svel = 50 + (int)std::lround(std::fabs(g_modVal[15])*40.f + trigEnv*15.f);
    gatedNoteOn(synth, midi, 3, shimmer, std::clamp(svel,30,110), tick, arpInterval-1);
  }

  // phrase burst flourishes
  if (g_music.phraseBurst > 0 && (tick % std::max(1, arpInterval/3)==0)) {
    static const int pat0[] = {0,2,4,7,4,2};
    static const int pat1[] = {0,3,5,7,10,7,5,3};
    static const int pat2[] = {0,2,3,5,7,8,10,12};
    static const int pat3[] = {0,4,7,11,7,4};
    static const int pat4[] = {0,1,3,6,8,11,12};
    static const int pat5[] = {0,2,5,9,12,9,5,2};
    static const int pat6[] = {0,2,4,5,7,9,11,12};
    const int* pat = pat0; int plen = 6;
    switch(g_music.phrasePattern){
      case 1: pat=pat1; plen=8; break;
      case 2: pat=pat2; plen=8; break;
      case 3: pat=pat3; plen=6; break;
      case 4: pat=pat4; plen=7; break;
      case 5: pat=pat5; plen=8; break;
      case 6: pat=pat6; plen=8; break;
    }
    int stepIdx = g_music.phraseStep++ % plen;
    int n = 60 + pat[stepIdx] + (int)std::lround(aggro*2.f) + (int)std::lround(curious*2.f);
    n = quantizeNoteToScale(n, root, scale);
    gatedNoteOn(synth, midi, 0, n, 100, tick, std::max(1, arpInterval/4));
    g_music.phraseBurst--;
  }

  // drums: three layered tracks (muted by default via volume)
  int stepInterval = std::max(2, (int)std::lround(4.0f / g_tempoMult));
  int step = (tick / stepInterval) % 16;
  float chaos = std::clamp(g_modVal[29]*0.6f + g_modVal[34]*0.4f, 0.f, 1.f);
  // track 1: core kit
  bool kick = (step==0 || step==8) || (chaos>0.5f && (step==3 || step==11));
  bool snare = (step==4 || step==12) || (chaos>0.7f && step==15);
  bool hat = (step%2==0) || (chaos>0.6f && step%2==1);
    if (g_drumVol>0) {
      int swing = (step % 2) ? 1 : 0;
      if (kick) drumHit(synth, midi, 36, 90, g_drumVol);
      if (snare) drumHit(synth, midi, 38, 80, g_drumVol);
      if (hat) drumHit(synth, midi, 42, 60, g_drumVol);
    }
  // track 2: percussion layer
  if (g_drum2Vol>0) {
    bool perc = (step==2 || step==6 || step==10 || step==14) || (chaos>0.6f && step%4==1);
    bool toms = (step==7 || step==15) && chaos>0.4f;
    if (perc) drumHit(synth, midi, 56, 70, g_drum2Vol);
    if (toms) drumHit(synth, midi, 45, 75, g_drum2Vol);
  }
  // track 3: cymbals/accents
  if (g_drum3Vol>0) {
    bool ride = (step%4==0) || (chaos>0.5f && step%2==1);
    bool crash = (step==0 || step==8) && chaos>0.5f;
    if (ride) drumHit(synth, midi, 51, 60, g_drum3Vol);
    if (crash) drumHit(synth, midi, 49, 90, g_drum3Vol);
  }

  for (const auto& ev : w.events){
    int note=36, vel=60, dur=4;
    switch(ev.type){
      case Event::EV_LIGHTNING: note=49; vel=110; dur=6; break;
      case Event::EV_STORM: note=57; vel=90; dur=4; break;
      case Event::EV_RAIN: note=42; vel=60; dur=3; break;
      case Event::EV_FIRE: note=38; vel=85; dur=4; break;
      case Event::EV_DEATH: note=45; vel=90; dur=5; break;
      case Event::EV_BIRTH: note=39; vel=70; dur=3; break;
      default: break;
    }
    if (g_drumVol>0) drumHit(synth, midi, note, vel, g_drumVol);
  }
}

static void applyAutomation(SynthOut& synth, MidiOut& midi, int tick){
  static int lastCC11[4] = {-1,-1,-1,-1};
  static int lastCC74[4] = {-1,-1,-1,-1};
  static int lastCC10[4] = {-1,-1,-1,-1};
  static int lastCC5[4] = {-1,-1,-1,-1};
  static int lastCC7[4] = {-1,-1,-1,-1};
  static int lastDVol = -1;
  static float lastGain = -1.f;
  bool periodic = (tick % 8) == 0;
  for(int ch=0; ch<4; ++ch){
    if (g_voiceProgDirty[ch] && (tick - g_progLastTick[ch] > 12)) {
      applyVoiceProgramNow(synth, midi, ch, tick);
    }
    int cc11 = (int)std::lround(std::clamp(g_cc11Expr,0.f,1.f)*127.f);
    int cc74 = (int)std::lround(std::clamp(g_cc74Bright,0.f,1.f)*127.f);
    int cc10 = (int)std::lround(std::clamp(g_pan01,0.f,1.f)*127.f);
    if (periodic || cc11 != lastCC11[ch]) { synth.cc(ch, 11, cc11); midi.sendCC(ch,11,cc11); lastCC11[ch]=cc11; }
    if (periodic || cc74 != lastCC74[ch]) { synth.cc(ch, 74, cc74); midi.sendCC(ch,74,cc74); lastCC74[ch]=cc74; }
    if (periodic || cc10 != lastCC10[ch]) { synth.cc(ch, 10, cc10); midi.sendCC(ch,10,cc10); lastCC10[ch]=cc10; }
    int porta = (int)std::lround(std::clamp(g_porta01[ch],0.f,1.f)*127.f);
    if (periodic || porta != lastCC5[ch]) { synth.cc(ch, 5, porta); midi.sendCC(ch,5,porta); lastCC5[ch]=porta; }
    int vol = clampi(g_voiceVol[ch], 0, 127);
    if (periodic || vol != lastCC7[ch]) { synth.cc(ch, 7, vol); midi.sendCC(ch,7,vol); lastCC7[ch]=vol; }
  }
  int dvol = std::max({clampi(g_drumVol, 0, 127), clampi(g_drum2Vol, 0, 127), clampi(g_drum3Vol, 0, 127)});
  if (periodic || dvol != lastDVol) { synth.cc(9, 7, dvol); midi.sendCC(9,7,dvol); lastDVol = dvol; }
  // track 2/3 share drum channel; scale their note velocities by volume gate (handled in synthTickMusic)
  float g = std::clamp(g_masterGain, 0.1f, 2.0f);
  if (periodic || std::fabs(g - lastGain) > 0.01f) { synth.setGain(g); lastGain = g; }
}

// ===== Rendering =====
static inline int waterDepthLevel(float w){
  if (w <= 0.f) return 0;
  int d = (int)std::lround(std::clamp(w / MAX_WATER, 0.f, 1.f) * 7.f);
  return clampi(d, 1, 7);
}

static inline char waterGlyph(float w){
  int d = waterDepthLevel(w);
  if (d <= 0) return '.';
  return (char)('0' + d);
}

static inline float wavePhase(const World& w, int x, int y, int tick){
  float swell = 0.5f;
  switch(w.biome){
    case TROPICAL: swell = 1.0f; break;
    case WETLAND:  swell = 0.6f; break;
    case MEADOW:   swell = 0.5f; break;
    case ALPINE:   swell = 0.35f; break;
    case TAIGA:    swell = 0.4f; break;
    case DESERT:   swell = 0.7f; break;
    case ALIEN:    swell = 0.9f; break;
  }
  float t = (float)tick * 0.05f * swell;
  float tide = std::sin((float)w.weather.timer * 0.01f);
  float wx = (float)w.wind.dx;
  float wy = (float)w.wind.dy;
  float f1 = std::sin(t + x*0.06f + y*0.02f + wx*0.8f + wy*0.5f);
  float f2 = std::cos(t*0.7f + x*0.02f - y*0.05f + tide*1.2f);
  float f3 = std::sin(t*1.1f + x*0.01f + y*0.01f + tide*0.6f);
  return f1 + f2 + 0.6f*f3;
}

static inline float wavePhaseLong(const World& w, int x, int y, int tick){
  float swell = 0.6f;
  switch(w.biome){
    case TROPICAL: swell = 1.1f; break;
    case WETLAND:  swell = 0.7f; break;
    case MEADOW:   swell = 0.6f; break;
    case ALPINE:   swell = 0.4f; break;
    case TAIGA:    swell = 0.45f; break;
    case DESERT:   swell = 0.8f; break;
    case ALIEN:    swell = 1.0f; break;
  }
  float t = (float)tick * 0.018f * swell;
  float tide = std::sin((float)w.weather.timer * 0.006f);
  float wx = (float)w.wind.dx;
  float wy = (float)w.wind.dy;
  float f1 = std::sin(t + x*0.018f + y*0.012f + wx*0.5f + wy*0.3f);
  float f2 = std::cos(t*0.6f + x*0.01f - y*0.02f + tide*0.9f);
  return f1 + 0.8f*f2;
}

static inline char waterFlowGlyph(const World& w, int x, int y, int tick){
  int d = waterDepthLevel(w.water[y][x]);
  if (d <= 0) return '.';

  int bestDx = 0, bestDy = 0;
  float here = (float)w.height[y][x] + w.water[y][x]*8.f;
  float bestDrop = 0.f;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (!dx && !dy) continue;
    int nx=x+dx, ny=y+dy;
    if (!inBounds(nx,ny)) continue;
    float there = (float)w.height[ny][nx] + w.water[ny][nx]*8.f;
    float drop = here - there;
    if (drop > bestDrop) { bestDrop = drop; bestDx = dx; bestDy = dy; }
  }

  // complex swell + wind-driven wave direction with eddies
  float phase = wavePhase(w, x, y, tick);
  float lphase = wavePhaseLong(w, x, y, tick);
  float tide = std::sin((float)w.weather.timer * 0.01f);
  float ang = (float)tick * 0.02f + tide*0.9f;
  float wx = (float)w.wind.dx * 0.6f;
  float wy = (float)w.wind.dy * 0.6f;
  float t = (float)tick * 0.035f;
  float eddy = std::sin(t + x*0.05f - y*0.03f + tide*0.8f) + std::cos(t*0.7f + x*0.02f + y*0.06f + tide*0.4f);
  float gx = 0.f, gy = 0.f;
  if (inBounds(x+1,y) && inBounds(x-1,y)) gx = (float)w.height[y][x+1] - (float)w.height[y][x-1] + (w.water[y][x+1] - w.water[y][x-1])*8.f;
  if (inBounds(x,y+1) && inBounds(x,y-1)) gy = (float)w.height[y+1][x] - (float)w.height[y-1][x] + (w.water[y+1][x] - w.water[y-1][x])*8.f;
  float glen = std::sqrt(gx*gx + gy*gy);
  if (glen > 0.001f) { gx/=glen; gy/=glen; }
  float swirl = (phase > 0.4f ? 0.25f : 0.12f) * eddy * (0.7f + 0.3f*std::fabs(tide));
  float vx = std::cos(ang) + wx + phase*0.2f + lphase*0.1f + (-gy)*swirl;
  float vy = std::sin(ang) + wy - phase*0.15f - lphase*0.08f + (gx)*swirl;
  int sdx = (vx > 0.35f) ? 1 : (vx < -0.35f ? -1 : 0);
  int sdy = (vy > 0.35f) ? 1 : (vy < -0.35f ? -1 : 0);
  if (d >= 2 && (sdx != 0 || sdy != 0)) { bestDx = sdx; bestDy = sdy; }

  // curl along shore when waves hit
  bool shore = false;
  float nx = 0.f, ny = 0.f;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx){
    if (!dx && !dy) continue;
    int nxp=x+dx, nyp=y+dy;
    if (!inBounds(nxp,nyp)) continue;
    if (w.water[nyp][nxp] <= 0.2f) {
      shore = true;
      nx -= (float)dx;
      ny -= (float)dy;
    }
  }
  if (shore) {
    float len = std::sqrt(nx*nx + ny*ny);
    if (len > 0.001f) { nx/=len; ny/=len; }
    float tx = -ny;
    float ty = nx;
    float phase = wavePhase(w, x, y, tick);
    float lphase = wavePhaseLong(w, x, y, tick);
    if (phase > 1.0f || lphase > 1.0f) {
      int tdx = (tx > 0.35f) ? 1 : (tx < -0.35f ? -1 : 0);
      int tdy = (ty > 0.35f) ? 1 : (ty < -0.35f ? -1 : 0);
      if (tdx!=0 || tdy!=0) { bestDx = tdx; bestDy = tdy; }
    }
  }

  if (bestDrop < 1.0f && (bestDx==0 && bestDy==0)) return (char)('0' + d);

  int cat = 0; // 0 horiz, 1 vert, 2 diag
  if (bestDx != 0 && bestDy != 0) cat = 2;
  else if (bestDy != 0) cat = 1;

  int base = (cat==0 ? 0x01 : (cat==1 ? 0x08 : 0x0F));
  int phaseStep = ((tick/6) + x + y) & 1;
  int dd = d;
  if (dd >= 4 && phaseStep) dd = std::max(1, dd-1);
  return (char)(base + (dd-1));
}

static inline RGB lerpRGB(const RGB& a, const RGB& b, float t){
  return { (uint8_t)std::lround(a.r + (b.r-a.r)*t),
           (uint8_t)std::lround(a.g + (b.g-a.g)*t),
           (uint8_t)std::lround(a.b + (b.b-a.b)*t) };
}

static inline RGB gradeColor(RGB c){
  float rf = c.r / 255.f;
  float gf = c.g / 255.f;
  float bf = c.b / 255.f;
  float avg = (rf + gf + bf) / 3.f;
  float sat = 1.35f * g_gradeSat;
  rf = avg + (rf - avg) * sat;
  gf = avg + (gf - avg) * sat;
  bf = avg + (bf - avg) * sat;
  auto adj = [&](float f){
    f = (f - 0.5f) * (1.10f * g_gradeContrast) + 0.5f; // contrast
    f += g_gradeLift;
    return (uint8_t)clampi((int)std::lround(f * 255.f), 0, 255);
  };
  c.r = adj(rf);
  c.g = adj(gf);
  c.b = adj(bf);
  return c;
}

static RGB fgForChar(const World& w, char c, int x, int y, Season s){
  const BiomeDef& b0 = g_biomesEdit[w.biome];
  const BiomeDef& b1 = g_biomesEdit[w.biomeMorphActive ? w.targetBiome : w.biome];
  float t = w.biomeMorphActive ? w.biomeMorphT : 0.f;
  BiomeDef b = b0;
  b.waterDeep = lerpRGB(b0.waterDeep, b1.waterDeep, t);
  b.waterShallow = lerpRGB(b0.waterShallow, b1.waterShallow, t);
  b.foam = lerpRGB(b0.foam, b1.foam, t);
  b.soil = lerpRGB(b0.soil, b1.soil, t);
  b.grass = lerpRGB(b0.grass, b1.grass, t);
  b.tree = lerpRGB(b0.tree, b1.tree, t);
  b.flower = lerpRGB(b0.flower, b1.flower, t);
  b.rock = lerpRGB(b0.rock, b1.rock, t);

  uint32_t h = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(w.seed + (uint32_t)s*131));
  auto jitter = [&](RGB c0, int amt)->RGB{
    int jr = (int)((h>>8)&7) - 3;
    int jg = (int)((h>>11)&7) - 3;
    int jb = (int)((h>>14)&7) - 3;
    c0.r = (uint8_t)clampi((int)c0.r + jr*amt, 0, 255);
    c0.g = (uint8_t)clampi((int)c0.g + jg*amt, 0, 255);
    c0.b = (uint8_t)clampi((int)c0.b + jb*amt, 0, 255);
    return c0;
  };
  auto pickRGB = [&](const RGB* arr, int n, uint32_t h)->RGB{
    return arr[h % (uint32_t)n];
  };

  if (c==' ') return {0,0,0};
  if ((c>='1' && c<='7') || (c>=0x01 && c<=0x15)) {
    int d = 1;
    if (c>='1' && c<='7') d = c - '0';
    else d = ((int)c - 0x01) % 7 + 1;
    float t = d/7.f;
    float t2 = t*t;
    int r = (int)(b.waterShallow.r*(1.f-t2) + b.waterDeep.r*t2);
    int g = (int)(b.waterShallow.g*(1.f-t2) + b.waterDeep.g*t2);
    int bl = (int)(b.waterShallow.b*(1.f-t2) + b.waterDeep.b*t2);
    float bright = (t < 0.4f) ? 1.25f : (t > 0.75f ? 0.70f : 1.0f);
    r = clampi((int)std::lround(r * bright), 0, 255);
    g = clampi((int)std::lround(g * bright), 0, 255);
    bl = clampi((int)std::lround(bl * bright), 0, 255);
    return { (uint8_t)r, (uint8_t)g, (uint8_t)bl };
  }
  if (c=='=') { return b.foam; }
  if (c=='d' || c=='e' || c=='g') { RGB out = boostColor(jitter({110,80,55}, 4), 1.15f, 1.10f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='^') { RGB out = boostColor(b.rock, 1.15f, 1.10f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='.') { RGB out = boostColor(jitter(b.soil, 3), 1.20f, 1.10f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='s') { RGB out = boostColor(jitter({150,120,80}, 4), 1.20f, 1.10f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c==',' || c=='"' || c==';' || c=='#') {
    if (w.biome==MEADOW) {
      static const RGB meadowShort[] = {{88,180,110},{68,156,96},{58,140,92},{96,196,132},{78,170,120}};
      static const RGB meadowTall[]  = {{66,170,98},{52,152,86},{44,136,78},{74,186,122}};
      static const RGB meadowShrub[] = {{52,146,86},{42,126,78},{36,116,72},{62,160,96}};
      static const RGB meadowReed[]  = {{72,180,118},{62,164,110},{82,196,130}};
      uint32_t hh = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(w.seed + 911));
      RGB base = b.grass;
      if (c==',') base = pickRGB(meadowShort, (int)(sizeof(meadowShort)/sizeof(meadowShort[0])), hh);
      else if (c=='"') base = pickRGB(meadowTall, (int)(sizeof(meadowTall)/sizeof(meadowTall[0])), hh);
      else if (c==';') base = pickRGB(meadowShrub, (int)(sizeof(meadowShrub)/sizeof(meadowShrub[0])), hh);
      else if (c=='#') base = pickRGB(meadowReed, (int)(sizeof(meadowReed)/sizeof(meadowReed[0])), hh);
      if (s==AUTUMN) base = { (uint8_t)std::min(255, base.r+50), (uint8_t)std::min(255, base.g+15), (uint8_t)std::max(0, base.b-20) };
      if (s==WINTER) base = { (uint8_t)std::min(255, base.r+30), (uint8_t)std::min(255, base.g+30), (uint8_t)std::min(255, base.b+30) };
      RGB out = boostColor(jitter(base, 6), 1.30f, 1.15f);
      return g_ghibliPalette?boostColor(out,1.06f,1.04f):out;
    }
    RGB base = b.grass;
    if (s==AUTUMN) base = { (uint8_t)std::min(255, base.r+50), (uint8_t)std::min(255, base.g+15), (uint8_t)std::min(255, base.b-20) };
    if (s==WINTER) base = { (uint8_t)std::min(255, base.r+30), (uint8_t)std::min(255, base.g+30), (uint8_t)std::min(255, base.b+30) };
    RGB out = boostColor(jitter(base, 4), 1.30f, 1.15f);
    return g_ghibliPalette?boostColor(out,1.06f,1.04f):out;
  }
  if (c=='T' || c=='Y') { RGB out = boostColor(jitter(b.tree, 3), 1.25f, 1.10f); return g_ghibliPalette?boostColor(out,1.06f,1.04f):out; }
  if (c=='m') { RGB out = boostColor({40,230,150}, 1.45f, 1.25f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='a') { RGB out = boostColor({30,200,190}, 1.45f, 1.25f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='t') { RGB out = boostColor({220,170,100}, 1.20f, 1.10f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='l') { RGB out = boostColor({140,210,160}, 1.20f, 1.10f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='n') { RGB out = boostColor({90,230,140}, 1.35f, 1.20f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='q') { RGB out = boostColor({200,90,255}, 1.35f, 1.25f); return g_ghibliPalette?boostColor(out,1.05f,1.03f):out; }
  if (c=='f' || c=='+' || c=='&' || c=='!') {
    if (w.biome==MEADOW) {
      static const RGB meadowFlower[] = {
        {255,160,190},{255,220,120},{200,170,255},{160,220,255},
        {255,190,140},{255,120,150},{245,245,245},{210,255,160}
      };
      uint32_t hh = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(w.seed + 777));
      RGB alt = pickRGB(meadowFlower, (int)(sizeof(meadowFlower)/sizeof(meadowFlower[0])), hh);
      if (c=='+') { alt.r = (uint8_t)clampi((int)alt.r + 12,0,255); alt.g = (uint8_t)clampi((int)alt.g + 8,0,255); }
      if (c=='&') { alt.b = (uint8_t)clampi((int)alt.b + 14,0,255); }
      if (c=='!') { alt.r = (uint8_t)clampi((int)alt.r + 16,0,255); alt.b = (uint8_t)clampi((int)alt.b + 10,0,255); }
      RGB out = boostColor(jitter(alt, 7), 1.45f, 1.25f);
      return g_ghibliPalette?boostColor(out,1.06f,1.04f):out;
    }
    static const RGB flowerPal[BIOME_COUNT][4] = {
      {{255,210,230},{255,150,190},{255,230,140},{190,220,255}}, // meadow
      {{240,240,255},{200,255,220},{255,200,230},{210,240,200}}, // wetland
      {{230,220,255},{200,230,255},{255,210,230},{210,255,230}}, // alpine
      {{255,210,150},{255,170,120},{240,220,140},{255,230,190}}, // desert
      {{255,180,200},{255,220,140},{180,240,220},{200,210,255}}, // tropical
      {{230,210,240},{210,230,200},{240,220,170},{200,220,255}}, // taiga
      {{230,90,255},{190,60,220},{120,200,255},{255,120,220}},   // alien
    };
    uint32_t hh = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(w.seed + 777));
    RGB alt = flowerPal[w.biome][hh & 3u];
    return boostColor(jitter(alt, 8), 1.45f, 1.25f);
  }
  if (c=='*') return {255,140,60};
  if (c=='x') return {80,80,80};
  if (c>='A' && c<='Z') {
    if (c=='A' || c=='Z') return jitter({200,80,220}, 6);
    RGB base = (b.tree.r + b.rock.r > 0) ? RGB{(uint8_t)((b.tree.r+b.rock.r)/2),(uint8_t)((b.tree.g+b.rock.g)/2),(uint8_t)((b.tree.b+b.rock.b)/2)} : b.tree;
    return boostColor(jitter(base, 5), 1.1f, 1.1f);
  }
  // animals
  if (std::strchr("rdgfcwpbevAZ", c)) {
    auto grade = [&](RGB c0){ return g_ghibliPalette?boostColor(c0,1.08f,1.04f):c0; };
    if (c=='r') return grade(boostColor({210,160,120}, 1.35f, 1.15f)); // rabbit
    if (c=='d') return grade(boostColor({200,150,95}, 1.2f, 1.10f));  // deer
    if (c=='g') return grade(boostColor({170,170,150}, 1.2f, 1.10f)); // goat
    if (c=='f') return grade(boostColor({90,220,255}, 1.45f, 1.25f));  // fish
    if (c=='c') return grade(boostColor({230,140,90}, 1.35f, 1.15f));  // crab
    if (c=='p') return grade(boostColor({120,230,150}, 1.35f, 1.15f)); // frog
    if (c=='w') return grade(boostColor({190,190,210}, 1.15f, 1.10f)); // wolf
    if (c=='b') return grade(boostColor({150,120,90}, 1.2f, 1.10f));  // bear
    if (c=='e') return grade(boostColor({70,180,225}, 1.3f, 1.15f));  // eel
    if (c=='v') return grade(boostColor({210,210,150}, 1.3f, 1.10f)); // bird
    if (c=='x') return grade(boostColor({220,120,90}, 1.3f, 1.15f)); // fox
    if (c=='o') return grade(boostColor({200,140,110}, 1.25f, 1.10f)); // boar
    if (c=='u') return grade(boostColor({120,200,140}, 1.25f, 1.10f)); // turtle
    if (c=='h') return grade(boostColor({200,200,120}, 1.25f, 1.10f)); // hawk
    if (c=='s') return grade(boostColor({200,160,120}, 1.25f, 1.10f)); // snake
    if (c=='A' || c=='Z') return grade(boostColor({220,90,240}, 1.4f, 1.20f));
    return {220,220,220};
  }
  return {200,200,200};
}

static void render(SDL_Renderer* ren, const Layout& L, World& w, GlyphCache& gcWorld, GlyphCache& gcText, int tick, bool showMenu, int menuPage, int menuSel){
  setColor(ren, 0,0,0);
  SDL_RenderClear(ren);
  char buf[256];

  float camRX = g_camFX;
  float camRY = g_camFY;
  float zoomR = std::clamp(g_zoomF, 1.0f, 12.0f);
  clampCameraFToZoom(camRX, camRY, zoomR);
  int camX0 = clampi((int)std::lround(camRX), 0, W-1);
  int camY0 = clampi((int)std::lround(camRY), 0, H-1);
  int viewW = zoomViewWFor(zoomR);
  int viewH = zoomViewHFor(zoomR);
  camX0 = clampi(camX0, 0, std::max(0, W - viewW));
  camY0 = clampi(camY0, 0, std::max(0, H - viewH));

  int mouseX = 0, mouseY = 0;
  SDL_GetMouseState(&mouseX, &mouseY);
  bool mouseInSim = (mouseX >= 0 && mouseX < L.screenW && mouseY >= 0 && mouseY < L.simHpx);
  int hoverWX = -1, hoverWY = -1;
  const Agent* hoverAgent = nullptr;
  const BigCreature* hoverBig = nullptr;
  if (g_hoverInspect && mouseInSim) {
    int sx = (int)((int64_t)mouseX * viewW / std::max(1, L.screenW));
    int sy = (int)((int64_t)mouseY * viewH / std::max(1, L.simHpx));
    hoverWX = clampi(camX0 + sx, 0, W-1);
    hoverWY = clampi(camY0 + sy, 0, H-1);
    hoverAgent = hoveredAgentAt(w, hoverWX, hoverWY);
    if (hoverAgent) {
      hoverWX = hoverAgent->x;
      hoverWY = hoverAgent->y;
    } else {
      hoverBig = hoveredBigAt(w, hoverWX, hoverWY);
    }
  }
  if (g_hoverInspect && g_inspectPinned) {
    if (!g_inspectPinnedIsBig) {
      const Agent* pinned = nullptr;
      for (const auto& a : w.agents) {
        if (a.id == g_inspectPinnedAgentId) { pinned = &a; break; }
      }
      if (pinned) {
        hoverAgent = pinned;
        hoverBig = nullptr;
        hoverWX = pinned->x;
        hoverWY = pinned->y;
      } else {
        clearInspectPin();
      }
    } else {
      const BigCreature* pinned = nullptr;
      int best = 9999;
      for (const auto& b : w.bigs) {
        if (b.glyph != g_inspectPinnedBigGlyph) continue;
        int d = std::abs(b.x - g_inspectPinnedBigX) + std::abs(b.y - g_inspectPinnedBigY);
        if (d < best) { best = d; pinned = &b; }
      }
      if (pinned && best < 32) {
        hoverBig = pinned;
        hoverAgent = nullptr;
        hoverWX = pinned->x;
        hoverWY = pinned->y;
        g_inspectPinnedBigX = pinned->x;
        g_inspectPinnedBigY = pinned->y;
      } else {
        clearInspectPin();
      }
    }
  }

  for(int y=0;y<viewH;++y){
    int y0 = (y * L.simHpx) / viewH;
    int y1 = ((y+1) * L.simHpx) / viewH;
    int hpx = std::max(1, y1 - y0);
    for(int x=0;x<viewW;++x){
      int x0 = (x * L.screenW) / viewW;
      int x1 = ((x+1) * L.screenW) / viewW;
      int wpx = std::max(1, x1 - x0);
      SDL_Rect rc{ x0, y0, wpx, hpx };

      int wx = camX0 + x;
      int wy = camY0 + y;
      char c = renderCharAt(w, wx, wy, tick);
      if (w.water[wy][wx] <= 0.2f && c == w.terrain[wy][wx]) {
        uint32_t h = hash3((uint32_t)wx,(uint32_t)wy,(uint32_t)(tick/7));
        c = terrainGlyphVariant(c, h, seasonAt(tick), w.weather);
      }

      SDL_Texture* gt = gcWorld.get(ren, (unsigned char)c);
      if (gt) {
        RGB fg = gradeColor(fgForChar(w, c, wx, wy, seasonAt(tick)));
        SDL_SetTextureColorMod(gt, fg.r, fg.g, fg.b);
        SDL_RenderCopy(ren, gt, nullptr, &rc);
      }

      if ((hoverAgent || hoverBig) && wx==hoverWX && wy==hoverWY) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        setColor(ren, 245,245,255, 45);
        SDL_RenderFillRect(ren, &rc);
        setColor(ren, 255,255,255, 190);
        SDL_RenderDrawRect(ren, &rc);
      }

      // Shoreline foam.
      if (w.water[wy][wx] > 0.2f) {
        bool shore = false;
        float nx = 0.f, ny = 0.f;
        for (int dy=-1; dy<=1 && !shore; ++dy) for (int dx=-1; dx<=1 && !shore; ++dx){
          if (!dx && !dy) continue;
          int nxp=wx+dx, nyp=wy+dy;
          if (!inBounds(nxp,nyp) || w.water[nyp][nxp] <= 0.2f) { shore = true; nx -= (float)dx; ny -= (float)dy; }
        }
        if (shore && ((hash3((uint32_t)wx,(uint32_t)wy,(uint32_t)(tick/3)) & 3u)==0u)) {
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
          setColor(ren, 200,210,220, 32);
          SDL_RenderFillRect(ren, &rc);
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        }
        // wave crests (wind + tide driven)
        float phase = wavePhase(w, wx, wy, tick);
        float lphase = wavePhaseLong(w, wx, wy, tick);
        float tide = std::sin((float)w.weather.timer * 0.01f);
        float waveBand = phase + tide*0.35f + ((float)w.wind.strength + g_windGust)*0.08f;
        float setBand = lphase + tide*0.25f;
        bool nearShore = shore;
        if (w.water[wy][wx] > 0.8f && waveBand > 1.7f) {
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
          setColor(ren, 190,205,215, nearShore ? 38 : 26);
          SDL_RenderFillRect(ren, &rc);
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        }
        if (w.water[wy][wx] > 0.6f && setBand > 1.35f) {
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
          setColor(ren, 180,195,210, nearShore ? 34 : 22);
          SDL_RenderFillRect(ren, &rc);
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        }
        if (shore) {
          float len = std::sqrt(nx*nx + ny*ny);
          if (len > 0.001f) { nx/=len; ny/=len; }
          float tx = -ny;
          float ty = nx;
          float along = wx*tx + wy*ty;
          float band = std::sin(along*0.35f + (float)tick*0.08f + phase*0.6f + tide*0.4f);
          float band2 = std::sin(along*0.22f + (float)tick*0.04f + lphase*0.5f + tide*0.2f);
          if ((phase > 1.0f && band > 0.55f) || (lphase > 1.0f && band2 > 0.45f)) {
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            setColor(ren, 210,220,230, nearShore ? 70 : 50);
            SDL_RenderFillRect(ren, &rc);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
          }
        }
      }

      // Localized emissive falloff (fire only).
      bool emissive = (c=='*');
      if (emissive) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
        uint8_t er=180, eg=210, eb=255;
        int a0 = 12;
        if (c=='*') { er=255; eg=130; eb=70; a0=28; }
        if (w.weather.state==STORM && ((hash3((uint32_t)wx,(uint32_t)wy,(uint32_t)tick) & 63u)==0u)) {
          er=220; eg=235; eb=255; a0 += 18;
        }
        SDL_Rect r0{rc.x-rc.w/2, rc.y-rc.h/2, rc.w*2, rc.h*2};
        SDL_Rect r1{rc.x-rc.w, rc.y-rc.h, rc.w*3, rc.h*3};
        SDL_Rect r2{rc.x-rc.w*2, rc.y-rc.h*2, rc.w*5, rc.h*5};
        setColor(ren, er,eg,eb, (uint8_t)clampi(a0, 0, 80)); SDL_RenderFillRect(ren, &r0);
        setColor(ren, er,eg,eb, (uint8_t)clampi(a0/2, 0, 40)); SDL_RenderFillRect(ren, &r1);
        setColor(ren, er,eg,eb, (uint8_t)clampi(a0/4, 0, 20)); SDL_RenderFillRect(ren, &r2);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
      }

  // cloud overlay
      if (w.overlay[wy][wx] == 'o') {
        SDL_Texture* ct = gcWorld.get(ren, (unsigned char)'o');
        if (ct) {
          SDL_SetTextureColorMod(ct, 230, 230, 240);
          SDL_SetTextureAlphaMod(ct, 140);
          SDL_RenderCopy(ren, ct, nullptr, &rc);
          SDL_SetTextureAlphaMod(ct, 255);
        } else {
          setColor(ren, 230,230,240, 120);
          SDL_RenderFillRect(ren, &rc);
        }
      }

      // cloud shadow (offset by wind)
      int sx = wx - w.wind.dx*2;
      int sy = wy - w.wind.dy*2;
      if (inBounds(sx,sy) && w.overlay[sy][sx]=='o') {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        setColor(ren, 0,0,0, 40);
        SDL_RenderFillRect(ren, &rc);
      }
    }
  }

  // Foreground particles (near-camera depth layer).
  {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int n = 160;
    if (w.weather.state==RAIN || w.weather.state==STORM) n = 260;
    if (g_wantSynth) n = std::max(60, (int)(n * 0.45f));
    int gust = (int)std::lround(g_windGust);
    for (int i=0; i<n; ++i){
      int x = (int)((i*53 + tick*(4 + std::max(0,w.wind.strength) + gust)) % std::max(1, L.screenW));
      int y = (int)((i*97 + tick*(2 + std::max(0,w.wind.strength) + gust)) % std::max(1, L.simHpx));
      x = (x + (int)std::lround(camRX*1.5f) + w.wind.dx*(i&3)) % std::max(1, L.screenW);
      y = (y + (int)std::lround(camRY*1.0f) + w.wind.dy*(i&1)) % std::max(1, L.simHpx);
      if (x<0) x += L.screenW;
      if (y<0) y += L.simHpx;
      SDL_Rect p{x,y,2,2};
      uint8_t a = (uint8_t)(w.weather.state==CLEAR ? 14 : 22);
      setColor(ren, 140,150,160, a);
      SDL_RenderFillRect(ren, &p);
    }
  }

  // Ambient biome particles (subtle, slow drift).
  {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int n = 60;
    uint8_t r=160,g=160,b=170,a=18;
    int speed = 1;
    switch(w.biome){
      case MEADOW:  r=140; g=160; b=120; a=16; n=70; speed=1; break;   // pollen
      case WETLAND: r=120; g=150; b=150; a=18; n=60; speed=1; break;   // mist motes
      case ALPINE:  r=210; g=220; b=230; a=20; n=50; speed=1; break;   // ice spark
      case DESERT:  r=170; g=130; b=80;  a=16; n=80; speed=2; break;   // dust
      case TROPICAL:r=120; g=180; b=140; a=18; n=70; speed=1; break;   // fireflies
      case TAIGA:   r=180; g=190; b=210; a=18; n=55; speed=1; break;   // chill motes
      case ALIEN:   r=190; g=120; b=220; a=20; n=65; speed=1; break;   // spores
      default: break;
    }
    if (g_wantSynth) n = std::max(20, (int)(n * 0.5f));
    for (int i=0; i<n; ++i){
      int x = (i*71 + tick*speed + (w.wind.dx*2)) % std::max(1, L.screenW);
      int y = (i*113 + tick*(speed/2 + 1) + (w.wind.dy*2)) % std::max(1, L.screenH);
      if (x<0) x += L.screenW;
      if (y<0) y += L.screenH;
      SDL_Rect p{x,y,2,2};
      setColor(ren, r,g,b, a);
      SDL_RenderFillRect(ren, &p);
    }
  }

  // biome-specific weather visuals
  if (w.biome==MEADOW && w.weather.state==CLEAR) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    setColor(ren, 120,110,80, 18);
    SDL_Rect rc{0,0,L.screenW,L.screenH};
    SDL_RenderFillRect(ren, &rc);
    for (int x=0; x<L.screenW; x+=70){
      int xo = (int)(20 * std::sin((float)(tick*0.02f + x*0.01f)));
      SDL_Rect rr{clampi(x+xo,0,L.screenW-1), 0, 10, L.screenH};
      setColor(ren, 140,130,90, 10);
      SDL_RenderFillRect(ren, &rr);
    }
  }
  if (w.biome==WETLAND && w.weather.humidity>0.6f) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    setColor(ren, 110,140,140, 22);
    SDL_Rect rc{0,0,L.screenW,L.screenH};
    SDL_RenderFillRect(ren, &rc);
    for (int y=0; y<L.screenH; y+=18){
      int a = 14 + (int)(6 * std::sin((float)(tick*0.05f + y*0.08f)));
      SDL_Rect rr{0,y,L.screenW,6};
      setColor(ren, 120,150,150, (uint8_t)clampi(a,8,22));
      SDL_RenderFillRect(ren, &rr);
    }
    for (int y=0; y<L.screenH; y+=42){
      int xo = (int)(12 * std::sin((float)(tick*0.03f + y*0.04f)));
      SDL_Rect rr{clampi(xo,0,L.screenW-1), y, L.screenW - std::abs(xo), 8};
      setColor(ren, 100,130,130, 12);
      SDL_RenderFillRect(ren, &rr);
    }
  }
  if (w.biome==DESERT && w.weather.state==CLEAR) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int y=0;y<L.screenH;y+=6){
      SDL_Rect rc{0,y,L.screenW,2};
      setColor(ren, 180,140,90, 10);
      SDL_RenderFillRect(ren, &rc);
    }
    for (int y=0; y<L.screenH; y+=20){
      int xoff = (int)(8 * std::sin((float)(tick*0.08f + y*0.11f)));
      SDL_Rect rc{clampi(xoff,0,L.screenW-1), y, L.screenW - std::abs(xoff), 3};
      setColor(ren, 170,120,80, 10);
      SDL_RenderFillRect(ren, &rc);
    }
  }
  if ((w.biome==ALPINE || w.biome==TAIGA) && seasonAt(tick)==WINTER) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int snowN = g_wantSynth ? 140 : 300;
    for (int i=0;i<snowN;i++){
      int x = (i*37 + tick*3) % L.screenW;
      int y = (i*71 + tick*2) % L.screenH;
      SDL_Rect rc{x,y,2,2};
      setColor(ren, 230,240,255, 40);
      SDL_RenderFillRect(ren, &rc);
    }
  }
  if (w.biome==ALPINE && w.weather.state==CLEAR) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int i=0;i<120;i++){
      int x = (i*59 + tick*2) % L.screenW;
      int y = (i*97 + tick*3) % L.screenH;
      SDL_Rect rc{x,y,2,2};
      setColor(ren, 210,230,240, 18);
      SDL_RenderFillRect(ren, &rc);
    }
  }
  if (w.biome==TAIGA && w.weather.state==CLEAR) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int y=0; y<L.screenH; y+=24){
      int a = 10 + (int)(6 * std::sin((float)(tick*0.04f + y*0.09f)));
      SDL_Rect rr{0,y,L.screenW,4};
      setColor(ren, 80,130,100, (uint8_t)clampi(a,6,16));
      SDL_RenderFillRect(ren, &rr);
    }
  }
  if (w.biome==TROPICAL) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
    for (int i=0;i<140;i++){
      int x = (i*83 + tick*3) % L.screenW;
      int y = (i*53 + tick*2) % L.screenH;
      int wx = camX0 + (int)((int64_t)x * viewW / std::max(1, L.screenW));
      int wy = camY0 + (int)((int64_t)y * viewH / std::max(1, L.simHpx));
      if (inBounds(wx,wy) && w.water[wy][wx] > 0.7f) {
        uint8_t a = (uint8_t)(12 + (int)(10 * std::sin((float)(tick*0.06f + i))));
        SDL_Rect rc{x,y,2,2};
        setColor(ren, 120,200,180, a);
        SDL_RenderFillRect(ren, &rc);
      }
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  }
  if (w.biome==ALIEN) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int x=0; x<L.screenW; x+=18){
      int a = 8 + (int)(8 * std::sin((float)(tick*0.05f + x*0.12f)));
      SDL_Rect rr{x,0,4,L.screenH};
      setColor(ren, 120,60,160, (uint8_t)clampi(a,6,18));
      SDL_RenderFillRect(ren, &rr);
    }
  }

  // CRT scanlines + vignette
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  if (!g_wantSynth) {
    for (int y=0; y<L.screenH; y+=2){
      SDL_Rect sl{0,y,L.screenW,1};
      setColor(ren, 0,0,0, 22);
      SDL_RenderFillRect(ren, &sl);
    }
    // phosphor tint mask
    for (int x=0; x<L.screenW; x+=3){
      SDL_Rect r1{x,0,1,L.screenH};
      SDL_Rect r2{x+1,0,1,L.screenH};
      SDL_Rect r3{x+2,0,1,L.screenH};
      setColor(ren, 255,80,80, 10);
      SDL_RenderFillRect(ren, &r1);
      setColor(ren, 80,255,120, 10);
      SDL_RenderFillRect(ren, &r2);
      setColor(ren, 80,120,255, 10);
      SDL_RenderFillRect(ren, &r3);
    }
  }
  SDL_Rect top{0,0,L.screenW,20};
  SDL_Rect bot{0,L.screenH-20,L.screenW,20};
  SDL_Rect left{0,0,20,L.screenH};
  SDL_Rect right{L.screenW-20,0,20,L.screenH};
  setColor(ren, 0,0,0, 28);
  SDL_RenderFillRect(ren, &top);
  SDL_RenderFillRect(ren, &bot);
  SDL_RenderFillRect(ren, &left);
  SDL_RenderFillRect(ren, &right);

  // on-screen legend
  if (!showMenu) {
    std::snprintf(buf, sizeof(buf), "M menu  B biome  R reseed  [ ] speed  WASD pan  Z/X zoom  G ghibli  H modhot  I inspect  RMB pin  F5 preset  F6 save  F7 midi  F9 audio  F11 fullscreen  %s", VERSION);
    drawString(ren, gcText, 12, 10, buf, 190,190,200, 230, 1);
  }

  if (!showMenu && g_showHotMods) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int panelW = 210;
    int panelH = 90;
    int px = L.screenW - panelW - 10;
    int py = 26;
    SDL_Rect panel{ px, py, panelW, panelH };
    setColor(ren, 0,0,0, 160);
    SDL_RenderFillRect(ren, &panel);
    int tx = px + 8;
    int ty = py + 6;
    char buf[128];
    drawString(ren, gcText, tx, ty, "HOT MODS", 210,210,220, 230, 1);
    ty += 12;
    for (int k=0; k<6; ++k){
      int mi = g_modHot[k];
      std::snprintf(buf, sizeof(buf), "%s %+.2f", g_modName[mi], g_modVal[mi]);
      drawString(ren, gcText, tx, ty + k*12, buf, 200,200,210, 230, 1);
    }
  }

  if (!showMenu && g_hoverInspect && (hoverAgent || hoverBig)) {
    bool pinnedView = g_inspectPinned;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int panelW = 410;
    int panelH = 164;
    int px = 10;
    int py = std::max(30, L.screenH - panelH - 12);
    SDL_Rect panel{ px, py, panelW, panelH };
    setColor(ren, 0,0,0, 182);
    SDL_RenderFillRect(ren, &panel);
    SDL_Rect border{ px, py, panelW, panelH };
    setColor(ren, 140,140,170, 160);
    SDL_RenderDrawRect(ren, &border);

    int tx = px + 8;
    int ty = py + 6;
    if (hoverAgent) {
      const SpeciesDef& sd = g_species[hoverAgent->sp];
      const char* diet = sd.carnivore ? "carnivore" : (sd.herbivore ? "herbivore" : "neutral");
      const char* habitat = sd.aquatic ? "aquatic" : "land";
      std::snprintf(buf, sizeof(buf), "%sCREATURE #%d  %s (%c)  %s/%s", pinnedView?"PINNED ":"", hoverAgent->id, sd.name, sd.glyph, diet, habitat);
      drawString(ren, gcText, tx, ty, buf, 220,220,230, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "xy %d,%d  age %d/%d  panic %s", hoverAgent->x, hoverAgent->y, hoverAgent->age, hoverAgent->maxAge, hoverAgent->panic ? "YES" : "NO");
      drawString(ren, gcText, tx, ty, buf, 205,205,220, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "health %.2f  hunger %.2f  thirst %.2f", hoverAgent->health, hoverAgent->hunger, hoverAgent->thirst);
      drawString(ren, gcText, tx, ty, buf, 205,205,220, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "stress %.2f  fatigue %.2f", hoverAgent->stress, hoverAgent->fatigue);
      drawString(ren, gcText, tx, ty, buf, 205,205,220, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "emotion %.2f  bold %.2f  social %.2f  curious %.2f  aggro %.2f", hoverAgent->emotion, hoverAgent->bold, hoverAgent->social, hoverAgent->curious, hoverAgent->aggro);
      drawString(ren, gcText, tx, ty, buf, 190,210,220, 230, 1); ty += 12;
      drawString(ren, gcText, tx, ty, "MOD LINKS (world matrix):", 190,190,205, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "stress -> stress_mean[%d]=%+.2f  stress_flux[%d]=%+.2f", 5, g_modVal[5], 29, g_modVal[29]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "hunger -> hunger_mean[%d]=%+.2f  hunger_flux[%d]=%+.2f", 8, g_modVal[8], 30, g_modVal[30]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "thirst -> thirst_mean[%d]=%+.2f  thirst_flux[%d]=%+.2f", 9, g_modVal[9], 31, g_modVal[31]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1); ty += 12;
      if (sd.carnivore) std::snprintf(buf, sizeof(buf), "role -> pred_pressure[%d]=%+.2f  hunt_rate[%d]=%+.2f", 12, g_modVal[12], 59, g_modVal[59]);
      else std::snprintf(buf, sizeof(buf), "role -> forage_rate[%d]=%+.2f  grazing_impact[%d]=%+.2f", 60, g_modVal[60], 76, g_modVal[76]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1);
    } else if (hoverBig) {
      const char* habitat = hoverBig->aquatic ? "aquatic" : "land";
      std::snprintf(buf, sizeof(buf), "%sBIG CREATURE  %s (%c)  %s", pinnedView?"PINNED ":"", bigCreatureName(hoverBig->glyph), hoverBig->glyph, habitat);
      drawString(ren, gcText, tx, ty, buf, 220,220,230, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "anchor xy %d,%d  size %dx%d  cooldown %d", hoverBig->x, hoverBig->y, hoverBig->w, hoverBig->h, hoverBig->moveCooldown);
      drawString(ren, gcText, tx, ty, buf, 205,205,220, 230, 1); ty += 12;
      drawString(ren, gcText, tx, ty, "MOD LINKS (world matrix):", 190,190,205, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "water_flux[%d]=%+.2f  edge_activity[%d]=%+.2f", 28, g_modVal[28], 68, g_modVal[68]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1); ty += 12;
      std::snprintf(buf, sizeof(buf), "pred_prey_ratio[%d]=%+.2f  biodiversity[%d]=%+.2f", 51, g_modVal[51], 50, g_modVal[50]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1); ty += 12;
      if (hoverBig->aquatic) std::snprintf(buf, sizeof(buf), "aquatic_ratio[%d]=%+.2f  water_turbulence[%d]=%+.2f", 65, g_modVal[65], 53, g_modVal[53]);
      else std::snprintf(buf, sizeof(buf), "land_ratio[%d]=%+.2f  mean_altitude[%d]=%+.2f", 66, g_modVal[66], 67, g_modVal[67]);
      drawString(ren, gcText, tx, ty, buf, 190,190,205, 230, 1);
    }
  }

  if (showMenu) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int panelW = std::min(460, L.screenW/2);
    SDL_Rect panel{ 10, 10, panelW, L.screenH - 20 };
    setColor(ren, 0,0,0, 200);
    SDL_RenderFillRect(ren, &panel);
    SDL_Rect border{ 10, 10, panelW, L.screenH - 20 };
    setColor(ren, 120,120,140, 120);
    SDL_RenderDrawRect(ren, &border);
    int tx = 20;
    int ty = 18;
    std::snprintf(buf, sizeof(buf), "MENU  PAGE %d/8", menuPage+1);
    drawString(ren, gcText, tx, ty, buf, 220,220,230, 230, 1);
    ty += 12;
    drawString(ren, gcText, tx, ty, "UP/DOWN select  +/- edit  TAB page  F9 reset audio", 170,170,180, 230, 1);
    ty += 8;
    SDL_Rect sep{ tx, ty, panelW - 20, 1 };
    setColor(ren, 120,120,140, 120);
    SDL_RenderFillRect(ren, &sep);

    if (menuPage==0) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "SIM STATUS", 200,200,200, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "Biome: %s", g_biomes[w.biome].name);
      drawString(ren, gcText, tx, ty, buf, 220,220,220, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "Weather: %d", (int)w.weather.state);
      drawString(ren, gcText, tx, ty, buf, 220,220,220, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "Agents: %d", (int)w.agents.size());
      drawString(ren, gcText, tx, ty, buf, 220,220,220, 230, 1);
    } else if (menuPage==1) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "SIM ALEA", 220,220,220, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "rain  %.2f", g_alea.rainChance);
      drawString(ren, gcText, tx, ty, buf, 210,210,220, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "spawn %.2f", g_alea.spawnChance);
      drawString(ren, gcText, tx, ty, buf, 210,210,220, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "drift %.2f", g_alea.drift);
      drawString(ren, gcText, tx, ty, buf, 210,210,220, 230, 1);
      ty += 12;
      std::snprintf(buf, sizeof(buf), "chaos %.2f", g_alea.chaos);
      drawString(ren, gcText, tx, ty, buf, 210,210,220, 230, 1);
    } else if (menuPage==2) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "MODS (78)", 200,200,200, 230, 1);
      ty += 12;
      int start = g_g_modScroll;
      for (int i=0;i<10;++i){
        int mi = start + i; if (mi>=MOD_N) break;
        std::snprintf(buf, sizeof(buf), "%2d %-14s %+.2f", mi, g_modName[mi], g_modVal[mi]);
        drawString(ren, gcText, tx, ty + i*12, buf, 210,210,220, 230, 1);
      }
    } else if (menuPage==3) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "MODMAP (E enable)", 200,200,200, 230, 1);
      ty += 12;
      g_g_mmSel = clampi(g_g_mmSel, 0, MOD_SLOTS-1);
      g_g_mmField = clampi(g_g_mmField, 0, 3);
      for (int i=0;i<MOD_SLOTS;++i){
        const ModMap& mm = g_modMap[i];
        std::snprintf(buf, sizeof(buf), "%2d %c src:%2d dest:%-10s amt:%+.2f sm:%.2f",
                      i, mm.enabled?'*':' ', mm.src, modDestName(mm.dest), mm.amt, mm.smooth);
        uint8_t rr = (i==g_g_mmSel)?255:200;
        uint8_t gg = (i==g_g_mmSel)?255:200;
        drawString(ren, gcText, tx, ty + i*12, buf, rr,gg,220, 230, 1);
      }
    } else if (menuPage==4) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "VOICE PROGRAMS + SF2 LIST", 200,200,200, 230, 1);
      ty += 12;
      menuSel = clampi(menuSel, 0, 3);
      for (int i=0;i<4;++i){
        const char* pname = presetName(0, g_voiceProg[i]);
        if (pname && pname[0]) std::snprintf(buf, sizeof(buf), "V%d program %3d %s", i, g_voiceProg[i], pname);
        else std::snprintf(buf, sizeof(buf), "V%d program %3d", i, g_voiceProg[i]);
        uint8_t rr = (i==menuSel)?255:200;
        uint8_t gg = (i==menuSel)?255:200;
        drawString(ren, gcText, tx, ty + i*12, buf, rr,gg,220, 230, 1);
      }
      ty += 4*12 + 10;
      drawString(ren, gcText, tx, ty, "SF2 PRESETS", 180,180,190, 230, 1);
      ty += 12;
      {
        int lineH = 10;
        int startY = ty;
        int maxLines = std::max(1, (L.screenH - startY - 10) / lineH);
        int colW = 260;
        int maxCols = std::max(1, (L.screenW - tx) / colW);
        int total = (int)g_sf2Presets.size();
        int cols = (total + maxLines - 1) / maxLines;
        cols = std::max(1, std::min(cols, maxCols));
        int shown = std::min(total, maxLines * cols);
        for (int i=0;i<shown; ++i){
          int col = i / maxLines;
          int row = i % maxLines;
          std::snprintf(buf, sizeof(buf), "%s", g_sf2Presets[i].c_str());
          drawString(ren, gcText, tx + col*colW, startY + row*lineH, buf, 190,190,200, 230, 1);
        }
        if (shown < total) {
          std::snprintf(buf, sizeof(buf), "(list truncated: %d/%d)", shown, total);
          drawString(ren, gcText, tx, startY + maxLines*lineH + 2, buf, 180,170,170, 230, 1);
        }
      }
      if (g_sf2Presets.empty()) {
        drawString(ren, gcText, tx, ty, "(no presets loaded)", 170,170,180, 230, 1);
      }
    } else if (menuPage==5) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "MIXER/DRUMS", 200,200,200, 230, 1);
      ty += 12;
      menuSel = clampi(menuSel, 0, 7);
      for (int i=0;i<4;++i){
        std::snprintf(buf, sizeof(buf), "V%d vol %3d", i, g_voiceVol[i]);
        uint8_t rr = (i==menuSel)?255:200;
        uint8_t gg = (i==menuSel)?255:200;
        drawString(ren, gcText, tx, ty + i*12, buf, rr,gg,220, 230, 1);
      }
      std::snprintf(buf, sizeof(buf), "Drum1 vol %3d", g_drumVol);
      uint8_t rr = (menuSel==4)?255:200;
      uint8_t gg = (menuSel==4)?255:200;
      drawString(ren, gcText, tx, ty + 4*12, buf, rr,gg,220, 230, 1);
      std::snprintf(buf, sizeof(buf), "Drum2 vol %3d", g_drum2Vol);
      rr = (menuSel==5)?255:200;
      gg = (menuSel==5)?255:200;
      drawString(ren, gcText, tx, ty + 5*12, buf, rr,gg,220, 230, 1);
      std::snprintf(buf, sizeof(buf), "Drum3 vol %3d", g_drum3Vol);
      rr = (menuSel==6)?255:200;
      gg = (menuSel==6)?255:200;
      drawString(ren, gcText, tx, ty + 6*12, buf, rr,gg,220, 230, 1);
      {
        const char* dname = presetName(g_drumBank, g_drumProg);
        if (dname && dname[0]) std::snprintf(buf, sizeof(buf), "Drum prog %3d bank %3d %s", g_drumProg, g_drumBank, dname);
        else std::snprintf(buf, sizeof(buf), "Drum prog %3d bank %3d", g_drumProg, g_drumBank);
      }
      rr = (menuSel==7)?255:200;
      gg = (menuSel==7)?255:200;
      drawString(ren, gcText, tx, ty + 7*12, buf, rr,gg,220, 230, 1);
      std::snprintf(buf, sizeof(buf), "Master gain %.2f", g_masterGain);
      drawString(ren, gcText, tx, ty + 8*12, buf, 180,180,200, 230, 1);
    } else if (menuPage==6) {
      ty += 16;
      drawString(ren, gcText, tx, ty, "MUSIC KEY/SCALE", 200,200,200, 230, 1);
      ty += 12;
      menuSel = clampi(menuSel, 0, 2);
      std::snprintf(buf, sizeof(buf), "Mode  %s", g_musicKeyManual ? "MANUAL" : "AUTO");
      uint8_t rr = (menuSel==0)?255:200;
      uint8_t gg = (menuSel==0)?255:200;
      drawString(ren, gcText, tx, ty + 0*12, buf, rr,gg,220, 230, 1);
      std::snprintf(buf, sizeof(buf), "Root  %s", rootName(g_musicRootManual));
      rr = (menuSel==1)?255:200;
      gg = (menuSel==1)?255:200;
      drawString(ren, gcText, tx, ty + 1*12, buf, rr,gg,220, 230, 1);
      ScaleType st = (ScaleType)clampi(g_musicScaleManual, (int)SCALE_CHROMATIC, (int)SCALE_WHOLE);
      std::snprintf(buf, sizeof(buf), "Scale %s", scaleName(st));
      rr = (menuSel==2)?255:200;
      gg = (menuSel==2)?255:200;
      drawString(ren, gcText, tx, ty + 2*12, buf, rr,gg,220, 230, 1);
    } else if (menuPage==7) {
      ty += 16;
      std::snprintf(buf, sizeof(buf), "PALETTE EDIT (%s)  MODE %s", g_biomes[w.biome].name, g_paletteHSV ? "HSV" : "RGB");
      drawString(ren, gcText, tx, ty, buf, 200,200,200, 230, 1);
      ty += 12;
      drawString(ren, gcText, tx, ty, "UP/DOWN pick  LEFT/RIGHT channel  +/- edit  P mode  F8 save/load", 170,170,180, 230, 1);
      ty += 12;
      menuSel = clampi(menuSel, 0, 11);
      g_paletteChan = clampi(g_paletteChan, 0, 2);
      BiomeDef& bb = g_biomesEdit[w.biome];
      for (int i=0;i<9;++i){
        RGB& c = biomeColorRef(bb, i);
        uint8_t rr = (i==menuSel)?255:200;
        uint8_t gg = (i==menuSel)?255:200;
        if (g_paletteHSV) {
          float h,s,v; rgbToHsv(c, h, s, v);
          const char* tag = (g_paletteChan==0)?"H":(g_paletteChan==1)?"S":"V";
          std::snprintf(buf, sizeof(buf), "%-13s %3.0f %3.0f %3.0f  [%s]", biomeColorName(i), h, s*100.f, v*100.f, tag);
        } else {
          const char* tag = (g_paletteChan==0)?"R":(g_paletteChan==1)?"G":"B";
          std::snprintf(buf, sizeof(buf), "%-13s %3d %3d %3d  [%s]", biomeColorName(i), c.r, c.g, c.b, tag);
        }
        drawString(ren, gcText, tx, ty + i*12, buf, rr,gg,220, 230, 1);
      }
      int baseY = ty + 9*12 + 8;
      for (int gi=0; gi<3; ++gi){
        int idx = 9 + gi;
        uint8_t rr = (idx==menuSel)?255:200;
        uint8_t gg = (idx==menuSel)?255:200;
        if (gi==0) std::snprintf(buf, sizeof(buf), "grade contrast %.2f", g_gradeContrast);
        else if (gi==1) std::snprintf(buf, sizeof(buf), "grade saturation %.2f", g_gradeSat);
        else std::snprintf(buf, sizeof(buf), "grade lift %.2f", g_gradeLift);
        drawString(ren, gcText, tx, baseY + gi*12, buf, rr,gg,220, 230, 1);
      }
    }
  }

  if (g_showAudioDebug) {
    // audio debug disabled
  }

  SDL_RenderPresent(ren);
}

// ===== Step =====
static void stepPartial(World& w, Rng& r, int tick, bool doWeather, bool doWater, bool doTerrain, bool doAgents, bool doClouds){
  w.events.clear();
  if (doWeather) {
    updateWeather(w, r);
    updateWind(w, r);
  }
  if (g_chaosStormTimer > 0) {
    g_chaosStormTimer--;
    w.weather.state = STORM;
    w.weather.rainStrength = std::max(w.weather.rainStrength, 0.9f);
  }
  if (g_shapeShiftTimer > 0) g_shapeShiftTimer--;
  if (g_panicFloodTimer > 0) g_panicFloodTimer--;
  if (w.biomeMorphActive) {
    w.biomeMorphT += 0.03f;
    if (w.biomeMorphT >= 1.0f) {
      w.biome = w.targetBiome;
      w.biomeMorphActive = false;
      w.biomeMorphT = 0.f;
    }
  }
  if (doWater) stepWater(w, r);
  if (doTerrain) {
    stepTerrain(w, r, seasonAt(tick));
    stepWaterPlants(w, r);
    stepBiomeSpecials(w, r);
    if (w.weather.state==STORM && r.oneIn(60)) {
      // lightning strikes: ignite vegetation
      for(int tries=0; tries<40; ++tries){
        int x=r.i(0,W-1), y=r.i(0,H-1);
        if (isVeg(w.terrain[y][x])) { w.terrain[y][x]='*'; break; }
      }
      Event ev; ev.type=Event::EV_LIGHTNING; ev.mag=1.f; w.events.push_back(ev);
    }
    stepFire(w, r);
  }
  if (doWater || doTerrain) stepNutrientCycle(w);
  if (doWater || doTerrain) stepBiomeEcoEvents(w, r, tick);
  if (doAgents) {
    stepBigCreatures(w, r);
    stepAgents(w, r, tick);
  }
  if (doClouds) updateClouds(w, r);
}

int main(int argc, char** argv){
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n"; return 1;
  }

  bool startFullscreen = true;
  Biome startBiome = MEADOW;
  g_wantSynth = false;
  g_sf2Path.clear();
  bool threadsExplicit = false;

  for(int i=1;i<argc;++i){
    if (std::strcmp(argv[i], "--windowed")==0) startFullscreen=false;
    else if (std::strcmp(argv[i], "--fullscreen")==0) startFullscreen=true;
    else if (std::strcmp(argv[i], "--biome")==0 && i+1<argc) {
      std::string s=argv[++i];
      if      (s=="meadow")   startBiome=MEADOW;
      else if (s=="wetland")  startBiome=WETLAND;
      else if (s=="alpine")   startBiome=ALPINE;
      else if (s=="desert")   startBiome=DESERT;
      else if (s=="tropical") startBiome=TROPICAL;
      else if (s=="taiga")    startBiome=TAIGA;
      else if (s=="alien")    startBiome=ALIEN;
      else std::fprintf(stderr, "terrarium: unknown biome '%s' — valid: meadow wetland alpine desert tropical taiga alien\n", s.c_str());
    }
    else if (std::strcmp(argv[i], "--threads")==0 && i+1<argc) {
      threadsExplicit = true;
      int n = std::atoi(argv[++i]);
      if (n <= 0) {
        unsigned hc = std::thread::hardware_concurrency();
        g_threads = hc ? (int)hc : 2;
      } else {
        g_threads = n;
      }
    }
    else if (std::strcmp(argv[i], "--gain")==0 && i+1<argc) {
      g_masterGain = std::clamp((float)std::atof(argv[++i]), 0.05f, 2.5f);
    }
    else if (std::strcmp(argv[i], "--audio-driver")==0 && i+1<argc) {
      g_audioDriver = argv[++i];
    }
    else if (std::strcmp(argv[i], "--audio-device")==0 && i+1<argc) {
      g_audioDevice = argv[++i];
    }
    else if (std::strcmp(argv[i], "--synth")==0) g_wantSynth=true;
    else if (std::strcmp(argv[i], "--sf2")==0 && i+1<argc) { g_sf2Path=argv[++i]; g_wantSynth=true; }
  }
  if (!threadsExplicit) {
    unsigned hc = std::thread::hardware_concurrency();
    int autoThreads = hc ? std::max(2, (int)hc - 1) : 2;
    // When the synth is active, run the simulation single-threaded.
    // Multiple sim threads (water + agents) compete with the FluidSynth audio thread
    // under SCHED_OTHER and cause buffer underruns. 0.50.6 was single-threaded and
    // had stable audio — match that. Use --threads N to override explicitly.
    if (g_wantSynth) autoThreads = 1;
    g_threads = autoThreads;
  }
  g_threads = clampi(g_threads, 1, MAX_THREADS);
  initBiomeEdits();
  g_userMixerTouched = false;
  g_drumVol = 0;
  g_drum2Vol = 0;
  g_drum3Vol = 0;

  Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
  if (startFullscreen) wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  std::string winTitle = std::string("Terrarium Remix ") + VERSION;
  SDL_Window* win = SDL_CreateWindow(winTitle.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, wflags);
  if (!win) { std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n"; SDL_Quit(); return 1; }

  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) { std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n"; SDL_DestroyWindow(win); SDL_Quit(); return 1; }

  uint32_t seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
  Rng r(seed);

  World world; seedWorld(world, r, startBiome);
  GlyphCache gcWorld;
  GlyphCache gcText; gcText.textMode = true;
  Layout layout = computeLayout(ren);

  g_pendingOffs.reserve(64); // avoid reallocation during playback
  SynthOut synth; MidiOut midi;
  if (g_wantSynth && !g_sf2Path.empty()) {
    if (synth.open(g_sf2Path, g_masterGain, g_audioDriver, g_audioDevice)) {
      synth.listPresets(g_sf2Presets);
      for (int i=0;i<4;++i) g_voiceProgDirty[i] = true;
      sendDrumProgram(synth, midi);
    }
  }
#ifdef _WIN32
  midi.open(0);
#endif

  bool running=true, paused=false, showMenu=false;
  int menuPage=0;
  int menuSel=0;
  int tps=DEFAULT_TPS; int tick=0;
  const int targetFrameMs = g_wantSynth ? 25 : 16; // lower render pressure when synth is active
  const int WATER_TICK=1, TERRAIN_TICK=2, AGENT_TICK=3, WEATHER_TICK=6, CLOUD_TICK=4;
  auto last = std::chrono::steady_clock::now();
  auto frameLast = last;
  g_camFX = (float)g_camX;
  g_camFY = (float)g_camY;
  g_zoomF = (float)g_zoom;

  while(running){
    auto frameStart = std::chrono::steady_clock::now();
    SDL_Event e; while(SDL_PollEvent(&e)){
      if (e.type==SDL_QUIT) running=false;
      if (e.type==SDL_KEYDOWN){
        switch(e.key.keysym.sym){
          case SDLK_ESCAPE: running=false; break;
          case SDLK_m: showMenu=!showMenu; break;
          case SDLK_TAB: if (showMenu) menuPage = (menuPage + 1) % 8; break;
          case SDLK_SPACE: paused=!paused; break;
          case SDLK_LEFTBRACKET: if (tps>1) tps--; break;
          case SDLK_RIGHTBRACKET: if (tps<30) tps++; break;
          case SDLK_w: if (!showMenu) { g_camY -= std::max(1, zoomViewH()/10); clampCameraToZoom(); } break;
          case SDLK_s: if (!showMenu) { g_camY += std::max(1, zoomViewH()/10); clampCameraToZoom(); } break;
          case SDLK_a: if (!showMenu) { g_camX -= std::max(1, zoomViewW()/10); clampCameraToZoom(); } break;
          case SDLK_d: if (!showMenu) { g_camX += std::max(1, zoomViewW()/10); clampCameraToZoom(); } break;
          case SDLK_z:
            if (!showMenu) {
              g_zoom = clampi(g_zoom + 1, 1, 12);
              clampCameraToZoom();
            }
            break;
          case SDLK_x:
            if (!showMenu) {
              g_zoom = clampi(g_zoom - 1, 1, 12);
              clampCameraToZoom();
            }
            break;
          case SDLK_r: seedWorld(world, r, world.biome); tick=0; break;
          case SDLK_b: {
            Biome next = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
            seedWorld(world, r, next);
            tick = 0;
          } break;
          case SDLK_F2: {
            Biome next = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
            seedWorld(world, r, next);
            tick = 0;
          } break;
          case SDLK_UP: {
            if (showMenu && menuPage==2) { g_g_modScroll = std::max(0, g_g_modScroll-1); }
            else if (showMenu && menuPage==3) { g_g_mmSel = (g_g_mmSel + MOD_SLOTS - 1) % MOD_SLOTS; }
            else if (showMenu && menuPage==4) { menuSel = (menuSel + 3) % 4; }
            else if (showMenu && menuPage==5) { menuSel = (menuSel + 7) % 8; }
            else if (showMenu && menuPage==6) { menuSel = (menuSel + 2) % 3; }
            else if (showMenu && menuPage==7) { menuSel = (menuSel + 11) % 12; }
          } break;
          case SDLK_DOWN: {
            if (showMenu && menuPage==2) { g_g_modScroll = std::min(MOD_N-1, g_g_modScroll+1); }
            else if (showMenu && menuPage==3) { g_g_mmSel = (g_g_mmSel + 1) % MOD_SLOTS; }
            else if (showMenu && menuPage==4) { menuSel = (menuSel + 1) % 4; }
            else if (showMenu && menuPage==5) { menuSel = (menuSel + 1) % 8; }
            else if (showMenu && menuPage==6) { menuSel = (menuSel + 1) % 3; }
            else if (showMenu && menuPage==7) { menuSel = (menuSel + 1) % 12; }
          } break;
          case SDLK_LEFT:
            if (showMenu && menuPage==3) g_g_mmField = std::max(0, g_g_mmField-1);
            else if (showMenu && menuPage==5 && menuSel==7) { g_drumBank = clampi(g_drumBank - 1, 0, 127); sendDrumProgram(synth, midi); }
            else if (showMenu && menuPage==7) g_paletteChan = std::max(0, g_paletteChan-1);
            break;
          case SDLK_RIGHT:
            if (showMenu && menuPage==3) g_g_mmField = std::min(3, g_g_mmField+1);
            else if (showMenu && menuPage==5 && menuSel==7) { g_drumBank = clampi(g_drumBank + 1, 0, 127); sendDrumProgram(synth, midi); }
            else if (showMenu && menuPage==7) g_paletteChan = std::min(2, g_paletteChan+1);
            break;
          case SDLK_p:
            if (showMenu && menuPage==7) g_paletteHSV = !g_paletteHSV;
            break;
          case SDLK_EQUALS:
          case SDLK_KP_PLUS: {
            if (showMenu && menuPage==1) g_alea.spawnChance = std::min(2.f, g_alea.spawnChance + 0.05f);
            else if (showMenu && menuPage==3) {
              ModMap& mm = g_modMap[g_g_mmSel];
              if (g_g_mmField==0) mm.src = std::min(MOD_N-1, mm.src+1);
              else if (g_g_mmField==1) mm.dest = std::min((int)DEST_TEMPO, mm.dest+1);
              else if (g_g_mmField==2) mm.amt = std::min(2.f, mm.amt+0.05f);
              else if (g_g_mmField==3) mm.smooth = std::min(0.98f, mm.smooth+0.02f);
            }
            else if (showMenu && menuPage==4) {
              g_voiceProg[menuSel] = clampi(g_voiceProg[menuSel] + 1, 0, 127);
              g_voiceProgDirty[menuSel] = true;
              applyVoiceProgramNow(synth, midi, menuSel, tick);
            }
            else if (showMenu && menuPage==5) {
              if (menuSel>=0 && menuSel<=3) g_voiceVol[menuSel] = clampi(g_voiceVol[menuSel] + 5, 0, 127);
              else if (menuSel==4) { g_drumVol = clampi(g_drumVol + 5, 0, 127); g_userMixerTouched = true; }
              else if (menuSel==5) { g_drum2Vol = clampi(g_drum2Vol + 5, 0, 127); g_userMixerTouched = true; }
              else if (menuSel==6) { g_drum3Vol = clampi(g_drum3Vol + 5, 0, 127); g_userMixerTouched = true; }
              else if (menuSel==7) { g_drumProg = clampi(g_drumProg + 1, 0, 127); sendDrumProgram(synth, midi); }
            }
            else if (showMenu && menuPage==6) {
              if (menuSel==0) g_musicKeyManual = true;
              else if (menuSel==1) g_musicRootManual = (g_musicRootManual + 1) % 12;
              else if (menuSel==2) g_musicScaleManual = clampi(g_musicScaleManual + 1, (int)SCALE_CHROMATIC, (int)SCALE_WHOLE);
            }
            else if (showMenu && menuPage==7) {
              if (menuSel < 9) {
                BiomeDef& bb = g_biomesEdit[world.biome];
                RGB& c = biomeColorRef(bb, menuSel);
                if (g_paletteHSV) {
                  float h,s,v; rgbToHsv(c, h, s, v);
                  if (g_paletteChan==0) h = std::fmod(h + 5.f, 360.f);
                  else if (g_paletteChan==1) s = std::clamp(s + 0.02f, 0.f, 1.f);
                  else v = std::clamp(v + 0.02f, 0.f, 1.f);
                  c = hsvToRgb(h, s, v);
                } else {
                  int delta = 5;
                  if (g_paletteChan==0) c.r = (uint8_t)clampi((int)c.r + delta, 0, 255);
                  else if (g_paletteChan==1) c.g = (uint8_t)clampi((int)c.g + delta, 0, 255);
                  else c.b = (uint8_t)clampi((int)c.b + delta, 0, 255);
                }
              } else {
                if (menuSel==9) g_gradeContrast = std::clamp(g_gradeContrast + 0.05f, 0.5f, 2.0f);
                else if (menuSel==10) g_gradeSat = std::clamp(g_gradeSat + 0.05f, 0.3f, 2.0f);
                else if (menuSel==11) g_gradeLift = std::clamp(g_gradeLift + 0.01f, -0.3f, 0.3f);
              }
            }
          } break;
          case SDLK_MINUS:
          case SDLK_KP_MINUS: {
            if (showMenu && menuPage==1) g_alea.spawnChance = std::max(0.f, g_alea.spawnChance - 0.05f);
            else if (showMenu && menuPage==3) {
              ModMap& mm = g_modMap[g_g_mmSel];
              if (g_g_mmField==0) mm.src = std::max(0, mm.src-1);
              else if (g_g_mmField==1) mm.dest = std::max((int)DEST_NONE, mm.dest-1);
              else if (g_g_mmField==2) mm.amt = std::max(-2.f, mm.amt-0.05f);
              else if (g_g_mmField==3) mm.smooth = std::max(0.f, mm.smooth-0.02f);
            }
            else if (showMenu && menuPage==4) {
              g_voiceProg[menuSel] = clampi(g_voiceProg[menuSel] - 1, 0, 127);
              g_voiceProgDirty[menuSel] = true;
              applyVoiceProgramNow(synth, midi, menuSel, tick);
            }
            else if (showMenu && menuPage==5) {
              if (menuSel>=0 && menuSel<=3) g_voiceVol[menuSel] = clampi(g_voiceVol[menuSel] - 5, 0, 127);
              else if (menuSel==4) { g_drumVol = clampi(g_drumVol - 5, 0, 127); g_userMixerTouched = true; }
              else if (menuSel==5) { g_drum2Vol = clampi(g_drum2Vol - 5, 0, 127); g_userMixerTouched = true; }
              else if (menuSel==6) { g_drum3Vol = clampi(g_drum3Vol - 5, 0, 127); g_userMixerTouched = true; }
              else if (menuSel==7) { g_drumProg = clampi(g_drumProg - 1, 0, 127); sendDrumProgram(synth, midi); }
            }
            else if (showMenu && menuPage==6) {
              if (menuSel==0) g_musicKeyManual = false;
              else if (menuSel==1) g_musicRootManual = (g_musicRootManual + 11) % 12;
              else if (menuSel==2) g_musicScaleManual = clampi(g_musicScaleManual - 1, (int)SCALE_CHROMATIC, (int)SCALE_WHOLE);
            }
            else if (showMenu && menuPage==7) {
              if (menuSel < 9) {
                BiomeDef& bb = g_biomesEdit[world.biome];
                RGB& c = biomeColorRef(bb, menuSel);
                if (g_paletteHSV) {
                  float h,s,v; rgbToHsv(c, h, s, v);
                  if (g_paletteChan==0) h = std::fmod(h - 5.f + 360.f, 360.f);
                  else if (g_paletteChan==1) s = std::clamp(s - 0.02f, 0.f, 1.f);
                  else v = std::clamp(v - 0.02f, 0.f, 1.f);
                  c = hsvToRgb(h, s, v);
                } else {
                  int delta = 5;
                  if (g_paletteChan==0) c.r = (uint8_t)clampi((int)c.r - delta, 0, 255);
                  else if (g_paletteChan==1) c.g = (uint8_t)clampi((int)c.g - delta, 0, 255);
                  else c.b = (uint8_t)clampi((int)c.b - delta, 0, 255);
                }
              } else {
                if (menuSel==9) g_gradeContrast = std::clamp(g_gradeContrast - 0.05f, 0.5f, 2.0f);
                else if (menuSel==10) g_gradeSat = std::clamp(g_gradeSat - 0.05f, 0.3f, 2.0f);
                else if (menuSel==11) g_gradeLift = std::clamp(g_gradeLift - 0.01f, -0.3f, 0.3f);
              }
            }
          } break;
          case SDLK_F8: {
            if (showMenu && menuPage==7) {
              bool shift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
              std::string p = "/home/user/terrarium/palettes/palette_current.txt";
              if (shift) loadPaletteFile(p);
              else savePaletteFile(p);
            }
          } break;
          case SDLK_e: if (showMenu && menuPage==3) g_modMap[g_g_mmSel].enabled = !g_modMap[g_g_mmSel].enabled; break;
          case SDLK_i: {
            g_hoverInspect = !g_hoverInspect;
          } break;
          case SDLK_F11: {
            Uint32 flags = SDL_GetWindowFlags(win);
            bool fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            layout = computeLayout(ren);
          } break;
          case SDLK_F9: {
            resetAudio(synth, midi);
          } break;
          case SDLK_F5: {
            applyBiomeModPreset(world.biome, true);
          } break;
          case SDLK_F6: {
            saveSnapshot(world, tps);
          } break;
          case SDLK_F7: {
            exportMidiStub();
          } break;
          case SDLK_g: {
            g_ghibliPalette = !g_ghibliPalette;
          } break;
          case SDLK_h: {
            g_showHotMods = !g_showHotMods;
          } break;
        }
      }
      if (e.type==SDL_WINDOWEVENT && (e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event==SDL_WINDOWEVENT_RESIZED)) {
        layout = computeLayout(ren);
      }
      if (e.type==SDL_MOUSEBUTTONDOWN) {
        int mx = e.button.x;
        int my = e.button.y;
        if (mx < 0 || mx >= layout.screenW || my < 0 || my >= layout.simHpx) continue;
        float camRX = g_camFX;
        float camRY = g_camFY;
        float zoomR = std::clamp(g_zoomF, 1.0f, 12.0f);
        clampCameraFToZoom(camRX, camRY, zoomR);
        int viewW = zoomViewWFor(zoomR);
        int viewH = zoomViewHFor(zoomR);
        int camX0 = clampi((int)std::lround(camRX), 0, std::max(0, W - viewW));
        int camY0 = clampi((int)std::lround(camRY), 0, std::max(0, H - viewH));
        int sx = (int)((int64_t)mx * viewW / std::max(1, layout.screenW));
        int sy = (int)((int64_t)my * viewH / std::max(1, layout.simHpx));
        int wx = clampi(camX0 + sx, 0, W-1);
        int wy = clampi(camY0 + sy, 0, H-1);
        if (e.button.button==SDL_BUTTON_RIGHT) {
          const Agent* a = hoveredAgentAt(world, wx, wy);
          if (a) {
            if (g_inspectPinned && !g_inspectPinnedIsBig && g_inspectPinnedAgentId==a->id) {
              clearInspectPin();
            } else {
              g_inspectPinned = true;
              g_inspectPinnedIsBig = false;
              g_inspectPinnedAgentId = a->id;
              g_inspectPinnedBigX = a->x;
              g_inspectPinnedBigY = a->y;
              g_inspectPinnedBigGlyph = ' ';
            }
            g_hoverInspect = true;
          } else {
            const BigCreature* b = hoveredBigAt(world, wx, wy);
            if (b) {
              if (g_inspectPinned && g_inspectPinnedIsBig &&
                  g_inspectPinnedBigGlyph==b->glyph &&
                  g_inspectPinnedBigX==b->x &&
                  g_inspectPinnedBigY==b->y) {
                clearInspectPin();
              } else {
                g_inspectPinned = true;
                g_inspectPinnedIsBig = true;
                g_inspectPinnedAgentId = -1;
                g_inspectPinnedBigX = b->x;
                g_inspectPinnedBigY = b->y;
                g_inspectPinnedBigGlyph = b->glyph;
              }
              g_hoverInspect = true;
            } else {
              clearInspectPin();
            }
          }
        } else if (e.button.button==SDL_BUTTON_LEFT) {
          Ripple rp; rp.cx=wx; rp.cy=wy;
          rp.amp = 3.0f + 5.0f * r.u01();
          rp.speed = 16.f + 18.f * r.u01();
          rp.width = 2.0f + 2.5f * r.u01();
          rp.chaos = 0.5f + 0.8f * r.u01();
          rp.mode = r.i(0,9);
          rp.seed = r.u32();
          g_ripples.push_back(rp);
          if ((int)g_ripples.size() > 16) g_ripples.erase(g_ripples.begin());
          triggerChaos(world, r, wx, wy);
        }
      }
    }

    auto now = std::chrono::steady_clock::now();
    auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    auto frameDtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - frameLast).count();
    frameLast = now;
    float frameDt = std::clamp((float)frameDtMs / 1000.0f, 0.0f, 0.050f);
    int msPerTick = 1000 / std::max(1, tps);
    if (!paused && dtMs >= msPerTick) {
      last = now; tick++;
      bool doWeather = (tick % WEATHER_TICK)==0;
      bool doWater = (tick % WATER_TICK)==0;
      bool doTerrain = (tick % TERRAIN_TICK)==0;
      bool doAgents = (tick % AGENT_TICK)==0;
      bool doClouds = (tick % CLOUD_TICK)==0;
      stepPartial(world, r, tick, doWeather, doWater, doTerrain, doAgents, doClouds);
      updateModPool(world, tick);
      applyModMatrix();
    applyAutomation(synth, midi, tick);
    synthTickMusic(world, synth, midi, tick);

    // Audio watchdog: periodically verify the synth is still alive and re-init
    // if PipeWire/PA killed the stream after underruns. SDL2 driver handles this
    // internally, but keep this as a belt-and-suspenders fallback.
    // Fires every ~5 minutes (1800 ticks at 6 TPS). SF2 reload takes ~1-2s.
    if (g_wantSynth && synth.enabled && tick > 0 && (tick % 1800) == 0) {
      resetAudio(synth, midi);
    }
    }

    // Cinematic camera smoothing + music-reactive zoom breathing.
    clampCameraToZoom();
    float pulse = std::clamp((g_modVal[15] + 1.0f) * 0.5f, 0.0f, 1.0f);
    float storm = std::clamp((g_modVal[19] + 1.0f) * 0.5f, 0.0f, 1.0f);
    float breath = 1.0f + 0.04f * pulse + 0.02f * storm * std::sin((float)tick * 0.21f);
    float zTarget = std::clamp((float)g_zoom * breath, 1.0f, 12.0f);
    float camLerp = std::clamp(frameDt * 8.0f, 0.0f, 1.0f);
    float zoomLerp = std::clamp(frameDt * 5.0f, 0.0f, 1.0f);
    g_camFX += ((float)g_camX - g_camFX) * camLerp;
    g_camFY += ((float)g_camY - g_camFY) * camLerp;
    g_zoomF += (zTarget - g_zoomF) * zoomLerp;
    clampCameraFToZoom(g_camFX, g_camFY, g_zoomF);

    updateRipples(frameDt);
    updateWhirlpools(frameDt);
    render(ren, layout, world, gcWorld, gcText, tick, showMenu, menuPage, menuSel);
    // Fixed 6ms yield every frame — gives FluidSynth's audio thread regular CPU time.
    // Dynamic targeting (old approach) could starve audio for 20ms+ stretches.
    // 6ms means 3+ yields per 1024-sample buffer period at 48kHz (~21ms).
    SDL_Delay(6);
  }

  synth.close();
  midi.close();
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
