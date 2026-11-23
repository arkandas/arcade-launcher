// gfx.cpp implements gfx.h on OpenGL ES 2. All seven programs share one vertex shader.
// Rounded boxes and shadows come from signed distance functions. Large boxes draw in pieces
// that run the SDF shader only on their corners or edge strips, which reduces fragment work
// on the Pi.
#include "gfx.h"
#include "brand.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx {

float scale = 1.0f, off_x = 0.0f, off_y = 0.0f;
int win_w = 1920, win_h = 1080;

static float home_x = 0.0f;       // off_x at shift(0)

namespace {

struct Vertex {
    float x, y;          // screen pixels
    float u, v;          // texture coordinates, or the offset from the box centre for the SDF programs
    float u2, v2;        // texture coordinates for P_ROUND_TEX, mix amount in u2 for P_MIXED
    uint8_t r, g, b, a;  // premultiplied
};

enum Program { P_NONE = -1, P_TEX, P_ALPHA, P_SOLID, P_ROUND, P_ROUND_TEX, P_SHADOW, P_MIXED, P_COUNT };

GLuint programs[P_COUNT];
GLint u_screen[P_COUNT], u_rect[P_COUNT], u_tex[P_COUNT];

// These variables hold the state of the pending batch. use() flushes the batch before this
// state changes.
std::vector<Vertex> batch;
int cur_program = P_NONE;
GLuint cur_texture = 0;
float cur_rect[4] = { 0, 0, 0, 0 };   // u_rect: half size, radius, line or fade width, or the P_MIXED colour
bool cur_blend = true;       // blending on for the pending batch
bool gl_blend = true;        // GL_BLEND as last set on the context
Texture bg_texture, bg_shadowed;

// The vertex shader maps screen pixels with a top-left origin to clip space. u_screen holds
// 2 / window size.
const char* VERTEX = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec2 a_uv2;
attribute vec4 a_col;
uniform vec2 u_screen;
varying vec2 v_uv;
varying vec2 v_uv2;
varying vec4 v_col;
void main() {
    v_uv = a_uv;
    v_uv2 = a_uv2;
    v_col = a_col;
    gl_Position = vec4(a_pos.x * u_screen.x - 1.0, 1.0 - a_pos.y * u_screen.y, 0.0, 1.0);
}
)";

const char* HEAD = R"(
precision mediump float;
uniform sampler2D u_tex;
uniform vec4 u_rect;
varying vec2 v_uv;
varying vec2 v_uv2;
varying vec4 v_col;
)";

// SHAPE holds signed distance functions for a rounded box centred on the origin. p is the
// fragment offset from the box centre in screen pixels. r.xyz holds the half width, half
// height and corner radius. shape() returns the antialiased coverage of the box, or of an
// outline r.w wide when r.w > 0. shade() fades from 1 to 0 with a smoothstep across r.w
// pixels centred on the edge.
const char* SHAPE = R"(
float shape(vec2 p, vec4 r) {
    vec2 q = abs(p) - (r.xy - vec2(r.z));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r.z;
    float a = clamp(0.5 - d, 0.0, 1.0);
    if (r.w > 0.0) a *= clamp(0.5 + d + r.w, 0.0, 1.0);
    return a;
}
// The same box as a shadow that fades over the distance r.w around its edge.
float shade(vec2 p, vec4 r) {
    vec2 q = abs(p) - (r.xy - vec2(r.z));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r.z;
    float a = clamp(0.5 - d / r.w, 0.0, 1.0);
    return a * a * (3.0 - 2.0 * a);
}
)";

// MAIN holds one fragment shader body per Program, in enum order. v_col arrives premultiplied.
const char* MAIN[P_COUNT] = {
    "void main() { gl_FragColor = texture2D(u_tex, v_uv) * v_col; }",
    "void main() { gl_FragColor = v_col * texture2D(u_tex, v_uv).a; }",
    "void main() { gl_FragColor = v_col; }",
    "void main() { gl_FragColor = v_col * shape(v_uv, u_rect); }",
    "void main() { gl_FragColor = texture2D(u_tex, v_uv2) * v_col * shape(v_uv, u_rect); }",
    "void main() { gl_FragColor = v_col * shade(v_uv, u_rect); }",
    "void main() { gl_FragColor = vec4(mix(texture2D(u_tex, v_uv).rgb, u_rect.rgb, v_uv2.x), 1.0) * v_col; }",
};

// compile() logs compile errors. init() detects a failed shader when the program fails to link.
GLuint compile(GLenum type, const char* a, const char* b = "", const char* c = "") {
    GLuint shader = glCreateShader(type);
    const char* parts[3] = { a, b, c };
    glShaderSource(shader, 3, parts, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        fprintf(stderr, "shader: %s\n", log);
    }
    return shader;
}

// use() selects the program, texture, u_rect values and blend state for the next quads. Any
// change flushes the pending batch first.
void use(int program, GLuint texture, float hw = 0, float hh = 0, float radius = 0, float line = 0, bool blend = true) {
    bool same_rect = cur_rect[0] == hw && cur_rect[1] == hh && cur_rect[2] == radius && cur_rect[3] == line;
    if (program == cur_program && texture == cur_texture && same_rect && blend == cur_blend)
        return;
    flush();
    cur_program = program;
    cur_texture = texture;
    cur_rect[0] = hw; cur_rect[1] = hh; cur_rect[2] = radius; cur_rect[3] = line;
    cur_blend = blend;
}

inline void vertex(float x, float y, float u, float v, float u2, float v2, const uint8_t* c) {
    batch.push_back({ x, y, u, v, u2, v2, c[0], c[1], c[2], c[3] });
}

// Converts a straight-alpha Color to premultiplied 8-bit RGBA.
inline void bytes_of(Color c, uint8_t col[4]) {
    float a = c.a < 0 ? 0 : c.a > 1 ? 1 : c.a;
    col[0] = (uint8_t)lroundf(c.r * a * 255);
    col[1] = (uint8_t)lroundf(c.g * a * 255);
    col[2] = (uint8_t)lroundf(c.b * a * 255);
    col[3] = (uint8_t)lroundf(a * 255);
}

// Appends a screen-space rectangle as two triangles, with (u, v) for a_uv and (s, t) for a_uv2.
void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
          float s0, float t0, float s1, float t1, Color c) {
    uint8_t col[4];
    bytes_of(c, col);
    vertex(x0, y0, u0, v0, s0, t0, col);
    vertex(x1, y0, u1, v0, s1, t0, col);
    vertex(x1, y1, u1, v1, s1, t1, col);
    vertex(x0, y0, u0, v0, s0, t0, col);
    vertex(x1, y1, u1, v1, s1, t1, col);
    vertex(x0, y1, u0, v1, s0, t1, col);
}

// sx() and sy() map canvas to screen coordinates, including the shift() offset.
inline float sx(float x) { return off_x + x * scale; }
inline float sy(float y) { return off_y + y * scale; }

// Builds the background texture, a horizontal gradient through the ground colours of brand.h.
// background() stretches the 480x270 texture over the canvas, which is cheap on the Pi.
// Random noise of up to half a level per channel hides 8-bit banding. box adds the shadow
// of a rounded box: x, y, w, h, radius and softness in canvas pixels, then the darkness 0..1.
Texture make_background(const float* box = nullptr) {
    const int w = 480, h = 270;
    const float stops[3] = { 0.0f, 0.36f, 1.0f };
    const float colours[3][3] = { { BRAND_GROUND_0 }, { BRAND_GROUND_1 }, { BRAND_GROUND_2 } };
    std::vector<uint8_t> px((size_t)w * h * 4);
    uint32_t seed = 7;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float t = x / (float)(w - 1);
            int i = t < stops[1] ? 0 : 1;
            float f = (t - stops[i]) / (stops[i + 1] - stops[i]);
            float light = 1;
            if (box) {
                // This matches shade() in SHAPE.
                float dx = (x + 0.5f) * 1920 / w - (box[0] + box[2] / 2), dy = (y + 0.5f) * 1080 / h - (box[1] + box[3] / 2);
                float qx = fabsf(dx) - (box[2] / 2 - box[4]), qy = fabsf(dy) - (box[3] / 2 - box[4]);
                float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - box[4];
                float a = fminf(fmaxf(0.5f - d / box[5], 0), 1);
                light = 1 - box[6] * a * a * (3 - 2 * a);
            }
            for (int ch = 0; ch < 3; ch++) {
                seed = seed * 1664525u + 1013904223u;
                float noise = (seed >> 8) / 16777216.0f - 0.5f;
                float value = (colours[i][ch] + (colours[i + 1][ch] - colours[i][ch]) * f) * light + noise;
                px[((size_t)y * w + x) * 4 + ch] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : lroundf(value));
            }
            px[((size_t)y * w + x) * 4 + 3] = 255;
        }
    }
    return make_rgba(px.data(), w, h);
}

}  // namespace

bool init(int window_w, int window_h) {
    win_w = window_w;
    win_h = window_h;
    scale = fminf(win_w / 1920.0f, win_h / 1080.0f);
    off_x = home_x = floorf((win_w - 1920.0f * scale) / 2);
    off_y = floorf((win_h - 1080.0f * scale) / 2);

    GLuint vs = compile(GL_VERTEX_SHADER, VERTEX);
    for (int p = 0; p < P_COUNT; p++) {
        GLuint fs = compile(GL_FRAGMENT_SHADER, HEAD, p >= P_ROUND ? SHAPE : "", MAIN[p]);
        programs[p] = glCreateProgram();
        glAttachShader(programs[p], vs);
        glAttachShader(programs[p], fs);
        glBindAttribLocation(programs[p], 0, "a_pos");
        glBindAttribLocation(programs[p], 1, "a_uv");
        glBindAttribLocation(programs[p], 2, "a_uv2");
        glBindAttribLocation(programs[p], 3, "a_col");
        glLinkProgram(programs[p]);
        GLint ok = 0;
        glGetProgramiv(programs[p], GL_LINK_STATUS, &ok);
        if (!ok) {
            fprintf(stderr, "could not link shader program %d\n", p);
            return false;
        }
        u_screen[p] = glGetUniformLocation(programs[p], "u_screen");
        u_rect[p] = glGetUniformLocation(programs[p], "u_rect");
        u_tex[p] = glGetUniformLocation(programs[p], "u_tex");
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);      // premultiplied alpha
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);            // tightly packed pixel rows
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    bg_texture = make_background();
    batch.reserve(4096);
    return true;
}

// The Pi's GL driver compiles a new shader variant the first time a program runs with a new
// combination of state, such as blending or texture format. That compile takes long enough
// to drop a frame. warm_up() draws one off-screen quad with every combination the menu
// uses, which moves those compiles to startup.
void warm_up() {
    const uint8_t dots[16] = { 0 };
    Texture alpha = make_alpha(dots, 2, 2), sharp = make_rgba(dots, 2, 2, true), console = make_565(dots, 2, 2);
    struct Way { int program; GLuint texture; bool blend; };
    const Way ways[] = {
        { P_TEX, bg_texture.id, true }, { P_TEX, bg_texture.id, false }, { P_TEX, sharp.id, true }, { P_TEX, sharp.id, false },
        { P_ALPHA, alpha.id, true }, { P_SOLID, 0, true }, { P_SOLID, 0, false }, { P_ROUND, 0, true },
        { P_ROUND_TEX, bg_texture.id, true }, { P_ROUND_TEX, sharp.id, true }, { P_SHADOW, 0, true },
        { P_MIXED, sharp.id, false }, { P_MIXED, console.id, false },
    };
    glViewport(0, 0, win_w, win_h);
    for (const Way& way : ways) {
        use(way.program, way.texture, 1, 1, 1, 1, way.blend);
        quad(-10, -10, -9, -9, 0, 0, 1, 1, 0, 0, 1, 1, { 1, 1, 1, 1 });
        flush();
    }
    glFinish();
    release(alpha);
    release(sharp);
    release(console);
}

void begin() {
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void flush() {
    if (batch.empty() || cur_program == P_NONE) {
        batch.clear();
        return;
    }
    if (cur_blend != gl_blend) {
        if (cur_blend) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);
        gl_blend = cur_blend;
    }
    glUseProgram(programs[cur_program]);
    glUniform2f(u_screen[cur_program], 2.0f / win_w, 2.0f / win_h);
    if (u_rect[cur_program] >= 0)
        glUniform4f(u_rect[cur_program], cur_rect[0], cur_rect[1], cur_rect[2], cur_rect[3]);
    if (u_tex[cur_program] >= 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, cur_texture);
        glUniform1i(u_tex[cur_program], 0);
    }
    const Vertex* v = batch.data();
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &v->x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &v->u);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &v->u2);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), &v->r);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)batch.size());
    batch.clear();
}

static Texture make(const uint8_t* data, int w, int h, GLenum format, bool pixels = false) {
    flush();
    Texture t;
    t.w = w;
    t.h = h;
    t.alpha_only = (format == GL_ALPHA);
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, pixels ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, GL_UNSIGNED_BYTE, data);
    cur_texture = 0;
    cur_program = P_NONE;
    return t;
}

Texture make_rgba(const uint8_t* premultiplied, int w, int h, bool pixels) { return make(premultiplied, w, h, GL_RGBA, pixels); }
Texture make_alpha(const uint8_t* alpha, int w, int h) { return make(alpha, w, h, GL_ALPHA); }

Texture make_565(const uint8_t* pixels, int w, int h) {
    flush();
    Texture t;
    t.w = w;
    t.h = h;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
    cur_texture = 0;
    cur_program = P_NONE;
    return t;
}

void sub_alpha(Texture& t, int x, int y, int w, int h, const uint8_t* alpha) {
    flush();
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_ALPHA, GL_UNSIGNED_BYTE, alpha);
    cur_texture = 0;
    cur_program = P_NONE;
}

void release(Texture& t) {
    if (t.id) {
        flush();
        glDeleteTextures(1, &t.id);
        cur_texture = 0;
        cur_program = P_NONE;
    }
    t = Texture();
}

void set_ground_shadow(float x, float y, float w, float h, float radius, float soft, float dark) {
    const float box[7] = { x, y, w, h, radius, soft, dark };
    release(bg_shadowed);
    bg_shadowed = make_background(box);
}

void background(bool shadowed, const float* covered) {
    use(P_TEX, shadowed && bg_shadowed.id ? bg_shadowed.id : bg_texture.id, 0, 0, 0, 0, false);
    auto band = [](float x0, float y0, float x1, float y1) {
        if (x1 > x0 && y1 > y0)
            quad(sx(x0), sy(y0), sx(x1), sy(y1), x0 / 1920, y0 / 1080, x1 / 1920, y1 / 1080, 0, 0, 0, 0, { 1, 1, 1, 1 });
    };
    if (!covered) {
        band(0, 0, 1920, 1080);
        return;
    }
    // Draw the bands above, below, left and right of the covered rectangle.
    float x0 = covered[0], y0 = covered[1], x1 = x0 + covered[2], y1 = y0 + covered[3];
    band(0, 0, 1920, y0);
    band(0, y1, 1920, 1080);
    band(0, y0, x0, y1);
    band(x1, y0, 1920, y1);
}

void clip(float x0, float x1) {
    flush();
    if (x0 <= 0 && x1 >= 1920) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    float from = floorf(home_x + x0 * scale), to = floorf(home_x + x1 * scale);
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)from, 0, (GLsizei)fmaxf(0, to - from), win_h);
}

// Rounding to whole screen pixels keeps textures pixel-aligned while a screen slides.
void shift(float dx) {
    off_x = home_x + roundf(dx * scale);
}

void image(const Texture& t, float x, float y, float w, float h, Color tint) {
    if (!t.id || tint.a <= 0)
        return;
    use(t.alpha_only ? P_ALPHA : P_TEX, t.id);
    quad(sx(x), sy(y), sx(x + w), sy(y + h), 0, 0, 1, 1, 0, 0, 0, 0, tint);
}

void image_round(const Texture& t, float x, float y, float w, float h, float radius, Color tint,
                 float u0, float v0, float u1, float v1, bool solid) {
    if (!t.id || tint.a <= 0)
        return;
    float x0 = sx(x), y0 = sy(y), x1 = sx(x + w), y1 = sy(y + h);
    float hw = (x1 - x0) / 2, hh = (y1 - y0) / 2, cx = x0 + hw, cy = y0 + hh, r = ceilf(radius * scale);
    // pu() and pv() map a screen position inside the box to texture coordinates between
    // (u0, v0) and (u1, v1).
    auto pu = [&](float px) { return u0 + (u1 - u0) * (px - x0) / (x1 - x0); };
    auto pv = [&](float py) { return v0 + (v1 - v0) * (py - y0) / (y1 - y0); };
    auto rounded = [&](float ax, float ay, float bx, float by) {
        quad(ax, ay, bx, by, ax - cx, ay - cy, bx - cx, by - cy, pu(ax), pv(ay), pu(bx), pv(by), tint);
    };
    // When the corner squares overlap, one SDF quad draws the whole box.
    if (2 * r >= x1 - x0 || 2 * r >= y1 - y0) {
        use(P_ROUND_TEX, t.id, hw, hh, radius * scale, 0);
        rounded(x0, y0, x1, y1);
        return;
    }
    // Only the four corner squares need the SDF program. The three strips between them use
    // P_TEX, which the Pi fills several times faster.
    auto plain = [&](float ax, float ay, float bx, float by) {
        quad(ax, ay, bx, by, pu(ax), pv(ay), pu(bx), pv(by), 0, 0, 0, 0, tint);
    };
    use(P_TEX, t.id, 0, 0, 0, 0, !(solid && tint.a >= 1));
    plain(x0 + r, y0, x1 - r, y0 + r);
    plain(x0, y0 + r, x1, y1 - r);
    plain(x0 + r, y1 - r, x1 - r, y1);
    use(P_ROUND_TEX, t.id, hw, hh, radius * scale, 0);
    rounded(x0, y0, x0 + r, y0 + r);
    rounded(x1 - r, y0, x1, y0 + r);
    rounded(x0, y1 - r, x0 + r, y1);
    rounded(x1 - r, y1 - r, x1, y1);
}

void rect(float x, float y, float w, float h, Color c) {
    if (c.a <= 0)
        return;
    use(P_SOLID, 0);
    quad(sx(x), sy(y), sx(x + w), sy(y + h), 0, 0, 0, 0, 0, 0, 0, 0, c);
}

void corners(const Texture* t, const float x[4], const float y[4], const float u[4], const float v[4], const Color c[4],
             bool solid) {
    if (t && !t->id)
        return;
    use(t ? (t->alpha_only ? P_ALPHA : P_TEX) : P_SOLID, t ? t->id : 0, 0, 0, 0, 0, !solid);
    uint8_t col[4][4];
    for (int i = 0; i < 4; i++)
        bytes_of(c[i], col[i]);
    static const int ORDER[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i : ORDER)
        vertex(sx(x[i]), sy(y[i]), t ? u[i] : 0, t ? v[i] : 0, 0, 0, col[i]);
}

void corners_mixed(const Texture& t, const float x[4], const float y[4], const float u[4], const float v[4],
                   Color with, const float amount[4]) {
    if (!t.id)
        return;
    // u_rect holds the mix colour. u2 holds the per-corner amount.
    use(P_MIXED, t.id, with.r, with.g, with.b, 0, false);
    uint8_t col[4];
    bytes_of({ 1, 1, 1, 1 }, col);
    static const int ORDER[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i : ORDER)
        vertex(sx(x[i]), sy(y[i]), u[i], v[i], amount[i], 0, col);
}

// Appends one piece of a rounded box or shadow in screen pixels, skipping empty pieces. The
// SDF coordinates are offsets from the centre (cx, cy) of the whole box, so every piece
// evaluates the same shape.
static void part(float x0, float y0, float x1, float y1, float cx, float cy, Color c) {
    if (x1 > x0 && y1 > y0)
        quad(x0, y0, x1, y1, x0 - cx, y0 - cy, x1 - cx, y1 - cy, 0, 0, 0, 0, c);
}

void round_rect(float x, float y, float w, float h, float radius, Color c, float line) {
    if (c.a <= 0)
        return;
    float hw = w * scale / 2, hh = h * scale / 2;
    use(P_ROUND, 0, hw, hh, radius * scale, line * scale);
    float x0 = sx(x), y0 = sy(y), x1 = sx(x + w), y1 = sy(y + h), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    // band and side are the sizes of the SDF strips along the edges: band for the top and
    // bottom, side for the left and right of an outline. Both include a pixel of margin for
    // antialiasing. One SDF quad draws the whole box when the strips overlap or when a
    // filled box covers fewer than 40000 screen pixels.
    float band = fmaxf(radius, line) * scale + 1, side = line * scale + 1;
    if (2 * band >= y1 - y0 || 2 * side >= x1 - x0 || (line <= 0 && (x1 - x0) * (y1 - y0) < 40000)) {
        part(x0, y0, x1, y1, cx, cy, c);
        return;
    }
    if (line <= 0) {
        // A large filled box needs the SDF only in its corner squares. P_SOLID fills the three
        // strips between them, with blending off for an opaque colour.
        float r = ceilf(radius * scale);
        part(x0, y0, x0 + r, y0 + r, cx, cy, c);
        part(x1 - r, y0, x1, y0 + r, cx, cy, c);
        part(x0, y1 - r, x0 + r, y1, cx, cy, c);
        part(x1 - r, y1 - r, x1, y1, cx, cy, c);
        use(P_SOLID, 0, 0, 0, 0, 0, c.a < 1);
        quad(x0 + r, y0, x1 - r, y0 + r, 0, 0, 0, 0, 0, 0, 0, 0, c);
        quad(x0, y0 + r, x1, y1 - r, 0, 0, 0, 0, 0, 0, 0, 0, c);
        quad(x0 + r, y1 - r, x1 - r, y1, 0, 0, 0, 0, 0, 0, 0, 0, c);
        return;
    }
    // An outline covers only the four edge strips. Skipping the interior saves its fragments.
    part(x0, y0, x1, y0 + band, cx, cy, c);
    part(x0, y1 - band, x1, y1, cx, cy, c);
    part(x0, y0 + band, x0 + side, y1 - band, cx, cy, c);
    part(x1 - side, y0 + band, x1, y1 - band, cx, cy, c);
}

void shadow(float x, float y, float w, float h, float radius, float soft, Color c, const float* covered) {
    if (c.a <= 0 || soft <= 0)
        return;
    // The fade reaches soft / 2 beyond the box edge. room adds one pixel to that.
    float hw = w * scale / 2, hh = h * scale / 2, room = soft * scale / 2 + 1;
    use(P_SHADOW, 0, hw, hh, radius * scale, soft * scale);
    float x0 = sx(x) - room, y0 = sy(y) - room, x1 = sx(x + w) + room, y1 = sy(y + h) + room;
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    if (!covered) {
        part(x0, y0, x1, y1, cx, cy, c);
        return;
    }
    // Draw the bands around the covered rectangle, clamped to the shadow quad.
    float hx0 = fminf(fmaxf(sx(covered[0]), x0), x1), hy0 = fminf(fmaxf(sy(covered[1]), y0), y1);
    float hx1 = fminf(fmaxf(sx(covered[0] + covered[2]), hx0), x1), hy1 = fminf(fmaxf(sy(covered[1] + covered[3]), hy0), y1);
    part(x0, y0, x1, hy0, cx, cy, c);
    part(x0, hy1, x1, y1, cx, cy, c);
    part(x0, hy0, hx0, hy1, cx, cy, c);
    part(hx1, hy0, x1, hy1, cx, cy, c);
}

void glyph(const Texture& atlas, float x, float y, float w, float h, float u0, float v0, float u1, float v1, Color c) {
    use(P_ALPHA, atlas.id);
    quad(x, y, x + w, y + h, u0, v0, u1, v1, 0, 0, 0, 0, c);
}

std::vector<uint8_t> read_pixels() {
    flush();
    std::vector<uint8_t> px((size_t)win_w * win_h * 4);
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

}  // namespace gfx
