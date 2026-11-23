// gfx draws 2D shapes, textures and glyphs with OpenGL ES 2. Callers lay out everything on a
// fixed 1920x1080 canvas. init() fits the canvas into the centre of the window with uniform
// scaling, which letterboxes other aspect ratios.
//
// Draw calls append quads to one vertex batch. A draw that needs another program, texture,
// shape uniform or blend state flushes the pending batch first. RGBA texture data must be
// premultiplied.
#pragma once

#include <GLES2/gl2.h>
#include <cstdint>
#include <vector>

// Color holds straight alpha with components in 0..1. The renderer premultiplies it per vertex.
struct Color { float r, g, b, a; };

inline Color rgb(int r, int g, int b, float a = 1.0f) { return { r / 255.0f, g / 255.0f, b / 255.0f, a }; }
inline Color faded(Color c, float a) { c.a *= a; return c; }

struct Texture {
    GLuint id = 0;
    int w = 0, h = 0;
    bool alpha_only = false;      // GL_ALPHA coverage mask, the tint sets its colour
};

namespace gfx {

extern float scale;               // screen pixels per canvas pixel
extern float off_x, off_y;        // screen position of the canvas origin, off_x includes shift()
extern int win_w, win_h;

bool init(int window_w, int window_h);   // false when a shader program fails to link
void warm_up();                   // once after init(): compiles the driver's shader variants before the first frame
void begin();                     // start a frame: set the viewport, clear to black
void flush();                     // submit the pending batch

// With pixels set, the texture magnifies with GL_NEAREST, which keeps enlarged pixel art sharp.
Texture make_rgba(const uint8_t* premultiplied, int w, int h, bool pixels = false);
Texture make_alpha(const uint8_t* alpha, int w, int h);
// Creates an RGB565 texture, the format of the 16-bit Linux framebuffer console. It samples
// with GL_NEAREST, so a full-screen copy of the console stays pixel exact.
Texture make_565(const uint8_t* pixels, int w, int h);
// Updates a region of an alpha texture with glTexSubImage2D. text.cpp uses it to fill glyph atlases.
void sub_alpha(Texture& t, int x, int y, int w, int h, const uint8_t* alpha);
void release(Texture& t);         // deletes the GL texture, resets t

// The functions below take canvas coordinates.

// Draws the opaque gradient background over the canvas. shadowed selects the copy with the
// shadow from set_ground_shadow(), falling back to the plain gradient before the first call.
// covered is an optional rectangle (x, y, w, h) that opaque content will cover afterwards.
// background() leaves it out to save fill rate.
void background(bool shadowed = false, const float* covered = nullptr);
// Bakes the soft shadow of a rounded box into a second background texture. x, y, w, h,
// radius and the fade width soft are in canvas pixels. dark is the darkening at full shadow,
// 0..1. The call rebuilds the texture on the CPU.
void set_ground_shadow(float x, float y, float w, float h, float radius, float soft, float dark);

void clip(float x0, float x1);    // scissor to canvas columns x0..x1 (ignores shift()), clip(0, 1920) turns it off
void shift(float dx);             // offset later draws dx canvas pixels to the right, shift(0) resets

// The VideoCore IV GPU fills opaque geometry much faster with blending disabled. Functions
// with a solid parameter disable blending when it is true. Set it only for fully opaque content.
void image(const Texture& t, float x, float y, float w, float h, Color tint = { 1, 1, 1, 1 });
// Draws the texture region (u0, v0)..(u1, v1) into a box with rounded corners. solid takes
// effect only when tint is opaque.
void image_round(const Texture& t, float x, float y, float w, float h, float radius, Color tint,
                 float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1, bool solid = false);
void rect(float x, float y, float w, float h, Color c);
// Draws a convex quadrilateral with one colour per corner, corners clockwise from the top
// left. With a texture, u and v hold the texture coordinates of each corner. Both may be null
// when t is null.
void corners(const Texture* t, const float x[4], const float y[4], const float u[4], const float v[4], const Color c[4],
             bool solid = false);
// Draws an opaque texture in such a quadrilateral, mixed toward the colour `with` by a
// per-corner `amount`: 0 keeps the texture, 1 gives the flat colour. It always draws with
// blending off.
void corners_mixed(const Texture& t, const float x[4], const float y[4], const float u[4], const float v[4],
                   Color with, const float amount[4]);
// Draws a filled rounded box, or with line > 0 an outline of that width in canvas pixels.
void round_rect(float x, float y, float w, float h, float radius, Color c, float line = 0);
// Draws the soft shadow of a rounded box. The shadow fades out over soft canvas pixels
// centred on the box edge. covered works as in background().
void shadow(float x, float y, float w, float h, float radius, float soft, Color c, const float* covered = nullptr);

// glyph() takes screen coordinates, which lets text.cpp place glyphs on whole pixels.
void glyph(const Texture& atlas, float x, float y, float w, float h, float u0, float v0, float u1, float v1, Color c);

std::vector<uint8_t> read_pixels();                                          // RGBA, bottom row first

}  // namespace gfx
