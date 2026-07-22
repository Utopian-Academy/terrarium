// terrarium-pico: a Raspberry-Pi-Zero-friendly build of the terrarium sim.
//
// The full app draws every world cell as an 8x8 glyph texture with two SDL
// draw calls per cell (~45k calls per frame at ~150 fps). That is far beyond
// what a 1GHz ARM11 can do. This build instead renders the whole world as a
// 200x200 pixel image (one pixel per cell) into a single streaming texture,
// and only re-renders when the simulation actually ticks.
//
// Build (part of the normal CMake build):
//   cmake -S . -B build && cmake --build build --target terrarium-pico
// On a Pi Zero (Raspberry Pi OS):
//   sudo apt install -y cmake g++ libsdl2-dev
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
//   ./build/terrarium-pico --scale 1 --tps 4
//
// Controls: SPACE pause, . step, [ ] speed, r reseed, b next biome, ESC quit.

#include "terrarium_core.hpp"
#include "terrarium_pixelview.hpp"

#include <SDL.h>

#ifndef _WIN32
// Kiosk remote control (no keyboard attached): SIGUSR1 = reseed,
// SIGUSR2 = next biome. Sent by the `vat` helper over SSH.
#include <csignal>
static volatile sig_atomic_t g_sigReseed = 0;
static volatile sig_atomic_t g_sigNextBiome = 0;
static void onControlSignal(int sig) {
  if (sig == SIGUSR1) g_sigReseed = 1;
  else g_sigNextBiome = 1;
}
#endif

namespace {

struct PicoOptions {
  Biome biome = MEADOW;
  int scale = 3;      // window = (W*scale) x (H*scale); use 1 on the Pi
  int tps = DEFAULT_TPS;
  bool fullscreen = false;
  uint32_t seed = 0;  // 0 = time-based
};

PicoOptions parseArgs(int argc, char** argv) {
  PicoOptions o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--biome") {
      std::string b = next();
      if (b == "wetland") o.biome = WETLAND;
      else if (b == "alpine") o.biome = ALPINE;
      else if (b == "alien") o.biome = ALIEN;
      else if (b == "tropical") o.biome = TROPICAL;
      else if (b == "desert") o.biome = DESERT;
    } else if (a == "--scale") {
      o.scale = std::clamp(std::atoi(next()), 1, 8);
    } else if (a == "--tps") {
      o.tps = std::clamp(std::atoi(next()), 1, 30);
    } else if (a == "--seed") {
      o.seed = (uint32_t)std::strtoul(next(), nullptr, 0);
    } else if (a == "--fullscreen") {
      o.fullscreen = true;
    }
  }
  return o;
}

// Cell colors live in terrarium_pixelview.hpp (shared with the plugin UI).
uint32_t cellColor(const World& w, int x, int y, int tick) {
  PixelviewRGB c = pixelviewCellColor(w, x, y, tick);
  return 0xFF000000u | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) |
         (uint32_t)c.b;  // SDL ARGB8888
}

}  // namespace

int main(int argc, char** argv) {
  PicoOptions opt = parseArgs(argc, argv);

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  Uint32 flags = SDL_WINDOW_SHOWN;
  if (opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_Window* win =
      SDL_CreateWindow("terrarium-pico", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, W * opt.scale, H * opt.scale, flags);
  if (!win) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // nearest: crisp pixels

  SDL_Texture* frame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, W, H);
  if (!frame) {
    std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  uint32_t seed = opt.seed ? opt.seed
                           : (uint32_t)std::chrono::high_resolution_clock::now()
                                 .time_since_epoch()
                                 .count();
  Rng rng(seed);
  World world;
  seedWorld(world, rng, opt.biome);

  bool running = true, paused = false, dirty = true;
  int tick = 0, tps = opt.tps;
  std::string banner = "calm";
  Uint32 lastTickMs = SDL_GetTicks();

#ifndef _WIN32
  std::signal(SIGUSR1, onControlSignal);
  std::signal(SIGUSR2, onControlSignal);
#endif

  auto doTick = [&]() {
    step(world, rng, banner, tick);
    g_stepEvents.clear();  // no audio consumer in the pico build
    ++tick;
    dirty = true;
  };

#ifdef TERRA_PICO_PROF
  double profPollMs = 0, profTickMs = 0, profRenderMs = 0, profDelayMs = 0;
  auto profClock = [] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
#endif

  while (running) {
#ifdef TERRA_PICO_PROF
    double profT0 = profClock();
#endif
#ifndef _WIN32
    if (g_sigReseed) {  // same as the 'r' key
      g_sigReseed = 0;
      rng = Rng(++seed);
      seedWorld(world, rng, world.biome);
      tick = 0;
      dirty = true;
    }
    if (g_sigNextBiome) {  // same as the 'b' key
      g_sigNextBiome = 0;
      Biome nb = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
      rng = Rng(++seed);
      seedWorld(world, rng, nb);
      tick = 0;
      dirty = true;
    }
#endif
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) running = false;
      if (ev.type != SDL_KEYDOWN) continue;
      switch (ev.key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_q: running = false; break;
        case SDLK_SPACE: paused = !paused; break;
        case SDLK_PERIOD: if (paused) doTick(); break;
        case SDLK_LEFTBRACKET: if (tps > 1) --tps; break;
        case SDLK_RIGHTBRACKET: if (tps < 30) ++tps; break;
        case SDLK_r:
          rng = Rng(++seed);
          seedWorld(world, rng, world.biome);
          tick = 0;
          dirty = true;
          break;
        case SDLK_b: {
          Biome nb = (Biome)(((int)world.biome + 1) % BIOME_COUNT);
          rng = Rng(++seed);
          seedWorld(world, rng, nb);
          tick = 0;
          dirty = true;
          break;
        }
        default: break;
      }
    }

#ifdef TERRA_PICO_PROF
    double profT1 = profClock();
    profPollMs += profT1 - profT0;
#endif

    Uint32 now = SDL_GetTicks();
    const Uint32 msPerTick = (Uint32)(1000 / std::max(1, tps));
    if (!paused && now - lastTickMs >= msPerTick) {
      lastTickMs = now;
      doTick();
    }

#ifdef TERRA_PICO_PROF
    double profT2 = profClock();
    profTickMs += profT2 - profT1;
#endif

    if (dirty) {
      void* pixels = nullptr;
      int pitch = 0;
      if (SDL_LockTexture(frame, nullptr, &pixels, &pitch) == 0) {
        for (int y = 0; y < H; ++y) {
          uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
          for (int x = 0; x < W; ++x) row[x] = cellColor(world, x, y, tick);
        }
        SDL_UnlockTexture(frame);
      }
      SDL_RenderClear(ren);
      if (opt.fullscreen) {
        // Fullscreen drives a specific physical panel: keep the world
        // unstretched at scale and pin it to the TOP-LEFT of the output
        // (the region the panel actually shows), rest stays black.
        SDL_Rect dst{0, 0, W * opt.scale, H * opt.scale};
        SDL_RenderCopy(ren, frame, nullptr, &dst);
      } else {
        SDL_RenderCopy(ren, frame, nullptr, nullptr);
      }
      SDL_RenderPresent(ren);
      dirty = false;

      char title[160];
      std::snprintf(title, sizeof(title),
                    "terrarium-pico | %s | tick %d | %s | tps %d%s",
                    biomeName(world.biome), tick, weatherName(world.weather.state),
                    tps, paused ? " | PAUSED" : "");
      SDL_SetWindowTitle(win, title);
    }

#ifdef TERRA_PICO_PROF
    double profT3 = profClock();
    profRenderMs += profT3 - profT2;
#endif

    // Sleep the rest of the tick away; the Pi has no cycles to burn on spin.
    SDL_Delay(paused ? 40 : 15);

#ifdef TERRA_PICO_PROF
    // Rough section accounting for perf debugging (enabled via
    // -DTERRA_PICO_PROF; not compiled into normal builds).
    profDelayMs += profClock() - profT3;
    static Uint32 profWindowStart = SDL_GetTicks();
    static int profLoops = 0, profTicks = tick;
    ++profLoops;
    Uint32 profNow = SDL_GetTicks();
    if (profNow - profWindowStart >= 2000) {
      std::fprintf(stderr,
                   "[prof] loops/s=%.1f ticks/s=%.1f poll=%.0fms tick=%.0fms "
                   "render=%.0fms delay=%.0fms (per window)\n",
                   profLoops * 1000.0f / (profNow - profWindowStart),
                   (tick - profTicks) * 1000.0f / (profNow - profWindowStart),
                   profPollMs, profTickMs, profRenderMs, profDelayMs);
      profWindowStart = profNow;
      profLoops = 0;
      profTicks = tick;
      profPollMs = profTickMs = profRenderMs = profDelayMs = 0;
    }
#endif
  }

  SDL_DestroyTexture(frame);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
