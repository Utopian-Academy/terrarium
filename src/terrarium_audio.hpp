#pragma once

#include "terrarium_core.hpp"

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
inline VoiceSettings g_voice[NUM_VOICES] = {
  {40, 88, 10, 0,0, 0, 1.0f}, // voice 0
  {40, 88, 12, 0,0, 0, 0.9f}, // voice 1
  {28, 72, 11, 0,0, 0, 0.8f}, // bass-ish voice 2
};

// Per-voice mixer (CC7 = fader, CC11 = animated expression)
inline int g_lastCC5[NUM_VOICES] = {-1, -1, -1};
inline int g_lastCC65[NUM_VOICES] = {-1, -1, -1};
inline int g_lastCC11[NUM_VOICES] = {-1, -1, -1};
inline int g_lastCC74[NUM_VOICES] = {-1, -1, -1};
inline int g_lastCC10[NUM_VOICES] = {-1, -1, -1};
inline float g_voiceFader[NUM_VOICES] = {1.f, 1.f, 1.f}; // 0..2 (1.0 = unity, >1 adds headroom via velocity gain)
inline float g_voiceAuto[NUM_VOICES] = {1.f, 1.f, 1.f}; // 0..1
inline bool g_voiceMute[NUM_VOICES] = {false, false, false};

inline float g_drumsFader = 1.f; // ch9
inline float g_drumsAuto = 1.f;
inline bool g_drumsMute = false;

// Animated (screen-driven) multipliers for note gating; multiplied into g_alea.noteLen/holdChance (user knobs)
inline float g_noteLenAutoMul = 1.0f;
inline float g_holdChanceAutoMul = 1.0f;

// Solo state for mixer page (-1 = none, 0..NUM_VOICES-1 voices, NUM_VOICES = drums)
inline int g_soloRow = -1;
inline float g_savedVoiceFader[NUM_VOICES] = {1.f, 1.f, 1.f};
inline float g_savedDrumsFader = 1.f;
inline bool g_savedVoiceMute[NUM_VOICES] = {false, false, false};
inline bool g_savedDrumsMute = false;


// --- Note gate / note-off scheduling (prevents stuck/overlong notes) ---




inline bool g_voiceProgManual[NUM_VOICES] = {false, false, false};

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


enum MidiParamId : uint8_t {
  MIDI_PARAM_WATER = 0,
  MIDI_PARAM_RAIN,
  MIDI_PARAM_WIND,
  MIDI_PARAM_SEASON,
  MIDI_PARAM_BIOME,
  MIDI_PARAM_FLORA,
  MIDI_PARAM_FAUNA,
  MIDI_PARAM_INSTR,
  MIDI_PARAM_AUTOKEY,
  MIDI_PARAM_AUTOSCALE,
  MIDI_PARAM_COUNT
};

struct MidiParam {
  MidiParamId id = MIDI_PARAM_WATER;
  const char* name = "";
  int cc = -1;
  float weight = 1.0f;
  float rawValue01 = 0.0f;
  float value01 = 0.0f;
  float lastSent01 = -1.0f;
};

struct TelemetrySnapshot {
  float windMag = 0.0f;
  float rain01 = 0.0f;
  float water01 = 0.0f;
  float flora01 = 0.0f;
  float fauna01 = 0.0f;
  float season01 = 0.0f;
  float biome01 = 0.0f;
};

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
  bool open(const std::string&, float, const std::string&, const std::string&){ enabled=false; return false; }
  void close() {}
  void noteOn(int,int,int) {}
  void noteOff(int,int,int=0) {}
  void cc(int,int,int) {}
#endif
};

int cc127f(float x);
void applyVoiceMixer(SynthOut& synth);
float getMidiParam01(const std::vector<MidiParam>& params, MidiParamId id, float def01);
void synthTickMusic(SynthOut& synth, const World& world, Rng& r, int tick,
                    int& heldNote, int& heldNote2, int& heldNote3,
                    int rootKey, ScaleType scaleType,
                    const std::vector<MidiParam>& params);
