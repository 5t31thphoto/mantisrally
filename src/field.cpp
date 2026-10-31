#pragma GCC optimize("Os")
#include "field.h"
#include "terrain.h"
#include "journey.h"
#include "scene.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

static Particle s_p[PART_N];
static int s_pNext = 0;
static Critter s_c[CRIT_N];
static Bird s_bird;
static uint32_t s_seed = 1, s_rng = 1;
static float s_deerNext = 8.f, s_birdNext = 20.f;

static inline float frand() { s_rng = hash32(s_rng + 0x9E3779B9u); return (s_rng >> 8) * (1.f / 16777216.f); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

void fieldInit(uint32_t seed) {
    memset(s_p, 0, sizeof(s_p)); memset(s_c, 0, sizeof(s_c)); memset(&s_bird, 0, sizeof(s_bird));
    s_seed = seed; s_rng = seed ^ 0xF1E1Du;
    s_deerNext = 6.f; s_birdNext = 15.f;
}

const Particle* fieldParticles() { return s_p; }
const Critter* fieldCritters() { return s_c; }
const Bird* fieldBirds() { return &s_bird; }

void fieldEmit(uint8_t kind, float x, float y, float z, float vx, float vy, float vz, float size, float life) {
    Particle& p = s_p[s_pNext];
    s_pNext = (s_pNext + 1) % PART_N;
    p.x = x; p.y = y; p.z = z; p.vx = vx; p.vy = vy; p.vz = vz; p.size = size; p.life = life; p.age = 0.f; p.kind = kind;
}

void fieldHorn(float x, float z) {
    for (Critter& c : s_c) if (c.on) { float d = hypotf(c.x - x, c.z - z); if (d < 80.f) c.fear = 1.f; }
}

static void spawnDeer(const VehicleState& car) {
    const Land& L = journeyLand();
    float forest = L.pine + L.broad + L.redwood + L.flowers * 0.5f + L.bush * 0.3f;
    if (forest < 0.2f || g_scene.biomeA == B_TUNDRA) return;
    for (Critter& c : s_c) if (!c.on) {
        float fx = cosf(car.yaw), fz = sinf(car.yaw);
        float d = 60.f + frand() * 70.f;
        float side = (frand() < 0.5f ? -1.f : 1.f) * (8.f + frand() * 25.f);
        float x = car.x + fx * d - fz * side, z = car.z + fz * d + fx * side;
        Ground g; terrainGround(x, z, g);
        if (g.mat == MAT_WATER || g.mat == MAT_ROCK) return;
        c.on = true; c.x = x; c.z = z; c.y = g.h; c.kind = 0; c.pose = 3; c.fear = 0.f; c.speed = 0.f;
        c.yaw = frand() * 6.283f; c.t = 0.f;
        // sometimes a small group
        if (frand() < 0.35f) for (Critter& d2 : s_c) if (!d2.on) { d2 = c; d2.x += (frand() - 0.5f) * 6.f; d2.z += (frand() - 0.5f) * 6.f; d2.yaw += frand() - 0.5f; break; }
        return;
    }
}

void fieldUpdate(const VehicleState& car, uint8_t vehicle, float dt) {
    // particles
    for (Particle& p : s_p) {
        if (p.life <= 0.f) continue;
        p.age += dt;
        if (p.age >= p.life) { p.life = 0.f; continue; }
        float drag = p.kind == PK_DUST || p.kind == PK_SMOKE ? 1.6f : 0.4f;
        p.vx -= p.vx * drag * dt; p.vz -= p.vz * drag * dt;
        p.vy += (p.kind == PK_DUST || p.kind == PK_SMOKE ? 0.6f - p.vy * 1.5f : -9.8f) * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        if (p.kind == PK_DUST || p.kind == PK_SMOKE) p.size += dt * 1.6f;
    }
    // what the wheels throw
    const VehicleSpec& V = vehicleSpec(vehicle);
    float cy = cosf(car.yaw), sy = sinf(car.yaw);
    float rearX = car.x - cy * V.wheelbase * 0.55f, rearZ = car.z - sy * V.wheelbase * 0.55f;
    float wx = cy * car.u - sy * car.v, wz = sy * car.u + cy * car.v;
    bool loose = car.surf != SURF_TARMAC;
    float slide = clampf(fabsf(car.v) / 6.f + car.spin, 0.f, 1.5f);
    float rate = (loose ? clampf(car.speed / 18.f, 0.f, 1.f) * 0.5f + slide : slide * 0.5f) * (car.air ? 0.f : 1.f);
    if (g_scene.wet > 0.4f && !loose) rate = clampf(car.speed / 20.f, 0.f, 1.f) * 0.6f;
    static float acc = 0.f;
    acc += rate * dt * 30.f;
    while (acc > 1.f) {
        acc -= 1.f;
        uint8_t k = PK_DUST;
        if (car.surf == SURF_MUD || (car.surf == SURF_DIRT && g_scene.wet > 0.4f)) k = PK_MUD;
        else if (car.surf == SURF_SNOW) k = PK_SNOW;
        else if (!loose) k = g_scene.wet > 0.4f ? PK_SPRAY : PK_SMOKE;
        if (car.water > 0.2f) k = PK_SPRAY;
        float side = frand() < 0.5f ? -1.f : 1.f;
        float ox = -sy * side * V.track * 0.5f, oz = cy * side * V.track * 0.5f;
        float spray = (k == PK_MUD || k == PK_SPRAY || k == PK_SNOW) ? 1.f : 0.3f;
        // spin throws it backwards and up, a slide throws it sideways
        float bx = -wx * 0.25f - cy * car.spin * 9.f * spray + (-sy) * car.v * 0.4f;
        float bz = -wz * 0.25f - sy * car.spin * 9.f * spray + cy * car.v * 0.4f;
        fieldEmit(k, rearX + ox, car.y - V.rideHeight + 0.2f, rearZ + oz,
                  bx + (frand() - 0.5f) * 3.f, (k == PK_DUST || k == PK_SMOKE ? 0.6f : 2.f + frand() * 3.5f * spray), bz + (frand() - 0.5f) * 3.f,
                  k == PK_DUST ? 0.9f : (k == PK_SMOKE ? 0.6f : 0.18f), k == PK_DUST ? 2.2f : (k == PK_SMOKE ? 1.2f : 0.9f));
    }
    if (car.evSplash > 0.f) for (int i = 0; i < 6; i++)
        fieldEmit(PK_SPRAY, car.x + (frand() - 0.5f) * 2.f, car.y, car.z + (frand() - 0.5f) * 2.f, wx * 0.3f + (frand() - 0.5f) * 5.f, 3.f + frand() * 4.f, wz * 0.3f + (frand() - 0.5f) * 5.f, 0.25f, 1.0f);

    // deer: graze near the road, look up, bound away when you come close or sound the horn
    s_deerNext -= dt;
    if (s_deerNext <= 0.f) { s_deerNext = 14.f + frand() * 30.f; spawnDeer(car); }
    for (Critter& c : s_c) {
        if (!c.on) continue;
        c.t += dt;
        float dx = c.x - car.x, dz = c.z - car.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d > 260.f) { c.on = false; continue; }
        if (d < 26.f + car.speed * 0.8f) c.fear = fmaxf(c.fear, 0.7f);
        if (c.fear > 0.3f) {
            float away = atan2f(dz, dx) + sinf(c.t * 0.7f + c.x) * 0.5f;
            c.yaw += clampf(wrapPi(away - c.yaw), -3.f * dt, 3.f * dt);
            c.speed += (11.f - c.speed) * dt * 2.f;
            c.fear -= dt * 0.05f;
        } else {
            c.speed += (0.f - c.speed) * dt * 2.f;
            if (fmodf(c.t, 9.f) < 0.02f) c.yaw += frand() - 0.5f;
        }
        c.x += cosf(c.yaw) * c.speed * dt; c.z += sinf(c.yaw) * c.speed * dt;
        c.y = terrainHeight(c.x, c.z);
        c.pose = c.speed > 1.f ? (uint8_t)(1 + ((int)(c.t * c.speed * 0.9f) & 1)) : (fmodf(c.t + c.x, 6.f) < 3.5f ? 3 : 0);
    }
    // a skein of birds now and then (daylight)
    s_birdNext -= dt;
    if (s_bird.on) { s_bird.az += s_bird.vaz * dt; s_bird.t += dt; if (s_bird.t > 40.f) s_bird.on = false; }
    else if (s_birdNext <= 0.f && g_scene.night < 0.4f && g_scene.rain < 0.3f) {
        s_birdNext = 40.f + frand() * 80.f;
        s_bird.on = true; s_bird.az = car.yaw - 0.9f + frand() * 0.4f; s_bird.vaz = 0.03f + frand() * 0.03f;
        s_bird.el = 0.12f + frand() * 0.2f; s_bird.t = 0.f; s_bird.n = 5 + (int)(frand() * 6.f);
    }
}

float fieldCollide(VehicleState& car, uint8_t vehicle) {
    float hit = 0.f;
    const Grid& g = terrainNear();
    int ci = (int)floorf(car.x * g.inv), cj = (int)floorf(car.z * g.inv);
    for (int j = cj - 2; j <= cj + 2; j++) for (int i = ci - 2; i <= ci + 2; i++) {
        Veg v;
        if (!vegAt(i, j, v) || v.radius <= 0.f) continue;
        if (car.y - car.comp > v.y + v.size + 0.5f) continue;        // flying over a boulder
        float r = v.radius;
        hit = fmaxf(hit, vehicleHitPost(car, vehicle, v.x, v.z, r));
    }
    const Store* st = roadStore();
    if (st && st->active) {
        // the building sits at the back of the lot
        float bx = st->x + (-sinf(st->yaw)) * 0.f, bz = st->z;
        float dx = car.x - bx, dz = car.z - bz;
        float along = dx * cosf(st->yaw) + dz * sinf(st->yaw);
        float lat = -dx * sinf(st->yaw) + dz * cosf(st->yaw);
        (void)along; (void)lat;
        hit = fmaxf(hit, vehicleHitPost(car, vehicle, bx, bz, 5.5f));
    }
    for (Critter& c : s_c) if (c.on) {                               // deer always leap clear
        float d = hypotf(c.x - car.x, c.z - car.z);
        if (d < 4.f) { c.fear = 1.f; c.speed = 14.f; }
    }
    return hit;
}

}
