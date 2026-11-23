// text draws strings with gfx. FreeType rasterizes each glyph once per screen pixel size into
// an alpha atlas texture. Drawing the glyph after that costs one textured quad.
#pragma once

#include "gfx.h"

#include <string>

struct Font;

namespace text {

bool init();
// Returns nullptr when FreeType cannot open the file. Every function below accepts a null
// font. Fonts stay loaded until the program exits.
Font* load(const std::string& path);

// Sizes and positions are in canvas pixels. spacing adds extra advance after every glyph,
// which width() counts after the last glyph too.
float width(Font* font, float size, const std::string& s, float spacing = 0);
float cap_height(Font* font, float size);    // top of 'H' above the baseline
void draw(Font* font, float size, float x, float baseline, const std::string& s, Color c, float spacing = 0);

// width() and draw() rasterize a glyph on first use, which can make that frame late.
// prepare() rasterizes up to `most` glyphs of `letters` that are missing at this size. It
// returns the number it rasterized, so 0 means every glyph of `letters` is cached.
int prepare(Font* font, float size, const std::string& letters, int most);

// Returns s when it fits in max_width. Otherwise it returns the longest prefix that fits with
// "..." appended. The cut falls on a UTF-8 character boundary, with trailing spaces and
// hyphens removed.
std::string fit(Font* font, float size, const std::string& s, float max_width, float spacing = 0);

}  // namespace text
