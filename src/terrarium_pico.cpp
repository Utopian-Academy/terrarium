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
  // 60, not the shared DEFAULT_TPS of 6. That 6 is a GLYPH-app pace: there
  // you are reading a grid of characters and want to watch each change land.
  // At 1px/cell the sim is a moving image, and 6 ticks/s reads as a slideshow
  // — water steps, agents teleport. The glyph app keeps 6.
  int tps = 60;
  bool fullscreen = false;
  bool circle = false;  // mask to the inscribed circle (round LED panels)
  bool island = false;  // radial island worldgen with an ocean ring
  int driftMin = 0;     // voyage mode: pan to a new biome every N minutes
  uint32_t seed = 0;    // 0 = time-based
  // Round-panel alignment. The world is compiled at W x H; `panel` is how
  // many of those cells the physical disc actually shows, taken as a centred
  // crop and blitted at (panelX, panelY). Build the world a few cells larger
  // than the panel and these can be nudged live — no rebuild to re-align.
  int panel = 0;        // 0 = use the full world
  int panelX = 0, panelY = 0;
  bool calibrate = false;  // draw the alignment target instead of the world
};

PicoOptions parseArgs(int argc, char** argv) {
  PicoOptions o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (a == "--biome") {
      // Matched against biomeName() rather than a hand-written chain. The
      // chain silently fell through to MEADOW for anything it did not list,
      // so adding SKY to the enum produced a kiosk that accepted
      // `--biome sky` without complaint and then rendered a meadow — with
      // rivers and flowers in it. An unknown name is now an ERROR, because
      // a typo you cannot see on a wall-mounted panel is worse than a exit.
      std::string b = next();
      bool found = false;
      for (int i = 0; i < BIOME_COUNT; ++i) {
        if (b == biomeName((Biome)i)) { o.biome = (Biome)i; found = true; break; }
      }
      if (!found) {
        std::fprintf(stderr, "unknown biome '%s'. known:", b.c_str());
        for (int i = 0; i < BIOME_COUNT; ++i)
          std::fprintf(stderr, " %s", biomeName((Biome)i));
        std::fprintf(stderr, "\n");
        std::exit(2);
      }
    } else if (a == "--scale") {
      o.scale = std::clamp(std::atoi(next()), 1, 8);
    } else if (a == "--tps") {
      // Headroom well past 60. Whether the machine can actually SUSTAIN it is
      // a separate question — the loop only steps when the interval has
      // elapsed, so asking for more than the hardware can do costs nothing
      // and simply runs at whatever it manages.
      o.tps = std::clamp(std::atoi(next()), 1, 240);
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
    } else if (a == "--panel") {
      o.panel = std::clamp(std::atoi(next()), 8, std::min(W, H));
    } else if (a == "--panel-x") {
      o.panelX = std::atoi(next());
    } else if (a == "--panel-y") {
      o.panelY = std::atoi(next());
    } else if (a == "--calibrate") {
      o.calibrate = true;
      o.circle = true;
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

// Alignment target for a round panel whose true LED count nobody knows.
// Photograph the panel with this up and read it off:
//   * white outer ring  = the LAST row of LEDs the world will ever light.
//     If a dark ring of LEDs sits outside it, --panel is too small.
//   * arms mark the cardinal extremes: RED right, GREEN left, BLUE top,
//     YELLOW bottom. An arm short of the rim on one side only means the
//     image is off-centre — nudge --panel-x / --panel-y.
//   * magenta pips step out from the centre every 10 cells, so a miss can
//     be counted rather than guessed.
uint32_t calibrationPixel(int x, int y, int cropX, int cropY, int n) {
  const float c = (float)(n - 1) * 0.5f;
  float px = (float)(x - cropX), py = (float)(y - cropY);
  if (px < 0.f || py < 0.f || px >= (float)n || py >= (float)n)
    return 0xFF000000u;
  float dx = px - c, dy = py - c;
  float dist = std::sqrt(dx * dx + dy * dy);
  const float rad = (float)n * 0.5f;
  if (dist > rad) return 0xFF000000u;

  int ix = (int)px, iy = (int)py;
  int ic = (int)c;
  bool onRow = (iy == ic) || (n % 2 == 0 && iy == ic + 1);
  bool onCol = (ix == ic) || (n % 2 == 0 && ix == ic + 1);

  // Cardinal arms: the outermost three cells along each axis.
  if (onRow && px >= (float)n - 3.f) return 0xFFFF2020u;  // right  red
  if (onRow && px <= 2.f)            return 0xFF20FF20u;  // left   green
  if (onCol && py <= 2.f)            return 0xFF4080FFu;  // top    blue
  if (onCol && py >= (float)n - 3.f) return 0xFFFFE020u;  // bottom yellow

  if (dist > rad - 1.f) return 0xFFFFFFFFu;               // outer ring

  // Decade pips out from the centre, both axes.
  if (onRow || onCol) {
    float along = onRow ? std::fabs(dx) : std::fabs(dy);
    int step = (int)(along + 0.5f);
    if (step > 0 && step % 10 == 0) return 0xFFFF40FFu;   // magenta
    return 0xFF303030u;                                   // dim crosshair
  }
  // Faint grid every 10 cells so the eye can walk the count.
  if ((ix - ic) % 10 == 0 && (iy - ic) % 10 == 0) return 0xFF202020u;
  return 0xFF000000u;
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
  // Windowed size follows the panel crop, so a desktop preview shows exactly
  // what the disc shows.
  const int winCells =
      std::clamp(opt.panel > 0 ? opt.panel : std::min(W, H), 8, std::min(W, H));
  SDL_Window* win =
      SDL_CreateWindow("terrarium-pico", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, winCells * opt.scale,
                       winCells * opt.scale, flags);
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
        case SDLK_RIGHTBRACKET: if (tps < 240) ++tps; break;
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
          {CITY, false},     // a harbour city
          {OCEAN, false},    // the open sea
          {SKY, false},      // and up, into the weather
          {TROPICAL, true},  // a new island
      };
      constexpr int kStops = (int)(sizeof(stops) / sizeof(stops[0]));
      int pick;
      do {
        pick = (int)(hash3(++seed, 0xD21F7u, 0x5A11u) % (uint32_t)kStops);
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
      // Panel geometry, re-read every frame: ~/.terrarium-panel lets the
      // kiosk be aligned against the live disc (a diameter there takes over
      // the whole geometry, offsets included).
      PanelGeom pg = displayPanel();
      const int panelN =
          std::clamp(pg.diameter > 0 ? pg.diameter
                                     : (opt.panel > 0 ? opt.panel : std::min(W, H)),
                     8, std::min(W, H));
      const int panelX = (pg.diameter > 0) ? pg.offX : opt.panelX;
      const int panelY = (pg.diameter > 0) ? pg.offY : opt.panelY;
      const int cropX = (W - panelN) / 2;
      const int cropY = (H - panelN) / 2;

      void* pixels = nullptr;
      int pitch = 0;
      if (SDL_LockTexture(frame, nullptr, &pixels, &pitch) == 0) {
        float animT = (float)SDL_GetTicks() * 0.001f;
        // The camera is ALWAYS 1:1. There used to be a gentle push-in across
        // the crossing — a nice idea on a screen, a bad one here. A 1.10x
        // zoom on a 140-cell panel lands every cell between two LEDs, so all
        // 28 seconds of every crossing came out soft, and it read as the
        // whole picture drifting out of focus and then recovering. Which is
        // precisely what it was doing. The dissolve below IS the crossing; it
        // does not need help from a lens. No resampling happens anywhere in
        // this path now, so the softness cannot come back.
        // Voyage crossing progress, 0..1 (see the dissolve below).
        float panT = 0.f;
        if (panning) {
          float p = (animT - panStart) / kPanSec;
          if (p >= 1.f) {
            world = std::move(nextWorld);
            tick = nextTick;
            panning = false;
            lastDriftMs = SDL_GetTicks();
          } else {
            panT = p * p * (3.f - 2.f * p);  // ease the crossing
          }
        }

        // Pass 1: render each visible world at crisp 1:1 into buffers.
        static std::vector<uint32_t> baseA((size_t)W * H), baseB((size_t)W * H);
        for (int y = 0; y < H; ++y)
          for (int x = 0; x < W; ++x)
            baseA[(size_t)y * W + x] = cellColor(world, x, y, tick, animT);
        if (panning)
          for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
              baseB[(size_t)y * W + x] = cellColor(nextWorld, x, y, nextTick, animT);

        // The disc only shows a `panelN`-cell square of the world, taken from
        // the middle; the circle mask is measured against THAT, not the
        // compiled world size, so a world built larger than the panel can be
        // aligned live (see resolvePanel above).
        const float cc = (float)cropX + (float)(panelN - 1) * 0.5f;
        const float rad = (float)panelN * 0.5f;
        for (int y = 0; y < H; ++y) {
          uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
          for (int x = 0; x < W; ++x) {
            uint32_t px;
            if (opt.calibrate) {
              px = calibrationPixel(x, y, cropX, cropY, panelN);
              row[x] = px;
              continue;
            }
            if (!panning) {
              px = baseA[(size_t)y * W + x];
            } else {
              // A DISSOLVE, not a wipe. The old crossing slid world B in
              // from the right, which put a hard vertical seam down the
              // middle of the disc — the one straight line in a world that
              // has none, travelling at constant speed. Instead both worlds
              // are rendered in place, each cell read straight out of its
              // own buffer, and a soft threshold sweeps across a noise
              // field: where the field is low the new world arrives first,
              // so the boundary is a ragged front that fingers and pools
              // like weather coming in. Each cell still crosses over exactly
              // once, and no cell crosses twice.
              uint32_t a2 = baseA[(size_t)y * W + x];
              uint32_t b3 = baseB[(size_t)y * W + x];
              // Two octaves so the front has both large lobes and a frayed
              // edge, plus a gentle left-to-right bias so the change still
              // reads as travelling rather than blinking.
              float n = pixelviewFbm2(x, y, 26.f, 0x7A0E1u);
              float bias = (float)x / (float)W;
              float field = n * 0.72f + bias * 0.28f;
              // The threshold oversteps 0..1 at both ends by the width of
              // the blend band, so the crossing genuinely finishes.
              const float kBand = 0.34f;
              float thr = panT * (1.f + 2.f * kBand) - kBand;
              float m = std::clamp((thr - field) / kBand + 0.5f, 0.f, 1.f);
              m = m * m * (3.f - 2.f * m);            // ease the handover
              auto mix = [&](int sh) {
                float av = (float)((a2 >> sh) & 0xFF);
                float bv = (float)((b3 >> sh) & 0xFF);
                return (uint32_t)(av + (bv - av) * m);
              };
              px = 0xFF000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
            }
            if (opt.circle) {
              float dx = (float)x - cc;
              float dy = (float)y - ((float)cropY + (float)(panelN - 1) * 0.5f);
              float dist = std::sqrt(dx * dx + dy * dy);
              if (dist > rad) { row[x] = 0xFF000000u; continue; }
              if (dist > rad - 1.5f) {
                float f = (rad - dist) / 1.5f;
                uint32_t r8 = (uint32_t)(((px >> 16) & 0xFF) * f);
                uint32_t g8 = (uint32_t)(((px >> 8) & 0xFF) * f);
                uint32_t b8 = (uint32_t)((px & 0xFF) * f);
                px = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
              }
            }
            row[x] = px;
          }
        }
        SDL_UnlockTexture(frame);
      }
      SDL_RenderClear(ren);
      SDL_Rect src{cropX, cropY, panelN, panelN};
      if (opt.fullscreen) {
        // Fullscreen drives a specific physical panel: blit the panel's
        // square of the world 1:1 at the offset the disc actually starts at
        // (default top-left), rest of the framebuffer stays black.
        SDL_Rect dst{panelX, panelY, panelN * opt.scale, panelN * opt.scale};
        SDL_RenderCopy(ren, frame, &src, &dst);
      } else {
        SDL_RenderCopy(ren, frame, &src, nullptr);
      }
      SDL_RenderPresent(ren);
      dirty = false;

      char title[200];
      if (opt.calibrate) {
        std::snprintf(title, sizeof(title),
                      "terrarium-pico CALIBRATE | world %dx%d | panel %d @ %d,%d",
                      W, H, panelN, panelX, panelY);
      } else {
        std::snprintf(title, sizeof(title),
                      "terrarium-pico | %s | tick %d | %s | tps %d | panel %d%s",
                      biomeName(world.biome), tick,
                      weatherName(world.weather.state), tps, panelN,
                      paused ? " | PAUSED" : "");
      }
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
