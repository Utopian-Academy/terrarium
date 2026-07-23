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
  bool circle = false;  // mask to the inscribed circle (round LED panels)
  bool island = false;  // radial island worldgen with an ocean ring
  int driftMin = 0;     // voyage mode: pan to a new biome every N minutes
  uint32_t seed = 0;    // 0 = time-based
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
      o.tps = std::clamp(std::atoi(next()), 1, 60);
    } else if (a == "--seed") {
      o.seed = (uint32_t)std::strtoul(next(), nullptr, 0);
    } else if (a == "--fullscreen") {
      o.fullscreen = true;
    } else if (a == "--circle") {
      o.circle = true;
    } else if (a == "--island") {
      o.island = true;
    } else if (a == "--drift") {
      o.driftMin = std::clamp(std::atoi(next()), 0, 1440);
    } else if (a == "--weather") {
      g_weatherMode = (next() == std::string("live")) ? 1 : 0;
    } else if (a == "--daynight") {
      std::string m = next();
      g_daynightMode = (m == "clock") ? 2 : (m == "off") ? 0 : 1;
    } else if (a == "--seasons") {
      std::string m = next();
      g_seasonMode = (m == "daily") ? 1 : (m == "real") ? 2 : 0;
    }
  }
  return o;
}

// Cell colors live in terrarium_pixelview.hpp (shared with the plugin UI).
uint32_t cellColor(const World& w, int x, int y, int tick, float animT) {
  PixelviewRGB c = pixelviewCellColor(w, x, y, tick, animT);
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
  world.island = opt.island;
  seedWorld(world, rng, opt.biome);

  bool running = true, paused = false, dirty = true;
  int tick = 0, tps = opt.tps;
  std::string banner = "calm";
  Uint32 lastTickMs = SDL_GetTicks();

  // Voyage mode: every driftMin minutes the view pans to a freshly grown
  // world in another biome — the porthole becomes a ship's window.
  World nextWorld;
  int nextTick = 0;
  bool panning = false;
  float panStart = 0.f;
  Uint32 lastDriftMs = SDL_GetTicks();
  const float kPanSec = 28.f;

#ifndef _WIN32
  std::signal(SIGUSR1, onControlSignal);
  std::signal(SIGUSR2, onControlSignal);
#endif

  auto doTick = [&]() {
    step(world, rng, banner, tick);
    if (panning) {  // the incoming world lives too
      std::string b2;
      step(nextWorld, rng, b2, nextTick);
      nextTick = wrapTick(nextTick + 1);
    }
    g_stepEvents.clear();  // no audio consumer in the pico build
    tick = wrapTick(tick + 1);
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
        case SDLK_RIGHTBRACKET: if (tps < 60) ++tps; break;
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

    // Voyage: time to set sail for a new biome?
    if (opt.driftMin > 0 && !panning && !paused &&
        now - lastDriftMs >= (Uint32)opt.driftMin * 60000u) {
      static const struct { Biome b; bool isl; } stops[] = {
          {MEADOW, false},  {WETLAND, false},  {ALPINE, false},
          {ALIEN, false},   {TROPICAL, false}, {DESERT, false},
          {TROPICAL, true},  // a new island
      };
      int pick;
      do {
        pick = (int)(hash3(++seed, 0xD21F7u, 0x5A11u) % 7u);
      } while (stops[pick].b == world.biome && stops[pick].isl == world.island);
      nextWorld = World{};
      nextWorld.island = stops[pick].isl;
      rng = Rng(++seed);
      seedWorld(nextWorld, rng, stops[pick].b);
      nextTick = 0;
      panning = true;
      panStart = (float)now * 0.001f;
    }

    // Water motion and fireflies run on wall-clock time: repaint ~12fps
    // between sim ticks when either is visible. Stillwater biomes keep the
    // tick-only redraw the Pi Zero 1 needs — except on firefly nights.
    static Uint32 lastAnimMs = 0;
    // Every biome now has day and night ambient life (butterflies, surf,
    // fireflies, aurora, shimmer...), so always repaint ~12fps. (A Pi
    // Zero 1 build could re-gate this; the Zero 2 kiosk doesn't blink.)
    if (!paused && now - lastAnimMs >= 85) {
      lastAnimMs = now;
      dirty = true;
    }

#ifdef TERRA_PICO_PROF
    double profT2 = profClock();
    profTickMs += profT2 - profT1;
#endif

    if (dirty) {
      void* pixels = nullptr;
      int pitch = 0;
      if (SDL_LockTexture(frame, nullptr, &pixels, &pitch) == 0) {
        float animT = (float)SDL_GetTicks() * 0.001f;
        // Ken Burns: in voyage mode the camera slowly pushes in and glides,
        // easing to a new framing every 75s (documentary about a tiny world).
        float kbZ = 1.f, kbCx = (float)W * 0.5f, kbCy = (float)H * 0.5f;
        if (opt.driftMin > 0) {
          auto kbTarget = [](uint32_t e, float& cx2, float& cy2, float& zz) {
            uint32_t hh = hash3(e, 0x6B454Eu, 0xB52u);
            zz = 1.06f + 0.22f * (float)((hh >> 4) & 255u) / 255.f;
            float span = (float)W * (1.f - 1.f / zz);
            cx2 = (float)W * 0.5f + ((float)((hh >> 12) & 255u) / 255.f - 0.5f) * span;
            cy2 = (float)H * 0.5f + ((float)((hh >> 20) & 255u) / 255.f - 0.5f) * span;
          };
          float kbT = animT / 75.f;
          uint32_t ke = (uint32_t)kbT;
          float kf = kbT - (float)ke;
          kf = kf * kf * (3.f - 2.f * kf);
          float ax, ay, az, bx2, by2, bz2;
          kbTarget(ke, ax, ay, az);
          kbTarget(ke + 1u, bx2, by2, bz2);
          kbCx = ax + (bx2 - ax) * kf;
          kbCy = ay + (by2 - ay) * kf;
          kbZ = az + (bz2 - az) * kf;
        }
        // Voyage pan: the old world slides out west as the new one arrives.
        int panOff = 0;
        if (panning) {
          float p = (animT - panStart) / kPanSec;
          if (p >= 1.f) {
            world = std::move(nextWorld);
            tick = nextTick;
            panning = false;
            lastDriftMs = SDL_GetTicks();
          } else {
            float e = p * p * (3.f - 2.f * p);  // ease the crossing
            panOff = (int)(e * (float)W);
          }
        }
        // Round panel mask: cells outside the inscribed circle stay black
        // (soft 1px edge so the boundary doesn't stair-step harshly).
        const float cc = (float)W * 0.5f - 0.5f;
        const float rad = (float)W * 0.5f;
        for (int y = 0; y < H; ++y) {
          uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
          for (int x = 0; x < W; ++x) {
            int srcYkb = y;
            bool useKbY = false;
            (void)useKbY;
            int srcX = x + panOff;
            const World& srcW = (srcX < W) ? world : nextWorld;
            int srcT = (srcX < W) ? tick : nextTick;
            if (srcX >= W) srcX -= W;
            if (kbZ != 1.f) {  // ken burns warp within the source world
              float wxf = kbCx + ((float)srcX - (float)W * 0.5f) / kbZ;
              float wyf = kbCy + ((float)y - (float)H * 0.5f) / kbZ;
              srcX = std::clamp((int)wxf, 0, W - 1);
              // note: y sampling handled via srcY below
              srcYkb = std::clamp((int)wyf, 0, H - 1);
              useKbY = true;
            }
            if (opt.circle) {
              float dx = (float)x - cc, dy = (float)y - cc;
              float dist = std::sqrt(dx * dx + dy * dy);
              if (dist > rad) { row[x] = 0xFF000000u; continue; }
              uint32_t px = cellColor(srcW, srcX, srcYkb, srcT, animT);
              if (dist > rad - 1.5f) {
                float f = (rad - dist) / 1.5f;
                uint32_t r8 = (uint32_t)(((px >> 16) & 0xFF) * f);
                uint32_t g8 = (uint32_t)(((px >> 8) & 0xFF) * f);
                uint32_t b8 = (uint32_t)((px & 0xFF) * f);
                px = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
              }
              row[x] = px;
            } else {
              row[x] = cellColor(srcW, srcX, srcYkb, srcT, animT);
            }
          }
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
