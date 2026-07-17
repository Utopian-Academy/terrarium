#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// ---------------- Sim config ----------------
static constexpr int W = 200;   // suggested: 200
static constexpr int H = 112;   // suggested: 112 (approx 16:9)
static constexpr int DEFAULT_TPS = 4;   // ticks/sec (pastoral)

static constexpr int EVENT_CHANCE    = 1600; // 1 in N ticks
static constexpr int EVENT_INTENSITY = 220;  // event ops per event

using Grid = std::vector<std::string>;

static inline bool inBounds(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

struct Rng {
  std::mt19937 rng;
  explicit Rng(uint32_t seed) : rng(seed) {}
  int i(int a, int b) { std::uniform_int_distribution<int> d(a, b); return d(rng); }
  bool oneIn(int n) { return i(1, n) == 1; }
  double u01() { std::uniform_real_distribution<double> d(0.0, 1.0); return d(rng); }
};

static int countNeighbors(const Grid& g, int x, int y, char c) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx, ny = y + dy;
      if (inBounds(nx, ny) && g[ny][nx] == c) n++;
    }
  return n;
}

static bool isPlant(char c) {
  return (c == ',' || c == '"' || c == '#' || c == 'T' || c == 'm');
}
static bool isWalkable(char c) {
  return (c != '~' && c != '*');
}

// ---------------- World seeding ----------------
static void seedWorld(Grid& g, Rng& r) {
  g.assign(H, std::string(W, '.'));

  // A few ponds (scale with world size)
  int ponds = std::max(3, (W * H) / 12000); // e.g. ~1 pond per 12k cells
  for (int p = 0; p < ponds; ++p) {
    int marginX = std::max(10, W / 20);
    int marginY = std::max(6,  H / 20);
    int cx = r.i(marginX, W - 1 - marginX);
    int cy = r.i(marginY, H - 1 - marginY);
    int rad = r.i(4, 10);

    for (int y = cy - rad; y <= cy + rad; ++y)
      for (int x = cx - rad; x <= cx + rad; ++x) {
        if (!inBounds(x, y)) continue;
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= rad * rad + r.i(-3, 3)) g[y][x] = '~';
      }
  }

  // Grass near water
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (g[y][x] == '.' && countNeighbors(g, x, y, '~') > 0 && r.oneIn(2))
        g[y][x] = ',';

  // Starter mushrooms near damp
  int mushSeeds = (W * H) / 500; // scale
  for (int k = 0; k < mushSeeds; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if ((g[y][x] == '.' || g[y][x] == ',') && countNeighbors(g, x, y, '~') > 0 && r.oneIn(2))
      g[y][x] = 'm';
  }
}

// ---------------- Chaos events ----------------
static void eventRain(Grid& g, Rng& r, std::string& banner) {
  banner = "Rain: water creeps; mushrooms bloom; rare rainbow";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (g[y][x] != '~') continue;
    int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
    if (!inBounds(nx, ny)) continue;
    char& t = g[ny][nx];
    if (t != 'T' && t != 'A' && t != 'W' && t != 'o') t = '~';
  }

  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (g[y][x] != '.' && g[y][x] != ',') continue;
    int damp = countNeighbors(g, x, y, '~') + countNeighbors(g, x, y, 'T');
    if (damp >= 2 && r.oneIn(3)) g[y][x] = 'm';
  }

  if (r.oneIn(3)) {
    int y = r.i(2, H - 3);
    for (int x = 0; x < W; ++x) {
      if (g[y][x] == '.' || g[y][x] == ',') g[y][x] = '=';
    }
  }
}

static void eventDrought(Grid& g, Rng& r, std::string& banner) {
  banner = "Drought: water shrinks; mushrooms die back";
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    char& c = g[y][x];
    if (c == '~' && r.oneIn(3)) c = '.';
    if (c == 'm' && r.oneIn(2)) c = '.';
    if ((c == ',' || c == '"') && r.oneIn(3)) c = '.';
  }
}

static void eventLightning(Grid& g, Rng& r, std::string& banner) {
  banner = "Lightning: fires ignite";
  int cx = r.i(0, W - 1), cy = r.i(0, H - 1);
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = cx + r.i(-18, 18), y = cy + r.i(-10, 10);
    if (!inBounds(x, y)) continue;
    if (isPlant(g[y][x])) g[y][x] = '*';
  }
}

static void eventAlien(Grid& g, Rng& r, std::string& banner) {
  banner = "Alien: reality flexes";
  for (int tries = 0; tries < 600; ++tries) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    if (g[y][x] == '~' || g[y][x] == '*') continue;
    g[y][x] = 'A';
    break;
  }
  for (int k = 0; k < EVENT_INTENSITY; ++k) {
    int x = r.i(0, W - 1), y = r.i(0, H - 1);
    char& c = g[y][x];
    if (c == '~' && r.oneIn(2)) c = '.';
    else if (c == '.' && r.oneIn(3)) c = ',';
    else if (c == ',' && r.oneIn(4)) c = 'm';
    else if (c == 'm' && r.oneIn(6)) c = '#';
    else if (c == '#' && r.oneIn(8)) c = 'T';
    else if (c == 'T' && r.oneIn(18)) c = '*';
  }
}

static void maybeChaos(Grid& g, Rng& r, std::string& banner) {
  if (!r.oneIn(EVENT_CHANCE)) { banner = "calm"; return; }
  int type = r.i(1, 5);
  if (type == 1) eventRain(g, r, banner);
  else if (type == 2) eventDrought(g, r, banner);
  else if (type == 3) eventLightning(g, r, banner);
  else if (type == 4) {
    banner = "Blight: vegetation collapse";
    for (int k = 0; k < EVENT_INTENSITY; ++k) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (isPlant(g[y][x]) && r.oneIn(2)) g[y][x] = (r.oneIn(3) ? 'x' : '.');
    }
  } else eventAlien(g, r, banner);
}

static int countAll(const Grid& g, char c) {
  int total = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (g[y][x] == c) total++;
  return total;
}

// ---------------- Simulation tick ----------------
static void step(Grid& g, Rng& r, std::string& banner) {
  Grid next = g;

  maybeChaos(next, r, banner);

  // fade rainbows
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (g[y][x] == '=' && r.oneIn(4))
        next[y][x] = (r.oneIn(2) ? ',' : '.');

  // fire/ash + ignition + water evaporation
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    char c = g[y][x];
    int nF = countNeighbors(g, x, y, '*');
    int nWtr = countNeighbors(g, x, y, '~');

    if (c == '*') { next[y][x] = (r.oneIn(3) ? 'x' : '*'); continue; }
    if (c == 'x') {
      if (nWtr > 0 && r.oneIn(6)) next[y][x] = ',';
      else if (r.oneIn(22)) next[y][x] = '.';
      continue;
    }
    if (isPlant(c) && nF > 0 && r.oneIn(2)) { next[y][x] = '*'; continue; }
    if (c == '~' && r.oneIn(360)) { next[y][x] = '.'; continue; }
  }

  // pastoral succession + mushrooms
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    char c = g[y][x];
    if (c == 'o' || c == 'W' || c == 'A') continue;
    if (c == '*' || c == 'x' || c == '~' || c == '=') continue;

    int nWtr = countNeighbors(g, x, y, '~');
    int nG   = countNeighbors(g, x, y, ',');
    int nTG  = countNeighbors(g, x, y, '"');
    int nS   = countNeighbors(g, x, y, '#');
    int nT   = countNeighbors(g, x, y, 'T');
    int nM   = countNeighbors(g, x, y, 'm');

    if (c == '.') {
      int fert = nWtr * 3 + nG + nTG + nM;
      if (fert > 0 && r.u01() < (0.008 * fert)) next[y][x] = ',';
      if (nWtr > 0 && nT > 0 && r.oneIn(120)) next[y][x] = 'm';
    } else if (c == ',') {
      if ((nG + nTG) >= 4 && r.oneIn(80)) next[y][x] = '"';
      if ((nG + nTG + nWtr) == 0 && r.oneIn(45)) next[y][x] = '.';
      if (nWtr > 0 && nT > 0 && r.oneIn(160)) next[y][x] = 'm';
    } else if (c == '"') {
      if ((nG + nTG) >= 5 && r.oneIn(130)) next[y][x] = '#';
      if (nWtr == 0 && r.oneIn(60)) next[y][x] = ',';
      if (nWtr > 0 && r.oneIn(200)) next[y][x] = 'm';
    } else if (c == '#') {
      if ((nS + nT) >= 3 && nWtr > 0 && r.oneIn(260)) next[y][x] = 'T';
      if (nWtr == 0 && r.oneIn(130)) next[y][x] = '"';
      if (nWtr > 0 && r.oneIn(240)) next[y][x] = 'm';
    } else if (c == 'T') {
      if (nWtr == 0 && r.oneIn(650)) next[y][x] = '#';
      if (nWtr > 0 && r.oneIn(220)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx, ny) && (g[ny][nx] == '.' || g[ny][nx] == ',')) next[ny][nx] = 'm';
      }
    } else if (c == 'm') {
      if (nWtr == 0 && nT == 0 && r.oneIn(18)) next[y][x] = '.';
      if ((nWtr + nT) >= 2 && r.oneIn(30)) {
        int nx = x + r.i(-1, 1), ny = y + r.i(-1, 1);
        if (inBounds(nx, ny) && (g[ny][nx] == '.' || g[ny][nx] == ',')) next[ny][nx] = 'm';
      }
    }
  }

  // creatures: spawn + simple random moves (cheap)
  Grid cur = next;
  Grid out = cur;

  const int MAX_HERB = std::max(18, (W * H) / 1600);
  const int MAX_PRED = std::max(8,  (W * H) / 3600);

  int herbCount = countAll(cur, 'o');
  int predCount = countAll(cur, 'W');

  if (herbCount < MAX_HERB && r.oneIn(18)) {
    for (int tries = 0; tries < 200; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (cur[y][x] == ',' || cur[y][x] == '"' || cur[y][x] == 'm') { out[y][x] = 'o'; break; }
    }
  }
  if (predCount < MAX_PRED && herbCount > MAX_HERB / 3 && r.oneIn(50)) {
    for (int tries = 0; tries < 200; ++tries) {
      int x = r.i(0, W - 1), y = r.i(0, H - 1);
      if (cur[y][x] == '.' || cur[y][x] == ',') { out[y][x] = 'W'; break; }
    }
  }

  auto tryMove = [&](int x, int y, int nx, int ny, char who) -> bool {
    if (!inBounds(nx, ny)) return false;
    char dest = cur[ny][nx];
    if (!isWalkable(dest)) return false;

    if (who == 'o') {
      if (dest == ',' || dest == '"' || dest == 'm' || dest == '.') {
        out[ny][nx] = 'o';
        out[y][x] = '.';
        return true;
      }
      return false;
    }
    if (who == 'W') {
      if (dest == 'o') { out[ny][nx] = 'W'; out[y][x] = '.'; return true; }
      if (dest == '.' || dest == ',' || dest == '"' || dest == 'm' || dest == '#') {
        out[ny][nx] = 'W'; out[y][x] = '.'; return true;
      }
      return false;
    }
    if (who == 'A') {
      out[ny][nx] = 'A';
      out[y][x] = (r.oneIn(10) ? 'x' : (r.oneIn(2) ? 'm' : ','));
      return true;
    }
    return false;
  };

  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    char c = cur[y][x];
    if (c != 'o' && c != 'W' && c != 'A') continue;
    if (out[y][x] != c) continue;

    int dx = r.i(-1, 1), dy = r.i(-1, 1);
    if (dx == 0 && dy == 0) continue;

    if (!tryMove(x, y, x + dx, y + dy, c)) {
      for (int tries = 0; tries < 2; ++tries) {
        int rx = x + r.i(-1, 1), ry = y + r.i(-1, 1);
        if (tryMove(x, y, rx, ry, c)) break;
      }
    }
  }

  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (out[y][x] == 'A' && r.oneIn(320)) out[y][x] = '=';

  g.swap(out);
}

// ---------------- Rendering ----------------
static inline void setColor(SDL_Renderer* r, uint8_t R, uint8_t G, uint8_t B) {
  SDL_SetRenderDrawColor(r, R, G, B, 255);
}

struct Layout {
  int screenW = 0;
  int screenH = 0;
  int cell = 1;
  int gridPixW = 0;
  int gridPixH = 0;
  int offX = 0;
  int offY = 0;
};

static Layout computeLayout(SDL_Renderer* ren) {
  Layout L;
  SDL_GetRendererOutputSize(ren, &L.screenW, &L.screenH);

  // Leave room for a simple HUD strip at bottom (no text rendering needed)
  int hudH = std::max(40, L.screenH / 18);

  int availW = L.screenW;
  int availH = L.screenH - hudH;

  int cellW = std::max(1, availW / W);
  int cellH = std::max(1, availH / H);
  L.cell = std::max(1, std::min(cellW, cellH));

  L.gridPixW = W * L.cell;
  L.gridPixH = H * L.cell;

  L.offX = (L.screenW - L.gridPixW) / 2;
  L.offY = (availH - L.gridPixH) / 2;

  return L;
}

static void drawTile(SDL_Renderer* ren, const Layout& L, int gx, int gy, char t) {
  SDL_Rect rc{ L.offX + gx * L.cell, L.offY + gy * L.cell, L.cell, L.cell };

  // Simple palette (fast and readable)
  switch (t) {
    case '.': setColor(ren, 18, 18, 18); break;    // dirt
    case ',': setColor(ren, 38, 70, 38); break;    // grass
    case '"': setColor(ren, 60, 95, 60); break;    // tall grass
    case '#': setColor(ren, 45, 60, 45); break;    // shrub
    case 'T': setColor(ren, 28, 48, 28); break;    // tree
    case '~': setColor(ren, 25, 40, 90); break;    // water
    case 'm': setColor(ren, 95, 75, 40); break;    // mushroom
    case '*': setColor(ren, 150, 60, 30); break;   // fire
    case 'x': setColor(ren, 60, 50, 45); break;    // ash
    case '=': setColor(ren, 130, 130, 130); break; // rainbow shimmer
    case 'o': setColor(ren, 220, 220, 220); break; // herbivore
    case 'W': setColor(ren, 200, 205, 240); break; // predator
    case 'A': setColor(ren, 220, 220, 120); break; // alien
    default:  setColor(ren, 90, 90, 90); break;
  }

  SDL_RenderFillRect(ren, &rc);
}

static void render(SDL_Renderer* ren, const Grid& g, const Layout& L) {
  setColor(ren, 0, 0, 0);
  SDL_RenderClear(ren);

  // Draw grid
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      drawTile(ren, L, x, y, g[y][x]);

  // HUD strip
  SDL_Rect hud{ 0, L.screenH - std::max(40, L.screenH / 18), L.screenW, std::max(40, L.screenH / 18) };
  setColor(ren, 8, 8, 8);
  SDL_RenderFillRect(ren, &hud);

  SDL_RenderPresent(ren);
}

int main(int, char**) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  SDL_Window* win = SDL_CreateWindow(
    "Terrarium SDL2",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    0, 0,
    SDL_WINDOW_FULLSCREEN_DESKTOP
  );

  if (!win) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) {
    std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  // Hint: helps on some Pi setups
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  uint32_t seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
  Rng r(seed);
  Grid g;
  seedWorld(g, r);

  bool running = true;
  bool paused = false;
  int tps = DEFAULT_TPS;
  int tick = 0;
  std::string banner = "calm";

  Layout layout = computeLayout(ren);

  auto last = std::chrono::steady_clock::now();

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;

      if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.sym) {
          case SDLK_ESCAPE: running = false; break;
          case SDLK_SPACE:  paused = !paused; break;
          case SDLK_PERIOD: if (paused) { step(g, r, banner); tick++; } break;
          case SDLK_r:      seedWorld(g, r); tick = 0; banner = "reset"; break;
          case SDLK_LEFTBRACKET:  if (tps > 1) tps--; break;
          case SDLK_RIGHTBRACKET: if (tps < 30) tps++; break;
          default: break;
        }
      }

      // Recompute layout if display mode changes / window resized (some backends emit this)
      if (e.type == SDL_WINDOWEVENT &&
          (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
           e.window.event == SDL_WINDOWEVENT_RESIZED ||
           e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)) {
        layout = computeLayout(ren);
      }
    }

    auto now = std::chrono::steady_clock::now();
    auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    const int msPerTick = 1000 / std::max(1, tps);

    if (!paused && dtMs >= msPerTick) {
      last = now;
      step(g, r, banner);
      tick++;
    }

    // Put status in window title (no font libs needed)
    std::string title =
      "Terrarium | " + std::to_string(W) + "x" + std::to_string(H) +
      " | tick " + std::to_string(tick) +
      " | " + (paused ? "PAUSED" : ("tps " + std::to_string(tps))) +
      " | " + banner +
      " | keys: SPACE pause  . step  [ ] speed  r reset  ESC quit";
    SDL_SetWindowTitle(win, title.c_str());

    render(ren, g, layout);

    // tiny sleep so we don't peg CPU at 100%
    SDL_Delay(6);
  }

  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
