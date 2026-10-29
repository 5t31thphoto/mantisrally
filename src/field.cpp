#pragma GCC optimize("Os")
#include "field.h"
#include "terrain.h"
#include "universe.h"
#include "journey.h"
#include <math.h>
#include <string.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static FieldObj s_obj[FIELD_N];
static float s_carX = 0.f, s_carZ = 0.f, s_carYaw = 0.f;
static float s_spawnAcc = 0.f;

void fieldInit() {
    memset(s_obj, 0, sizeof(s_obj));
    s_spawnAcc = 0.f;
}

static int freeSlot() {
    for (int i = 0; i < FIELD_N; i++) if (!s_obj[i].on) return i;
    return -1;
}

static void place(int i, uint8_t type, float x, float z, uint32_t h) {
    TrackSample ts = terrainAt(x, z);
    s_obj[i].type = type;
    s_obj[i].var  = (uint8_t)(h & 3);
    s_obj[i].x = x; s_obj[i].z = z;
    s_obj[i].y = ts.height;
    s_obj[i].yaw = (h >> 8) * (kTau / 256.f);
    s_obj[i].on = true;
    switch (type) {
        case OBJ_TREE:
            s_obj[i].a = 1.4f + (h & 15) * 0.18f;           // trunk radius-ish
            s_obj[i].b = 5.5f + (h >> 4 & 31) * 0.35f;      // height
            break;
        case OBJ_ROCK:
            s_obj[i].a = 0.8f + (h & 15) * 0.15f;
            s_obj[i].b = s_obj[i].a * (0.7f + (h & 7) * 0.08f);
            break;
        case OBJ_BARRIER:
            s_obj[i].a = 1.2f + (h & 7) * 0.1f;
            s_obj[i].b = 0.9f;
            break;
        case OBJ_HAYBALE:
            s_obj[i].a = 1.0f + (h & 7) * 0.08f;
            s_obj[i].b = 0.8f;
            break;
        case OBJ_SIGN:
            s_obj[i].a = 0.4f; s_obj[i].b = 2.2f;
            break;
        case OBJ_SPECTATOR:
            s_obj[i].a = 0.35f; s_obj[i].b = 1.6f;
            break;
        default:
            s_obj[i].a = 1.f; s_obj[i].b = 1.f;
            break;
    }
}

static void spawnWave(float cx, float cz, float carYaw) {
    const BiomeInfo& st = journeyBiome(journeyCurrent());
    const BiomeInfo& nx = journeyBiome(journeyNext());
    float bl = journeyBlend();
    float treeD = st.treeDensity * (1.f - bl) + nx.treeDensity * bl;
    float rockD = st.rockDensity * (1.f - bl) + nx.rockDensity * bl;

    float ax = cosf(carYaw), az = sinf(carYaw);
    float rx = -az, rz = ax;

    int budget = 10;
    for (int attempt = 0; attempt < 28 && budget > 0; attempt++) {
        int i = freeSlot();
        if (i < 0) break;
        uint32_t h = hash32(worldSeed() + (uint32_t)(cx * 7.f) + (uint32_t)(cz * 11.f) + attempt * 97u + (uint32_t)s_spawnAcc);
        float along = 18.f + (h & 255) / 255.f * 55.f;
        float side = ((h >> 8) & 255) / 255.f * 2.f - 1.f;
        // prefer banks, not the road centre
        float lat = (6.5f + (h >> 16 & 31) * 0.45f) * (side < 0.f ? -1.f : 1.f);
        float x = cx + ax * along + rx * lat;
        float z = cz + az * along + rz * lat;
        TrackSample ts = terrainAt(x, z);
        if (fabsf(ts.across) < ts.roadHalf + 1.2f) continue; // still on road

        float roll = (h >> 24) / 255.f;
        uint8_t type = OBJ_NONE;
        if (roll < treeD * 0.55f) type = OBJ_TREE;
        else if (roll < treeD * 0.55f + rockD * 0.35f) type = OBJ_ROCK;
        else if (roll < 0.78f) type = OBJ_SIGN;
        else if (roll < 0.86f) type = OBJ_SPECTATOR;
        else if (roll < 0.93f) type = OBJ_HAYBALE;
        else type = OBJ_BARRIER;

        // no spectators in desert/volcano/salt empties
        if (type == OBJ_SPECTATOR && treeD < 0.15f && rockD > 0.4f) type = OBJ_ROCK;
        if (type == OBJ_TREE && treeD < 0.08f) type = OBJ_ROCK;

        place(i, type, x, z, h);
        budget--;
    }
}

void fieldUpdate(float carX, float carZ, float carYaw, float speed, float dt) {
    s_carX = carX; s_carZ = carZ; s_carYaw = carYaw;
    s_spawnAcc += speed * dt;

    // Cull behind / far
    float ax = cosf(carYaw), az = sinf(carYaw);
    for (int i = 0; i < FIELD_N; i++) {
        if (!s_obj[i].on) continue;
        float dx = s_obj[i].x - carX, dz = s_obj[i].z - carZ;
        float d2 = dx * dx + dz * dz;
        if (d2 > 110.f * 110.f) { s_obj[i].on = false; continue; }
        // drop far behind
        float along = dx * ax + dz * az;
        if (along < -18.f && d2 > 30.f * 30.f) s_obj[i].on = false;
    }

    // Spawn on a distance clock
    static float lastSpawn = 0.f;
    if (s_spawnAcc - lastSpawn > 12.f) {
        lastSpawn = s_spawnAcc;
        spawnWave(carX + ax * 40.f, carZ + az * 40.f, carYaw);
    }
}

float fieldMove(float& x, float& y, float& z, float& yaw,
                float dx, float dy, float dz, float dyaw,
                float* bump, float* grip) {
    if (bump) *bump = 0.f;
    TrackSample ts = terrainAt(x + dx, z + dz);
    if (grip) *grip = ts.grip;

    float nx = x + dx, nz = z + dz;
    float ny = ts.height + 0.35f;

    // Soft verge: grip already drops off-road; do NOT zero speed (that made the car die).
    float hit = 0.f;
    if (fabsf(ts.across) > ts.roadHalf + ts.bankL * 0.95f) {
        float push = (fabsf(ts.across) - ts.roadHalf - ts.bankL * 0.5f) * 0.04f;
        hit = fmaxf(hit, clampf(push, 0.f, 0.25f));
        // mild scrub only
        nx = x + dx * 0.92f;
        nz = z + dz * 0.92f;
    }

    for (int i = 0; i < FIELD_N; i++) {
        if (!s_obj[i].on) continue;
        uint8_t t = s_obj[i].type;
        if (t == OBJ_SIGN || t == OBJ_SPECTATOR || t == OBJ_CHECKPOINT) continue;
        float ox = s_obj[i].x - nx, oz = s_obj[i].z - nz;
        float rad = s_obj[i].a * (t == OBJ_TREE ? 0.7f : 0.55f);
        float d2 = ox * ox + oz * oz;
        if (d2 < rad * rad) {
            float d = sqrtf(fmaxf(d2, 1e-6f));
            float push = (rad - d) / rad;
            // slide: kill the component into the object
            float nxn = ox / d, nzn = oz / d;
            float into = dx * nxn + dz * nzn;
            if (into > 0.f) {
                nx -= nxn * into * 0.85f;
                nz -= nzn * into * 0.85f;
            }
            nx -= nxn * push * rad * 0.35f;
            nz -= nzn * push * rad * 0.35f;
            hit = fmaxf(hit, push);
        }
    }
    if (bump) *bump = hit;

    float frac = 1.f - hit * 0.25f;
    x = x + (nx - x);
    z = z + (nz - z);
    // re-sample height at final pos
    y = terrainAt(x, z).height + 0.35f;
    yaw += dyaw * frac;
    return frac;
}

const FieldObj* fieldObjects() { return s_obj; }

float fieldNearest(uint8_t objType) {
    float best = 999.f;
    for (int i = 0; i < FIELD_N; i++) {
        if (!s_obj[i].on || s_obj[i].type != objType) continue;
        float dx = s_obj[i].x - s_carX, dz = s_obj[i].z - s_carZ;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < best) best = d;
    }
    return best;
}

void fieldScare(float x, float z, float radius) {
    for (int i = 0; i < FIELD_N; i++) {
        if (!s_obj[i].on || s_obj[i].type != OBJ_SPECTATOR) continue;
        float dx = s_obj[i].x - x, dz = s_obj[i].z - z;
        if (dx * dx + dz * dz < radius * radius) s_obj[i].var = 3;
    }
}

}
