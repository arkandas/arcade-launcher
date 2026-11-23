// launcher-load: animates the loading bar while a game starts, then prepares the frame that
// the display shows when the game ends.
//
// When a game starts, the menu copies its loading screen into the framebuffer of the Linux
// console (/dev/fb0). That frame stays visible from the moment the menu lets go of the display
// until the emulator draws, a few seconds on a Pi 3B+. Before starting this program, the menu
// writes the state of the bar to /dev/shm/launcher-load as one line:
//
//   x y w h part step period track_r track_g track_b lit_r lit_g lit_b began
//
// Positions and sizes are in screen pixels. part is the length of the lit part, which moves in
// steps of step pixels. period is the duration of one sweep in seconds. began is the
// CLOCK_MONOTONIC time at which the movement started. The program redraws the bar in the
// framebuffer about sixty times a second, so the bar keeps moving as it did in the menu.
//
// Once the emulator owns the display (pi/arcade.sh then creates /dev/shm/launcher-game-drew),
// the program copies the game list frame that the menu saved in /dev/shm/launcher-frame.fb into
// the framebuffer, out of sight, and exits. The display shows that frame when the game ends.
// The program also exits when /dev/shm/launcher-load disappears or after two minutes.
//
// A 1 after began marks a wait: the screen of a RetroPie tool that opens, or of the menu
// looking for new games. Then the program exits as soon as another program draws into the
// framebuffer, such as the tool's text screen, and leaves the framebuffer alone.

#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static const char* BAR_FILE = "/dev/shm/launcher-load";
static const char* GAME_DREW_FILE = "/dev/shm/launcher-game-drew";
static const char* RETURN_FRAME = "/dev/shm/launcher-frame.fb";

static double seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int read_numbers(const char* path, const char* format, int* a, int* b) {
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    int found = fscanf(f, format, a, b);
    fclose(f);
    return found;
}

// Writes one pixel in the framebuffer format. At 16 bpp that is RGB565. At 32 bpp it is the
// bytes B, G, R and 0xFF.
static void put(uint8_t* at, int bytes, const int rgb[3]) {
    if (bytes == 2) {
        unsigned v = (unsigned)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
        at[0] = (uint8_t)(v & 0xFF);
        at[1] = (uint8_t)(v >> 8);
    } else {
        at[0] = (uint8_t)rgb[2];
        at[1] = (uint8_t)rgb[1];
        at[2] = (uint8_t)rgb[0];
        at[3] = 255;
    }
}

// A grid of pixels outside the bar, read from the framebuffer one row at a time. When several
// of them change, another program draws. The grid leaves out the bottom rows, which
// pi/arcade.sh blanks below the last text row before a tool starts.
enum { SAMPLE_COLUMNS = 24, SAMPLE_ROWS = 14, SAMPLES = SAMPLE_COLUMNS * SAMPLE_ROWS };

static int read_samples(int fd, int fb_w, int fb_h, int stride, int bytes, int bar_y, int bar_h,
                        uint8_t* line, uint32_t* out) {
    int count = 0;
    for (int r = 0; r < SAMPLE_ROWS; r++) {
        int sy = 8 + (int)((long)(fb_h - 40) * r / (SAMPLE_ROWS - 1));
        if (sy >= bar_y - 4 && sy < bar_y + bar_h + 4)
            continue;
        if (pread(fd, line, (size_t)fb_w * bytes, (off_t)sy * stride) != (ssize_t)fb_w * bytes)
            return -1;
        for (int c = 0; c < SAMPLE_COLUMNS; c++) {
            int sx = 8 + (int)((long)(fb_w - 16) * c / (SAMPLE_COLUMNS - 1));
            uint32_t value = 0;
            for (int b = 0; b < bytes; b++)
                value |= (uint32_t)line[(size_t)sx * bytes + b] << (8 * b);
            out[count++] = value;
        }
    }
    return count;
}

// Copies a saved frame of exactly size bytes into the framebuffer.
static void restore_frame(int fd, const char* path, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    static uint8_t chunk[1 << 16];
    size_t done = 0, got;
    while (done < size && (got = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (done + got > size || pwrite(fd, chunk, got, (off_t)done) != (ssize_t)got)
            break;
        done += got;
    }
    fclose(f);
}

int main(void) {
    int x, y, w, h, part, step, track[3], lit[3], wait = 0;
    double period, began;
    FILE* f = fopen(BAR_FILE, "r");
    if (!f)
        return 1;
    int found = fscanf(f, "%d %d %d %d %d %d %lf %d %d %d %d %d %d %lf %d", &x, &y, &w, &h, &part, &step, &period,
                       &track[0], &track[1], &track[2], &lit[0], &lit[1], &lit[2], &began, &wait);
    fclose(f);
    if (found < 14 || w <= 0 || h <= 0 || step <= 0 || period <= 0)
        return 1;

    int fb_w = 0, fb_h = 0, bits = 0, stride = 0, unused = 0;
    if (read_numbers("/sys/class/graphics/fb0/virtual_size", "%d,%d", &fb_w, &fb_h) != 2 ||
        read_numbers("/sys/class/graphics/fb0/bits_per_pixel", "%d", &bits, &unused) != 1 ||
        read_numbers("/sys/class/graphics/fb0/stride", "%d", &stride, &unused) != 1)
        return 1;
    int bytes = bits / 8;
    if ((bytes != 2 && bytes != 4) || x < 0 || y < 0 || x + w > fb_w || y + h > fb_h)
        return 1;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return 1;
    uint8_t* row = malloc((size_t)w * bytes);
    uint8_t* line = malloc((size_t)fb_w * bytes);
    if (!row || !line)
        return 1;
    static uint32_t before[SAMPLES], now[SAMPLES];
    int samples = wait == 1 ? read_samples(fd, fb_w, fb_h, stride, bytes, y, h, line, before) : -1;

    double start = seconds();
    int other = 0;          // another program has drawn
    while (seconds() - start < 120 && access(GAME_DREW_FILE, F_OK) != 0 && access(BAR_FILE, F_OK) == 0) {
        // In a wait, look before every frame whether another program has drawn.
        if (samples > 0 && read_samples(fd, fb_w, fb_h, stride, bytes, y, h, line, now) == samples) {
            int changed = 0;
            for (int i = 0; i < samples; i++)
                changed += now[i] != before[i];
            if (changed >= 8) {
                other = 1;
                break;
            }
        }
        // The motion matches load_bar_part in main.cpp. Once per period the lit part crosses the
        // track from left to right, eased with smoothstep, in steps of step pixels.
        double at = (seconds() - began) / period;
        at -= floor(at);
        at = at * at * (3 - 2 * at);
        int from = (int)(floor((at * (w + part) - part) / step) * step), to = from + part;
        for (int i = 0; i < w; i++)
            put(row + (size_t)i * bytes, bytes, i >= from && i < to ? lit : track);
        for (int j = 0; j < h; j++)
            if (pwrite(fd, row, (size_t)w * bytes, (off_t)(y + j) * stride + (off_t)x * bytes) < 0)
                break;
        usleep(16000);
    }
    if (wait != 1 && access(GAME_DREW_FILE, F_OK) == 0)
        restore_frame(fd, RETURN_FRAME, (size_t)stride * fb_h);
    // The other program may have cleared the screen just before the last bar went in. The row
    // just above the bar shows its new background, which then covers the bar.
    if (other && y > 0 && pread(fd, row, (size_t)w * bytes, (off_t)(y - 1) * stride + (off_t)x * bytes) == (ssize_t)w * bytes)
        for (int j = 0; j < h; j++)
            if (pwrite(fd, row, (size_t)w * bytes, (off_t)(y + j) * stride + (off_t)x * bytes) < 0)
                break;
    free(row);
    free(line);
    close(fd);
    return 0;
}
