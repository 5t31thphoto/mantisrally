#pragma once
#include <stdint.h>
#include "journey.h"

// The land and the road.
//
// Road: a deque of nodes (3 m apart) dreamt at BOTH ends to ~450 m from the car and forgotten ~700 m behind it.
// Junctions grow a second polyline (the arm); taking it swaps the arm in as the road. Every 3-7 miles.
// Land: a natural height function (biome blended) with the road cut in. Two scrolling caches hold it baked for the
// renderer and the physics: near (2.5 m cells, ~240 m) and far (8 m cells, ~512 m). Each cell knows its height,
// signed distance to the road, distance along it, its material and its normal.

namespace mantis {

constexpr float ROAD_DS = 3.0f;          // metres between road nodes

struct RoadNode {
    float x, z, y, yaw, s;               // position, road height, heading, arc length
    uint8_t surf, half20, flags, side;   // surface, half-width (1/20 m), flags, store side
};
enum : uint8_t { RN_CREST = 1, RN_JUNCTION = 2, RN_STORE = 4, RN_SIGN = 8 };

struct Junction {
    bool active;
    float x, z, yaw;                     // at the split, yaw of the main road (outward)
    int dir;                             // +1 at the front end, -1 at the back end
    int armSide;                         // +1 the arm leaves to the right, -1 to the left
    uint8_t destMain, destArm;
    float milesMain, milesArm;
    float s;                             // arc length of the split on the main road
    bool decided;
};

struct RoadSign { bool active; float x, z, yaw; int8_t side; };   // the junction's sign
struct Store    { bool active; float x, z, yaw; float s; };      // MANTIS MOTORS (a lot beside the road)

// Baked cell
struct Cell {
    int16_t h;        // height, decimetres
    int16_t across;   // signed distance to the road centre (right +), 1/50 m; saturates at +-600 m
    uint8_t along;    // distance along the road, 1/20 m, mod 12.8 m (paint dashes)
    uint8_t mat;      // low nibble MAT_*, high nibble SURF_*
    uint8_t nrm;      // normal: high nibble x, low nibble z, (v - 8) / 8
    uint8_t extra;    // road / shoulder: half-width 1/20 m.  water: depth 1/20 m.  ground: vegetation hint
};

struct Grid {
    Cell* c; int N; float cell, inv;
    int ox, oz;                     // window origin (cell coords)
    int jobAxis, jobLine, jobPos;   // the row / column being dreamt
    int rebake;                     // >=0: sweeping every cell again (a road appeared)
    float maxH;                     // highest cell seen (ray early-out)
};

void terrainInit(uint32_t seed);
// Each frame: extends / drops road, handles junction decisions, scrolls the caches (budgeted).
void terrainUpdate(float carX, float carZ, float carYaw, float speed, float dt);
void terrainRebuild();                       // re-dream everything now (start, teleports, previews)

// --- land ---
float terrainNatural(float x, float z);      // height before the road is cut in
float terrainWaterLevel(float x, float z);   // lake / sea surface here
struct Ground {
    float h, nx, ny, nz;       // surface height and normal
    float across, along, half; // road frame
    uint8_t mat, surf;         // MAT_*, SURF_* (tyre surface)
    float water;               // water depth (m) when mat == MAT_WATER
    bool cached;               // came from a cache (else computed)
};
void terrainGround(float x, float z, Ground& g);
float terrainHeight(float x, float z);

// --- road ---
int  roadCount();
const RoadNode& roadNode(int i);             // 0 = back end .. count-1 = front end
int  roadArmCount();
const RoadNode& roadArmNode(int i);
struct RoadHit { float across, along, y, yaw, half, dist; uint8_t surf; bool arm; int idx; };
bool roadNearest(float x, float z, RoadHit& h, float maxDist = 1e9f);
// Put a point back on the nearest road (edge or centre), facing along it nearest to `yaw`.
bool roadRecover(float& x, float& z, float& yaw, float edge);
const Junction* roadJunction();
const RoadSign* roadSign();
const Store* roadStore();
void roadRequestStore();                     // the game wants a store soon (affordable vehicle etc.)
bool roadJunctionEvent(bool& tookArm);       // true once when a fork is decided
float roadDistanceTo(float x, float z);      // metres to the nearest road (big if none)
// Spawn a fresh road crossing ahead (free roaming far from any road)
void roadSpawnCrossing(float carX, float carZ, float carYaw);

// Pace note: the next notable corner ahead of a car at arc length s travelling dir (+1/-1)
struct PaceNote { int dir; int sev; float dist; bool crest; bool valid; };
PaceNote roadPaceNote(float carX, float carZ, float carYaw, float speed);

// --- caches (renderer) ---
const Grid& terrainNear();
const Grid& terrainFar();
inline int gridWrap(int i, int N) { int m = i % N; return m < 0 ? m + N : m; }

// --- vegetation: deterministic placement on a 4.5 m jittered lattice ---
enum : uint8_t { VEG_NONE = 0, VEG_PINE, VEG_BROAD, VEG_REDWOOD, VEG_BUSH, VEG_CACTUS, VEG_SNAG, VEG_BOULDER, VEG_FLOWERS, VEG_N };
constexpr float VEG_CELL = 4.5f;
struct Veg { uint8_t type, var; float x, z, y, size, radius; };
bool vegAt(int ci, int cj, Veg& v);          // false if nothing grows there (or outside the near cache)

}
