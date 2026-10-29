#pragma GCC optimize("Os")
#include "terrain.h"
#include "universe.h"
#include "journey.h"
#include "mem.h"
#include <math.h>
#include <string.h>

namespace mantis {

static uint32_t s_seed = 1;
static uint8_t  s_cur = 0, s_next = 1;
static float    s_blend = 0.f;

// height grid (metres * 10 stored as int16)
static int16_t* s_h = nullptr;
static int s_ox = 0, s_oz = 0;
static float s_px = 0.f, s_pz = 0.f, s_D = 0.f;
static int s_jobAxis = -1, s_jobLine = 0, s_jobPos = TG_N;

static Junction s_junc;
static RoadSign s_signs[6];
static int s_signN = 0;
static float s_along = 0.f;
static float s_nextJuncAt = 160.f;
static bool s_committed = false, s_tookLeft = false;
static float s_branchMix = 0.f, s_branchYaw = 0.f;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smoothstep(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// ---- road centreline in world XZ (snaking + branch bias) ----
static void roadFrame(float x, float z, float& along, float& across, float& yaw) {
    // low-freq heading field
    float n = fbm2(x * 0.003f, z * 0.003f, 4, s_seed + 3);
    yaw = (n - 0.5f) * 2.6f + s_branchYaw * s_branchMix;
    // tighter curves for interest
    float n2 = fbm2(x * 0.009f, z * 0.009f, 2, s_seed + 5);
    yaw += (n2 - 0.5f) * 0.9f;
    float cx = cosf(yaw), cz = sinf(yaw);
    along = x * cx + z * cz;
    // lateral: project onto perpendicular + a soft offset so the ribbon wanders
    float off = (fbm2(x * 0.004f, z * 0.004f, 2, s_seed + 7) - 0.5f) * 10.f;
    across = -x * cz + z * cx + off;
}

// Biome height character
static void biomeAmps(uint8_t id, float& hills, float& ridges, float& cliffs, float& canyon, float& rough) {
    // defaults
    hills = 10.f; ridges = 3.5f; cliffs = 0.f; canyon = 0.f; rough = 0.4f;
    switch (id) {
        case 0:  hills = 9.f; ridges = 3.f; rough = 0.5f; break; // forest
        case 1:  hills = 22.f; ridges = 8.f; cliffs = 14.f; rough = 0.7f; break; // mountain
        case 2:  hills = 2.f; ridges = 0.5f; rough = 0.2f; break; // coast
        case 3:  hills = 24.f; ridges = 7.f; cliffs = 10.f; rough = 0.3f; break; // alpine
        case 4:  hills = 3.f; ridges = 1.f; canyon = 4.f; rough = 0.4f; break; // desert
        case 5:  hills = 7.f; ridges = 2.f; rough = 0.5f; break; // heath
        case 6:  hills = 4.f; ridges = 1.f; rough = 0.8f; break; // mud
        case 7:  hills = 1.f; ridges = 0.2f; rough = 0.15f; break; // city
        case 8:  hills = 2.f; ridges = 0.5f; rough = 0.2f; break; // ice lake
        case 9:  hills = 8.f; ridges = 2.f; rough = 0.6f; break; // jungle
        case 10: hills = 20.f; ridges = 10.f; cliffs = 16.f; rough = 0.7f; break; // volcano
        case 11: hills = 16.f; ridges = 5.f; canyon = 18.f; cliffs = 9.f; rough = 0.6f; break; // canyon
        case 12: hills = 3.f; ridges = 0.8f; rough = 0.25f; break; // meadow
        case 13: hills = 10.f; ridges = 3.5f; rough = 0.55f; break; // moor
        case 14: hills = 9.f; ridges = 2.5f; rough = 0.45f; break; // redwood
        case 15: hills = 1.f; ridges = 0.1f; rough = 0.1f; break; // salt
        case 16: hills = 11.f; ridges = 3.f; rough = 0.5f; break; // autumn
        default: break;
    }
}

float terrainShape(float x, float z, float* acrossOut) {
    float along, across, yaw;
    roadFrame(x, z, along, across, yaw);
    if (acrossOut) *acrossOut = across;

    float hillsA, ridgesA, cliffsA, canyonA, roughA;
    float hillsB, ridgesB, cliffsB, canyonB, roughB;
    biomeAmps(s_cur, hillsA, ridgesA, cliffsA, canyonA, roughA);
    biomeAmps(s_next, hillsB, ridgesB, cliffsB, canyonB, roughB);
    float hills = hillsA * (1.f - s_blend) + hillsB * s_blend;
    float ridges = ridgesA * (1.f - s_blend) + ridgesB * s_blend;
    float cliffs = cliffsA * (1.f - s_blend) + cliffsB * s_blend;
    float canyon = canyonA * (1.f - s_blend) + canyonB * s_blend;
    float rough = roughA * (1.f - s_blend) + roughB * s_blend;

    // base rolling hills
    float zH = fbm2(x * 0.006f, z * 0.006f, 5, s_seed) * hills;
    zH += (fbm2(x * 0.02f, z * 0.02f, 3, s_seed + 11) - 0.5f) * hills * 0.35f;

    // ridges (sharp crests) — Seadiver style
    if (ridges > 0.05f) {
        float r = fbm2(x * 0.008f, z * 0.008f, 3, s_seed + 0x41);
        r = 1.f - fabsf(2.f * r - 1.f);
        float rg = r * r;
        zH += (rg - 0.35f) * 1.4f * ridges;
    }

    // cliffs / steps
    if (cliffs > 0.05f) {
        float c = fbm2(x * 0.004f, z * 0.004f, 2, s_seed + 0xC1);
        float st = smoothstep(0.42f, 0.5f, c) + smoothstep(0.58f, 0.64f, c);
        zH += (st - 1.f) * cliffs;
    }

    // canyon cut
    if (canyon > 0.05f) {
        float n = fbm2(x * 0.0035f, z * 0.0035f, 3, s_seed + 0xCA);
        float w = fabsf(n - 0.5f);
        float cut = 1.f - smoothstep(0.02f, 0.09f, w);
        float walls = 1.f - smoothstep(0.08f, 0.18f, w);
        zH += canyon * (0.35f + 0.15f * walls - cut);
    }

    // fine roughness
    zH += (fbm2(x * 0.08f, z * 0.08f, 2, s_seed + 0x70) - 0.5f) * rough;

    // ---- road cut: flatten a corridor, raise banks ----
    float half = 5.5f;
    float absA = fabsf(across);
    if (absA < half + 6.f) {
        // target road height: smooth sample of hills along the centre
        float roadH = fbm2(x * 0.006f, z * 0.006f, 4, s_seed) * hills * 0.95f;
        // bank profile: ditch then berm
        float bank = 0.f;
        if (absA > half) {
            float t = (absA - half) / 6.f; // 0 at edge .. 1 at 6m out
            // rise up then fall (classic rally bank / spectator berm)
            bank = sinf(t * 3.14159f) * (2.0f + 1.4f * rough) * (1.f + ridges * 0.12f);
            // outer drop into wilderness
            if (t > 0.7f) bank -= (t - 0.7f) * 3.f;
        }
        float blend = 1.f - smoothstep(half - 0.5f, half + 4.0f, absA);
        blend *= 0.82f; // leave more natural undulation on the asphalt
        // camber: slight crown on road
        float crown = (1.f - (absA / half) * (absA / half)) * 0.15f;
        if (absA > half) crown = 0.f;
        float flat = roadH + crown + bank;
        zH = zH * (1.f - blend) + flat * blend;
    }

    return zH;
}

static void dreamCell(int ix, int iz) {
    float x = ix * TG_CELL, z = iz * TG_CELL;
    float h = terrainShape(x, z, nullptr);
    int idx = (iz % TG_N + TG_N) % TG_N * TG_N + (ix % TG_N + TG_N) % TG_N;
    float d = h * 10.f;
    if (d > 32000.f) d = 32000.f;
    if (d < -32000.f) d = -32000.f;
    s_h[idx] = (int16_t)lroundf(d);
}

static float sampleGrid(float x, float z) {
    if (!s_h) return terrainShape(x, z, nullptr);
    float gx = x / TG_CELL, gz = z / TG_CELL;
    int ix = (int)floorf(gx), iz = (int)floorf(gz);
    // outside window → shape function
    if (ix < s_ox + 1 || iz < s_oz + 1 || ix + 2 >= s_ox + TG_N || iz + 2 >= s_oz + TG_N)
        return terrainShape(x, z, nullptr);
    float fx = gx - ix, fz = gz - iz;
    auto at = [&](int i, int j) -> float {
        int idx = (j % TG_N + TG_N) % TG_N * TG_N + (i % TG_N + TG_N) % TG_N;
        return s_h[idx] * 0.1f;
    };
    float a = at(ix, iz), b = at(ix + 1, iz), c = at(ix, iz + 1), d = at(ix + 1, iz + 1);
    float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
    return ab + (cd - ab) * fz;
}

void terrainInit(uint32_t seed) {
    s_seed = seed ? seed : 1;
    s_cur = 0; s_next = 1; s_blend = 0.f;
    if (!s_h) s_h = (int16_t*)memFast(TG_N * TG_N * sizeof(int16_t));
    s_ox = -TG_N / 2; s_oz = -TG_N / 2;
    s_px = 0.f; s_pz = 0.f; s_D = 0.f;
    for (int j = 0; j < TG_N; j++)
        for (int i = 0; i < TG_N; i++) dreamCell(s_ox + i, s_oz + j);
    s_jobPos = TG_N;
    memset(&s_junc, 0, sizeof(s_junc));
    s_signN = 0; s_along = 0.f;
    s_nextJuncAt = 140.f + hashf(s_seed) * 100.f;
    s_committed = false; s_branchMix = 0.f; s_branchYaw = 0.f;
}

void terrainSetBiome(uint8_t cur, uint8_t next, float blend) {
    s_cur = cur % 17; s_next = next % 17; s_blend = clampf(blend, 0.f, 1.f);
}

static const uint8_t SURF[17] = {
    1,2,0,3,1,2,4,0,3,2,1,2,0,2,1,0,1
};

TrackSample terrainAt(float x, float z) {
    TrackSample t;
    float along, across, yaw;
    roadFrame(x, z, along, across, yaw);
    t.across = across;
    t.roadHalf = 5.4f;
    t.height = sampleGrid(x, z);

    // slope along road direction
    float d = 1.5f;
    float h1 = sampleGrid(x + cosf(yaw) * d, z + sinf(yaw) * d);
    t.slope = (h1 - t.height) / d;

    // camber from bank height difference left/right
    float hL = sampleGrid(x - sinf(yaw) * 3.f, z + cosf(yaw) * 3.f);
    float hR = sampleGrid(x + sinf(yaw) * 3.f, z - cosf(yaw) * 3.f);
    t.camber = atanf((hR - hL) / 6.f);

    float curvN = fbm2(x * 0.01f, z * 0.01f, 2, s_seed + 19);
    t.curv = (curvN - 0.5f) * 0.08f;

    uint8_t sa = SURF[s_cur], sb = SURF[s_next];
    t.surface = (s_blend < 0.5f) ? sa : sb;
    static const float GRIP0[5] = { 0.96f, 0.74f, 0.60f, 0.50f, 0.42f };
    float ga = GRIP0[sa] * journeyBiome(s_cur).gripBias;
    float gb = GRIP0[sb] * journeyBiome(s_next).gripBias;
    float g = ga * (1.f - s_blend) + gb * s_blend;
    float edge = clampf((t.roadHalf + 2.f - fabsf(across)) / 3.f, 0.25f, 1.f);
    // slope reduces grip a bit when climbing steep
    g *= 1.f - clampf(fabsf(t.slope) * 0.15f, 0.f, 0.25f);
    t.grip = g * edge;
    t.roughness = (t.surface == 1 || t.surface == 2)
        ? 0.3f + 0.45f * fbm2(x * 0.06f, z * 0.06f, 2, s_seed + 7)
        : (t.surface == 3 ? 0.12f : (t.surface == 4 ? 0.55f : 0.06f));
    t.bankL = t.bankR = 7.f + 3.f * hashf(s_seed + (int)along);
    return t;
}

float terrainHeight(float x, float z) { return sampleGrid(x, z); }

void terrainDirection(float x, float z, float& dirX, float& dirZ, float& curvature) {
    float along, across, yaw;
    roadFrame(x, z, along, across, yaw);
    dirX = cosf(yaw); dirZ = sinf(yaw);
    curvature = (fbm2(x * 0.01f, z * 0.01f, 2, s_seed + 19) - 0.5f) * 0.08f;
    (void)along; (void)across;
}

// ---- junctions (same as before) ----
static uint8_t randomBiome(uint32_t salt) {
    uint8_t b = (uint8_t)(hash32(s_seed + salt) % 17);
    if (b == s_cur || b == s_next) b = (b + 5) % 17;
    return b;
}

static void spawnJunction(float carX, float carZ, float carYaw) {
    float ax = cosf(carYaw), az = sinf(carYaw);
    float dist = 50.f + hashf(s_seed + (int)s_along) * 30.f;
    s_junc.active = true;
    s_junc.x = carX + ax * dist;
    s_junc.z = carZ + az * dist;
    s_junc.yaw = carYaw;
    s_junc.destL = randomBiome((uint32_t)(s_along * 3) + 11);
    s_junc.destR = randomBiome((uint32_t)(s_along * 5) + 29);
    if (s_junc.destR == s_junc.destL) s_junc.destR = (s_junc.destL + 4) % 17;
    s_junc.armL = -0.6f - hashf(s_seed + 41) * 0.3f;
    s_junc.armR =  0.6f + hashf(s_seed + 43) * 0.3f;
    s_junc.dist = dist;
    s_committed = false; s_branchMix = 0.f;
    s_signN = 2;
    float sx = cosf(carYaw + 1.57f), sz = sinf(carYaw + 1.57f);
    s_signs[0] = {true, s_junc.x + sx * 5.f - ax * 8.f, s_junc.z + sz * 5.f - az * 8.f, carYaw, s_junc.destL, true};
    s_signs[1] = {true, s_junc.x - sx * 5.f - ax * 8.f, s_junc.z - sz * 5.f - az * 8.f, carYaw, s_junc.destR, false};
}

void terrainUpdateRoad(float carX, float carZ, float carYaw, float speed, float dt) {
    s_along += speed * dt;
    s_px = carX; s_pz = carZ; s_D = s_along;

    // scroll height grid (Seadiver budgeted rows/cols)
    int wx = (int)floorf(carX / TG_CELL) - TG_N / 2;
    int wz = (int)floorf(carZ / TG_CELL) - TG_N / 2;
    int budget = 48;
    while (budget > 0) {
        if (s_jobPos >= TG_N) {
            if (s_ox < wx) { s_ox++; s_jobAxis = 0; s_jobLine = s_ox + TG_N - 1; }
            else if (s_ox > wx) { s_ox--; s_jobAxis = 0; s_jobLine = s_ox; }
            else if (s_oz < wz) { s_oz++; s_jobAxis = 1; s_jobLine = s_oz + TG_N - 1; }
            else if (s_oz > wz) { s_oz--; s_jobAxis = 1; s_jobLine = s_oz; }
            else break;
            s_jobPos = 0;
        }
        int n = TG_N - s_jobPos < budget ? TG_N - s_jobPos : budget;
        for (int k = 0; k < n; k++, s_jobPos++) {
            if (s_jobAxis == 0) dreamCell(s_jobLine, s_oz + s_jobPos);
            else dreamCell(s_ox + s_jobPos, s_jobLine);
        }
        budget -= n;
    }

    if (!s_junc.active && s_along > s_nextJuncAt) {
        spawnJunction(carX, carZ, carYaw);
        s_nextJuncAt = s_along + 200.f + hashf(s_seed + (int)s_along) * 180.f;
    }
    if (!s_junc.active) return;
    float dx = s_junc.x - carX, dz = s_junc.z - carZ;
    float d = sqrtf(dx * dx + dz * dz);
    if (d > 90.f) {
        float fwd = dx * cosf(carYaw) + dz * sinf(carYaw);
        if (fwd < -5.f) { s_junc.active = false; s_signN = 0; }
    }
    if (!s_committed && d < 18.f) {
        float side = -dx * sinf(s_junc.yaw) + dz * cosf(s_junc.yaw);
        float headingDiff = carYaw - s_junc.yaw;
        while (headingDiff > 3.14f) headingDiff -= 6.28f;
        while (headingDiff < -3.14f) headingDiff += 6.28f;
        bool left = (side < -1.2f) || (headingDiff < -0.18f);
        bool right = (side > 1.2f) || (headingDiff > 0.18f);
        if (left || right) {
            s_committed = true; s_tookLeft = left && !right;
            uint8_t dest = s_tookLeft ? s_junc.destL : s_junc.destR;
            s_branchYaw = s_tookLeft ? s_junc.armL : s_junc.armR;
            s_branchMix = s_tookLeft ? -1.f : 1.f;
            journeyCommitBranch(dest);
            s_junc.active = false; s_signN = 0;
        }
    }
    if (fabsf(s_branchMix) > 0.01f) {
        s_branchMix *= (1.f - 0.12f * dt);
        if (fabsf(s_branchMix) < 0.05f) { s_branchMix = 0.f; s_branchYaw = 0.f; }
    }
}

const Junction* terrainJunction() { return &s_junc; }
const RoadSign* terrainSigns(int& count) { count = s_signN; return s_signs; }
bool terrainBranchCommitted(bool& tookLeft) {
    if (!s_committed) return false;
    tookLeft = s_tookLeft; s_committed = false;
    return true;
}

}
