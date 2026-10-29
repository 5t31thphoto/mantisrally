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
// One validity bit per cached height. A shifted strip is invalid until generated.
static uint8_t s_valid[(TG_N * TG_N + 7) / 8] = {};
static int s_ox = 0, s_oz = 0;
static float s_px = 0.f, s_pz = 0.f, s_D = 0.f;
static int s_jobAxis = -1, s_jobLine = 0, s_jobPos = TG_N;

static Junction s_junc;
static RoadSign s_signs[6];
static int s_signN = 0;
static float s_along = 0.f;
static float s_nextJuncAt = 160.f;
static bool s_committed = false, s_tookLeft = false, s_branchTaken = false;
static float s_turnYawRemaining = 0.f, s_turnDistanceRemaining = 0.f;
// continuous road ribbon pose (centerline under / ahead of the car)
static float s_cx = 0.f, s_cz = 0.f, s_cyaw = 0.f, s_cRoadCurv = 0.f;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smoothstep(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// ---- Smooth, continuous road ribbon in world XZ ----
// A low-amplitude heading field creates long sweepers. The local curve is
// integrated relative to the current ribbon anchor so moving the anchor each
// frame does not make the road jump sideways.
static inline float roadHeadingSlope(float d) {
    const float phaseA = (float)(s_seed & 1023u) * 0.006135923f;
    const float phaseB = (float)((s_seed >> 10) & 1023u) * 0.006135923f;
    const float q = s_along + d;
    float curvature = 0.34f * 0.0082f * cosf(q * 0.0082f + phaseA)
                    + 0.13f * 0.021f * cosf(q * 0.021f + phaseB);

    // One broad, seeded hairpin zone every few hundred metres. A half-sine
    // curvature pulse enters and exits at zero curvature, so there are no
    // steering discontinuities or expensive per-segment spline allocations.
    const float section = floorf((q + 40.f) / 300.f);
    const float u = (q + 40.f) - section * 300.f;
    if (u >= 72.f && u <= 144.f) {
        const float t = (u - 72.f) * (3.14159265f / 72.f);
        const uint32_t key = s_seed ^ ((uint32_t)((int32_t)section) * 0x9E3779B9u);
        const float direction = (hash32(key) & 1u) ? 1.f : -1.f;
        curvature += direction * 0.052f * sinf(t);
    }
    return curvature;
}
static void roadFrame(float x, float z, float& along, float& across, float& yaw) {
    const float baseC = cosf(s_cyaw), baseS = sinf(s_cyaw);
    float dx = x - s_cx, dz = z - s_cz;
    float d = dx * baseC + dz * baseS;
    // The centerline is locally a circular arc. Two inexpensive projection
    // passes are enough for the renderer's 70 m view and the off-road timer.
    for (int i = 0; i < 2; ++i) {
        float off = 0.5f * s_cRoadCurv * d * d;
        float px = s_cx + baseC * d - baseS * off;
        float pz = s_cz + baseS * d + baseC * off;
        float localYaw = s_cyaw + s_cRoadCurv * d;
        d += (x - px) * cosf(localYaw) + (z - pz) * sinf(localYaw);
    }
    float off = 0.5f * s_cRoadCurv * d * d;
    float px = s_cx + baseC * d - baseS * off;
    float pz = s_cz + baseS * d + baseC * off;
    yaw = s_cyaw + s_cRoadCurv * d;
    along = s_along + d;
    across = -(x - px) * sinf(yaw) + (z - pz) * cosf(yaw);
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

// The road has its own low-frequency elevation profile. Keeping this separate
// from mountain/cliff noise gives the tires a continuous surface while the
// surrounding terrain can stay dramatic. Analytic derivatives are cheap enough
// to use directly in the physics step.
static inline float roadElevation(float along, float hills) {
    const float phaseA = (float)(s_seed & 1023u) * 0.006135923f;
    const float phaseB = (float)((s_seed >> 10) & 1023u) * 0.006135923f;
    const float amp = 1.35f + hills * 0.045f;
    return amp * sinf(along * 0.039f + phaseA)
         + 0.48f * sinf(along * 0.087f + phaseB)
         + 0.12f * sinf(along * 0.19f + phaseA * 1.7f);
}
static inline float roadElevationSlope(float along, float hills) {
    const float phaseA = (float)(s_seed & 1023u) * 0.006135923f;
    const float phaseB = (float)((s_seed >> 10) & 1023u) * 0.006135923f;
    const float amp = 1.35f + hills * 0.045f;
    return amp * 0.039f * cosf(along * 0.039f + phaseA)
         + 0.48f * 0.087f * cosf(along * 0.087f + phaseB)
         + 0.12f * 0.19f * cosf(along * 0.19f + phaseA * 1.7f);
}

static float applyRoadCut(float x, float z, float zH, float across, float along) {
    (void)x; (void)z;
    const float half = 5.5f;
    const float absA = fabsf(across);
    // Most terrain samples are off-road, especially the renderer's scenery rays.
    // Reject them before biome blending/noise work.
    if (absA >= half + 6.f) return zH;
    float hillsA, ridgesA, cliffsA, canyonA, roughA;
    float hillsB, ridgesB, cliffsB, canyonB, roughB;
    biomeAmps(s_cur, hillsA, ridgesA, cliffsA, canyonA, roughA);
    biomeAmps(s_next, hillsB, ridgesB, cliffsB, canyonB, roughB);
    const float hills = hillsA * (1.f - s_blend) + hillsB * s_blend;
    const float ridges = ridgesA * (1.f - s_blend) + ridgesB * s_blend;
    const float rough = roughA * (1.f - s_blend) + roughB * s_blend;
    const float roadH = roadElevation(along, hills);
    float bank = 0.f;
    if (absA > half) {
        float t = (absA - half) / 6.f;
        bank = sinf(t * 3.14159f) * (2.0f + 1.4f * rough) * (1.f + ridges * 0.12f);
        if (t > 0.7f) bank -= (t - 0.7f) * 3.f;
    }
    // Full-strength replacement across the driveable surface; only the verge
    // blends back to natural terrain. No cliff/ridge noise leaks through the road.
    float blend = 1.f - smoothstep(half - 0.35f, half + 4.0f, absA);
    float crown = absA <= half ? (1.f - (absA / half) * (absA / half)) * 0.12f : 0.f;
    const float flat = roadH + crown + bank;
    return zH * (1.f - blend) + flat * blend;
}

static float terrainShapeInternal(float x, float z, float* acrossOut, bool cutRoad) {
    float along = 0.f, across = 0.f, yaw = 0.f;
    if (cutRoad || acrossOut) roadFrame(x, z, along, across, yaw);
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

    // The height cache stores natural terrain only. Apply the moving road cut
    // at sample time so the curved ribbon cannot slide away from its own ground.
    if (cutRoad) zH = applyRoadCut(x, z, zH, across, along);

    return zH;
}

float terrainShape(float x, float z, float* acrossOut) {
    return terrainShapeInternal(x, z, acrossOut, true);
}

static inline int cellIndex(int ix, int iz) {
    return ((iz % TG_N + TG_N) % TG_N) * TG_N + (ix % TG_N + TG_N) % TG_N;
}
static inline bool cellValid(int idx) {
    return (s_valid[idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0;
}
static inline void setCellValid(int idx, bool valid) {
    const uint8_t bit = (uint8_t)(1u << (idx & 7));
    if (valid) s_valid[idx >> 3] |= bit;
    else s_valid[idx >> 3] &= (uint8_t)~bit;
}
static void invalidateLine(int axis, int line, int start, int count) {
    for (int k = 0; k < count; ++k) {
        const int idx = axis == 0 ? cellIndex(line, start + k) : cellIndex(start + k, line);
        setCellValid(idx, false);
    }
}
static void dreamCell(int ix, int iz) {
    if (!s_h) return;
    const int idx = cellIndex(ix, iz);
    setCellValid(idx, false);
    float x = ix * TG_CELL, z = iz * TG_CELL;
    float h = terrainShapeInternal(x, z, nullptr, false);
    float d = h * 10.f;
    if (d > 32000.f) d = 32000.f;
    if (d < -32000.f) d = -32000.f;
    s_h[idx] = (int16_t)lroundf(d);
    setCellValid(idx, true);
}

static float sampleGrid(float x, float z) {
    if (!s_h) return terrainShape(x, z, nullptr);
    float gx = x / TG_CELL, gz = z / TG_CELL;
    int ix = (int)floorf(gx), iz = (int)floorf(gz);
    // outside window → shape function
    if (ix < s_ox + 1 || iz < s_oz + 1 || ix + 2 >= s_ox + TG_N || iz + 2 >= s_oz + TG_N)
        return terrainShape(x, z, nullptr);
    const int i00 = cellIndex(ix, iz), i10 = cellIndex(ix + 1, iz);
    const int i01 = cellIndex(ix, iz + 1), i11 = cellIndex(ix + 1, iz + 1);
    if (!cellValid(i00) || !cellValid(i10) || !cellValid(i01) || !cellValid(i11))
        return terrainShape(x, z, nullptr);
    float fx = gx - ix, fz = gz - iz;
    auto at = [&](int idx) -> float { return s_h[idx] * 0.1f; };
    float a = at(i00), b = at(i10), c = at(i01), d = at(i11);
    float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
    float natural = ab + (cd - ab) * fz;
    float along, across, yaw;
    roadFrame(x, z, along, across, yaw);
    (void)yaw;
    return applyRoadCut(x, z, natural, across, along);
}

void terrainInit(uint32_t seed) {
    s_seed = seed ? seed : 1;
    // The journey chooses the starting biome before terrainInit. Preserve that
    // selection so the initial height cache is generated with the right terrain.
    if (!s_h) s_h = (int16_t*)memFast(TG_N * TG_N * sizeof(int16_t));
    memset(s_valid, 0, sizeof(s_valid));
    s_ox = -TG_N / 2; s_oz = -TG_N / 2;
    s_px = 0.f; s_pz = 0.f; s_D = 0.f;
    for (int j = 0; j < TG_N; j++)
        for (int i = 0; i < TG_N; i++) dreamCell(s_ox + i, s_oz + j);
    s_jobPos = TG_N;
    memset(&s_junc, 0, sizeof(s_junc));
    s_signN = 0; s_along = 0.f;
    s_cx = 0.f; s_cz = 0.f; s_cyaw = 0.f;
    s_cRoadCurv = roadHeadingSlope(0.f);
    s_nextJuncAt = 140.f + hashf(s_seed) * 100.f;
    s_committed = false; s_branchTaken = false;
    s_turnYawRemaining = s_turnDistanceRemaining = 0.f;
}

void terrainSetBiome(uint8_t cur, uint8_t next, float blend) {
    s_cur = cur % 17; s_next = next % 17; s_blend = clampf(blend, 0.f, 1.f);
}

void terrainRefresh() {
    if (!s_h) return;
    for (int j = 0; j < TG_N; ++j)
        for (int i = 0; i < TG_N; ++i) dreamCell(s_ox + i, s_oz + j);
    s_jobPos = TG_N;
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

    // On the driveable surface, use the exact derivative of its smooth profile.
    // This removes three expensive height queries (and their noise evaluations)
    // from the common physics path. Off-road still samples the real terrain.
    if (fabsf(across) < t.roadHalf - 0.25f) {
        float hillsA, ridgesA, cliffsA, canyonA, roughA;
        float hillsB, ridgesB, cliffsB, canyonB, roughB;
        biomeAmps(s_cur, hillsA, ridgesA, cliffsA, canyonA, roughA);
        biomeAmps(s_next, hillsB, ridgesB, cliffsB, canyonB, roughB);
        const float hills = hillsA * (1.f - s_blend) + hillsB * s_blend;
        t.slope = roadElevationSlope(along, hills);
        t.camber = 0.f;
    } else {
        const float d = 1.5f;
        float h1 = sampleGrid(x + cosf(yaw) * d, z + sinf(yaw) * d);
        t.slope = (h1 - t.height) / d;
        float hL = sampleGrid(x - sinf(yaw) * 3.f, z + cosf(yaw) * 3.f);
        float hR = sampleGrid(x + sinf(yaw) * 3.f, z - cosf(yaw) * 3.f);
        t.camber = atanf((hR - hL) / 6.f);
    }

    t.curv = s_cRoadCurv;

    uint8_t sa = SURF[s_cur], sb = SURF[s_next];
    t.surface = (s_blend < 0.5f) ? sa : sb;
    static const float GRIP0[5] = { 0.96f, 0.74f, 0.60f, 0.50f, 0.42f };
    float ga = GRIP0[sa] * journeyBiome(s_cur).gripBias;
    float gb = GRIP0[sb] * journeyBiome(s_next).gripBias;
    float g = ga * (1.f - s_blend) + gb * s_blend;
    // Grip fades gradually from asphalt to rough ground; it never vanishes.
    float off = fmaxf(0.f, fabsf(across) - t.roadHalf);
    float edge = 1.f - 0.58f * smoothstep(0.f, 10.f, off);
    g *= 1.f - clampf(fabsf(t.slope) * 0.15f, 0.f, 0.25f);
    t.grip = g * edge;
    t.roughness = (t.surface == 1 || t.surface == 2)
        ? 0.3f + 0.45f * fbm2(x * 0.06f, z * 0.06f, 2, s_seed + 7)
        : (t.surface == 3 ? 0.12f : (t.surface == 4 ? 0.55f : 0.06f));
    t.bankL = t.bankR = 7.f + 3.f * hashf(s_seed + (int)along);
    return t;
}

float terrainHeight(float x, float z) { return sampleGrid(x, z); }

void terrainRecoverRoad(float& x, float& z) {
    float along, across, yaw;
    roadFrame(x, z, along, across, yaw);
    // Recover to the nearest edge, not the centre: shortcuts remain possible and
    // the driver can immediately steer back onto the road.
    const float targetAcross = (across < 0.f ? -1.f : 1.f) * 4.4f;
    const float shift = targetAcross - across;
    x += -sinf(yaw) * shift;
    z +=  cosf(yaw) * shift;
    (void)along;
}

void terrainDirection(float x, float z, float& dirX, float& dirZ, float& curvature) {
    float along, across, yaw;
    roadFrame(x, z, along, across, yaw);
    dirX = cosf(yaw); dirZ = sinf(yaw);
    curvature = s_cRoadCurv;
    (void)along; (void)across;
}

// ---- junctions (same as before) ----
static uint8_t randomBiome(uint32_t salt) {
    uint8_t b = (uint8_t)(hash32(s_seed + salt) % 17);
    if (b == s_cur || b == s_next) b = (b + 5) % 17;
    return b;
}

static void spawnJunction() {
    float dist = 50.f + hashf(s_seed + (int)s_along) * 30.f;
    float lateral = 0.5f * s_cRoadCurv * dist * dist;
    float roadYaw = s_cyaw + s_cRoadCurv * dist;
    float baseC = cosf(s_cyaw), baseS = sinf(s_cyaw);
    float ax = cosf(roadYaw), az = sinf(roadYaw);
    s_junc.active = true;
    s_junc.x = s_cx + baseC * dist - baseS * lateral;
    s_junc.z = s_cz + baseS * dist + baseC * lateral;
    s_junc.yaw = roadYaw;
    s_junc.destL = randomBiome((uint32_t)(s_along * 3) + 11);
    s_junc.destR = randomBiome((uint32_t)(s_along * 5) + 29);
    if (s_junc.destR == s_junc.destL) s_junc.destR = (s_junc.destL + 4) % 17;
    s_junc.armL = -0.6f - hashf(s_seed + 41) * 0.3f;
    s_junc.armR =  0.6f + hashf(s_seed + 43) * 0.3f;
    s_junc.dist = dist;
    s_committed = false; s_branchTaken = false;
    s_signN = 2;
    float sx = cosf(roadYaw + 1.57f), sz = sinf(roadYaw + 1.57f);
    s_signs[0] = {true, s_junc.x + sx * 5.f - ax * 8.f, s_junc.z + sz * 5.f - az * 8.f, roadYaw, s_junc.destL, true};
    s_signs[1] = {true, s_junc.x - sx * 5.f - ax * 8.f, s_junc.z - sz * 5.f - az * 8.f, roadYaw, s_junc.destR, false};
}

void terrainUpdateRoad(float carX, float carZ, float carYaw, float speed, float dt) {
    s_px = carX; s_pz = carZ;
    // The road is an independent curved ribbon. It does not rotate to follow
    // the player: steering off-line is real, and the recovery timer handles it.
    float dx = carX - s_cx, dz = carZ - s_cz;
    float c = cosf(s_cyaw), sn = sinf(s_cyaw);
    float lat = -dx * sn + dz * c;
    float forward = speed * dt;
    float pay = (fabsf(lat) < 14.f) ? forward : forward * 0.35f;
    if (pay < 0.f) pay = 0.f;

    // A selected fork bends the ribbon over 28 metres and then remains the road.
    float turnStep = 0.f;
    if (s_turnDistanceRemaining > 0.001f && pay > 0.f) {
        float fraction = clampf(pay / s_turnDistanceRemaining, 0.f, 1.f);
        turnStep = s_turnYawRemaining * fraction;
        s_turnYawRemaining -= turnStep;
        s_turnDistanceRemaining -= pay;
        if (s_turnDistanceRemaining <= 0.001f) {
            s_turnYawRemaining = 0.f;
            s_turnDistanceRemaining = 0.f;
        }
    }
    float baseCurv = roadHeadingSlope(0.f);
    float curveStep = baseCurv * pay;
    float turnCurv = (pay > 0.001f) ? turnStep / pay : 0.f;
    float midYaw = s_cyaw + 0.5f * (turnStep + curveStep);
    s_cx += cosf(midYaw) * pay;
    s_cz += sinf(midYaw) * pay;
    s_cyaw += turnStep + curveStep;
    s_along += pay;
    s_cRoadCurv = roadHeadingSlope(0.f) + turnCurv;
    s_D = s_along;
    // scroll height grid (Seadiver budgeted rows/cols). If the cache allocation
    // failed, sampleGrid falls back to the procedural shape function.
    if (s_h) {
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
            // Invalidate the entire newly exposed strip before incremental filling.
            // Any query touching unfinished cells safely uses the analytic fallback.
            invalidateLine(s_jobAxis, s_jobLine, s_jobAxis == 0 ? s_oz : s_ox, TG_N);
            s_jobPos = 0;
        }
        int n = TG_N - s_jobPos < budget ? TG_N - s_jobPos : budget;
        for (int k = 0; k < n; k++, s_jobPos++) {
            if (s_jobAxis == 0) dreamCell(s_jobLine, s_oz + s_jobPos);
            else dreamCell(s_ox + s_jobPos, s_jobLine);
        }
        budget -= n;
    }
    }

    if (!s_junc.active && s_along > s_nextJuncAt) {
        spawnJunction();
        s_nextJuncAt = s_along + 200.f + hashf(s_seed + (int)s_along) * 180.f;
    }
    if (!s_junc.active) return;
    // Junction-relative vector (use distinct names from the ribbon delta above).
    float jdx = s_junc.x - carX, jdz = s_junc.z - carZ;
    float d = sqrtf(jdx * jdx + jdz * jdz);
    if (d > 90.f) {
        float fwd = jdx * cosf(carYaw) + jdz * sinf(carYaw);
        if (fwd < -5.f) { s_junc.active = false; s_signN = 0; }
    }
    if (!s_branchTaken && d < 18.f) {
        // Signed side of the car relative to the junction (not the vector
        // from the car back to the junction, whose sign is reversed).
        float side = jdx * sinf(s_junc.yaw) - jdz * cosf(s_junc.yaw);
        float headingDiff = carYaw - s_junc.yaw;
        while (headingDiff > 3.14f) headingDiff -= 6.28f;
        while (headingDiff < -3.14f) headingDiff += 6.28f;
        bool left = (side < -1.2f) || (headingDiff < -0.18f);
        bool right = (side > 1.2f) || (headingDiff > 0.18f);
        if (left || right) {
            // Prefer a clear heading choice when it disagrees with position.
            s_tookLeft = (left && !right) || (left && right && headingDiff < -0.18f);
            s_committed = true; s_branchTaken = true;
            uint8_t dest = s_tookLeft ? s_junc.destL : s_junc.destR;
            s_turnYawRemaining = s_tookLeft ? s_junc.armL : s_junc.armR;
            s_turnDistanceRemaining = 28.f;
            journeyCommitBranch(dest);
            // Keep the fork geometry alive as the car passes it; it is culled
            // once the junction is well behind the player. This prevents the
            // chosen and unchosen arm from vanishing at the instant of commit.
        }
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
