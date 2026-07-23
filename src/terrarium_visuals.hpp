#pragma once

#include "terrarium_render.hpp"

struct RGB {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

inline void setColor(SDL_Renderer* renderer, uint8_t red, uint8_t green,
                     uint8_t blue, uint8_t alpha = 255) {
  SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
}

uint8_t adjustedCloudCoverage(const World& world, int worldX, int worldY);
char terrainGlyphVariant(char terrainGlyph, uint32_t hash, Season season,
                         const Weather& weather);
char renderCharAt(const World& world, int x, int y, int tick);
RGB baseBgFor(const World& world, int x, int y, int tick, Season season,
              float seasonBlend);
RGB fgForChar(const World& world, char glyph, Season season, float seasonBlend,
              int tick, int x, int y);
void applyCloudShadow(RGB& bg, uint8_t cloudVal);
// Day/night grade: brightness from d.level (floor 0.38 — moonlit, not
// black), warm/cool tint from d.warm. Smooth by construction.
void applyDaylight(RGB& color, const Daylight& d);
void applyCloudLayer(SDL_Renderer* renderer, const SDL_Rect& rect,
                     uint8_t cloudVal);
void drawString(SDL_Renderer* renderer, GlyphCache& glyphCache, int x, int y,
                const std::string& text, uint8_t red, uint8_t green,
                uint8_t blue, uint8_t alpha, int scale);
