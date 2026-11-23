// images loads pictures asynchronously, with a texture cache in memory and a disk cache of
// finished pictures.
//
// Worker threads prepare each picture: PNG decoding, optional trimming, downscaling to display
// size and an optional blurred drop shadow. get() returns at once, which keeps the render loop
// running. pump() uploads finished pictures as textures. draw() skips a picture until it is
// ready.
//
// Pictures that took at least 80 ms to prepare go to the disk cache in their final form. A
// cached picture loads in a few milliseconds. Deleting the cache folder at any time is safe.
#pragma once

#include "gfx.h"

#include <cstddef>
#include <string>

struct Picture {
    Texture tex;
    Texture shadow;               // blurred alpha mask drawn under tex, id 0 without a shadow
    float w = 0, h = 0;           // draw size in canvas pixels
    float shadow_margin = 0;      // canvas pixels the shadow extends past each edge of tex
    float shadow_w = 0, shadow_h = 0;           // shadow draw size in canvas pixels
    float shadow_drop = 0, shadow_dark = 0;     // from the Shadow of the first get()
    bool ready = false;
    bool failed = false;          // missing or undecodable file
    unsigned last_used = 0;       // frame of the last get(), for eviction
    size_t bytes = 0;             // texture memory of tex and shadow
    float focus = 0.5f;           // SMOOTH: centre of the most detailed strip-wide slice, 0 (left) to 1 (right)
};

// Shadow describes a drop shadow under a picture. drop is the downward offset in canvas
// pixels. soft is the approximate Gaussian sigma of the blur in canvas pixels. dark is the
// opacity, 0..1.
struct Shadow {
    float drop, soft, dark;
};
const Shadow NO_SHADOW = { 0, 0, 0 };

namespace images {

// These flags for get() control how a picture fits into max_w x max_h.
enum {
    TRIM = 1,         // crop near-transparent margins first
    NO_GROW = 4,      // at most one canvas pixel per PNG pixel
    AS_IS = 8,        // keep the PNG size, ignoring max_w, max_h and the shadow (w and h are its pixel size)
    PIXELS = 16,      // GL_NEAREST magnification for pixel art
    SMOOTH = 32,      // with AS_IS, for screenshots: doubled with Scale2x, GL_LINEAR magnification, and a focus
};

// Starts the worker threads. init() creates cache_folder and its parent directory when they
// are missing. An empty or unwritable cache_folder disables the disk cache. A non-empty
// log_file receives one line per loaded picture with its load time.
void init(const std::string& cache_folder, const std::string& log_file);
void shutdown();                  // joins the worker threads, closes the log

// Returns the picture at path, fitted into max_w x max_h canvas pixels as the flags in how
// specify. The first call queues the load. It returns a Picture that is not ready yet. Call
// get() in every frame that draws the picture. The pointer stays valid for at least 180
// frames after the last call.
//
// ahead marks a prefetch, such as the pictures of the next game in a list. Workers start a
// prefetch only when every urgent request (ahead = false) has started. They take prefetches
// in the order of this frame's get() calls. Request the most wanted picture first, every
// frame. pump() erases a prefetch that is still queued when its requests stop, which
// invalidates its pointer.
Picture* get(const std::string& path, float max_w, float max_h, int how = 0, Shadow shadow = NO_SHADOW,
             bool ahead = false);

// Call once per frame. pump() uploads finished pictures as textures within a byte budget per
// frame, because each upload takes a few milliseconds. While moving is true, prefetched
// pictures wait, which keeps their uploads out of animation frames. It returns the number of
// pictures that became ready or failed.
int pump(bool moving);
void wait(Picture* p, int ms);    // blocks until p is ready or failed, at most ms milliseconds

void draw(Picture* p, float x, float y, float alpha = 1.0f);                  // draws the shadow first, alpha fades both

}  // namespace images
