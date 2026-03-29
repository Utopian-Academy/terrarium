#pragma once

#include "terrarium_render.hpp"

void renderMenuOverlay(SDL_Renderer* renderer, const Layout& layout,
                       World& world, GlyphCache& textGlyphs, int menuPage,
                       const std::vector<MidiParam>& params, int menuSel,
                       bool synthEnabled, const std::string& sf2Path,
                       UiLang uiLang, Season season);
