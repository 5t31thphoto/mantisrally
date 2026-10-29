#pragma GCC optimize("Os")
#include "journey.h"
#include "universe.h"
#include "terrain.h"
#include <math.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static const BiomeInfo BIOMES[17] = {
    { "Forest Gravel",   "FOREST",   1, 0.25f, 0.15f, 0.35f, 0.00f, 0.90f, 0.25f, 0.95f, 0 },
    { "Mountain Dirt",   "MTN DIRT", 2, 0.15f, 0.40f, 0.10f, 0.05f, 0.20f, 0.75f, 0.90f, 1 },
    { "Coast Tarmac",    "COAST",    0, 0.10f, 0.05f, 0.20f, 0.00f, 0.10f, 0.15f, 1.05f, 2 },
    { "Alpine Snow",     "ALPINE",   3, 0.35f, 0.00f, 0.00f, 0.70f, 0.15f, 0.40f, 0.75f, 3 },
    { "Desert Track",    "DESERT",   1, 0.05f, 0.60f, 0.00f, 0.00f, 0.05f, 0.55f, 0.88f, 4 },
    { "Highland Heath",  "HEATH",    2, 0.40f, 0.10f, 0.45f, 0.10f, 0.35f, 0.25f, 0.92f, 5 },
    { "Mud Bowl",        "MUD",      4, 0.20f, 0.20f, 0.65f, 0.00f, 0.25f, 0.20f, 0.70f, 6 },
    { "Night City",      "CITY",     0, 0.15f, 0.10f, 0.15f, 0.00f, 0.00f, 0.10f, 1.08f, 7 },
    { "Ice Lake",        "ICE",      3, 0.30f, 0.00f, 0.00f, 0.45f, 0.05f, 0.20f, 0.65f, 8 },
    { "Jungle Path",     "JUNGLE",   2, 0.45f, 0.05f, 0.55f, 0.00f, 0.95f, 0.35f, 0.85f, 9 },
    { "Volcano Ash",     "VOLCANO",  1, 0.25f, 0.65f, 0.05f, 0.00f, 0.00f, 0.55f, 0.80f, 10 },
    { "Canyon Switch",   "CANYON",   2, 0.10f, 0.30f, 0.05f, 0.00f, 0.05f, 0.80f, 0.90f, 11 },
    { "Meadow Lanes",    "MEADOW",   0, 0.08f, 0.05f, 0.10f, 0.00f, 0.30f, 0.10f, 1.02f, 12 },
    { "Storm Moor",      "MOOR",     2, 0.55f, 0.15f, 0.70f, 0.05f, 0.20f, 0.30f, 0.82f, 13 },
    { "Redwood Dark",    "REDWOOD",  1, 0.35f, 0.05f, 0.25f, 0.00f, 0.98f, 0.20f, 0.93f, 14 },
    { "Salt Flat",       "SALT",     0, 0.02f, 0.25f, 0.00f, 0.00f, 0.00f, 0.05f, 1.00f, 15 },
    { "Autumn Pass",     "AUTUMN",   1, 0.20f, 0.10f, 0.20f, 0.00f, 0.70f, 0.30f, 0.94f, 16 },
};

static uint8_t s_cur = 0, s_next = 1;
static float   s_blend = 0.f;
static float   s_headingBias = 0.f;
static float   s_weather = 0.3f;
static uint32_t s_seed = 1;
static float   s_distOnPath = 0.f;
static float   s_pathLen = 400.f;   // metres until next biome fully takes over

const BiomeInfo& journeyBiome(uint8_t id) { return BIOMES[id % BM_COUNT]; }
uint8_t journeyCurrent() { return s_cur; }
uint8_t journeyNext() { return s_next; }
float journeyBlend() { return s_blend; }

void journeyInit(uint32_t seed) {
    s_seed = seed ? seed : 1;
    // Always random start
    s_cur = (uint8_t)(hash32(s_seed) % BM_COUNT);
    s_next = (uint8_t)(hash32(s_seed + 7) % BM_COUNT);
    if (s_next == s_cur) s_next = (s_cur + 1) % BM_COUNT;
    s_blend = 0.f;
    s_headingBias = 0.f;
    s_weather = 0.25f;
    s_distOnPath = 0.f;
    s_pathLen = 320.f + hashf(s_seed + 3) * 280.f;
    terrainSetBiome(s_cur, s_next, 0.f);
    terrainRefresh();
}

void journeyCommitBranch(uint8_t destBiome) {
    destBiome %= BM_COUNT;
    // Committing means: current path's "next" becomes the destination of the fork you took
    s_next = destBiome;
    s_distOnPath = 0.f;
    s_pathLen = 280.f + hashf(worldSeed() + destBiome * 13 + (int)s_distOnPath) * 320.f;
    s_blend = 0.f;
}

void journeyUpdate(float x, float z, float speed, float dt) {
    s_distOnPath += speed * dt;
    s_blend = clampf(s_distOnPath / fmaxf(s_pathLen, 80.f), 0.f, 1.f);

    if (s_blend >= 1.f) {
        // Arrived: next becomes current, pick a fresh random next for the continuing road
        s_cur = s_next;
        uint8_t n = (uint8_t)(hash32(worldSeed() + (uint32_t)(x * 3) + (uint32_t)(z * 5) + s_cur * 17) % BM_COUNT);
        if (n == s_cur) n = (s_cur + 3) % BM_COUNT;
        s_next = n;
        s_distOnPath = 0.f;
        s_pathLen = 300.f + hashf(worldSeed() + s_cur * 31) * 300.f;
        s_blend = 0.f;
    }

    terrainSetBiome(s_cur, s_next, s_blend);

    float n = fbm2(x * 0.002f, z * 0.002f, 2, s_seed + 41);
    s_headingBias = (n - 0.5f) * 0.4f;

    const BiomeInfo& a = journeyBiome(s_cur);
    const BiomeInfo& b = journeyBiome(s_next);
    float w = a.rain * (1.f - s_blend) + b.rain * s_blend;
    w += (a.snowChance * (1.f - s_blend) + b.snowChance * s_blend) * 0.5f;
    s_weather += ((w + 0.12f * sinf(s_distOnPath * 0.005f)) - s_weather) * (1.f - expf(-dt * 0.1f));
}

void journeyForceBiome(uint8_t cur, uint8_t next, float blend) {
    s_cur = cur % BM_COUNT;
    s_next = next % BM_COUNT;
    s_blend = blend < 0.f ? 0.f : (blend > 1.f ? 1.f : blend);
    s_distOnPath = s_blend * s_pathLen;
    terrainSetBiome(s_cur, s_next, s_blend);
    terrainRefresh();
}

float journeyHeadingBias() { return s_headingBias; }
float journeyWeatherMix()  { return s_weather; }

}
