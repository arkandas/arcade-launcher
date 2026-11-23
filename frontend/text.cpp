// text.cpp implements text.h with FreeType. Each Font caches its glyphs in one GlyphSet per
// screen pixel size. A GlyphSet packs the glyph bitmaps row by row into a square alpha atlas.
#include "text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

// Light text on a dark background looks thin when blending uses the linear coverage from
// FreeType as is. Raising coverage to the power 0.6 thickens the antialiased edges, which
// brings the stroke weight close to browser rendering of the same font.
const float EDGE_CURVE = 0.6f;
uint8_t edge[256];                  // coverage lookup table for EDGE_CURVE, filled by init()

// Glyph holds the atlas texture coordinates of a glyph, plus its bitmap size, bearing and
// advance in screen pixels. A glyph without a bitmap, or one that did not fit in the atlas,
// has w = 0.
struct Glyph {
    float u0, v0, u1, v1;
    int w, h, left, top;
    float advance;
};

// A GlyphSet holds the glyphs of one font at one screen pixel size in one alpha atlas, packed
// row by row. The atlas is 256, 512 or 1024 pixels square depending on the pixel size, so
// small text gets a small atlas that is quick to create.
struct GlyphSet {
    Texture atlas;
    int side = 0;
    int pen_x = 1, pen_y = 1, row_h = 0;    // row packing cursor, 1-pixel gaps between glyphs
    std::unordered_map<uint32_t, Glyph> glyphs;
};

FT_Library library;

// Decodes the UTF-8 code point at s[i], leaving i on the next one. Decoding a truncated
// sequence stops at the end of s.
uint32_t next_codepoint(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80)
        return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80)
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

}  // namespace

struct Font {
    FT_Face face = nullptr;
    std::map<int, GlyphSet> sets;     // keyed by screen pixel size

    // Returns the glyph set for px, creating its empty atlas on first use.
    GlyphSet& set(int px) {
        GlyphSet& s = sets[px];
        if (!s.atlas.id) {
            s.side = px <= 24 ? 256 : px <= 48 ? 512 : 1024;
            std::vector<uint8_t> empty((size_t)s.side * s.side, 0);
            s.atlas = gfx::make_alpha(empty.data(), s.side, s.side);
        }
        return s;
    }

    // Returns the cached glyph, rasterizing it into the atlas on first use. A glyph that does
    // not fit in the atlas keeps w = 0, so draw() advances past it without a quad.
    const Glyph& glyph(int px, uint32_t cp) {
        GlyphSet& s = set(px);
        auto found = s.glyphs.find(cp);
        if (found != s.glyphs.end())
            return found->second;

        Glyph g = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        FT_Set_Pixel_Sizes(face, 0, px);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
            FT_GlyphSlot slot = face->glyph;
            int w = (int)slot->bitmap.width, h = (int)slot->bitmap.rows;
            if (s.pen_x + w + 1 > s.side) {
                s.pen_x = 1;
                s.pen_y += s.row_h + 1;
                s.row_h = 0;
            }
            if (w > 0 && h > 0 && s.pen_y + h + 1 <= s.side) {
                // Copy the bitmap without its row padding, through the EDGE_CURVE table.
                std::vector<uint8_t> tight((size_t)w * h);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        tight[(size_t)y * w + x] = edge[slot->bitmap.buffer[y * slot->bitmap.pitch + x]];
                gfx::sub_alpha(s.atlas, s.pen_x, s.pen_y, w, h, tight.data());
                g.u0 = s.pen_x / (float)s.side;
                g.v0 = s.pen_y / (float)s.side;
                g.u1 = (s.pen_x + w) / (float)s.side;
                g.v1 = (s.pen_y + h) / (float)s.side;
                g.w = w;
                g.h = h;
                s.pen_x += w + 1;
                if (h > s.row_h)
                    s.row_h = h;
            }
            g.left = slot->bitmap_left;
            g.top = slot->bitmap_top;
            g.advance = slot->advance.x / 64.0f;
        }
        return s.glyphs.emplace(cp, g).first->second;
    }
};

namespace text {

bool init() {
    for (int i = 0; i < 256; i++)
        edge[i] = (uint8_t)lroundf(255.0f * powf(i / 255.0f, EDGE_CURVE));
    return FT_Init_FreeType(&library) == 0;
}

Font* load(const std::string& path) {
    Font* font = new Font();
    if (FT_New_Face(library, path.c_str(), 0, &font->face) != 0) {
        fprintf(stderr, "font not found: %s\n", path.c_str());
        delete font;
        return nullptr;
    }
    return font;
}

// pixels() converts a canvas font size to the screen pixel size that keys Font::sets, at
// least 4.
static int pixels(float size) {
    int px = (int)lroundf(size * gfx::scale);
    return px < 4 ? 4 : px;
}

float width(Font* font, float size, const std::string& s, float spacing) {
    if (!font)
        return 0;
    int px = pixels(size);
    float w = 0;
    for (size_t i = 0; i < s.size();)
        w += font->glyph(px, next_codepoint(s, i)).advance + spacing * gfx::scale;
    return w / gfx::scale;
}

int prepare(Font* font, float size, const std::string& letters, int most) {
    if (!font)
        return 0;
    int px = pixels(size), drawn = 0;
    GlyphSet& set = font->set(px);
    for (size_t i = 0; i < letters.size() && drawn < most;) {
        uint32_t cp = next_codepoint(letters, i);
        if (set.glyphs.count(cp))
            continue;
        font->glyph(px, cp);
        drawn++;
    }
    return drawn;
}

float cap_height(Font* font, float size) {
    if (!font)
        return 0;
    return font->glyph(pixels(size), 'H').top / gfx::scale;
}

void draw(Font* font, float size, float x, float baseline, const std::string& s, Color c, float spacing) {
    if (!font || c.a <= 0)
        return;
    int px = pixels(size);
    // Each glyph quad sits on whole screen pixels at the bitmap's size, so atlas texels map
    // 1:1 onto the framebuffer.
    float pen = gfx::off_x + x * gfx::scale;
    float base = roundf(gfx::off_y + baseline * gfx::scale);
    const Texture& atlas = font->set(px).atlas;
    for (size_t i = 0; i < s.size();) {
        const Glyph& g = font->glyph(px, next_codepoint(s, i));
        if (g.w > 0)
            gfx::glyph(atlas, roundf(pen + g.left), base - g.top, (float)g.w, (float)g.h, g.u0, g.v0, g.u1, g.v1, c);
        pen += g.advance + spacing * gfx::scale;
    }
}

std::string fit(Font* font, float size, const std::string& s, float max_width, float spacing) {
    if (width(font, size, s, spacing) <= max_width)
        return s;
    std::string cut = s;
    while (!cut.empty()) {
        // Remove the last UTF-8 character, then any trailing spaces and hyphens.
        while (!cut.empty()) {
            unsigned char last = (unsigned char)cut.back();
            cut.pop_back();
            if ((last & 0xC0) != 0x80)
                break;
        }
        while (!cut.empty() && (cut.back() == ' ' || cut.back() == '-'))
            cut.pop_back();
        if (width(font, size, cut + "...", spacing) <= max_width)
            break;
    }
    return cut + "...";
}

}  // namespace text
