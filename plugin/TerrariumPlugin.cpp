// Terrarium as a plugin (CLAP / VST3 / LV2, via DPF): a MIDI generator that
// runs the ecosystem simulation on a sample clock inside the host and emits
// the same notes + CC streams as the standalone app. Route its MIDI output
// to any instrument (Serum, etc.) and MIDI-learn knobs from the mod matrix.
// The editor window shows the living world (see TerrariumUI.cpp) — that's
// the whole point: you watch the vat that's playing your synth.
//
// The plugin loads the same patch file the standalone edits
// (~/.config/terrarium/patch.txt), so you design the matrix in the app with
// its full UI, then perform with the plugin in a host.
//
// Known limitation: the engine uses shared globals (mod matrix, voices), so
// run ONE Terrarium instance per host process for now.

#include "terrarium_core.hpp"
#include "terrarium_audio.hpp"
#include "terrarium_app.hpp"
#include "terrarium_patch.hpp"
#include "terrarium_pixelview.hpp"

#include "TerrariumShared.hpp"

#include "DistrhoPlugin.hpp"

START_NAMESPACE_DISTRHO

// Queues engine MIDI (via g_midiMirror) for delivery into the host block.
struct HostMidiSink : MidiSink {
  struct Msg { uint8_t status, d1, d2; };
  std::vector<Msg> queue;

  void sendNoteOn(int ch, int note, int vel) override {
    queue.push_back({(uint8_t)(0x90 | (ch & 15)), (uint8_t)(note & 127),
                     (uint8_t)(vel & 127)});
  }
  void sendNoteOff(int ch, int note, int vel) override {
    queue.push_back({(uint8_t)(0x80 | (ch & 15)), (uint8_t)(note & 127),
                     (uint8_t)(vel & 127)});
  }
  void sendCC(int ch, int cc, int val) override {
    queue.push_back({(uint8_t)(0xB0 | (ch & 15)), (uint8_t)(cc & 127),
                     (uint8_t)(val & 127)});
  }
};

class TerrariumPlugin : public Plugin {
public:
  enum Params {
    kParamTps = 0,
    kParamBiome,
    kParamChaos,
    kParamRainChance,
    kParamSpawnChance,
    kParamMutation,
    kParamDrift,
    kParamNoteLen,
    kParamHoldChance,
    kParamRootKey,
    kParamScale,
    kParamReseed,
    kParamCount
  };

  TerrariumPlugin() : Plugin(kParamCount, 0, 0) {
    // Share the standalone's patch: mod matrix, chaos weights, voices, mixer.
    int rk = 0, st = (int)SCALE_PENTATONIC;
    loadPatch(defaultPatchPath(), rk, st);
    rootKey_ = rk;
    scale_ = st;
    chaos_ = g_alea.chaos;
    rain_ = g_alea.rainChance;
    spawn_ = g_alea.spawnChance;
    mutation_ = g_alea.mutationRate;
    drift_ = g_alea.drift;
    noteLen_ = g_alea.noteLen;
    hold_ = g_alea.holdChance;

    params_ = makeDefaultMidiParams();
    reseed();
  }

  ~TerrariumPlugin() override {
    if (g_midiMirror == &sink_) g_midiMirror = nullptr;
  }

protected:
  const char* getLabel() const override { return "Terrarium"; }
  const char* getDescription() const override {
    return "Ecosystem simulation MIDI generator: the vat plays your synth.";
  }
  const char* getMaker() const override { return "Utopian Academy"; }
  const char* getLicense() const override { return "ISC"; }
  uint32_t getVersion() const override { return d_version(0, 7, 1); }
  int64_t getUniqueId() const override { return d_cconst('T', 'e', 'r', 'a'); }

  void initParameter(uint32_t index, Parameter& parameter) override {
    parameter.hints = kParameterIsAutomatable;
    switch (index) {
      case kParamTps:
        parameter.name = "Sim speed";
        parameter.symbol = "tps";
        parameter.hints |= kParameterIsInteger;
        parameter.ranges.def = DEFAULT_TPS;
        parameter.ranges.min = 1.0f;
        parameter.ranges.max = 30.0f;
        break;
      case kParamBiome:
        parameter.name = "Biome";
        parameter.symbol = "biome";
        parameter.hints |= kParameterIsInteger;
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = (float)(BIOME_COUNT - 1);
        break;
      case kParamChaos:
        parameter.name = "Chaos";
        parameter.symbol = "chaos";
        parameter.ranges.def = chaos_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 2.0f;
        break;
      case kParamRainChance:
        parameter.name = "Rain chance";
        parameter.symbol = "rain";
        parameter.ranges.def = rain_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 2.0f;
        break;
      case kParamSpawnChance:
        parameter.name = "Spawn chance";
        parameter.symbol = "spawn";
        parameter.ranges.def = spawn_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 2.0f;
        break;
      case kParamMutation:
        parameter.name = "Mutation";
        parameter.symbol = "mutation";
        parameter.ranges.def = mutation_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 2.0f;
        break;
      case kParamDrift:
        parameter.name = "Drift";
        parameter.symbol = "drift";
        parameter.ranges.def = drift_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 2.0f;
        break;
      case kParamNoteLen:
        parameter.name = "Note length";
        parameter.symbol = "notelen";
        parameter.ranges.def = noteLen_;
        parameter.ranges.min = 0.10f;
        parameter.ranges.max = 2.50f;
        break;
      case kParamHoldChance:
        parameter.name = "Hold chance";
        parameter.symbol = "hold";
        parameter.ranges.def = hold_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 0.50f;
        break;
      case kParamRootKey:
        parameter.name = "Root key";
        parameter.symbol = "rootkey";
        parameter.hints |= kParameterIsInteger;
        parameter.ranges.def = (float)rootKey_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 11.0f;
        break;
      case kParamScale:
        parameter.name = "Scale";
        parameter.symbol = "scale";
        parameter.hints |= kParameterIsInteger;
        parameter.ranges.def = (float)scale_;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 6.0f;
        break;
      case kParamReseed:
        parameter.name = "Reseed";
        parameter.symbol = "reseed";
        parameter.hints |= kParameterIsTrigger;
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1.0f;
        break;
    }
  }

  float getParameterValue(uint32_t index) const override {
    switch (index) {
      case kParamTps: return (float)tps_;
      case kParamBiome: return (float)biome_;
      case kParamChaos: return chaos_;
      case kParamRainChance: return rain_;
      case kParamSpawnChance: return spawn_;
      case kParamMutation: return mutation_;
      case kParamDrift: return drift_;
      case kParamNoteLen: return noteLen_;
      case kParamHoldChance: return hold_;
      case kParamRootKey: return (float)rootKey_;
      case kParamScale: return (float)scale_;
      case kParamReseed: return 0.0f;
      default: return 0.0f;
    }
  }

  void setParameterValue(uint32_t index, float value) override {
    switch (index) {
      case kParamTps: tps_ = clampi((int)(value + 0.5f), 1, 30); break;
      case kParamBiome: {
        int b = clampi((int)(value + 0.5f), 0, BIOME_COUNT - 1);
        if (b != biome_) {
          biome_ = b;
          reseed();
        }
        break;
      }
      case kParamChaos: chaos_ = g_alea.chaos = value; break;
      case kParamRainChance: rain_ = g_alea.rainChance = value; break;
      case kParamSpawnChance: spawn_ = g_alea.spawnChance = value; break;
      case kParamMutation: mutation_ = g_alea.mutationRate = value; break;
      case kParamDrift: drift_ = g_alea.drift = value; break;
      case kParamNoteLen: noteLen_ = g_alea.noteLen = value; break;
      case kParamHoldChance: hold_ = g_alea.holdChance = value; break;
      case kParamRootKey: rootKey_ = clampi((int)(value + 0.5f), 0, 11); break;
      case kParamScale: scale_ = clampi((int)(value + 0.5f), 0, 6); break;
      case kParamReseed:
        if (value > 0.5f) reseed();
        break;
    }
  }

  void activate() override {
    sink_.enabled = true;
    g_midiMirror = &sink_;
    samplesUntilTick_ = 0.0;
  }

  void deactivate() override {
    if (g_midiMirror == &sink_) g_midiMirror = nullptr;
    sink_.enabled = false;
  }

  void run(const float**, float** outputs, uint32_t frames) override {
    // This plugin makes no sound of its own — outputs stay silent.
    std::memset(outputs[0], 0, sizeof(float) * frames);
    std::memset(outputs[1], 0, sizeof(float) * frames);

    if (g_midiMirror != &sink_) {  // another instance took the globals
      g_midiMirror = &sink_;
    }

    const double samplesPerTick = getSampleRate() / (double)std::max(1, tps_);
    uint32_t done = 0;
    while (done < frames) {
      if (samplesUntilTick_ <= 0.0) {
        doTick(done);
        samplesUntilTick_ += samplesPerTick;
      }
      const uint32_t step = (uint32_t)std::min(
          (double)(frames - done), std::max(1.0, std::ceil(samplesUntilTick_)));
      samplesUntilTick_ -= (double)step;
      done += step;
    }
  }

private:
  void reseed() {
    seed_ = seed_ * 6364136223846793005ull + 1442695040888963407ull;
    rng_ = Rng((uint32_t)(seed_ >> 32));
    seedWorld(world_, rng_, (Biome)biome_);
    tick_ = 0;
    heldNote_ = heldNote2_ = heldNote3_ = -1;
  }

  void doTick(uint32_t frameOffset) {
    step(world_, rng_, banner_, tick_);
    ++tick_;

    updateModPool(world_, tick_, W, H);
    applyModMatrix();
    telemetry_ = collectTelemetry(world_, tick_);
    updateTelemetryParams(params_, telemetry_);

    synthTickMusic(silentSynth_, world_, rng_, tick_, heldNote_, heldNote2_,
                   heldNote3_, rootKey_, (ScaleType)scale_, params_);
    g_stepEvents.clear();
    sendModMatrixMidi(sink_);

    // Feed the UI (1px per cell, same renderer as terrarium-pico). Skipped
    // entirely while no editor is open; ~200x200 cheap cells when one is.
    if (g_terrariumView.viewers.load(std::memory_order_acquire) > 0) {
      std::lock_guard<std::mutex> lk(g_terrariumView.mutex);
      auto& px = g_terrariumView.pixels;
      px.resize((size_t)W * H);
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          PixelviewRGB c = pixelviewCellColor(world_, x, y, tick_);
          px[(size_t)y * W + x] = (uint32_t)c.r | ((uint32_t)c.g << 8) |
                                  ((uint32_t)c.b << 16) | 0xFF000000u;
        }
      }
      g_terrariumView.tick.store(tick_, std::memory_order_relaxed);
      g_terrariumView.biome.store(biome_, std::memory_order_relaxed);
      g_terrariumView.weather.store((int)world_.weather.state,
                                    std::memory_order_relaxed);
      g_terrariumView.dirty.store(true, std::memory_order_release);
    }

    for (const auto& msg : sink_.queue) {
      MidiEvent ev;
      ev.frame = frameOffset;
      ev.size = 3;
      ev.data[0] = msg.status;
      ev.data[1] = msg.d1;
      ev.data[2] = msg.d2;
      writeMidiEvent(ev);
    }
    sink_.queue.clear();
  }

  World world_;
  Rng rng_{0xC0FFEEu};
  HostMidiSink sink_;
  SynthOut silentSynth_;  // stub build: never enabled, mirrors to sink
  std::vector<MidiParam> params_;
  TelemetrySnapshot telemetry_{};
  std::string banner_;
  uint64_t seed_ = 0x7E44A21ull;

  int tps_ = DEFAULT_TPS;
  int biome_ = 0;
  int rootKey_ = 0;
  int scale_ = (int)SCALE_PENTATONIC;
  float chaos_ = 1.0f, rain_ = 1.0f, spawn_ = 1.0f, mutation_ = 1.0f,
        drift_ = 1.0f, noteLen_ = 1.0f, hold_ = 0.06f;

  int tick_ = 0;
  int heldNote_ = -1, heldNote2_ = -1, heldNote3_ = -1;
  double samplesUntilTick_ = 0.0;

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TerrariumPlugin)
};

Plugin* createPlugin() { return new TerrariumPlugin(); }

END_NAMESPACE_DISTRHO
