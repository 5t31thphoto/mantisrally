#pragma once
#include <stdint.h>

// 17 biomes. The road is a graph of branches; the path you drive chooses the next biome.
// Signs at junctions name the destination you will eventually reach down that fork.

namespace mantis {

enum BiomeId : uint8_t {
    BM_FOREST_GRAVEL = 0,
    BM_MOUNTAIN_DIRT,
    BM_COAST_TARMAC,
    BM_ALPINE_SNOW,
    BM_DESERT_TRACK,
    BM_HIGHLAND_HEATH,
    BM_MUD_BOWL,
    BM_NIGHT_CITY,
    BM_ICE_LAKE,
    BM_JUNGLE_PATH,
    BM_VOLCANO_ASH,
    BM_CANYON_SWITCH,
    BM_MEADOW_LANES,
    BM_STORM_MOOR,
    BM_REDWOOD_DARK,
    BM_SALT_FLAT,
    BM_AUTUMN_PASS,
    BM_COUNT = 17
};

struct BiomeInfo {
    const char* name;
    const char* shortName;   // for road signs
    uint8_t surface;         // 0 tarmac 1 gravel 2 dirt 3 snow 4 mud
    float fog, dust, rain, snowChance;
    float treeDensity, rockDensity;
    float gripBias;          // multiplies base surface grip
    uint8_t sky;
};

const BiomeInfo& journeyBiome(uint8_t id);
uint8_t journeyCurrent();
uint8_t journeyNext();              // biome you're heading toward on current road
float   journeyBlend();             // 0 = fully current, 1 = fully next

void journeyInit(uint32_t seed);
void journeyUpdate(float x, float z, float speed, float dt);

// Called by terrain when the car commits to a branch
void journeyCommitBranch(uint8_t destBiome);
void journeyForceBiome(uint8_t cur, uint8_t next, float blend);  // host / previews

// Soft forces
float journeyHeadingBias();
float journeyWeatherMix();

}
