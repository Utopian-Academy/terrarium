#include "terrarium_core.hpp"

#include <ctime>

static inline float smooth1(float cur,float tgt,float s){
  float a=std::clamp(s,0.0f,0.98f);
  return cur*a + tgt*(1.0f-a);
}

void applyModMatrix(){
  g_cc11Expr=1.0f; g_cc74Bright=0.5f; g_pan01=0.5f;
  for(int v=0; v<NUM_VOICES; ++v) g_porta01[v]=0.0f;
  for(int i=0;i<MOD_SLOTS;++i) g_modCC01[i]=-1.0f;

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
      case DEST_MIDI_CC:  g_modCC01[i]=std::clamp(0.5f+0.5f*v,0.0f,1.0f); break;
      default: break;
    }
  }
}
static void applyRippleChaos(World& w, Rng& r, int tick);



static int countNeighborsWater(const Water& w, int x, int y) {
  int n = 0;
  for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
    if (dx==0 && dy==0) continue;
    int nx=x+dx, ny=y+dy;
    if (inBounds(nx,ny) && w[ny][nx]>0) n++;
  }
  return n;
}

int g_seasonMode = 0;  // 0 = sim ticks, 1 = one season per real day, 2 = calendar

static void localNow(std::tm& lt) {
  std::time_t now = std::time(nullptr);
#ifdef _WIN32
  localtime_s(&lt, &now);
#else
  localtime_r(&now, &lt);
#endif
}

Season seasonAt(int tick) {
  if (g_seasonMode == 1) {
    // One season per real day: a four-day year, aligned to local midnight.
    return (Season)((std::time(nullptr) / 86400) % 4);
  }
  if (g_seasonMode == 2) {
    std::tm lt{}; localNow(lt);
    int m = lt.tm_mon;  // 0=Jan; northern-hemisphere calendar
    if (m == 11 || m <= 1) return WINTER;
    if (m <= 4) return SPRING;
    if (m <= 7) return SUMMER;
    return AUTUMN;
  }
  return (Season)((tick / SEASON_TICKS) % 4);
}

float seasonLerp(int tick) {
  if (g_seasonMode == 1) {
    std::tm lt{}; localNow(lt);
    return ((float)lt.tm_hour * 3600.f + (float)lt.tm_min * 60.f +
            (float)lt.tm_sec) / 86400.f;
  }
  if (g_seasonMode == 2) {
    std::tm lt{}; localNow(lt);
    int m = lt.tm_mon;
    int intoSeason = (m == 11) ? 0 : ((m + 1) % 3);  // months into the season
    return ((float)intoSeason + (float)(lt.tm_mday - 1) / 31.f) / 3.f;
  }
  return float(tick % SEASON_TICKS) / float(SEASON_TICKS);
}
bool nightish(int tick) { return daylightNow(tick).level < 0.35f; }

int g_daynightMode = 1;

Daylight daylightNow(int tick) {
  float hour;
  if (g_daynightMode == 2) {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    hour = (float)lt.tm_hour + (float)lt.tm_min / 60.0f;
  } else if (g_daynightMode == 1) {
    // One full day = 8*DAY_TICKS (24 minutes at 5 tps). Start mid-morning
    // so a fresh vat opens in daylight.
    const int cycle = DAY_TICKS * 8;
    hour = 9.0f + 24.0f * (float)(tick % cycle) / (float)cycle;
    if (hour >= 24.0f) hour -= 24.0f;
  } else {
    return {1.0f, 0.0f};
  }

  auto smooth = [](float x) {
    x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
    return x * x * (3.f - 2.f * x);
  };

  Daylight d;
  if (hour < 5.f)        d.level = 0.f;
  else if (hour < 8.f)   d.level = smooth((hour - 5.f) / 3.f);   // dawn
  else if (hour < 18.f)  d.level = 1.f;
  else if (hour < 22.f)  d.level = 1.f - smooth((hour - 18.f) / 4.f);  // dusk
  else                   d.level = 0.f;

  if (hour >= 5.f && hour < 8.5f)
    d.warm = 0.8f * (1.f - std::fabs((hour - 6.75f) / 1.75f));   // sunrise gold
  else if (hour >= 17.5f && hour < 22.f)
    d.warm = 1.0f * (1.f - std::fabs((hour - 19.75f) / 2.25f));  // sunset amber
  else if (d.level <= 0.f)
    d.warm = -0.6f;                                              // moonlight
  return d;
}

int displayBgMode() {
  static int cached = 0;
  static std::chrono::steady_clock::time_point lastRead{};
  auto now = std::chrono::steady_clock::now();
  if (now - lastRead < std::chrono::seconds(1)) return cached;
  lastRead = now;
  const char* home = std::getenv("HOME");
  if (!home) return cached;
  std::string path = std::string(home) + "/.terrarium-bg";
  cached = 0;
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    int c = std::fgetc(f);
    if (c == 'o' || c == 'O') cached = 1;
    std::fclose(f);
  }
  return cached;
}

float displayBrightness() {
  static float cached = 1.0f;
  static std::chrono::steady_clock::time_point lastRead{};
  auto now = std::chrono::steady_clock::now();
  if (now - lastRead < std::chrono::seconds(1)) return cached;
  lastRead = now;
  const char* home = std::getenv("HOME");
  if (!home) return cached;
  std::string path = std::string(home) + "/.terrarium-brightness";
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    float v = 1.0f;
    if (std::fscanf(f, "%f", &v) == 1)
      cached = std::clamp(v, 0.05f, 1.0f);
    std::fclose(f);
  } else {
    cached = 1.0f;
  }
  return cached;
}

const char* weatherName(WeatherState state) {
  switch (state) {
    case CLEAR: return "clear";
    case OVERCAST: return "overcast";
    case RAIN: return "rain";
    case STORM: return "storm";
  }
  return "clear";
}

const char* seasonName(Season season) {
  switch (season) {
    case SPRING: return "spring";
    case SUMMER: return "summer";
    case AUTUMN: return "autumn";
    case WINTER: return "winter";
  }
  return "spring";
}


const char* speciesName(uint8_t s){
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
static inline char speciesDisplayGlyph(uint8_t spec, Biome /*b*/, int tick, bool legendA, bool legendB){
  int ph = (tick/5)&1; // 2-frame animation
  if(legendA) return (char)(ph? 'Y' : 'y');  // Legendary couple (A)
  if(legendB) return (char)(ph? 'Z' : 'z');  // Legendary couple (B)
  // Use dedicated "sprite glyphs" in the 0x80+ range so agents are always visually distinct from terrain.
  // (Rendered via custom 8x8 bitmaps in glyph8_world().)
  uint8_t s = (uint8_t)(spec % SPEC_COUNT);
  return (char)(0x80u + (uint8_t)(s*2u + (uint8_t)ph));
}

const char* biomeName(Biome b) {
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

BiomeWeights lerpBiomeWeights(const BiomeWeights& a, const BiomeWeights& b, float t){
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

BiomeWeights weightsFor(Biome b) {
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

// --- Mod pool update (moved here so World/Species are defined) ---
void updateModPool(World& w, int tick, int viewW, int viewH){
  // Sample the current camera viewport to keep modulation "what you see is what you hear".
  int x0=g_camX, y0=g_camY;
  int x1=std::min(W, g_camX+viewW);
  int y1=std::min(H, g_camY+viewH);
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

int speciesVariant2(const World& w, int x, int y, int n) {
  if (n <= 1) return 0;
  return (int)(speciesSeed2(w,x,y) % (uint32_t)n);
}

// ---------------- Helpers ----------------
char waterFlowGlyph(const World& w, int x, int y, int tick) {
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

bool isWaterVisualGlyph(unsigned char c) {
  // depth digits and custom wave/flow bitmaps live in low control-code range.
  if (c >= (unsigned char)'1' && c <= (unsigned char)'7') return true;
  if (c == (unsigned char)'0') return true; // used for d>=? and calm water
  if (c == (unsigned char)FOAM_GLYPH) return true;
  if (c < 0x20u && c != 0u) return true; // custom flow glyphs 0x01..0x15
  return false;
}

bool isTree(char c) { return c=='T' || c=='Y' || c=='P'; }
bool isVeg(char c) {
  return (c==','||c=='"'||c=='#'||c=='m'||c=='f'||c=='+'||c=='&'||c==';'||c==':'||c=='$'||c=='!'||isTree(c));
}
static inline bool blocksEntity(char terrain, uint8_t waterDepth) {
  if (waterDepth > 0) return true;
  if (terrain == '*' || terrain == 'B' || terrain == 'M' || terrain == '^' || terrain=='X' || terrain=='c') return true;
  return false;
}

void initClouds(Clouds& c, Rng& r, Biome b) {
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
  if (c.offX < 0) {
    c.offX += CW;
  }
  if (c.offX >= CW) {
    c.offX -= CW;
  }
  if (c.offY < 0) {
    c.offY += CH;
  }
  if (c.offY >= CH) {
    c.offY -= CH;
  }

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
  // Quadratic in strength: a shower rolling in starts as a few scattered
  // drops and thickens, instead of arriving as a wall of streaks.
  float rs = w.weather.rainStrength;
  int drops = (int)((float)(W * H) * rs * rs / 12.0f);
  if (drops < 1) return;

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
      case WETLAND:  return 1.15f;
      case TROPICAL: return 1.10f;
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
      w.weather.rainStrength = std::max(0.f, w.weather.rainStrength - 0.008f);
      if (cloudy && r.oneIn(9)) toState(OVERCAST);
      int chance = (s==SUMMER? 300 : s==SPRING? 240 : s==AUTUMN? 260 : 340);
      // Apply biome raininess: lower raininess => higher chance value (rarer transitions)
      chance = (int)std::clamp((float)chance / std::max(0.05f, raininess), 80.0f, 12000.0f);
      if (w.biome==TROPICAL) chance = std::max(120, chance-60);
      if (w.biome==DESERT) chance = std::max(4000, chance+2800); // far rarer rain in desert
      chance = (int)std::clamp((float)chance / std::max(0.10f, g_alea.rainChance * g_alea.chaos), 1.0f, 20000.0f);
      if (r.oneIn(chance)) toState(OVERCAST);
    } break;

    case OVERCAST: {
      // Overcast is grey sky, not drizzle: rain fades out here rather than
      // pre-loading to 0.35 (which made every shower arrive full-strength
      // and left permanent background rain — the "binary and heavy" feel).
      w.weather.rainStrength = std::max(0.f, w.weather.rainStrength - 0.006f);
      if (veryCloudy && w.weather.timer > 110 && r.oneIn(4)) toState(RAIN);
      if (!cloudy && w.weather.timer > 110 && r.oneIn(3)) toState(CLEAR);
      if (w.weather.timer > 500 && r.oneIn(3)) toState(CLEAR);
    } break;

    case RAIN: {
      // Roll in gently: ~2 minutes from first drops to full rain.
      float maxRain = (w.biome==ALPINE? 0.50f : (w.biome==MEADOW? 0.55f : (w.biome==DESERT? 0.18f : 1.0f)));
      w.weather.rainStrength = std::min(maxRain, w.weather.rainStrength + 0.0025f);
      int stormChance = (s==SUMMER ? 180 : 420);
      stormChance = (int)std::clamp((float)stormChance / std::max(0.05f, raininess), 90.0f, 25000.0f);
      if (w.biome==TROPICAL) stormChance = std::max(120, stormChance-80);
      if (w.biome==DESERT) stormChance = std::max(8000, stormChance+6000); // basically no storms
      if (w.weather.timer > 120 && r.oneIn(stormChance)) toState(STORM);
      if (w.weather.timer > 260 && r.oneIn(5)) toState(OVERCAST);
      if (w.weather.timer > 700) toState(OVERCAST);
    } break;

    case STORM: {
      float maxRain = (w.biome==ALPINE? 0.60f : (w.biome==MEADOW? 0.65f : (w.biome==DESERT? 0.22f : 1.0f)));
      w.weather.rainStrength = std::min(maxRain, w.weather.rainStrength + 0.008f);
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

void seedWorld(World& w, Rng& r, Biome biome) {
  
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
// How much of the world is underwater right now (0..1).
static float wetFraction(const World& w) {
  int wet = 0;
  for (int y=0;y<H;++y) for (int x=0;x<W;++x) if (w.water[y][x] > 0) ++wet;
  return (float)wet / (float)(W*H);
}

// The wet fraction each biome should settle around. A kiosk vat runs for
// days: without a target, any tiny source/sink imbalance eventually turns
// the whole world to ocean (or desert).
static float biomeWetTarget(Biome b) {
  switch (b) {
    case WETLAND:  return 0.45f;
    case TROPICAL: return 0.30f;
    case ALIEN:    return 0.28f;
    case DESERT:   return 0.08f;
    case ALPINE:   return 0.18f;
    default:       return 0.22f;  // meadow
  }
}

static void stepWater(World& w, Rng& r) {
  Water next = w.water;

  // Homeostat, source side: springs throttle as the world exceeds its
  // biome's wet target (sinks strengthen in waterSinks — the two together
  // guarantee an equilibrium instead of hoping the tuning balances).
  const float wetOver = wetFraction(w) - biomeWetTarget(w.biome);

  // Persistent springs: keep rivers/lakes alive long-term.
  if (!w.springs.empty()) {
    for (auto &p : w.springs) {
      if (wetOver > 0.f && r.u01() < std::min(0.95f, wetOver * 6.f)) continue;
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

  // Homeostat, sink side (see stepWater): the wetter the world is beyond
  // its biome's target, the harder evaporation/infiltration/drainage pull.
  {
    float over = wetFraction(w) - biomeWetTarget(w.biome);
    if (over > 0.f) {
      float boost = 1.0f + 14.0f * over;
      evap *= boost;
      infil *= boost;
      edgeDrain *= boost;
    }
  }

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

  // Fire is rare; skip the per-cell ignite neighbor scan entirely on the
  // (vast majority of) ticks where nothing is burning.
  bool anyFire = false;
  for (int y=0; y<H && !anyFire; ++y)
    anyFire = (w.terrain[y].find('*') != std::string::npos);

  for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
    if (w.water[y][x] > 0) {
      if (w.terrain[y][x] == '*') next[y][x] = 'x';

      // Kelp dies back rarely so beds stay sparse instead of carpeting the
      // whole lake over kiosk timescales (it had no removal path at all).
      if (w.terrain[y][x] == KELP_GLYPH && r.oneIn(700)) next[y][x] = '.';

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

    // Beached kelp (water receded) dries out quickly.
    if (c == KELP_GLYPH) {
      if (r.oneIn(12)) next[y][x] = (r.oneIn(2) ? ',' : '.');
      continue;
    }

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

    if (anyFire && isVeg(c)) {
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

    // One pass over the 8 neighbors, bucketing by glyph. This used to be 12
    // separate countNeighborsChar() calls (~96 probes per cell) and dominated
    // the whole tick (~87% of step() in gprof).
    int wet=0, g=0, tg=0, sh=0, tr=0, flo=0, fern=0, reeds=0;
    for (int dy=-1; dy<=1; ++dy) for (int dx=-1; dx<=1; ++dx) {
      if (dx==0 && dy==0) continue;
      int nx=x+dx, ny=y+dy;
      if (!inBounds(nx,ny)) continue;
      if (w.water[ny][nx] > 0) wet++;
      switch (w.terrain[ny][nx]) {
        case ',': g++; break;
        case '"': tg++; break;
        case '#': sh++; break;
        case 'T': case 'Y': case 'P': tr++; break;
        case 'f': case '+': case '&': case '!': case '$': flo++; break;
        case ';': fern++; break;
        case ':': reeds++; break;
        default: break;
      }
    }

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
      // Fruit ripens and falls year-round (was winter-only decay — fine
      // with 3-minute seasons, but with day-long seasons a whole summer of
      // unchecked spread turned the land yellow).
      if (s==SUMMER && wet>0 && r.oneIn(900)) {
        int nx=x+r.i(-1,1), ny=y+r.i(-1,1);
        if (inBounds(nx,ny) && w.water[ny][nx]==0 && (w.terrain[ny][nx]==',' || w.terrain[ny][nx]==';')) next[ny][nx]='$';
      }
      if (r.oneIn(350)) next[y][x]='#';
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
      if (s==SUMMER && r.oneIn(2000) && w.biome!=ALPINE) next[y][x]='$';
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
      // Old age takes a tree now and then even on wet ground — in wetland
      // the dry-ground path above never fires, so forests only ever grew.
      if (r.oneIn(9000)) next[y][x]='#';
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

float clamp01(float v){ return v<0.f?0.f:(v>1.f?1.f:v); }

static void applyRippleChaos(World& w, Rng& r, int tick) {
  if (g_ripples.empty()) return;

  for (const auto& rp : g_ripples) {
    float ring = rp.speed * rp.t;

    const float chaos = rp.chaos * g_alea.chaos;
    for (auto& agent : w.agents) {
      float rx = float(agent.x - rp.cx);
      float ry = float(agent.y - rp.cy);
      float dist = std::sqrt(rx * rx + ry * ry);
      float delta = std::fabs(dist - ring);
      if (delta > rp.width) continue;

      float strength =
          (1.0f - delta / std::max(0.001f, rp.width)) * rp.amp;
      if (dist < 0.001f) {
        rx = 1.0f;
        ry = 0.0f;
        dist = 1.0f;
      }

      float nx = rx / dist;
      float ny = ry / dist;
      int push = (strength > 1.6f) ? 2 : 1;
      int nextX = clampi(agent.x + (int)std::lround(nx * push), 0, W - 1);
      int nextY = clampi(agent.y + (int)std::lround(ny * push), 0, H - 1);

      if (!isAquatic(agent.glyph) && w.water[nextY][nextX] > 5) {
        if (std::fabs(nx) > std::fabs(ny)) {
          nextY = clampi(nextY + (r.oneIn(2) ? 1 : -1), 0, H - 1);
        } else {
          nextX = clampi(nextX + (r.oneIn(2) ? 1 : -1), 0, W - 1);
        }
      }

      agent.x = nextX;
      agent.y = nextY;
      agent.stress = clamp01(agent.stress + 0.10f * strength * chaos);
      if (r.u01() < 0.10f * chaos) agent.flags |= AGF_PANIC;
      if (r.u01() < 0.08f * chaos) {
        agent.hunger = clamp01(agent.hunger + 0.05f * chaos);
        agent.thirst = clamp01(agent.thirst + 0.05f * chaos);
      }
    }

    for (int i = 0; i < 10; ++i) {
      float angle = float(r.u01() * 6.2831853);
      float radius = ring + (r.u01() * 2.0f - 1.0f) * rp.width;
      int x = rp.cx + (int)std::lround(std::cos(angle) * radius);
      int y = rp.cy + (int)std::lround(std::sin(angle) * radius);
      if (!inBounds(x, y)) continue;

      float p = rp.chaos * g_alea.chaos;
      if (r.u01() < 0.25f * p) {
        int depth = (int)w.water[y][x];
        depth += (r.oneIn(2) ? 1 : -1);
        w.water[y][x] = (uint8_t)clampi(depth, 0, 7);
      }
      if (r.u01() < 0.18f * p) {
        static const char kOverlayChoices[] = {'~', '`', '*', '+', ';', '"', ':', '.'};
        w.overlay[y][x] = kOverlayChoices[r.irange(0, (int)(sizeof(kOverlayChoices) - 1))];
      }
      if (r.u01() < 0.08f * p) {
        char& terrain = w.terrain[y][x];
        if (terrain == '.') terrain = ',';
        else if (terrain == ',') terrain = '"';
        else if (terrain == '"') terrain = ';';
        else if (terrain == ';') terrain = '.';
      }
    }
  }

  (void)tick;
}


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
  int best = radius + 1;
  for (const auto &p : w.agents) {
    if (!isPredator(p.glyph)) continue;
    int dx = p.x - x;
    int dy = p.y - y;
    int d = std::abs(dx) + std::abs(dy);
    if (d > radius) continue;
    if (d < best) best = d;
  }
  return best;
}

static inline void moveRandom(Rng& r,int &x,int &y){
  static const int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
  int k=r.irange(0,7);
  x+=dirs[k][0]; y+=dirs[k][1];
}
// Occasional immigration keeps the ecosystem alive: without it the only
// agents that ever exist are the legendary couple (deaths are never replaced).
static void maybeImmigrateAgents(World& w, Rng& r) {
  const int cap = (W * H) / 300;
  if ((int)w.agents.size() >= cap) return;

  int denom = (int)std::max(2.0f, 8.0f / std::max(0.1f, g_alea.spawnChance));
  if (!r.oneIn(denom)) return;

  // Pick a behavior glyph: mostly grazers, some birds, a few predators,
  // fish when we land on water.
  char glyph = 'r';
  float u = r.u01();
  if (u < 0.15f) glyph = 'n';
  else if (u < 0.32f) glyph = 'v';

  for (int tries = 0; tries < 60; ++tries) {
    // Arrive from a world edge like a real migrant.
    int side = r.irange(0, 3);
    int x = (side == 0) ? 0 : (side == 1) ? W - 1 : r.irange(0, W - 1);
    int y = (side == 2) ? 0 : (side == 3) ? H - 1 : r.irange(0, H - 1);

    uint8_t depth = w.water[y][x];
    char resolved = glyph;
    if (depth > 2) resolved = (r.oneIn(2) ? '>' : '<');  // deep water: fish
    else if (depth > 0 && !isBird(glyph)) continue;      // shallows: birds only
    if (!cellPassableForAgent(w, x, y)) continue;

    Agent a;
    a.id = w.agents.empty() ? 1 : (w.agents.back().id + 1);
    a.x = x;
    a.y = y;
    a.glyph = resolved;
    a.species = pickBiomeSpecies(w.biome, r);
    a.hunger = r.u01() * 0.4f;
    a.thirst = r.u01() * 0.4f;
    a.fatigue = r.u01() * 0.3f;
    a.stress = r.u01() * 0.2f;
    a.health = 0.85f + r.u01() * 0.15f;
    w.agents.push_back(a);
    return;
  }
}

static void stepEntities(World& w, Rng& r, int tick) {
  // One-time migration: if agents list is empty, seed it from the existing entity glyph grid.
  if (w.agents.empty()) agentsInitFromGrid(w, r);

  // Update at a lower rate than the main sim tick to keep things cheap.
  // 10Hz-ish: every 6 ticks at 60 TPS, but still works at other TPS.
  bool doUpdate = (tick % 6) == 0;

  if (doUpdate) {
    maybeImmigrateAgents(w, r);
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

      // Night: calm creatures mostly sleep (rest recovers fatigue); anyone
      // fleeing, starving or parched still moves. Daylight ramps back in
      // smoothly, so activity fades rather than switching.
      {
        float dl = daylightNow(tick).level;
        bool urgent = (a.flags & 1) || a.hunger > 0.75f || a.thirst > 0.75f;
        if (!urgent && dl < 0.6f) {
          float restChance = (0.6f - dl) / 0.6f * 0.75f;  // up to 75% at full dark
          if (r.u01() < restChance) {
            a.intent = INTENT_WANDER;
            a.fatigue = clamp01(a.fatigue - 0.05f * dt);
            continue;
          }
        }
      }

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

    // Cull dead agents (and clear their glyph). The inspected agent is
    // tracked by index, so re-find it by id after the erase shifts indices.
    int inspectId = -1;
    if (g_inspectIdx >= 0 && g_inspectIdx < (int)w.agents.size())
      inspectId = w.agents[g_inspectIdx].id;
    w.agents.erase(std::remove_if(w.agents.begin(), w.agents.end(),
      [&](const Agent& a){ return a.health <= 0.01f; }), w.agents.end());
    if (inspectId >= 0) {
      g_inspectIdx = -1;
      for (int i = 0; i < (int)w.agents.size(); ++i) {
        if (w.agents[i].id == inspectId) { g_inspectIdx = i; break; }
      }
    }
  }

  agentsWriteToGrid(w, tick);
}

// big ancient tree anchors 'Q'
static void maybeSpawnAncientTree(World& w, Rng& r) {
  // Ancient trees were immortal — over kiosk timescales they slowly filled
  // the world. Cap the population and let each one very rarely fall,
  // leaving a mushroom ring on the forest floor.
  int count = 0;
  for (int y=0;y<H;++y) for (int x=0;x<W;++x) {
    if (w.entities[y][x] != 'Q') continue;
    ++count;
    if (r.oneIn(30000)) {
      w.entities[y][x] = ' ';
      w.terrain[y][x] = 'm';
      --count;
    }
  }
  if (count >= std::max(2, (W*H)/900)) return;
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
void step(World& w, Rng& r, std::string& banner, int tick) {
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
  stepEntities(w, r, tick);
  applyRippleChaos(w, r, tick);
  maybeSpawnAncientTree(w, r);

  applyRainOverlay(w, tick);
}
