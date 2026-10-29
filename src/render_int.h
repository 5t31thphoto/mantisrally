#pragma once
// Renderer internals — palette ramps, frame state, put/z helpers.
// Lighting model follows MantisPlanet: 16×16 ramps, fog twins, ordered dither.
#include "render.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {
namespace rr {

constexpr int W = FB_W, H = FB_H;
constexpr float FOV = 1.10f;
constexpr float F = (W * 0.5f) / 0.61310f;

enum : uint8_t {
    R_SKY = 0, R_CLOUD = 16, R_SUN = 32, R_RIDGE = 48,
    R_ROAD = 64, R_VERGE = 80, R_ROCK = 96, R_SNOW = 112,
    R_HUD = 128, R_CAR = 144, R_DUST = 160, R_ACCENT = 176
};

extern uint8_t fb[W * H];
extern uint8_t* zb;
extern float g_cx, g_cy, g_cz, g_cY, g_sY, g_t, g_vis, g_pitch, g_yaw;
extern int g_hor, g_pan;
extern float g_alpha;
extern uint8_t g_zc;

static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
inline float dith(int x, int y) { return (BAYER[(((y - g_hor) & 3) << 2) | ((x + g_pan) & 3)] + 0.5f) * (1.f / 16.f); }
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
inline float smoothstep(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
inline float fsin(float x) {
    x *= 0.15915494f; x -= floorf(x + 0.5f);
    float y = 8.f * x - 16.f * x * fabsf(x);
    return 0.225f * (y * fabsf(y) - y) + y;
}
inline float fcos(float x) { return fsin(x + 1.5707963f); }
inline uint8_t zcode(float d) { float c = sqrtf(fmaxf(d, 0.f)) * 22.f; return (uint8_t)(c > 254.f ? 254.f : c); }
inline float fogOf(float d) { return 1.f - expf(-d / g_vis); }

inline void put(int x, int y, uint8_t c) {
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) {
        int i = y * W + x;
        if (zb[i] < g_zc) return;
        if (g_alpha < 1.f && dith(x, y) >= g_alpha) return;
        fb[i] = c;
    }
}

}
}
