#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "universe.h"
#include "journey.h"
#include "terrain.h"

using namespace mantis;

int main() {
    const uint32_t seed = 0x52414C4Cu;
    universeInit(seed);
    journeyInit(seed);
    terrainInit(seed);

    TrackSample center = terrainAt(0.f, 0.f);
    assert(fabsf(center.across) < 0.05f);
    assert(center.roadHalf > 5.f && center.grip > 0.1f);

    // An off-road car must be able to return to the road edge without losing
    // its along-road position or requiring a physics-engine teleport helper.
    float x = 0.f, z = 18.f;
    TrackSample before = terrainAt(x, z);
    assert(before.across > before.roadHalf + 5.f);
    terrainRecoverRoad(x, z);
    TrackSample after = terrainAt(x, z);
    assert(fabsf(after.across - 4.4f) < 0.15f);

    // The road is genuinely curved, not an infinite straight line.
    float dx0, dz0, k0, dx1, dz1, k1;
    terrainDirection(0.f, 0.f, dx0, dz0, k0);
    terrainDirection(80.f, 0.f, dx1, dz1, k1);
    float headingDelta = fabsf(atan2f(dx0 * dz1 - dz0 * dx1, dx0 * dx1 + dz0 * dz1));
    assert(headingDelta > 0.01f);

    // Sampling the ground and road grip must remain finite even well off-road.
    TrackSample rough = terrainAt(0.f, 35.f);
    assert(isfinite(rough.height) && isfinite(rough.grip));
    assert(rough.grip > 0.f && rough.grip < center.grip);

    // Drive the ribbon until a procedural fork appears, then take its left arm.
    journeyInit(seed + 1u);
    terrainInit(seed + 1u);
    float carX = 0.f, carZ = 0.f, carYaw = 0.f;
    const float dt = 0.05f, speed = 20.f;
    bool foundFork = false;
    for (int i = 0; i < 700; ++i) {
        float dirX, dirZ, curvature;
        terrainDirection(carX, carZ, dirX, dirZ, curvature);
        carYaw = atan2f(dirZ, dirX);
        carX += cosf(carYaw) * speed * dt;
        carZ += sinf(carYaw) * speed * dt;
        terrainUpdateRoad(carX, carZ, carYaw, speed, dt);
        const Junction* j = terrainJunction();
        if (j && j->active) { foundFork = true; break; }
    }
    assert(foundFork);
    const Junction fork = *terrainJunction();
    // Cross the left side of the junction while keeping the nose straight;
    // side-of-fork detection should still select the left route.
    carX = fork.x + sinf(fork.yaw) * 5.f;
    carZ = fork.z - cosf(fork.yaw) * 5.f;
    carYaw = fork.yaw;
    terrainUpdateRoad(carX, carZ, carYaw, 0.f, dt);
    bool tookLeft = false;
    assert(terrainBranchCommitted(tookLeft));
    assert(tookLeft);
    assert(journeyNext() == fork.destL);
    assert(terrainJunction()->active); // fork stays rendered while passing it

    printf("PASS: road center, off-road recovery, curved road, rough-ground grip, and persistent fork selection\n");
    return 0;
}
