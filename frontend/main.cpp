// launcher: the fullscreen game menu of an arcade cabinet built around a Raspberry Pi 3B+
// with RetroPie. It renders with SDL2 on KMS/DRM and OpenGL ES 2. Layout values are pixels on
// a 1920x1080 canvas that gfx scales to the display.
//
// The screens are the console screen (slanted screenshot strips), the game screen (a list, or
// a grid of boxes for Recommended), settings, a hidden achievement screen opened with the
// Konami code and a screensaver. pi/make_library.py writes the game library, library.tsv.
//
// pi/arcade.sh runs the menu in a loop. To launch a game or a tool, the menu copies its last
// frame into the console framebuffer (/dev/fb0), writes the action to /dev/shm/launcher-action
// and exits. While a game loads, loading.c animates the loading bar in that frame. After
// power-on the menu takes the display over from the boot animation (boot.c) through handshake
// files in /dev/shm. It starts from the animation's final frame, the boot frame.

#include "brand.h"
#include "gfx.h"
#include "images.h"
#include "sound.h"
#include "text.h"
#include "version.h"

#include <SDL.h>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ------------------------------------------------------------------ files

static std::string HOME = "/opt/retropie/configs/all/arcade-launcher";   // install directory, overridden by LAUNCHER_HOME
static const char* ACTION_FILE = "/dev/shm/launcher-action";      // what pi/arcade.sh shall do next
static const char* STATE_FILE = "/dev/shm/launcher-state";        // screen and selection for the next start
static const char* NOTE_FILE = "/dev/shm/launcher-note";          // one-line status message from pi/arcade.sh
static const char* FRAME_FILE = "/dev/shm/launcher-frame.fb";     // last frame, for pi/arcade.sh to redraw after a tool
static const char* LOADING_FILE = "/dev/shm/launcher-load";     // loading bar parameters for loading.c
static const char* GAME_DREW_FILE = "/dev/shm/launcher-game-drew"; // pi/arcade.sh: the emulator is DRM master
static const char* BOOTED_FILE = "/dev/shm/launcher-booted";      // exists from the first frame after power-on
// boot.c and the menu use these files to hand over the display after power-on.
static const char* MENU_STARTS = "/dev/shm/launcher-menu-starts"; // the menu has started loading
static const char* MENU_READY = "/dev/shm/launcher-menu-ready";   // the menu has finished loading
static const char* START_DONE = "/dev/shm/launcher-boot-done";    // boot.c has left the boot frame in /dev/fb0
// The development aids below require LAUNCHER_DEV=1.
static const char* SHOT_REQUEST = "/dev/shm/launcher-shot-request";
static const char* SHOT_FILE = "/dev/shm/launcher-shot.ppm";
static const char* DEV_KEYS = "/dev/shm/launcher-dev-keys";
static const char* DEV_LOG = "/dev/shm/launcher.log";
static const char* PICTURE_LOG = "/dev/shm/launcher-pictures.log";
static const char* DEV_ALWAYS = "/dev/shm/launcher-dev-always";    // while present, render and time every frame

static bool exists(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

static void touch(const char* path) {
    FILE* f = fopen(path, "w");
    if (f)
        fclose(f);
}

// ------------------------------------------------------------------ colours and fonts

static const Color ACCENT = rgb(BRAND_ACCENT);
static const Color TEXT = rgb(BRAND_TEXT);
static const Color MUTED = rgb(BRAND_MUTED);
static const Color INK = rgb(BRAND_INK);

static Font* regular = nullptr;      // Poppins Regular
static Font* bold = nullptr;         // Poppins SemiBold
static Font* mono = nullptr;         // JetBrains Mono
static Font* mono_medium = nullptr;  // JetBrains Mono Medium, for the lead word of a facts line

static const float MONO_SPACING = 0.08f;      // letter spacing as a fraction of the font size

// ------------------------------------------------------------------ library

struct Game {
    std::string system, rom, title, year, maker, category, tag, box, snap, wheel;
};

struct Console {
    std::string key, name, group, maker, facts, kind, logo, photo;
    std::vector<Game> games;
    std::vector<std::string> shots;       // screenshots for the console screen strips
    bool cut = false;                     // shots holds strip pictures of tools/make_strips.py
};

struct PadMap {
    int open = 0, back = 1;
    int next = -1, previous = -1;         // buttons for the next and previous initial letter, -1 for none
    int favourite = -1;                   // button that adds a game to Recommended or takes it out, -1 for none
    bool swapped = false;                 // inverted Y axis
};

static std::vector<Console> consoles;
static std::map<std::string, PadMap> pad_maps;

static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return parts;
}

// library.tsv has one tab-separated record per line:
//   J  controller: device name, button that opens, button that goes back, 1 for an inverted Y
//      axis, buttons for the next and previous initial letter, button that adds a game to
//      Recommended or takes it out (-1 for none)
//   C  console: key, name, group heading, maker, facts, kind, logo, optional photo
//   X  strip screenshot for the preceding console
//   S  strip picture of tools/make_strips.py for the preceding console, as a path under HOME
//   G  game of the preceding console: system, ROM file, title, year, maker, category, tag, box,
//      screenshot, wheel logo
static void load_library() {
    consoles.clear();
    std::ifstream in(HOME + "/library.tsv");
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = split(line);
        if (f[0] == "J" && f.size() >= 5) {
            PadMap map;
            map.open = atoi(f[2].c_str());
            map.back = atoi(f[3].c_str());
            map.swapped = f[4] == "1";
            if (f.size() >= 7) {
                map.next = atoi(f[5].c_str());
                map.previous = atoi(f[6].c_str());
            }
            if (f.size() >= 8)
                map.favourite = atoi(f[7].c_str());
            pad_maps[f[1]] = map;
        } else if (f[0] == "C" && f.size() >= 8) {
            Console c;
            c.key = f[1]; c.name = f[2]; c.group = f[3]; c.maker = f[4]; c.facts = f[5]; c.kind = f[6];
            c.logo = f[7];
            if (f.size() > 8)
                c.photo = f[8];
            consoles.push_back(c);
        } else if ((f[0] == "X" || f[0] == "S") && f.size() >= 2 && !consoles.empty()) {
            Console& c = consoles.back();
            c.cut = f[0] == "S";
            c.shots.push_back(c.cut ? HOME + "/" + f[1] : f[1]);
        } else if (f[0] == "G" && f.size() >= 11 && !consoles.empty()) {
            Game g;
            g.system = f[1]; g.rom = f[2]; g.title = f[3]; g.year = f[4]; g.maker = f[5]; g.category = f[6];
            g.tag = f[7]; g.box = f[8]; g.snap = f[9]; g.wheel = f[10];
            consoles.back().games.push_back(g);
        }
    }
    if (consoles.empty()) {
        // Keep the settings reachable when the library is empty.
        Console c;
        c.key = "settings"; c.name = "Settings"; c.group = "MACHINE"; c.kind = "settings";
        consoles.push_back(c);
    }
}

// A settings entry passes its action string to pi/arcade.sh unchanged. "tool" with a file
// name on the next line opens that RetroPie setup tool from ~/RetroPie/retropiemenu. When
// confirm is true, the entry needs a second press within four seconds.
struct Setting {
    const char* name;
    const char* about;
    const char* action;
    bool confirm;
};

static const Setting SETTINGS[] = {
    { "Wi-Fi", "Join a wireless network. This opens the Wi-Fi tool of RetroPie.", "tool\nwifi.rp", false },
    { "Sound", "Choose where the sound comes out. This opens the sound tool of RetroPie.", "tool\naudiosettings.rp", false },
    { "Emulator settings", "Video, sound and controls of the games. This opens the menu of RetroArch, the program that runs them.", "tool\nretroarch.rp", false },
    { "Bluetooth", "Pair a wireless controller. This opens the Bluetooth tool of RetroPie.", "tool\nbluetooth.rp", false },
    { "RetroPie setup", "Install and update emulators. This opens the setup program of RetroPie.", "tool\nrpsetup.rp", false },
    { "Look for new games", "Read the game folders again after you copied games to the card.", "rescan", false },
    { "Restart", "Restart the machine.", "restart", true },
    { "Shut down", "Switch the machine off.", "shutdown", true },
};
static const int SETTING_COUNT = sizeof SETTINGS / sizeof SETTINGS[0];

// ------------------------------------------------------------------ input

enum Action { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_SELECT, A_BACK, A_FAVOURITE, A_TERMINAL };

static std::deque<Action> actions;
static std::map<SDL_JoystickID, PadMap> joysticks;
static std::map<int, bool> held_by;            // pressed state per source * 4 + direction
static bool held[4] = { false, false, false, false };
static double repeat_at[4];
static int repeats[4];
static double last_input = 0;

// held_by tracks each direction per input source: 0 is the keyboard, 1 + instance ID the axes
// of a joystick and 1000 + instance ID its hat. A direction counts as held while any source
// holds it. A new press queues one action.
static void set_direction(int source, int direction, bool down, double now) {
    held_by[source * 4 + direction] = down;
    bool any = false;
    for (auto& entry : held_by)
        if (entry.first % 4 == direction && entry.second)
            any = true;
    if (any && !held[direction]) {
        actions.push_back((Action)direction);
        repeat_at[direction] = now + 0.38;
        repeats[direction] = 0;
    }
    held[direction] = any;
}

// Repeats held directions. The first repeat comes 380 ms after the press. Repeats two to nine
// follow every 110 ms, later ones every 45 ms.
static void repeat_directions(double now) {
    for (int d = 0; d < 4; d++) {
        if (held[d] && now >= repeat_at[d]) {
            actions.push_back((Action)d);
            repeats[d]++;
            repeat_at[d] = now + (repeats[d] > 8 ? 0.045 : 0.11);
        }
    }
}

// ------------------------------------------------------------------ presets for new controllers
//
// RetroArch drives a controller in a game only through a preset in retroarch-joypads. RetroPie's
// EmulationStation writes one when someone sets up a controller there, and this menu takes the
// place of EmulationStation. So when a controller without a preset connects, the menu writes
// one from SDL's controller database, which knows the layout of most gamepads: Xbox,
// PlayStation, Switch and 8BitDo pads among them. SDL and RetroArch number the buttons and axes
// of the same device in different orders, so the preset goes through the device's key and axis
// codes: SDL's number to the code, then the code to RetroArch's number.

static void dev_note(const char* format, ...);

// The parts of SDL's mapping string of a controller, such as "a" -> "b0", "dpup" -> "h0.1" and
// "lefttrigger" -> "a5".
static std::map<std::string, std::string> sdl_mapping(int index) {
    std::map<std::string, std::string> parts;
    char* text = SDL_GameControllerMappingForDeviceIndex(index);
    if (!text)
        return parts;
    std::string mapping = text;
    SDL_free(text);
    size_t at = 0;
    for (int field = 0; at <= mapping.size(); field++) {
        size_t end = mapping.find(',', at);
        std::string part = mapping.substr(at, end == std::string::npos ? std::string::npos : end - at);
        size_t colon = part.find(':');
        if (field >= 2 && colon != std::string::npos)      // the GUID and the name come first
            parts[part.substr(0, colon)] = part.substr(colon + 1);
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return parts;
}

// The SDL button number of a mapping part such as "b3", or -1.
static int sdl_button(const std::map<std::string, std::string>& parts, const char* key) {
    auto part = parts.find(key);
    return part != parts.end() && part->second.size() > 1 && part->second[0] == 'b' ? atoi(part->second.c_str() + 1) : -1;
}

#if defined(__linux__) && SDL_VERSION_ATLEAST(2, 24, 0)
static const char* JOYPADS = "/opt/retropie/configs/all/retroarch-joypads";
// pi/make_library.py leaves presets with this first line out of its J lines, because the menu
// reads these controllers from SDL itself.
static const char* SDL_PRESET_MARK = "# Written by the arcade launcher from SDL's controller database";

static bool has_bit(const unsigned long* bits, int n) {
    const int per = 8 * (int)sizeof(long);
    return (bits[n / per] >> (n % per)) & 1UL;
}

// Whether the preset file at path names a device. RetroArch cannot use an empty preset file,
// so the menu writes a new one in its place.
static bool names_device(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
        if (line.find("input_device") != std::string::npos)
            return true;
    return false;
}

// Writes the RetroArch preset of the SDL game controller at index when retroarch-joypads has no
// usable file of its name yet.
static void write_sdl_preset(int index) {
    const char* path = SDL_JoystickPathForIndex(index);
    struct stat st;
    if (!SDL_IsGameController(index) || !path || strncmp(path, "/dev/input/event", 16) != 0 || stat(JOYPADS, &st) != 0)
        return;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    char name[256] = "";
    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {}, axes[ABS_MAX / (8 * sizeof(long)) + 1] = {};
    bool read_all = ioctl(fd, EVIOCGNAME(sizeof name - 1), name) >= 0 && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 &&
                    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof axes), axes) >= 0;
    // SDL takes a pair of hat axes as a hat when its range is -1 to 1, and as two axes otherwise.
    bool digital_hat[4] = {};
    for (int h = 0; read_all && h < 4; h++) {
        bool any = false, digital = true;
        for (int code = ABS_HAT0X + 2 * h; code <= ABS_HAT0X + 2 * h + 1; code++) {
            struct input_absinfo info;
            if (!has_bit(axes, code))
                continue;
            any = true;
            digital = digital && ioctl(fd, EVIOCGABS(code), &info) >= 0 && info.minimum == -1 && info.maximum == 1;
        }
        digital_hat[h] = any && digital;
    }
    close(fd);
    std::string file = name;
    std::replace(file.begin(), file.end(), '/', '_');
    file = std::string(JOYPADS) + "/" + file + ".cfg";
    if (!read_all || !name[0] || names_device(file))
        return;

    // SDL's numbers: buttons from BTN_JOYSTICK on, then the keys below it; axes in code order
    // without the digital hats; hats in code order.
    std::vector<int> sdl_buttons, sdl_axes, sdl_hats;
    for (int code = BTN_JOYSTICK; code < KEY_MAX; code++)
        if (has_bit(keys, code)) sdl_buttons.push_back(code);
    for (int code = 0; code < BTN_JOYSTICK; code++)
        if (has_bit(keys, code)) sdl_buttons.push_back(code);
    for (int code = 0; code < ABS_MAX; code++)
        if (has_bit(axes, code) && !(code >= ABS_HAT0X && code <= ABS_HAT3Y && digital_hat[(code - ABS_HAT0X) / 2]))
            sdl_axes.push_back(code);
    for (int h = 0; h < 4; h++)
        if (digital_hat[h]) sdl_hats.push_back(h);
    // RetroArch's numbers (input/drivers_joypad/udev_joypad.c): keys KEY_UP to KEY_DOWN, then
    // from BTN_MISC on, then below KEY_UP, then between KEY_DOWN and BTN_MISC; axes in code
    // order below ABS_MISC without any hat; a hat by the number of its axis pair.
    std::map<int, int> ra_button, ra_axis;
    const int ranges[4][2] = { { KEY_UP, KEY_DOWN + 1 }, { BTN_MISC, KEY_MAX }, { 0, KEY_UP }, { KEY_DOWN + 1, BTN_MISC } };
    for (const auto& range : ranges)
        for (int code = range[0]; code < range[1]; code++)
            if (has_bit(keys, code)) {
                int next = (int)ra_button.size();
                ra_button[code] = next;
            }
    for (int code = 0; code < ABS_MISC; code++)
        if (has_bit(axes, code) && !(code >= ABS_HAT0X && code <= ABS_HAT3Y)) {
            int next = (int)ra_axis.size();
            ra_axis[code] = next;
        }

    // A part of SDL's mapping as a RetroArch binding: ("btn", "3"), ("btn", "h0up") or
    // ("axis", "+5"); an empty kind when RetroArch cannot reach it.
    auto binding = [&](const std::string& part, bool negative) -> std::pair<std::string, std::string> {
        if (part.size() < 2)
            return {};
        if (part[0] == 'b') {
            size_t n = (size_t)atoi(part.c_str() + 1);
            if (n < sdl_buttons.size() && ra_button.count(sdl_buttons[n]))
                return { "btn", std::to_string(ra_button[sdl_buttons[n]]) };
        } else if (part[0] == 'h') {
            size_t n = (size_t)atoi(part.c_str() + 1);
            size_t dot = part.find('.');
            int mask = dot == std::string::npos ? 0 : atoi(part.c_str() + dot + 1);
            const char* way = mask == 1 ? "up" : mask == 2 ? "right" : mask == 4 ? "down" : mask == 8 ? "left" : nullptr;
            if (n < sdl_hats.size() && way)
                return { "btn", "h" + std::to_string(sdl_hats[n]) + way };
        } else {
            // "a2" is a whole axis, "+a2" and "-a2" its halves, "a2~" an axis upside down.
            bool minus = (part[0] == '-') != negative;
            if (part.back() == '~')
                minus = !minus;
            size_t from = part.find('a');
            size_t n = from == std::string::npos ? sdl_axes.size() : (size_t)atoi(part.c_str() + from + 1);
            if (n < sdl_axes.size() && ra_axis.count(sdl_axes[n]))
                return { "axis", (minus ? "-" : "+") + std::to_string(ra_axis[sdl_axes[n]]) };
        }
        return {};
    };

    std::map<std::string, std::string> parts = sdl_mapping(index);
    std::map<std::string, std::pair<std::string, std::string>> bound;
    // RetroArch names the buttons as on a Super Nintendo pad: B at the bottom, A on the right,
    // Y on the left and X at the top. SDL names them as on an Xbox pad.
    const char* names[][2] = { { "a", "b" }, { "b", "a" }, { "x", "y" }, { "y", "x" }, { "back", "select" },
                               { "start", "start" }, { "guide", "menu_toggle" }, { "leftshoulder", "l" },
                               { "rightshoulder", "r" }, { "lefttrigger", "l2" }, { "righttrigger", "r2" },
                               { "leftstick", "l3" }, { "rightstick", "r3" }, { "dpup", "up" }, { "dpdown", "down" },
                               { "dpleft", "left" }, { "dpright", "right" } };
    for (const auto& pair : names) {
        auto part = parts.find(pair[0]);
        if (part != parts.end()) {
            auto b = binding(part->second, false);
            if (!b.first.empty())
                bound[pair[1]] = b;
        }
    }
    std::string lines;
    auto line = [&lines](const std::string& key, const std::string& value) { lines += key + " = \"" + value + "\"\n"; };
    for (const auto& b : bound)
        line("input_" + b.first + "_" + b.second.first, b.second.second);
    const char* sticks[][2] = { { "leftx", "l_x" }, { "lefty", "l_y" }, { "rightx", "r_x" }, { "righty", "r_y" } };
    for (const auto& stick : sticks) {
        auto part = parts.find(stick[0]);
        if (part == parts.end())
            continue;
        auto plus = binding(part->second, false), minus = binding(part->second, true);
        if (plus.first == "axis") {
            line(std::string("input_") + stick[1] + "_plus_axis", plus.second);
            line(std::string("input_") + stick[1] + "_minus_axis", minus.second);
        }
    }
    // The hotkeys of the cabinet's presets (pi/make_library.py): with Select held, A quits, X
    // pauses, L loads and R saves the state, left and right change the state slot, up and
    // down the shader.
    const char* hotkeys[][2] = { { "select", "enable_hotkey" }, { "a", "exit_emulator" }, { "x", "pause_toggle" },
                                 { "l", "load_state" }, { "r", "save_state" }, { "left", "state_slot_decrease" },
                                 { "right", "state_slot_increase" }, { "up", "shader_next" }, { "down", "shader_prev" } };
    for (const auto& hotkey : hotkeys) {
        auto b = bound.find(hotkey[0]);
        if (b != bound.end())
            line(std::string("input_") + hotkey[1] + "_" + b->second.first, b->second.second);
    }
    char ids[96];
    snprintf(ids, sizeof ids, "input_vendor_id = \"%d\"\ninput_product_id = \"%d\"\n",
             SDL_JoystickGetDeviceVendor(index), SDL_JoystickGetDeviceProduct(index));
    FILE* out = fopen(file.c_str(), "w");
    if (!out)
        return;
    fprintf(out, "%s, for a controller that had no preset.\n# Remove this file to have it written again.\n"
                 "input_device = \"%s\"\ninput_driver = \"udev\"\n%s%s",
            SDL_PRESET_MARK, name, ids, lines.c_str());
    fclose(out);
    dev_note("controller: wrote the RetroArch preset %s", file.c_str());
}
#else
static void write_sdl_preset(int) {}
#endif

static void open_joystick(int index) {
    SDL_Joystick* joy = SDL_JoystickOpen(index);
    if (!joy)
        return;
    // library.tsv holds device names with runs of spaces collapsed, as in
    // "DragonRise Inc. Generic USB Joystick", so the name from SDL gets the same treatment.
    std::string name;
    for (const char* p = SDL_JoystickName(joy) ? SDL_JoystickName(joy) : ""; *p; p++)
        if (*p != ' ' || (!name.empty() && name.back() != ' '))
            name += *p;
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    PadMap map;
    auto known = pad_maps.find(name);
    if (known != pad_maps.end()) {
        map = known->second;
    } else if (SDL_IsGameController(index)) {
        // A pad that SDL knows: its A opens, its B goes back and its Back button (Select) adds
        // a game to Recommended or takes it out, whatever their numbers on this pad.
        std::map<std::string, std::string> parts = sdl_mapping(index);
        if (sdl_button(parts, "a") >= 0 && sdl_button(parts, "b") >= 0) {
            map.open = sdl_button(parts, "a");
            map.back = sdl_button(parts, "b");
            map.favourite = sdl_button(parts, "back");
        }
        write_sdl_preset(index);
    }
    joysticks[SDL_JoystickInstanceID(joy)] = map;
}

static void handle_event(const SDL_Event& e, double now, bool& running) {
    switch (e.type) {
    case SDL_QUIT:
        running = false;
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        bool down = e.type == SDL_KEYDOWN;
        if (down && e.key.repeat)
            break;
        last_input = now;
        switch (e.key.keysym.sym) {
        case SDLK_UP: set_direction(0, A_UP, down, now); break;
        case SDLK_DOWN: set_direction(0, A_DOWN, down, now); break;
        case SDLK_LEFT: set_direction(0, A_LEFT, down, now); break;
        case SDLK_RIGHT: set_direction(0, A_RIGHT, down, now); break;
        case SDLK_RETURN: if (down) actions.push_back(A_SELECT); break;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE: if (down) actions.push_back(A_BACK); break;
        case SDLK_RSHIFT: if (down) actions.push_back(A_FAVOURITE); break;     // Select in RetroArch's keyboard layout
        case SDLK_F4: if (down) actions.push_back(A_TERMINAL); break;
        default: break;
        }
        break;
    }
    case SDL_JOYDEVICEADDED:
        open_joystick(e.jdevice.which);
        break;
    case SDL_JOYDEVICEREMOVED:
        // Release the directions of a removed joystick so that they stop repeating.
        for (int d = 0; d < 4; d++) {
            set_direction(1 + (int)e.jdevice.which, d, false, now);
            set_direction(1000 + (int)e.jdevice.which, d, false, now);
        }
        joysticks.erase(e.jdevice.which);
        break;
    case SDL_JOYBUTTONDOWN: {
        last_input = now;
        auto joy = joysticks.find(e.jbutton.which);
        if (joy == joysticks.end())
            break;
        const PadMap& map = joy->second;
        int button = e.jbutton.button;
        if (button == map.open) actions.push_back(A_SELECT);
        else if (button == map.back) actions.push_back(A_BACK);
        else if (button == map.next) actions.push_back(A_RIGHT);
        else if (button == map.previous) actions.push_back(A_LEFT);
        else if (button == map.favourite) actions.push_back(A_FAVOURITE);
        break;
    }
    case SDL_JOYAXISMOTION: {
        auto joy = joysticks.find(e.jaxis.which);
        if (joy == joysticks.end() || e.jaxis.axis > 1)
            break;
        int source = 1 + (int)e.jaxis.which;
        bool low = e.jaxis.value < -16000, high = e.jaxis.value > 16000;
        if (low || high)
            last_input = now;
        if (e.jaxis.axis == 0) {
            set_direction(source, A_LEFT, low, now);
            set_direction(source, A_RIGHT, high, now);
        } else {
            bool swapped = joy->second.swapped;
            set_direction(source, A_UP, swapped ? high : low, now);
            set_direction(source, A_DOWN, swapped ? low : high, now);
        }
        break;
    }
    case SDL_JOYHATMOTION: {
        last_input = now;
        int source = 1000 + (int)e.jhat.which;
        set_direction(source, A_UP, (e.jhat.value & SDL_HAT_UP) != 0, now);
        set_direction(source, A_DOWN, (e.jhat.value & SDL_HAT_DOWN) != 0, now);
        set_direction(source, A_LEFT, (e.jhat.value & SDL_HAT_LEFT) != 0, now);
        set_direction(source, A_RIGHT, (e.jhat.value & SDL_HAT_RIGHT) != 0, now);
        break;
    }
    default:
        break;
    }
}

// Development aid: replays the words in DEV_KEYS as presses, for driving the menu over SSH.
static void read_dev_keys(double now) {
    std::ifstream in(DEV_KEYS);
    if (!in)
        return;
    std::string word;
    while (in >> word) {
        if (word == "up") actions.push_back(A_UP);
        else if (word == "down") actions.push_back(A_DOWN);
        else if (word == "left") actions.push_back(A_LEFT);
        else if (word == "right") actions.push_back(A_RIGHT);
        else if (word == "a") actions.push_back(A_SELECT);
        else if (word == "b") actions.push_back(A_BACK);
        else if (word == "select") actions.push_back(A_FAVOURITE);
    }
    in.close();
    remove(DEV_KEYS);
    last_input = now;
}

// ------------------------------------------------------------------ screen state

enum Screen { S_CONSOLES, S_GAMES, S_SETTINGS, S_SECRET };

static Screen screen = S_CONSOLES;
static int console_sel = 0;
static int console_way = 1;                // direction of the last console change: 1 right, -1 left
static std::vector<int> game_sel;          // selected game per console
static int game_way = 1;                   // direction of the last game list move: 1 down, -1 up
static int setting_sel = 0;
static double confirm_until = 0;           // deadline for the confirming second press
static double remove_until = 0;            // deadline for the second Select that takes a game out of Recommended
static int remove_index = -1;              // the Recommended game that the first Select chose
static std::string note;                   // one-line status message, such as a failed game launch
static double note_until = 0;

// The pager is the row of console names along the bottom of the console screen.
static std::vector<float> pager_x, pager_w;    // offset and width of each name within the row
static float pager_width = 0;
static float pager_at = 0, pager_to = 0;       // current and target x of the row on the canvas

// The values below control transitions and scrolling.
static float list_shift = 0;               // scroll offset of the current list from its rest position, in pixels
static float mix = 0, mix_to = 0;          // slide transition: 0 console screen, 1 game, settings or hidden screen
static bool launching = false;             // a game is starting: render() draws the loading screen
static bool waiting = false;               // a RetroPie tool opens or the menu looks for new games: render() draws the wait screen
static double loading_since = 0;           // start of the loading bar animation, in now_s
static double load_demo = 0;               // LAUNCHER_LOAD_DEMO: seconds to hold the launch frame in the desktop build

// A stage shows the pictures of the current selection once it has settled: the logo, facts and
// photo on the console screen; the screenshot, wheel logo and box on the game screen.
struct Stage {
    int shown = -1;
    float alpha = 0;
    double since = 0;                      // time of the last selection change
    bool timed = true;                     // development aid: false until update_stage logs the press-to-picture time
};
static Stage console_stage, game_stage;

static double now_s = 0;                   // seconds since the main loop started
static bool animating = true;              // this frame needs a redraw
static bool dev = false;                   // LAUNCHER_DEV is set

static const float LIST_X = 63.36f;        // left edge of every list, 3.3 % of the canvas width

// Development aid: appends one line to DEV_LOG.
static void dev_note(const char* format, ...) {
    if (!dev)
        return;
    FILE* f = fopen(DEV_LOG, "a");
    if (!f)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static std::string grouped(int n) {
    std::string digits = std::to_string(n), out;
    while (digits.size() > 3) {
        out = "," + digits.substr(digits.size() - 3) + out;
        digits.erase(digits.size() - 3);
    }
    return digits + out;
}

static std::string count_label(const Console& c) {
    if (c.kind == "settings")
        return "";
    int n = (int)c.games.size();
    return grouped(n) + (n == 1 ? " GAME" : " GAMES");
}

static std::string upper(std::string s) {
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
    return s;
}

// ------------------------------------------------------------------ drawing helpers

static void mono_text(float size, float x, float baseline, const std::string& s, Color c) {
    text::draw(mono, size, x, baseline, s, c, size * MONO_SPACING);
}

static float mono_width(float size, const std::string& s) {
    return text::width(mono, size, s, size * MONO_SPACING);
}

// Draws the lead word of a facts line in the accent colour at Medium weight. Returns the x where the rest
// of the line continues.
static float lead_text(float size, float x, float baseline, const std::string& s, float alpha) {
    text::draw(mono_medium, size, x, baseline, s, faded(ACCENT, alpha), size * MONO_SPACING);
    return x + text::width(mono_medium, size, s, size * MONO_SPACING);
}

// Draws a button hint (key cap in the accent colour and label) at the bottom right that ends at x = right.
// Returns the right edge for the next hint to its left.
static float hint(float right, const char* key, const char* label, float alpha) {
    float spacing = 18 * MONO_SPACING;
    float label_w = mono_width(18, label) - spacing;
    float key_w = mono_width(18, key) - spacing;
    float cap_w = key_w + 24;
    float x = right - label_w - 10.6f - cap_w;
    gfx::round_rect(x, 997, cap_w, 27, 10, faded(ACCENT, alpha));
    mono_text(18, x + 12, 1017, key, faded(INK, alpha));
    mono_text(18, x + cap_w + 10.6f, 1017, label, faded(MUTED, alpha));
    return x - 45;
}

static Picture* corner_logo() {
    return images::get(HOME + "/art/brand-logo-small.png", 178, 50);
}

static Picture* large_logo() {
    return images::get(HOME + "/art/brand-logo.png", 760, 220);
}

// Draws the large brand logo in the centre, over a black fill when ground is true. It is the
// first frame when the menu starts without a restored position or a boot frame. Restart and
// shutdown leave it on screen.
static void draw_logo_screen(float alpha, bool ground) {
    if (alpha <= 0)
        return;
    if (ground)
        gfx::rect(0, 0, 1920, 1080, { 0, 0, 0, 1 });
    Picture* logo = large_logo();
    if (logo->ready)
        images::draw(logo, floorf((1920 - logo->w) / 2), floorf((1080 - logo->h) / 2), alpha);
}

// Word-wraps s into lines that fit within width w. A '\n' in s forces a line break.
static std::vector<std::string> wrap_text(Font* font, float size, const std::string& s, float w) {
    std::vector<std::string> lines;
    std::string line, word;
    auto end_word = [&]() {
        if (word.empty())
            return;
        std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && text::width(font, size, tried) > w) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ') {
            end_word();
        } else if (ch == '\n') {
            end_word();
            lines.push_back(line);
            line.clear();
        } else {
            word += ch;
        }
    }
    end_word();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

// Draws up to three wrapped lines downwards from y.
static void draw_wrapped(Font* font, float size, float x, float y, float w, const std::string& s, Color c) {
    std::vector<std::string> lines = wrap_text(font, size, s, w);
    if (lines.size() > 3)
        lines.resize(3);
    float step = size * 1.25f;
    for (size_t i = 0; i < lines.size(); i++)
        text::draw(font, size, x, y + step * i + size * 0.95f, text::fit(font, size, lines[i], w), c);
}

static bool settled(Picture* p) {
    return !p || p->ready || p->failed;
}

// ------------------------------------------------------------------ console screen
//
// The console screen shows one console at a time: six slanted strips with screenshots of its
// games, the stage in the centre and the pager along the bottom.

static const int STRIPS = 6;
// The strip row overhangs both sides of the canvas to keep the slanted outer edges off screen.
// Screenshots fill a box 60 px taller than the canvas at each end.
static const float STRIP_LEFT = -160, STRIP_WIDTH = 2240.0f / STRIPS;
static const float STRIP_TOP = -60, STRIP_HEIGHT = 1200;
static const float STRIP_LINE = 6;                // width of the dark line between strips
static const float STRIP_LEAN = 0.158384f;        // horizontal offset per pixel of height, tan(9 degrees)
static const Shadow LOGO_SHADOW = { 10, 30, 0.9f };
static const float PAGER_GAP = 32, PAGER_EDGE = 120;

// The strips darken towards the top and bottom, behind the corner logo, the pager and the
// button hints. SHADE_AT lists heights as fractions of the canvas height. SHADE lists the
// darkening at each height, from 0 (screenshot unchanged) to 1 (solid INK).
static const float SHADE_AT[4] = { 0.0f, 0.30f, 0.62f, 1.0f }, SHADE[4] = { 0.82f, 0.35f, 0.45f, 0.94f };

// A strip shows a strip picture of tools/make_strips.py, a slice of a screenshot, or a flat
// colour for a console without screenshots.
struct StripPicture {
    std::string file;
    bool cut = false;                     // file is a strip picture
    int slice = 0;                        // 0: the screenshot's most detailed slice, 1 and up: its neighbours
    bool black = false;                   // solid black, as on the logo screen at startup
    bool start = false;                   // this strip's slice of the boot frame
    bool operator==(const StripPicture& other) const {
        return file == other.file && cut == other.cut && slice == other.slice && black == other.black &&
               start == other.start;
    }
    bool operator!=(const StripPicture& other) const { return !(*this == other); }
};

struct Strip {
    StripPicture shown, under, wanted;
    float down = 1;                       // wipe progress of shown over under, 0 to 1
    double change_at = 0;                 // earliest time to switch to wanted
};
static Strip strips[STRIPS];
static double strips_wait = 0;            // strips keep their pictures until this time
static Texture start_picture;             // the boot frame, until the screenshots cover it

// Screenshots load doubled with Scale2x and magnify with linear filtering, which keeps pixel
// art from looking blocky. The strips, the game screen and the loading screen share one
// picture per file.
static Picture* shot_picture(const std::string& file, bool ahead = false) {
    if (file.empty())
        return nullptr;
    return images::get(file, 0, 0, images::AS_IS | images::SMOOTH, NO_SHADOW, ahead);
}

// Strip pictures come cut to shape and enlarged, so they load as they are.
static Picture* strip_picture(const StripPicture& what, bool ahead = false) {
    if (!what.cut)
        return shot_picture(what.file, ahead);
    return images::get(what.file, 0, 0, images::AS_IS, NO_SHADOW, ahead);
}

static Picture* console_logo(const Console& c, bool ahead = false) {
    if (c.logo.empty())
        return nullptr;
    return images::get(HOME + "/art/logos/" + c.logo, 900, 200, images::NO_GROW, LOGO_SHADOW, ahead);
}

// Returns the console logo at the corner logo's height, for the top line of the game screen.
static Picture* console_logo_small(const Console& c, bool ahead = false) {
    if (c.logo.empty())
        return nullptr;
    return images::get(HOME + "/art/logos/" + c.logo, 360, 46, 0, NO_SHADOW, ahead);
}

// The optional console photo sits below the logo and facts, in the area that the box row of
// Recommended uses too. tools/build_assets.py sizes the photos to PHOTO_W x PHOTO_H.
static const float PHOTO_Y = 650, PHOTO_W = 520, PHOTO_H = 216;
static const Shadow PHOTO_SHADOW = { 16, 22, 0.8f };

static Picture* console_photo(const Console& c, bool ahead = false) {
    if (c.photo.empty())
        return nullptr;
    return images::get(HOME + "/art/consoles/" + c.photo, PHOTO_W, PHOTO_H, images::NO_GROW, PHOTO_SHADOW, ahead);
}

// Recommended shows the boxes of the first five games of its list in place of a photo, each
// box partly covered by the next.
static const int ROW_BOXES = 5;
static const float ROW_BOX_W = 280, ROW_BOX_H = 204;
static const float ROW_SEEN = 0.52f;          // visible fraction of each overlapped box
static const float ROW_UNDER = 28;            // how far a box extends under the next one, behind its slanted edge
static const Shadow ROW_SHADOW = { 5, 9, 0.6f };

struct BoxRow {
    Picture* box[ROW_BOXES];
    int count = 0;
};

static BoxRow box_row(const Console& c, bool ahead = false) {
    BoxRow row;
    if (c.kind != "recommended")
        return row;
    for (const Game& g : c.games) {
        if (g.box.empty())
            continue;
        row.box[row.count++] = images::get(g.box, ROW_BOX_W, ROW_BOX_H, images::TRIM, ROW_SHADOW, ahead);
        if (row.count == ROW_BOXES)
            break;
    }
    return row;
}

static bool box_row_settled(const BoxRow& row) {
    bool all = true;
    for (int i = 0; i < row.count; i++)
        all = settled(row.box[i]) && all;
    return all;
}

// Draws the leftmost w of the texture's full_w canvas pixels.
static void draw_left_part(const Texture& t, float x, float y, float w, float full_w, float h, Color tint) {
    const float xs[4] = { x, x + w, x + w, x }, ys[4] = { y, y, y + h, y + h };
    const float us[4] = { 0, w / full_w, w / full_w, 0 }, vs[4] = { 0, 0, 1, 1 };
    const Color tints[4] = { tint, tint, tint, tint };
    gfx::corners(&t, xs, ys, us, vs, tints);
}

// Draws the box row left to right in the photo's area. Each box and its shadow stop where the
// next box covers them, plus ROW_UNDER behind that box's slanted edge, so that overlapped
// parts stay invisible while the row fades in.
static void draw_box_row(const BoxRow& row, float a) {
    int last = -1;
    for (int i = 0; i < row.count; i++)
        if (row.box[i]->ready)
            last = i;
    if (last < 0)
        return;
    float total = 0;
    for (int i = 0; i <= last; i++)
        if (row.box[i]->ready)
            total += i == last ? row.box[i]->w : floorf(row.box[i]->w * ROW_SEEN);
    float x = floorf(960 - total / 2);
    // Draw the same glow as behind a console photo.
    gfx::shadow(x + total * 0.1f, PHOTO_Y + PHOTO_H * 0.15f, total * 0.8f, PHOTO_H * 0.7f, PHOTO_H * 0.35f, 220,
                { 0.76f, 1.0f, 0.93f, 0.16f * a });
    for (int i = 0; i <= last; i++) {
        Picture* box = row.box[i];
        if (!box->ready)
            continue;
        float y = PHOTO_Y + floorf((PHOTO_H - box->h) / 2);
        float step = i == last ? box->w : floorf(box->w * ROW_SEEN);
        float seen = fminf(box->w, step + ROW_UNDER);
        if (box->shadow.id) {
            float shadow_seen = i == last ? box->shadow_w : fminf(box->shadow_w, box->shadow_margin + seen);
            draw_left_part(box->shadow, x - box->shadow_margin, y - box->shadow_margin + box->shadow_drop, shadow_seen,
                           box->shadow_w, box->shadow_h, { 0, 0, 0, box->shadow_dark * a });
        }
        draw_left_part(box->tex, x, y, seen, box->w, box->h, { 1, 1, 1, a });
        x += step;
    }
}

// Returns what strip i shows for console c. With fewer screenshots than strips, the extra
// strips show other slices of the same screenshots.
static StripPicture strip_of(const Console& c, int i) {
    StripPicture picture;
    int count = (int)c.shots.size();
    if (count > 0) {
        picture.file = c.shots[i % count];
        picture.cut = c.cut;
        picture.slice = i / count;
    }
    return picture;
}

// Returns the flat colour of strip i without a screenshot. It matches the background gradient
// of the other screens at the strip's position.
static Color plain_strip(int i) {
    static const float STOPS[3] = { 0.0f, 0.36f, 1.0f };
    static const float COLOURS[3][3] = { { BRAND_GROUND_0 }, { BRAND_GROUND_1 }, { BRAND_GROUND_2 } };
    float t = (STRIP_LEFT + (i + 0.5f) * STRIP_WIDTH) / 1920;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    int k = t < STOPS[1] ? 0 : 1;
    float f = (t - STOPS[k]) / (STOPS[k + 1] - STOPS[k]);
    float lift = i % 2 ? 1.0f : 1.45f;    // lighten alternate strips to keep them distinct
    Color c = { 0, 0, 0, 1 };
    c.r = (COLOURS[k][0] + (COLOURS[k + 1][0] - COLOURS[k][0]) * f) * lift / 255;
    c.g = (COLOURS[k][1] + (COLOURS[k + 1][1] - COLOURS[k][1]) * f) * lift / 255;
    c.b = (COLOURS[k][2] + (COLOURS[k + 1][2] - COLOURS[k][2]) * f) * lift / 255;
    return c;
}

static Color mixed(Color a, Color b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

// Returns the strip darkening at canvas height y, linear between the SHADE_AT stops.
static float shade_at(float y) {
    float t = y / 1080;
    for (int k = 0; k < 3; k++)
        if (t <= SHADE_AT[k + 1])
            return SHADE[k] + (SHADE[k + 1] - SHADE[k]) * (t - SHADE_AT[k]) / (SHADE_AT[k + 1] - SHADE_AT[k]);
    return SHADE[3];
}

// Draws strip i between canvas heights from and to, opaque so that the GPU can skip blending.
static void draw_strip(int i, const StripPicture& what, float from, float to) {
    if (to <= from)
        return;
    float left = STRIP_LEFT + i * STRIP_WIDTH, right = left + STRIP_WIDTH;      // edges at mid-height (y = 540)
    if (what.start) {
        // Map the boot frame 1:1 onto screen pixels.
        float lean0 = (540 - from) * STRIP_LEAN, lean1 = (540 - to) * STRIP_LEAN;
        const float x[4] = { left + lean0, right + lean0, right + lean1, left + lean1 };
        const float y[4] = { from, from, to, to };
        float u[4], v[4];
        for (int k = 0; k < 4; k++) {
            u[k] = (gfx::off_x + x[k] * gfx::scale) / gfx::win_w;
            v[k] = (gfx::off_y + y[k] * gfx::scale) / gfx::win_h;
        }
        const float as_it_is[4] = { 0, 0, 0, 0 };
        gfx::corners_mixed(start_picture, x, y, u, v, INK, as_it_is);
        return;
    }
    Picture* p = strip_picture(what);
    bool pictured = p && p->ready;
    float u0 = 0, u1 = 0, top = 0, bottom = 0;
    if (pictured) {
        // Scale the picture to cover the strip. A strip picture shows its centre. Most of a
        // screenshot's width falls outside the strip, so the strip shows the slice around
        // the picture's focus, the part with the most detail. Repeated screenshots show the
        // slices beside it.
        static const float SLICE_SHIFT[6] = { 0, 1, -1, 2, -2, 3 };
        float grow = fmaxf((STRIP_WIDTH - STRIP_LINE) / p->w, STRIP_HEIGHT / p->h);
        float seen_w = STRIP_WIDTH / (p->w * grow), seen_h = STRIP_HEIGHT / (p->h * grow);
        float centre = what.cut ? 0.5f : p->focus + SLICE_SHIFT[what.slice % 6] * seen_w;
        u0 = std::max(0.0f, std::min(centre - seen_w / 2, 1 - seen_w));
        u1 = u0 + seen_w;
        top = (1 - seen_h) / 2 + seen_h * (0 - STRIP_TOP) / STRIP_HEIGHT;
        bottom = (1 - seen_h) / 2 + seen_h * (1080 - STRIP_TOP) / STRIP_HEIGHT;
    }
    const Color fill = plain_strip(i);
    // Draw one quad per shading segment.
    for (int piece = 0; piece < 3; piece++) {
        float y0 = std::max(from, 1080 * SHADE_AT[piece]), y1 = std::min(to, 1080 * SHADE_AT[piece + 1]);
        if (y1 <= y0)
            continue;
        float lean0 = (540 - y0) * STRIP_LEAN, lean1 = (540 - y1) * STRIP_LEAN;
        const float x[4] = { left + lean0, right + lean0, right + lean1, left + lean1 };
        const float y[4] = { y0, y0, y1, y1 };
        const float dark[4] = { shade_at(y0), shade_at(y0), shade_at(y1), shade_at(y1) };
        if (pictured) {
            float v0 = top + (bottom - top) * y0 / 1080, v1 = top + (bottom - top) * y1 / 1080;
            const float u[4] = { u0, u1, u1, u0 }, v[4] = { v0, v0, v1, v1 };
            gfx::corners_mixed(p->tex, x, y, u, v, INK, dark);
        } else {
            const Color black = { 0, 0, 0, 1 };
            const Color above = what.black ? black : mixed(fill, INK, dark[0]);
            const Color below = what.black ? black : mixed(fill, INK, dark[2]);
            const Color colours[4] = { above, above, below, below };
            gfx::corners(nullptr, x, y, nullptr, nullptr, colours, true);
        }
    }
}

// Draws an INK line from (x0, y0) to (x1, y1) that extends `wide` canvas pixels to the right.
// Both long edges fade over one screen pixel as antialiasing for the slant.
static void draw_line(float x0, float y0, float x1, float y1, float wide) {
    const Color ink = INK, none = faded(INK, 0);
    const float soft = 0.5f / gfx::scale;
    const float at[4] = { -soft, soft, wide - soft, wide + soft };
    const Color across[4] = { none, ink, ink, none };
    const float y[4] = { y0, y0, y1, y1 };
    for (int piece = 0; piece < 3; piece++) {
        const float x[4] = { x0 + at[piece], x0 + at[piece + 1], x1 + at[piece + 1], x1 + at[piece] };
        const Color colours[4] = { across[piece], across[piece + 1], across[piece + 1], across[piece] };
        gfx::corners(nullptr, x, y, nullptr, nullptr, colours);
    }
}

static void draw_strips() {
    const float lean = 540 * STRIP_LEAN;
    for (int i = 0; i < STRIPS; i++) {
        const Strip& s = strips[i];
        // The new picture wipes down over the old one with a cubic ease-out.
        float open = 1 - s.down;
        float cut = 1080 * (1 - open * open * open);
        draw_strip(i, s.shown, 0, cut);
        draw_strip(i, s.under, cut, 1080);
        if (cut > 0 && cut < 1080) {
            // Draw a dark line along the wipe edge.
            float left = STRIP_LEFT + i * STRIP_WIDTH + (540 - cut) * STRIP_LEAN;
            gfx::rect(left, cut - STRIP_LINE / 2, STRIP_WIDTH, STRIP_LINE, INK);
        }
    }
    for (int i = 1; i < STRIPS; i++) {
        float right = STRIP_LEFT + i * STRIP_WIDTH;
        // The boot frame already contains these lines under the brand logo and ARCADE.
        // Skip the line wherever the strip to its left still shows the boot frame.
        const Strip& s = strips[i - 1];
        float open = 1 - s.down;
        float to = s.shown.start ? 0 : s.under.start ? 1080 * (1 - open * open * open) : 1080;
        if (to > 0)
            draw_line(right - STRIP_LINE + lean, 0, right - STRIP_LINE + (540 - to) * STRIP_LEAN, to, STRIP_LINE);
    }
}

static bool console_stage_ready(const Console& c) {
    bool logo = settled(console_logo(c)), photo = settled(console_photo(c)), row = box_row_settled(box_row(c));
    return logo && photo && row;
}

// Draws the console logo with its facts line and, below them, the console photo or the box row.
static void draw_console_stage(const Console& c, float a) {
    if (a <= 0)
        return;
    // Move the logo and facts up when a photo or box row sits below them.
    Picture* photo = console_photo(c);
    BoxRow row = box_row(c);
    bool photographed = photo && photo->ready, boxed = false;
    for (int i = 0; i < row.count; i++)
        boxed = boxed || row.box[i]->ready;
    bool pictured = photographed || boxed;
    float middle = pictured ? 412 : 500, facts_at = pictured ? 586 : 674;
    Picture* logo = console_logo(c);
    if (logo && logo->ready) {
        images::draw(logo, floorf(960 - logo->w / 2), floorf(middle - logo->h / 2), a);
    } else if (!logo || logo->failed) {
        // Without a logo, draw the console name as large as fits in 900 px.
        float size = 150;
        float w = text::width(bold, size, c.name);
        if (w > 900) {
            size = floorf(size * 900 / w);
            w = text::width(bold, size, c.name);
        }
        gfx::shadow(960 - w / 2, middle - size * 0.3f, w, size * 0.6f, size * 0.3f, 110, { 0, 0, 0, 0.5f * a });
        text::draw(bold, size, floorf(960 - w / 2), middle + text::cap_height(bold, size) / 2, c.name, faded(TEXT, a));
    }

    // The facts line starts with the maker in the accent colour, followed by the facts and the game count.
    std::string rest;
    for (const std::string& part : { c.facts, count_label(c) })
        if (!part.empty())
            rest += ((rest.empty() && c.maker.empty()) ? "" : " \xC2\xB7 ") + part;
    float spacing = 24 * MONO_SPACING;
    float lead_w = c.maker.empty() ? 0 : text::width(mono_medium, 24, c.maker, spacing);
    float total = lead_w + mono_width(24, rest) - spacing;
    float x = floorf(960 - total / 2);
    gfx::shadow(x - 6, facts_at - 20, total + 12, 24, 12, 48, { 0, 0, 0, 0.6f * a });      // keeps the line readable on a bright screenshot
    if (!c.maker.empty())
        lead_text(24, x, facts_at, c.maker, a);
    mono_text(24, x + lead_w, facts_at, rest, faded(TEXT, a));

    if (photographed) {
        float px = floorf(960 - photo->w / 2), py = PHOTO_Y + floorf((PHOTO_H - photo->h) / 2);
        // A soft glow in the accent colour behind the photo keeps a black console visible against the dark strips.
        gfx::shadow(px + photo->w * 0.1f, py + photo->h * 0.15f, photo->w * 0.8f, photo->h * 0.7f, photo->h * 0.35f, 220,
                    { 0.76f, 1.0f, 0.93f, 0.16f * a });
        images::draw(photo, px, py, a);
    } else if (boxed) {
        draw_box_row(row, a);
    }
}

static void build_pager() {
    pager_x.assign(consoles.size(), 0);
    pager_w.assign(consoles.size(), 0);
    float x = 0;
    for (size_t i = 0; i < consoles.size(); i++) {
        pager_x[i] = x;
        pager_w[i] = mono_width(18, upper(consoles[i].name)) - 18 * MONO_SPACING;
        x += pager_w[i] + PAGER_GAP;
    }
    pager_width = consoles.empty() ? 0 : x - PAGER_GAP;
}

// Returns the target x of the pager. A pager that fits stays centred. A longer one scrolls to
// centre the selected name, clamped so that each end stays within PAGER_EDGE of its canvas edge.
static float pager_goal() {
    if (consoles.empty())
        return 0;
    if (pager_width <= 1920 - 2 * PAGER_EDGE)
        return floorf((1920 - pager_width) / 2);
    float goal = 960 - (pager_x[console_sel] + pager_w[console_sel] / 2);
    goal = std::min(goal, PAGER_EDGE);
    goal = std::max(goal, 1920 - PAGER_EDGE - pager_width);
    return goal;
}

static void draw_pager(float a) {
    bool moves = pager_width > 1920 - 2 * PAGER_EDGE;
    for (size_t i = 0; i < consoles.size(); i++) {
        float x = pager_at + pager_x[i];
        float shown = 1;
        if (moves) {
            // A scrolling pager fades out towards the canvas edges.
            float middle = x + pager_w[i] / 2;
            shown = std::min(middle - 40, 1880 - middle) / 150;
            shown = shown < 0 ? 0 : shown > 1 ? 1 : shown;
        }
        if (shown <= 0)
            continue;
        Color colour = (int)i == console_sel ? faded(ACCENT, a * shown) : faded(MUTED, 0.55f * a * shown);
        mono_text(18, x, 944, upper(consoles[i].name), colour);
    }
}

// Draws everything over the strips: the stage, the pager and the button hints.
static void draw_consoles(float a) {
    if (console_stage.shown >= 0 && console_stage.shown < (int)consoles.size())
        draw_console_stage(consoles[console_stage.shown], a * console_stage.alpha);
    draw_pager(a);
    float right = hint(1859, "STICK", "BROWSE", a);
    hint(right, "A", "OPEN", a);
}

// ------------------------------------------------------------------ game screen
//
// The game list sits on the left. On the right the selected game's screenshot fills a frame,
// with the game's wheel logo over the lower left corner and its box over the lower right
// corner. Recommended shows a grid of boxes in place of the list.

static const float GAME_LIST_WIDTH = 640;
static const float GAME_LIST_MIDDLE = 551;        // y of the selected line
static const float SETTING_LIST_MIDDLE = 605;
static const float SHOT_X = 760, SHOT_Y = 130, SHOT_W = 1100, SHOT_H = 660, SHOT_ROUND = 24;
static const float SHOT_LEAST_SEEN = 0.72f;       // crop to fill the frame while at least this fraction stays visible
static const Shadow GAME_LOGO_SHADOW = { 10, 24, 0.85f };
static const Shadow BOX_SHADOW = { 26, 30, 0.7f };

static Picture* box_picture(const Game& g, bool ahead = false) {
    return g.box.empty() ? nullptr : images::get(g.box, 400, 300, images::TRIM, BOX_SHADOW, ahead);
}

static Picture* wheel_picture(const Game& g, bool ahead = false) {
    return g.wheel.empty() ? nullptr : images::get(g.wheel, 620, 190, images::TRIM, GAME_LOGO_SHADOW, ahead);
}

static Picture* snap_picture(const Game& g, bool ahead = false) {
    return shot_picture(g.snap, ahead);
}

static bool game_stage_ready(const Game& g) {
    bool box = settled(box_picture(g)), wheel = settled(wheel_picture(g)), snap = settled(snap_picture(g));
    return box && wheel && snap;
}

// Preloads the pictures of a game that is not on screen yet.
static void want_game(const Game& g) {
    snap_picture(g, true);
    wheel_picture(g, true);
    box_picture(g, true);
}

// SHOT_ROOM is the screenshot area inset by the corner radius. The screenshot or the frame's
// dark fill always covers it, so the background pass skips it.
static const float SHOT_ROOM[4] = { SHOT_X + SHOT_ROUND, SHOT_Y + SHOT_ROUND, SHOT_W - 2 * SHOT_ROUND, SHOT_H - 2 * SHOT_ROUND };

static void draw_game_shot(const Game& g, float a) {
    Picture* snap = snap_picture(g);
    if (a <= 0 || !snap || !snap->ready)
        return;
    float cover = fmaxf(SHOT_W / snap->w, SHOT_H / snap->h);
    float seen_w = SHOT_W / (snap->w * cover), seen_h = SHOT_H / (snap->h * cover);
    float cut_x = (1 - seen_w) / 2, cut_y = (1 - seen_h) / 2;
    if (seen_w * seen_h >= SHOT_LEAST_SEEN) {
        gfx::image_round(snap->tex, SHOT_X, SHOT_Y, SHOT_W, SHOT_H, SHOT_ROUND, { 1, 1, 1, a }, cut_x, cut_y, 1 - cut_x,
                         1 - cut_y, true);
        return;
    }
    // A screenshot of a different aspect ratio, such as a vertical arcade game, would lose too
    // much to cropping. Fit it whole over a darkened copy that fills the frame.
    gfx::image_round(snap->tex, SHOT_X, SHOT_Y, SHOT_W, SHOT_H, SHOT_ROUND, { 0.2f, 0.2f, 0.2f, a }, cut_x, cut_y, 1 - cut_x,
                     1 - cut_y, true);
    float fit = fminf(SHOT_W / snap->w, SHOT_H / snap->h);
    float w = roundf(snap->w * fit), h = roundf(snap->h * fit);
    gfx::image(snap->tex, SHOT_X + floorf((SHOT_W - w) / 2), SHOT_Y + floorf((SHOT_H - h) / 2), w, h, { 1, 1, 1, a });
}

// Draws a game's facts line up to x = end. The lead in the accent colour is the tag (CO-OP or VERSUS in
// Recommended) when present, otherwise the year.
static void draw_facts(const Game& g, float x, float baseline, float end, float a) {
    std::string lead = g.tag.empty() ? g.year : g.tag, rest;
    std::vector<std::string> parts;
    if (!g.tag.empty() && !g.year.empty()) parts.push_back(g.year);
    if (!g.maker.empty()) parts.push_back(upper(g.maker));
    if (!g.category.empty()) parts.push_back(upper(g.category));
    for (const std::string& part : parts)
        rest += ((rest.empty() && lead.empty()) ? "" : " \xC2\xB7 ") + part;
    if (!lead.empty())
        x = lead_text(21, x, baseline, lead, a);
    mono_text(21, x, baseline, text::fit(mono, 21, rest, end - x, 21 * MONO_SPACING), faded(MUTED, a));
}

// Draws the wheel logo and box over the lower corners of the screenshot and the facts line below.
static void draw_game_extras(const Game& g, float a) {
    if (a <= 0)
        return;
    Picture* logo = wheel_picture(g);
    if (logo && logo->ready) {
        images::draw(logo, 800, 830 - logo->h, a);
    } else if (!logo || logo->failed) {
        // Without a wheel logo, draw the title in its place.
        std::string title = text::fit(bold, 56, g.title, 640);
        gfx::shadow(800, 764, text::width(bold, 56, title), 56, 20, 110, { 0, 0, 0, 0.6f * a });
        text::draw(bold, 56, 800, 812, title, faded(TEXT, a));
    }

    float facts_end = 1859;
    Picture* box = box_picture(g);
    if (box && box->ready) {
        images::draw(box, 1859 - box->w, 900 - box->h, a);
        facts_end = 1859 - box->w - 40;
    }

    draw_facts(g, SHOT_X, 901, facts_end, a);
}

// Draws a list with the selected line fixed at y = middle, fading lines beyond 6.5 rows.
template <typename Label>
static void draw_centred_list(int count, int selected, float middle, float shift, float others, Label label) {
    for (int offset = -10; offset <= 10; offset++) {
        int index = selected + offset;
        if (index < 0 || index >= count)
            continue;
        float gap = offset > 0 ? 5.0f : offset < 0 ? -5.0f : 0.0f;
        float centre = middle + offset * 49 + gap + shift;
        float away = fabsf(centre - middle) / 49;
        float shown = away > 6.5f ? (8.9f - away) / 2.4f : 1.0f;
        if (shown <= 0)
            continue;
        if (offset == 0) {
            // Shrink a long name in steps before truncating it.
            std::string name = label(index);
            float size = 40;
            for (float smaller : { 36.0f, 32.0f })
                if (text::width(bold, size, name) > GAME_LIST_WIDTH)
                    size = smaller;
            text::draw(bold, size, LIST_X, centre + 12 * size / 40, text::fit(bold, size, name, GAME_LIST_WIDTH),
                       faded(TEXT, shown));
        } else
            text::draw(regular, 29, LIST_X, centre + 8, text::fit(regular, 29, label(index), GAME_LIST_WIDTH),
                       faded(MUTED, others * shown));
    }
}

// Draws the top line of a list: the console logo, or the name as text for a console without a
// logo, followed by the position in the list.
static void draw_top_line(const std::string& name, Picture* logo, const std::string& place, float place_alpha) {
    float x = 296;
    if (logo && logo->ready) {
        images::draw(logo, x, floorf(83 - logo->h / 2));
        x += logo->w + 28;
    } else if (logo && !logo->failed) {
        return;                 // logo still loading
    } else {
        mono_text(21, x, 90, name, ACCENT);
        x += mono_width(21, name) + 23;
    }
    mono_text(21, x, 90, place, faded(MUTED, place_alpha));
}

// Recommended shows its games as a grid of boxes with titles, five per row and two rows in view.
static const int GRID_COLUMNS = 5, GRID_ROWS = 2;
static const float GRID_LEFT = 61, GRID_WIDTH = 1798, GRID_TOP = 150;
static const float GRID_ROW = 374;                                  // row pitch
static const float GRID_BOX_W = 300, GRID_BOX_H = 262, GRID_SLOT = 270;   // box size limit; GRID_SLOT is the box bottom below the row top
static const Shadow GRID_SHADOW = { 19, 19, 0.55f };
static int grid_top = 0;                    // first visible row
static float grid_shift = 0;                // scroll offset of the rows from their rest position, in pixels

static bool is_grid(const Console& c) {
    return c.kind == "recommended";
}

static Picture* grid_box(const Game& g, bool ahead = false) {
    return g.box.empty() ? nullptr : images::get(g.box, GRID_BOX_W, GRID_BOX_H, images::TRIM, GRID_SHADOW, ahead);
}

// Draws s centred on x = middle in up to two lines, with an ellipsis when it overflows.
static void draw_centred_lines(Font* font, float size, float middle, float top, float width, const std::string& s, Color c) {
    std::vector<std::string> lines;
    std::string line, word;
    auto end_word = [&]() {
        if (word.empty())
            return;
        std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && lines.empty() && text::width(font, size, tried) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ')
            end_word();
        else
            word += ch;
    }
    end_word();
    if (!line.empty())
        lines.push_back(line);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string shown = text::fit(font, size, lines[i], width);
        text::draw(font, size, floorf(middle - text::width(font, size, shown) / 2), top + size + i * (size + 6), shown, c);
    }
}

static void draw_grid(const Console& c) {
    int count = (int)c.games.size(), selected = game_sel[console_sel];
    float cell = GRID_WIDTH / GRID_COLUMNS;
    for (int row = grid_top - 1; row <= grid_top + GRID_ROWS; row++) {
        if (row < 0)
            continue;
        float top = GRID_TOP + (row - grid_top) * GRID_ROW + grid_shift;
        // Rows scrolling in or out fade with their distance from the visible rows.
        float away = std::max(GRID_TOP - top, top - (GRID_TOP + (GRID_ROWS - 1) * GRID_ROW)) / GRID_ROW;
        float shown = away <= 0 ? 1 : 1 - away * 1.6f;
        for (int column = 0; column < GRID_COLUMNS; column++) {
            int index = row * GRID_COLUMNS + column;
            if (index >= count)
                break;
            const Game& g = c.games[index];
            bool on = index == selected;
            float alpha = shown;
            if (alpha <= 0)
                continue;
            float middle = GRID_LEFT + cell * (column + 0.5f);
            Picture* box = grid_box(g);
            bool pictured = box && box->ready;
            float w = pictured ? box->w : 190, h = pictured ? box->h : GRID_BOX_H;
            float x = floorf(middle - w / 2), y = floorf(top + GRID_SLOT - h);
            if (pictured)
                images::draw(box, x, y, alpha * (on ? 1 : 0.86f));
            if (on)
                gfx::round_rect(x - 13, y - 13, w + 26, h + 26, 4, faded(ACCENT, alpha), 4);
            draw_centred_lines(on ? bold : regular, 22, middle, top + GRID_SLOT + 21, 320, g.title, faded(on ? TEXT : MUTED, alpha));
        }
    }
    if (selected < count)
        draw_facts(c.games[selected], GRID_LEFT, 933, 1300, 1);
}

static void move_grid(int to) {
    const Console& c = consoles[console_sel];
    int count = (int)c.games.size();
    int& selected = game_sel[console_sel];
    // Down from the row above a shorter last row selects the last game.
    if (to >= count && selected / GRID_COLUMNS < (count - 1) / GRID_COLUMNS)
        to = count - 1;
    if (to < 0 || to >= count || to == selected)
        return;
    selected = to;
    int row = to / GRID_COLUMNS, before = grid_top;
    if (row < grid_top) grid_top = row;
    if (row > grid_top + GRID_ROWS - 1) grid_top = row - (GRID_ROWS - 1);
    grid_shift += (grid_top - before) * GRID_ROW;
    note_until = 0;
}

static void draw_games() {
    if (consoles.empty())
        return;
    const Console& c = consoles[console_sel];
    int selected = game_sel[console_sel];
    int count = (int)c.games.size();
    bool grid = is_grid(c);

    // Recommended slides away empty after Select took out its last game.
    draw_top_line(upper(c.name), console_logo_small(c), count ? grouped(selected + 1) + " OF " + grouped(count) : "", 1);
    if (grid) {
        draw_grid(c);
    } else {
        draw_centred_list(count, selected, GAME_LIST_MIDDLE, list_shift, 1, [&](int i) { return c.games[i].title; });

        // The screenshot frame stays in place while its pictures change. Its dark fill shows
        // until a fully opaque screenshot covers it.
        bool staged = game_stage.shown >= 0 && game_stage.shown < count;
        Picture* shot = staged ? snap_picture(c.games[game_stage.shown]) : nullptr;
        if (!(shot && shot->ready && game_stage.alpha >= 1))
            gfx::round_rect(SHOT_X, SHOT_Y, SHOT_W, SHOT_H, SHOT_ROUND, INK);
        if (staged)
            draw_game_shot(c.games[game_stage.shown], game_stage.alpha);
        gfx::round_rect(SHOT_X - 4, SHOT_Y - 4, SHOT_W + 8, SHOT_H + 8, SHOT_ROUND + 4, faded(ACCENT, 0.55f), 4);
        if (staged)
            draw_game_extras(c.games[game_stage.shown], game_stage.alpha);
    }

    if (grid && now_s < remove_until)
        mono_text(21, GRID_LEFT, 975, "PRESS SELECT AGAIN TO REMOVE", ACCENT);
    else if (!note.empty() && now_s < note_until)
        mono_text(21, grid ? GRID_LEFT : SHOT_X, grid ? 975 : 943, note, ACCENT);

    float right = hint(1859, "STICK", "BROWSE", 1);
    right = hint(right, "B", "CONSOLES", 1);
    right = hint(right, "A", "PLAY", 1);
    if (grid)
        hint(right, "SELECT", "REMOVE", 1);
}

// ------------------------------------------------------------------ loading screen
//
// While a game starts, its whole screenshot stands in the middle of the display over a blurred
// copy of itself, with LOADING in pixel letters and a moving bar at the bottom. While a RetroPie
// tool opens or the menu looks for new games, the word and the bar stand larger in the middle
// of the empty background. The menu draws the first frames and leaves the last one in /dev/fb0,
// where loading.c keeps the bar moving until the emulator, the tool or the menu draws.

// Sizes and positions in canvas pixels. The lit part of the bar moves in steps of one pixel of
// the LOADING art.
struct LoadLayout {
    float word_pixel;       // canvas pixels per pixel of the LOADING art
    float word_y;           // top of the word
    float bar_w, bar_h, bar_y;
    float bar_part;         // length of the lit part
};
static const LoadLayout GAME_LOAD = { 5, 906, 400, 10, 972, 100 };
static const LoadLayout WAIT_LOAD = { 8, 484, 448, 16, 580, 128 };
static LoadLayout load = GAME_LOAD;
static const double LOAD_BAR_ROUND = 1.6;               // seconds per sweep, as the STARTING bar in looks.c
static const Color LOAD_TRACK = rgb(BRAND_LOAD_TRACK);  // the accent at about 25 % brightness

// A copy of the screenshot only a few pixels across. Scaled up with linear filtering, it fills
// the display as a blurred backdrop around the screenshot.
static Picture* loading_backdrop(const Game& g, bool ahead = false) {
    return g.snap.empty() ? nullptr : images::get(g.snap, 24, 24, 0, NO_SHADOW, ahead);
}

static Picture* loading_word() {
    return images::get(HOME + "/art/loading-word.png", 0, 0, images::AS_IS | images::PIXELS);
}

static const Game& launched_game() {
    return consoles[console_sel].games[game_sel[console_sel]];
}

// The lit part of the bar at t seconds, [from, to) in pixels from the left end of the bar.
// loading.c computes the same in screen pixels.
static void load_bar_part(double t, float& from, float& to) {
    double at = t / LOAD_BAR_ROUND;
    at -= floor(at);
    at = at * at * (3 - 2 * at);
    float lead = floorf(((float)at * (load.bar_w + load.bar_part) - load.bar_part) / load.word_pixel) * load.word_pixel;
    from = std::max(lead, 0.0f);
    to = std::min(lead + load.bar_part, load.bar_w);
}

// Draws LOADING and the bar, centred, at the positions of the current layout.
static void draw_load_word_and_bar() {
    Picture* word = loading_word();
    if (word->ready) {
        float w = word->w * load.word_pixel, h = word->h * load.word_pixel;
        gfx::image(word->tex, floorf(960 - w / 2), load.word_y, w, h);
    }
    float bar_x = 960 - load.bar_w / 2, from, to;
    gfx::rect(bar_x, load.bar_y, load.bar_w, load.bar_h, LOAD_TRACK);
    load_bar_part(now_s - loading_since, from, to);
    if (to > from)
        gfx::rect(bar_x + from, load.bar_y, to - from, load.bar_h, ACCENT);
}

static void draw_loading_screen() {
    const Game& g = launched_game();
    Picture* shot = snap_picture(g);
    Picture* backdrop = loading_backdrop(g);
    if (shot && shot->ready) {
        if (backdrop && backdrop->ready) {
            float cover = std::max(1920 / backdrop->w, 1080 / backdrop->h);
            float w = ceilf(backdrop->w * cover), h = ceilf(backdrop->h * cover);
            gfx::image(backdrop->tex, floorf(960 - w / 2), floorf(540 - h / 2), w, h);
            gfx::rect(0, 0, 1920, 1080, { 0, 0, 0, 0.6f });
        }
        // The whole screenshot at the largest whole-number scale that fits, so that every pixel
        // of the game has the same size. Larger screenshots scale down to fit.
        float fit = std::min(1920 / shot->w, 1080 / shot->h);
        float scale = fit >= 1 ? floorf(fit) : fit;
        float w = roundf(shot->w * scale), h = roundf(shot->h * scale);
        gfx::image(shot->tex, floorf(960 - w / 2), floorf(540 - h / 2), w, h);
    } else {
        gfx::background();
        Picture* box = box_picture(g);
        if (box && box->ready)
            images::draw(box, floorf(960 - box->w / 2), floorf(480 - box->h / 2));
    }
    // A dark gradient keeps the word and the bar readable over any screenshot.
    const float x[4] = { 0, 1920, 1920, 0 }, y[4] = { 640, 640, 1080, 1080 };
    const Color clear = { 0, 0, 0, 0 }, dark = { 0, 0, 0, 0.88f };
    const Color colours[4] = { clear, clear, dark, dark };
    gfx::corners(nullptr, x, y, nullptr, nullptr, colours);
    draw_load_word_and_bar();
}

// The screen while a RetroPie tool opens or the menu looks for new games.
static void draw_wait_screen() {
    gfx::background();
    images::draw(corner_logo(), 61, 58, 1);
    draw_load_word_and_bar();
}

// ------------------------------------------------------------------ settings screen

// Facts about the machine from pi/machine-info.sh. -1 and empty strings stand for facts that
// the script left out.
struct MachineFacts {
    std::string address, link, network;
    int signal = -1, temperature = -1, clock = -1;
    long memory_used = -1, memory_total = -1, uptime = -1, power = -1;
    double card_free = -1, card_total = -1;           // GB
    bool bluetooth = false;
    std::vector<std::string> devices;                // connected Bluetooth devices
};

static MachineFacts facts;                           // what the settings screen draws
static MachineFacts facts_read;                      // the helper thread's latest result
static bool facts_fresh = false;                     // facts_read is newer than facts
static std::mutex facts_mutex;
static std::atomic<bool> facts_wanted(false), facts_stop(false);
static std::thread facts_thread;

static MachineFacts read_facts() {
    MachineFacts f;
    FILE* pipe = popen(("nice -n 10 /bin/sh '" + HOME + "/machine-info.sh' 2>/dev/null").c_str(), "r");
    if (!pipe)
        return f;
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        size_t space = line.find(' ');
        if (space == std::string::npos)
            continue;
        std::string key = line.substr(0, space), value = line.substr(space + 1);
        const char* v = value.c_str();
        if (key == "address") f.address = value;
        else if (key == "link") f.link = value;
        else if (key == "network") f.network = value;
        else if (key == "signal") f.signal = atoi(v);
        else if (key == "bluetooth") f.bluetooth = value == "on";
        else if (key == "device") f.devices.push_back(value);
        else if (key == "temperature") f.temperature = atoi(v);
        else if (key == "clock") f.clock = atoi(v);
        else if (key == "memory") sscanf(v, "%ld %ld", &f.memory_used, &f.memory_total);
        else if (key == "uptime") f.uptime = atol(v);
        else if (key == "power") f.power = strtol(v, nullptr, 16);
        else if (key == "card") {
            long free_kb = 0, total_kb = 0;
            if (sscanf(v, "%ld %ld", &free_kb, &total_kb) == 2 && total_kb > 0) {
                f.card_free = free_kb / 1048576.0;
                f.card_total = total_kb / 1048576.0;
            }
        }
    }
    pclose(pipe);
    return f;
}

// Runs the script every three seconds while the settings screen shows. One run takes about
// 125 ms on the Pi, too long for the frame loop.
static void gather_facts() {
    while (!facts_stop) {
        if (!facts_wanted) {
            SDL_Delay(100);
            continue;
        }
        MachineFacts f = read_facts();
        {
            std::lock_guard<std::mutex> lock(facts_mutex);
            facts_read = f;
            facts_fresh = true;
        }
        for (int i = 0; i < 30 && facts_wanted && !facts_stop; i++)
            SDL_Delay(100);
    }
}

static void want_facts(bool wanted) {
    facts_wanted = wanted;
    if (wanted && !facts_thread.joinable())
        facts_thread = std::thread(gather_facts);
}

// Takes the helper thread's latest facts. Returns true when they are new.
static bool take_facts() {
    std::lock_guard<std::mutex> lock(facts_mutex);
    if (!facts_fresh)
        return false;
    facts = facts_read;
    facts_fresh = false;
    return true;
}

static const Color WARM = rgb(BRAND_WARNING);        // warnings
static const Color ALERT = rgb(255, 112, 112);       // problems
static const Color GLASS = { 1, 1, 1, 0.05f };       // tile and pill grounds
static const Color TRACK = { 1, 1, 1, 0.1f };        // empty part of a meter

// One fact as the settings screen shows it.
struct Fact {
    const char* icon;                                // art/icons/<icon>.png
    std::string label, value, detail;
    float meter;                                     // 0 to 1 for a bar, -1 for none
    Color colour;
};

static Color level(float x, float warn, float alert) {
    return x >= alert ? ALERT : x >= warn ? WARM : ACCENT;
}

static std::string one_decimal(double x) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "%.1f", x);
    return buffer;
}

static std::vector<Fact> machine_facts() {
    const MachineFacts& f = facts;
    std::vector<Fact> list;
    if (f.temperature >= 0)
        list.push_back({ "cpu", "CPU", std::to_string(f.temperature) + " \xC2\xB0" "C",
                         f.clock > 0 ? std::to_string(f.clock) + " MHz" : "", f.temperature / 85.0f,
                         level((float)f.temperature, 65, 80) });
    if (f.memory_total > 0) {
        float used = (float)f.memory_used / f.memory_total;
        list.push_back({ "memory", "MEMORY", std::to_string(lroundf(used * 100)) + " %",
                         std::to_string(f.memory_used) + " of " + std::to_string(f.memory_total) + " MB", used,
                         level(used, 0.75f, 0.9f) });
    }
    if (f.card_total > 0) {
        float used = 1 - (float)(f.card_free / f.card_total);
        list.push_back({ "card", "CARD", one_decimal(f.card_free) + " GB free", "of " + one_decimal(f.card_total) + " GB",
                         used, level(used, 0.85f, 0.95f) });
    }
    if (f.address.empty())
        list.push_back({ "wifi", "NETWORK", "Offline", "", -1, MUTED });
    else if (f.link == "wifi")
        list.push_back({ "wifi", "WI-FI", f.network.empty() ? "Wi-Fi" : f.network, f.address,
                         f.signal >= 0 ? std::min(f.signal, 100) / 100.0f : -1, ACCENT });
    else
        list.push_back({ "ethernet", "CABLE", "Ethernet", f.address, -1, ACCENT });

    if (!f.bluetooth)
        list.push_back({ "bluetooth", "BLUETOOTH", "Off", "", -1, MUTED });
    else if (f.devices.empty())
        list.push_back({ "bluetooth", "BLUETOOTH", "On", "0 connected", -1, ACCENT });
    else
        list.push_back({ "bluetooth", "BLUETOOTH", f.devices[0],
                         f.devices.size() == 1 ? "connected" : std::to_string(f.devices.size()) + " connected", -1, ACCENT });

    if (f.uptime >= 0) {
        long hours = f.uptime / 3600, minutes = f.uptime / 60 % 60;
        list.push_back({ "clock", "UPTIME", (hours ? std::to_string(hours) + " h " : std::string()) + std::to_string(minutes) + " min",
                         "", -1, ACCENT });
    }
    int games = 0, systems = 0;
    for (const Console& c : consoles) {
        if (c.kind == "console") {
            games += (int)c.games.size();
            systems++;
        }
    }
    list.push_back({ "gamepad", "GAMES", std::to_string(games), "in " + std::to_string(systems) + (systems == 1 ? " console" : " consoles"),
                     -1, ACCENT });
    // vcgencmd get_throttled sets bit 0 while the supply gives too little voltage and bit 16
    // once that has happened since the start.
    if (f.power >= 0) {
        if (f.power & 0x1)
            list.push_back({ "bolt", "POWER", "Too low", "supply voltage now", -1, ALERT });
        else if (f.power & 0x10000)
            list.push_back({ "bolt", "POWER", "Was low", "since the start", -1, WARM });
        else
            list.push_back({ "bolt", "POWER", "OK", "supply voltage", -1, ACCENT });
    }
    return list;
}

static void draw_icon(const char* name, float x, float y, float size, Color tint) {
    Picture* p = images::get(HOME + "/art/icons/" + name + ".png", 0, 0, images::AS_IS);
    if (p && p->ready)
        gfx::image(p->tex, x, y, size, size, tint);
}

static void meter_bar(float x, float y, float w, float h, float share, Color colour) {
    gfx::round_rect(x, y, w, h, h / 2, TRACK);
    if (share > 0)
        gfx::round_rect(x, y, std::max(h, w * std::min(share, 1.0f)), h, h / 2, colour);
}

// The machine facts fill the area below the setting's description: x 787 to 1859 and from
// FACTS_TOP down to above the button hints.
static const float FACTS_TOP = 622;

// Four columns of two rows of tiles, each with icon, label, value, detail and meter.
static void draw_facts_tiles(const std::vector<Fact>& list) {
    const float w = 256, h = 156, gap = 16;
    for (size_t i = 0; i < list.size() && i < 8; i++) {
        const Fact& f = list[i];
        float x = 787 + (i % 4) * (w + gap), y = FACTS_TOP + (float)(i / 4) * (h + gap);
        gfx::round_rect(x, y, w, h, 16, GLASS);
        draw_icon(f.icon, x + 20, y + 20, 28, f.colour);
        mono_text(15, x + 58, y + 40, f.label, MUTED);
        text::draw(bold, 30, x + 20, y + 94, text::fit(bold, 30, f.value, w - 40), TEXT);
        if (!f.detail.empty())
            mono_text(16, x + 20, y + 124, text::fit(mono, 16, f.detail, w - 40, 16 * MONO_SPACING), MUTED);
        if (f.meter >= 0)
            meter_bar(x + 20, y + h - 18, w - 40, 5, f.meter, f.colour);
    }
}

static void draw_settings() {
    draw_top_line("SETTINGS", nullptr, "", 1);
    draw_centred_list(SETTING_COUNT, setting_sel, SETTING_LIST_MIDDLE, list_shift, 1,
                      [&](int i) { return std::string(SETTINGS[i].name); });

    const Setting& s = SETTINGS[setting_sel];
    text::draw(bold, 64, 787, 330, s.name, TEXT);
    draw_wrapped(regular, 29, 787, 372, 960, s.about, MUTED);
    if (s.confirm && now_s < confirm_until)
        mono_text(21, 787, 560, std::string("PRESS A AGAIN TO ") + upper(s.name), ACCENT);
    else if (!note.empty() && now_s < note_until)
        mono_text(21, 787, 560, note, ACCENT);

    mono_text(18, 787, FACTS_TOP - 24, "THIS MACHINE", ACCENT);
    // The version of this program, level with THIS MACHINE and ending at the right edge of the tiles.
    // mono_width counts the letter spacing after the last letter too.
    const std::string version = "VERSION " LAUNCHER_VERSION;
    mono_text(18, 1859 - (mono_width(18, version) - 18 * MONO_SPACING), FACTS_TOP - 24, version, MUTED);
    draw_facts_tiles(machine_facts());

    float right = hint(1859, "STICK", "BROWSE", 1);
    right = hint(right, "B", "CONSOLES", 1);
    hint(right, "A", "OPEN", 1);
}

// ------------------------------------------------------------------ hidden screen
//
// The Konami code on the console screen opens this screen. It shows a spinning ASCII-art logo
// and an achievement notice in the style of the Dungeon Crawler Carl books. The notice's
// reward is a QR code that links to the source code.

// A LogoRun is a run of characters in one shade, starting at one row and column of a logo frame.
struct LogoRun {
    int row, column, shade;
    std::string letters;
};

static std::vector<std::vector<LogoRun>> logo_frames;
static int logo_columns = 0;
static bool logo_read = false;
static const Color LOGO_SHADES[6] = { rgb(255, 190, 128), rgb(255, 165, 93), rgb(236, 146, 76),
                                      rgb(217, 127, 60),  rgb(190, 109, 51), rgb(161, 92, 43) };
static const double LOGO_TURN = 3.6;      // seconds per full rotation

// Loads art/credits/logo.txt, generated by tools/build_assets.py. A line "F" starts a frame.
// All remaining lines hold one tab-separated run each: row, column, shade and characters.
static void read_logo() {
    if (logo_read)
        return;
    logo_read = true;
    std::ifstream in(HOME + "/art/credits/logo.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (line == "F") {
            logo_frames.emplace_back();
            continue;
        }
        std::vector<std::string> f = split(line);
        if (f.size() < 4 || logo_frames.empty())
            continue;
        LogoRun run = { atoi(f[0].c_str()), atoi(f[1].c_str()), (atoi(f[2].c_str()) % 6 + 6) % 6, f[3] };
        logo_columns = std::max(logo_columns, run.column + (int)run.letters.size());
        logo_frames.back().push_back(run);
    }
}

// These strings hold the achievement notice. wrap_text turns a '\n' into a line break.
#define APOSTROPHE "\xE2\x80\x99"     // typographic apostrophe, U+2019 in UTF-8
static const char* NOTICE_TITLE = "Ancient Knowledge!";
static const char* NOTICE_LINES = "Cheaters get rewarded. It" APOSTROPHE "s a known bug.";
static const char* NOTICE_REWARD = "You" APOSTROPHE "ve unlocked the source code!";
// The QR code (assets/credits/github-qr.png) encodes https:// and this address, which the
// screen prints beside it. It is the public repository of this project.
static const char* NOTICE_LINK = "github.com/arkandas/arcade-launcher";

// The credit line at the bottom, after the bug of art/credits/bug.png, with the author's name
// in the orange of arkandas.com.
static const char* CREDIT_BEFORE = "\xC2\xA9 2026 ";       // the copyright sign in UTF-8
static const char* CREDIT_NAME = "Arkandas";
static const char* CREDIT_AFTER = ". All bugs reserved.";
static const Color AUTHOR_ORANGE = rgb(255, 164, 91);   // --color-brand-orange of arkandas.com
static const float BUG_PIXEL = 3;                       // canvas pixels per pixel of the bug art
// The space of the monospace font is as wide as a letter, which spreads the credit line's
// words apart. The line advances by this fraction of the font size for each space instead.
static const float CREDIT_SPACE = 0.32f;

// Draws one part of the credit line with the narrow spaces and returns its width. With
// draw false it only measures.
static float credit_part(float size, float x, float baseline, const std::string& s, Color c, bool draw) {
    float start = x;
    size_t from = 0;
    for (;;) {
        size_t space = s.find(' ', from);
        std::string word = s.substr(from, space == std::string::npos ? std::string::npos : space - from);
        if (draw && !word.empty())
            mono_text(size, x, baseline, word, c);
        x += mono_width(size, word);
        if (space == std::string::npos)
            return x - start;
        x += size * CREDIT_SPACE;
        from = space + 1;
    }
}

static void draw_credit() {
    const float size = 24, baseline = 1017, gap = 14;
    Picture* bug = images::get(HOME + "/art/credits/bug.png", 0, 0, images::AS_IS | images::PIXELS);
    float bug_w = bug->ready ? bug->w * BUG_PIXEL : 0, bug_h = bug->ready ? bug->h * BUG_PIXEL : 0;
    float total = bug_w + gap + credit_part(size, 0, baseline, CREDIT_BEFORE, MUTED, false) +
                  credit_part(size, 0, baseline, CREDIT_NAME, AUTHOR_ORANGE, false) +
                  credit_part(size, 0, baseline, CREDIT_AFTER, MUTED, false);
    float x = floorf(960 - total / 2);
    if (bug->ready)
        gfx::image(bug->tex, x, floorf(baseline - text::cap_height(mono, size) / 2 - bug_h / 2), bug_w, bug_h);
    x += bug_w + gap;
    x += credit_part(size, x, baseline, CREDIT_BEFORE, MUTED, true);
    x += credit_part(size, x, baseline, CREDIT_NAME, AUTHOR_ORANGE, true);
    credit_part(size, x, baseline, CREDIT_AFTER, MUTED, true);
}

static void draw_secret() {
    const float x[4] = { 0, 1920, 1920, 0 }, y[4] = { 0, 0, 1080, 1080 };
    const Color ground[4] = { INK, INK, INK, INK };
    gfx::corners(nullptr, x, y, nullptr, nullptr, ground, true);

    // The logo is ASCII art with one drawing per animation frame. Draw the runs of the frame
    // due at the current time in the monospaced font.
    read_logo();
    if (!logo_frames.empty()) {
        const float size = 14;
        float step = text::width(mono_medium, size, "@");       // monospace advance
        float line = step / 0.496f;                             // character cells are about twice as tall as wide
        float left = floorf(250 + (568 - logo_columns * step) / 2);
        size_t due = (size_t)(now_s / LOGO_TURN * logo_frames.size()) % logo_frames.size();
        for (const LogoRun& run : logo_frames[due])
            text::draw(mono_medium, size, left + run.column * step, 259 + (run.row + 1) * line, run.letters,
                       LOGO_SHADES[run.shade]);
    }

    // The achievement notice on the right stacks the cup icon with NEW ACHIEVEMENT!, the title,
    // the body text and the reward (the QR code with its URL). Offsets count from the top of
    // the cup. `top` centres the whole notice vertically.
    const float left = 980, room = 880, step = 46;
    const float cup_w = 90, cup_h = 96;                          // 15 x 16 pixel-art cup at 6 canvas pixels per pixel
    float title_size = 64;
    while (title_size > 40 && text::width(bold, title_size, NOTICE_TITLE) > room)
        title_size -= 2;
    std::vector<std::string> body = wrap_text(regular, 34, NOTICE_LINES, room);
    std::vector<std::string> reward = wrap_text(regular, 34, NOTICE_REWARD, room);
    float title_at = cup_h + 74;
    float body_at = title_at + 62;
    float label_at = body_at + step * (body.size() - 1) + 74;
    float reward_at = label_at + 50;
    float card_at = reward_at + step * (reward.size() - 1) + 34;
    float top = floorf(540 - (card_at + 237) / 2);

    Picture* cup = images::get(HOME + "/art/credits/cup.png", 0, 0, images::AS_IS | images::PIXELS);
    if (cup->ready)
        gfx::image(cup->tex, left, top, cup_w, cup_h);
    lead_text(26, left + cup_w + 30, top + cup_h / 2 + text::cap_height(mono_medium, 26) / 2, "NEW ACHIEVEMENT!", 1);
    text::draw(bold, title_size, left, top + title_at, NOTICE_TITLE, TEXT);
    for (size_t i = 0; i < body.size(); i++)
        text::draw(regular, 34, left, top + body_at + step * i, body[i], MUTED);
    lead_text(26, left, top + label_at, "REWARD", 1);
    for (size_t i = 0; i < reward.size(); i++)
        text::draw(regular, 34, left, top + reward_at + step * i, reward[i], TEXT);

    // Draw the QR code on a light card at seven canvas pixels per module.
    Picture* code = images::get(HOME + "/art/credits/github-qr.png", 0, 0, images::AS_IS | images::PIXELS);
    gfx::round_rect(left, top + card_at, 237, 237, 24, TEXT);
    if (code->ready)
        gfx::image(code->tex, left + 17, top + card_at + 17, 203, 203);
    // Print the URL beside the card, centred on its height, small enough to end inside the screen.
    const float link_size = 24;
    mono_text(link_size, left + 237 + 40, floorf(top + card_at + 237 / 2.0f + text::cap_height(mono, link_size) / 2),
              NOTICE_LINK, TEXT);
    draw_credit();
}

// ------------------------------------------------------------------ exit handoff

static void write_action(const std::string& what) {
    FILE* f = fopen(ACTION_FILE, "w");
    if (f) {
        fputs(what.c_str(), f);
        fputc('\n', f);
        fclose(f);
    }
}

// STATE_FILE holds one value per line: screen, console key, ROM of the selected game, setting
// index and first grid row.
static void save_state() {
    FILE* f = fopen(STATE_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%d\n%s\n", (int)screen, consoles.empty() ? "" : consoles[console_sel].key.c_str());
    if (!consoles.empty() && !consoles[console_sel].games.empty())
        fprintf(f, "%s\n", consoles[console_sel].games[game_sel[console_sel]].rom.c_str());
    else
        fprintf(f, "\n");
    fprintf(f, "%d\n%d\n", setting_sel, grid_top);
    fclose(f);
}

// Returns true when STATE_FILE names a console of the current library.
static bool restore_state() {
    std::ifstream in(STATE_FILE);
    std::string kind, key, rom, setting;
    if (!std::getline(in, kind) || !std::getline(in, key))
        return false;
    std::string top;
    std::getline(in, rom);
    std::getline(in, setting);
    std::getline(in, top);
    grid_top = std::max(0, atoi(top.c_str()));
    for (size_t i = 0; i < consoles.size(); i++) {
        if (consoles[i].key != key)
            continue;
        console_sel = (int)i;
        bool found = false;
        for (size_t g = 0; g < consoles[i].games.size(); g++) {
            if (consoles[i].games[g].rom == rom) {
                game_sel[i] = (int)g;
                found = true;
                break;
            }
        }
        int wanted = atoi(kind.c_str());
        if (wanted == S_GAMES && found) screen = S_GAMES;
        else if (wanted == S_SETTINGS && consoles[i].kind == "settings") screen = S_SETTINGS;
        setting_sel = std::min(std::max(atoi(setting.c_str()), 0), SETTING_COUNT - 1);
        return true;
    }
    return false;
}

// 8x8 Bayer matrix for ordered dithering to RGB565.
static const uint8_t SPREAD[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 },  { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 },  { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

// Reads the geometry of the console framebuffer from sysfs. Zeros mean unknown.
static void console_geometry(int& bits, int& fb_w, int& fb_h, int& stride) {
    char comma = 0;
    bits = fb_w = fb_h = stride = 0;
    std::ifstream("/sys/class/graphics/fb0/bits_per_pixel") >> bits;
    std::ifstream("/sys/class/graphics/fb0/virtual_size") >> fb_w >> comma >> fb_h;
    std::ifstream("/sys/class/graphics/fb0/stride") >> stride;
}

// Converts the frame rendered last into the format of the console framebuffer (/dev/fb0): top
// row first, RGB565 with ordered dithering or BGR with 24 or 32 bits per pixel. Returns an
// empty vector when the framebuffer size differs from the window size.
static std::vector<uint8_t> frame_for_console() {
    int bits, fb_w, fb_h, stride;
    console_geometry(bits, fb_w, fb_h, stride);
    int bytes = bits / 8;
    if (bytes < 2 || bytes > 4 || fb_w != gfx::win_w || fb_h != gfx::win_h)
        return {};
    if (stride < fb_w * bytes)
        stride = fb_w * bytes;

    std::vector<uint8_t> px = gfx::read_pixels();
    std::vector<uint8_t> out((size_t)stride * fb_h, 0);
    for (int y = 0; y < fb_h; y++) {
        const uint8_t* src = &px[(size_t)(fb_h - 1 - y) * fb_w * 4];
        uint8_t* dst = &out[(size_t)y * stride];
        for (int x = 0; x < fb_w; x++, src += 4, dst += bytes) {
            if (bytes == 2) {
                // Dither to avoid banding in RGB565.
                int t = SPREAD[y & 7][x & 7];
                int r = std::min(255, src[0] + (t >> 3)) >> 3;
                int g = std::min(255, src[1] + (t >> 4)) >> 2;
                int b = std::min(255, src[2] + (t >> 3)) >> 3;
                unsigned v = (unsigned)((r << 11) | (g << 5) | b);
                dst[0] = (uint8_t)(v & 0xFF);
                dst[1] = (uint8_t)(v >> 8);
            } else {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                if (bytes == 4) dst[3] = 255;
            }
        }
    }
    return out;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    close(fd);
    return done == bytes.size();
}

// Copies the frame rendered last into the console framebuffer. The display falls back to that
// framebuffer whenever the DRM master exits, so the frame stays visible until the next program
// draws. pi/arcade.sh keeps the terminal from drawing over it.
static void copy_frame_to_console() {
    std::vector<uint8_t> frame = frame_for_console();
    if (!frame.empty())
        write_file("/dev/fb0", frame);
}

extern char** environ;

// Starts launcher-load (loading.c), which animates the loading bar in /dev/fb0. For a game it
// runs until the emulator draws and then puts the game list frame (FRAME_FILE) back into
// /dev/fb0 for the moment the game ends. For a tool or a new look for games (wait) it runs
// until another program draws into /dev/fb0 or the next menu starts. LOADING_FILE passes the
// bar's position and size in screen pixels, the step of its lit part, its period, its
// colours, the start of its animation on CLOCK_MONOTONIC and 1 for a wait.
static void hand_over_load_bar(bool wait) {
    remove(GAME_DREW_FILE);         // stale marker from the previous game
    FILE* f = fopen(LOADING_FILE, "w");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double began = ts.tv_sec + ts.tv_nsec / 1e9 - (now_s - loading_since);
    auto pixels = [](float v) { return std::max(1, (int)lroundf(v * gfx::scale)); };
    auto level = [](float v) { return (int)lroundf(v * 255); };
    fprintf(f, "%d %d %d %d %d %d %.3f %d %d %d %d %d %d %.6f %d\n",
            (int)lroundf(gfx::off_x + (960 - load.bar_w / 2) * gfx::scale), (int)lroundf(gfx::off_y + load.bar_y * gfx::scale),
            pixels(load.bar_w), pixels(load.bar_h), pixels(load.bar_part), pixels(load.word_pixel), LOAD_BAR_ROUND,
            level(LOAD_TRACK.r), level(LOAD_TRACK.g), level(LOAD_TRACK.b), level(ACCENT.r), level(ACCENT.g), level(ACCENT.b),
            began, wait ? 1 : 0);
    fclose(f);
    std::string program = HOME + "/frontend/launcher-load";
    char* argv[] = { (char*)program.c_str(), nullptr };
    pid_t pid;
    if (posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv, environ) != 0)
        fprintf(stderr, "could not start %s\n", program.c_str());
}

// Development aid: writes the current frame to SHOT_FILE as a binary PPM.
static void write_screenshot() {
    std::vector<uint8_t> px = gfx::read_pixels();
    std::string tmp = std::string(SHOT_FILE) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", gfx::win_w, gfx::win_h);
    std::vector<uint8_t> row((size_t)gfx::win_w * 3);
    for (int y = gfx::win_h - 1; y >= 0; y--) {
        const uint8_t* src = &px[(size_t)y * gfx::win_w * 4];
        for (int x = 0; x < gfx::win_w; x++) {
            row[x * 3] = src[x * 4];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    rename(tmp.c_str(), SHOT_FILE);
    remove(SHOT_REQUEST);
}

// ------------------------------------------------------------------ update and render

// Moves value towards goal with an exponential ease-out of the given half-life in seconds,
// snapping to goal within 0.4. Sets animating while the value moves.
static float approach(float value, float goal, float dt, float half_life) {
    if (value == goal)
        return goal;
    value += (goal - value) * (1.0f - powf(0.5f, dt / half_life));
    if (fabsf(goal - value) < 0.4f)
        value = goal;
    animating = true;
    return value;
}

// Moves value linearly towards goal: a full change from 0 to 1 takes the given seconds. Sets
// animating while the value moves.
static float slide(float value, float goal, float dt, float seconds) {
    if (value == goal)
        return goal;
    float step = dt / seconds;
    value = value < goal ? std::min(goal, value + step) : std::max(goal, value - step);
    animating = true;
    return value;
}

// Fades the stage out when the selection changes. It fades back in once the selection has
// rested for 130 ms and its pictures have loaded, or after 1.5 s at the latest. A held stick
// moves on before that, so the stage stays empty while a list scrolls.
static void update_stage(Stage& stage, const char* name, int selected, bool ready, float dt) {
    if (stage.shown != selected) {
        stage.alpha = slide(stage.alpha, 0, dt, 0.08f);
        double waited = now_s - stage.since;
        if (stage.alpha <= 0 && waited > 0.13 && (ready || waited > 1.5))
            stage.shown = selected;
        animating = true;
    } else {
        stage.alpha = slide(stage.alpha, 1, dt, 0.12f);
        if (stage.alpha >= 1 && !stage.timed) {
            stage.timed = true;
            dev_note("%s: full picture %.0f ms after the press", name, (now_s - stage.since) * 1000);
        }
    }
}

static void select_console(int index, int way) {
    if (index == console_sel || consoles.empty())
        return;
    console_sel = index;
    console_way = way;
    console_stage.since = now_s;
    console_stage.timed = false;
}

// Up and down on the console screen jump between groups: down to the first console of the next
// group, up to the first console of the current group or, from there, of the previous one.
// Both directions wrap around.
static void jump_heading(int direction) {
    int count = (int)consoles.size();
    if (count == 0)
        return;
    const std::string& here = consoles[console_sel].group;
    int target = console_sel;
    if (direction > 0) {
        target = 0;
        for (int i = console_sel + 1; i < count; i++)
            if (consoles[i].group != here) { target = i; break; }
    } else {
        int first = console_sel;
        while (first > 0 && consoles[first - 1].group == here) first--;
        if (first < console_sel) {
            target = first;
        } else {
            int last = first == 0 ? count - 1 : first - 1;
            const std::string& before = consoles[last].group;
            while (last > 0 && consoles[last - 1].group == before) last--;
            target = last;
        }
    }
    select_console(target, direction);
}

static char first_letter(const std::string& title) {
    char ch = title.empty() ? ' ' : title[0];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    return (ch >= 'A' && ch <= 'Z') ? ch : '#';
}

static void move_game(int to) {
    const Console& c = consoles[console_sel];
    int count = (int)c.games.size();
    if (count == 0)
        return;
    int& selected = game_sel[console_sel];
    to = ((to % count) + count) % count;
    if (to == selected)
        return;
    int moved = to - selected;
    selected = to;
    list_shift = (moved == 1 || moved == -1) ? list_shift + moved * 49 : (moved > 0 ? 70.0f : -70.0f);
    game_way = moved > 0 ? 1 : -1;
    game_stage.since = now_s;
    game_stage.timed = false;
    note_until = 0;
}

// Left and right in a game list jump to the first game of the next or previous initial letter.
static void jump_letter(int direction) {
    const Console& c = consoles[console_sel];
    int count = (int)c.games.size();
    if (count == 0)
        return;
    int selected = game_sel[console_sel];
    char here = first_letter(c.games[selected].title);
    int i = selected;
    if (direction > 0) {
        while (i < count && first_letter(c.games[i].title) == here) i++;
        if (i >= count) i = 0;
    } else {
        // Go to the start of the current letter, or of the previous letter when already there.
        while (i > 0 && first_letter(c.games[i - 1].title) == here) i--;
        if (i == selected) {
            i = selected == 0 ? count - 1 : selected - 1;
            char before = first_letter(c.games[i].title);
            while (i > 0 && first_letter(c.games[i - 1].title) == before) i--;
        }
    }
    move_game(i);
}

// Select adds the selected game to the end of Recommended, and inside Recommended a second Select
// within four seconds takes the selected game out.
// RECOMMENDED_MINE in HOME keeps the list across restarts and deploys: M lines hold the list as
// the players left it, H lines the hand-picked list of library.tsv that it started from. A game
// that a later recommended.json adds to the hand-picked list joins at the end.
static const char* RECOMMENDED_MINE = "recommended-mine.tsv";
static std::vector<std::string> hand_picked;    // keys of the Recommended games in library.tsv

static std::string game_key(const Game& g) {
    return g.system + "\t" + g.rom;
}

static int recommended_index() {
    for (size_t i = 0; i < consoles.size(); i++)
        if (consoles[i].kind == "recommended")
            return (int)i;
    return -1;
}

// Returns the game with this key from the consoles' own lists, or nullptr.
static const Game* find_game(const std::string& key) {
    for (const Console& c : consoles) {
        if (c.kind == "recommended")
            continue;
        for (const Game& g : c.games)
            if (game_key(g) == key)
                return &g;
    }
    return nullptr;
}

// Replaces the hand-picked Recommended list of library.tsv with the players' list.
static void apply_mine() {
    int r = recommended_index();
    if (r < 0)
        return;
    Console& rec = consoles[r];
    hand_picked.clear();
    for (const Game& g : rec.games)
        hand_picked.push_back(game_key(g));
    std::ifstream in(HOME + "/" + RECOMMENDED_MINE);
    if (!in)
        return;
    std::vector<std::string> mine;
    std::set<std::string> started_from;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f = split(line);
        if (f.size() < 3)
            continue;
        if (f[0] == "M") mine.push_back(f[1] + "\t" + f[2]);
        else if (f[0] == "H") started_from.insert(f[1] + "\t" + f[2]);
    }
    std::vector<Game> games;
    std::set<std::string> taken;
    auto take = [&](const std::string& key) {
        if (taken.count(key))
            return;
        for (const Game& g : rec.games) {
            // A hand-picked game keeps the mode that recommended.json gives it.
            if (game_key(g) == key) {
                games.push_back(g);
                taken.insert(key);
                return;
            }
        }
        if (const Game* g = find_game(key)) {
            games.push_back(*g);
            taken.insert(key);
        }
    };
    for (const std::string& key : mine)
        take(key);
    for (const std::string& key : hand_picked)
        if (!started_from.count(key))
            take(key);
    rec.games = games;
}

static void save_mine() {
    int r = recommended_index();
    if (r < 0)
        return;
    std::string text;
    for (const Game& g : consoles[r].games)
        text += "M\t" + game_key(g) + "\n";
    for (const std::string& key : hand_picked)
        text += "H\t" + key + "\n";
    // Through a new file and a rename, so that a power cut keeps the old list or the new one.
    std::string path = HOME + "/" + RECOMMENDED_MINE;
    int fd = open((path + ".new").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    bool written = write(fd, text.data(), text.size()) == (ssize_t)text.size() && fsync(fd) == 0;
    close(fd);
    if (written)
        rename((path + ".new").c_str(), path.c_str());
}

static void add_to_recommended(const Game& g) {
    int r = recommended_index();
    if (r < 0)
        return;
    sound::play(sound::OPEN);
    std::string key = game_key(g);
    for (const Game& had : consoles[r].games)
        if (game_key(had) == key)
            return;
    consoles[r].games.push_back(g);
    save_mine();
}

static void remove_from_recommended() {
    Console& rec = consoles[console_sel];
    int& selected = game_sel[console_sel];
    if (selected < 0 || selected >= (int)rec.games.size())
        return;
    rec.games.erase(rec.games.begin() + selected);
    save_mine();
    sound::play(sound::BACK);
    note_until = 0;
    int count = (int)rec.games.size();
    if (count == 0) {
        selected = 0;
        grid_top = 0;
        mix_to = 0;             // back to the console screen
        return;
    }
    selected = std::min(selected, count - 1);
    int rows = (count + GRID_COLUMNS - 1) / GRID_COLUMNS, row = selected / GRID_COLUMNS;
    grid_top = std::max(0, std::min(grid_top, rows - GRID_ROWS));
    if (row < grid_top) grid_top = row;
    if (row > grid_top + GRID_ROWS - 1) grid_top = row - (GRID_ROWS - 1);
}

static std::string leaving;      // action for pi/arcade.sh; a non-empty value starts the exit

// The Konami code opens the hidden screen from the console screen.
static const Action SECRET_CODE[10] = { A_UP, A_UP, A_DOWN, A_DOWN, A_LEFT, A_RIGHT, A_LEFT, A_RIGHT, A_BACK, A_SELECT };
static std::deque<Action> last_actions;
static std::deque<int> consoles_before;     // console_sel before each action in last_actions

static void act(Action action) {
    if (action == A_TERMINAL) {
        leaving = "terminal";
        return;
    }
    if (consoles.empty())
        return;
    if (screen == S_CONSOLES) {
        last_actions.push_back(action);
        consoles_before.push_back(console_sel);
        if (last_actions.size() > 10) {
            last_actions.pop_front();
            consoles_before.pop_front();
        }
        if (last_actions.size() == 10 && std::equal(last_actions.begin(), last_actions.end(), SECRET_CODE)) {
            // The final A opens the hidden screen in place of the selected console. The code's
            // moves change the console, so the console from before the code comes back.
            select_console(consoles_before.front(), 0);
            last_actions.clear();
            consoles_before.clear();
            screen = S_SECRET;
            mix_to = 1;
            sound::play(sound::SECRET);
            return;
        }
    } else {
        last_actions.clear();
        consoles_before.clear();
    }
    if (screen == S_CONSOLES) {
        int count = (int)consoles.size(), before = console_sel;
        switch (action) {
        case A_LEFT: select_console((console_sel + count - 1) % count, -1); break;
        case A_RIGHT: select_console((console_sel + 1) % count, 1); break;
        case A_UP: jump_heading(-1); break;
        case A_DOWN: jump_heading(1); break;
        case A_SELECT:
            if (consoles[console_sel].kind == "settings") {
                screen = S_SETTINGS;
                want_facts(true);
            } else if (!consoles[console_sel].games.empty()) {
                screen = S_GAMES;
                game_stage = Stage();
                game_stage.since = now_s - 1;
                // Scroll the Recommended grid so that the selected game's row is on top, clamped
                // to keep the view full of rows near the end.
                int rows = ((int)consoles[console_sel].games.size() + GRID_COLUMNS - 1) / GRID_COLUMNS;
                grid_top = std::max(0, std::min(game_sel[console_sel] / GRID_COLUMNS, rows - GRID_ROWS));
                grid_shift = 0;
            } else {
                break;
            }
            mix_to = 1;
            list_shift = 0;
            sound::play(sound::OPEN);
            break;
        default: break;
        }
        if (console_sel != before)
            sound::play(sound::CONSOLE);
    } else if (screen == S_GAMES) {
        // Recommended can lose its last game while its grid shows; the console screen returns then.
        if (consoles[console_sel].games.empty())
            return;
        int selected = game_sel[console_sel];
        if (action == A_FAVOURITE) {
            if (!is_grid(consoles[console_sel])) {
                add_to_recommended(consoles[console_sel].games[selected]);
            } else if (now_s < remove_until && remove_index == selected) {
                remove_until = 0;
                remove_from_recommended();
            } else {
                // As Restart in Settings: the first press asks, a second press on the same game
                // within four seconds takes it out of Recommended.
                remove_until = now_s + 4;
                remove_index = selected;
                note_until = 0;
                sound::play(sound::MOVE);
            }
            return;
        }
        remove_until = 0;           // any other press keeps the game
        if (is_grid(consoles[console_sel]) && action <= A_RIGHT) {
            static const int STEP[4] = { -GRID_COLUMNS, GRID_COLUMNS, -1, 1 };      // up, down, left, right
            move_grid(selected + STEP[action]);
        } else {
            switch (action) {
            case A_UP: move_game(selected - 1); break;
            case A_DOWN: move_game(selected + 1); break;
            case A_LEFT: jump_letter(-1); break;
            case A_RIGHT: jump_letter(1); break;
            case A_BACK:
                mix_to = 0;
                sound::play(sound::BACK);
                break;
            case A_SELECT: {
                const Game& g = consoles[console_sel].games[selected];
                leaving = "game\n" + g.system + "\n" + g.rom;
                loading_since = now_s;
                sound::play(sound::PLAY);
                break;
            }
            default: break;
            }
        }
        if (game_sel[console_sel] != selected)
            sound::play(sound::MOVE);
    } else if (screen == S_SECRET) {
        if (action == A_BACK || action == A_SELECT) {
            mix_to = 0;
            sound::play(sound::BACK);
        }
    } else {
        int before = setting_sel;
        switch (action) {
        case A_UP:
            if (setting_sel > 0) { setting_sel--; list_shift -= 49; confirm_until = 0; note_until = 0; }
            break;
        case A_DOWN:
            if (setting_sel < SETTING_COUNT - 1) { setting_sel++; list_shift += 49; confirm_until = 0; note_until = 0; }
            break;
        case A_BACK:
            mix_to = 0;
            want_facts(false);
            sound::play(sound::BACK);
            break;
        case A_SELECT: {
            const Setting& s = SETTINGS[setting_sel];
            sound::play(sound::OPEN);
            if (s.confirm && now_s >= confirm_until) {
                confirm_until = now_s + 4;
                break;
            }
            leaving = s.action;
            break;
        }
        default: break;
        }
        if (setting_sel != before)
            sound::play(sound::MOVE);
    }
}

// Each strip wipes to its slice of the selected console once that screenshot has loaded. The
// wipes start 35 ms apart, beginning on the side the selection moved towards.
static void update_strips(float dt) {
    const Console& c = consoles[console_sel];
    for (int i = 0; i < STRIPS; i++) {
        Strip& s = strips[i];
        StripPicture wanted = strip_of(c, i);
        if (wanted != s.wanted) {
            s.wanted = wanted;
            // Stagger the wipes in the direction of the last move. Over the boot frame they run
            // from the left, like the strips of the boot animation.
            s.change_at = std::max(now_s, strips_wait) + 0.035 * (console_way > 0 && !s.shown.start ? STRIPS - 1 - i : i);
        }
        s.down = slide(s.down, 1, dt, 0.3f);
        if (s.shown != s.wanted) {
            animating = true;
            if (s.down >= 1 && now_s >= s.change_at && settled(strip_picture(s.wanted))) {
                if (s.shown.start)
                    dev_note("strip %d comes down over the picture of the start after %.3f s", i + 1, now_s);
                s.under = s.shown;
                s.shown = s.wanted;
                s.down = 0;
            }
        }
    }
}

// Preloads the pictures the next press is likely to need, nearest first. In a game list these
// are the games around the selection, starting in the direction of the last move. On the
// console screen they are the game that A would open and the neighbouring consoles.
static const int GAMES_AHEAD = 10, CONSOLES_AHEAD = 2;

static void look_ahead() {
    const Console& c = consoles[console_sel];
    int count = (int)c.games.size();
    // first the loading screen of the selected game, so that A shows it at once
    if (screen == S_GAMES && count > 0) {
        snap_picture(c.games[game_sel[console_sel]], true);
        loading_backdrop(c.games[game_sel[console_sel]], true);
    }
    if (count > 0 && is_grid(c)) {
        // In the grid, preload the visible rows first, then the row below and the row above.
        int first = (screen == S_GAMES ? grid_top : game_sel[console_sel] / GRID_COLUMNS) * GRID_COLUMNS;
        for (int i = first; i < first + (GRID_ROWS + 1) * GRID_COLUMNS && i < count; i++)
            grid_box(c.games[i], true);
        for (int i = std::max(0, first - GRID_COLUMNS); i < first; i++)
            grid_box(c.games[i], true);
    } else if (count > 0) {
        int selected = game_sel[console_sel];
        console_logo_small(c, true);
        if (screen != S_GAMES)
            want_game(c.games[selected]);
        int reach = screen == S_GAMES ? GAMES_AHEAD : 1;
        for (int d = 1; d <= reach && d < count; d++) {
            want_game(c.games[(((selected + game_way * d) % count) + count) % count]);
            want_game(c.games[(((selected - game_way * d) % count) + count) % count]);
        }
    }
    if (mix < 1) {
        int total = (int)consoles.size();
        for (int d = 1; d <= CONSOLES_AHEAD && d < total; d++) {
            for (int way : { console_way, -console_way }) {
                const Console& near = consoles[(((console_sel + way * d) % total) + total) % total];
                console_logo(near, true);
                console_photo(near, true);
                box_row(near, true);
                for (int i = 0; i < STRIPS && i < (int)near.shots.size(); i++)
                    strip_picture(strip_of(near, i), true);
            }
        }
    }
}

static void update(float dt) {
    list_shift = approach(list_shift, 0, dt, 0.036f);
    mix = slide(mix, mix_to, dt, 0.22f);
    if (mix <= 0 && screen != S_CONSOLES) {
        screen = S_CONSOLES;
        console_stage.since = now_s - 1;
    }
    if (consoles.empty())
        return;

    pager_to = pager_goal();
    pager_at = approach(pager_at, pager_to, dt, 0.05f);
    update_strips(dt);          // also while hidden, so the strips are current when the console screen returns
    if (mix < 1)
        update_stage(console_stage, "console", console_sel, console_stage_ready(consoles[console_sel]), dt);
    if (screen == S_GAMES && !consoles[console_sel].games.empty()) {
        int selected = game_sel[console_sel];
        if (is_grid(consoles[console_sel]))
            grid_shift = approach(grid_shift, 0, dt, 0.05f);
        else
            update_stage(game_stage, "game", selected, game_stage_ready(consoles[console_sel].games[selected]), dt);
    }
    look_ahead();
    if (screen == S_SECRET)
        animating = true;       // spinning logo
    if ((confirm_until > 0 && now_s < confirm_until + 0.1) || (note_until > 0 && now_s < note_until + 0.1) ||
        (remove_until > 0 && now_s < remove_until + 0.1))
        animating = true;
}

// The game, settings and hidden screens enter and leave by sliding over the console screen
// from the right. Each screen draws only inside its clip region because the Pi's GPU cannot
// blend a full-screen translucent layer at 60 fps. `over` sets the opacity of the overlays
// (the corner logo and everything over the strips), which fade in at startup.
static void render(float over = 1) {
    gfx::begin();
    if (launching || waiting) {
        if (launching)
            draw_loading_screen();
        else
            draw_wait_screen();
        gfx::flush();
        return;
    }
    float eased = mix * mix * (3 - 2 * mix);
    float edge = roundf(1920 * (1 - eased));         // left edge of the incoming screen
    if (edge > 0) {
        gfx::clip(0, edge);
        draw_strips();
        draw_consoles(over);
        if (edge < 1920) {
            // Draw the shadow that the sliding screen casts to its left.
            const float x[4] = { edge - 70, edge, edge, edge - 70 }, y[4] = { 0, 0, 1080, 1080 };
            const Color dark = { 0, 0, 0, 0.5f }, none = { 0, 0, 0, 0 };
            const Color colours[4] = { none, dark, dark, none };
            gfx::corners(nullptr, x, y, nullptr, nullptr, colours);
        }
    }
    if (edge < 1920) {
        gfx::clip(edge, 1920);
        gfx::shift(edge);
        if (screen == S_GAMES) {
            if (is_grid(consoles[console_sel]))
                gfx::background();
            else
                gfx::background(true, SHOT_ROOM);       // with the frame shadow, skipping the screenshot area
            draw_games();
        } else if (screen == S_SETTINGS) {
            gfx::background();
            draw_settings();
        } else if (screen == S_SECRET) {
            draw_secret();
        }
        gfx::shift(0);
    }
    // The hidden screen covers the corner logo as it slides in.
    gfx::clip(0, screen == S_SECRET ? edge : 1920);
    images::draw(corner_logo(), 61, 58, over);
    gfx::clip(0, 1920);
    gfx::flush();
}

// Fills the font atlases incrementally for the text sizes in USES, up to `most` glyphs per
// frame. Without this, a list that appears for the first time draws all its glyphs in one
// frame, which then misses the vertical blank.
static void prepare_letters(int most) {
    static const char* LETTERS = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\xC2\xB7";
    struct Use { Font** font; float size; };
    static const Use USES[] = { { &bold, 40 }, { &regular, 29 }, { &mono, 21 }, { &mono_medium, 21 }, { &mono, 18 },
                                { &mono, 24 }, { &mono_medium, 24 }, { &bold, 36 }, { &bold, 32 },
                                { &bold, 22 }, { &regular, 22 } };
    static size_t at = 0;
    while (at < sizeof USES / sizeof USES[0] && text::prepare(*USES[at].font, USES[at].size, LETTERS, most) == 0)
        at++;
}

// ------------------------------------------------------------------ screensaver
//
// The brand logo with ARCADE below it bounces around a black screen, above a fixed line of
// pixel text (SAVER_LINE in tools/build_assets.py) that pulses gently. Each bounce gives the
// logo mark a new random colour while the lettering stays white.

static const double SAVER_AFTER = 600;      // seconds without input
static const float NAME_GAP = 54;           // gap between the logo and ARCADE, as in looks.c
static const float WORD_PIXEL = 16;         // canvas pixels per pixel of the ARCADE art, as in looks.c
static const float LINE_PIXEL = 3;          // canvas pixels per pixel of the bottom line art
static const float LINE_BOTTOM = 1024;      // bottom edge of the line, level with the button hints
static const float LINE_ROOM = 60;          // clearance between the bouncing logo and the line
static const double LINE_PULSE = 1.6;       // seconds per pulse of the line, between 35 % and full opacity
static float saver_x = 420, saver_y = 500, saver_vx = 105, saver_vy = 74;
static float saver_hue = -1;                // hue of the mark in degrees, negative while it is white
static Color saver_tint = { 1, 1, 1, 1 };

// A bright colour of the given hue (0 to 360 degrees).
static Color bright(float hue) {
    float h = fmodf(hue, 360) / 60, f = h - floorf(h);
    const float s = 0.7f, p = 1 - s, q = 1 - s * f, t = 1 - s * (1 - f);
    switch ((int)h % 6) {
    case 0: return { 1, t, p, 1 };
    case 1: return { q, 1, p, 1 };
    case 2: return { p, 1, t, 1 };
    case 3: return { p, q, 1, 1 };
    case 4: return { t, p, 1, 1 };
    default: return { 1, p, q, 1 };
    }
}

// A random hue at least 60 degrees away from the current one, so that every bounce shows.
static void next_saver_tint() {
    float hue;
    do {
        hue = (float)(rand() % 360);
    } while (saver_hue >= 0 && fabsf(fmodf(hue - saver_hue + 540, 360) - 180) < 60);
    saver_hue = hue;
    saver_tint = bright(hue);
}

struct SaverPictures {
    Picture* mark;
    Picture* letters;
    Picture* word;
    Picture* line;
};

// Requests the screensaver pictures. Call it every frame of the screensaver and of the fade
// before it, so that the pictures have loaded when the screensaver starts.
static SaverPictures saver_pictures() {
    return { images::get(HOME + "/art/brand-logo-mark.png", 760, 220),
             images::get(HOME + "/art/brand-logo-letters.png", 760, 220),
             images::get(HOME + "/art/arcade-word.png", 0, 0, images::AS_IS | images::PIXELS),
             images::get(HOME + "/art/saver-line.png", 0, 0, images::AS_IS | images::PIXELS) };
}

static void render_saver(float dt) {
    SaverPictures p = saver_pictures();
    gfx::begin();
    if (p.mark->ready && p.letters->ready && p.word->ready && p.line->ready) {
        float word_w = p.word->w * WORD_PIXEL, word_h = p.word->h * WORD_PIXEL;
        float line_w = p.line->w * LINE_PIXEL, line_h = p.line->h * LINE_PIXEL;
        float line_y = LINE_BOTTOM - line_h;
        float w = std::max(p.mark->w, word_w), h = p.mark->h + NAME_GAP + word_h;
        float max_x = 1920 - w, max_y = line_y - LINE_ROOM - h;
        saver_x += saver_vx * dt;
        saver_y += saver_vy * dt;
        bool turned = false;
        if (saver_x < 0) { saver_x = -saver_x; saver_vx = -saver_vx; turned = true; }
        if (saver_y < 0) { saver_y = -saver_y; saver_vy = -saver_vy; turned = true; }
        if (saver_x > max_x) { saver_x = 2 * max_x - saver_x; saver_vx = -saver_vx; turned = true; }
        if (saver_y > max_y) { saver_y = 2 * max_y - saver_y; saver_vy = -saver_vy; turned = true; }
        if (turned)
            next_saver_tint();
        // Snap to whole canvas pixels so that the pixel art keeps sharp edges.
        float x = floorf(saver_x), y = floorf(saver_y);
        float logo_x = x + floorf((w - p.mark->w) / 2);
        gfx::image(p.mark->tex, logo_x, y, p.mark->w, p.mark->h, saver_tint);
        gfx::image(p.letters->tex, logo_x, y, p.letters->w, p.letters->h);
        // Centre ARCADE under the logo, ignoring the one-pixel drop shadow on its right.
        float word_x = x + floorf((w - (word_w - WORD_PIXEL)) / 2), word_y = y + p.mark->h + NAME_GAP;
        gfx::image(p.word->tex, word_x, word_y, word_w, word_h);
        float pulse = 0.675f + 0.325f * cosf((float)(now_s * 2 * M_PI / LINE_PULSE));
        gfx::image(p.line->tex, floorf((1920 - line_w) / 2), line_y, line_w, line_h, faded(ACCENT, pulse));
    }
    gfx::flush();
}

// ------------------------------------------------------------------ boot handoff
//
// boot.c draws the boot animation into /dev/fb0 while the Pi starts. The menu takes the
// display over without a visible break: it creates MENU_STARTS when it starts loading and
// MENU_READY once loaded, waits for START_DONE and reads the boot frame from /dev/fb0. Each
// strip starts with its slice of the boot frame before the screenshots wipe in.

// Checks /proc for a running launcher-boot process (boot.c).
static bool start_program_runs() {
    DIR* proc = opendir("/proc");
    if (!proc)
        return false;
    bool found = false;
    while (dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        std::string name;
        std::ifstream(std::string("/proc/") + entry->d_name + "/comm") >> name;
        if (name == "launcher-boot")
            found = true;
    }
    closedir(proc);
    return found;
}

// Signals MENU_READY, waits up to 3 s for START_DONE and loads the boot frame from /dev/fb0
// into start_picture (16-bit framebuffers only). Returns the wait in seconds.
static double take_start_picture() {
    Uint64 from = SDL_GetPerformanceCounter();
    touch(MENU_READY);
    if (start_program_runs())
        for (int waited = 0; waited < 3000 && !exists(START_DONE); waited += 2)
            SDL_Delay(2);
    double took = (SDL_GetPerformanceCounter() - from) / (double)SDL_GetPerformanceFrequency();

    int bits, fb_w, fb_h, stride;
    console_geometry(bits, fb_w, fb_h, stride);
    if (bits != 16 || fb_w != gfx::win_w || fb_h != gfx::win_h || stride < fb_w * 2)
        return took;            // other framebuffer format: start from the logo screen
    std::vector<uint8_t> rows((size_t)stride * fb_h);
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0)
        return took;
    size_t done = 0;
    while (done < rows.size()) {
        ssize_t n = read(fd, rows.data() + done, rows.size() - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    close(fd);
    if (done != rows.size())
        return took;
    std::vector<uint8_t> pixels((size_t)fb_w * 2 * fb_h);
    for (int y = 0; y < fb_h; y++)
        memcpy(&pixels[(size_t)y * fb_w * 2], &rows[(size_t)y * stride], (size_t)fb_w * 2);
    start_picture = gfx::make_565(pixels.data(), fb_w, fb_h);
    return took;
}

// ------------------------------------------------------------------ main

int main(int, char**) {
    Uint64 began = SDL_GetPerformanceCounter();
    if (!exists(BOOTED_FILE))
        touch(MENU_STARTS);     // cold boot: boot.c starts the ending of its animation
    if (getenv("LAUNCHER_HOME"))
        HOME = getenv("LAUNCHER_HOME");
    dev = getenv("LAUNCHER_DEV") != nullptr;
    if (dev)
        remove(DEV_KEYS);       // discard presses queued before this start
    if (getenv("LAUNCHER_LOAD_DEMO"))
        load_demo = atof(getenv("LAUNCHER_LOAD_DEMO"));
    srand((unsigned)time(nullptr));     // the screensaver's colour sequence differs from start to start

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    // On the Pi every window fills the display. Creating it at the desktop size without
    // SDL_WINDOW_FULLSCREEN avoids a surface rebuild on the first frame, which would drop that
    // frame and show as a black flash after a game.
    SDL_DisplayMode desktop;
    if (SDL_GetDesktopDisplayMode(0, &desktop) != 0) {
        desktop.w = 1920;
        desktop.h = 1080;
    }
    SDL_Window* window = SDL_CreateWindow("Arcade launcher", 0, 0, desktop.w, desktop.h, SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        fprintf(stderr, "OpenGL: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (!gfx::init(w, h) || !text::init())
        return 2;
    gfx::set_ground_shadow(SHOT_X, SHOT_Y + 34, SHOT_W, SHOT_H, SHOT_ROUND, 140, 0.55f);
    double warm_from = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    gfx::warm_up();
    double warm_ms = (SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() - warm_from) * 1000;
    images::init(HOME + "/cache/pictures", dev ? PICTURE_LOG : "");

    regular = text::load(HOME + "/fonts/Poppins-Regular.ttf");
    bold = text::load(HOME + "/fonts/Poppins-SemiBold.ttf");
    mono = text::load(HOME + "/fonts/JetBrainsMono-Regular.ttf");
    mono_medium = text::load(HOME + "/fonts/JetBrainsMono-Medium.ttf");
    if (!mono_medium)
        mono_medium = mono;

    dev_note("shaders ready after %.0f ms", warm_ms);
    load_library();
    apply_mine();
    game_sel.assign(consoles.size(), 0);
    build_pager();

    // On a restart after a game, a tool or a rescan, restore the previous position. After
    // power-on, start on the first console.
    bool cold = !exists(BOOTED_FILE);
    bool returned = !cold && restore_state();
    remove(STATE_FILE);
    {
        std::ifstream in(NOTE_FILE);
        if (std::getline(in, note))
            note_until = 8;
        in.close();
        remove(NOTE_FILE);
    }
    pager_at = pager_to = pager_goal();
    console_stage.shown = console_sel;
    if (screen != S_CONSOLES) {
        mix = mix_to = 1;
        if (screen == S_SETTINGS)
            want_facts(true);
    }
    if (returned && screen == S_GAMES) {
        // The display shows the game list as the menu left it: loading.c put that frame back
        // into /dev/fb0 while the game ran. Load what the list shows before the first frame, so
        // that the two match. Requesting every picture first lets them load in parallel.
        const Console& c = consoles[console_sel];
        const Game& g = c.games[game_sel[console_sel]];
        game_stage.shown = game_sel[console_sel];
        game_stage.alpha = 1;
        std::vector<Picture*> needed = { console_logo_small(c) };
        if (is_grid(c)) {
            int first = grid_top * GRID_COLUMNS;
            for (int i = first; i < first + GRID_ROWS * GRID_COLUMNS && i < (int)c.games.size(); i++)
                needed.push_back(grid_box(c.games[i]));
        } else {
            needed.insert(needed.end(), { snap_picture(g), wheel_picture(g), box_picture(g) });
        }
        for (Picture* picture : needed)
            images::wait(picture, 1500);
    }
    // A warm start that fails to restore its position opens with the large logo, so wait for
    // it. After a game or power-on the menu continues from the frame on screen.
    Picture* logos[] = { corner_logo(), returned || cold ? nullptr : large_logo() };
    for (Picture* picture : logos)
        images::wait(picture, 1500);

    double freq = (double)SDL_GetPerformanceFrequency();
    double loaded_after = 0, start_took = 0;
    if (cold && !consoles.empty()) {
        // boot.c is still animating. Load everything the first screen needs now so that the
        // screenshots can wipe in as soon as the menu owns the display.
        const Console& first = consoles[console_sel];
        std::vector<Picture*> needed = { console_logo(first), console_photo(first) };
        BoxRow row = box_row(first);
        needed.insert(needed.end(), row.box, row.box + row.count);
        for (int i = 0; i < STRIPS; i++)
            needed.push_back(strip_picture(strip_of(first, i)));
        for (Picture* picture : needed)
            images::wait(picture, 1500);
        // Render the first screen once without presenting it. The GPU prepares its textures
        // and glyphs now instead of during the first animation, where that work costs frames.
        Stage stage = console_stage;
        console_stage.alpha = 1;
        for (int i = 0; i < STRIPS; i++)
            strips[i].shown = strip_of(first, i);
        render(1);
        glFinish();
        console_stage = stage;
        for (Strip& strip : strips)
            strip = Strip();
        loaded_after = (SDL_GetPerformanceCounter() - began) / freq;
        start_took = take_start_picture();
    }
    bool from_start = start_picture.id != 0;             // continuing from the boot frame

    Uint64 start = SDL_GetPerformanceCounter();
    double last = 0, intro_until = from_start ? 0.1 : cold ? 0.9 : 0;
    int idle_checks = 0, shown_frames = 0;
    float saver = 0, intro = returned ? 0.0f : 1.0f;     // intro 1 hides the overlays
    if (!returned) {
        // After power-on each strip starts with its slice of the boot frame, which the first
        // console's screenshots then wipe over. Without a boot frame the menu opens with the
        // logo on black. After the logo fades, the screenshots wipe in over black strips.
        for (Strip& strip : strips) {
            strip.shown.start = from_start;
            strip.shown.black = !from_start;
        }
        strips_wait = from_start ? 0.12 : intro_until;      // the first frames render slower than later ones
    }
    bool running = true, terminal_emptied = false, sound_started = false, sound_noted = false;
    double still_since = 0;                                 // last time the screen moved
    // Development aid: frame times during animation, with the CPU and GPU time of each frame.
    double log_at = 5, last_shown = 0, total_ms = 0, slowest_ms = 0, work_ms = 0, most_work_ms = 0;
    int frames = 0, late = 0;
    bool in_a_row = false;

    while (running) {
        now_s = (SDL_GetPerformanceCounter() - start) / freq;
        float dt = (float)(now_s - last);
        last = now_s;
        if (dt > 0.1f) dt = 0.1f;

        animating = false;
        SDL_Event e;
        while (SDL_PollEvent(&e))
            handle_event(e, now_s, running);
        if (dev)
            read_dev_keys(now_s);
        repeat_directions(now_s);

        bool saving = now_s - last_input > SAVER_AFTER;
        if (!actions.empty() && saver > 0) {
            actions.clear();                    // the first press only wakes the screen
            last_input = now_s;
            saving = false;
        }
        while (!actions.empty()) {
            Action action = actions.front();
            actions.pop_front();
            if (leaving.empty() && now_s >= intro_until && mix == mix_to)
                act(action);
            animating = true;
        }

        update(dt);
        if (images::pump(animating) > 0)
            animating = true;
        prepare_letters(animating ? 2 : 12);
        bool measuring = dev && exists(DEV_ALWAYS);
        if (measuring) {
            animating = true;
            last_input = now_s;         // keep the screensaver off while measuring
        }
        saver = slide(saver, saving ? 1.0f : 0.0f, dt, 0.6f);
        if (saver > 0)
            saver_pictures();           // load them during the fade so the screensaver starts complete
        // The overlays wait until every strip has replaced its slice of the boot frame.
        bool start_shows = false, start_under = false;
        for (const Strip& strip : strips) {
            start_shows = start_shows || strip.shown.start;
            start_under = start_under || (strip.under.start && strip.down < 1);
        }
        intro = slide(intro, now_s >= intro_until && !start_shows ? 0.0f : 1.0f, dt, 0.5f);
        // Every strip shows a screenshot now. Freeing the boot frame texture costs the GPU about
        // 10 ms, so wait for an idle frame.
        if (start_picture.id && !start_shows && !start_under && !animating)
            gfx::release(start_picture);
        // The sounds start after a second without movement or input. Opening the audio device
        // takes about 250 ms on the Pi, and a moving screen dropped frames meanwhile.
        if (animating || intro > 0)
            still_since = now_s;
        if (!sound_started && leaving.empty() && now_s - still_since > 1 && now_s - last_input > 1) {
            sound_started = true;
            sound::start(HOME + "/art/sounds");
            dev_note("sound: thread started at %.2f s", now_s);
        }
        if (sound_started && !sound_noted && sound::ready_after() >= 0) {
            sound_noted = true;
            dev_note("sound: ready %d ms after the start of its thread", sound::ready_after());
        }
        if (screen == S_SETTINGS && take_facts())
            animating = true;

        if (!leaving.empty()) {
            bool game = leaving.compare(0, 4, "game") == 0;
            bool tool = leaving.compare(0, 4, "tool") == 0;
            bool wait = tool || leaving == "rescan";
            if (game && !launching) {
                // Switch to the loading screen at once. Its screenshot has normally loaded ahead.
                launching = true;
                load = GAME_LOAD;
                note_until = 0;
                images::wait(snap_picture(launched_game()), 150);
                images::wait(loading_backdrop(launched_game()), 150);
                images::wait(loading_word(), 150);
                render();
                SDL_GL_SwapWindow(window);
                continue;
            }
            if (wait && !waiting) {
                // A RetroPie tool takes a few seconds to open, and so does a new look for games.
                // The wait screen shows LOADING and the moving bar meanwhile.
                waiting = true;
                load = WAIT_LOAD;
                loading_since = now_s;
                note_until = 0;
                images::wait(loading_word(), 150);
                render();
                SDL_GL_SwapWindow(window);
                continue;
            }
            // With LAUNCHER_LOAD_DEMO set (desktop build), hold the launch frame for as long as a
            // game launch takes on the Pi.
            if ((game || wait) && now_s < loading_since + load_demo) {
                render();
                if (dev && exists(SHOT_REQUEST))
                    write_screenshot();
                SDL_GL_SwapWindow(window);
                continue;
            }
            save_state();
            if (game || wait) {
                // Leave the final frame in /dev/fb0. It stays on screen until the game, the
                // tool or the restarted menu draws.
                if (game) {
                    // The frame for the end of the game: the game list as it was. loading.c
                    // copies it into /dev/fb0 once the emulator has the display, and the menu
                    // starts from it again.
                    launching = false;
                    render();
                    std::vector<uint8_t> list_frame = frame_for_console();
                    if (!list_frame.empty())
                        write_file(FRAME_FILE, list_frame);
                    launching = true;
                }
                render();
                std::vector<uint8_t> frame = frame_for_console();
                if (!frame.empty()) {
                    write_file("/dev/fb0", frame);
                    // pi/arcade.sh puts the wait screen back when a tool ends, until the menu draws.
                    if (tool)
                        write_file(FRAME_FILE, frame);
                    hand_over_load_bar(wait);
                }
                if (game)
                    dev_note("game: frame in /dev/fb0 %.0f ms after the press",
                             ((SDL_GetPerformanceCounter() - start) / freq - loading_since) * 1000);
            } else if (leaving == "restart" || leaving == "shutdown") {
                gfx::begin();
                draw_logo_screen(1, true);
                gfx::flush();
                copy_frame_to_console();
            } else {
                gfx::begin();                   // F4: a black screen for the terminal
                gfx::flush();
            }
            if (dev && exists(SHOT_REQUEST))
                write_screenshot();
            SDL_GL_SwapWindow(window);
            write_action(leaving);
            break;
        }

        if (saver >= 1) {
            render_saver(dt);
        } else if (animating || intro > 0 || saver > 0 || idle_checks <= 0) {
            if (from_start) {
                // Continue from the boot frame on the strips, then fade in the overlays.
                render(std::max(0.0f, 1 - 2 * intro));
            } else if (intro >= 1) {
                gfx::begin();
                draw_logo_screen(1, true);
            } else {
                // Fade out the large logo first, then fade in the overlays.
                render(std::max(0.0f, 1 - 2 * intro));
                draw_logo_screen(std::max(0.0f, 2 * intro - 1), false);
            }
            if (saver > 0)
                gfx::rect(0, 0, 1920, 1080, { 0, 0, 0, saver });
            gfx::flush();
            idle_checks = 30;
        } else {
            // Idle: keep the current frame and poll again in 12 ms.
            SDL_Delay(12);
            idle_checks--;
            in_a_row = false;
            if (dev && exists(SHOT_REQUEST)) {
                render();
                write_screenshot();
            }
            continue;
        }

        if (dev && exists(SHOT_REQUEST))
            write_screenshot();
        double worked = 0;
        if (measuring) {
            // Measure the time from the start of this iteration until the GPU has finished the frame.
            glFinish();
            worked = ((SDL_GetPerformanceCounter() - start) / freq - now_s) * 1000;
        }
        SDL_GL_SwapWindow(window);
        shown_frames++;
        // The menu's first frame is on screen. A launcher-load still moving the bar of a tool's
        // or a new look's wait screen exits once its state file is gone.
        if (shown_frames == 1)
            remove(LOADING_FILE);
        if (shown_frames == 1 && cold) {
            // The first frame is on screen. A still running boot.c exits when BOOTED_FILE
            // appears. The file also records the handoff timings.
            FILE* f = fopen(BOOTED_FILE, "w");
            if (f) {
                fprintf(f, "the menu had loaded %.2f s after its start, the start program needed %.2f s more for its picture, %s\n",
                        loaded_after, start_took, from_start ? "and the menu went on from that picture" : "and the menu began with its logo");
                fclose(f);
            }
        }
        if (!terminal_emptied && shown_frames >= 12 && now_s > 2.5 && !animating) {
            // The menu covers the terminal now, so clear it. Boot messages on the terminal could
            // otherwise reappear on a later redraw. Clearing stalls the Pi for about 40 ms, so
            // wait for an idle frame.
            terminal_emptied = true;
            FILE* terminal = fopen("/dev/tty", "w");
            if (terminal) {
                fputs("\033[2J\033[H\033[?25l", terminal);
                fclose(terminal);
            }
        }
        if (dev) {
            // At 60 Hz a frame lasts 16.7 ms. A frame interval over 20 ms counts as late because
            // it missed a vertical blank.
            double shown = (SDL_GetPerformanceCounter() - start) / freq;
            if (in_a_row) {
                double took = (shown - last_shown) * 1000;
                frames++;
                total_ms += took;
                work_ms += worked;
                if (took > slowest_ms) slowest_ms = took;
                if (worked > most_work_ms) most_work_ms = worked;
                if (took > 20) late++;
            }
            last_shown = shown;
            in_a_row = true;
            if (shown >= log_at && frames > 0) {
                FILE* f = fopen(DEV_LOG, "a");
                if (f) {
                    fprintf(f, "%d frames: average %.1f ms, slowest %.1f ms, %d over 20 ms. Work per frame: average %.1f ms, most %.1f ms\n",
                            frames, total_ms / frames, slowest_ms, late, work_ms / frames, most_work_ms);
                    fclose(f);
                }
                frames = late = 0;
                total_ms = slowest_ms = work_ms = most_work_ms = 0;
                log_at = shown + 5;
            }
        }
    }

    double closing = (SDL_GetPerformanceCounter() - start) / freq;
    auto since_closing = [&]() { return ((SDL_GetPerformanceCounter() - start) / freq - closing) * 1000; };
    images::shutdown();
    dev_note("closing: picture threads stopped after %.0f ms", since_closing());
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    dev_note("closing: window closed after %.0f ms", since_closing());
    sound::stop();
    dev_note("closing: sound closed after %.0f ms", since_closing());
    facts_stop = true;
    if (facts_thread.joinable())
        facts_thread.join();
    SDL_Quit();
    dev_note("closing: SDL closed after %.0f ms", since_closing());
    return 0;
}
