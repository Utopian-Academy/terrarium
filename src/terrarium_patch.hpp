#pragma once

// Patch persistence: the user's sonic "patch" — mod matrix slots, chaos
// weights, voice settings, mixer levels, root key + scale — saved to a small
// text file. Shared between the standalone app and the plugin build (both
// read the same default path), and deliberately SDL-free.

#include "terrarium_audio.hpp"

#include <string>

// Set by any edit that changes the patch; the app loop flushes it to disk.
inline bool g_patchDirty = false;

std::string defaultPatchPath();
bool savePatch(const std::string& path, int rootKey, int scaleType);
// Missing file is not an error (returns false, leaves defaults untouched).
bool loadPatch(const std::string& path, int& rootKey, int& scaleType);

// Emit changed mod-matrix MIDI CC values (DEST_MIDI_CC slots) to a sink.
void sendModMatrixMidi(MidiSink& midi);
