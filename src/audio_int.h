#pragma once
#include "dsp.h"
#include "scene.h"
#include <stdint.h>

namespace mantis {
namespace snd {

constexpr int BLOCK = 512;          // ~21 ms: enough slack that a busy frame can't starve the speaker
struct Bus { float dry[BLOCK], rev[BLOCK], dly[BLOCK]; };

extern dsp::Rng rng;

// Every sound has a tag, so the desktop tools can solo one (tools/host/stems.sh). On the device soloGain() is a
// constant 1 and costs nothing.
enum : uint8_t {
    // road / weather                   car                                        stage life
    TAG_SEA, TAG_WASH, TAG_RAIN,       TAG_ENGINE, TAG_BUBBLES, TAG_CREAK,         TAG_SHRIMP, TAG_WHALE, TAG_DOLPHIN,
    TAG_THUNDER, TAG_VENT, TAG_ERUPT,  TAG_BUMP, TAG_SONAR, TAG_SCAN, TAG_UI,     TAG_NARWHAL, TAG_SEAL, TAG_SERPENT,
    TAG_CHIME, TAG_EERIE, TAG_INTRO,   TAG_TRIM, TAG_BOLT,
    // the score
    TAG_M_DRONE, TAG_M_PAD, TAG_M_STAB, TAG_M_SHAKER, TAG_M_KICK, TAG_M_BASS, TAG_M_PLUCK, TAG_M_BELL, TAG_M_CHOIR,
    TAG_N
};
#ifdef HOST
extern int g_solo;                                            // -1: everything
inline float soloGain(int tag) { return (g_solo < 0 || g_solo == tag) ? 1.f : 0.f; }
#else
inline constexpr float soloGain(int) { return 1.f; }
#endif
extern uint32_t samples;

// road / engine bed (ocean.cpp keeps the symbol names the audio task expects)
void oceanInit();
void oceanBlock(Bus& b, int n, const Scene& s);
void oceanBump(float strength);
void oceanThunder(float delaySec, float power);
void oceanCritter(uint8_t kind, float dist);
void oceanUi(uint8_t kind);
void oceanSonar(float delay, float strength);
void oceanMoment(uint8_t kind);

// music.cpp
void musicInit();
void musicBlock(Bus& b, int n, const Scene& s);
void musicMoment(uint8_t kind);
float musicKick();
float musicGroove();
float musicDelaySamples();

}
}
