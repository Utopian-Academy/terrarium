// Bridge between the plugin DSP (which owns and steps the World) and the
// plugin UI (which draws it). The DSP renders a 1px-per-cell RGBA frame
// after each sim tick — only while a UI is open — and the UI uploads it.
//
// This is a process-global, consistent with the engine's existing
// one-instance-per-process constraint (mod matrix / voices are globals too).
// It works in the single-binary formats (VST3, CLAP); LV2 splits DSP and UI
// into separate modules, so the LV2 UI stays dark for now.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct TerrariumViewBridge {
  std::mutex mutex;
  std::vector<uint32_t> pixels;  // RGBA8888, W*H, row-major, written post-tick
  std::atomic<bool> dirty{false};
  std::atomic<int> viewers{0};   // UIs currently open; 0 = skip rendering
  std::atomic<int> tick{0};
  std::atomic<int> biome{0};
  std::atomic<int> weather{0};
};

// C++17 inline: one instance per linked module. DSP and UI share it in the
// single-binary formats; LV2's separate UI module gets its own (unfed) copy.
inline TerrariumViewBridge g_terrariumView;
