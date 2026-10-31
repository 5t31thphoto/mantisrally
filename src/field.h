#pragma once
#include <stdint.h>
#include "vehicle.h"

// The near field: deer, birds, particles (dust, mud, spray, snow), and everything solid you can hit.

namespace mantis {

enum : uint8_t { PK_DUST = 0, PK_MUD, PK_SPRAY, PK_SNOW, PK_GRAVEL, PK_SMOKE };
struct Particle { float x, y, z, vx, vy, vz, life, age, size; uint8_t kind; };
constexpr int PART_N = 72;

struct Critter { bool on; float x, z, y, yaw, speed, t, fear; uint8_t kind, pose; };
constexpr int CRIT_N = 6;

struct Bird { bool on; float az, el, vaz, t; int n; };

void fieldInit(uint32_t seed);
void fieldUpdate(const VehicleState& car, uint8_t vehicle, float dt);
// solid things near the car: trees, rocks, the dealership. Returns the hardest impact (m/s).
float fieldCollide(VehicleState& car, uint8_t vehicle);
void fieldEmit(uint8_t kind, float x, float y, float z, float vx, float vy, float vz, float size, float life);
void fieldHorn(float x, float z);

const Particle* fieldParticles();
const Critter* fieldCritters();
const Bird* fieldBirds();

}
