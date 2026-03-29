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

Layout computeLayout(SDL_Renderer* ren);

struct GlyphCache {
  std::unordered_map<unsigned char, SDL_Texture*> tex;
  bool textMode = false;

  void destroy();
  SDL_Texture* makeGlyph(SDL_Renderer* ren, unsigned char c);
  SDL_Texture* get(SDL_Renderer* ren, unsigned char c);
};

void render(SDL_Renderer* ren, const Layout& layout, World& world, GlyphCache& gcWorld,
            GlyphCache& gcText, int tick, bool showMenu, int menuPage,
            const std::vector<MidiParam>& params, int menuSel, bool synthEnabled,
            const std::string& sf2Path, UiLang uiLang);
