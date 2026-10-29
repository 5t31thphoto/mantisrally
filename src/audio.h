#pragma once
#include <stdint.h>

// Live 24 kHz synthesis on core 0 → Core2 speaker. Bass lives in the motor (haptics).
// Same bus / event / stems architecture as the Seadiver systems, retargeted to engines,
// gravel, wind, rain and a stage score.

namespace mantis {

constexpr int   AUDIO_SR = 24000;
constexpr float MUSIC_BPM = 118.f;

enum : uint8_t { MOMENT_INTRO, MOMENT_STAGE, MOMENT_CREST, MOMENT_JUMP, MOMENT_FINISH, MOMENT_STORM, MOMENT_NIGHT };
enum : uint8_t { CRIT_RIVAL, CRIT_CROWD, CRIT_HELI, CRIT_BOLT, CRIT_ANIMAL, CRIT_RADIO, CRIT_EERIE };
enum : uint8_t { UI_SCAN_TICK, UI_SCAN_OK, UI_SCAN_NEW, UI_LIGHTS_ON, UI_LIGHTS_OFF, UI_CARD, UI_LEVEL, UI_CRUISE, UI_CREAK, UI_DETENT };

void audioSpeakerBegin();   // right after M5.begin(), before other allocations
void audioBegin();

// game -> audio (lock-free, safe from the main loop)
void sfxBump(float strength);
void sfxThunder(float delaySec, float power);
void sfxCritter(uint8_t kind, float dist);
void sfxMoment(uint8_t kind);
void sfxUi(uint8_t kind);
void sfxHorn(float strength);          // short blast

// audio -> game
float audioBeats();
float audioKick();
float audioGroove();

bool audioRunning();
void audioSetMuted(bool muted);
bool audioMuted();
void audioRender(int16_t* out, int n);

}
