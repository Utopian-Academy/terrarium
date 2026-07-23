#pragma once

#include "terrarium_audio.hpp"

#include <SDL.h>
#include <unordered_map>

struct Layout {
  int scale = 2;
  int screenW = 0;
  int screenH = 0;
  int hudH = 0;
  int simHpx = 0;
};

// showHud=false collapses the bottom HUD strip so the world fills the window
// (used when the menu overlay is hidden).
Layout computeLayout(SDL_Renderer* ren, bool showHud = true);

struct GlyphCache {
  std::unordered_map<unsigned char, SDL_Texture*> tex;
  bool textMode = false;
  // Purpose-built micro font for tiny cells (kiosk panels): hand-drawn 4x4
  // hero glyphs, OR-downsampled 8x8 for the rest (ink never drops out the
  // way it does when SDL nearest-scales an 8x8 texture down). microSize 8
  // = off, 4 = 4x4 marks, 2 = 2x2 marks (OR-folded from the 4x4s).
  int microSize = 8;

  void destroy();
  SDL_Texture* makeGlyph(SDL_Renderer* ren, unsigned char c);
  SDL_Texture* get(SDL_Renderer* ren, unsigned char c);
};

void render(SDL_Renderer* ren, const Layout& layout, World& world, GlyphCache& gcWorld,
            GlyphCache& gcText, int tick, bool showMenu, int menuPage,
            const std::vector<MidiParam>& params, int menuSel, bool synthEnabled,
            const std::string& sf2Path, UiLang uiLang);
