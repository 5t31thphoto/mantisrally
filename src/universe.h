#pragma once
#include <stdint.h>

// Hashes, noise and the clock the whole stage shares.
// Two kinds of noise: periodic in direction (for the far panorama, which closes into a 360 degree ring, as in
// MantisPlanet) and plain 2D over the ground (for the road and the banks you can leave).

namespace mantis {

constexpr float kTau = 6.28318531f;
constexpr float PI_F = 3.14159265f;

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline float hashf(uint32_t x) { return (hash32(x) >> 8) * (1.0f / 16777216.0f); }
inline float wrapPi(float a) { return a - kTau * __builtin_floorf((a + PI_F) / kTau); }

// Map a world angle (radians, unbounded) onto a lattice that wraps every `period` cells.
inline float angU(float angle, int period) { return angle * ((float)period / kTau); }

// 2D value noise, periodic in u with the given period. v is unbounded (distance / time).
float vnoiseP(float u, float v, int period, uint32_t salt);
float fbmP(float u, float v, int period, int octaves, uint32_t salt);
// 2D value noise over the ground (not periodic). 0..1.
float vnoise2(float x, float y, uint32_t salt);
float fbm2(float x, float y, int octaves, uint32_t salt);

void universeInit(uint32_t seed);
uint32_t worldSeed();

// The day turns with time (about ten minutes standing still) and a little faster as you travel.
float dayClock(float timeSec, float distance);
inline float timeOfDayAt(float timeSec, float distance) { float c = dayClock(timeSec, distance); return c - (float)(int)c; }
void setTimeOfDay(float tod, float timeSec, float distance);    // previews / tests

// The compass. Bearings grow to your right (clockwise from above), east = 0: south = +pi/2, west = pi, north = -pi/2.
constexpr float AZ_EAST = 0.f, AZ_SOUTH = 1.5707963f, AZ_WEST = 3.14159265f, AZ_NORTH = -1.5707963f;
// The sun's true height above the horizon (radians) along its arc: east at dawn, high through the south, west at dusk.
float sunPathElevation(float tod);

}
