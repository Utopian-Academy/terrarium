#include "terrarium_audio.hpp"

int cc127f(float x){
  if (x < 0.f) x = 0.f;
  if (x > 1.f) x = 1.f;
  return (int)std::lround(x * 127.f);
}

// The note/CC engine also runs when only external MIDI is active, so
// Terrarium can "play" an external instrument with no internal synth at all.
static inline bool audioActive(const SynthOut& synth) {
  return synth.enabled || (g_midiMirror && g_midiMirror->enabled);
}

void applyVoiceMixer(SynthOut& synth){
  if (!audioActive(synth)) return;
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

float getMidiParam01(const std::vector<MidiParam>& params,
                     MidiParamId id, float def01) {
  const size_t index = static_cast<size_t>(id);
  if (index >= params.size()) {
    return def01;
  }
  return std::clamp(params[index].value01, 0.0f, 1.0f);
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
  // Keyed by agent id: indices shift when agents die, which used to attribute
  // one agent's motion to another for a tick.
  static std::unordered_map<int, std::pair<int,int>> s_prevPos;
  if (s_prevPos.size() > w.agents.size() * 2 + 16) s_prevPos.clear();

  for (size_t i=0;i<w.agents.size();++i) {
    const auto& a = w.agents[i];
    if (a.x < x0 || a.x >= x0+viewW || a.y < y0 || a.y >= y0+viewH) continue;
    agentsHere++;
    cx += (float)(a.x - x0) / (float)std::max(1, viewW-1);

    auto it = s_prevPos.find(a.id);
    if (it != s_prevPos.end()) {
      speedAccum += (float)(std::abs(a.x - it->second.first) +
                            std::abs(a.y - it->second.second));
    }
    s_prevPos[a.id] = {a.x, a.y};
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
  if (!audioActive(synth)) return;

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

  // Pan (subtle): center of activity in the view. When the mod matrix has an
  // enabled slot targeting pan, it owns CC10 — don't fight it here. Routed
  // through synth.cc() so it also mirrors to the external MIDI port.
  if ((tick & 3) == 0 && !modMapControls(DEST_PAN)) { // CC throttling
    int pan = (int)std::lround(std::clamp(m.centroidX01, 0.f, 1.f) * 127.f);
    for (int v=0; v<NUM_VOICES; ++v) synth.cc(v, 10, pan);
    synth.cc(9, 10, pan);
  }

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
  // A muted voice sends nothing — clamping to velocity 1 used to leak ghost
  // notes to external MIDI instruments.
  if (fader <= 0.0001f) return;
  vel = (int)std::lround((float)vel * std::clamp(fader, 0.f, 2.f));
  vel = std::clamp(vel, 1, 127);
  synth.noteOn(ch, note, vel);
  int dur = (durTicksOverride > 0) ? durTicksOverride : pickNoteDurationTicks(r);
  g_activeNotes[ch].note = note;
  g_activeNotes[ch].offTick = tick + dur;
  g_activeNotes[ch].on = true;
}


// ---- Built-in synth music driver ----
// Called whenever the simulation advances a tick (realtime or single-step).
void synthTickMusic(SynthOut& synth, const World& world, Rng& r, int tick,
                    int& heldNote, int& heldNote2, int& heldNote3,
                    int rootKey, ScaleType scaleType,
                    const std::vector<MidiParam>& params)
{
  if (!audioActive(synth)) return;
  // Always service scheduled note-offs first (prevents stuck/overlong notes)
  serviceScheduledNoteOffs(synth, tick);

  // Per-tick CC automation (expression/brightness/pan/portamento) driven by MODMAP
  // CC7 faders are set in applyVoiceMixer (menu changes); CC11 is animated here.
  // When the mod matrix targets CC11, it takes over expression outright
  // (otherwise it could only attenuate the animated automation value).
  const bool matrixOwnsExpr = modMapControls(DEST_CC11_EXPR);
  for (int v=0; v<NUM_VOICES; ++v){
    int ch=v;
    float autoPart = matrixOwnsExpr ? 1.0f : g_voiceAuto[v];
    float expr = std::clamp(g_voiceMute[v]?0.f:(autoPart*g_cc11Expr), 0.f, 1.f);
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

  // If the Instr knob has moved from near-zero, treat it as an explicit selection (global palette).
  float instr01 = getMidiParam01(params, MIDI_PARAM_INSTR, 0.0f);
  bool instrExplicit = instr01 > 0.01f;

  // Route program changes through g_voice[0].program so the cached per-voice
  // sender above stays the single writer of channel 0's program (previously
  // this sent its own program_change and the two caches could disagree).
  auto applyProgram = [&](int prog){
    if (prog != currentProgram) {
      g_voice[0].program = std::clamp(prog, 0, 127);
      currentProgram = prog;
    }
  };

  // Auto-switch rarely based on sim state, but only when not explicitly
  // overridden — by the Instr knob or by a manual V0 program edit.
  if (!instrExplicit && !g_voiceProgManual[0] && tick >= nextAutoChangeTick) {
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
static int lastHeld1 = -1;
static int holdUntil1 = 0, holdUntil2 = 0;

// Grab modulation params (already 0..1 and weight-scaled in the UI loop).
  float water01 = getMidiParam01(params, MIDI_PARAM_WATER, 0.0f);
  float rain01  = getMidiParam01(params, MIDI_PARAM_RAIN, 0.0f);
  float wind01  = getMidiParam01(params, MIDI_PARAM_WIND, 0.0f);
  float flora01 = getMidiParam01(params, MIDI_PARAM_FLORA, 0.0f);
  float fauna01 = getMidiParam01(params, MIDI_PARAM_FAUNA, 0.0f);


// Auto key/scale modulation (sim-derived).
  float autoKey01   = getMidiParam01(params, MIDI_PARAM_AUTOKEY, 1.0f);
  float autoScale01 = getMidiParam01(params, MIDI_PARAM_AUTOSCALE, 1.0f);

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
  auto pickPool = [&](const int* a, int /*an*/, const int* b, int /*bn*/, float t)->const int*{
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
  // City: short, even, insistent — a groove, with a busier variant.
  static const int CIT_A[] = {  6,  9, 12, 12,  9 };
  static const int CIT_B[] = {  6,  6,  9, 12, 15 };
  // Ocean: long swells and a lot of room between them.
  static const int OCE_A[] = { 36, 48, 60, 72 };
  static const int OCE_B[] = { 30, 36, 48, 90 };
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
    case CITY: {
      // Busier when the streets are busy: rush hour drives the variant.
      base = pickPool(CIT_A, (int)(sizeof(CIT_A)/sizeof(CIT_A[0])),
                      CIT_B, (int)(sizeof(CIT_B)/sizeof(CIT_B[0])),
                      std::clamp(0.30f + 0.50f*fastBias + 0.30f*diversity01, 0.0f, 1.0f));
      break;
    }
    case OCEAN: {
      base = pickPool(OCE_A, (int)(sizeof(OCE_A)/sizeof(OCE_A[0])),
                      OCE_B, (int)(sizeof(OCE_B)/sizeof(OCE_B[0])),
                      std::clamp(0.25f + 0.35f*wind01, 0.0f, 1.0f));
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
  else if (base==CIT_A) n=(int)(sizeof(CIT_A)/sizeof(CIT_A[0]));
  else if (base==CIT_B) n=(int)(sizeof(CIT_B)/sizeof(CIT_B[0]));
  else if (base==OCE_A) n=(int)(sizeof(OCE_A)/sizeof(OCE_A[0]));
  else if (base==OCE_B) n=(int)(sizeof(OCE_B)/sizeof(OCE_B[0]));
  else n=(int)(sizeof(ALI_B)/sizeof(ALI_B[0]));

  // Phrase length: biome-dependent.
  int minL=4,maxL=8;
  switch(world.biome){
    case ALPINE: minL=3; maxL=6; break; // sparse, short phrases
    case DESERT: minL=3; maxL=5; break;
    case WETLAND: minL=5; maxL=8; break;
    case TROPICAL: minL=6; maxL=8; break;
    case ALIEN: minL=5; maxL=8; break;
    case CITY: minL=6; maxL=8; break;   // long, looping phrases
    case OCEAN: minL=3; maxL=5; break;  // a few notes, then space
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
  else if (world.biome==CITY) repeatP = 0.66f;   // city pop is a groove
  else if (world.biome==OCEAN) repeatP = 0.42f;  // sets never repeat exactly

  for (int i=0;i<8;i++){
    int v = base[r.irange(0, n-1)];

    // Syncopation / odd accents.
    float sync = 0.08f + 0.22f*fauna01 + 0.18f*wind01 + 0.18f*(0.5f-std::fabs(lifePhase01-0.5f))*2.0f;
    if (world.biome==TROPICAL) sync += 0.12f;
    if (world.biome==WETLAND) sync += 0.06f;
    if (world.biome==ALPINE)  sync -= 0.04f;
    if (world.biome==DESERT)  sync -= 0.02f;
    if (world.biome==ALIEN)   sync += 0.16f;
    if (world.biome==CITY)    sync += 0.18f;   // off-beats are the point
    if (world.biome==OCEAN)   sync -= 0.05f;

    if (r.u01() < sync) v = base[r.irange(0, n-1)];

    // Occasional “rest cell” (extend gap) in calmer states or sparse biomes.
    float restP = 0.06f*(1.0f-activity);
    if (world.biome==ALPINE) restP += 0.10f;
    if (world.biome==DESERT) restP += 0.12f;
    if (world.biome==WETLAND) restP += 0.04f;
  if (world.biome==OCEAN) restP += 0.16f;   // mostly the sound of waiting
  if (world.biome==CITY) restP -= 0.02f;
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
  case CITY:     regBias = +3; break;  // bright electric piano register
  case OCEAN:    regBias = -8; break;  // deep, and a long way off
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
  // City: 9ths and 6ths, the intervals city pop leans on.
  static const int CIT[] = { -9,-7,-5,-4,-2,0,2,4,5,7,9,14 };
  // Ocean: wide, slow, mostly open fifths and octaves.
  static const int OCE[] = { -12,-7,-5,0,5,7,12 };

  const int* pool = MEA; int n = (int)(sizeof(MEA)/sizeof(MEA[0]));
  switch(world.biome){
    case WETLAND: pool=WET; n=(int)(sizeof(WET)/sizeof(WET[0])); break;
    case ALPINE:  pool=ALP; n=(int)(sizeof(ALP)/sizeof(ALP[0])); break;
    case TROPICAL:pool=TRO; n=(int)(sizeof(TRO)/sizeof(TRO[0])); break;
    case DESERT:  pool=DES; n=(int)(sizeof(DES)/sizeof(DES[0])); break;
    case ALIEN:   pool=ALI; n=(int)(sizeof(ALI)/sizeof(ALI[0])); break;
    case CITY:    pool=CIT; n=(int)(sizeof(CIT)/sizeof(CIT[0])); break;
    case OCEAN:   pool=OCE; n=(int)(sizeof(OCE)/sizeof(OCE[0])); break;
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
  case CITY:     spread += 2; break;
  case OCEAN:    spread += 4; break;
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
  case CITY:     sparkleP += 0.04f; break;   // neon glints
  case OCEAN:    sparkleP -= 0.02f; break;
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
  case CITY:     harmP += 0.22f; break;   // lush: it is nearly always a chord
  case OCEAN:    harmP += 0.12f; break;   // open, suspended
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
