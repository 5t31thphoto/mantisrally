#pragma once
#include <stdint.h>

// Scrolling heightfield (Seadiver pattern): dreamt ahead of the car, forgotten behind.
// Road is cut into the land with banks; shape has ridges, cliffs, canyons, humps.

namespace mantis {

constexpr int   TG_N = 96;               // cells per side
constexpr int   TG_MASK = 127;           // for safety when indexing — use % TG_N carefully
constexpr float TG_CELL = 2.5f;          // metres per cell → ~240 m window

struct TrackSample {
    float height;       // road/ground surface Y
    float camber;       // rad, bank angle (positive = right side up)
    float grip;
    float roughness;
    float bankL, bankR;
    uint8_t surface;
    float across;       // signed metres from road centre
    float roadHalf;
    float slope;        // dz along road (positive = climbing)
    float curv;         // centreline curvature
};

struct Junction {
    bool  active;
    float x, z, yaw;
    uint8_t destL, destR;
    float armL, armR, dist;
};

struct RoadSign {
    bool  active;
    float x, z, yaw;
    uint8_t destBiome;
    bool  isLeft;
};

void terrainInit(uint32_t seed);
void terrainSetBiome(uint8_t cur, uint8_t next, float blend);
void terrainRefresh(); // rebuild cached heights after an explicit biome reset

// Call each frame: scrolls the height grid, updates junctions
void terrainUpdateRoad(float carX, float carZ, float carYaw, float speed, float dt);

TrackSample terrainAt(float x, float z);
// Move an off-road position back to the nearest road edge while preserving its along-road progress.
void terrainRecoverRoad(float& x, float& z);
float terrainHeight(float x, float z);
void  terrainDirection(float x, float z, float& dirX, float& dirZ, float& curvature);

// Shape function (height in metres) — also used outside the grid
float terrainShape(float x, float z, float* acrossOut = nullptr);

const Junction* terrainJunction();
const RoadSign* terrainSigns(int& count);
bool terrainBranchCommitted(bool& tookLeft);

}
