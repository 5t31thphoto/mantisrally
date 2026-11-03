#pragma once
#include <stdint.h>

// Biomes and the journey between them.
//
// The world is coherent, not persistent (like MantisPlanet): it is dreamt ahead and forgotten behind. Biomes sit
// on a small "climate map" (east = wetter / coastal, north = colder / higher). A road carries a destination biome;
// staying on it steps you biome by biome across the climate map toward that destination (seeded, with rules).
// Off the road, your compass heading is the direction you move across the climate map. A biome is ~2 miles.

namespace mantis {

enum BiomeId : uint8_t {
    B_PINE = 0, B_REDWOOD, B_LAKES, B_MOOR, B_DEVILS, B_ALPINE,
    B_CANYON, B_COAST, B_MEADOW, B_AUTUMN, B_BAYOU, B_TUNDRA,
    B_COUNT
};

// Road surfaces (also what the tyres feel)
enum : uint8_t { SURF_TARMAC = 0, SURF_GRAVEL, SURF_DIRT, SURF_MUD, SURF_SNOW, SURF_SAND, SURF_GRASS, SURF_ROCK, SURF_N };
// Ground cover (what a terrain cell is)
enum : uint8_t { MAT_GRASS = 0, MAT_SOIL, MAT_ROCK, MAT_SNOW, MAT_SAND, MAT_MUD, MAT_WATER, MAT_ROAD, MAT_SHOULDER, MAT_LOT };

constexpr float MILE = 1609.344f;
constexpr float BIOME_LEN = 2.0f * MILE;

struct Biome {
    const char* name;
    const char* sign;          // road-sign text (7 chars max)
    float east, north;         // climate-map position (-1..1)
    // road
    uint8_t surface;
    float roadHalf;            // metres
    float straight, sweep, tight, hairpin;   // corner mix (relative weights)
    float grade;               // max road grade (0.12 = 12 %)
    float crest;               // how often the road throws a crest / dip (0..1)
    float vRadius;             // vertical curve radius over crests (m): smaller = easier to get air
    uint8_t paved;             // 1 centre/edge paint
    // land
    float base;                // base elevation (m)
    float hills, ridge, cliff, water, rough;
    uint8_t ground;            // MAT_* off the road
    float snowLine;            // metres above base where high ground turns white (900 = never, -900 = everywhere)
    // things that grow / lie about
    float pine, broad, redwood, bush, cactus, snag, boulder, flowers;
    // weather tendencies
    float cloud, rain, fog, snow, storm, dust;
    float fireflies, aurora, beams, leaves;
    // colours (RGB)
    uint8_t grassD[3], grassL[3], soilD[3], soilL[3], rockD[3], rockL[3], leafD[3], leafL[3], accent[3], waterC[3], mtn[3];
    float peaks;               // panorama mountain height (0 = flat horizon)
};

const Biome& biome(uint8_t id);

// Blended scalars for the land being dreamt right now
struct Land {
    float hills, ridge, cliff, water, rough, snowLine;
    float pine, broad, redwood, bush, cactus, snag, boulder, flowers;
    float roadHalf, straight, sweep, tight, hairpin, grade, crest, vRadius;
    uint8_t surface, ground, paved;
};

void    journeyInit(uint32_t seed);
// Distance travelled this frame, compass yaw, and whether you are following a road.
void    journeyUpdate(float dist, float yaw, bool onRoad);
uint8_t journeyCurrent();
float   journeyMetersToTransition();   // driving before the next biome transition begins (-1: one is underway)
uint8_t journeyNext();          // the biome you are blending into
float   journeyBlend();         // 0 = all current .. 1 = all next
uint8_t journeyDestination();   // the special biome the current road leads to
float   journeyMilesToDest();
float   journeyMilesTo(uint8_t dest);           // estimate from here along a road to dest
void    journeyFollowRoad(uint8_t dest);        // took a fork (or joined a road) leading to dest
uint8_t journeyPickDestination(uint32_t salt, uint8_t avoid);
uint16_t journeyVisited();      // bit per biome
void    journeySetVisited(uint16_t mask);
bool    journeyArrivedEvent();  // true once when the destination is reached
bool    journeyArrivedPending();          // the arrival hasn't been announced yet
bool    journeyBiomeEvent();    // true once when a new biome takes over
void    journeyForce(uint8_t cur, uint8_t next, float blend);   // previews / tests
const Land& journeyLand();

// The base-elevation plane: coherent in every direction, tilting so the land climbs or falls toward each
// biome's base as you travel (the Pikes Peak climb, the drop to the coast).
float journeyBase(float x, float z);
void  journeySetCar(float x, float z, float yaw, float dist);

}
