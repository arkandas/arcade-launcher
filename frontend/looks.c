// looks.c renders the boot animation of launcher-boot. looks.h declares the interface.
//
// Each frame, paint_strips builds a display list of coverage masks, rectangles and slanted
// strips, each with a position, colour and opacity. looks_draw compares the list with the
// previous frame's list, marks the 8 x 8 tiles under every entry that changed, merges the
// dirty tiles into rectangles and repaints only those into an RGB888 frame. The caller copies
// the same rectangles to the framebuffer. Full repaints are too slow on a Pi 3, which paints
// about 100,000 pixels per 60 Hz frame, roughly 5% of a 1920 x 1080 screen.
//
// The layout is 1920 x 1080, centred on screens of other sizes. launcher-boot is a static
// binary that can run from the initramfs, so this file depends on libc and libm only.

#include "looks.h"
#include "brand.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ coverage masks

typedef struct {
    int w, h;
    uint8_t* px;        // coverage per pixel, 0 to 255
} Gray;

// Reads a binary PGM (P5, maxval 255) from dir/name into g. Returns 0 on failure.
static int read_pgm(const char* dir, const char* name, Gray* g) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    int max = 0;
    if (fscanf(f, "P5 %d %d %d", &g->w, &g->h, &max) != 3 || max != 255 || g->w <= 0 || g->h <= 0) {
        fclose(f);
        return 0;
    }
    fgetc(f);                                       // single whitespace byte after maxval
    g->px = malloc((size_t)g->w * g->h);
    int ok = g->px && fread(g->px, 1, (size_t)g->w * g->h, f) == (size_t)g->w * g->h;
    fclose(f);
    if (!ok) {
        free(g->px);
        g->px = NULL;
    }
    return ok;
}

// ------------------------------------------------------------------ display list

enum { K_NONE, K_MASK, K_RECT, K_STRIP };

typedef struct {
    int kind;
    int x, y, w, h;         // bounding box in screen pixels
    const Gray* what;
    int my;                 // K_MASK: first mask row inside the bounding box
    int amount;             // opacity, 0 to 256
    uint8_t rgb[3];
    int i;                  // kind-specific parameter, see paint_strip
    float a, b, c, d;       // kind-specific parameters, see paint_strip
} Thing;

// MOST_THINGS, MOST_BOXES and MOST_ENTRIES cap the display list, the dirty-rectangle list and
// the spatial index. TILE is the side of a dirty-tracking tile in pixels. CELL is the side of a
// spatial-index cell, which lists the entries that overlap it.
enum { MOST_THINGS = 800, MOST_BOXES = 6000, TILE = 8, CELL = 128, MOST_ENTRIES = 16000 };

static const uint8_t WHITE[3] = { 255, 255, 255 };
static const uint8_t ACCENT[3] = { BRAND_ACCENT };
static const uint8_t STRIP_RGB[3] = { BRAND_START_STRIPS };   // the strips, shaded by STRIP_SHADE

static int screen_w, screen_h, ox, oy;      // ox, oy: offset of the 1920 x 1080 layout
static uint8_t* picture;                    // RGB888 frame, screen_w * screen_h * 3 bytes
static Thing now_things[MOST_THINGS], then_things[MOST_THINGS];
static int now_count, then_count, first_frame;
static uint8_t* tiles;                      // dirty flag per tile
static int tiles_w, tiles_h;
static int* cell_from;                      // per cell: start offset into cell_things, plus one end offset
static int* cell_next;                      // per cell: write position while filling cell_things
static uint16_t cell_things[MOST_ENTRIES];
static int cells_w, cells_h;
static LookRect boxes[MOST_BOXES];
static int box_count;
static double expected = 17;                // expected seconds from the first frame to the end request
static double ending = 0;                   // duration of the ending in seconds, set by paint_strips

static Thing spare;

// Returns a new display-list entry. When the list is full, it returns the scratch entry spare,
// which looks_draw never paints. Callers can therefore write to the result without a check.
// thing() takes screen coordinates. mask(), rows() and rect() take layout coordinates and an
// opacity from 0 to 1.
static Thing* thing(int kind, int x, int y, int w, int h, const uint8_t* rgb, int amount) {
    Thing* t = now_count < MOST_THINGS ? &now_things[now_count++] : &spare;
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->amount = amount < 0 ? 0 : amount > 256 ? 256 : amount;
    memcpy(t->rgb, rgb, 3);
    return t;
}

static Thing* mask(const Gray* g, int x, int y, const uint8_t* rgb, float strength) {
    Thing* t = thing(K_MASK, ox + x, oy + y, g->w, g->h, rgb, (int)(256 * strength + 0.5f));
    t->what = g;
    return t;
}

// Adds rows [from, to) of mask g at the position they have when the whole mask is at (x, y).
static Thing* rows(const Gray* g, int from, int to, int x, int y, const uint8_t* rgb, float strength) {
    Thing* t = thing(K_MASK, ox + x, oy + y + from, g->w, to - from, rgb, (int)(256 * strength + 0.5f));
    t->what = g;
    t->my = from;
    return t;
}

static Thing* rect(int x, int y, int w, int h, const uint8_t* rgb, float strength) {
    return thing(K_RECT, ox + x, oy + y, w, h, rgb, (int)(256 * strength + 0.5f));
}

static void scaled(uint8_t* out, const uint8_t* rgb, float by) {
    for (int i = 0; i < 3; i++) {
        float v = rgb[i] * by;
        out[i] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v);
    }
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease_out(float v) { v = 1 - clamp01(v); return 1 - v * v * v; }

// ------------------------------------------------------------------ rasterization

typedef struct {
    int x0, y0, x1, y1;     // half-open: x0 <= x < x1, y0 <= y < y1
} Clip;

static inline int cover(int value, int amount) {       // coverage 0 to 256 from a mask byte and an opacity
    int c = (value * amount + 128) >> 8;
    return c + (c >> 7);                               // rescale to 0..256: 255 becomes fully opaque
}

// Blends rgb over the pixel p with coverage c (0 to 256).
static inline void blend(uint8_t* p, const uint8_t* rgb, int c) {
    for (int i = 0; i < 3; i++)
        p[i] = (uint8_t)(p[i] + (((rgb[i] - p[i]) * c) >> 8));
}

// Fills row y between the fractional x positions left and right. The two end pixels get
// coverage in proportion to the part of them inside the span.
static void span(int y, float left, float right, const uint8_t* rgb, int amount, Clip c) {
    if (right <= left || y < c.y0 || y >= c.y1)
        return;
    int from = (int)floorf(left), to = (int)ceilf(right);       // pixels [from, to)
    uint8_t* row = picture + (size_t)y * screen_w * 3;
    if (from >= c.x0 && from < c.x1) {
        float share = fminf(from + 1, right) - left;
        blend(row + from * 3, rgb, cover((int)(share * 255), amount));
    }
    if (to - 1 > from && to - 1 >= c.x0 && to - 1 < c.x1)
        blend(row + (to - 1) * 3, rgb, cover((int)((right - (to - 1)) * 255), amount));
    int x0 = from + 1 < c.x0 ? c.x0 : from + 1, x1 = to - 1 > c.x1 ? c.x1 : to - 1;
    if (amount >= 256) {
        for (uint8_t* p = row + x0 * 3; x0 < x1; x0++, p += 3) {
            p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2];
        }
    } else {
        int full = cover(255, amount);
        for (uint8_t* p = row + x0 * 3; x0 < x1; x0++, p += 3)
            blend(p, rgb, full);
    }
}

static void paint_mask(const Thing* t, Clip c) {
    const Gray* g = t->what;
    for (int y = c.y0; y < c.y1; y++) {
        const uint8_t* m = g->px + (size_t)(y - t->y + t->my) * g->w + (c.x0 - t->x);
        uint8_t* p = picture + ((size_t)y * screen_w + c.x0) * 3;
        for (int x = c.x0; x < c.x1; x++, m++, p += 3) {
            if (*m)
                blend(p, t->rgb, cover(*m, t->amount));
        }
    }
}

static void paint_rect(const Thing* t, Clip c) {
    for (int y = c.y0; y < c.y1; y++)
        span(y, c.x0, c.x1, t->rgb, t->amount, c);
}

// Paints a slanted strip. t->a and t->b are its left and right edges at row t->i. The edges
// shift right by t->c pixels for each row above t->i. t->d is a vertical brightness gradient:
// with t->i at mid-layout, the colour scales from 1 + d at the top to 1 - d at the bottom.
static void paint_strip(const Thing* t, Clip c) {
    for (int y = c.y0; y < c.y1; y++) {
        float lean = (t->i - (y + 0.5f)) * t->c;
        uint8_t rgb[3];
        scaled(rgb, t->rgb, 1 + t->d * (t->i - y) / 540.0f);
        span(y, t->a + lean, t->b + lean, rgb, t->amount, c);
    }
}

static void paint(const Thing* t, Clip c) {
    if (c.x0 < t->x) c.x0 = t->x;
    if (c.y0 < t->y) c.y0 = t->y;
    if (c.x1 > t->x + t->w) c.x1 = t->x + t->w;
    if (c.y1 > t->y + t->h) c.y1 = t->y + t->h;
    if (c.x0 >= c.x1 || c.y0 >= c.y1)
        return;
    switch (t->kind) {
    case K_MASK: paint_mask(t, c); break;
    case K_RECT: paint_rect(t, c); break;
    case K_STRIP: paint_strip(t, c); break;
    }
}

// ------------------------------------------------------------------ dirty tracking

// Marks the tiles that overlap the pixels [x0, x1) x [y0, y1) as dirty.
static void mark(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > screen_w) x1 = screen_w;
    if (y1 > screen_h) y1 = screen_h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++)
        memset(tiles + (size_t)ty * tiles_w + x0 / TILE, 1, (size_t)((x1 - 1) / TILE - x0 / TILE + 1));
}

// Marks the tiles under rows [y0, y1) of a slanted strip. In each tile row it marks only the
// strip's horizontal extent, because the bounding box of a slanted strip is much wider than
// the strip. The rows follow the strip's edges regardless of t->y and t->h, so mark_change can
// also mark rows that the strip has left since the previous frame.
static void mark_strip(const Thing* t, int y0, int y1) {
    for (int y = y0; y < y1;) {
        int next = (y / TILE + 1) * TILE;
        if (next > y1)
            next = y1;
        float lean0 = (t->i - y) * t->c, lean1 = (t->i - next) * t->c;
        mark((int)floorf(t->a + fminf(lean0, lean1)) - 1, y, (int)ceilf(t->b + fmaxf(lean0, lean1)) + 1, next);
        y = next;
    }
}

static void mark_thing(const Thing* t) {
    if (t->kind == K_STRIP)
        mark_strip(t, t->y, t->y + t->h);
    else if (t->kind != K_NONE)
        mark(t->x, t->y, t->x + t->w, t->y + t->h);
}

// Marks the tiles affected by an entry that is a in this frame and was b in the previous one.
static void mark_change(const Thing* a, const Thing* b) {
    if (!memcmp(a, b, sizeof(Thing)))
        return;
    if (a->kind == K_STRIP && b->kind == K_STRIP) {
        // When only the vertical extent of a strip changed, mark only the rows between its old
        // and new edges.
        Thing same;
        memcpy(&same, b, sizeof same);              // bytewise copy, padding included, for the memcmp below
        same.y = a->y;
        same.h = a->h;
        if (!memcmp(a, &same, sizeof(Thing))) {
            int a0 = a->y, a1 = a->y + a->h, b0 = b->y, b1 = b->y + b->h;
            if (a1 <= b0 || b1 <= a0) {
                mark_strip(a, a0, a1);                  // disjoint: mark both extents
                mark_strip(a, b0, b1);
            } else {
                mark_strip(a, a0 < b0 ? a0 : b0, a0 < b0 ? b0 : a0);
                mark_strip(a, a1 < b1 ? a1 : b1, a1 < b1 ? b1 : a1);
            }
            return;
        }
    }
    mark_thing(a);
    mark_thing(b);
}

// ------------------------------------------------------------------ layout and timing

static Gray logo, label;

// NAME_GAP is the gap in pixels between the logo and the word ARCADE. The screensaver in
// main.cpp uses the same value. name_x centres a mask horizontally in the layout.
enum { NAME_GAP = 54 };
static int name_x(const Gray* g) { return (1920 - g->w) / 2; }

// Returns the start time of step k (1-based) of n. Step 1 starts at first. The steps up to
// n - 1 follow at even intervals, spaced so that step n would fall on expected. Step n waits
// for the end request. asked is the animation time of the end request (1e9 until then). Steps
// still pending at that time follow it at 0.14 s intervals, so a boot that is faster than
// expected still shows every step.
static double step_at(int k, int n, double first, double asked) {
    int undone = 0;
    for (int j = 1; j <= k; j++) {
        double planned = j < n ? first + (expected - first) * (j - 1) / (n - 1) : 1e9;
        if (planned <= asked) {
            if (j == k)
                return planned;
        } else {
            if (j == k)
                return asked + 0.14 * undone;
            undone++;
        }
    }
    return 1e9;
}

// ------------------------------------------------------------------ boot animation
//
// Six slanted dark green strips come down one after another behind the logo, the word ARCADE,
// the STARTING label and the progress bar. The strip geometry matches the console screen of
// the menu (main.cpp) including the gaps between strips, so the menu can bring its screenshots
// down over the last boot frame. STRIP_LEAN is tan(9 degrees).

static const float STRIP_LEFT = -160, STRIP_WIDTH = 2240.0f / 6, STRIP_LEAN = 0.158384f, STRIP_LINE = 6;
static const float STRIP_SHADE[6] = { 0.50f, 0.66f, 0.56f, 0.72f, 0.60f, 0.78f };
// From WAVE_FIRST on, every WAVE_EVERY seconds, a band of light sweeps down each strip in PASS
// seconds, starting NEXT seconds later on each strip to the right. A moving band dirties only
// the rows at its edges. Brightening a whole strip at once would repaint all of it, which
// costs a Pi 3 over 15 ms of the 16.7 ms frame time.
static const double WAVE_FIRST = 2.0, WAVE_EVERY = 3.4, PASS = 0.6, NEXT = 0.22;
enum { BAND = 220, TAIL = 180 };                // heights in pixels of the bright band and its dimmer tail

// pixel_word is the word ARCADE in Press Start 2P at 16 screen pixels per font pixel. Its seven
// rows of font pixels form four colour bands (yellow, orange, rose, purple) over a dark drop
// shadow offset by one font pixel to the right and down. tools/build_assets.py uses the same
// colours for the screensaver in the menu.
static Gray pixel_word;
static const uint8_t WORD_BANDS[4][3] = { { 249, 209, 89 }, { 243, 160, 88 }, { 216, 115, 166 }, { 161, 118, 210 } };
static const int BAND_FROM[5] = { 0, 2, 4, 6, 7 };          // start row of each band in font pixels, then the end row
static const uint8_t WORD_SHADOW[3] = { BRAND_INK };

static void paint_strips(double t, double end) {
    // asked is the animation time of the end request (1e9 before it). The first strip starts
    // at 0.4 s. Each strip takes 0.5 s to come down, so full_at is when the last one is down.
    double asked = end >= 0 ? t - end : 1e9;
    double full_at = step_at(6, 6, 0.4, asked) + 0.5;
    // Waves start only before the end request. A wave already under way finishes its sweep.
    int wave = t >= WAVE_FIRST ? (int)((t - WAVE_FIRST) / WAVE_EVERY) : -1;
    double wave_began = WAVE_FIRST + WAVE_EVERY * wave;
    ending = full_at + 0.05 - asked;                // ends 50 ms after the last strip is down

    for (int i = 0; i < 6; i++) {
        double since = t - step_at(i + 1, 6, 0.4, asked);
        // The strip slides down from the top in 0.5 s with a 6-pixel leading edge in the accent colour.
        float down = since < 0 ? 0 : ease_out((float)since / 0.5f);
        int cut = (int)(-60 + 1200 * down);
        uint8_t rgb[3];
        scaled(rgb, STRIP_RGB, STRIP_SHADE[i]);
        // The rightmost STRIP_LINE pixels stay black, where the menu draws the line between strips.
        float left = STRIP_LEFT + i * STRIP_WIDTH, right = STRIP_LEFT + (i + 1) * STRIP_WIDTH - STRIP_LINE;
        int box_x = (int)floorf(left - 600 * STRIP_LEAN) - 1, box_w = (int)ceilf(right - left + 1200 * STRIP_LEAN) + 3;
        int bottom = cut < 0 ? 0 : cut > 1080 ? 1080 : cut;
        int moving = since >= 0 && down < 1 && cut > 6 && cut < 1080;
        // While moving, the strip ends where its leading edge begins.
        Thing* strip = thing(K_STRIP, ox + box_x, oy + 0, box_w, moving ? bottom - 6 : bottom, rgb, since < 0 ? 0 : 256);
        strip->a = ox + left; strip->b = ox + right; strip->c = STRIP_LEAN; strip->d = 0.3f; strip->i = oy + 540;
        Thing* edge = thing(K_STRIP, ox + box_x, oy + bottom - 6, box_w, moving ? 6 : 0, ACCENT, moving ? 256 : 0);
        edge->a = ox + left; edge->b = ox + right; edge->c = STRIP_LEAN; edge->i = oy + 540;

        // The light band shows only on strips that are fully down. head is its lower edge, with
        // BAND bright rows above it and TAIL dimmer rows above those.
        double pass = wave >= 0 && wave_began < asked && since >= 0.5 ? (t - wave_began - NEXT * i) / PASS : -1;
        int lit = pass >= 0 && pass < 1;
        int head = (int)(-60 + pass * (1140 + BAND + TAIL));
        for (int part = 0; part < 2; part++) {
            int from = part ? head - BAND - TAIL : head - BAND, to = part ? head - BAND : head;
            if (from < 0) from = 0;
            if (to > 1080) to = 1080;
            uint8_t light[3];
            scaled(light, STRIP_RGB, STRIP_SHADE[i] * (part ? 1.3f : 1.75f));
            Thing* band = thing(K_STRIP, ox + box_x, oy + from, box_w, lit && to > from ? to - from : 0, light, lit ? 256 : 0);
            band->a = ox + left; band->b = ox + right; band->c = STRIP_LEAN; band->d = 0.3f; band->i = oy + 540;
        }
    }

    // The logo and the word ARCADE appear at full opacity at 0.15 s. A fade would repaint their
    // whole area every frame, which takes a Pi 3 a full frame time.
    float there = t >= 0.15 ? 1 : 0;
    int unit = pixel_word.h / 7;                                // screen pixels per font pixel
    int top = (1080 - (logo.h + NAME_GAP + pixel_word.h)) / 2;
    int word_x = (1920 - pixel_word.w) / 2, word_top = top + logo.h + NAME_GAP;
    mask(&logo, name_x(&logo), top, WHITE, there);
    mask(&pixel_word, word_x + unit, word_top + unit, WORD_SHADOW, there);
    for (int band = 0; band < 4; band++)
        rows(&pixel_word, BAND_FROM[band] * unit, BAND_FROM[band + 1] * unit, word_x, word_top, WORD_BANDS[band], there);

    // The STARTING label and the progress bar sit below the word. The LOADING bar in main.cpp
    // has the same geometry and timing.
    int label_y = word_top + pixel_word.h + unit + 54;
    mask(&label, (1920 - label.w) / 2, label_y, ACCENT, there);
    int bar_x = (1920 - 240) / 2, bar_y = label_y + label.h + 24;
    uint8_t track[3];
    scaled(track, ACCENT, 0.22f);
    rect(bar_x, bar_y, 240, 4, track, there);
    // The 72-pixel lit segment crosses the 240-pixel track from left to right once every 1.6 s,
    // eased with smoothstep.
    double at = t / 1.6;
    at -= floor(at);
    at = at * at * (3 - 2 * at);
    int from = bar_x - 72 + (int)(at * (240 + 72)), to = from + 72;
    if (from < bar_x) from = bar_x;
    if (to > bar_x + 240) to = bar_x + 240;
    rect(from, bar_y, to > from ? to - from : 0, 4, ACCENT, to > from ? there : 0);
}

static int opened = 0;

void looks_expect(double seconds) { expected = seconds < 3 ? 3 : seconds; }
double looks_ending(void) { return ending; }
const LookRect* looks_boxes(void) { return boxes; }
const uint8_t* looks_pixels(void) { return picture; }

void looks_close(void) {
    free(picture);
    free(tiles);
    free(cell_from);
    free(cell_next);
    picture = NULL;
    tiles = NULL;
    cell_from = NULL;
    cell_next = NULL;
    opened = 0;
}

// looks_open reads the masks once. Later calls reuse them, because in --early mode dir lies
// inside the initramfs, which boot.c leaves with a chroot.
int looks_open(const char* dir, int w, int h) {
    looks_close();
    if ((!logo.px && !read_pgm(dir, "logo.pgm", &logo)) || (!label.px && !read_pgm(dir, "label.pgm", &label)) ||
        (!pixel_word.px && !read_pgm(dir, "arcade-pixel.pgm", &pixel_word)))
        return 0;
    screen_w = w;
    screen_h = h;
    ox = (w - 1920) / 2;
    oy = (h - 1080) / 2;
    picture = calloc((size_t)w * h, 3);
    tiles_w = (w + TILE - 1) / TILE;
    tiles_h = (h + TILE - 1) / TILE;
    tiles = calloc((size_t)tiles_w * tiles_h, 1);
    cells_w = (w + CELL - 1) / CELL;
    cells_h = (h + CELL - 1) / CELL;
    cell_from = calloc((size_t)cells_w * cells_h + 1, sizeof(int));
    cell_next = calloc((size_t)cells_w * cells_h + 1, sizeof(int));
    if (!picture || !tiles || !cell_from || !cell_next)
        return 0;
    now_count = then_count = 0;
    first_frame = 1;
    ending = 0;
    opened = 1;
    return 1;
}

// Builds the spatial index: for each cell, the indices of the entries that overlap it, in
// paint order. Returns 0 when the index would exceed MOST_ENTRIES.
static int sort_into_cells(void) {
    int cells = cells_w * cells_h;
    memset(cell_from, 0, sizeof(int) * (size_t)(cells + 1));
    // Counting sort. Pass 0 counts the entries per cell and turns the counts into start
    // offsets. Pass 1 writes the entries.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < now_count; i++) {
            const Thing* t = &now_things[i];
            if (t->kind == K_NONE)
                continue;
            int x0 = t->x < 0 ? 0 : t->x, y0 = t->y < 0 ? 0 : t->y;
            int x1 = t->x + t->w > screen_w ? screen_w : t->x + t->w, y1 = t->y + t->h > screen_h ? screen_h : t->y + t->h;
            if (x0 >= x1 || y0 >= y1)
                continue;
            for (int cy = y0 / CELL; cy <= (y1 - 1) / CELL; cy++)
                for (int cx = x0 / CELL; cx <= (x1 - 1) / CELL; cx++) {
                    int cell = cy * cells_w + cx;
                    if (pass == 0)
                        cell_from[cell + 1]++;
                    else
                        cell_things[cell_next[cell]++] = (uint16_t)i;
                }
        }
        if (pass == 0) {
            for (int cell = 0; cell < cells; cell++)
                cell_from[cell + 1] += cell_from[cell];
            if (cell_from[cells] > MOST_ENTRIES)
                return 0;
            memcpy(cell_next, cell_from, sizeof(int) * (size_t)cells);
        }
    }
    return 1;
}

int looks_draw(double t, double end) {
    if (!opened)
        return 0;
    now_count = 0;
    paint_strips(t, end);
    // Entries with zero opacity or an empty box become K_NONE, so they compare equal to absent
    // entries whatever their other fields hold.
    for (int i = 0; i < now_count; i++)
        if (!now_things[i].amount || now_things[i].w <= 0 || now_things[i].h <= 0)
            memset(&now_things[i], 0, sizeof(Thing));

    // Mark the tiles where the display list differs from the previous frame. The first frame
    // repaints the whole screen.
    if (first_frame) {
        memset(tiles, 1, (size_t)tiles_w * tiles_h);
        first_frame = 0;
    } else {
        static const Thing none;                    // zero entry for the shorter list
        int most = now_count > then_count ? now_count : then_count;
        for (int i = 0; i < most; i++)
            mark_change(i < now_count ? &now_things[i] : &none, i < then_count ? &then_things[i] : &none);
    }

    // Merge the dirty tiles into rectangles. Each run of dirty tiles in a tile row becomes a
    // box. A run with the same x and width as a box ending at this row extends that box.
    box_count = 0;
    int row_from = 0;
    for (int ty = 0; ty < tiles_h; ty++) {
        int row_start = box_count;
        for (int tx = 0; tx < tiles_w; tx++) {
            if (!tiles[(size_t)ty * tiles_w + tx])
                continue;
            int from = tx;
            while (tx < tiles_w && tiles[(size_t)ty * tiles_w + tx])
                tx++;
            LookRect box = { from * TILE, ty * TILE, (tx - from) * TILE, TILE };
            if (box.x + box.w > screen_w) box.w = screen_w - box.x;
            if (box.y + box.h > screen_h) box.h = screen_h - box.y;
            int joined = 0;
            for (int i = row_from; i < row_start && !joined; i++)
                if (boxes[i].x == box.x && boxes[i].w == box.w && boxes[i].y + boxes[i].h == box.y) {
                    boxes[i].h += box.h;
                    // Move the grown box into this row's range so the next row can extend it.
                    LookRect grown = boxes[i];
                    boxes[i] = boxes[row_start - 1];
                    boxes[row_start - 1] = boxes[box_count - 1];
                    boxes[box_count - 1] = grown;
                    row_start--;
                    joined = 1;
                }
            if (!joined) {
                if (box_count == MOST_BOXES) {
                    // The box list is full: repaint the whole screen as one box.
                    box_count = 1;
                    boxes[0] = (LookRect){ 0, 0, screen_w, screen_h };
                    ty = tiles_h;
                    row_start = 0;
                    break;
                }
                boxes[box_count++] = box;
            }
        }
        row_from = row_start;
    }
    memset(tiles, 0, (size_t)tiles_w * tiles_h);

    // Repaint each box cell by cell: clear the area to black, then paint the overlapping
    // entries in display-list order. If sort_into_cells failed, each cell goes through the
    // whole display list.
    int sorted = box_count ? sort_into_cells() : 1;
    for (int b = 0; b < box_count; b++) {
        Clip box = { boxes[b].x, boxes[b].y, boxes[b].x + boxes[b].w, boxes[b].y + boxes[b].h };
        for (int cy = box.y0 / CELL; cy * CELL < box.y1; cy++)
            for (int cx = box.x0 / CELL; cx * CELL < box.x1; cx++) {
                Clip c = { cx * CELL, cy * CELL, (cx + 1) * CELL, (cy + 1) * CELL };
                if (c.x0 < box.x0) c.x0 = box.x0;
                if (c.y0 < box.y0) c.y0 = box.y0;
                if (c.x1 > box.x1) c.x1 = box.x1;
                if (c.y1 > box.y1) c.y1 = box.y1;
                for (int y = c.y0; y < c.y1; y++)
                    memset(picture + ((size_t)y * screen_w + c.x0) * 3, 0, (size_t)(c.x1 - c.x0) * 3);
                if (sorted) {
                    int cell = cy * cells_w + cx;
                    for (int e = cell_from[cell]; e < cell_from[cell + 1]; e++)
                        paint(&now_things[cell_things[e]], c);
                } else {
                    for (int i = 0; i < now_count; i++)
                        if (now_things[i].kind != K_NONE)
                            paint(&now_things[i], c);
                }
            }
    }
    memcpy(then_things, now_things, sizeof(Thing) * (size_t)now_count);
    then_count = now_count;
    return box_count;
}

// RGB565 has 32 levels of red and blue and 64 of green, so smooth gradients show bands.
// looks_to_565 dithers by adding a per-pixel threshold to each channel before truncating it.
// The thresholds come from a 256 x 256 table of interleaved gradient noise with values from 0
// to 63, scaled to one quantization step per channel. Its pattern is harder to see than that
// of a small Bayer matrix.
enum { SPREAD_SIZE = 256 };
static uint8_t* spread_table = NULL;

static void make_spread(void) {
    spread_table = malloc(SPREAD_SIZE * SPREAD_SIZE);
    if (!spread_table)
        exit(1);
    for (int y = 0; y < SPREAD_SIZE; y++)
        for (int x = 0; x < SPREAD_SIZE; x++) {
            double v = 0.06711056 * x + 0.00583715 * y;
            v = 52.9829189 * (v - floor(v));
            spread_table[y * SPREAD_SIZE + x] = (uint8_t)(64 * (v - floor(v)));
        }
}

void looks_to_565(const LookRect* box, uint8_t* out, int stride) {
    if (!spread_table)
        make_spread();
    for (int y = box->y; y < box->y + box->h; y++) {
        const uint8_t* src = picture + ((size_t)y * screen_w + box->x) * 3;
        const uint8_t* noise = spread_table + (y & (SPREAD_SIZE - 1)) * SPREAD_SIZE;
        uint8_t* dst = out + (size_t)y * stride + (size_t)box->x * 2;
        for (int x = box->x; x < box->x + box->w; x++, src += 3, dst += 2) {
            int spread = noise[x & (SPREAD_SIZE - 1)];
            int r = src[0] + (spread >> 3), g = src[1] + (spread >> 4), b = src[2] + (spread >> 3);
            unsigned v = (unsigned)(((r > 255 ? 255 : r) >> 3) << 11 | ((g > 255 ? 255 : g) >> 2) << 5 | (b > 255 ? 255 : b) >> 3);
            dst[0] = (uint8_t)(v & 0xFF);
            dst[1] = (uint8_t)(v >> 8);
        }
    }
}
