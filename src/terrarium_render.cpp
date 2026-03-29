#include "terrarium_render.hpp"

#include "terrarium_ui.hpp"
#include "terrarium_visuals.hpp"

Layout computeLayout(SDL_Renderer* renderer) {
  Layout layout;
  SDL_GetRendererOutputSize(renderer, &layout.screenW, &layout.screenH);
  layout.hudH = std::max(40, layout.screenH / 18);
  layout.simHpx = layout.screenH - layout.hudH;
  return layout;
}

namespace {

struct RenderPassState {
  Season season = SPRING;
  float seasonBlend = 0.0f;
  int viewW = 0;
  int viewH = 0;
};

void renderWorldPass(SDL_Renderer* renderer, const Layout& layout, World& world,
                     GlyphCache& worldGlyphs, int tick,
                     const RenderPassState& frame) {
  for (int y = 0; y < frame.viewH; ++y) {
    int worldY = g_camY + y;
    int y0 = (y * layout.simHpx) / frame.viewH;
    int y1 = ((y + 1) * layout.simHpx) / frame.viewH;
    int cellHeight = std::max(1, y1 - y0);

    for (int x = 0; x < frame.viewW; ++x) {
      int worldX = g_camX + x;
      int x0 = (x * layout.screenW) / frame.viewW;
      int x1 = ((x + 1) * layout.screenW) / frame.viewW;
      int cellWidth = std::max(1, x1 - x0);

      SDL_Rect cellRect{x0, y0, cellWidth, cellHeight};

      RGB bg = baseBgFor(world, worldX, worldY, tick, frame.season,
                         frame.seasonBlend);
      uint8_t cloud = adjustedCloudCoverage(world, worldX, worldY);
      applyCloudShadow(bg, cloud);

      setColor(renderer, bg.r, bg.g, bg.b);
      SDL_RenderFillRect(renderer, &cellRect);

      char glyph = renderCharAt(world, worldX, worldY, tick);
      if (glyph == '.' && world.water[worldY][worldX] == 0 &&
          world.entities[worldY][worldX] == ' ' &&
          world.overlay[worldY][worldX] == ' ') {
        applyCloudLayer(renderer, cellRect, cloud);
        continue;
      }

      if (world.entities[worldY][worldX] == ' ' &&
          world.overlay[worldY][worldX] == ' ' &&
          world.water[worldY][worldX] == 0) {
        uint32_t hash =
            hash3((uint32_t)worldX, (uint32_t)worldY, (uint32_t)(tick / 6));
        glyph = terrainGlyphVariant(glyph, hash, frame.season, world.weather);
      }

      SDL_Texture* texture = worldGlyphs.get(renderer, (unsigned char)glyph);
      if (texture) {
        RGB fg = fgForChar(world, glyph, frame.season, frame.seasonBlend, tick,
                           x, y);

        if ((world.terrain[worldY][worldX] == ',' ||
             world.terrain[worldY][worldX] == '"') &&
            world.wind.strength > 0) {
          uint32_t hash =
              hash3((uint32_t)x, (uint32_t)y, (uint32_t)(tick / 3));
          if (hash & 1u) {
            fg.g = clampU8((int)fg.g + 20);
            fg.r = clampU8((int)fg.r + 5);
          }
        }

        if ((glyph == '/' || glyph == '\\' || glyph == '|') &&
            world.weather.state == STORM) {
          fg.r = clampU8(fg.r + 30);
          fg.g = clampU8(fg.g + 30);
          fg.b = clampU8(fg.b + 30);
        }

        SDL_SetTextureColorMod(texture, fg.r, fg.g, fg.b);
        SDL_RenderCopy(renderer, texture, nullptr, &cellRect);
      }

      applyCloudLayer(renderer, cellRect, cloud);
    }
  }
}

void renderHudBackground(SDL_Renderer* renderer, const Layout& layout) {
  SDL_Rect hud{0, layout.simHpx, layout.screenW, layout.hudH};
  setColor(renderer, 8, 8, 10);
  SDL_RenderFillRect(renderer, &hud);
}

}  // namespace

void render(SDL_Renderer* renderer, const Layout& layout, World& world,
            GlyphCache& worldGlyphs, GlyphCache& textGlyphs, int tick,
            bool showMenu, int menuPage, const std::vector<MidiParam>& params,
            int menuSel, bool synthEnabled, const std::string& sf2Path,
            UiLang uiLang) {
  RenderPassState frame;
  frame.season = seasonAt(tick);
  frame.seasonBlend = seasonLerp(tick) * world.cloudOpacity;
  frame.viewW = std::max(1, W / std::max(1, g_zoom));
  frame.viewH = std::max(1, H / std::max(1, g_zoom));

  setColor(renderer, 0, 0, 0);
  SDL_RenderClear(renderer);

  updateModPool(world, tick, frame.viewW, frame.viewH);
  applyModMatrix();

  renderWorldPass(renderer, layout, world, worldGlyphs, tick, frame);
  renderHudBackground(renderer, layout);
  if (showMenu) {
    renderMenuOverlay(renderer, layout, world, textGlyphs, menuPage, params,
                      menuSel, synthEnabled, sf2Path, uiLang, frame.season);
  }

  SDL_RenderPresent(renderer);
}
