#pragma once

#include <string>

// Short sounds for the menu's moves and presses. SDL's audio thread mixes them, up to four at
// a time, on the default audio device (the one RetroPie's Sound tool chooses).
namespace sound {

enum Kind { MOVE, CONSOLE, OPEN, BACK, PLAY, SECRET, KINDS };

// Opens the audio device and loads folder/<name>.wav for each kind (move, console, open, back,
// play, secret) on a helper thread, because opening the device takes about 250 ms on the Pi.
// Sounds asked for before that stay silent, as do kinds without a device or a file.
void start(const std::string& folder);

// Milliseconds the helper thread took, or -1 while it runs.
int ready_after();

void play(Kind kind);

// Waits for the helper thread and closes the device. Call before SDL_Quit.
void stop();

}
