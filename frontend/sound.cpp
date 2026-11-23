#include "sound.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace sound {
namespace {

const char* const NAMES[KINDS] = { "move", "console", "open", "back", "play", "secret" };
const int RATE = 48000, CHANNELS = 2;

struct Voice {
    const std::vector<int16_t>* samples = nullptr;
    size_t at = 0;
};

std::vector<int16_t> clips[KINDS];      // interleaved stereo at RATE
Voice voices[4];
SDL_AudioDeviceID device = 0;
std::thread opener;
std::atomic<int> opened_ms(-1);         // see ready_after()

void mix(void*, Uint8* stream, int bytes) {
    int16_t* out = (int16_t*)stream;
    int count = bytes / (int)sizeof(int16_t);
    for (int i = 0; i < count; i++) {
        int sum = 0;
        for (Voice& v : voices) {
            if (v.samples && v.at < v.samples->size())
                sum += (*v.samples)[v.at++];
        }
        out[i] = (int16_t)std::max(-32768, std::min(32767, sum));
    }
    for (Voice& v : voices)
        if (v.samples && v.at >= v.samples->size())
            v.samples = nullptr;
}

// Reads a WAV file and converts it to the device's format.
std::vector<int16_t> load(const std::string& path) {
    std::vector<int16_t> clip;
    SDL_AudioSpec spec;
    Uint8* data = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &data, &length))
        return clip;
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, CHANNELS, RATE) >= 0) {
        std::vector<Uint8> buffer((size_t)length * std::max(1, cvt.len_mult));
        memcpy(buffer.data(), data, length);
        cvt.buf = buffer.data();
        cvt.len = (int)length;
        if (!cvt.needed || SDL_ConvertAudio(&cvt) == 0) {
            size_t bytes = cvt.needed ? (size_t)cvt.len_cvt : length;
            clip.assign((int16_t*)buffer.data(), (int16_t*)(buffer.data() + bytes));
        }
    }
    SDL_FreeWAV(data);
    return clip;
}

void open(const std::string& folder) {
    Uint32 began = SDL_GetTicks();
    // Without the sound files the device stays closed.
    SDL_RWops* first = SDL_RWFromFile((folder + "/" + NAMES[0] + ".wav").c_str(), "rb");
    if (first)
        SDL_RWclose(first);
    if (first && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = RATE;
        want.format = AUDIO_S16SYS;
        want.channels = CHANNELS;
        want.samples = 2048;            // about 43 ms; with 512 the Pi's ALSA reports underruns
        want.callback = mix;
        SDL_AudioDeviceID opened = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (opened) {
            for (int k = 0; k < KINDS; k++)
                clips[k] = load(folder + "/" + NAMES[k] + ".wav");
            SDL_PauseAudioDevice(opened, 0);
            device = opened;
        }
    }
    opened_ms = (int)(SDL_GetTicks() - began);
}

}  // namespace

void start(const std::string& folder) {
    if (!opener.joinable())
        opener = std::thread(open, folder);
}

int ready_after() {
    return opened_ms;
}

void play(Kind kind) {
    if (opened_ms < 0 || !device || clips[kind].empty())
        return;
    SDL_LockAudioDevice(device);
    // A free voice, or else the one that has played longest.
    Voice* voice = &voices[0];
    for (Voice& v : voices) {
        if (!v.samples) {
            voice = &v;
            break;
        }
        if (v.at > voice->at)
            voice = &v;
    }
    voice->samples = &clips[kind];
    voice->at = 0;
    SDL_UnlockAudioDevice(device);
}

void stop() {
    if (opener.joinable())
        opener.join();
    if (device) {
        SDL_CloseAudioDevice(device);
        device = 0;
    }
}

}  // namespace sound
