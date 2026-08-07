// Headless contact sheet: render the pixelview palette to a BMP without a
// window, a panel or a Pi. Colour work is impossible to judge by reading
// arithmetic — this is how you look at it.
//
//   terrarium-contact --out sheet.bmp [--scale 3] [--warm 300]
//                     [--biomes meadow,alpine,...] [--seasons spring,...]
//                     [--tick N] [--day 1] [--crop N]
//
// One tile per (biome x season): each world is seeded, stepped `--warm` ticks
// so the planting settles, then stepped to a tick that lands inside the
// requested season and rendered at 1px/cell. Labels are drawn in a 5x7 font
// so a sheet is readable on its own.

#define SDL_MAIN_HANDLED
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "terrarium_core.hpp"
#include "terrarium_pixelview.hpp"

namespace {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px;  // RGB
  Image() = default;
  Image(int W_, int H_) : w(W_), h(H_), px((size_t)W_ * H_ * 3, 0) {}
  void set(int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    size_t i = ((size_t)y * w + x) * 3;
    px[i] = (uint8_t)std::clamp(r, 0, 255);
    px[i + 1] = (uint8_t)std::clamp(g, 0, 255);
    px[i + 2] = (uint8_t)std::clamp(b, 0, 255);
  }
};

bool writeBmp(const Image& im, const char* path) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const int rowRaw = im.w * 3;
  const int pad = (4 - (rowRaw % 4)) % 4;
  const uint32_t dataSize = (uint32_t)((rowRaw + pad) * im.h);
  const uint32_t fileSize = 54u + dataSize;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &fileSize, 4);
  uint32_t off = 54; std::memcpy(hdr + 10, &off, 4);
  uint32_t dib = 40;  std::memcpy(hdr + 14, &dib, 4);
  int32_t wv = im.w, hv = im.h;
  std::memcpy(hdr + 18, &wv, 4);
  std::memcpy(hdr + 22, &hv, 4);
  uint16_t planes = 1, bpp = 24;
  std::memcpy(hdr + 26, &planes, 2);
  std::memcpy(hdr + 28, &bpp, 2);
  std::memcpy(hdr + 34, &dataSize, 4);
  std::fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> row((size_t)rowRaw + pad, 0);
  for (int y = im.h - 1; y >= 0; --y) {   // BMP is bottom-up
    for (int x = 0; x < im.w; ++x) {
      size_t i = ((size_t)y * im.w + x) * 3;
      row[(size_t)x * 3 + 0] = im.px[i + 2];   // BGR
      row[(size_t)x * 3 + 1] = im.px[i + 1];
      row[(size_t)x * 3 + 2] = im.px[i + 0];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

// ---- 5x7 font, enough for labels ----
const char* kGlyphRows[] = {
  /*A*/ "01110" "10001" "10001" "11111" "10001" "10001" "10001",
  /*B*/ "11110" "10001" "10001" "11110" "10001" "10001" "11110",
  /*C*/ "01110" "10001" "10000" "10000" "10000" "10001" "01110",
  /*D*/ "11110" "10001" "10001" "10001" "10001" "10001" "11110",
  /*E*/ "11111" "10000" "10000" "11110" "10000" "10000" "11111",
  /*F*/ "11111" "10000" "10000" "11110" "10000" "10000" "10000",
  /*G*/ "01110" "10001" "10000" "10111" "10001" "10001" "01111",
  /*H*/ "10001" "10001" "10001" "11111" "10001" "10001" "10001",
  /*I*/ "01110" "00100" "00100" "00100" "00100" "00100" "01110",
  /*J*/ "00111" "00010" "00010" "00010" "00010" "10010" "01100",
  /*K*/ "10001" "10010" "10100" "11000" "10100" "10010" "10001",
  /*L*/ "10000" "10000" "10000" "10000" "10000" "10000" "11111",
  /*M*/ "10001" "11011" "10101" "10101" "10001" "10001" "10001",
  /*N*/ "10001" "11001" "10101" "10011" "10001" "10001" "10001",
  /*O*/ "01110" "10001" "10001" "10001" "10001" "10001" "01110",
  /*P*/ "11110" "10001" "10001" "11110" "10000" "10000" "10000",
  /*Q*/ "01110" "10001" "10001" "10001" "10101" "10010" "01101",
  /*R*/ "11110" "10001" "10001" "11110" "10100" "10010" "10001",
  /*S*/ "01111" "10000" "10000" "01110" "00001" "00001" "11110",
  /*T*/ "11111" "00100" "00100" "00100" "00100" "00100" "00100",
  /*U*/ "10001" "10001" "10001" "10001" "10001" "10001" "01110",
  /*V*/ "10001" "10001" "10001" "10001" "10001" "01010" "00100",
  /*W*/ "10001" "10001" "10001" "10101" "10101" "11011" "10001",
  /*X*/ "10001" "10001" "01010" "00100" "01010" "10001" "10001",
  /*Y*/ "10001" "10001" "01010" "00100" "00100" "00100" "00100",
  /*Z*/ "11111" "00001" "00010" "00100" "01000" "10000" "11111",
};
const char* kDigitRows[] = {
  /*0*/ "01110" "10001" "10011" "10101" "11001" "10001" "01110",
  /*1*/ "00100" "01100" "00100" "00100" "00100" "00100" "01110",
  /*2*/ "01110" "10001" "00001" "00010" "00100" "01000" "11111",
  /*3*/ "11111" "00010" "00100" "00010" "00001" "10001" "01110",
  /*4*/ "00010" "00110" "01010" "10010" "11111" "00010" "00010",
  /*5*/ "11111" "10000" "11110" "00001" "00001" "10001" "01110",
  /*6*/ "00110" "01000" "10000" "11110" "10001" "10001" "01110",
  /*7*/ "11111" "00001" "00010" "00100" "01000" "01000" "01000",
  /*8*/ "01110" "10001" "10001" "01110" "10001" "10001" "01110",
  /*9*/ "01110" "10001" "10001" "01111" "00001" "00010" "01100",
};

void drawChar(Image& im, int x, int y, char c, int r, int g, int b) {
  const char* rows = nullptr;
  char u = (char)std::toupper((unsigned char)c);
  if (u >= 'A' && u <= 'Z') rows = kGlyphRows[u - 'A'];
  else if (u >= '0' && u <= '9') rows = kDigitRows[u - '0'];
  else if (u == '-') rows = "00000" "00000" "00000" "11111" "00000" "00000" "00000";
  else if (u == '.') rows = "00000" "00000" "00000" "00000" "00000" "01100" "01100";
  else return;  // space and anything unmapped
  for (int ry = 0; ry < 7; ++ry)
    for (int rx = 0; rx < 5; ++rx)
      if (rows[ry * 5 + rx] == '1') im.set(x + rx, y + ry, r, g, b);
}

void drawText(Image& im, int x, int y, const std::string& s, int r, int g, int b) {
  for (size_t i = 0; i < s.size(); ++i) drawChar(im, x + (int)i * 6, y, s[i], r, g, b);
}

std::vector<std::string> splitCsv(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur += (char)std::tolower((unsigned char)c);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Every biome in the enum must round-trip through its own name. This is here
// because `--biome sky` silently ran a MEADOW on the kiosk for an hour: pico
// and the standalone each had a hand-written if-chain that returned the
// default for anything it did not list, so a new biome parsed as meadow and
// the panel showed rivers and flowers in the "sky". Nothing in the render
// path could have caught that — the contact sheet sets the enum directly.
int selfTestBiomeNames() {
  int bad = 0;
  for (int i = 0; i < BIOME_COUNT; ++i) {
    const char* nm = biomeName((Biome)i);
    if (!nm || !*nm) {
      std::printf("FAIL biome %d has no name\n", i);
      ++bad;
      continue;
    }
    // The name must be unique, or a lookup by name is ambiguous.
    for (int k = 0; k < i; ++k) {
      if (std::string(nm) == biomeName((Biome)k)) {
        std::printf("FAIL biome %d and %d share the name '%s'\n", k, i, nm);
        ++bad;
      }
    }
  }
  std::printf("%s: %d biomes, %d problem(s)\n", bad ? "FAIL" : "ok",
              BIOME_COUNT, bad);
  return bad;
}

// Nothing in the sky may wink out where it can be seen. Every flyer must be
// OUTSIDE the visible disc on the first and last frame of its crossing, and
// its path must actually cross the middle. Written because two of them
// failed it in different ways: the UFO's waypoints were all interior so it
// blinked out mid-hover, and the balloons used an un-normalised heading, so
// a near-vertical one covered less than half its intended distance and
// popped in and out on screen.
int selfTestSkyExits() {
  const float cx = (float)W * 0.5f, cy = (float)H * 0.5f;
  const float vis = 0.5f * std::sqrt((float)W * W + (float)H * H) + 6.f;
  struct Probe { const char* name; };
  int bad = 0;

  auto offPanel = [&](float x, float y) {
    float dx = x - cx, dy = y - cy;
    return std::sqrt(dx * dx + dy * dy) > (float)std::min(W, H) * 0.5f + 4.f;
  };

  // Walk a long stretch of time and, for each flyer, record where it was on
  // the first and last frame it was up.
  struct Track {
    const char* name;
    bool was = false;
    float fx = 0, fy = 0, lx = 0, ly = 0;
    bool haveFirst = false;
    int crossings = 0, badEntry = 0, badExit = 0;
    bool everInside = false;
  };
  Track tr[6] = {{"dragon"}, {"unicorn"}, {"ufo"}, {"rider"}, {"witch"},
                 {"balloon0"}};

  for (float t = 0.f; t < 4000.f; t += 0.20f) {
    const PixelviewSkyCast& S = pixelviewSkyCast(t);
    bool up[6] = {S.dragonUp, S.unicornUp, S.ufoUp, S.riderUp, S.witchUp,
                  S.balloonUp[0]};
    float px[6] = {S.dragX[0], S.uniX, S.ufoX, S.riderX, S.witchX,
                   S.balloonX[0]};
    float py[6] = {S.dragY[0], S.uniY, S.ufoY, S.riderY, S.witchY,
                   S.balloonY[0]};
    for (int k = 0; k < 6; ++k) {
      Track& q = tr[k];
      if (up[k]) {
        if (!q.was) {                       // first frame of a crossing
          q.fx = px[k]; q.fy = py[k];
          q.haveFirst = true;
          if (!offPanel(px[k], py[k])) ++q.badEntry;
        }
        q.lx = px[k]; q.ly = py[k];
        if (!offPanel(px[k], py[k])) q.everInside = true;
      } else if (q.was && q.haveFirst) {    // it just left
        ++q.crossings;
        if (!offPanel(q.lx, q.ly)) ++q.badExit;
      }
      q.was = up[k];
    }
  }

  for (Track& q : tr) {
    bool ok = (q.badEntry == 0 && q.badExit == 0);
    if (!q.crossings) {
      std::printf("   %-9s no crossings sampled\n", q.name);
      continue;
    }
    if (!q.everInside) {
      std::printf("   %-9s never enters the disc at all\n", q.name);
      ++bad;
      continue;
    }
    std::printf("   %-9s %d crossings  entry %s  exit %s\n", q.name,
                q.crossings, q.badEntry ? "*** POPS IN ***" : "off-panel",
                q.badExit ? "*** POPS OUT ***" : "off-panel");
    if (!ok) ++bad;
  }
  (void)vis;
  std::printf("%s: sky exits\n", bad ? "FAIL" : "ok");
  return bad;
}

int biomeFromName(const std::string& n) {
  for (int i = 0; i < BIOME_COUNT; ++i) {
    std::string bn = biomeName((Biome)i);
    for (auto& c : bn) c = (char)std::tolower((unsigned char)c);
    if (bn == n || bn.rfind(n, 0) == 0) return i;
  }
  return -1;
}

// The first tick inside `want`, at or after `from`.
int tickInSeason(Season want, float phase) {
  // Seasons advance every SEASON_TICKS; phase 0..1 picks how deep in.
  return (int)want * SEASON_TICKS + (int)(phase * (float)(SEASON_TICKS - 1));
}

}  // namespace

int main(int argc, char** argv) {
  std::string out = "contact.bmp";
  std::string biomesArg, seasonsArg = "spring,summer,autumn,winter";
  int scale = 3, warm = 300, crop = 0, forceTick = -1, darkProbe = 0;
  bool greyProbe = false;
  float animT = 12.0f;
  bool island = false;

  auto need = [&](int& i) -> std::string {
    return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out") out = need(i);
    else if (a == "--scale") scale = std::clamp(std::atoi(need(i).c_str()), 1, 12);
    else if (a == "--warm") warm = std::max(0, std::atoi(need(i).c_str()));
    else if (a == "--crop") crop = std::max(0, std::atoi(need(i).c_str()));
    else if (a == "--tick") forceTick = std::atoi(need(i).c_str());
    else if (a == "--animt") animT = (float)std::atof(need(i).c_str());
    else if (a == "--biomes") biomesArg = need(i);
    else if (a == "--seasons") seasonsArg = need(i);
    else if (a == "--island") island = true;
    else if (a == "--dark") darkProbe = std::atoi(need(i).c_str());
    else if (a == "--grey") { greyProbe = true; darkProbe = 255; }
    else if (a == "--selftest")
      return (selfTestBiomeNames() + selfTestSkyExits()) ? 1 : 0;
    else if (a == "--daynight") g_daynightMode = std::atoi(need(i).c_str());
    else {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      return 2;
    }
  }

  // Always noon unless asked otherwise: a palette judged under a dusk grade
  // is a palette you cannot judge.
  static bool dnSet = false;
  (void)dnSet;

  std::vector<int> biomes;
  if (biomesArg.empty()) {
    for (int i = 0; i < BIOME_COUNT; ++i) biomes.push_back(i);
  } else {
    for (auto& n : splitCsv(biomesArg)) {
      int b = biomeFromName(n);
      if (b < 0) { std::fprintf(stderr, "unknown biome: %s\n", n.c_str()); return 2; }
      biomes.push_back(b);
    }
  }
  std::vector<Season> seasons;
  for (auto& n : splitCsv(seasonsArg)) {
    if (n.rfind("sp", 0) == 0) seasons.push_back(SPRING);
    else if (n.rfind("su", 0) == 0) seasons.push_back(SUMMER);
    else if (n.rfind("a", 0) == 0) seasons.push_back(AUTUMN);
    else if (n.rfind("w", 0) == 0) seasons.push_back(WINTER);
    else { std::fprintf(stderr, "unknown season: %s\n", n.c_str()); return 2; }
  }

  const int cellW = (crop > 0) ? std::min(crop, W) : W;
  const int cellH = (crop > 0) ? std::min(crop, H) : H;
  const int cx0 = (W - cellW) / 2, cy0 = (H - cellH) / 2;
  const int tileW = cellW * scale, tileH = cellH * scale;
  const int labelH = 12, gap = 6, leftPad = 8, topPad = 12;

  Image sheet(leftPad + (int)seasons.size() * (tileW + gap) + gap,
              topPad + (int)biomes.size() * (tileH + labelH + gap) + gap);
  for (size_t i = 0; i < sheet.px.size(); i += 3) {
    sheet.px[i] = 18; sheet.px[i + 1] = 18; sheet.px[i + 2] = 22;
  }

  for (size_t si = 0; si < seasons.size(); ++si) {
    std::string hdr = seasonName(seasons[si]);
    drawText(sheet, leftPad + (int)si * (tileW + gap) + 2, 2, hdr, 190, 190, 200);
  }

  for (size_t bi = 0; bi < biomes.size(); ++bi) {
    for (size_t si = 0; si < seasons.size(); ++si) {
      Rng rng(1234u + (uint32_t)biomes[bi] * 77u);
      World world;
      world.island = island;
      seedWorld(world, rng, (Biome)biomes[bi]);

      int tick = (forceTick >= 0) ? forceTick : tickInSeason(seasons[si], 0.55f);
      // Warm the world up at its own clock, then hold the season tick for
      // rendering: stepping is what grows the planting.
      std::string banner;
      for (int k = 0; k < warm; ++k) {
        step(world, rng, banner, tick + k);
        g_stepEvents.clear();
      }

      const int ox = leftPad + (int)si * (tileW + gap);
      const int oy = topPad + (int)bi * (tileH + labelH + gap);
      for (int y = 0; y < cellH; ++y) {
        for (int x = 0; x < cellW; ++x) {
          PixelviewRGB c = pixelviewCellColor(world, cx0 + x, cy0 + y, tick, animT);
          for (int sy = 0; sy < scale; ++sy)
            for (int sx = 0; sx < scale; ++sx)
              sheet.set(ox + x * scale + sx, oy + y * scale + sy, c.r, c.g, c.b);
        }
      }
      if (si == 0) {
        std::string lbl = biomeName((Biome)biomes[bi]);
        drawText(sheet, ox + 2, oy + tileH + 2, lbl, 170, 175, 185);
      }

      // Which GLYPH is that? Colour work stalls the moment you start
      // guessing which terrain a bad patch came from, so ask the world.
      if (darkProbe > 0) {
        struct Tally { char t; char e; int n; float lum; };
        std::vector<Tally> tally;
        int total = 0;
        for (int y = 0; y < cellH; ++y) {
          for (int x = 0; x < cellW; ++x) {
            int wx = cx0 + x, wy = cy0 + y;
            PixelviewRGB c = pixelviewCellColor(world, wx, wy, tick, animT);
            float lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
            ++total;
            int mx = std::max(c.r, std::max(c.g, c.b));
            int mn = std::min(c.r, std::min(c.g, c.b));
            bool hit = greyProbe ? (mx - mn < 34 && lum > 70.f)
                                 : (lum <= (float)darkProbe);
            if (!hit) continue;
            char tg = world.terrain[wy][wx], eg = world.entities[wy][wx];
            if (world.water[wy][wx] > 0)
              tg = (char)('0' + std::min<int>(9, world.water[wy][wx]));
            bool found = false;
            for (auto& q : tally)
              if (q.t == tg && q.e == eg) { ++q.n; q.lum += lum; found = true; break; }
            if (!found) tally.push_back({tg, eg, 1, lum});
          }
        }
        std::sort(tally.begin(), tally.end(),
                  [](const Tally& a, const Tally& b2) { return a.n > b2.n; });
        int dark = 0;
        for (auto& q : tally) dark += q.n;
        std::printf("%s/%s: %d/%d cells matched (%s)\n",
                    biomeName((Biome)biomes[bi]), seasonName(seasons[si]),
                    dark, total, greyProbe ? "flat/grey" : "dark");
        for (size_t q = 0; q < tally.size() && q < 8; ++q)
          std::printf("    terrain '%c' entity '%c'  x%-5d  mean lum %.0f\n",
                      tally[q].t, tally[q].e, tally[q].n,
                      tally[q].lum / (float)tally[q].n);
      }
    }
  }

  if (!writeBmp(sheet, out.c_str())) {
    std::fprintf(stderr, "could not write %s\n", out.c_str());
    return 1;
  }
  std::printf("wrote %s (%dx%d), %d biomes x %d seasons @ %dx\n", out.c_str(),
              sheet.w, sheet.h, (int)biomes.size(), (int)seasons.size(), scale);
  return 0;
}
