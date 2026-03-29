#include "terrarium_visuals.hpp"

#include <cmath>
#include <iterator>

namespace {

bool tryRenderEntityGlyph(const World& world, int x, int y, char& glyph) {
  char entity = world.entities[y][x];
  if (entity != ' ') {
    if (entity == 'D') {
      glyph = 'r';
    } else if (entity == 'Q') {
      glyph = 'T';
    } else {
      glyph = entity;
    }
    return true;
  }

  for (int ay = y - 1; ay <= y; ++ay) {
    for (int ax = x - 1; ax <= x; ++ax) {
      if (!inBounds(ax, ay)) continue;
      if (world.entities[ay][ax] != 'D') continue;

      int dx = x - ax;
      int dy = y - ay;
      if (dx >= 0 && dx < 2 && dy >= 0 && dy < 2) {
        glyph = ((dx ^ dy) == 0) ? 'r' : 'v';
        return true;
      }
    }
  }

  for (int ay = y - 1; ay <= y + 1; ++ay) {
    for (int ax = x - 1; ax <= x + 1; ++ax) {
      if (!inBounds(ax, ay)) continue;
      if (world.entities[ay][ax] != 'Q') continue;

      int dx = x - ax;
      int dy = y - ay;
      if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) {
        glyph = ((dx + dy) & 1) ? 'Y' : 'T';
        return true;
      }
    }
  }

  return false;
}

bool tryRenderOverlayGlyph(const World& world, int x, int y, char& glyph) {
  glyph = world.overlay[y][x];
  return glyph != ' ';
}

bool tryRenderShallowWaterDecoration(const World& world, int x, int y, int tick,
                                     uint8_t depth, char& glyph) {
  if (depth > 2 || world.entities[y][x] != ' ' || world.overlay[y][x] != ' ') {
    return false;
  }

  int driftX = (tick / 18) * world.wind.dx;
  int driftY = (tick / 18) * world.wind.dy;
  int wave = (tick / 9) & 3;
  uint32_t hash =
      hash3((uint32_t)(x + driftX + wave), (uint32_t)(y + driftY - wave), 555u);
  if ((hash % 73u) == 0u) {
    glyph = LILYPAD_GLYPH;
    return true;
  }
  if ((hash % 997u) == 0u) {
    glyph = FOAM_GLYPH;
    return true;
  }
  return false;
}

bool isShorelineWaterTile(const World& world, int x, int y) {
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (!dx && !dy) continue;
      int nx = x + dx;
      int ny = y + dy;
      if (!inBounds(nx, ny)) continue;
      if (world.water[ny][nx] == 0) return true;
    }
  }
  return false;
}

int waterFoamChance(const World& world, int x, int y, int tick, uint8_t depth,
                    bool shoreline) {
  int chance = shoreline ? (90 - 10 * std::clamp(world.wind.strength, 0, 5)) : 320;
  if (!shoreline && world.wind.strength >= 2) {
    chance = std::max(45, chance - 55 * world.wind.strength);
  }
  if (!shoreline && depth >= 4) chance = std::max(38, chance - 18);

  int phase = ((tick / 5) + x * 3 + y * 7) & 7;
  if (shoreline && phase == 0) chance = std::max(18, chance - 28);
  return chance;
}

bool tryRenderWindWaveGlyph(const World& world, int x, int y, int tick,
                            uint8_t depth, char& glyph) {
  if (world.wind.strength < 3 || depth < 3) return false;

  uint32_t hash =
      hash3((uint32_t)(x + tick / 3), (uint32_t)(y + tick / 5), 0x57415645u);
  if ((hash % 3u) != 0u) return false;

  int phase = ((tick / 4) + x + y) % 3;
  glyph = (char)('5' + phase);
  return true;
}

char renderWaterGlyph(const World& world, int x, int y, int tick, uint8_t depth) {
  char glyph = ' ';
  if (tryRenderShallowWaterDecoration(world, x, y, tick, depth, glyph)) {
    return glyph;
  }

  bool shoreline = isShorelineWaterTile(world, x, y);
  int foamChance = waterFoamChance(world, x, y, tick, depth, shoreline);
  uint32_t foamHash =
      hash3((uint32_t)(x + tick / 7), (uint32_t)(y - tick / 11), 0xF0A1u);
  if ((foamHash % (uint32_t)foamChance) == 0u) return FOAM_GLYPH;

  if (tryRenderWindWaveGlyph(world, x, y, tick, depth, glyph)) {
    return glyph;
  }

  return waterFlowGlyph(world, x, y, tick);
}

char renderCharAtBase(const World& world, int x, int y, int tick) {
  char glyph = ' ';
  if (tryRenderEntityGlyph(world, x, y, glyph)) return glyph;
  if (tryRenderOverlayGlyph(world, x, y, glyph)) return glyph;

  uint8_t depth = world.water[y][x];
  if (depth > 0) return renderWaterGlyph(world, x, y, tick, depth);

  return world.terrain[y][x];
}

uint8_t sampleCloud(const Clouds& clouds, int x, int y) {
  float fx = ((float)x / (float)W) * (float)CW + clouds.offX;
  float fy = ((float)y / (float)H) * (float)CH + clouds.offY;
  int x0 = (int)floorf(fx) % CW;
  int y0 = (int)floorf(fy) % CH;
  if (x0 < 0) x0 += CW;
  if (y0 < 0) y0 += CH;

  int x1 = (x0 + 1) % CW;
  int y1 = (y0 + 1) % CH;
  float tx = fx - floorf(fx);
  float ty = fy - floorf(fy);

  int a = clouds.field[y0 * CW + x0];
  int b = clouds.field[y0 * CW + x1];
  int d = clouds.field[y1 * CW + x0];
  int e = clouds.field[y1 * CW + x1];

  float top = a + (b - a) * tx;
  float bottom = d + (e - d) * tx;
  float value = top + (bottom - top) * ty;
  value = std::clamp(value, 0.0f, 255.0f);
  return (uint8_t)(value + 0.5f);
}

RGB pickPaletteColor(uint32_t hash, std::initializer_list<RGB> palette) {
  size_t idx =
      (size_t)((hash ^ (hash >> 11) ^ (hash >> 21)) % (uint32_t)palette.size());
  auto it = palette.begin();
  std::advance(it, idx);
  return *it;
}

RGB jitterColor(uint32_t hash, RGB color, int amount) {
  int jr = (int)((hash >> 8) & 7) - 3;
  int jg = (int)((hash >> 11) & 7) - 3;
  int jb = (int)((hash >> 14) & 7) - 3;
  color.r = clampU8((int)color.r + jr * amount);
  color.g = clampU8((int)color.g + jg * amount);
  color.b = clampU8((int)color.b + jb * amount);
  return color;
}

RGB pickAndJitter(uint32_t hash, std::initializer_list<RGB> palette,
                  int amount) {
  return jitterColor(hash, pickPaletteColor(hash, palette), amount);
}

void vividifyColor(RGB& color) {
  float red = color.r / 255.0f;
  float green = color.g / 255.0f;
  float blue = color.b / 255.0f;
  float light = (red + green + blue) / 3.0f;
  red = light + (red - light) * VIVID_SAT;
  green = light + (green - light) * VIVID_SAT;
  blue = light + (blue - light) * VIVID_SAT;
  red *= VIVID_VAL;
  green *= VIVID_VAL;
  blue *= VIVID_VAL;
  color.r = clampU8((int)(red * 255.0f));
  color.g = clampU8((int)(green * 255.0f));
  color.b = clampU8((int)(blue * 255.0f));
}

void applySeasonTintToColor(RGB& color, Season season) {
  if (season == AUTUMN) {
    color.r = clampU8((int)color.r + 10);
    color.g = clampU8((int)color.g - 2);
  } else if (season == WINTER) {
    color.r = clampU8((int)color.r + 6);
    color.g = clampU8((int)color.g + 6);
    color.b = clampU8((int)color.b + 10);
  }
}

void applySceneDimToColor(RGB& color, const World& world, float seasonBlend) {
  float dim = std::clamp(1.0f - seasonBlend, 0.45f, 1.0f);
  dim *= (1.0f - world.biomeFade);
  color.r = (uint8_t)(color.r * dim);
  color.g = (uint8_t)(color.g * dim);
  color.b = (uint8_t)(color.b * dim);
}

RGB colorShortGrass(Biome biome, uint32_t hash) {
  switch (biome) {
    case ALPINE:
      return pickAndJitter(hash,
                           {{80, 140, 120}, {70, 130, 140}, {92, 160, 150},
                            {86, 150, 168}},
                           5);
    case WETLAND:
      return pickAndJitter(hash,
                           {{60, 170, 120}, {52, 156, 120}, {70, 190, 140},
                            {78, 206, 152}},
                           6);
    case DESERT:
      return pickAndJitter(hash,
                           {{150, 170, 110}, {170, 190, 120}, {140, 160, 96}},
                           4);
    default:
      return pickAndJitter(hash,
                           {{88, 180, 110}, {68, 156, 96}, {58, 140, 92},
                            {96, 196, 132}, {78, 170, 120}},
                           6);
  }
}

RGB colorTallGrass(Biome biome, uint32_t hash) {
  switch (biome) {
    case ALPINE:
      return pickAndJitter(hash, {{64, 120, 140}, {58, 110, 150},
                                  {74, 130, 160}},
                           5);
    case WETLAND:
      return pickAndJitter(hash, {{50, 160, 112}, {44, 146, 108},
                                  {62, 182, 132}},
                           6);
    case DESERT:
      return pickAndJitter(hash, {{170, 170, 120}, {190, 180, 130},
                                  {160, 160, 110}},
                           4);
    default:
      return pickAndJitter(hash, {{66, 170, 98}, {52, 152, 86},
                                  {44, 136, 78}, {74, 186, 122}},
                           6);
  }
}

RGB colorShrub(Biome biome, uint32_t hash) {
  switch (biome) {
    case ALPINE:
      return pickAndJitter(hash, {{70, 120, 130}, {62, 110, 140},
                                  {82, 132, 150}},
                           4);
    case WETLAND:
      return pickAndJitter(hash, {{40, 126, 96}, {34, 116, 92},
                                  {52, 150, 118}},
                           5);
    case DESERT:
      return pickAndJitter(hash, {{140, 150, 96}, {120, 132, 86},
                                  {160, 170, 110}},
                           4);
    default:
      return pickAndJitter(hash, {{52, 146, 86}, {42, 126, 78},
                                  {36, 116, 72}, {62, 160, 96}},
                           5);
  }
}

RGB colorReed(Biome biome, uint32_t hash) {
  switch (biome) {
    case WETLAND:
      return pickAndJitter(hash, {{72, 210, 150}, {62, 190, 140},
                                  {90, 230, 170}},
                           6);
    case MEADOW:
      return pickAndJitter(hash, {{72, 180, 118}, {62, 164, 110},
                                  {82, 196, 130}},
                           5);
    case ALPINE:
      return pickAndJitter(hash, {{90, 170, 180}, {80, 160, 190},
                                  {110, 190, 205}},
                           4);
    default:
      return pickAndJitter(hash, {{72, 190, 120}, {62, 170, 112},
                                  {82, 206, 132}},
                           5);
  }
}

RGB colorFlower(Biome biome, uint32_t hash, char glyph) {
  RGB color{255, 255, 255};
  switch (biome) {
    case TROPICAL:
      color = pickAndJitter(hash,
                            {{255, 80, 120}, {255, 140, 60}, {255, 220, 60},
                             {140, 220, 255}, {190, 120, 255}},
                            7);
      break;
    case ALPINE:
      color = pickAndJitter(hash,
                            {{245, 245, 245}, {220, 235, 255},
                             {255, 220, 240}, {210, 255, 250},
                             {235, 235, 210}},
                            4);
      break;
    case WETLAND:
      color = pickAndJitter(hash,
                            {{180, 150, 255}, {140, 200, 255},
                             {200, 170, 255}, {170, 220, 255},
                             {240, 240, 255}},
                            5);
      break;
    case DESERT:
      color = pickAndJitter(hash,
                            {{255, 220, 120}, {255, 190, 90},
                             {255, 160, 80}, {245, 235, 180}},
                            4);
      break;
    case ALIEN:
      color = pickAndJitter(hash,
                            {{120, 255, 180}, {170, 255, 120},
                             {255, 110, 220}, {110, 220, 255},
                             {255, 255, 120}},
                            7);
      break;
    default:
      color = pickAndJitter(hash,
                            {{255, 160, 190}, {255, 220, 120},
                             {200, 170, 255}, {160, 220, 255},
                             {255, 190, 140}, {255, 120, 150},
                             {245, 245, 245}, {210, 255, 160}},
                            6);
      break;
  }

  if (glyph == '+') {
    color.r = clampU8((int)color.r + 12);
    color.g = clampU8((int)color.g + 8);
  }
  if (glyph == '&') color.b = clampU8((int)color.b + 14);
  if (glyph == '!') {
    color.r = clampU8((int)color.r + 16);
    color.b = clampU8((int)color.b + 10);
  }
  return color;
}

RGB colorLilyPad(Biome biome, uint32_t hash) {
  switch (biome) {
    case WETLAND:
      return pickAndJitter(hash, {{72, 210, 150}, {64, 190, 140},
                                  {86, 230, 170}},
                           5);
    case MEADOW:
      return pickAndJitter(hash, {{92, 210, 140}, {72, 190, 120},
                                  {110, 230, 160}},
                           4);
    default:
      return pickAndJitter(hash, {{70, 190, 150}, {60, 170, 140},
                                  {92, 210, 170}},
                           4);
  }
}

RGB colorWaterFlowGlyph(char glyph, int tick, int x, int y) {
  unsigned char uc = (unsigned char)glyph;
  int depth = 1;
  if (uc <= 0x07) {
    depth = 1 + (uc - 0x01);
  } else if (uc <= 0x0E) {
    depth = 1 + (uc - 0x08);
  } else {
    depth = 1 + (uc - 0x0F);
  }

  uint32_t shimmerHash = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(tick / 5));
  int shimmer = (int)((shimmerHash & 3u)) - 1;
  int red = 34 - depth * 2;
  int green = 130 + depth * 3 + shimmer * 2;
  int blue = 210 + depth * 6 + shimmer * 3;
  int darken = depth * 7;
  red = std::max(0, red - darken);
  green = std::max(0, green - darken / 2);
  blue = std::max(0, blue - darken / 3);
  return {clampU8(red), clampU8(green), clampU8(blue)};
}

RGB colorOpenWaterGlyph(const World& world, char glyph, int x, int y) {
  int depth = 0;
  if (inBounds(x, y)) depth = std::min<int>(7, (int)world.water[y][x]);
  if (depth <= 0) depth = (glyph >= '1' && glyph <= '7') ? (int)(glyph - '0') : 2;

  int red = 16 + depth * 2;
  int green = 150 + depth * 8;
  int blue = 190 + depth * 9;
  int windBoost = std::clamp(world.wind.strength, 0, 5) * 4;
  if (world.weather.state == STORM) windBoost += 6;
  red += windBoost;
  green += windBoost;
  blue += windBoost;
  return {clampU8(red), clampU8(green), clampU8(blue)};
}

bool tryColorWaterGlyph(const World& world, char glyph, int tick, int x, int y,
                        RGB& color) {
  unsigned char uc = (unsigned char)glyph;
  if (uc >= 0x01 && uc <= 0x15) {
    color = colorWaterFlowGlyph(glyph, tick, x, y);
    return true;
  }
  if ((glyph >= '1' && glyph <= '7') || isWaterVisualGlyph(uc)) {
    color = (glyph == FOAM_GLYPH) ? RGB{255, 255, 255}
                                  : colorOpenWaterGlyph(world, glyph, x, y);
    return true;
  }
  return false;
}

bool tryColorFloraGlyph(const World& world, char glyph, uint32_t hash,
                        RGB& color) {
  switch (glyph) {
    case ',':
      color = colorShortGrass(world.biome, hash);
      return true;
    case '"':
      color = colorTallGrass(world.biome, hash);
      return true;
    case ';':
      color = colorShrub(world.biome, hash);
      return true;
    case '#':
      color = colorReed(world.biome, hash);
      return true;
    case ':':
      color = pickAndJitter(hash,
                            {{54, 160, 100}, {44, 140, 90}, {64, 178, 110}},
                            5);
      return true;
    case 'm':
      color = pickAndJitter(hash,
                            {{230, 210, 190}, {210, 180, 220},
                             {255, 150, 180}, {200, 245, 255},
                             {255, 240, 170}},
                            4);
      return true;
    case 'f':
    case '+':
    case '&':
    case '!':
      color = colorFlower(world.biome, hash, glyph);
      return true;
    case 'l':
      color = colorLilyPad(world.biome, hash);
      return true;
    case KELP_GLYPH:
      color = pickAndJitter(hash,
                            {{28, 160, 120}, {22, 132, 106},
                             {34, 186, 142}},
                            6);
      return true;
    case 'T':
    case 't':
      color = pickAndJitter(hash,
                            {{140, 98, 66}, {120, 82, 56}, {165, 120, 84}},
                            3);
      return true;
    case 'F':
    case 'P':
      color = pickAndJitter(hash,
                            {{60, 190, 120}, {46, 170, 110}, {70, 210, 140}},
                            5);
      return true;
    default:
      return false;
  }
}

bool tryColorTerrainGlyph(char glyph, uint32_t hash, RGB& color) {
  switch (glyph) {
    case 'd':
      color = pickAndJitter(hash,
                            {{110, 70, 42}, {92, 58, 36}, {138, 92, 58},
                             {120, 78, 48}},
                            3);
      return true;
    case 'B':
      color = pickAndJitter(hash,
                            {{170, 170, 178}, {152, 152, 160},
                             {190, 190, 198}},
                            3);
      return true;
    case '^':
      color = pickAndJitter(hash,
                            {{160, 160, 175}, {140, 140, 160},
                             {185, 185, 205}},
                            3);
      return true;
    case 'M':
      color = pickAndJitter(hash,
                            {{140, 160, 185}, {120, 140, 170},
                             {165, 185, 210}},
                            3);
      return true;
    case 'c':
      color = {120, 220, 150};
      return true;
    case '`':
      color = {230, 210, 150};
      return true;
    default:
      return false;
  }
}

bool tryColorFaunaGlyph(char glyph, RGB& color) {
  switch (glyph) {
    case 'r':
      color = {255, 245, 220};
      return true;
    case 'b':
      color = {220, 255, 180};
      return true;
    case 'v':
      color = {210, 210, 255};
      return true;
    case '>':
    case '<':
      color = {160, 240, 255};
      return true;
    case 'C':
      color = {220, 220, 230};
      return true;
    case 'H':
      color = {230, 245, 255};
      return true;
    case 'A':
      color = {170, 255, 220};
      return true;
    case 'D':
      color = {160, 220, 255};
      return true;
    case 'W':
      color = {190, 210, 240};
      return true;
    case 'K':
      color = {170, 200, 120};
      return true;
    case 'S':
      color = {120, 255, 210};
      return true;
    case 'L':
      color = {220, 255, 180};
      return true;
    case 'X':
      color = {180, 120, 255};
      return true;
    case 'E':
      color = {255, 110, 140};
      return true;
    case 'R':
      color = {255, 200, 120};
      return true;
    case '*':
      color = {255, 255, 255};
      return true;
    case 'o':
      color = {255, 210, 120};
      return true;
    case 'x':
      color = {255, 120, 120};
      return true;
    case 'n':
      color = {180, 255, 180};
      return true;
    default:
      return false;
  }
}

}  // namespace

uint8_t adjustedCloudCoverage(const World& world, int worldX, int worldY) {
  uint8_t cloud = sampleCloud(world.clouds, worldX, worldY);
  cloud = (uint8_t)std::min<int>(255, (int)(cloud * world.cloudOpacity));
  if (world.biome == TROPICAL) cloud = (uint8_t)std::max<int>(0, cloud - 35);
  if (world.biome == DESERT) cloud = (uint8_t)std::max<int>(0, cloud - 25);
  return cloud;
}

char terrainGlyphVariant(char terrainGlyph, uint32_t hash, Season season,
                         const Weather& weather) {
  uint32_t variant = hash & 7u;
  if (terrainGlyph == 'f' || terrainGlyph == '+' || terrainGlyph == '&' ||
      terrainGlyph == '!') {
    if ((season == SPRING || weather.state == RAIN ||
         weather.state == STORM) &&
        variant == 0) {
      return '!';
    }
    if (variant == 1) return '&';
    if (variant == 2) return '+';
    return terrainGlyph;
  }

  if (terrainGlyph == 'd') {
    switch (hash & 3u) {
      case 0:
        return 'd';
      case 1:
        return 'e';
      case 2:
        return 'g';
      default:
        return 'd';
    }
  }

  return terrainGlyph;
}

char renderCharAt(const World& world, int x, int y, int tick) {
  int dx = 0;
  int dy = 0;
  for (const auto& ripple : g_ripples) {
    float rx = float(x - ripple.cx);
    float ry = float(y - ripple.cy);
    float dist = std::sqrt(rx * rx + ry * ry);
    float ring = ripple.speed * ripple.t;
    float delta = std::fabs(dist - ring);
    if (delta < ripple.width) {
      float strength = (1.0f - delta / ripple.width) * ripple.amp;
      float inv = (dist > 0.001f) ? (1.0f / dist) : 0.0f;
      dx += int(std::lround(rx * inv * strength));
      dy += int(std::lround(ry * inv * strength));
    }
  }

  int sampleX = clampi(x + dx, 0, W - 1);
  int sampleY = clampi(y + dy, 0, H - 1);
  return renderCharAtBase(world, sampleX, sampleY, tick);
}

RGB baseBgFor(const World&, int, int, int, Season, float) { return RGB{0, 0, 0}; }

RGB fgForChar(const World& world, char c, Season season, float seasonBlend,
              int tick, int x, int y) {
  uint32_t hash = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(tick / 12));
  RGB fg{235, 235, 235};

  if (c == '.' || c == ' ') {
    fg = {0, 0, 0};
  } else if (!tryColorWaterGlyph(world, c, tick, x, y, fg) &&
             !tryColorFloraGlyph(world, c, hash, fg) &&
             !tryColorTerrainGlyph(c, hash, fg) &&
             !tryColorFaunaGlyph(c, fg)) {
    fg = {235, 235, 235};
  }

  applySeasonTintToColor(fg, season);
  applySceneDimToColor(fg, world, seasonBlend);
  vividifyColor(fg);
  return fg;
}

void applyCloudShadow(RGB& bg, uint8_t cloudVal) {
  float cloud = cloudVal / 255.0f;
  float shadow = 1.0f - cloud * 0.42f;
  bg.r = clampU8((int)(bg.r * shadow));
  bg.g = clampU8((int)(bg.g * shadow));
  bg.b = clampU8((int)(bg.b * shadow));
}

void applyCloudLayer(SDL_Renderer* renderer, const SDL_Rect& rect,
                     uint8_t cloudVal) {
  if (cloudVal < 120) return;
  float cloud = (cloudVal - 120) / 135.0f;
  cloud = std::clamp(cloud, 0.0f, 1.0f);
  uint8_t alpha = (uint8_t)(cloud * 60);
  setColor(renderer, 180, 190, 210, alpha);
  SDL_RenderFillRect(renderer, &rect);
}

void drawString(SDL_Renderer* renderer, GlyphCache& glyphCache, int px, int py,
                const std::string& text, uint8_t red, uint8_t green,
                uint8_t blue, uint8_t alpha, int scale) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  int x = px;
  for (char c : text) {
    if (c == '\n') {
      py += 8 * scale;
      x = px;
      continue;
    }

    SDL_Texture* texture = glyphCache.get(renderer, (unsigned char)c);
    if (!texture) {
      x += 8 * scale;
      continue;
    }

    SDL_SetTextureColorMod(texture, red, green, blue);
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_Rect dst{x, py, 8 * scale, 8 * scale};
    SDL_RenderCopy(renderer, texture, nullptr, &dst);
    x += 8 * scale;
  }
}
