// The boot animation of launcher-boot shows the brand logo and the word ARCADE over six slanted
// strips that come down one after another, with a STARTING label and a progress bar below.
//
// Every frame is a function of the time values passed to looks_draw. looks_draw repaints the
// regions that changed since the previous frame and returns them as dirty rectangles, so the
// caller copies only those to the framebuffer.
#ifndef LAUNCHER_LOOKS_H
#define LAUNCHER_LOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x, y, w, h;
} LookRect;

// Opens the animation for a screen of w x h pixels. dir holds the masks that
// tools/build_assets.py writes to art/boot. Returns 0 when a mask is missing or an allocation
// fails.
int looks_open(const char* dir, int w, int h);
void looks_close(void);

// Sets the expected time from the first frame to the end request, in seconds (at least 3).
// The strips come down at even intervals across that time. The last one waits for the end
// request.
void looks_expect(double seconds);

// Renders the frame at animation time t. end is negative until the caller requests the
// ending, then counts the seconds since that request. The ending lasts looks_ending()
// seconds. Returns the number of dirty rectangles that looks_boxes() holds.
int looks_draw(double t, double end);
double looks_ending(void);
const LookRect* looks_boxes(void);

// Returns the current frame as packed RGB888, w x h pixels.
const uint8_t* looks_pixels(void);

// Converts one rectangle of the frame to dithered RGB565 at the same position in out, a
// full-screen buffer whose rows are stride bytes apart.
void looks_to_565(const LookRect* box, uint8_t* out, int stride);

#ifdef __cplusplus
}
#endif

#endif
