#pragma once

#include "terrarium_audio.hpp"

struct Layout;

inline constexpr int kMenuPageCount = 9;
inline constexpr int kChaosWeightRowCount = 7;
inline constexpr float kParamAdjustStep = 0.05f;
inline constexpr float kMixerMuteThreshold = 0.0001f;
inline constexpr uint32_t kMidiParamSendIntervalMs = 50;

void clampCameraToWorld();
void updateRipples(float dt);
std::string defaultSf2Path();

std::vector<MidiParam> makeDefaultMidiParams();
int menuSelectionCount(bool showMenu, int menuPage,
                       const std::vector<MidiParam>& params,
                       const World& world);
void cycleMenuSelection(int& menuSel, int maxSel, int delta);
void adjustChaosWeight(int menuSel, float delta);
void adjustVoiceSettings(int menuSel, int delta);
void adjustMixerLevel(SynthOut& synth, int menuSel, float delta);
void toggleMixerMuteSelection(SynthOut& synth, int menuSel);
void toggleMixerSoloSelection(SynthOut& synth, int menuSel);

void startBiomeMorph(World& world, Rng& r);
void stepSimulationOnce(World& world, Rng& r, std::string& banner, int& tick,
                        SynthOut& synth, int& heldNote, int& heldNote2,
                        int& heldNote3, int rootKey, ScaleType scaleType,
                        const std::vector<MidiParam>& params);
void advanceBiomeFade(World& world, Rng& r);
void followSelectedAgent(const World& world);
void handleWorldClick(World& world, Rng& r, const Layout& layout,
                      int mouseX, int mouseY);

std::string buildWindowTitle(const World& world, int tick, bool paused, int tps,
                             const std::string& banner);
TelemetrySnapshot collectTelemetry(const World& world, int tick);
void refreshMidiParamValues(std::vector<MidiParam>& params);
void updateTelemetryParams(std::vector<MidiParam>& params,
                           const TelemetrySnapshot& telemetry);
void sendChangedMidiParams(MidiOut& midi, std::vector<MidiParam>& params,
                           uint32_t nowMs, uint32_t& lastParamSendMs);
void pumpMidiClock(MidiOut& midi, bool midiClockOut, bool useSimClock,
                   const TelemetrySnapshot& telemetry, uint32_t nowMs,
                   uint32_t& lastClockMs);
