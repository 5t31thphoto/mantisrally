// World regression test (CI): universe + journey + terrain only.
// A kinematic driver follows the road for 40 miles, takes forks, and checks the road, the forks, the biome
// journey, the dealership, recovery and free-roam road crossings.
#include <cstdio>
#include <cmath>
#include "universe.h"
#include "journey.h"
#include "terrain.h"
using namespace mantis;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static float wrapA(float a) { while (a > 3.14159f) a -= 6.28318f; while (a < -3.14159f) a += 6.28318f; return a; }

int main() {
  const uint32_t seed = 0xA11CEu;
  universeInit(seed); journeyInit(seed); terrainInit(seed);
  // ---- the road is continuous and sane ----
  CHECK(roadCount() > 200, "road too short: %d", roadCount());
  for (int i = 1; i < roadCount(); i++) {
    const RoadNode &a = roadNode(i - 1), &b = roadNode(i);
    float d = hypotf(b.x - a.x, b.z - a.z);
    CHECK(fabsf(d - ROAD_DS) < 0.3f && std::isfinite(b.y), "node spacing %d: %.2f", i, d);
    if (fabsf(d - ROAD_DS) >= 0.3f) break;
  }
  // ---- drive it ----
  RoadHit h; roadNearest(0, 0, h);
  float x = 0, z = 0, yaw = h.yaw, dist = 0;
  int forks = 0, arms = 0, biomes = 0, arrivals = 0, stores = 0; uint16_t seen = 0;
  bool storeSeen = false; float maxGrade = 0.f; float lastY = 0.f; bool first = true; int jumpSites = 0; float siteX[64], siteZ[64];
  const float dt = 0.1f, v = 30.f;
  for (int step = 0; step < (int)(40.f * MILE / (v * dt)); step++) {
    RoadHit r;
    CHECK(roadNearest(x, z, r, 300.f), "lost the road at %.1f mi", dist / MILE);
    if (!roadNearest(x, z, r, 300.f)) break;
    // prefer the arm at every other fork
    const Junction* j = roadJunction();
    bool wantArm = j->active && !j->decided && (forks & 1);
    int dir = cosf(yaw - r.yaw) >= 0 ? 1 : -1;
    float tx, tz;
    if (wantArm && roadArmCount() > 20 && hypotf(j->x - x, j->z - z) < 60.f) {
      int best = 0; float bd = 1e9f;
      for (int k = 0; k < roadArmCount(); k++) { float d = hypotf(roadArmNode(k).x - x, roadArmNode(k).z - z); if (d < bd) { bd = d; best = k; } }
      int k = best + 4 < roadArmCount() ? best + 4 : roadArmCount() - 1;
      tx = roadArmNode(k).x; tz = roadArmNode(k).z;
    } else {
      int n = r.arm ? roadArmCount() : roadCount();
      int k = r.idx + dir * 4; k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
      const RoadNode& p = r.arm ? roadArmNode(k) : roadNode(k);
      tx = p.x; tz = p.z;
    }
    yaw += wrapA(atan2f(tz - z, tx - x) - yaw) * 0.6f;
    float px = x, pz = z;
    x += cosf(yaw) * v * dt; z += sinf(yaw) * v * dt;
    float moved = hypotf(x - px, z - pz);
    dist += moved;
    terrainUpdate(x, z, yaw, v, dt);
    journeyUpdate(moved, yaw, true);
    journeySetCar(x, z, yaw, moved);
    float y = terrainHeight(x, z);
    CHECK(std::isfinite(y), "height NaN at %.1f mi", dist / MILE);
    if (!first) { float g = fabsf(y - lastY) / fmaxf(moved, 0.1f); if (g > maxGrade && fabsf(r.across) < r.half) maxGrade = g;
      if (g > 0.6f && fabsf(r.across) < r.half) {           // count places, not passes (the test driver can loop)
        bool known = false;
        for (int q = 0; q < jumpSites && q < 64; q++) if (hypotf(siteX[q] - x, siteZ[q] - z) < 80.f) known = true;
        if (!known && jumpSites < 64) { siteX[jumpSites] = x; siteZ[jumpSites] = z; jumpSites++; }
      } }
    lastY = y; first = false;
    bool tookArm;
    if (roadJunctionEvent(tookArm)) { forks++; if (tookArm) arms++; }
    if (journeyBiomeEvent()) biomes++;
    if (journeyArrivedEvent()) arrivals++;
    seen |= (uint16_t)(1u << journeyCurrent());
    if ((step % 600) == 0) roadRequestStore();
    if (roadStore()->active && !storeSeen) { stores++; storeSeen = true; }
    if (!roadStore()->active) storeSeen = false;
  }
  printf("drove %.1f mi: forks %d (arms taken %d), biome changes %d, arrivals %d, stores %d, steps in the road surface %d\n",
         dist / MILE, forks, arms, biomes, arrivals, stores, jumpSites);
  CHECK(forks >= 4, "too few forks in 40 miles: %d", forks);
  CHECK(arms >= 1, "never took a fork's arm");
  CHECK(biomes >= 12 && biomes <= 30, "biome changes %d (about one every 2 miles expected)", biomes);
  CHECK(stores >= 2, "the dealership never appeared");
  // known: where switchbacks meet a fork the road can still overlap itself at a different height (rare)
  CHECK(jumpSites <= 6, "the road surface steps in %d places", jumpSites);
  // ---- recovery puts you on the road ----
  float rx = x + 25.f, rz = z + 25.f, ry = yaw;
  CHECK(roadRecover(rx, rz, ry, 1.f), "recover failed");
  RoadHit rr; roadNearest(rx, rz, rr);
  CHECK(fabsf(rr.across) <= 1.2f, "recover left us %.1f m off", rr.across);
  // ---- free roam: far from any road, a new one crosses ahead ----
  float fx = x, fz = z, fy = yaw + 1.57f;
  for (int s = 0; s < 400; s++) { fx += cosf(fy) * 5.f; fz += sinf(fy) * 5.f; terrainUpdate(fx, fz, fy, 30.f, 0.1f); }
  CHECK(roadDistanceTo(fx, fz) < 900.f, "no road crossing near a free roamer (%.0f m)", roadDistanceTo(fx, fz));
  // ---- the caches are filled and finite ----
  const Grid& g = terrainNear();
  int bad = 0; for (int k = 0; k < g.N * g.N; k++) if (std::abs(g.c[k].h) > 30000) bad++;
  CHECK(bad == 0, "%d near cells out of range", bad);
  printf(fails ? "FAILED (%d)\n" : "all world checks passed\n", fails);
  return fails ? 1 : 0;
}
