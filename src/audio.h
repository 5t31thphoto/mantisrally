#pragma once
#include <stdint.h>

// Live 24 kHz synthesis on core 0 -> the Core2 speaker. The engine, the tyres and the weather are synthesised
// (no samples); the nature bed is MantisPlanet's. Bass lives in the vibration motor (haptics).

namespace mantis {

constexpr int AUDIO_SR = 24000;

// The car as the audio hears it (written by the main loop every frame; floats are atomic on the ESP32)
struct CarAudio {
    uint8_t engine;            // ENG_*
    float rpm, rpm01, throttle, boost, spin, slide, speed, water;
    uint8_t surf;
    bool air, limiter, reverse, cockpit;
};
extern CarAudio g_car;

enum : uint8_t { BUMP_TREE, BUMP_ROCK, BUMP_WATER };
enum : uint8_t { CRIT_DEER, CRIT_RABBIT, CRIT_BIRDS, CRIT_BUTTERFLY, CRIT_DUCK, CRIT_BEAVER, CRIT_SQUIRREL, CRIT_SPLASH, CRIT_HERON };
enum : uint8_t { UI_TICK, UI_OK, UI_SOLD, UI_ARRIVE, UI_FORK, UI_WARN, UI_MUTE };

void audioSpeakerBegin();   // right after M5.begin(), before other allocations
void audioBegin();

// game -> audio (lock-free, never blocks the game)
void sfxBump(float strength);       // suspension slam / pothole
void sfxLand(float strength);       // touching down after air
void sfxShift(int dir);             // +1 up, -1 down
void sfxHorn();
void sfxThunder(float delaySec, float power);
void sfxSplash(float strength);
void sfxCrash(float strength, bool tree);
void sfxCritter(uint8_t kind, float dist);
void sfxUi(uint8_t kind);

bool audioRunning();
void audioSetMuted(bool muted);
bool audioMuted();
void audioRender(int16_t* out, int n);

}
