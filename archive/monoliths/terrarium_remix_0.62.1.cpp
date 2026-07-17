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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <utility>
#include <cmath>

// ===== Config =====
static constexpr int W = 200;
static constexpr int H = 112;
static constexpr int DEFAULT_TPS = 6;
static constexpr int MAX_AGENTS = 100;
static constexpr int START_AGENTS = 6;

static constexpr int SEASON_TICKS = 1200;
static constexpr int DAY_TICKS = 1000;

// Fluid sim
static constexpr float MAX_WATER = 6.0f;      // max water height per cell
static constexpr float FLOW_RATE = 0.25f;     // flow speed
static constexpr float EVAP_RATE = 0.0006f;   // evaporation per tick
static constexpr float RAIN_RATE = 0.015f;    // rain intensity

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

// ===== Shared sim controls =====
static int g_zoom = 1;
static int g_camX = 0;
static int g_camY = 0;

struct Ripple {
  int cx=0, cy=0;
  float t=0.f;
  float amp=3.f;
  float speed=18.f;
  float width=2.5f;
  float chaos=1.f;
};
static std::vector<Ripple> g_ripples;

struct AleaWeights {
  float rainChance=1.f;
  float spawnChance=1.f;
  float mutationRate=1.f;
  float drift=1.f;
  float chaos=1.f;
};
static AleaWeights g_alea;

// ===== Mod matrix =====
static constexpr int MOD_N = 70;
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
  "aquatic_ratio", "land_ratio", "mean_altitude", "edge_activity", "calmness"
};
static float g_modVal[MOD_N] = {0.f};
static int g_voiceProg[4] = {0, 48, 32, 89};
static bool g_voiceProgDirty[4] = {true, true, true, true};
static int g_voiceVol[4] = {100, 100, 100, 100};
static int g_drumProg = 0;
static int g_drumBank = 128;
static int g_drumVol = 0;
static int g_drum2Vol = 0;
static int g_drum3Vol = 0;
static float g_masterGain = 0.7f;
static float g_tempoMult = 1.0f;

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

enum Biome { MEADOW=0, WETLAND=1, ALPINE=2, DESERT=3, TROPICAL=4, TAIGA=5, ALIEN=6 };
static constexpr int BIOME_COUNT = 7;

struct RGB { uint8_t r,g,b; };
static inline char waterGlyph(float w);
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
  float speed;
  float hungerRate;
  float thirstRate;
  float reproduce;
};

enum SpeciesId : uint8_t {
  SP_RABBIT=0, SP_DEER, SP_GOAT,
  SP_FISH, SP_CRAB, SP_FROG,
  SP_WOLF, SP_BEAR, SP_EEL,
  SP_BIRD,
  SP_ALIEN1, SP_ALIEN2,
  SP_COUNT
};

static const SpeciesDef g_species[SP_COUNT] = {
  {"RABBIT", 'r', false, true,  false, false, 1.2f, 0.015f, 0.020f, 0.010f},
  {"DEER",   'd', false, true,  false, false, 1.0f, 0.012f, 0.018f, 0.008f},
  {"GOAT",   'g', false, true,  false, false, 1.0f, 0.012f, 0.018f, 0.007f},
  {"FISH",   'f', true,  true,  false, true,  1.1f, 0.010f, 0.010f, 0.010f},
  {"CRAB",   'c', true,  true,  false, false, 0.8f, 0.010f, 0.010f, 0.006f},
  {"FROG",   'p', true,  true,  false, false, 0.9f, 0.010f, 0.012f, 0.006f},
  {"WOLF",   'w', false, false, true,  false, 1.4f, 0.020f, 0.020f, 0.004f},
  {"BEAR",   'b', false, false, true,  false, 0.9f, 0.018f, 0.018f, 0.003f},
  {"EEL",    'e', true,  false, true,  false, 1.2f, 0.020f, 0.015f, 0.004f},
  {"BIRD",   'v', false, true,  false, false, 1.6f, 0.010f, 0.020f, 0.006f},
  {"ALIEN1", 'A', false, false, true,  false, 1.2f, 0.015f, 0.015f, 0.006f},
  {"ALIEN2", 'Z', true,  true,  false, true,  1.0f, 0.010f, 0.010f, 0.006f},
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
static const BigDef g_big_tropical[]= { {'H',2,2,true},  {'T',2,2,false} };
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

static const BiomeDef g_biomes[BIOME_COUNT] = {
  {"MEADOW", {18,60,140},{40,120,200},{220,240,255},{90,70,50},{70,160,100},{80,150,100},{200,120,160},{140,140,160},{10,10,16},
   {SP_RABBIT,SP_DEER},{SP_WOLF},{SP_FISH}},
  {"WETLAND",{20,70,130},{40,140,210},{230,245,255},{80,70,60},{50,150,120},{70,130,110},{150,200,170},{120,120,140},{10,10,16},
   {SP_FROG,SP_CRAB},{SP_EEL},{SP_FISH,SP_CRAB,SP_FROG,SP_EEL}},
  {"ALPINE", {15,50,120},{35,100,180},{220,240,255},{80,75,70},{70,140,120},{90,140,130},{190,180,200},{160,170,190},{10,10,16},
   {SP_GOAT},{SP_BEAR},{SP_FISH}},
  {"DESERT", {20,70,120},{60,120,170},{230,240,250},{140,110,70},{150,160,90},{120,140,90},{210,170,120},{170,150,120},{10,10,16},
   {SP_RABBIT},{SP_WOLF},{SP_FISH}},
  {"TROPICAL",{20,70,150},{50,150,220},{240,250,255},{80,60,40},{60,170,120},{70,160,130},{220,120,140},{120,120,140},{10,10,16},
   {SP_DEER,SP_RABBIT},{SP_WOLF},{SP_FISH,SP_CRAB}},
  {"TAIGA",  {20,60,130},{40,120,190},{230,245,255},{70,60,60},{60,140,90},{60,130,100},{180,140,160},{130,130,150},{10,10,16},
   {SP_DEER},{SP_WOLF,SP_BEAR},{SP_FISH}},
  {"ALIEN",  {50,20,80},{90,40,130},{240,220,255},{60,20,60},{80,40,120},{110,60,150},{200,80,200},{140,80,160},{8,6,12},
   {SP_ALIEN1},{SP_ALIEN1},{SP_ALIEN2}},
};

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
  std::vector<std::vector<uint8_t>> height; // 0..255
  std::vector<std::vector<uint8_t>> moist;  // 0..255
  Weather weather;
  Wind wind;
  std::vector<Agent> agents;
  std::vector<BigCreature> bigs;
  std::vector<Cloud> clouds;
  std::vector<Event> events;
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
  bool open(const std::string& sf2, float g){
    gain=g;
    settings = new_fluid_settings();
    if(!settings) return false;
    fluid_settings_setnum(settings, "synth.gain", (double)gain);
    fluid_settings_setnum(settings, "synth.sample-rate", 48000.0);
    fluid_settings_setint(settings, "synth.reverb.active", 1);
    fluid_settings_setnum(settings, "synth.reverb.room-size", 0.85);
    fluid_settings_setnum(settings, "synth.reverb.damp", 0.25);
    fluid_settings_setnum(settings, "synth.reverb.width", 100.0);
    fluid_settings_setnum(settings, "synth.reverb.level", 0.6);
    fluid_settings_setint(settings, "synth.chorus.active", 1);
    fluid_settings_setint(settings, "synth.chorus.nr", 4);
    fluid_settings_setnum(settings, "synth.chorus.level", 1.0);
    fluid_settings_setnum(settings, "synth.chorus.speed", 0.2);
    fluid_settings_setnum(settings, "synth.chorus.depth", 8.0);
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
#else
  bool open(const std::string&, float){ enabled=false; return false; }
  void close(){}
  void noteOn(int,int,int){}
  void noteOff(int,int,int=0){}
  void cc(int,int,int){}
  void programChange(int,int){}
  void bankSelect(int,int){}
  void setGain(float){}
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
static inline bool isVeg(char c){ return (c==','||c=='"'||c==';'||c=='#'||c=='m'||c=='f'||c=='+'||c=='&'||c=='!'||c=='t'||c=='l'||c=='n'||c=='q'||isTree(c)); }
static inline bool isEdiblePlant(char c){ return (c==','||c=='"'||c==';'||c=='#'||c=='f'||c=='+'||c=='&'||c=='!'); }
static inline bool isFlower(char c){ return (c=='f'||c=='+'||c=='&'||c=='!'); }

static inline void updateRipples(float dt) {
  for (auto &r : g_ripples) r.t += dt;
  g_ripples.erase(std::remove_if(g_ripples.begin(), g_ripples.end(),
    [](const Ripple& r){ return r.t > 3.0f; }), g_ripples.end());
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
  static constexpr int MAX_BIGS = 6;
  if ((int)w.bigs.size() < MAX_BIGS && r.oneIn(500)) {
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
    stampBig(w, b, ' ');
    b.x = nx; b.y = ny;
    stampBig(w, b, b.glyph);
  }
}

static inline char renderCharAtBase(const World& w, int x, int y, int tick){
  (void)tick;
  if (w.entities[y][x] != ' ') return w.entities[y][x];
  if (w.water[y][x] > 0.2f && w.terrain[y][x] == 'm') return 'm';
  if (w.water[y][x] > 0.2f) {
    bool shore=false;
    for(int dy=-1;dy<=1 && !shore;++dy) for(int dx=-1;dx<=1 && !shore;++dx){
      if(!dx && !dy) continue;
      int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
      if (w.water[ny][nx] <= 0.2f) shore=true;
    }
    if (shore || (w.wind.strength>=2 && ((hash3(x,y,tick/5) % (uint32_t)(20 - 3*w.wind.strength))==0u))) return '=';
    return waterGlyph(w.water[y][x]);
  }
  return w.terrain[y][x];
}

static inline char renderCharAt(const World& w, int x, int y, int tick){
  // Agents are not displaced by ripples.
  for (const auto& a : w.agents){
    if (a.x==x && a.y==y) return g_species[a.sp].glyph;
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
      dx += (int)std::lround(rx * inv * s);
      dy += (int)std::lround(ry * inv * s);
    }
  }
  int sx = clampi(x+dx,0,W-1);
  int sy = clampi(y+dy,0,H-1);
  return renderCharAtBase(w, sx, sy, tick);
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
  w.overlay.assign(H, std::string(W, ' '));
  w.moist.assign(H, std::vector<uint8_t>(W, 80));
  w.agents.clear();
  w.bigs.clear();
  w.events.clear();
  genHeight(w, r);

  // base terrain
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      uint8_t alt = w.height[y][x];
      if (biome==DESERT) w.terrain[y][x]='s';
      if (alt > 240) w.terrain[y][x]='^';
    }
  }

  // ponds
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
      if (g_species[sp].aquatic && w.water[y][x] <= 0) continue;
      if (!g_species[sp].aquatic && w.water[y][x] > 0) continue;
      Agent a; a.id = (int)w.agents.size()+1; a.x=x; a.y=y; a.sp=sp;
      a.hunger=r.u01()*0.2f; a.thirst=r.u01()*0.2f; a.health=1.0f;
      a.fatigue=r.u01()*0.2f; a.stress=r.u01()*0.2f; a.emotion=0.6f;
      a.bold=0.3f+0.7f*r.u01();
      a.social=0.3f+0.7f*r.u01();
      a.curious=0.3f+0.7f*r.u01();
      a.aggro=0.2f+0.8f*r.u01();
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
  // humidity drift
  humid = clamp01(humid + (r.u01()-0.5f)*0.02f);
  press = clamp01(press + (r.u01()-0.5f)*0.01f);

  if (humid > 0.65f && press < 0.45f) w.weather.state = (r.oneIn(4)?STORM:RAIN);
  else if (humid > 0.5f) w.weather.state = OVERCAST;
  else w.weather.state = CLEAR;

  w.weather.humidity = humid;
  w.weather.pressure = press;
  w.weather.rainStrength = (w.weather.state==RAIN || w.weather.state==STORM) ? (0.5f + 0.5f*humid) : 0.f;
  w.weather.cloudOpacity = std::clamp(0.3f + humid*0.8f, 0.2f, 1.0f);

  if (w.weather.state==STORM && r.oneIn(120)) {
    Event ev; ev.type=Event::EV_LIGHTNING; ev.mag=1.0f; w.events.push_back(ev);
  }
}

static void updateWind(World& w, Rng& r){
  if (r.oneIn(50)) {
    int t = r.i(0,7);
    w.wind.dx = (t%3)-1;
    w.wind.dy = ((t/3)%3)-1;
    w.wind.strength = r.i(0,5);
  }
  if (w.weather.state==STORM) w.wind.strength = std::max(w.wind.strength, 3);
}

// ===== Water simulation =====
static void stepWater(World& w, Rng& r){
  WaterF next = w.water;
  const float maxMass = MAX_WATER;

  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      float here = w.water[y][x];
      if (here <= 0.0001f) continue;

      // flow to 4 neighbors
      const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
      for(int i=0;i<4;++i){
        int nx=x+dirs[i][0], ny=y+dirs[i][1];
        if(!inBounds(nx,ny)) continue;
        float nwater = w.water[ny][nx];
        float diff = (here - nwater) / 4.f;
        if (diff <= 0.001f) continue;
        float flow = std::min(diff * FLOW_RATE, here);
        next[y][x] -= flow;
        next[ny][nx] = std::min(maxMass, next[ny][nx] + flow);
      }
    }
  }

  // rainfall
  if (w.weather.state==RAIN || w.weather.state==STORM) {
    int hits = (int)((W*H) * w.weather.rainStrength * 0.02f);
    for(int i=0;i<hits;++i){
      int x=r.i(0,W-1), y=r.i(0,H-1);
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

  // evaporation (less aggressive)
  for(int y=0;y<H;++y) for(int x=0;x<W;++x){
    if (next[y][x] > 0.f) next[y][x] = std::max(0.f, next[y][x] - EVAP_RATE*0.35f);
  }

  w.water.swap(next);
}

// ===== Terrain + plants =====
static void stepTerrain(World& w, Rng& r, Season s){
  Grid next = w.terrain;
  float drought = (w.weather.state==CLEAR && s==SUMMER) ? 1.5f : 1.0f;

  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      if (w.water[y][x] > 0.2f) continue;
      char c = w.terrain[y][x];
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
        if (r.u01() < 0.0008f * drought) next[y][x]='.';
      }

      // growth
      if (c=='.') {
        if (r.u01() < 0.0015f / drought) next[y][x]=',';
      } else if (c=='d') {
        if (w.weather.state==CLEAR && r.oneIn(40)) next[y][x]='.';
      } else if (c==',') {
        if (r.u01() < 0.0012f) next[y][x]='"';
        if (r.u01() < 0.0007f) next[y][x]=';';
        float flowerChance = 0.0004f;
        if (w.biome==MEADOW) flowerChance=0.0010f;
        else if (w.biome==TROPICAL) flowerChance=0.0008f;
        else if (w.biome==WETLAND) flowerChance=0.0006f;
        else if (w.biome==ALPINE) flowerChance=0.0005f;
        else if (w.biome==TAIGA) flowerChance=0.0004f;
        else if (w.biome==DESERT) flowerChance=0.0002f;
        else if (w.biome==ALIEN) flowerChance=0.0012f;
        if (r.u01() < flowerChance / drought) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
          next[y][x] = flowerForBiome(w.biome, r, h);
        }
      } else if (c=='"') {
        if (r.u01() < 0.0009f) next[y][x]='#';
        if (r.u01() < 0.0003f / drought) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
          next[y][x] = flowerForBiome(w.biome, r, h);
        }
      } else if (c==';') {
        if (r.u01() < 0.0002f / drought) {
          uint32_t h = hash3((uint32_t)x,(uint32_t)y,w.seed);
          next[y][x] = flowerForBiome(w.biome, r, h);
        }
      }
    }
  }
  w.terrain.swap(next);
}

static void stepWaterPlants(World& w, Rng& r){
  Grid next = w.terrain;
  bool wet = (w.biome==WETLAND || w.biome==TROPICAL);
  int lilyCount = countChar(w.terrain, 'm');
  int lilyCap = wet ? (W*H)/280 : (W*H)/900;
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      float water = w.water[y][x];
      if (water <= 0.2f) {
        if (next[y][x]=='m') next[y][x]=' ';
        continue;
      }
      if (water > 3.5f && next[y][x]=='m') { next[y][x]=' '; continue; }

      if (lilyCount < lilyCap && (wet || r.oneIn(1200))) {
        if (water < 2.6f && (next[y][x]==' ' || next[y][x]=='.')) {
          if (r.oneIn(300)) { next[y][x]='m'; lilyCount++; }
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
    }
  } else if (w.biome==ALPINE) {
    if (r.oneIn(600)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='^') next[y][x]='l';
    }
  } else if (w.biome==TROPICAL) {
    if (r.oneIn(500)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]==';' || w.terrain[y][x]=='"') next[y][x]='n';
    }
  } else if (w.biome==TAIGA) {
    if (r.oneIn(700)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='T' || w.terrain[y][x]=='Y') next[y][x]='l';
    }
  } else if (w.biome==ALIEN) {
    if (r.oneIn(400)) {
      int x=r.i(0,W-1), y=r.i(0,H-1);
      if (w.terrain[y][x]=='.') next[y][x]='q';
    }
  }
  w.terrain.swap(next);
}

// ===== Fire =====
static void stepFire(World& w, Rng& r){
  Grid next = w.terrain;
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      if (w.terrain[y][x]=='*') {
        // burn neighbors
        for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
          if (!dx && !dy) continue;
          int nx=x+dx, ny=y+dy; if(!inBounds(nx,ny)) continue;
          if (isVeg(w.terrain[ny][nx]) && r.oneIn(3)) next[ny][nx]='*';
        }
        // turn to ash
        if (r.oneIn(3)) next[y][x]='x';
        if (r.oneIn(4)) { Event ev; ev.type=Event::EV_FIRE; ev.x=x; ev.y=y; ev.mag=1.f; w.events.push_back(ev); }
      }
      if (w.terrain[y][x]=='x' && r.oneIn(40)) next[y][x]='.';
    }
  }
  w.terrain.swap(next);
}

// ===== Agents =====
static void stepAgents(World& w, Rng& r){
  // immigration
  if ((int)w.agents.size() < MAX_AGENTS) {
    int denom = (int)std::max(20.f, 120.f / std::max(0.1f, g_alea.spawnChance));
    if (r.oneIn(denom)) {
    bool aquatic = r.oneIn(2);
    SpeciesId sp;
    if (aquatic) sp = g_biomes[w.biome].aqua[r.i(0,(int)g_biomes[w.biome].aqua.size()-1)];
    else sp = g_biomes[w.biome].herb[r.i(0,(int)g_biomes[w.biome].herb.size()-1)];
    int tries=0;
    while(tries++<200){
      int side = r.i(0,3);
      int x = (side==0?0: side==1?W-1:r.i(0,W-1));
      int y = (side==2?0: side==3?H-1:r.i(0,H-1));
      if (g_species[sp].aquatic && w.water[y][x] <= 0) continue;
      if (!g_species[sp].aquatic && w.water[y][x] > 0) continue;
      Agent a; a.id=(int)w.agents.size()+1; a.x=x; a.y=y; a.sp=sp; a.health=1.f;
      a.hunger=r.u01()*0.3f; a.thirst=r.u01()*0.3f; a.fatigue=r.u01()*0.3f; a.stress=r.u01()*0.2f; a.emotion=0.6f;
      a.bold=0.3f+0.7f*r.u01();
      a.social=0.3f+0.7f*r.u01();
      a.curious=0.3f+0.7f*r.u01();
      a.aggro=0.2f+0.8f*r.u01();
      w.agents.push_back(a);
      break;
    }
    }
  }

  for (auto &a : w.agents){
    const SpeciesDef &sd = g_species[a.sp];
    a.hunger = clamp01(a.hunger + sd.hungerRate);
    a.thirst = clamp01(a.thirst + sd.thirstRate);
    a.stress = clamp01(a.stress + 0.01f * (a.hunger + a.thirst));
    a.fatigue = clamp01(a.fatigue + 0.004f);

    // drink
    if (a.thirst > 0.7f) {
      if ((sd.aquatic && w.water[a.y][a.x] > 0) || (!sd.aquatic && w.water[a.y][a.x] > 0)) {
        a.thirst = std::max(0.f, a.thirst - 0.4f);
        a.stress = clamp01(a.stress - 0.05f);
        Event ev; ev.type=Event::EV_DRINK; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
      }
    }

    // eat
    if (sd.herbivore && isEdiblePlant(w.terrain[a.y][a.x])) {
      a.hunger = std::max(0.f, a.hunger - 0.4f);
      w.terrain[a.y][a.x] = '.';
      a.stress = clamp01(a.stress - 0.08f);
      Event ev; ev.type=Event::EV_EAT; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
    }

    // predator proximity stress + fear
    int predDist = 999;
    if (!sd.carnivore) {
      for (auto &p : w.agents){
        if (g_species[p.sp].carnivore) {
          int d = std::abs(p.x-a.x)+std::abs(p.y-a.y);
          if (d < predDist) predDist = d;
        }
      }
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
    if (sd.schooling) {
      for (const auto& o : w.agents){
        if (o.sp != a.sp) continue;
        int d = std::abs(o.x-a.x) + std::abs(o.y-a.y);
        if (d>4) continue;
        schoolCx += o.x; schoolCy += o.y; schoolN++;
      }
      schoolCx /= schoolN; schoolCy /= schoolN;
    }

    // move toward target (allow stay to rest)
    int bestDx=0,bestDy=0; int bestScore=-999;
    for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx){
      int nx=a.x+dx, ny=a.y+dy; if(!inBounds(nx,ny)) continue;
      if (!sd.aquatic && w.water[ny][nx] > 0.4f) continue;
      if (sd.aquatic && w.water[ny][nx] <= 0.2f) continue;
      int score=0;
      bool staying = (dx==0 && dy==0);
      if (sd.herbivore && isEdiblePlant(w.terrain[ny][nx])) score += 3;
      if (sd.carnivore) {
        for (auto &p : w.agents){ if (&p==&a) continue; if (g_species[p.sp].herbivore) {
            int d = std::abs(p.x-nx)+std::abs(p.y-ny);
            if (d<3) score += (int)std::lround(2.0f + a.aggro);
          }}
      }
      // thirst seeking
      if (a.thirst > 0.6f) {
        if (w.water[ny][nx] > 0.2f) score += 2;
      }
      // rest when fatigued
      if (a.fatigue > 0.7f && staying) score += 3;
      if (a.fatigue > 0.8f && !staying) score -= 2;
      if (sd.schooling) {
        int dcur = std::abs(a.x-schoolCx)+std::abs(a.y-schoolCy);
        int dnext = std::abs(nx-schoolCx)+std::abs(ny-schoolCy);
        if (dnext < dcur) score += 2;
        if (dnext > dcur) score -= 1;
      }
      // social/curious personality
      if (!sd.schooling && a.social > 0.6f) {
        score += (int)std::lround((a.social-0.6f) * 2.0f);
      }
      uint32_t k = hash3((uint32_t)nx,(uint32_t)ny,(uint32_t)w.seed);
      if (a.curious > 0.6f && (k & 7u)==0u) score += 1;
      score += r.i(0,2);
      if (score>bestScore){ bestScore=score; bestDx=dx; bestDy=dy; }
    }
    a.x = clampi(a.x + bestDx, 0, W-1);
    a.y = clampi(a.y + bestDy, 0, H-1);
    if (bestDx==0 && bestDy==0) a.fatigue = clamp01(a.fatigue - 0.02f);
    else a.fatigue = clamp01(a.fatigue + 0.01f);

    // birds pick fish at water edges
    if (a.sp == SP_BIRD && a.hunger > 0.4f) {
      for (auto &p : w.agents){
        if (p.sp != SP_FISH) continue;
        int d = std::abs(p.x-a.x)+std::abs(p.y-a.y);
        if (d==1 && w.water[p.y][p.x] > 0.2f && r.oneIn(4)) {
          p.health -= 0.6f;
          a.hunger = std::max(0.f, a.hunger - 0.5f);
          Event ev; ev.type=Event::EV_EAT; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
          break;
        }
      }
    }

    // carnivores eat adjacent herbivores
    if (sd.carnivore) {
      for (auto &p : w.agents){
        if (&p==&a) continue;
        if (!g_species[p.sp].herbivore) continue;
        int d = std::abs(p.x-a.x)+std::abs(p.y-a.y);
        if (d==0 && r.oneIn(3)) {
          p.health -= 0.5f;
          a.hunger = std::max(0.f, a.hunger - 0.5f);
          a.stress = clamp01(a.stress - 0.12f);
          Event ev; ev.type=Event::EV_EAT; ev.x=a.x; ev.y=a.y; ev.mag=2.f; w.events.push_back(ev);
        }
      }
    }

    // health
    float harm = 0.f;
    if (a.hunger>0.9f) harm += (a.hunger-0.9f);
    if (a.thirst>0.9f) harm += (a.thirst-0.9f)*1.2f;
    a.health = clamp01(a.health - harm*0.05f);
    a.panic = (a.stress > 0.75f);
    if (a.health < 0.1f) {
      Event ev; ev.type=Event::EV_DEATH; ev.x=a.x; ev.y=a.y; ev.mag=1.f; w.events.push_back(ev);
    }

    // reproduction
    if (a.health>0.8f && a.hunger<0.4f && r.u01() < sd.reproduce*0.15f) {
      if ((int)w.agents.size() < MAX_AGENTS) {
        Agent child=a; child.id=(int)w.agents.size()+1; child.hunger=0.2f; child.thirst=0.2f; child.health=0.7f;
        child.fatigue=0.2f; child.stress=0.2f; child.emotion=0.6f;
        float mut = 0.08f;
        child.bold = clamp01(a.bold + (r.u01()-0.5f)*mut);
        child.social = clamp01(a.social + (r.u01()-0.5f)*mut);
        child.curious = clamp01(a.curious + (r.u01()-0.5f)*mut);
        child.aggro = clamp01(a.aggro + (r.u01()-0.5f)*mut);
        w.agents.push_back(child);
        Event ev; ev.type=Event::EV_BIRTH; ev.x=child.x; ev.y=child.y; ev.mag=1.f; w.events.push_back(ev);
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

  // sample all (cheap enough at 200x112)
  for(int y=0;y<H;++y){
    for(int x=0;x<W;++x){
      if (w.water[y][x] > 0.2f) ++waterC;
      if (isVeg(w.terrain[y][x])) ++plantC;
      if (w.terrain[y][x] == '*') ++fireCells;
      if (w.overlay[y][x] != ' ') ++overC;
      float alt = (float)w.height[y][x] / 255.f;
      meanAlt += alt;
      meanAlt2 += alt*alt;
    }
  }
  float waterFrac = (float)waterC / (float)(W*H);
  float plantFrac = (float)plantC / (float)(W*H);
  float overFrac  = (float)overC  / (float)(W*H);

  static std::vector<std::pair<int,int>> prevPos;
  if (prevPos.size() != w.agents.size()) prevPos.assign(w.agents.size(), {-9999,-9999});

  int predators=0, prey=0;
  int crabN=0, eelN=0, fishN=0, birdN=0, alienN=0;

  for (size_t i=0;i<w.agents.size();++i){
    const auto& a = w.agents[i];
    agentsV++;
    stressSum += a.stress;
    hungerSum += a.hunger;
    thirstSum += a.thirst;
    fatSum    += a.fatigue;
    healthSum += a.health;
    emoSum    += a.emotion;
    boldSum   += a.bold;
    socialSum += a.social;
    curiousSum+= a.curious;
    aggroSum  += a.aggro;
    if (a.stress > 0.75f) stressHi++;
    if (a.panic) panicC++;

    if (prevPos[i].first!=-9999) {
      int dx=a.x - prevPos[i].first;
      int dy=a.y - prevPos[i].second;
      speedSum += std::sqrt(float(dx*dx + dy*dy));
      if (dx==0 && dy==0) restCount++;
    }
    prevPos[i] = {a.x,a.y};

    if (g_species[a.sp].carnivore) predators++; else prey++;
    if (a.sp==SP_CRAB) crabN++;
    if (a.sp==SP_EEL) eelN++;
    if (a.sp==SP_FISH) fishN++;
    if (a.sp==SP_BIRD) birdN++;
    if (a.sp==SP_ALIEN1 || a.sp==SP_ALIEN2) alienN++;
    speciesSeen[a.sp] = true;
    if (g_species[a.sp].aquatic) aquaticCount++; else landCount++;

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
    distWaterSum += (float)bestD / 4.f;

    // schooling cohesion
    if (g_species[a.sp].schooling) {
      int nearest=99;
      for (const auto& o : w.agents){
        if (&o==&a) continue;
        if (o.sp!=a.sp) continue;
        int d = std::abs(o.x-a.x)+std::abs(o.y-a.y);
        if (d < nearest) nearest = d;
      }
      if (nearest < 99) { schoolDistSum += (float)nearest; schoolingCount++; }
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
  float predPressure = (prey>0)? (float)predators/(float)prey : (predators? 4.f:0.f);

  float p = plantFrac;
  plantVarSum = p*(1.f-p);
  float f = (agentsV>0)? (float)agentsV / (float)(W*H) : 0.f;
  faunaVarSum = f*(1.f-f);

  for (const auto& a : w.agents){
    if (a.x<2 || a.y<2 || a.x>W-3 || a.y>H-3) edgeAct += 1.f;
  }
  edgeAct *= invA;

  for (const auto& ev : w.events){
    if (ev.type==Event::EV_EAT && ev.mag>1.2f) huntEvents++;
    if (ev.type==Event::EV_EAT && ev.mag<=1.2f) forageEvents++;
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

  auto clamp01f=[](float v){ return v<0.f?0.f:(v>1.f?v: v); };
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
  float o8 = g_modVal[25] * (0.2f + g_modVal[4]);
  float o9 = (float)((tick/37)%11)/10.0f;
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

  static float prev[MOD_N] = {0};
  for(int i=0;i<MOD_N;++i){
    float v = g_modVal[i]*2.0f - 1.0f;
    float dv = v - prev[i];
    prev[i] = v;
    float sp = v + 0.85f*dv + ((float)((tick + i*131) % 97) / 96.0f - 0.5f) * 0.06f;
    g_modVal[i] = clamp11(sp);
  }
}

// ===== MIDI event mapping =====
enum ScaleType { SCALE_CHROMATIC=0, SCALE_MAJOR=1, SCALE_MINOR=2, SCALE_PENTATONIC=3, SCALE_DORIAN=4, SCALE_LYDIAN=5, SCALE_WHOLE=6 };

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

static inline void serviceNoteOffs(SynthOut& synth, MidiOut& midi, int tick){
  for (int ch=0; ch<16; ++ch){
    if (g_activeNotes[ch].on && tick >= g_activeNotes[ch].offTick){
      int note = g_activeNotes[ch].note;
      synth.noteOff(ch, note, 0);
      midi.sendNoteOff(ch, note, 0);
      g_activeNotes[ch].on=false;
    }
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

static inline void gatedNoteOn(SynthOut& synth, MidiOut& midi, int ch, int note, int vel, int tick, int dur){
  if (g_activeNotes[ch].on) {
    synth.noteOff(ch, g_activeNotes[ch].note, 0);
    midi.sendNoteOff(ch, g_activeNotes[ch].note, 0);
  }
  synth.noteOn(ch, note, vel);
  midi.sendNoteOn(ch, note, vel);
  g_activeNotes[ch].note = note;
  g_activeNotes[ch].offTick = tick + std::max(1, dur);
  g_activeNotes[ch].on = true;
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
};
static MusicState g_music;

static void synthTickMusic(const World& w, SynthOut& synth, MidiOut& midi, int tick){
  serviceNoteOffs(synth, midi, tick);

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
  float emo = std::clamp(g_modVal[35], -1.f, 1.f);
  float bold = std::clamp(g_modVal[36], -1.f, 1.f);
  float social = std::clamp(g_modVal[37], -1.f, 1.f);
  float curious = std::clamp(g_modVal[38], -1.f, 1.f);
  float aggro = std::clamp(g_modVal[39], -1.f, 1.f);

  // chord progression selection
  static const int progA[] = {0,3,4,3}; // I-IV-V-IV
  static const int progB[] = {5,3,0,4}; // vi-IV-I-V
  static const int progC[] = {0,5,3,4}; // I-vi-IV-V
  const int* prog = progA;
  int psel = (int)std::lround((g_modVal[24]+1.f) * 1.5f);
  if (psel==1) prog = progB;
  else if (psel>=2) prog = progC;

  static const int biomeChordBase[BIOME_COUNT] = {48, 44, 46, 56, 40, 50, 38};
  static const int biomeArpBase[BIOME_COUNT]   = {8,  7,  8,  10, 6,  9,  5};
  int chordInterval = (int)std::lround((biomeChordBase[w.biome] - 6.0f*aggro) / g_tempoMult);
  int arpInterval = (int)std::lround((biomeArpBase[w.biome] - 2.0f*aggro) / g_tempoMult);
  if (g_modVal[16] > 0.2f) arpInterval = 6;
  if (g_modVal[19] > 0.4f) arpInterval = 5;
  chordInterval = std::max(16, chordInterval);
  arpInterval = std::max(3, arpInterval);

  if (tick >= g_music.nextChordTick){
    g_music.chordIndex = (g_music.chordIndex + 1) % 4;
    g_music.chordType = (g_modVal[15] > 0.2f) ? 3 : (g_modVal[5] > 0.3f ? 4 : 0);
    if (aggro > 0.3f) g_music.chordType = 2; // dominant 7 for edge
    g_music.nextChordTick = tick + chordInterval;

    // chord pad + bass
    int degree = prog[g_music.chordIndex];
    int rootNote = 48 + degree*2; // rough diatonic-ish
    rootNote = quantizeNoteToScale(rootNote, root, scale);
    std::vector<int> chord;
    buildChord(chord, rootNote, scale, g_music.chordType);

    // voice 1: chord pad
    for (int n : chord){
      int note = quantizeNoteToScale(n+12, root, scale);
      gatedNoteOn(synth, midi, 1, note, 70, tick, chordInterval-2);
    }
    // voice 2: bass
    gatedNoteOn(synth, midi, 2, rootNote-12, 80, tick, chordInterval-4);
  }

  if ((tick % arpInterval)==0){
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
    mel = quantizeNoteToScale(mel, root, scale);
    if (aggro > 0.4f && (g_music.arpStep % 3)==0) mel += (aggro>0.f ? 1 : -1); // chromatic passing
    int vel = 60 + (int)std::lround(std::clamp(g_modVal[4], -1.f, 1.f)*20.f + 20.f);
    gatedNoteOn(synth, midi, 0, mel, std::clamp(vel,30,110), tick, arpInterval-1);

    // voice 3 shimmer tied to ripple energy
    int shimmer = note + 12 + (int)std::lround(g_modVal[15]*6.f + social*2.f);
    shimmer = quantizeNoteToScale(shimmer, root, scale);
    int svel = 50 + (int)std::lround(std::fabs(g_modVal[15])*40.f);
    gatedNoteOn(synth, midi, 3, shimmer, std::clamp(svel,30,110), tick, arpInterval-1);
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
    if (kick) gatedNoteOn(synth, midi, 9, 36, 90, tick, stepInterval-1);
    if (snare) gatedNoteOn(synth, midi, 9, 38, 80, tick, stepInterval-1);
    if (hat) gatedNoteOn(synth, midi, 9, 42, 60, tick, stepInterval-1);
  }
  // track 2: percussion layer
  if (g_drum2Vol>0) {
    bool perc = (step==2 || step==6 || step==10 || step==14) || (chaos>0.6f && step%4==1);
    bool toms = (step==7 || step==15) && chaos>0.4f;
    if (perc) gatedNoteOn(synth, midi, 9, 56, 70, tick, stepInterval-1);
    if (toms) gatedNoteOn(synth, midi, 9, 45, 75, tick, stepInterval-1);
  }
  // track 3: cymbals/accents
  if (g_drum3Vol>0) {
    bool ride = (step%4==0) || (chaos>0.5f && step%2==1);
    bool crash = (step==0 || step==8) && chaos>0.5f;
    if (ride) gatedNoteOn(synth, midi, 9, 51, 60, tick, stepInterval-1);
    if (crash) gatedNoteOn(synth, midi, 9, 49, 90, tick, stepInterval-1);
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
    if (g_drumVol>0) gatedNoteOn(synth, midi, 9, note, vel, tick, dur);
  }
}

static void applyAutomation(SynthOut& synth, MidiOut& midi){
  for(int ch=0; ch<4; ++ch){
    if (g_voiceProgDirty[ch]) {
      int prog = clampi(g_voiceProg[ch], 0, 127);
      g_voiceProg[ch] = prog;
      synth.programChange(ch, prog);
      midi.sendProgramChange(ch, (uint8_t)prog);
      g_voiceProgDirty[ch] = false;
    }
    int cc11 = (int)std::lround(std::clamp(g_cc11Expr,0.f,1.f)*127.f);
    int cc74 = (int)std::lround(std::clamp(g_cc74Bright,0.f,1.f)*127.f);
    int cc10 = (int)std::lround(std::clamp(g_pan01,0.f,1.f)*127.f);
    synth.cc(ch, 11, cc11); midi.sendCC(ch,11,cc11);
    synth.cc(ch, 74, cc74); midi.sendCC(ch,74,cc74);
    synth.cc(ch, 10, cc10); midi.sendCC(ch,10,cc10);
    int porta = (int)std::lround(std::clamp(g_porta01[ch],0.f,1.f)*127.f);
    synth.cc(ch, 5, porta); midi.sendCC(ch,5,porta);
    int vol = clampi(g_voiceVol[ch], 0, 127);
    synth.cc(ch, 7, vol); midi.sendCC(ch,7,vol);
  }
  int dvol = clampi(g_drumVol, 0, 127);
  synth.cc(9, 7, dvol); midi.sendCC(9,7,dvol);
  // track 2/3 share drum channel; scale their note velocities by volume gate (handled in synthTickMusic)
  synth.setGain(std::clamp(g_masterGain, 0.1f, 2.0f));
}

// ===== Rendering =====
static inline char waterGlyph(float w){
  int d = (int)std::round(std::clamp(w, 0.f, MAX_WATER));
  d = std::clamp(d, 1, 7);
  return (char)('0' + d);
}

static inline RGB lerpRGB(const RGB& a, const RGB& b, float t){
  return { (uint8_t)std::lround(a.r + (b.r-a.r)*t),
           (uint8_t)std::lround(a.g + (b.g-a.g)*t),
           (uint8_t)std::lround(a.b + (b.b-a.b)*t) };
}

static RGB fgForChar(const World& w, char c, int x, int y, Season s){
  const BiomeDef& b0 = g_biomes[w.biome];
  const BiomeDef& b1 = g_biomes[w.biomeMorphActive ? w.targetBiome : w.biome];
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

  if (c==' ') return {0,0,0};
  if (c>='1' && c<='7') {
    int d = c-'0';
    float t = d/7.f;
    int r = (int)(b.waterShallow.r*(1.f-t) + b.waterDeep.r*t);
    int g = (int)(b.waterShallow.g*(1.f-t) + b.waterDeep.g*t);
    int bl = (int)(b.waterShallow.b*(1.f-t) + b.waterDeep.b*t);
    return boostColor({ (uint8_t)r, (uint8_t)g, (uint8_t)bl }, 1.1f, 1.05f);
  }
  if (c=='=') return boostColor(b.foam, 1.1f, 1.15f);
  if (c=='d' || c=='e' || c=='g') return boostColor(jitter({110,80,55}, 4), 1.05f, 1.05f);
  if (c=='^') return boostColor(b.rock, 1.05f, 1.05f);
  if (c=='.') return boostColor(jitter(b.soil, 3), 1.08f, 1.05f);
  if (c=='s') return boostColor(jitter({150,120,80}, 4), 1.1f, 1.05f);
  if (c==',' || c=='"' || c==';' || c=='#') return boostColor(jitter(b.grass, 4), 1.15f, 1.08f);
  if (c=='T' || c=='Y') return boostColor(jitter(b.tree, 3), 1.12f, 1.05f);
  if (c=='m') return boostColor({70,170,120}, 1.2f, 1.1f);
  if (c=='t') return boostColor({200,150,90}, 1.1f, 1.05f);
  if (c=='l') return boostColor({120,180,140}, 1.1f, 1.05f);
  if (c=='n') return boostColor({90,190,120}, 1.2f, 1.1f);
  if (c=='q') return boostColor({180,80,220}, 1.2f, 1.15f);
  if (c=='f' || c=='+' || c=='&' || c=='!') {
    uint32_t hh = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(w.seed + 777));
    RGB alt = b.flower;
    if ((hh & 3u) == 0u) alt = {240,200,60};
    else if ((hh & 3u) == 1u) alt = {220,120,200};
    else if ((hh & 3u) == 2u) alt = {120,200,240};
    if (w.biome==ALIEN) alt = {210,60,230};
    return boostColor(jitter(alt, 6), 1.35f, 1.20f);
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
    if (c=='r') return boostColor({200,160,120}, 1.1f, 1.05f); // rabbit
    if (c=='d') return boostColor({180,140,90}, 1.1f, 1.05f);  // deer
    if (c=='g') return boostColor({160,160,140}, 1.1f, 1.05f); // goat
    if (c=='f') return boostColor({120,180,220}, 1.2f, 1.1f);  // fish
    if (c=='c') return boostColor({200,120,80}, 1.2f, 1.05f);  // crab
    if (c=='p') return boostColor({120,200,120}, 1.2f, 1.05f); // frog
    if (c=='w') return boostColor({180,180,200}, 1.05f, 1.05f); // wolf
    if (c=='b') return boostColor({140,110,80}, 1.1f, 1.05f);  // bear
    if (c=='e') return boostColor({80,160,200}, 1.2f, 1.05f);  // eel
    if (c=='v') return boostColor({200,200,140}, 1.2f, 1.05f); // bird
    if (c=='A' || c=='Z') return boostColor({210,80,230}, 1.3f, 1.15f);
    return {220,220,220};
  }
  return {200,200,200};
}

static void render(SDL_Renderer* ren, const Layout& L, World& w, GlyphCache& gcWorld, GlyphCache& gcText, int tick, bool showMenu, int menuPage, int menuSel){
  setColor(ren, 0,0,0);
  SDL_RenderClear(ren);

  int viewW=W, viewH=H;
  for(int y=0;y<viewH;++y){
    int y0 = (y * L.simHpx) / viewH;
    int y1 = ((y+1) * L.simHpx) / viewH;
    int hpx = std::max(1, y1 - y0);
    for(int x=0;x<viewW;++x){
      int x0 = (x * L.screenW) / viewW;
      int x1 = ((x+1) * L.screenW) / viewW;
      int wpx = std::max(1, x1 - x0);
      SDL_Rect rc{ x0, y0, wpx, hpx };

      char c = renderCharAt(w, x, y, tick);
      if (w.water[y][x] <= 0.2f && c == w.terrain[y][x]) {
        uint32_t h = hash3((uint32_t)x,(uint32_t)y,(uint32_t)(tick/7));
        c = terrainGlyphVariant(c, h, seasonAt(tick), w.weather);
      }

      SDL_Texture* gt = gcWorld.get(ren, (unsigned char)c);
      if (gt) {
        RGB fg = fgForChar(w, c, x, y, seasonAt(tick));
        SDL_SetTextureColorMod(gt, fg.r, fg.g, fg.b);
        SDL_RenderCopy(ren, gt, nullptr, &rc);

        // global glow (CRT-ish)
        {
          int lum = (fg.r + fg.g + fg.b) / 3;
          int a = clampi(18 + lum / 10, 20, 60);
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
          setColor(ren, fg.r, fg.g, fg.b, (uint8_t)a);
          SDL_RenderFillRect(ren, &rc);
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        }

        // subtle glow for water
        if (c>='1' && c<='7') {
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
          setColor(ren, fg.r, fg.g, fg.b, 28);
          SDL_RenderFillRect(ren, &rc);
          SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        }
      }

      // cloud overlay
      if (w.overlay[y][x] == 'o') {
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
    }
  }

  // CRT scanlines + vignette
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
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
  SDL_Rect top{0,0,L.screenW,20};
  SDL_Rect bot{0,L.screenH-20,L.screenW,20};
  SDL_Rect left{0,0,20,L.screenH};
  SDL_Rect right{L.screenW-20,0,20,L.screenH};
  setColor(ren, 0,0,0, 28);
  SDL_RenderFillRect(ren, &top);
  SDL_RenderFillRect(ren, &bot);
  SDL_RenderFillRect(ren, &left);
  SDL_RenderFillRect(ren, &right);

  if (showMenu) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int panelW = std::min(360, L.screenW/3);
    SDL_Rect panel{ 8, 8, panelW, L.screenH - 16 };
    setColor(ren, 0,0,0, 170);
    SDL_RenderFillRect(ren, &panel);
    int tx = 16;
    int ty = 16;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "MENU [%d]  M toggle  TAB page", menuPage);
    drawString(ren, gcText, tx, ty, buf, 200,200,200, 230, 1);
    ty += 12;
    drawString(ren, gcText, tx, ty, "UP/DOWN select  +/- edit", 180,180,180, 230, 1);

    if (menuPage==0) {
      ty += 16;
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
      drawString(ren, gcText, tx, ty, "Alea", 220,220,220, 230, 1);
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
      drawString(ren, gcText, tx, ty, "MODS (70)", 200,200,200, 230, 1);
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
      drawString(ren, gcText, tx, ty, "VOICE PROGS", 200,200,200, 230, 1);
      ty += 12;
      menuSel = clampi(menuSel, 0, 3);
      for (int i=0;i<4;++i){
        std::snprintf(buf, sizeof(buf), "V%d program %3d", i, g_voiceProg[i]);
        uint8_t rr = (i==menuSel)?255:200;
        uint8_t gg = (i==menuSel)?255:200;
        drawString(ren, gcText, tx, ty + i*12, buf, rr,gg,220, 230, 1);
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
      std::snprintf(buf, sizeof(buf), "Drum prog %3d bank %3d", g_drumProg, g_drumBank);
      rr = (menuSel==7)?255:200;
      gg = (menuSel==7)?255:200;
      drawString(ren, gcText, tx, ty + 7*12, buf, rr,gg,220, 230, 1);
      std::snprintf(buf, sizeof(buf), "Master gain %.2f", g_masterGain);
      drawString(ren, gcText, tx, ty + 8*12, buf, 180,180,200, 230, 1);
    }
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
  if (w.biomeMorphActive) {
    w.biomeMorphT += 0.01f;
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
  if (doAgents) {
    stepBigCreatures(w, r);
    stepAgents(w, r);
  }
  if (doClouds) updateClouds(w, r);
}

int main(int argc, char** argv){
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n"; return 1;
  }

  bool startFullscreen = true;
  Biome startBiome = MEADOW;
  bool wantSynth=false;
  std::string sf2Path="";

  for(int i=1;i<argc;++i){
    if (std::strcmp(argv[i], "--windowed")==0) startFullscreen=false;
    else if (std::strcmp(argv[i], "--fullscreen")==0) startFullscreen=true;
    else if (std::strcmp(argv[i], "--biome")==0 && i+1<argc) {
      std::string s=argv[++i];
      if (s=="meadow") startBiome=MEADOW;
      if (s=="wetland") startBiome=WETLAND;
      if (s=="alpine") startBiome=ALPINE;
      if (s=="desert") startBiome=DESERT;
      if (s=="tropical") startBiome=TROPICAL;
      if (s=="taiga") startBiome=TAIGA;
      if (s=="alien") startBiome=ALIEN;
    }
    else if (std::strcmp(argv[i], "--synth")==0) wantSynth=true;
    else if (std::strcmp(argv[i], "--sf2")==0 && i+1<argc) { sf2Path=argv[++i]; wantSynth=true; }
  }

  Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
  if (startFullscreen) wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_Window* win = SDL_CreateWindow("Terrarium Remix", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, wflags);
  if (!win) { std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n"; SDL_Quit(); return 1; }

  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) { std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n"; SDL_DestroyWindow(win); SDL_Quit(); return 1; }

  uint32_t seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
  Rng r(seed);

  World world; seedWorld(world, r, startBiome);
  GlyphCache gcWorld;
  GlyphCache gcText; gcText.textMode = true;
  Layout layout = computeLayout(ren);

  SynthOut synth; MidiOut midi;
  if (wantSynth && !sf2Path.empty()) {
    if (synth.open(sf2Path, 0.7f)) {
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
  const int WATER_TICK=1, TERRAIN_TICK=2, AGENT_TICK=3, WEATHER_TICK=6, CLOUD_TICK=4;
  auto last = std::chrono::steady_clock::now();

  while(running){
    SDL_Event e; while(SDL_PollEvent(&e)){
      if (e.type==SDL_QUIT) running=false;
      if (e.type==SDL_KEYDOWN){
        switch(e.key.keysym.sym){
          case SDLK_ESCAPE: running=false; break;
          case SDLK_m: showMenu=!showMenu; break;
          case SDLK_TAB: if (showMenu) menuPage = (menuPage + 1) % 6; break;
          case SDLK_SPACE: paused=!paused; break;
          case SDLK_LEFTBRACKET: if (tps>1) tps--; break;
          case SDLK_RIGHTBRACKET: if (tps<30) tps++; break;
          case SDLK_r: seedWorld(world, r, world.biome); tick=0; break;
          case SDLK_b: {
            world.targetBiome = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
            world.biomeMorphActive = true;
            world.biomeMorphT = 0.f;
          } break;
          case SDLK_F2: {
            world.targetBiome = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
            world.biomeMorphActive = true;
            world.biomeMorphT = 0.f;
          } break;
          case SDLK_UP: {
            if (showMenu && menuPage==2) { g_g_modScroll = std::max(0, g_g_modScroll-1); }
            else if (showMenu && menuPage==3) { g_g_mmSel = (g_g_mmSel + MOD_SLOTS - 1) % MOD_SLOTS; }
            else if (showMenu && menuPage==4) { menuSel = (menuSel + 3) % 4; }
            else if (showMenu && menuPage==5) { menuSel = (menuSel + 7) % 8; }
          } break;
          case SDLK_DOWN: {
            if (showMenu && menuPage==2) { g_g_modScroll = std::min(MOD_N-1, g_g_modScroll+1); }
            else if (showMenu && menuPage==3) { g_g_mmSel = (g_g_mmSel + 1) % MOD_SLOTS; }
            else if (showMenu && menuPage==4) { menuSel = (menuSel + 1) % 4; }
            else if (showMenu && menuPage==5) { menuSel = (menuSel + 1) % 8; }
          } break;
          case SDLK_LEFT:
            if (showMenu && menuPage==3) g_g_mmField = std::max(0, g_g_mmField-1);
            else if (showMenu && menuPage==5 && menuSel==7) { g_drumBank = clampi(g_drumBank - 1, 0, 127); sendDrumProgram(synth, midi); }
            break;
          case SDLK_RIGHT:
            if (showMenu && menuPage==3) g_g_mmField = std::min(3, g_g_mmField+1);
            else if (showMenu && menuPage==5 && menuSel==7) { g_drumBank = clampi(g_drumBank + 1, 0, 127); sendDrumProgram(synth, midi); }
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
            }
            else if (showMenu && menuPage==5) {
              if (menuSel>=0 && menuSel<=3) g_voiceVol[menuSel] = clampi(g_voiceVol[menuSel] + 5, 0, 127);
              else if (menuSel==4) g_drumVol = clampi(g_drumVol + 5, 0, 127);
              else if (menuSel==5) g_drum2Vol = clampi(g_drum2Vol + 5, 0, 127);
              else if (menuSel==6) g_drum3Vol = clampi(g_drum3Vol + 5, 0, 127);
              else if (menuSel==7) { g_drumProg = clampi(g_drumProg + 1, 0, 127); sendDrumProgram(synth, midi); }
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
            }
            else if (showMenu && menuPage==5) {
              if (menuSel>=0 && menuSel<=3) g_voiceVol[menuSel] = clampi(g_voiceVol[menuSel] - 5, 0, 127);
              else if (menuSel==4) g_drumVol = clampi(g_drumVol - 5, 0, 127);
              else if (menuSel==5) g_drum2Vol = clampi(g_drum2Vol - 5, 0, 127);
              else if (menuSel==6) g_drum3Vol = clampi(g_drum3Vol - 5, 0, 127);
              else if (menuSel==7) { g_drumProg = clampi(g_drumProg - 1, 0, 127); sendDrumProgram(synth, midi); }
            }
          } break;
          case SDLK_e: if (showMenu && menuPage==3) g_modMap[g_g_mmSel].enabled = !g_modMap[g_g_mmSel].enabled; break;
          case SDLK_F11: {
            Uint32 flags = SDL_GetWindowFlags(win);
            bool fs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            layout = computeLayout(ren);
          } break;
        }
      }
      if (e.type==SDL_WINDOWEVENT && (e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event==SDL_WINDOWEVENT_RESIZED)) {
        layout = computeLayout(ren);
      }
      if (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
        int mx = e.button.x;
        int my = e.button.y;
        int viewW = W;
        int viewH = H;
        int sx = (int)((int64_t)mx * viewW / std::max(1, layout.screenW));
        int sy = (int)((int64_t)my * viewH / std::max(1, layout.simHpx));
        int wx = clampi(sx, 0, W-1);
        int wy = clampi(sy, 0, H-1);
        Ripple rp; rp.cx=wx; rp.cy=wy;
        rp.amp = 3.0f + 5.0f * r.u01();
        rp.speed = 16.f + 18.f * r.u01();
        rp.width = 2.0f + 2.5f * r.u01();
        rp.chaos = 0.5f + 0.8f * r.u01();
        g_ripples.push_back(rp);
      }
    }

    auto now = std::chrono::steady_clock::now();
    auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
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
      applyAutomation(synth, midi);
      synthTickMusic(world, synth, midi, tick);
    }
    updateRipples((float)dtMs/1000.0f);
    render(ren, layout, world, gcWorld, gcText, tick, showMenu, menuPage, menuSel);
  }

  synth.close();
  midi.close();
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
  return 0;
}
