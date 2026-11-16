#pragma once
#include <stdint.h>
#include "vehicle.h"

// The near field: deer, birds, particles (dust, mud, spray, snow), and everything solid you can hit.

namespace mantis {

enum : uint8_t { PK_DUST = 0, PK_MUD, PK_SPRAY, PK_SNOW, PK_GRAVEL, PK_SMOKE, PK_FUR, PK_MIST };
struct Particle { float x, y, z, vx, vy, vz, life, age, size; uint8_t kind; };
constexpr int PART_N = 72;

struct Critter { bool on; float x, z, y, yaw, speed, t, fear; uint8_t kind, pose; bool dodger; };
constexpr int CRIT_N = 6;

struct Bird { bool on; float az, el, vaz, t; int n; };

void fieldInit(uint32_t seed);
void fieldUpdate(const VehicleState& car, uint8_t vehicle, float dt);
// solid things near the car: trees, rocks, the dealership, deer. Reports the hardest impact (m/s) and what it was.
enum : uint8_t { HIT_NONE = 0, HIT_TREE, HIT_ROCK, HIT_STORE, HIT_DEER };
struct FieldHit { float strength; uint8_t kind; };
FieldHit fieldCollide(VehicleState& car, uint8_t vehicle);
void fieldEmit(uint8_t kind, float x, float y, float z, float vx, float vy, float vz, float size, float life);
void fieldHorn(float x, float z);

// Red Canyon's arches over the road and geyser country's geysers, near the car (worked out in fieldUpdate).
struct Arch { float x, z, y, ax, az, ri, ro; };   // centre on the road, (ax, az) across it, inner / outer half-span
struct Geyser { float x, z, y, h; };              // h: the column's height now (0: quiet, steaming)
const Arch* fieldArches(int& n);
// Pastures (terrainPlot use 3): the post-and-rail fence round each and its herd. A fence is solid below 30 mph and
// bursts above it; a cow is solid below 45 mph and, above it, goes the way of a deer. Both stay gone.
constexpr int FENCE_POSTS = 12;                   // spans a side
bool fieldFencePost(int pi, int pj, int side, int k, float& x, float& z);   // post k (0..12) of a side, if it stands
bool fieldFenceBroken(int pi, int pj, int side, int k);                   // span k (post k to k+1) burst
struct Cow { float x, z, heading; uint8_t pose; };   // pose 0 head up, 1 grazing
int fieldCows(float cx, float cz, float R, float t, Cow* out, int max);
const Geyser* fieldGeysers(int& n);

const Particle* fieldParticles();
const Critter* fieldCritters();
const Bird* fieldBirds();

}
