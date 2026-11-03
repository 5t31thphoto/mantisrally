#pragma once
#include "dsp.h"
#include <stdint.h>

namespace mantis {
namespace snd {

constexpr int BLOCK = 512;
struct Bus { float dry[BLOCK], rev[BLOCK], dly[BLOCK]; };
extern dsp::Rng rng;

// What MantisPlanet's nature bed listens to (filled from the rally scene by audio.cpp)
struct NatScene {
    float t, tod, yaw, speed, stillFor, cloud, rain, rainAgo, storm, wet;
    float forestNear, lakeNear, oceanNear, streamNear, fallsNear, open;
    float gust, hush, murmur, frogBoost, snowy, tundra, tropical, desert, japan, crows, cuckoo, giant;
};

void natureInit();
void natureBlock(Bus& b, int n, const NatScene& s);
void natureBump(uint8_t kind);
void natureThunder(float delaySec, float power, float close);
void natureCritter(uint8_t kind, float dist);
void natureImpact(uint8_t kind, float strength);
float* natureNearBuffer();                 // this block's close sounds (impacts, shutter): mixed dry

void engineInit();
void engineBlock(Bus& b, int n);
void engineEvent(uint8_t kind, float a);
enum : uint8_t { EE_BUMP, EE_LAND, EE_SHIFT, EE_HORN, EE_SPLASH, EE_CRASH, EE_UI };

}
}
