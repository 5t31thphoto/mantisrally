#pragma GCC optimize("Os")
#include "scene.h"
#include "journey.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

Scene g_scene;
static uint32_t s_seed = 1;
static float s_frontV = 0.f, s_frontT = 0.f;
static float s_rainAgo = 999.f, s_nextBolt = 6.f;
static bool s_forced = false;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float follow(float c, float t, float dt, float r) { return c + (t - c) * (1.f - expf(-dt * r)); }
static inline float sstep(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

void sceneInit(uint32_t seed) {
    memset(&g_scene, 0, sizeof(g_scene));
    s_seed = seed;
    g_scene.front = 0.25f; s_frontT = 0.3f;
    s_rainAgo = 999.f; s_nextBolt = 6.f; s_forced = false;
    g_scene.boltAge = 99.f;
}

void sceneForceWeather(float cloud, float rain, float fog, float storm, float snow) {
    Scene& s = g_scene;
    s.cloud = cloud; s.rain = rain; s.fog = fog; s.storm = storm; s.snow = snow; s.front = clampf(rain + storm, 0.f, 1.f);
    s.wet = rain > 0.1f ? 0.8f : 0.f;
    s_forced = true;
}

void sceneUpdate(float t, float dt, float x, float z, float yaw, float speed, float distance) {
    Scene& s = g_scene;
    s.t = t; s.dt = dt; s.x = x; s.z = z; s.yaw = yaw; s.speed = speed; s.distance = distance;
    s.tod = timeOfDayAt(t, distance);
    s.sunElev = sunPathElevation(s.tod);
    s.sunAz = AZ_EAST + (s.tod - 0.25f) * kTau;                   // east at dawn, south at noon, west at dusk
    s.night = clampf(1.f - (s.sunElev + 0.12f) / 0.22f, 0.f, 1.f);
    s.dayLight = clampf(0.08f + 0.92f * sstep(-0.10f, 0.35f, s.sunElev), 0.f, 1.f);
    s.moon = clampf(0.5f + 0.5f * sinf(t * 0.0007f + (float)(s_seed & 255)), 0.f, 1.f);
    s.biomeA = journeyCurrent(); s.biomeB = journeyNext(); s.blend = journeyBlend();
    const Biome& A = biome(s.biomeA);
    const Biome& B = biome(s.biomeB);
    float bl = s.blend, al = 1.f - bl;
#define BM(f) (A.f * al + B.f * bl)

    if (!s_forced) {
        // the weather front wanders: a few minutes of clear, a few of whatever this biome brews
        s_frontT += dt * (1.f / 70.f);
        float target = fbmP((float)(s_seed & 63), s_frontT, 1 << 20, 2, s_seed + 0xF0);
        target = sstep(0.32f, 0.72f, target);
        s_frontV += (target - s.front) * dt * 0.02f - s_frontV * dt * 0.3f;
        s.front = clampf(s.front + s_frontV * dt + (target - s.front) * dt * 0.03f, 0.f, 1.f);
        float w = s.front;
        float cold = clampf(BM(snow) * 1.6f, 0.f, 1.f);
        float tCloud = clampf(BM(cloud) * 0.45f + w * (0.25f + BM(cloud) * 0.6f), 0.f, 1.f);
        float tRain = clampf((w - 0.35f) * 1.6f, 0.f, 1.f) * BM(rain) * 1.3f * (1.f - cold);
        float tSnow = clampf((w - 0.25f) * 1.5f, 0.f, 1.f) * cold;
        float tStorm = clampf((w - 0.6f) * 2.5f, 0.f, 1.f) * BM(storm) * 1.6f;
        float dawnMist = sstep(0.18f, 0.24f, s.tod) * (1.f - sstep(0.30f, 0.38f, s.tod));
        float tFog = clampf(BM(fog) * (0.25f + 0.5f * w) + dawnMist * 0.35f * BM(fog), 0.f, 1.f);
        float tDust = BM(dust) * clampf(0.3f + speed / 30.f, 0.f, 1.f) * (1.f - tRain);
        s.cloud = follow(s.cloud, tCloud, dt, 0.08f);
        s.rain = follow(s.rain, clampf(tRain, 0.f, 1.f), dt, 0.12f);
        s.snow = follow(s.snow, tSnow, dt, 0.1f);
        s.storm = follow(s.storm, clampf(tStorm, 0.f, 1.f), dt, 0.08f);
        s.fog = follow(s.fog, tFog, dt, 0.06f);
        s.dust = follow(s.dust, tDust, dt, 0.3f);
        s.mist = follow(s.mist, dawnMist * (0.4f + BM(fog)) * (1.f - s.wind), dt, 0.1f);
        if (s.rain > 0.15f) { s_rainAgo = 0.f; s.wet = follow(s.wet, 1.f, dt, 0.25f); }
        else { s_rainAgo += dt; s.wet = follow(s.wet, 0.f, dt, 0.02f); }
    } else {
        s_rainAgo = s.rain > 0.15f ? 0.f : 60.f;
    }
    s.wind = follow(s.wind, 0.15f + s.storm * 0.7f + s.cloud * 0.15f, dt, 0.1f);
    s.windAz += (fbmP(1.f, t * 0.003f, 1 << 20, 1, s_seed + 3) - 0.5f) * dt * 0.05f;

    // lightning: bolts somewhere around you, thunder after the light
    s.flash = fmaxf(0.f, s.flash - dt * 5.f);
    s.boltAge += dt;
    if (s.thunderIn > 0.f) s.thunderIn -= dt;
    if (s.storm > 0.3f) {
        s_nextBolt -= dt * s.storm;
        if (s_nextBolt <= 0.f) {
            uint32_t h = hash32(s_seed + (uint32_t)(t * 100.f));
            s.boltSeed = h;
            s.boltAz = yaw + (hashf(h) - 0.5f) * 2.4f;
            s.boltAge = 0.f;
            s.flash = 0.6f + 0.4f * hashf(h >> 3);
            s.thunderIn = 0.6f + hashf(h >> 7) * 5.f;
            s.thunderPow = 1.2f - s.thunderIn / 6.f;
            s_nextBolt = 3.f + hashf(h >> 11) * 14.f;
        }
    }
    // the gifts
    float lowSun = s.sunElev > 0.02f && s.sunElev < 0.65f ? 1.f : 0.f;
    float rbT = (s_rainAgo > 2.f && s_rainAgo < 150.f && s.rain < 0.15f && s.cloud < 0.85f) ? lowSun : 0.f;
    s.rainbow = follow(s.rainbow, rbT, dt, 0.5f);
    s.aurora = follow(s.aurora, BM(aurora) * s.night * clampf(1.2f - s.cloud * 1.6f, 0.f, 1.f), dt, 0.2f);
    float dusk = sstep(0.72f, 0.78f, s.tod) * (1.f - sstep(0.92f, 0.98f, s.tod)) + (1.f - sstep(0.06f, 0.2f, s.tod)) * 0.6f;
    s.fireflies = follow(s.fireflies, BM(fireflies) * dusk * (1.f - s.rain * 2.f), dt, 0.2f);
    s.leaves = BM(leaves) * (0.4f + 0.6f * s.wind);
    float gaps = sstep(0.1f, 0.35f, s.cloud) * (1.f - sstep(0.75f, 0.95f, s.cloud));
    float lowLight = 1.f - sstep(0.25f, 0.7f, s.sunElev);
    s.beams = follow(s.beams, clampf(BM(beams) * (0.35f + 0.65f * lowLight) * (0.3f + 0.7f * gaps) * (1.f - s.rain) * (s.sunElev > -0.02f ? 1.f : 0.f) + s.fog * 0.2f * (1.f - s.night), 0.f, 1.f), dt, 0.3f);
    // headlights: automatic, like a real car's dusk sensor
    float dark = 1.f - s.dayLight * (1.f - s.storm * 0.5f) * (1.f - s.fog * 0.4f) * (1.f - (s.biomeA == B_REDWOOD ? 0.35f : 0.f));
    float lt = dark > 0.62f ? 1.f : (dark < 0.5f ? 0.f : s.lights);
    s.lights = follow(s.lights, lt, dt, 3.f);
    // awe: sunsets, rainbows, aurora, storms
    float sunset = (s.sunElev > -0.05f && s.sunElev < 0.25f) ? (1.f - s.cloud * 0.6f) : 0.f;
    s.awe = fmaxf(fmaxf(s.rainbow, s.aurora), fmaxf(sunset * 0.7f, s.storm * 0.5f));
#undef BM
}

}
