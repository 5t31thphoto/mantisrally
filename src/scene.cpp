#pragma GCC optimize("Os")
#include "scene.h"
#include "journey.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

Scene g_scene;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float follow(float cur, float target, float dt, float rate) {
    float a = 1.f - expf(-dt * rate);
    return cur + (target - cur) * a;
}
static inline float smoothstep(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

void sceneInit() {
    memset(&g_scene, 0, sizeof(g_scene));
    g_scene.lights = true;
    g_scene.rainAgo = 999.f;
}

void sceneUpdate(float t, float dt,
                 float x, float y, float z,
                 float yaw, float pitch,
                 float distance, float speed, float throttle,
                 float grip, float slip, bool handbrake) {
    Scene& s = g_scene;
    s.t = t; s.dt = dt;
    s.x = x; s.y = y; s.z = z;
    s.yaw = yaw; s.pitch = pitch;
    s.distance = distance; s.speed = speed; s.throttle = throttle;
    s.grip = grip;
    s.roughness = fmaxf(0.f, 1.f - grip) * 0.7f;
    s.handbrake = handbrake ? 1.f : 0.f;
    s.slip = slip;
    s.engineLoad = clampf(throttle * (0.4f + speed / 50.f), 0.f, 1.f);

    s.tod = timeOfDayAt(t, distance);
    s.sunElev = sunPathElevation(s.tod);
    s.sunAz = AZ_EAST + s.tod * kTau;
    s.dayLight = clampf(0.12f + 0.88f * fmaxf(0.f, s.sunElev * 1.7f), 0.f, 1.f);

    s.stage = journeyCurrent();
    const BiomeInfo& A = journeyBiome(s.stage);
    const BiomeInfo& B = journeyBiome(journeyNext());
    float bl = journeyBlend();
    float wMix = journeyWeatherMix();

    // Target weather from biome blend + breathing
    float tCloud = (A.fog * (1.f - bl) + B.fog * bl) * 0.7f + 0.15f * wMix;
    float tRain  = (A.rain * (1.f - bl) + B.rain * bl) * wMix;
    float tDust  = (A.dust * (1.f - bl) + B.dust * bl) * (0.35f + 0.65f * clampf(speed / 28.f, 0.f, 1.f));
    float tSnow  = (A.snowChance * (1.f - bl) + B.snowChance * bl) * wMix;
    float tFog   = (A.fog * (1.f - bl) + B.fog * bl) * (0.45f + 0.55f * wMix);

    // Storm chance: heavy cloud + rain
    float tStorm = 0.f;
    if (tCloud > 0.55f && tRain > 0.4f) tStorm = (tCloud - 0.55f) * (tRain - 0.2f) * 1.8f;

    s.cloud = follow(s.cloud, clampf(tCloud, 0.f, 1.f), dt, 0.12f);
    s.rain  = follow(s.rain,  clampf(tRain, 0.f, 1.f), dt, 0.18f);
    s.dust  = follow(s.dust,  clampf(tDust, 0.f, 1.f), dt, 0.25f);
    s.snow  = follow(s.snow,  clampf(tSnow, 0.f, 1.f), dt, 0.15f);
    s.fog   = follow(s.fog,   clampf(tFog, 0.f, 1.f), dt, 0.10f);
    s.storm = follow(s.storm, clampf(tStorm, 0.f, 1.f), dt, 0.08f);
    s.wind  = follow(s.wind,  0.2f + s.storm * 0.6f + s.cloud * 0.2f, dt, 0.15f);

    if (s.rain > 0.2f) { s.rainAgo = 0.f; s.wet = follow(s.wet, 1.f, dt, 0.4f); }
    else { s.rainAgo += dt; s.wet = follow(s.wet, 0.f, dt, 0.05f); }

    // Lightning
    s.flash = fmaxf(0.f, s.flash - dt * 4.f);
    s.boltAge += dt;
    if (s.storm > 0.35f && s.boltAge > 2.5f + hashf((uint32_t)(t * 3.f) + worldSeed()) * 8.f) {
        if (hashf((uint32_t)(t * 17.f) + worldSeed() + 9) < s.storm * 0.08f) {
            s.flash = 0.7f + 0.3f * hashf((uint32_t)(t * 11.f));
            s.boltAge = 0.f;
        }
    }

    s.light = s.dayLight * (1.f - s.fog * 0.5f) * (1.f - s.cloud * 0.4f) * (1.f - s.storm * 0.25f);
    if (s.flash > 0.1f) s.light = fminf(1.f, s.light + s.flash * 0.5f);

    // God-ray strength: low sun, clear-ish air
    float low = s.sunElev > -0.05f ? clampf(1.f - s.sunElev / 0.45f, 0.f, 1.f) : 0.f;
    float clearSky = clampf(1.f - (s.cloud - 0.4f) * 2.2f, 0.f, 1.f) * (1.f - s.rain * 0.9f);
    s.beams = low * clearSky * (0.5f + 0.5f * s.dust);

    // Rainbow after rain, low sun
    float rbT = (s.rainAgo > 3.f && s.rainAgo < 120.f && s.wet > 0.2f && s.rain < 0.12f &&
                 s.cloud < 0.8f && s.sunElev > 0.0f && s.sunElev < 0.4f) ? 1.f : 0.f;
    s.rainbow = follow(s.rainbow, rbT, dt, 3.f);

    s.dustTrail = clampf(slip * 1.5f + s.dust * 0.55f, 0.f, 1.f);
    if (speed > 22.f && grip < 0.55f) s.sparks = fminf(s.sparks + dt * 2.f, 1.f);
    else s.sparks = fmaxf(0.f, s.sparks - dt * 3.f);

    if (speed < 1.5f) s.stillFor += dt; else s.stillFor = 0.f;
    if (speed > 12.f && throttle > 0.3f && throttle < 0.7f) s.cruiseFor += dt;
    else s.cruiseFor = 0.f;

    s.daySeed = worldSeed();
}

}
