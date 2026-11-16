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

// Bigfoot: rare, in the redwoods, the pines and on the coast. It stands at the forest's edge 100-150 m ahead, side-on to
// the road; once you're within 75 m (or honk) it turns and walks off into the trees, and is gone. Never hit.
static bool spawnBigfoot(const VehicleState& car) {
    const uint8_t b = g_scene.biomeA;
    if (b != B_REDWOOD && b != B_PINE && b != B_COAST) return false;
    const Land& L = journeyLand();
    if (L.pine + L.redwood + L.broad < 0.3f || frand() > 0.06f) return false;
    for (const Critter& c : s_c) if (c.on && c.kind == 1) return false;
    RoadHit rh;
    if (!roadNearest(car.x, car.z, rh, 120.f) || rh.arm) return false;
    const int dir = cosf(car.yaw - rh.yaw) >= 0.f ? 1 : -1;
    const float d = 100.f + frand() * 50.f;
    const int k = rh.idx + dir * (int)(d / 3.f);
    if (k <= 0 || k >= roadCount() - 1) return false;
    const RoadNode& n = roadNode(k);
    const float side = (frand() < 0.5f ? -1.f : 1.f) * (16.f + frand() * 14.f);
    const float x = n.x - sinf(n.yaw) * side, z = n.z + cosf(n.yaw) * side;
    Ground g; terrainGround(x, z, g);
    if (g.mat == MAT_WATER) return false;
    for (Critter& c : s_c) if (!c.on) {
        c.on = true; c.x = x; c.z = z; c.y = g.h; c.kind = 1; c.pose = 0; c.fear = 0.f; c.speed = 0.f; c.t = 0.f;
        c.yaw = atan2f(n.z - z, n.x - x) + (frand() - 0.5f) * 0.6f;              // looking toward the road
        c.dodger = false;
        return true;
    }
    return false;
}

static void spawnDeer(const VehicleState& car) {
    const Land& L = journeyLand();
    float forest = L.pine + L.broad + L.redwood + L.flowers * 0.5f + L.bush * 0.3f;
    if (forest < 0.2f || g_scene.biomeA == B_TUNDRA) return;
    for (Critter& c : s_c) if (!c.on) {
        float fx = cosf(car.yaw), fz = sinf(car.yaw);
        // far enough ahead to see them grazing before they notice you; some on or right beside the road
        float x = 0.f, z = 0.f; Ground g; bool ok = false;
        for (int tries = 0; tries < 5 && !ok; tries++) {
            float d = 110.f + frand() * 100.f;
            float side = (frand() < 0.5f ? -1.f : 1.f) * (frand() < 0.33f ? frand() * 9.f : 9.f + frand() * 26.f);
            x = car.x + fx * d - fz * side; z = car.z + fz * d + fx * side;
            RoadHit rh;                                                  // along the road ahead, where you'll pass
            if (roadNearest(car.x, car.z, rh, 120.f) && !rh.arm) {
                int dir = cosf(car.yaw - rh.yaw) >= 0.f ? 1 : -1;
                int k = rh.idx + dir * (int)(d / 3.f);
                if (k > 0 && k < roadCount() - 1) {
                    const RoadNode& n = roadNode(k);
                    x = n.x - sinf(n.yaw) * side; z = n.z + cosf(n.yaw) * side;
                }
            }
            terrainGround(x, z, g);
            ok = g.mat != MAT_WATER && g.mat != MAT_ROCK;           // a bad spot: try another, don't give up
        }
        if (!ok) return;
        c.on = true; c.x = x; c.z = z; c.y = g.h; c.kind = 0; c.pose = 3; c.fear = 0.f; c.speed = 0.f;
        c.dodger = frand() < 0.8f;                                  // most leap clear at the last moment
        c.yaw = frand() * 6.283f; c.t = 0.f;
        // sometimes a small group
        if (frand() < 0.35f) for (Critter& d2 : s_c) if (!d2.on) { d2 = c; d2.x += (frand() - 0.5f) * 6.f; d2.z += (frand() - 0.5f) * 6.f; d2.yaw += frand() - 0.5f; break; }
        return;
    }
}

// ---- pastures: fences and cattle (the renderer draws exactly these) ----
static uint32_t s_broken[24]; static int s_brokenN = 0;          // burst fence spans (a ring: the oldest heal)
static uint32_t s_dead[16];   static int s_deadN = 0;            // cattle that met a car at speed
static inline uint32_t fenceKey(int pi, int pj, int side, int k) { return hash32((uint32_t)pi * 0x9E3779B1u ^ (uint32_t)pj * 0x85EBCA77u ^ (uint32_t)(side * 16 + k) * 0xC2B2AE3Du) | 1u; }
static inline uint32_t cowKey(int pi, int pj, int k) { return hash32((uint32_t)pi * 0x27D4EB2Fu ^ (uint32_t)pj * 0x165667B1u ^ (uint32_t)k * 0x9E3779B1u) | 1u; }
static bool inList(const uint32_t* a, int n, int cap, uint32_t key) { for (int i = 0; i < (n < cap ? n : cap); i++) if (a[i] == key) return true; return false; }
bool fieldFenceBroken(int pi, int pj, int side, int k) { return s_brokenN && inList(s_broken, s_brokenN, 24, fenceKey(pi, pj, side, k)); }
static void sideGeom(int side, float s, float& u, float& v, float& iu, float& iv) {
    const float a = 3.2f, b = PLOT_M - 3.2f;
    switch (side) {
        case 0: u = s; v = a; iu = 0.f; iv = 3.f; break;
        case 1: u = b; v = s; iu = -3.f; iv = 0.f; break;
        case 2: u = s; v = b; iu = 0.f; iv = -3.f; break;
        default: u = a; v = s; iu = 3.f; iv = 0.f; break;
    }
}
bool fieldFencePost(int pi, int pj, int side, int k, float& x, float& z) {
    const float a = 3.2f, b = PLOT_M - 3.2f, st = (b - a) / FENCE_POSTS;
    float u, v, iu, iv;
    sideGeom(side, a + st * k, u, v, iu, iv);
    x = pi * PLOT_M + u; z = pj * PLOT_M + v;
    return (terrainNearExtra(x + iu, z + iv) & 0xE0) == 0xE0;       // stands only where the pasture is (flat, off the road)
}
int fieldCows(float cx, float cz, float R, float tt, Cow* out, int max) {
    int m = 0;
    const int i0 = (int)floorf((cx - R) * (1.f / PLOT_M)), i1 = (int)floorf((cx + R) * (1.f / PLOT_M));
    const int j0 = (int)floorf((cz - R) * (1.f / PLOT_M)), j1 = (int)floorf((cz + R) * (1.f / PLOT_M));
    for (int pj = j0; pj <= j1; pj++) for (int pi = i0; pi <= i1; pi++) {
        if ((terrainPlot(pi, pj) & 0xE0) != 0xE0) continue;
        const float bx = pi * PLOT_M, bz = pj * PLOT_M;
        uint32_t hp = hash32((uint32_t)pi * 0x2545F491u ^ (uint32_t)pj * 0x9E3779B1u ^ 0xC0Du);
        int n = 3 + (int)(hp & 3);
        float hx0 = bx + 12.f + 32.f * hashf(hp >> 3), hz0 = bz + 12.f + 32.f * hashf(hp >> 7);   // the herd's centre
        for (int k = 0; k < n && m < max; k++) {
            uint32_t hc = hash32(hp + (uint32_t)k * 0x632BE5ABu);
            float yaw = hashf(hc) * 6.2831853f, ph = hashf(hc >> 5) * 6.2831853f;
            float amble = sinf(tt * 0.04f + ph) * 2.5f;
            float x = hx0 + (hashf(hc >> 9) - 0.5f) * 16.f + cosf(yaw) * amble, z = hz0 + (hashf(hc >> 13) - 0.5f) * 16.f + sinf(yaw) * amble;
            if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > R * R) continue;
            if ((terrainNearExtra(x, z) & 0xE0) != 0xE0) continue;
            if (s_deadN && inList(s_dead, s_deadN, 16, cowKey(pi, pj, k))) continue;
            Cow& c = out[m++];
            c.x = x; c.z = z; c.pose = sinf(tt * 0.3f + ph * 3.f) > -0.4f ? 1 : 0;        // mostly grazing
            c.heading = yaw + (cosf(tt * 0.04f + ph) < 0.f ? 3.1415927f : 0.f);          // facing the way it ambles
        }
    }
    return m;
}
// the car against the pastures round it
static void pastureCollide(VehicleState& car, uint8_t vehicle, FieldHit& out) {
    const float FENCE_BURST = 13.41f, COW_BURST = 20.12f;            // 30 mph, 45 mph
    const int pi0 = (int)floorf(car.x * (1.f / PLOT_M)), pj0 = (int)floorf(car.z * (1.f / PLOT_M));
    const float reach = (vehicleSpec(vehicle).bike ? 0.45f : vehicleSpec(vehicle).track * 0.6f) + 0.15f;
    for (int pj = pj0 - 1; pj <= pj0 + 1; pj++) for (int pi = pi0 - 1; pi <= pi0 + 1; pi++) {
        if ((terrainPlot(pi, pj) & 0xE0) != 0xE0) continue;
        for (int side = 0; side < 4; side++) {
            float ax, az, bx, bz;
            fieldFencePost(pi, pj, side, 0, ax, az); fieldFencePost(pi, pj, side, FENCE_POSTS, bx, bz);
            float dx = bx - ax, dz = bz - az, L2 = dx * dx + dz * dz;
            float tq = ((car.x - ax) * dx + (car.z - az) * dz) / L2;
            if (tq < -0.02f || tq > 1.02f) continue;
            tq = tq < 0.f ? 0.f : (tq > 1.f ? 1.f : tq);
            float px = ax + dx * tq, pz = az + dz * tq;
            if ((car.x - px) * (car.x - px) + (car.z - pz) * (car.z - pz) > reach * reach) continue;
            int k = (int)(tq * FENCE_POSTS); if (k >= FENCE_POSTS) k = FENCE_POSTS - 1;
            float qx, qz;
            if (!fieldFencePost(pi, pj, side, k, qx, qz) || !fieldFencePost(pi, pj, side, k + 1, qx, qz) || fieldFenceBroken(pi, pj, side, k)) continue;
            if (car.speed > FENCE_BURST) {                            // straight through it: splinters, a jolt
                s_broken[s_brokenN % 24] = fenceKey(pi, pj, side, k); s_brokenN++;
                float cy = cosf(car.yaw), sy = sinf(car.yaw), wx = cy * car.u - sy * car.v, wz = sy * car.u + cy * car.v;
                float gy = terrainHeight(px, pz);
                for (int q = 0; q < 14; q++)
                    fieldEmit(PK_FUR, px + (frand() - 0.5f) * 3.f, gy + 0.4f + frand() * 0.8f, pz + (frand() - 0.5f) * 3.f,
                              wx * 0.7f + (frand() - 0.5f) * 6.f, 2.f + frand() * 4.f, wz * 0.7f + (frand() - 0.5f) * 6.f, 0.12f, 1.0f + frand() * 0.8f);
                car.u *= 0.92f;
                if (3.f > out.strength) { out.strength = 3.f; out.kind = HIT_TREE; }
            } else {
                float h = vehicleHitPost(car, vehicle, px, pz, 0.15f);
                if (h > out.strength) { out.strength = h; out.kind = HIT_TREE; }
            }
        }
    }
    Cow cows[12];
    int n = fieldCows(car.x, car.z, 6.f, g_scene.t, cows, 12);
    for (int i = 0; i < n; i++) {
        const Cow& c = cows[i];
        float dx = c.x - car.x, dz = c.z - car.z;
        if (dx * dx + dz * dz > 2.6f * 2.6f) continue;
        if (car.speed > COW_BURST) {                                  // as a deer: fur, a red mist, gone
            int pi = (int)floorf(c.x * (1.f / PLOT_M)), pj = (int)floorf(c.z * (1.f / PLOT_M));
            uint32_t hp = hash32((uint32_t)pi * 0x2545F491u ^ (uint32_t)pj * 0x9E3779B1u ^ 0xC0Du);
            int nk = 3 + (int)(hp & 3);
            for (int k = 0; k < nk; k++) {                            // which of its herd it is (by position)
                uint32_t hc = hash32(hp + (uint32_t)k * 0x632BE5ABu);
                float yaw = hashf(hc) * 6.2831853f, ph = hashf(hc >> 5) * 6.2831853f, amble = sinf(g_scene.t * 0.04f + ph) * 2.5f;
                float hx0 = pi * PLOT_M + 12.f + 32.f * hashf(hp >> 3), hz0 = pj * PLOT_M + 12.f + 32.f * hashf(hp >> 7);
                float x = hx0 + (hashf(hc >> 9) - 0.5f) * 16.f + cosf(yaw) * amble, z = hz0 + (hashf(hc >> 13) - 0.5f) * 16.f + sinf(yaw) * amble;
                if (fabsf(x - c.x) < 0.01f && fabsf(z - c.z) < 0.01f) { s_dead[s_deadN % 16] = cowKey(pi, pj, k); s_deadN++; break; }
            }
            float cy = cosf(car.yaw), sy = sinf(car.yaw), wx = cy * car.u - sy * car.v, wz = sy * car.u + cy * car.v;
            float bx = car.x + cy * 3.f, bz = car.z + sy * 3.f, by = terrainHeight(c.x, c.z) + 1.3f;
            for (int k = 0; k < 26; k++)
                fieldEmit(PK_FUR, bx + (frand() - 0.5f) * 1.4f, by + frand() * 0.8f, bz + (frand() - 0.5f) * 1.4f, wx * 0.95f + (frand() - 0.5f) * 6.f, 2.5f + frand() * 4.f, wz * 0.95f + (frand() - 0.5f) * 6.f, 0.1f, 1.3f + frand() * 0.8f);
            for (int k = 0; k < 8; k++)
                fieldEmit(PK_MIST, bx + (frand() - 0.5f) * 2.f, by + 0.3f + frand() * 0.6f, bz + (frand() - 0.5f) * 2.f, wx * 0.9f + (frand() - 0.5f) * 2.f, 1.2f, wz * 0.9f + (frand() - 0.5f) * 2.f, 0.5f, 0.35f + frand() * 0.15f);
            car.u *= 0.8f;
            if (car.speed > out.strength) { out.strength = car.speed; out.kind = HIT_DEER; }
        } else {
            float h = vehicleHitPost(car, vehicle, c.x, c.z, 1.0f);
            if (h > out.strength) { out.strength = h; out.kind = HIT_ROCK; }
        }
    }
}

// ---- tunnels: the tube's walls and roof are solid (as drawn: walls 1.6 m past the road's edge, the crown 6.6 m up,
// the haunches down to 4.6 m at the walls). The car is held inside: pushed back out of a wall with the speed into it
// taken off (a little bounce, a scrape), and kept under the roof.
static void tunnelCollide(VehicleState& car, uint8_t vehicle, FieldHit& out) {
    RoadHit rh;
    if (!roadNearest(car.x, car.z, rh, 14.f) || !(rh.flags & RN_TUNNEL)) return;
    const int n = rh.arm ? roadArmCount() : roadCount();
    if (rh.idx < 0 || rh.idx + 1 >= n) return;
    const RoadNode& a = rh.arm ? roadArmNode(rh.idx) : roadNode(rh.idx);
    const RoadNode& b = rh.arm ? roadArmNode(rh.idx + 1) : roadNode(rh.idx + 1);
    float dx = b.x - a.x, dz = b.z - a.z, L = sqrtf(dx * dx + dz * dz);
    if (L < 0.1f) return;
    dx /= L; dz /= L;
    const float px = -dz, pz = dx;                                  // across the segment
    float tq = ((car.x - a.x) * dx + (car.z - a.z) * dz) / L;
    tq = tq < 0.f ? 0.f : (tq > 1.f ? 1.f : tq);
    const float ac = (car.x - a.x) * px + (car.z - a.z) * pz;
    const float hf = (a.half20 + (b.half20 - a.half20) * tq) * 0.05f + 1.6f;
    const float roadY = a.y + (b.y - a.y) * tq;
    const VehicleSpec& V = vehicleSpec(vehicle);
    const float body = V.bike ? 0.45f : V.track * 0.6f, lim = hf - body;
    const float cy = cosf(car.yaw), sy = sinf(car.yaw);
    if (fabsf(ac) > lim) {                                          // a wall
        const float sg = ac > 0.f ? 1.f : -1.f, nx = px * sg, nz = pz * sg;
        const float pen = fabsf(ac) - lim;
        car.x -= nx * pen; car.z -= nz * pen;
        float wx = cy * car.u - sy * car.v, wz = sy * car.u + cy * car.v;
        const float vn = wx * nx + wz * nz;
        if (vn > 0.f) {
            wx -= nx * vn * 1.2f; wz -= nz * vn * 1.2f;
            wx *= 0.97f; wz *= 0.97f;
            car.u = cy * wx + sy * wz; car.v = -sy * wx + cy * wz;
            if (vn > out.strength) { out.strength = vn; out.kind = HIT_ROCK; }
        }
    }
    const float aa = fabsf(ac) < hf ? fabsf(ac) : hf;               // the roof
    const float roof = aa < 0.55f * hf ? 6.6f : 4.6f + 2.f * (hf - aa) / (0.45f * hf);
    const float top = V.bike ? 1.25f : 1.0f;                        // the car's roof over its own height
    if (car.y + top > roadY + roof) {
        car.y = roadY + roof - top;
        if (car.vy > 0.f) { if (car.vy > out.strength) { out.strength = car.vy; out.kind = HIT_ROCK; } car.vy *= -0.2f; }
    }
}

// ---- arches: in the Red Canyon, now and then the road runs under a red rock arch (one chance in each 360 m of road)
static Arch s_arch[4]; static int s_archN = 0;
static Geyser s_gey[8]; static int s_geyN = 0;
const Arch* fieldArches(int& n) { n = s_archN; return s_arch; }
const Geyser* fieldGeysers(int& n) { n = s_geyN; return s_gey; }
static void findArches() {
    s_archN = 0;
    if (journeyCurrent() != B_CANYON) return;
    const int n = roadCount();
    if (n < 2) return;
    const int c = roadCarIndex();
    const int lo = c - 40 < 1 ? 1 : c - 40, hi = c + 130 > n - 2 ? n - 2 : c + 130;
    for (int i = lo; i <= hi && s_archN < 4; i++) {
        const RoadNode& r = roadNode(i);
        if (r.flags & (RN_BRIDGE | RN_JUNCTION | RN_STORE | RN_XING | RN_TUNNEL)) continue;
        const float blk = floorf(r.s * (1.f / 360.f));
        const uint32_t h = hash32((uint32_t)(int)blk * 0x9E3779B1u ^ worldSeed() ^ 0xA4C4u);
        if (hashf(h) > 0.6f) continue;
        const float sA = blk * 360.f + 60.f + 240.f * hashf(h >> 8);
        if (fabsf(r.s - sA) >= ROAD_DS * 0.5f) continue;
        Arch& a = s_arch[s_archN++];
        a.x = r.x; a.z = r.z; a.y = r.y; a.ax = -sinf(r.yaw); a.az = cosf(r.yaw);
        a.ri = r.half20 * 0.05f + 4.5f; a.ro = a.ri + 3.5f;
    }
}
// ---- geysers: in Devil's Pass, the tundra and the moor; one chance in each 120 m square, off the road. Each erupts
// for 7 s in its own cycle (30-50 s): a column up to 20 m, steaming between.
static void findGeysers(float cx, float cz, float t) {
    s_geyN = 0;
    const uint8_t b = journeyCurrent();
    if (b != B_DEVILS && b != B_TUNDRA && b != B_MOOR) return;
    const float Q = 120.f, R = 260.f;
    const int i0 = (int)floorf((cx - R) / Q), i1 = (int)floorf((cx + R) / Q), j0 = (int)floorf((cz - R) / Q), j1 = (int)floorf((cz + R) / Q);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1 && s_geyN < 8; i++) {
        const uint32_t h = hash32((uint32_t)i * 0x85EBCA77u ^ (uint32_t)j * 0xC2B2AE3Du ^ worldSeed() ^ 0x6E75u);
        if (hashf(h) > 0.4f) continue;
        const float x = i * Q + 20.f + 80.f * ((h >> 8) & 255) * (1.f / 255.f), z = j * Q + 20.f + 80.f * ((h >> 16) & 255) * (1.f / 255.f);
        if ((x - cx) * (x - cx) + (z - cz) * (z - cz) > R * R) continue;
        RoadHit rh;
        if (roadNearest(x, z, rh, 30.f) && fabsf(rh.across) < rh.half + 12.f) continue;
        Ground gd; terrainGround(x, z, gd);
        if (gd.mat == MAT_WATER || gd.mat == MAT_ROAD || gd.mat == MAT_SHOULDER || gd.ny < 0.92f) continue;   // on open, level ground
        const float P = 30.f + 20.f * hashf(h >> 3), ph = fmodf(t + 97.f * hashf(h >> 5), P);
        float hh = 0.f;
        if (ph < 7.f) { float u = ph * (1.f / 7.f); hh = 20.f * (u < 0.15f ? u / 0.15f : 1.f - (u - 0.15f) * (u - 0.15f) * (1.f / 0.7225f)); }
        Geyser& g = s_gey[s_geyN++];
        g.x = x; g.z = z; g.y = gd.h; g.h = hh;
    }
}

void fieldUpdate(const VehicleState& car, uint8_t vehicle, float dt) {
    findArches();
    findGeysers(car.x, car.z, g_scene.t);
    for (int k = 0; k < s_geyN; k++) {                              // spray from an erupting geyser near enough to see it (white: the red mist is the deer's)
        const Geyser& g = s_gey[k];
        if (g.h < 3.f || (g.x - car.x) * (g.x - car.x) + (g.z - car.z) * (g.z - car.z) > 140.f * 140.f || frand() > 0.5f) continue;
        fieldEmit(PK_SNOW, g.x + (frand() - 0.5f), g.y + g.h * (0.7f + 0.3f * frand()), g.z + (frand() - 0.5f),   // white droplets raining
                  (frand() - 0.5f) * 5.f, 2.f + frand() * 3.f, (frand() - 0.5f) * 5.f, 0.25f, 2.f + frand());     // back down round it
    }
    // particles
    for (Particle& p : s_p) {
        if (p.life <= 0.f) continue;
        p.age += dt;
        if (p.age >= p.life) { p.life = 0.f; continue; }
        float drag = (p.kind == PK_DUST || p.kind == PK_SMOKE) ? 1.6f : (p.kind == PK_FUR ? 2.2f : (p.kind == PK_MIST ? 3.f : 0.4f));
        p.vx -= p.vx * drag * dt; p.vz -= p.vz * drag * dt;
        p.vy += (p.kind == PK_DUST || p.kind == PK_SMOKE || p.kind == PK_MIST ? 0.6f - p.vy * 1.5f : (p.kind == PK_FUR ? -1.6f - p.vy * 1.2f : -9.8f)) * dt;
        p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
        if (p.kind == PK_DUST || p.kind == PK_SMOKE) p.size += dt * 0.9f;
        if (p.kind == PK_MIST) p.size += dt * 2.2f;
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
                  k == PK_DUST ? 0.45f : (k == PK_SMOKE ? 0.35f : 0.08f), k == PK_DUST ? 1.8f : (k == PK_SMOKE ? 1.0f : 0.8f));
    }
    if (car.evSplash > 0.f) for (int i = 0; i < 6; i++)
        fieldEmit(PK_SPRAY, car.x + (frand() - 0.5f) * 2.f, car.y, car.z + (frand() - 0.5f) * 2.f, wx * 0.3f + (frand() - 0.5f) * 5.f, 3.f + frand() * 4.f, wz * 0.3f + (frand() - 0.5f) * 5.f, 0.25f, 1.0f);

    // deer: graze near the road, look up, bound away when you come close or sound the horn
    s_deerNext -= dt;
    if (s_deerNext <= 0.f) { s_deerNext = 14.f + frand() * 30.f; if (!spawnBigfoot(car)) spawnDeer(car); }
    for (Critter& c : s_c) {
        if (!c.on) continue;
        c.t += dt;
        float dx = c.x - car.x, dz = c.z - car.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d > 260.f) { c.on = false; continue; }
        if (c.kind == 1) {                                                   // Bigfoot
            if (c.fear < 0.3f && d < 75.f) c.fear = 1.f;                     // seen: it goes
            if (c.fear > 0.3f) {
                c.yaw += clampf(wrapPi(atan2f(dz, dx) - c.yaw), -1.2f * dt, 1.2f * dt);   // turning away from the car
                c.speed += (2.4f - c.speed) * dt * 1.5f;                     // long, unhurried strides
            }
            if (c.fear > 0.3f && c.t > 12.f && d > 85.f) { c.on = false; continue; }   // into the trees
            c.x += cosf(c.yaw) * c.speed * dt; c.z += sinf(c.yaw) * c.speed * dt;
            c.y = terrainHeight(c.x, c.z);
            c.pose = c.speed > 0.4f ? (uint8_t)(1 + ((int)(c.t * 1.8f) & 1)) : 0;
            continue;
        }
        // at night a deer caught in the headlights freezes, until the car is nearly on it
        float hx = c.x - car.x, hz = c.z - car.z, ahd = hx * cosf(car.yaw) + hz * sinf(car.yaw);
        bool frozen = g_scene.lights > 0.5f && ahd > 7.f && ahd < 80.f && fabsf(-hx * sinf(car.yaw) + hz * cosf(car.yaw)) < 7.f + ahd * 0.35f;   // the beams light the roadside too
        if (frozen) { c.speed = 0.f; c.fear = 0.f; c.pose = 3; }      // stock still in the light
        else if (d < 26.f + car.speed * 0.8f) c.fear = fmaxf(c.fear, 0.7f);
        // a car bearing down: a last-moment leap sideways, clear of its path
        float fx = cosf(car.yaw), fz = sinf(car.yaw);
        float ahead = dx * fx + dz * fz, lat = -dx * fz + dz * fx;           // the deer, in the car's frame
        if (c.dodger && ahead > 0.f && ahead < 4.f + car.speed * 0.35f && fabsf(lat) < 3.f) {
            c.yaw = car.yaw + (lat >= 0.f ? 1.5708f : -1.5708f);
            c.speed = 15.f; c.fear = 1.f;
        }
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

FieldHit fieldCollide(VehicleState& car, uint8_t vehicle) {
    FieldHit out = {0.f, HIT_NONE};
    const Grid& g = terrainNear();
    int ci = (int)floorf(car.x * g.inv), cj = (int)floorf(car.z * g.inv);
    for (int j = cj - 2; j <= cj + 2; j++) for (int i = ci - 2; i <= ci + 2; i++) {
        Veg v;
        if (!vegAt(i, j, v) || v.radius <= 0.f) continue;
        if (car.y - car.comp > v.y + v.size + 0.5f) continue;        // flying over a boulder
        float h = vehicleHitPost(car, vehicle, v.x, v.z, v.radius);
        if (h > out.strength) { out.strength = h; out.kind = v.type == VEG_BOULDER ? HIT_ROCK : HIT_TREE; }
    }
    const Store* st = roadStore();
    if (st && st->active) {
        float h = vehicleHitPost(car, vehicle, st->x, st->z, 5.5f);
        if (h > out.strength) { out.strength = h; out.kind = HIT_STORE; }
    }
    for (int k = 0; k < s_archN; k++) {                                   // an arch's legs, either side of the road
        const Arch& a = s_arch[k];
        const float m = (a.ri + a.ro) * 0.5f, r = (a.ro - a.ri) * 0.5f;
        for (int e = -1; e <= 1; e += 2) {
            float h = vehicleHitPost(car, vehicle, a.x + a.ax * m * e, a.z + a.az * m * e, r);
            if (h > out.strength) { out.strength = h; out.kind = HIT_ROCK; }
        }
    }
    for (int k = 0; k < s_geyN; k++) {                                    // an erupting geyser throws the car up
        const Geyser& g = s_gey[k];
        if (g.h < 4.f || car.air) continue;
        if ((g.x - car.x) * (g.x - car.x) + (g.z - car.z) * (g.z - car.z) > 2.5f * 2.5f) continue;
        car.vy = fmaxf(car.vy, 7.f + g.h * 0.25f);
    }
    pastureCollide(car, vehicle, out);
    tunnelCollide(car, vehicle, out);
    // deer: rarely, one doesn't make it. A puff of fur, a brief light-red mist, and it's gone.
    float cy = cosf(car.yaw), sy = sinf(car.yaw);
    for (Critter& c : s_c) {
        if (!c.on || c.kind == 1) continue;                                  // (Bigfoot is never there when you arrive)
        float dx = c.x - car.x, dz = c.z - car.z;
        if (dx * dx + dz * dz > 2.0f * 2.0f || car.speed < 3.f) continue;
        c.on = false;
        float wx = cy * car.u - sy * car.v, wz = sy * car.u + cy * car.v;
        // it bursts up and forward over the bonnet, carried on with the car for a moment (seen from the seat)
        float bx = car.x + cy * 3.f, bz = car.z + sy * 3.f, by = c.y + 1.2f;
        for (int k = 0; k < 18; k++)
            fieldEmit(PK_FUR, bx + (frand() - 0.5f), by + frand() * 0.6f, bz + (frand() - 0.5f), wx * 0.95f + (frand() - 0.5f) * 5.f, 2.5f + frand() * 3.5f, wz * 0.95f + (frand() - 0.5f) * 5.f, 0.08f, 1.2f + frand() * 0.8f);
        for (int k = 0; k < 6; k++)
            fieldEmit(PK_MIST, bx + (frand() - 0.5f) * 1.5f, by + 0.3f + frand() * 0.5f, bz + (frand() - 0.5f) * 1.5f, wx * 0.9f + (frand() - 0.5f) * 2.f, 1.2f, wz * 0.9f + (frand() - 0.5f) * 2.f, 0.4f, 0.32f + frand() * 0.15f);
        car.u *= 0.88f;
        if (car.speed > out.strength) { out.strength = car.speed; out.kind = HIT_DEER; }
    }
    return out;
}

}
