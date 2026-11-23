// launcher-boot: shows the boot animation (looks.h) while the Pi boots.
//
//   launcher-boot [DIR]          DIR holds the masks of art/boot
//   launcher-boot --early DIR    started from the initramfs, before the root filesystem is mounted
//
// The program runs for the first twenty seconds or so of the boot, until the menu takes over
// the display. It draws directly into the framebuffer of the Linux console (/dev/fb0). The menu
// creates MENU_STARTS when it begins loading, which starts the ending of the animation. Once
// the ending is over and the menu has loaded (MENU_READY), the program leaves its last frame in
// /dev/fb0, creates PICTURE_STANDS and exits. The menu reads that frame and continues from it.
//
// Early in the boot the vc4 DRM driver replaces the firmware framebuffer with a moment of
// black. The first frame therefore waits up to WAIT_FOR_DRIVER seconds for the driver's
// framebuffer. Kernel messages go to tty3. When another process overwrites the frame, the
// program redraws it. Started from the initramfs, the program chroots into the root filesystem
// once the initramfs has mounted it.
//
// It logs event times and frame pacing to /run/launcher-boot.log.

#define _GNU_SOURCE
#include <dirent.h>
#include <drm/drm.h>
#include <fcntl.h>
#include <linux/tiocl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "looks.h"

#define LAUNCHER_DIR "/opt/retropie/configs/all/arcade-launcher"
#define MENU_STARTS "/dev/shm/launcher-menu-starts"   // the menu has started loading
#define MENU_READY "/dev/shm/launcher-menu-ready"     // the menu has loaded and wants the display
#define PICTURE_STANDS "/dev/shm/launcher-boot-done"  // the last frame is in place for the menu
#define MENU_IS_UP "/dev/shm/launcher-booted"         // the menu shows its first frame
#define LAST_START LAUNCHER_DIR "/cache/boot-seconds" // previous boot: seconds until the menu started loading
#define GIVE_UP_AFTER 240.0                          // timeout in seconds in case the menu never starts
#define WAIT_FOR_DRIVER 4.0                          // maximum wait for the DRM driver's framebuffer, in seconds

static int fb_fd = -1;
static int fb_w = 0, fb_h = 0, fb_bits = 0, fb_stride = 0;
static uint8_t* out = NULL;         // the frame in the framebuffer's pixel format and stride
static uint8_t* fb_map = NULL;      // mmap of /dev/fb0, set once the DRM driver owns it
static size_t fb_map_size = 0;
static char events[32][240];
static int event_count = 0;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void note(const char* format, ...) {
    if (event_count >= 32)
        return;
    char text[200];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    snprintf(events[event_count++], sizeof events[0], "%.2f s: %s", now(), text);
}

static int read_number(const char* path, const char* format, int* a, int* b) {
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    int n = fscanf(f, format, a, b);
    fclose(f);
    return n;
}

// Reads the framebuffer geometry and pixel format from sysfs. Returns 1 when they changed to
// valid values.
static int look_at_format(void) {
    int w = 0, h = 0, bits = 0, stride = 0, unused = 0;
    if (read_number("/sys/class/graphics/fb0/virtual_size", "%d,%d", &w, &h) != 2 ||
        read_number("/sys/class/graphics/fb0/bits_per_pixel", "%d", &bits, &unused) != 1 ||
        read_number("/sys/class/graphics/fb0/stride", "%d", &stride, &unused) != 1)
        return 0;                                   // sysfs not readable yet: keep the last values
    if (w == fb_w && h == fb_h && bits == fb_bits && stride == fb_stride)
        return 0;
    if (w <= 0 || h <= 0 || (bits != 16 && bits != 24 && bits != 32) || stride < w * bits / 8)
        return 0;
    fb_w = w; fb_h = h; fb_bits = bits; fb_stride = stride;
    return 1;
}

// Returns 1 when fb0 is the DRM driver's framebuffer, which stays until shutdown. vc4 replaces
// the firmware's simplefb, whose name starts with "simple".
static int screen_stays(void) {
    char name[32] = "";
    FILE* f = fopen("/sys/class/graphics/fb0/name", "r");
    if (!f)
        return 0;
    if (!fgets(name, sizeof name, f))
        name[0] = 0;
    fclose(f);
    return name[0] && strncmp(name, "simple", 6) != 0;
}

// Reopens /dev/fb0 and drops the old mapping. While /dev/fb0 is missing, which happens for a
// moment when the boot leaves the initramfs, it keeps the old handle.
static void open_screen(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return;
    if (fb_map)
        munmap(fb_map, fb_map_size);
    fb_map = NULL;
    if (fb_fd >= 0)
        close(fb_fd);
    fb_fd = fd;
}

// Maps the framebuffer so that put() copies each row with memcpy in place of a pwrite system
// call. It maps only the DRM driver's framebuffer, since the firmware's simplefb disappears
// during the boot.
static void map_screen(void) {
    if (fb_map || fb_fd < 0 || !fb_stride || !screen_stays())
        return;
    size_t size = (size_t)fb_stride * fb_h;
    void* memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (memory != MAP_FAILED) {
        fb_map = memory;
        fb_map_size = size;
    }
}

// Converts one dirty rectangle into out. At 16 bpp it uses dithered RGB565. At 24 and 32 bpp it
// writes the bytes B, G, R, plus 0xFF at 32 bpp.
static void convert(const LookRect* box) {
    if (fb_bits == 16) {
        looks_to_565(box, out, fb_stride);
        return;
    }
    int bytes = fb_bits / 8;
    const uint8_t* picture = looks_pixels();
    for (int y = box->y; y < box->y + box->h; y++) {
        const uint8_t* src = picture + ((size_t)y * fb_w + box->x) * 3;
        uint8_t* dst = out + (size_t)y * fb_stride + (size_t)box->x * bytes;
        for (int x = 0; x < box->w; x++, src += 3, dst += bytes) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            if (bytes == 4)
                dst[3] = 255;
        }
    }
}

// Waits for the next vertical blank to avoid tearing. It uses the first CRTC from 0 to 3 that
// answers. Returns 0 when vblank waits fail, in which case the caller paces frames by the
// clock. Until the DRM device exists, each call tries to open it again.
static int wait_for_screen(void) {
    static int card = -1, output = -1, failed = 0;
    if (failed)
        return 0;
    if (card < 0) {
        card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (card < 0)
            return 0;
        // The first process to open the card becomes DRM master. Dropping master lets the menu take it.
        ioctl(card, DRM_IOCTL_DROP_MASTER, 0);
    }
    for (int tried = 0; tried < 4; tried++) {
        int which = output >= 0 ? output : tried;
        union drm_wait_vblank wait;
        memset(&wait, 0, sizeof wait);
        wait.request.type = _DRM_VBLANK_RELATIVE | (which << _DRM_VBLANK_HIGH_CRTC_SHIFT);
        wait.request.sequence = 1;
        if (ioctl(card, DRM_IOCTL_WAIT_VBLANK, &wait) == 0) {
            output = which;
            return 1;
        }
        if (output >= 0)
            break;
    }
    failed = 1;
    return 0;
}

// Copies a rectangle of out to the framebuffer. Returns 0 when a pwrite fails.
static int put(int x, int y, int w, int h) {
    int bytes = fb_bits / 8, ok = 1;
    if (fb_map) {
        for (int row = y; row < y + h; row++) {
            size_t at = (size_t)row * fb_stride + (size_t)x * bytes;
            memcpy(fb_map + at, out + at, (size_t)w * bytes);
        }
        return 1;
    }
    for (int row = y; row < y + h; row++) {
        off_t at = (off_t)row * fb_stride + (off_t)x * bytes;
        if (pwrite(fb_fd, out + at, (size_t)w * bytes, at) != (ssize_t)w * bytes)
            ok = 0;
    }
    return ok;
}

// Returns 1 while the framebuffer still holds the frame. It compares one row at mid-height
// through the logo, at most 760 pixels wide.
static int picture_is_there(void) {
    static uint8_t seen[1024 * 4];
    int bytes = fb_bits / 8;
    int width = fb_w < 760 ? fb_w : 760;
    size_t size = (size_t)width * bytes;
    off_t at = (off_t)(fb_h / 2) * fb_stride + (off_t)((fb_w - width) / 2) * bytes;
    if (pread(fb_fd, seen, size, at) != (ssize_t)size)
        return 0;
    return memcmp(seen, out + at, size) == 0;
}

// Returns 1 when console text has appeared. The console writes from the left edge, so it
// compares two pixel rows of every text line within the leftmost WIDTH pixels.
static int text_is_there(void) {
    enum { LINE = 16, WIDTH = 64 };                 // text line height and checked width, in pixels
    uint8_t seen[WIDTH * 4];
    int bytes = fb_bits / 8;
    if (fb_w < WIDTH)
        return 0;
    for (int y = 0; y + LINE <= fb_h; y += LINE) {
        for (int row = y + 6; row <= y + 10; row += 4) {
            off_t at = (off_t)row * fb_stride;
            if (pread(fb_fd, seen, (size_t)WIDTH * bytes, at) != (ssize_t)WIDTH * bytes)
                return 0;                           // read failed: picture_is_there() handles a lost framebuffer
            if (memcmp(seen, out + at, (size_t)WIDTH * bytes))
                return 1;
        }
    }
    return 0;
}

// Redirects kernel messages to tty3 with TIOCL_SETKMSGREDIRECT. Opening /dev/tty3 allocates
// that virtual console, which has to exist for the redirect.
static void send_kernel_messages_away(void) {
    int terminal = open("/dev/tty3", O_WRONLY | O_NOCTTY);
    if (terminal < 0)
        return;
    char request[2] = { TIOCL_SETKMSGREDIRECT, 3 };
    ioctl(terminal, TIOCLINUX, request);
    close(terminal);
}

// Returns 1 when another launcher-boot process runs. The program starts twice, from the
// initramfs and from systemd (pi/install.sh). The second instance exits. The check matches the
// basename of argv[0] in /proc/PID/cmdline, because the initramfs script that launches the
// program also has the comm name launcher-boot.
static int another_one_runs(void) {
    DIR* proc = opendir("/proc");
    if (!proc)
        return 0;
    int found = 0;
    struct dirent* entry;
    while ((entry = readdir(proc))) {
        int pid = atoi(entry->d_name);
        if (pid <= 0 || pid == getpid())
            continue;
        char path[64], started_as[256] = "";
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        FILE* f = fopen(path, "r");
        if (!f)
            continue;
        size_t got = fread(started_as, 1, sizeof started_as - 1, f);
        fclose(f);
        started_as[got] = 0;                        // argv[0] ends at the first NUL
        const char* name = strrchr(started_as, '/');
        if (!strcmp(name ? name + 1 : started_as, "launcher-boot"))
            found = 1;
    }
    closedir(proc);
    return found;
}

static void write_log(const char* path) {
    FILE* log = fopen(path, "w");
    if (!log)
        return;
    for (int i = 0; i < event_count; i++)
        fprintf(log, "%s\n", events[i]);
    fclose(log);
}

static void pause_ms(long ms) {
    struct timespec pause = { 0, ms * 1000 * 1000 };
    nanosleep(&pause, NULL);
}

int main(int argc, char** argv) {
    int early = argc > 2 && !strcmp(argv[1], "--early");
    const char* dir = argc > 1 ? argv[argc - 1] : LAUNCHER_DIR "/art/boot";

    if (another_one_runs())
        return 0;
    if (!early && access(MENU_IS_UP, F_OK) == 0)
        return 0;
    // SCHED_FIFO keeps the frame rate steady while the boot keeps all four cores busy. nice(-10)
    // is the fallback.
    struct sched_param first = { .sched_priority = 1 };
    if (sched_setscheduler(0, SCHED_FIFO, &first) != 0 && nice(-10) == -1) {}
    send_kernel_messages_away();

    int in_card = !early;                           // the root filesystem is reachable
    int opened = 0;                                 // the animation is open for the current framebuffer
    int drawn = 0, by_screen = 1;                   // drawn: full frame written; by_screen: vblank pacing
    double start = now(), began = 0;                // began: first frame, origin of animation time
    double last_frame = 0, longest = 0, painting = 0, most_painting = 0;
    double asked = -1;                              // animation time at which the menu started loading
    long frames = 0, late = 0;
    int quick_waits = 0, knows_last_start = 0;
    note(early ? "started from the initramfs" : "started by systemd");

    for (long tick = 0;; tick++) {
        double t = now();
        if (t - start > GIVE_UP_AFTER)
            break;
        if (in_card && access(MENU_IS_UP, F_OK) == 0) {
            note("the menu is up");
            break;
        }
        if (!in_card && access("/root/etc/os-release", F_OK) == 0 && chdir("/root") == 0 && chroot(".") == 0) {
            in_card = 1;
            note("moved into the card's file system");
        }
        if (in_card && !knows_last_start) {
            // Spread the strips over the duration of the previous boot.
            knows_last_start = 1;
            double seconds = 0;
            FILE* f = fopen(LAST_START, "r");
            if (f) {
                if (fscanf(f, "%lf", &seconds) == 1 && seconds >= 6 && seconds <= 120)
                    looks_expect(seconds);
                fclose(f);
            }
        }

        if (!began && !screen_stays() && t - start < WAIT_FOR_DRIVER) {
            pause_ms(10);
            continue;
        }

        int changed = (tick % 8 == 0 || !drawn) ? look_at_format() : 0;
        if (fb_fd < 0 || changed || !drawn)
            open_screen();
        if (changed || !opened) {
            if (!fb_w || !looks_open(dir, fb_w, fb_h)) {
                if (fb_w)
                    return 1;                       // masks missing or unreadable
                pause_ms(20);
                continue;
            }
            opened = 1;
            by_screen = 1;                          // a new framebuffer: try vblank pacing again
            quick_waits = 0;
            free(out);
            out = calloc((size_t)fb_stride, (size_t)fb_h);
            if (!out)
                return 1;
            drawn = 0;
            note("screen is %dx%d with %d bits", fb_w, fb_h, fb_bits);
        }
        if (fb_fd < 0) {
            pause_ms(20);
            continue;
        }
        if (!fb_map && (tick % 8 == 0 || !drawn))
            map_screen();
        if (!began)
            began = t;

        double look_t = t - began;
        if (asked < 0 && (access(MENU_STARTS, F_OK) == 0 || access(MENU_READY, F_OK) == 0)) {
            asked = look_t;
            note("the menu loads");
        }
        double end = asked >= 0 ? look_t - asked : -1;
        double from = now();
        int boxes = looks_draw(look_t, end);
        for (int i = 0; i < boxes; i++)
            convert(&looks_boxes()[i]);
        painting += now() - from;
        if (now() - from > most_painting && look_t > 0.1)
            most_painting = now() - from;

        // Count vblank waits that fail or return within 0.5 ms. After nine in a row, the clock
        // paces the frames.
        double waited_from = now();
        if (by_screen) {
            if (wait_for_screen() && now() - waited_from > 0.0005)
                quick_waits = 0;
            else if (++quick_waits > 8)
                by_screen = 0;
        }
        if (!by_screen) {
            double left = 1 / 60.0 - (now() - last_frame);
            if (left > 0.0005) {
                struct timespec pause = { 0, (long)(left * 1e9) };
                nanosleep(&pause, NULL);
            }
        }
        if (!drawn) {
            drawn = put(0, 0, fb_w, fb_h);
            if (drawn)
                note("drew the first frame");
        } else {
            for (int i = 0; i < boxes; i++)
                put(looks_boxes()[i].x, looks_boxes()[i].y, looks_boxes()[i].w, looks_boxes()[i].h);
        }
        double shown = now();
        if (last_frame > 0 && look_t > 0) {
            frames++;
            if (shown - last_frame > 0.025)
                late++;
            if (shown - last_frame > longest)
                longest = shown - last_frame;
        }
        last_frame = shown;

        // Hand the frame to the menu once the ending is over and the menu has loaded.
        if (drawn && asked >= 0 && end >= looks_ending() && access(MENU_READY, F_OK) == 0) {
            int said = open(PICTURE_STANDS, O_WRONLY | O_CREAT, 0644);
            if (said >= 0)
                close(said);
            note("left the frame to the menu");
            break;
        }

        // Redraw the whole frame when something else has written to the framebuffer.
        if (drawn && !picture_is_there()) {
            open_screen();
            drawn = 0;
            note("the frame was overwritten");
        }
        if (drawn && tick % 6 == 0 && text_is_there()) {
            drawn = 0;
            note("text on the screen");
        }
    }

    if (frames > 0)
        note("%ld frames, %.1f ms apart on average, %ld over 25 ms, longest gap %.0f ms, %.1f ms of work each and %.0f ms at most%s",
             frames, (last_frame - began) * 1000 / frames, late, longest * 1000, painting * 1000 / frames,
             most_painting * 1000, by_screen ? "" : ", paced by the clock");
    // Save the duration of this boot for pacing the next one.
    if (in_card && asked >= 6) {
        FILE* f = fopen(LAST_START, "w");
        if (f) {
            fprintf(f, "%.1f\n", asked);
            fclose(f);
        }
    }
    if (in_card)
        write_log("/run/launcher-boot.log");
    return 0;
}
