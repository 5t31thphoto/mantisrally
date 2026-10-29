#include "universe.h"
#include <math.h>
#ifndef HOST
#include <esp_attr.h>
#define MP_HOT IRAM_ATTR          // the noise runs thousands of times a frame: keep it in instruction RAM
#else
#define MP_HOT
#endif

namespace mantis {

static uint32_t s_seed = 0x5EE49A3Fu;

void universeInit(uint32_t seed) { s_seed = seed ? hash32(seed) : 0x5EE49A3Fu; }
uint32_t worldSeed() { return s_seed; }

MP_HOT float vnoiseP(float u, float v, int P, uint32_t salt) {
    float fu = floorf(u), fv = floorf(v);
    int iu = (int)fu, iv = (int)fv;
    float tu = u - fu, tv = v - fv;
    tu = tu * tu * (3.f - 2.f * tu);
    tv = tv * tv * (3.f - 2.f * tv);
    int x0 = iu % P; if (x0 < 0) x0 += P;
    int x1 = x0 + 1; if (x1 == P) x1 = 0;
    const uint32_t r0 = hash32((uint32_t)iv * 0x85EBCA77u ^ s_seed ^ salt);
    const uint32_t r1 = hash32((uint32_t)(iv + 1) * 0x85EBCA77u ^ s_seed ^ salt);
    float a = (hash32((uint32_t)x0 * 0x9E3779B1u ^ r0) >> 8) * (1.0f / 16777216.0f);
    float b = (hash32((uint32_t)x1 * 0x9E3779B1u ^ r0) >> 8) * (1.0f / 16777216.0f);
    float c = (hash32((uint32_t)x0 * 0x9E3779B1u ^ r1) >> 8) * (1.0f / 16777216.0f);
    float d = (hash32((uint32_t)x1 * 0x9E3779B1u ^ r1) >> 8) * (1.0f / 16777216.0f);
    float ab = a + (b - a) * tu, cd = c + (d - c) * tu;
    return ab + (cd - ab) * tv;
}

MP_HOT float fbmP(float u, float v, int P, int oct, uint32_t salt) {
    float sum = 0.f, amp = 0.5f, norm = 0.f;
    for (int i = 0; i < oct; i++) {
        sum += amp * vnoiseP(u, v, P, salt + (uint32_t)i * 0x632BE5ABu);
        norm += amp;
        u *= 2.f; v *= 2.f; P *= 2; amp *= 0.5f;
    }
    return sum / norm;
}

MP_HOT float vnoise2(float x, float y, uint32_t salt) {
    float fx = floorf(x), fy = floorf(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    const uint32_t r0 = hash32((uint32_t)iy * 0x85EBCA77u ^ s_seed ^ salt);
    const uint32_t r1 = hash32((uint32_t)(iy + 1) * 0x85EBCA77u ^ s_seed ^ salt);
    float a = (hash32((uint32_t)ix * 0x9E3779B1u ^ r0) >> 8) * (1.0f / 16777216.0f);
    float b = (hash32((uint32_t)(ix + 1) * 0x9E3779B1u ^ r0) >> 8) * (1.0f / 16777216.0f);
    float c = (hash32((uint32_t)ix * 0x9E3779B1u ^ r1) >> 8) * (1.0f / 16777216.0f);
    float d = (hash32((uint32_t)(ix + 1) * 0x9E3779B1u ^ r1) >> 8) * (1.0f / 16777216.0f);
    float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

MP_HOT float fbm2(float x, float y, int oct, uint32_t salt) {
    float sum = 0.f, amp = 0.5f, norm = 0.f;
    for (int i = 0; i < oct; i++) {
        sum += amp * vnoise2(x, y, salt + (uint32_t)i * 0x632BE5ABu);
        norm += amp;
        float nx = x * 1.6f - y * 1.2f, ny = x * 1.2f + y * 1.6f;          // rotate each octave: no grid-aligned streaks
        x = nx; y = ny; amp *= 0.5f;
    }
    return sum / norm;
}

static float s_dayOffset = 0.36f;      // the stage begins in clear mid-morning light
float dayClock(float t, float distance) { return s_dayOffset + t * (1.f / 600.f) + distance * 0.0002f + 10.f; }
void setTimeOfDay(float tod, float t, float distance) {
    float c = dayClock(t, distance);
    s_dayOffset += tod - (c - floorf(c));
}

float sunPathElevation(float tod) {
    float u = (tod - 0.245f) / 0.49f;                       // 0 at sunrise, 1 at sunset
    if (u >= 0.f && u <= 1.f) return 1.05f * sinf(3.14159265f * u);   // up to ~60 degrees at noon
    float d = u < 0.f ? -u : u - 1.f;
    return -0.95f * sinf(3.14159265f * fminf(d, 0.5f));
}

}
