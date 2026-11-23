// images.cpp implements images.h. Worker threads prepare pictures: libpng decoding, trimming,
// an area-averaging downscale and a box-blurred shadow. The memory cache belongs to the
// render thread, which uploads results in pump(). The disk cache keeps finished pictures in a
// QOI-style encoding.
#include "images.h"

#include <png.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// MOST_BYTES and KEEP_BYTES bound the texture memory of the cache. Above MOST_BYTES,
// forget_old() evicts the least recently requested pictures until the total drops to
// KEEP_BYTES.
const size_t MOST_BYTES = 72u << 20;
const size_t KEEP_BYTES = 48u << 20;
const unsigned FRESH_FRAMES = 180;        // forget_old() keeps pictures requested this recently
const size_t FRAME_BYTES = 1200u << 10;   // upload budget per pump(), keeps uploads from dropping frames

// The constants below configure the disk cache.
const double WORTH_KEEPING_MS = 80;                      // cache pictures that took at least this long to prepare
const unsigned long long ROOM_TO_LEAVE = 1ull << 30;     // stop writing below 1 GiB of free space
const char KEPT_MAGIC[4] = { 'A', 'P', 'I', 'C' };
const uint32_t KEPT_VERSION = 2;                         // increment when the preparation or the file format changes

// Three workers on the four cores of the Pi 3 leave one core for the render thread. Worker 0
// takes only urgent requests, so prefetch work can never occupy every worker.
const int HELPERS = 3;

typedef std::chrono::steady_clock Clock;

// A request starts as URGENT or LATER (prefetch). A worker sets WORKING when it takes the
// request. upload() sets DONE.
enum State { URGENT, LATER, WORKING, DONE };

// An Item holds one request and its picture. Workers read the request fields during load().
// The render thread owns the Picture.
struct Item {
    Picture picture;
    std::string key, path;
    float max_w = 0, max_h = 0, scale = 1;
    int how = 0;
    float shadow_soft = 0;
    std::atomic<int> state { URGENT };    // queue transitions happen under the mutex
    unsigned wished = 0;                  // last frame with a prefetch request for it
    unsigned needed = 0;                  // last frame with an urgent request for it
};

// load() returns a Result, which a worker passes to the render thread through results.
struct Result {
    std::string key;
    std::vector<uint8_t> rgba;        // premultiplied
    int w = 0, h = 0;
    float draw_w = 0, draw_h = 0;     // canvas pixels
    std::vector<uint8_t> shade;       // shadow alpha mask at quarter resolution
    int shade_w = 0, shade_h = 0;
    float shade_margin = 0;           // canvas pixels
    float focus = 0.5f;               // see Picture::focus
    bool failed = false;
};

std::unordered_map<std::string, std::unique_ptr<Item>> cache;     // render thread only
size_t cache_bytes = 0;
std::vector<Item*> wishes, old_wishes;                            // prefetch requests of this frame and the previous one
std::deque<Item*> urgent, later;                                  // guarded by mutex, as are results and quitting
std::deque<Result> results;
std::mutex mutex;
std::condition_variable wake;
std::thread workers[HELPERS];
bool quitting = false;
unsigned frame = 0;                                               // pump() count, time base of last_used, wished and needed
std::string kept_folder;                                          // disk cache directory, empty when disabled
FILE* log_file = nullptr;
Clock::time_point started;

double ms_since(Clock::time_point then) {
    return std::chrono::duration<double, std::milli>(Clock::now() - then).count();
}

// Decodes a PNG to straight-alpha RGBA8 with the libpng simplified API.
bool read_png(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h) {
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str()))
        return false;
    image.format = PNG_FORMAT_RGBA;
    rgba.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    w = (int)image.width;
    h = (int)image.height;
    return w > 0 && h > 0;
}

// Crops margins of near-transparent pixels (alpha <= 8).
void trim(std::vector<uint8_t>& rgba, int& w, int& h) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            if (row[x * 4 + 3] > 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0 || (x0 == 0 && y0 == 0 && x1 == w - 1 && y1 == h - 1))
        return;
    int nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    std::vector<uint8_t> out((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++)
        memcpy(&out[(size_t)y * nw * 4], &rgba[((size_t)(y + y0) * w + x0) * 4], (size_t)nw * 4);
    rgba.swap(out);
    w = nw;
    h = nh;
}

void premultiply(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        unsigned a = rgba[i + 3];
        if (a == 255)
            continue;
        rgba[i] = (uint8_t)((rgba[i] * a + 127) / 255);
        rgba[i + 1] = (uint8_t)((rgba[i + 1] * a + 127) / 255);
        rgba[i + 2] = (uint8_t)((rgba[i + 2] * a + 127) / 255);
    }
}

// Spans holds the area-averaging filter taps along one axis. Destination pixel i covers
// count[i] source pixels from first[i]. Its weights are fixed point values with 12 fraction
// bits that sum to exactly 4096.
struct Spans {
    std::vector<int> first, count;
    std::vector<uint16_t> weight;         // all taps, in destination pixel order
};

Spans spans(int from, int to) {
    Spans s;
    s.first.resize(to);
    s.count.resize(to);
    for (int i = 0; i < to; i++) {
        double a = i * (double)from / to, b = (i + 1) * (double)from / to;
        int i0 = (int)floor(a), i1 = (int)ceil(b);
        if (i1 > from) i1 = from;
        if (i1 <= i0) i1 = i0 + 1;
        size_t start = s.weight.size();
        int total = 0, largest = 0;
        for (int j = i0; j < i1; j++) {
            double part = std::min(b, j + 1.0) - std::max(a, (double)j);
            int w = (int)lround(part / (b - a) * 4096);
            if (w < 0) w = 0;
            s.weight.push_back((uint16_t)w);
            total += w;
            if (w > s.weight[start + largest]) largest = j - i0;
        }
        // Adding the rounding error to the largest tap keeps the sum at 4096, which preserves
        // brightness.
        s.weight[start + largest] = (uint16_t)(s.weight[start + largest] + 4096 - total);
        s.first[i] = i0;
        s.count[i] = i1 - i0;
    }
    return s;
}

// Downscales a premultiplied picture by area averaging, a horizontal pass followed by a
// vertical one. Averaging premultiplied values keeps the colour of transparent pixels out of
// the result. The fixed-point integer math runs several times faster on the Pi than
// floating point.
void shrink(const std::vector<uint8_t>& src, int sw, int sh, std::vector<uint8_t>& dst, int dw, int dh) {
    Spans across = spans(sw, dw), down = spans(sh, dh);

    std::vector<uint16_t> rows((size_t)dw * sh * 4);          // horizontal pass output, 8.8 fixed point
    for (int y = 0; y < sh; y++) {
        const uint8_t* in = &src[(size_t)y * sw * 4];
        uint16_t* out = &rows[(size_t)y * dw * 4];
        const uint16_t* w = across.weight.data();
        for (int x = 0; x < dw; x++, out += 4) {
            const uint8_t* p = in + (size_t)across.first[x] * 4;
            uint32_t sum[4] = { 8, 8, 8, 8 };                 // rounds the >> 4 to nearest
            for (int k = across.count[x]; k > 0; k--, p += 4, w++) {
                sum[0] += p[0] * *w;
                sum[1] += p[1] * *w;
                sum[2] += p[2] * *w;
                sum[3] += p[3] * *w;
            }
            out[0] = (uint16_t)(sum[0] >> 4);
            out[1] = (uint16_t)(sum[1] >> 4);
            out[2] = (uint16_t)(sum[2] >> 4);
            out[3] = (uint16_t)(sum[3] >> 4);
        }
    }

    dst.resize((size_t)dw * dh * 4);
    std::vector<uint32_t> sum((size_t)dw * 4);
    const uint16_t* w = down.weight.data();
    const int values = dw * 4;
    for (int y = 0; y < dh; y++) {
        std::fill(sum.begin(), sum.end(), 1u << 19);          // rounds the >> 20 to nearest
        for (int k = 0; k < down.count[y]; k++, w++) {
            const uint16_t* in = &rows[(size_t)(down.first[y] + k) * dw * 4];
            const uint32_t weight = *w;
            for (int i = 0; i < values; i++)
                sum[i] += in[i] * weight;
        }
        uint8_t* out = &dst[(size_t)y * dw * 4];
        for (int i = 0; i < values; i++)
            out[i] = (uint8_t)(sum[i] >> 20);
    }
}

// One box-blur pass of radius r with a running sum, along rows or, with down set, along
// columns. Samples outside the line count as zero.
void blur_lines(const std::vector<float>& from, std::vector<float>& to, int w, int h, int r, bool down) {
    int n = down ? h : w, lines = down ? w : h;
    for (int line = 0; line < lines; line++) {
        auto at = [&](int i) { return down ? (size_t)i * w + line : (size_t)line * w + i; };
        float sum = 0;
        for (int i = 0; i <= r && i < n; i++) sum += from[at(i)];
        for (int i = 0; i < n; i++) {
            to[at(i)] = sum / (2 * r + 1);
            if (i + r + 1 < n) sum += from[at(i + r + 1)];
            if (i - r >= 0) sum -= from[at(i - r)];
        }
    }
}

const int SHADOW_SCALE = 4;     // the shadow mask has 1/SHADOW_SCALE of the picture's resolution

// Renders the shadow mask. It box-filters the alpha channel to quarter resolution, then blurs
// it with three box passes per axis. soft is the blur sigma in screen pixels.
void make_shadow(const std::vector<uint8_t>& rgba, int w, int h, float soft, Result& out) {
    const int Q = SHADOW_SCALE;
    float sigma = soft / Q;
    int r = (int)lroundf((sqrtf(4 * sigma * sigma + 1) - 1) / 2);     // three box passes of this radius approximate sigma
    if (r < 1) r = 1;
    int margin = 3 * r;                                               // three passes spread the mask by up to 3r
    int qw = (w + Q - 1) / Q, qh = (h + Q - 1) / Q;
    int ow = qw + 2 * margin, oh = qh + 2 * margin;
    std::vector<float> a((size_t)ow * oh, 0.0f), b((size_t)ow * oh, 0.0f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            a[(size_t)(y / Q + margin) * ow + x / Q + margin] += rgba[((size_t)y * w + x) * 4 + 3] / (255.0f * Q * Q);
    for (int pass = 0; pass < 3; pass++) {
        blur_lines(a, b, ow, oh, r, false);
        blur_lines(b, a, ow, oh, r, true);
    }
    out.shade.resize((size_t)ow * oh);
    for (size_t i = 0; i < out.shade.size(); i++)
        out.shade[i] = (uint8_t)lroundf(fminf(1.0f, a[i]) * 255.0f);
    out.shade_w = ow;
    out.shade_h = oh;
    out.shade_margin = (float)(margin * Q);        // screen pixels, prepare() converts them to canvas pixels
}

// ------------------------------------------------------------------ disk cache
//
// Cached pictures use the QOI chunk encoding without QOI's header and end marker. Each chunk
// encodes a run of the previous pixel, an index into a table of 64 recent colours, a small
// delta from the previous pixel or a literal value. Decoding is a single loop, which the Pi
// runs many times faster than libpng decodes a PNG.

// Returns the colour table index of a pixel (the QOI hash).
inline int slot(const uint8_t* p) {
    return (p[0] * 3 + p[1] * 5 + p[2] * 7 + p[3] * 11) & 63;
}

void pack(const std::vector<uint8_t>& rgba, std::vector<uint8_t>& out) {
    uint8_t recent[64][4];
    memset(recent, 0, sizeof recent);
    uint8_t before[4] = { 0, 0, 0, 255 };
    int run = 0;
    const size_t count = rgba.size() / 4;
    out.clear();
    out.reserve(rgba.size() / 2);
    for (size_t i = 0; i < count; i++) {
        const uint8_t* p = &rgba[i * 4];
        if (memcmp(p, before, 4) == 0) {
            if (++run == 62) {
                out.push_back((uint8_t)(0xC0 | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run) {
            out.push_back((uint8_t)(0xC0 | (run - 1)));
            run = 0;
        }
        int s = slot(p);
        if (memcmp(recent[s], p, 4) == 0) {
            out.push_back((uint8_t)s);
        } else {
            memcpy(recent[s], p, 4);
            int dr = (int8_t)(p[0] - before[0]), dg = (int8_t)(p[1] - before[1]), db = (int8_t)(p[2] - before[2]);
            int dr_g = dr - dg, db_g = db - dg;
            if (p[3] != before[3]) {
                out.push_back(0xFF);
                out.insert(out.end(), p, p + 4);
            } else if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                out.push_back((uint8_t)(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
            } else if (dg >= -32 && dg <= 31 && dr_g >= -8 && dr_g <= 7 && db_g >= -8 && db_g <= 7) {
                out.push_back((uint8_t)(0x80 | (dg + 32)));
                out.push_back((uint8_t)(((dr_g + 8) << 4) | (db_g + 8)));
            } else {
                out.push_back(0xFE);
                out.insert(out.end(), p, p + 3);
            }
        }
        memcpy(before, p, 4);
    }
    if (run)
        out.push_back((uint8_t)(0xC0 | (run - 1)));
}

// Decodes count pixels. It returns false when the data ends early or a run overflows count.
bool unpack(const uint8_t* in, size_t size, std::vector<uint8_t>& rgba, size_t count) {
    uint8_t recent[64][4];
    memset(recent, 0, sizeof recent);
    uint8_t px[4] = { 0, 0, 0, 255 };
    rgba.resize(count * 4);
    uint8_t* out = rgba.data();
    size_t at = 0, done = 0;
    while (done < count) {
        if (at >= size)
            return false;
        uint8_t op = in[at++];
        if (op >= 0xC0 && op < 0xFE) {
            size_t run = (size_t)(op & 0x3F) + 1;
            if (run > count - done)
                return false;
            for (size_t i = 0; i < run; i++, out += 4)
                memcpy(out, px, 4);
            done += run;
            continue;
        }
        if (op < 0x40) {
            memcpy(px, recent[op], 4);
        } else {
            if (op == 0xFF) {
                if (at + 4 > size) return false;
                memcpy(px, in + at, 4);
                at += 4;
            } else if (op == 0xFE) {
                if (at + 3 > size) return false;
                memcpy(px, in + at, 3);
                at += 3;
            } else if (op < 0x80) {
                px[0] = (uint8_t)(px[0] + ((op >> 4) & 3) - 2);
                px[1] = (uint8_t)(px[1] + ((op >> 2) & 3) - 2);
                px[2] = (uint8_t)(px[2] + (op & 3) - 2);
            } else {
                if (at >= size) return false;
                uint8_t next = in[at++];
                int dg = (op & 0x3F) - 32;
                px[0] = (uint8_t)(px[0] + dg - 8 + (next >> 4));
                px[1] = (uint8_t)(px[1] + dg);
                px[2] = (uint8_t)(px[2] + dg - 8 + (next & 0x0F));
            }
            memcpy(recent[slot(px)], px, 4);
        }
        memcpy(out, px, 4);
        out += 4;
        done++;
    }
    return true;
}

// KeptHead starts every disk cache file. write_kept() writes it as raw memory in native byte
// order. The encoded pixels follow, then the shadow mask at one byte per pixel.
struct KeptHead {
    char magic[4];
    uint32_t version;
    uint32_t w, h;
    float draw_w, draw_h;
    uint32_t shade_w, shade_h;
    float shade_margin;
    float focus;
    uint32_t packed;      // size of the encoded pixels in bytes
};

// Returns the cache file path for a request. The name is a 64-bit FNV-1a hash of the PNG
// path, its mtime, its size and every parameter that affects the prepared pixels, so a
// modified PNG or a new display size maps to a new file. The hash leaves out PIXELS, which
// only changes the texture filter. The top byte of the hash selects one of 256 subdirectories.
std::string kept_name(const Item& item, const struct stat& st) {
    char about[200];
    snprintf(about, sizeof about, "|%lld|%ld|%lld|%.0fx%.0f|%d|%.1f|%.3f|%u", (long long)st.st_mtime,
             (long)st.st_mtim.tv_nsec, (long long)st.st_size, item.max_w, item.max_h, item.how & ~images::PIXELS,
             item.shadow_soft, item.scale, KEPT_VERSION);
    std::string text = item.path + about;
    uint64_t hash = 14695981039346656037ull;        // FNV-1a 64-bit offset basis
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    char name[48];
    snprintf(name, sizeof name, "/%02x/%014llx.pic", (unsigned)(hash >> 56), (unsigned long long)(hash & 0x00FFFFFFFFFFFFFFull));
    return kept_folder + name;
}

// Reads a cache file. The header checks reject foreign, outdated and corrupt files before any
// large allocation.
bool read_kept(const std::string& file, Result& r) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f)
        return false;
    KeptHead head;
    bool good = fread(&head, sizeof head, 1, f) == 1 && memcmp(head.magic, KEPT_MAGIC, 4) == 0
                && head.version == KEPT_VERSION && head.w >= 1 && head.w <= 8192 && head.h >= 1 && head.h <= 8192
                && head.shade_w <= 8192 && head.shade_h <= 8192 && head.packed <= (64u << 20);
    std::vector<uint8_t> packed;
    if (good) {
        packed.resize(head.packed);
        r.shade.resize((size_t)head.shade_w * head.shade_h);
        good = fread(packed.data(), 1, packed.size(), f) == packed.size()
               && fread(r.shade.data(), 1, r.shade.size(), f) == r.shade.size();
    }
    fclose(f);
    if (!good || !unpack(packed.data(), packed.size(), r.rgba, (size_t)head.w * head.h))
        return false;
    r.w = (int)head.w;
    r.h = (int)head.h;
    r.draw_w = head.draw_w;
    r.draw_h = head.draw_h;
    r.shade_w = (int)head.shade_w;
    r.shade_h = (int)head.shade_h;
    r.shade_margin = head.shade_margin;
    r.focus = head.focus;
    return true;
}

// Writes a cache file, skipping the write when free space is under ROOM_TO_LEAVE. rename()
// moves the finished .part file into place, so read_kept() never opens a partly written file.
bool write_kept(const std::string& file, const Result& r) {
    struct statvfs room;
    if (statvfs(kept_folder.c_str(), &room) != 0 || (unsigned long long)room.f_bavail * room.f_frsize < ROOM_TO_LEAVE)
        return false;
    std::vector<uint8_t> packed;
    pack(r.rgba, packed);
    KeptHead head;
    memcpy(head.magic, KEPT_MAGIC, 4);
    head.version = KEPT_VERSION;
    head.w = (uint32_t)r.w;
    head.h = (uint32_t)r.h;
    head.draw_w = r.draw_w;
    head.draw_h = r.draw_h;
    head.shade_w = (uint32_t)r.shade_w;
    head.shade_h = (uint32_t)r.shade_h;
    head.shade_margin = r.shade_margin;
    head.focus = r.focus;
    head.packed = (uint32_t)packed.size();

    mkdir(file.substr(0, file.rfind('/')).c_str(), 0755);
    std::string part = file + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f)
        return false;
    bool good = fwrite(&head, sizeof head, 1, f) == 1 && fwrite(packed.data(), 1, packed.size(), f) == packed.size()
                && fwrite(r.shade.data(), 1, r.shade.size(), f) == r.shade.size();
    good = fclose(f) == 0 && good;
    if (good && rename(part.c_str(), file.c_str()) == 0)
        return true;
    remove(part.c_str());
    return false;
}

// ------------------------------------------------------------------ loading

// Appends a load-log line: seconds since init(), load time, source (kept, png or bad) and the
// last two components of the path.
void note(const char* how, const Item& item, Clock::time_point since) {
    if (!log_file)
        return;
    size_t cut = item.path.rfind('/');
    if (cut != std::string::npos && cut > 0)
        cut = item.path.rfind('/', cut - 1);
    fprintf(log_file, "%9.3f s %5.0f ms  %-5s %s\n", ms_since(started) / 1000, ms_since(since), how,
            item.path.c_str() + (cut == std::string::npos ? 0 : cut + 1));
    fflush(log_file);
}

// Doubles a picture with Scale2x (EPX), which rounds the stair steps of pixel art where a
// plain doubling keeps them square.
void scale2x(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out) {
    std::vector<uint32_t> src((size_t)w * h), dst((size_t)w * h * 4);
    memcpy(src.data(), rgba.data(), src.size() * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t e = src[(size_t)y * w + x];
            uint32_t up = y > 0 ? src[(size_t)(y - 1) * w + x] : e, down = y < h - 1 ? src[(size_t)(y + 1) * w + x] : e;
            uint32_t left = x > 0 ? src[(size_t)y * w + x - 1] : e, right = x < w - 1 ? src[(size_t)y * w + x + 1] : e;
            uint32_t e0 = e, e1 = e, e2 = e, e3 = e;
            if (up != down && left != right) {
                e0 = left == up ? left : e;
                e1 = up == right ? right : e;
                e2 = left == down ? left : e;
                e3 = down == right ? right : e;
            }
            uint32_t* o = &dst[(size_t)(2 * y) * (2 * w) + 2 * x];
            o[0] = e0;
            o[1] = e1;
            o[2 * w] = e2;
            o[2 * w + 1] = e3;
        }
    }
    out.resize(dst.size() * 4);
    memcpy(out.data(), dst.data(), out.size());
}

// Returns the horizontal centre (0 to 1) of the strip-wide slice of a screenshot with the most
// detail: colour edges and saturation, summed per column. The console screen darkens the top
// and the bottom of its strips, so the rows in between count most. A mild preference for the
// middle settles near ties.
float detail_focus(const std::vector<uint8_t>& rgba, int w, int h) {
    const float STRIP_ASPECT = 0.31f;     // width over height of a strip on the console screen
    int window = (int)lroundf(h * STRIP_ASPECT);
    if (window < 1 || window >= w || h < 2)
        return 0.5f;
    std::vector<double> column(w, 0.0);
    for (int y = 1; y < h; y++) {
        float t = (float)y / h;
        float weight = t < 0.25f || t > 0.7f ? 0.3f : 1.0f;
        const uint8_t* row = &rgba[(size_t)y * w * 4];
        const uint8_t* above = row - (size_t)w * 4;
        for (int x = 1; x < w; x++) {
            const uint8_t* p = row + x * 4;
            const uint8_t* left = p - 4;
            const uint8_t* up = above + x * 4;
            int edge = 0;
            for (int c = 0; c < 3; c++)
                edge += abs(p[c] - left[c]) + abs(p[c] - up[c]);
            int saturation = std::max({ p[0], p[1], p[2] }) - std::min({ p[0], p[1], p[2] });
            column[x] += weight * (edge + 0.5 * saturation);
        }
    }
    double sum = 0, best = -1;
    int best_at = 0;
    for (int x = 0; x < window; x++)
        sum += column[x];
    for (int x0 = 0; x0 + window <= w; x0++) {
        if (x0 > 0)
            sum += column[x0 + window - 1] - column[x0 - 1];
        double centre = (x0 + window / 2.0) / w;
        double score = sum * (1 - 0.35 * fabs(centre - 0.5) * 2);
        if (score > best) {
            best = score;
            best_at = x0;
        }
    }
    return (best_at + window / 2.0f) / w;
}

bool prepare(const Item& item, Result& r) {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!read_png(item.path, rgba, w, h))
        return false;
    if (item.how & images::TRIM)
        trim(rgba, w, h);
    premultiply(rgba);

    if (item.how & images::AS_IS) {
        r.w = w;
        r.h = h;
        r.draw_w = (float)w;
        r.draw_h = (float)h;
        // SMOOTH doubles small screenshots only: a doubled 400 x 300 picture is under 2 MB.
        if ((item.how & images::SMOOTH) && w * h <= 400 * 300) {
            r.focus = detail_focus(rgba, w, h);
            scale2x(rgba, w, h, r.rgba);
            r.w = 2 * w;
            r.h = 2 * h;
            return true;
        }
        if (item.how & images::SMOOTH)
            r.focus = detail_focus(rgba, w, h);
        r.rgba.swap(rgba);
        return true;
    }

    // fitted is the scale from PNG pixels to canvas pixels.
    float fitted = fminf(item.max_w / w, item.max_h / h);
    if ((item.how & images::NO_GROW) && fitted > 1)
        fitted = 1;
    r.draw_w = roundf(w * fitted);
    r.draw_h = roundf(h * fitted);
    int tw = (int)lroundf(r.draw_w * item.scale), th = (int)lroundf(r.draw_h * item.scale);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (tw < w && th < h) {
        shrink(rgba, w, h, r.rgba, tw, th);
        r.w = tw;
        r.h = th;
    } else {
        r.rgba.swap(rgba);                                  // the GPU upscales it when drawing
        r.w = w;
        r.h = h;
    }
    if (item.shadow_soft > 0) {
        // make_shadow() needs the alpha at display size (tw x th). When the texture kept the
        // PNG size, a nearest-neighbour upscaled copy provides it.
        if (r.w == tw && r.h == th) {
            make_shadow(r.rgba, tw, th, item.shadow_soft * item.scale, r);
        } else {
            std::vector<uint8_t> grown((size_t)tw * th * 4);
            for (int y = 0; y < th; y++)
                for (int x = 0; x < tw; x++)
                    memcpy(&grown[((size_t)y * tw + x) * 4],
                           &r.rgba[((size_t)(y * (long)r.h / th) * r.w + x * (long)r.w / tw) * 4], 4);
            make_shadow(grown, tw, th, item.shadow_soft * item.scale, r);
        }
        r.shade_margin /= item.scale;   // screen to canvas pixels
    }
    return true;
}

// load() runs on a worker. It tries the disk cache first, then the PNG.
Result load(const Item& item) {
    Clock::time_point since = Clock::now();
    Result r;
    r.key = item.key;
    struct stat st;
    if (stat(item.path.c_str(), &st) != 0) {
        r.failed = true;
        note("missing", item, since);
        return r;
    }
    std::string kept = kept_folder.empty() ? std::string() : kept_name(item, st);
    if (!kept.empty() && read_kept(kept, r)) {
        note("kept", item, since);
        return r;
    }
    r = Result();   // drop what a failed read_kept() left in r
    r.key = item.key;
    if (!prepare(item, r)) {
        r.failed = true;
        note("bad", item, since);
        return r;
    }
    note("png", item, since);
    if (!kept.empty() && ms_since(since) >= WORTH_KEEPING_MS)
        write_kept(kept, r);
    return r;
}

void work(bool urgent_only) {
    // Workers run at nice 10, which makes the scheduler favour the render thread. Linux keeps
    // a nice value per thread, so passing the thread ID changes only this worker.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    for (;;) {
        Item* item = nullptr;
        {
            std::unique_lock<std::mutex> hold(mutex);
            wake.wait(hold, [&] { return quitting || !urgent.empty() || (!urgent_only && !later.empty()); });
            if (quitting)
                return;
            int expected;
            if (!urgent.empty()) {
                item = urgent.back();           // LIFO, the newest request is what the screen shows now
                urgent.pop_back();
                expected = URGENT;
            } else {
                item = later.front();           // highest-priority prefetch
                later.pop_front();
                expected = LATER;
            }
            if (item->state != expected)
                continue;                       // stale entry, promoted to urgent meanwhile
            item->state = WORKING;
        }
        Result result = load(*item);
        std::lock_guard<std::mutex> hold(mutex);
        results.push_back(std::move(result));
    }
}

// Uploads finished results as textures until the uploaded bytes reach most_bytes. With
// needed_only, a result waits unless its picture had an urgent request in the last two
// frames. upload() discards results whose cache entry is gone.
int upload(size_t most_bytes, bool needed_only) {
    std::deque<Result> finished, waiting;
    {
        std::lock_guard<std::mutex> hold(mutex);
        finished.swap(results);
    }
    int done = 0;
    size_t bytes = 0;
    for (Result& r : finished) {
        auto found = cache.find(r.key);
        if (found == cache.end())
            continue;
        Item& item = *found->second;
        if (bytes >= most_bytes || (needed_only && frame - item.needed > 2)) {
            waiting.push_back(std::move(r));
            continue;
        }
        done++;
        Picture& p = item.picture;
        item.state = DONE;
        if (r.failed) {
            p.failed = true;
            continue;
        }
        p.tex = gfx::make_rgba(r.rgba.data(), r.w, r.h, (item.how & images::PIXELS) != 0);
        p.w = r.draw_w;
        p.h = r.draw_h;
        p.focus = r.focus;
        p.bytes = (size_t)r.w * r.h * 4;
        if (!r.shade.empty()) {
            p.shadow = gfx::make_alpha(r.shade.data(), r.shade_w, r.shade_h);
            p.shadow_margin = r.shade_margin;
            p.shadow_w = r.shade_w * SHADOW_SCALE / gfx::scale;
            p.shadow_h = r.shade_h * SHADOW_SCALE / gfx::scale;
            p.bytes += (size_t)r.shade_w * r.shade_h;
        }
        cache_bytes += p.bytes;
        bytes += p.bytes;
        p.ready = true;
    }
    if (!waiting.empty()) {
        std::lock_guard<std::mutex> hold(mutex);
        results.insert(results.begin(), std::make_move_iterator(waiting.begin()), std::make_move_iterator(waiting.end()));
    }
    return done;
}

// Above MOST_BYTES, evicts ready or failed pictures whose last request is more than
// FRESH_FRAMES frames old, oldest first, until cache_bytes drops to KEEP_BYTES.
void forget_old() {
    if (cache_bytes <= MOST_BYTES)
        return;
    std::vector<std::pair<unsigned, std::string>> old;
    for (auto& entry : cache) {
        const Picture& p = entry.second->picture;
        if ((p.ready || p.failed) && frame - p.last_used > FRESH_FRAMES)
            old.push_back({ p.last_used, entry.first });
    }
    std::sort(old.begin(), old.end());
    for (auto& entry : old) {
        if (cache_bytes <= KEEP_BYTES)
            break;
        auto found = cache.find(entry.second);
        Picture& p = found->second->picture;
        cache_bytes -= p.bytes;
        gfx::release(p.tex);
        gfx::release(p.shadow);
        cache.erase(found);
    }
}

// Rebuilds the prefetch queue from this frame's prefetch requests, in request order. It also
// erases the previous frame's prefetches that this frame did not repeat, as long as they are
// still LATER.
void pass_wishes_on() {
    std::vector<Item*> dropped;
    bool any;
    {
        std::lock_guard<std::mutex> hold(mutex);
        later.clear();
        for (Item* item : wishes)
            if (item->state == LATER)
                later.push_back(item);
        for (Item* item : old_wishes)
            if (item->wished != frame && item->state == LATER)
                dropped.push_back(item);
        any = !later.empty();
    }
    if (any)
        wake.notify_all();
    for (Item* item : dropped)
        cache.erase(item->key);
    old_wishes.swap(wishes);
    wishes.clear();
}

}  // namespace

namespace images {

void init(const std::string& cache_folder, const std::string& log) {
    started = Clock::now();
    // Create the cache folder and its parent. The disk cache stays off unless the folder is a
    // writable directory.
    if (!cache_folder.empty()) {
        mkdir(cache_folder.substr(0, cache_folder.rfind('/')).c_str(), 0755);
        mkdir(cache_folder.c_str(), 0755);
        struct stat st;
        if (stat(cache_folder.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(cache_folder.c_str(), W_OK) == 0)
            kept_folder = cache_folder;
    }
    if (!log.empty())
        log_file = fopen(log.c_str(), "a");
    for (int i = 0; i < HELPERS; i++)
        workers[i] = std::thread(work, i == 0);
}

void shutdown() {
    {
        std::lock_guard<std::mutex> hold(mutex);
        quitting = true;
    }
    wake.notify_all();
    for (std::thread& worker : workers)
        if (worker.joinable())
            worker.join();
    if (log_file)
        fclose(log_file);
    log_file = nullptr;
}

Picture* get(const std::string& path, float max_w, float max_h, int how, Shadow shadow, bool ahead) {
    // The key holds every parameter of the request, the shadow's drop and darkness included,
    // which affect only drawing.
    char sizes[96];
    snprintf(sizes, sizeof sizes, "|%.0fx%.0f|%d|%.0f|%.1f|%.2f", max_w, max_h, how, shadow.soft, shadow.drop, shadow.dark);
    std::string key = path + sizes;
    auto found = cache.find(key);
    if (found != cache.end()) {
        Item* item = found->second.get();
        item->picture.last_used = frame;
        if (!ahead)
            item->needed = frame;
        if (item->state == LATER) {
            if (!ahead) {
                // Promote the queued prefetch, since the screen needs it now.
                {
                    std::lock_guard<std::mutex> hold(mutex);
                    if (item->state == LATER) {
                        item->state = URGENT;
                        urgent.push_back(item);
                    }
                }
                wake.notify_all();
            } else if (item->wished != frame) {
                item->wished = frame;
                wishes.push_back(item);
            }
        }
        return &item->picture;
    }

    Item* item = new Item();
    item->key = key;
    item->path = path;
    item->max_w = max_w;
    item->max_h = max_h;
    item->scale = gfx::scale;
    item->how = how;
    item->shadow_soft = shadow.soft;
    item->picture.last_used = frame;
    item->needed = ahead ? 0 : frame;
    item->picture.shadow_drop = shadow.drop;
    item->picture.shadow_dark = shadow.dark;
    cache.emplace(key, std::unique_ptr<Item>(item));
    if (ahead) {
        item->state = LATER;                // pump() queues it with this frame's prefetches
        item->wished = frame;
        wishes.push_back(item);
    } else {
        {
            std::lock_guard<std::mutex> hold(mutex);
            urgent.push_back(item);
        }
        wake.notify_all();
    }
    return &item->picture;
}

int pump(bool moving) {
    pass_wishes_on();
    frame++;
    int done = upload(FRAME_BYTES, moving);
    forget_old();
    return done;
}

void wait(Picture* p, int ms) {
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (p && !p->ready && !p->failed && Clock::now() < end) {
        upload(64u << 20, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void draw(Picture* p, float x, float y, float alpha) {
    if (!p || !p->ready || alpha <= 0)
        return;
    if (p->shadow.id)
        gfx::image(p->shadow, x - p->shadow_margin, y - p->shadow_margin + p->shadow_drop, p->shadow_w, p->shadow_h,
                   { 0, 0, 0, p->shadow_dark * alpha });
    gfx::image(p->tex, x, y, p->w, p->h, { 1, 1, 1, alpha });
}

}  // namespace images
