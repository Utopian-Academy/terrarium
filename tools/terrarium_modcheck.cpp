// Mod-matrix + musical identity check, headless.
//
// Two things it answers, per biome:
//   1. Do the modulation sources actually MOVE? A source pinned at its floor
//      for every biome is a dead knob in the matrix, and the only way to see
//      that is to run each world and look.
//   2. Does each biome have a distinct musical identity — rhythm pool,
//      register, phrase length, interval pool?
//
// The convention that makes this confusing (and why it is worth a harness):
// a post-pass maps every source 0..1 -> -1..+1, so a source AT REST READS
// -1.000, not 0. That is correct, not a dead source. This tool reports the
// raw pool value before that mapping.
//
//   terrarium-modcheck [--ticks N]

#define SDL_MAIN_HANDLED
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "terrarium_core.hpp"

int main(int argc, char** argv) {
  int ticks = 400;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc)
      ticks = std::atoi(argv[++i]);
  }

  std::printf("mod sources: %d\n\n", MOD_N);

  // Per-source min/max across every biome, so we can spot dead ones.
  std::vector<float> lo((size_t)MOD_N, 1e9f), hi((size_t)MOD_N, -1e9f);

  for (int b = 0; b < BIOME_COUNT; ++b) {
    Rng rng(4242u + (uint32_t)b * 31u);
    World w;
    seedWorld(w, rng, (Biome)b);
    std::string banner;
    for (int t = 0; t < ticks; ++t) {
      step(w, rng, banner, t);
      g_stepEvents.clear();
    }
    updateModPool(w, ticks, W, H);

    std::printf("=== %-9s ===\n", biomeName((Biome)b));
    // Only report the appended biome-identity block plus a few staples, or
    // the output is unreadable.
    static const int watch[] = {58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69};
    for (int k : watch) {
      if (k >= MOD_N) continue;
      std::printf("   %-14s %.3f\n", g_modName[k], g_modVal[k]);
    }
    for (int k = 0; k < MOD_N; ++k) {
      lo[(size_t)k] = std::min(lo[(size_t)k], g_modVal[k]);
      hi[(size_t)k] = std::max(hi[(size_t)k], g_modVal[k]);
    }
    std::printf("\n");
  }

  std::printf("=== sources that never move across any biome ===\n");
  int dead = 0;
  for (int k = 0; k < MOD_N; ++k) {
    if (hi[(size_t)k] - lo[(size_t)k] < 1e-6f) {
      std::printf("   [%2d] %-14s pinned at %.3f\n", k, g_modName[k],
                  lo[(size_t)k]);
      ++dead;
    }
  }
  if (!dead) std::printf("   (none)\n");
  std::printf("\n%d/%d sources vary by biome\n", MOD_N - dead, MOD_N);
  return 0;
}
