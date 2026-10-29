#pragma once
// Small, cheap, stable DSP building blocks for a 24 kHz mono engine on an ESP32 FPU.
#include <stdint.h>
#include <stddef.h>
#include <math.h>

namespace dsp {

constexpr float SR = 24000.f;
constexpr float INV_SR = 1.f / 24000.f;
constexpr float TWO_PI_F = 6.28318531f;

extern float* SINE;                       // 1025 entries (made by init())
void init();
// Runtime buffer: internal RAM if there's room, else PSRAM; zeroed. Keeps big buffers out of .bss.
void* allocFast(size_t bytes);

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

struct Rng {
    uint32_t s = 0x1234567u;
    inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float bi() { return (int32_t)next() * (1.f / 2147483648.f); }   // -1..1
    inline float uni() { return (next() >> 8) * (1.f / 16777216.f); }       // 0..1
};

// 32-bit phase accumulator oscillators
inline uint32_t inc(float hz) { return (uint32_t)(int64_t)(hz * (4294967296.f * INV_SR)); }
inline float sine(uint32_t ph) {
    uint32_t i = ph >> 22;
    float f = (ph & 0x3FFFFFu) * (1.f / 4194304.f);
    return SINE[i] + (SINE[i + 1] - SINE[i]) * f;
}
inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}
struct Saw {
    uint32_t ph = 0, dph = 0;
    inline float tick() {
        float t = ph * (1.f / 4294967296.f), dt = dph * (1.f / 4294967296.f);
        float v = 2.f * t - 1.f - blep(t, dt);
        ph += dph;
        return v;
    }
};

inline float midiHz(float m) { return 440.f * powf(2.f, (m - 69.f) / 12.f); }
inline float coef(float hz) { return 1.f - expf(-TWO_PI_F * hz * INV_SR); }   // one-pole smoothing

// Topology-preserving state variable filter (Simper). Set coefficients at block rate.
struct SVF {
    float ic1 = 0, ic2 = 0, a1 = 1, a2 = 0, a3 = 0, k = 1;
    void set(float fc, float q) {
        fc = clampf(fc, 20.f, SR * 0.45f);
        float g = tanf(3.14159265f * fc * INV_SR);
        k = 1.f / q;
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    inline void tick(float v0, float& lp, float& bp) {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        lp = v2; bp = v1;
    }
    inline float lp(float x) { float l, b; tick(x, l, b); return l; }
    inline float bp(float x) { float l, b; tick(x, l, b); return b; }
    inline float hp(float x) { float l, b; tick(x, l, b); return x - k * b - l; }
    void reset() { ic1 = ic2 = 0; }
};

struct Pink {
    float b0 = 0, b1 = 0, b2 = 0;
    inline float tick(float w) {
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        return (b0 + b1 + b2 + w * 0.1848f) * 0.22f;
    }
};

inline float softclip(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// 4-line feedback delay network with Householder mixing and damping, fed through two diffusers.
struct Reverb {
    float* line[4] = {nullptr, nullptr, nullptr, nullptr};
    int len[4] = {1103, 1427, 1801, 2263}, pos[4] = {0, 0, 0, 0};
    float lp[4] = {0, 0, 0, 0};
    float* ap[2] = {nullptr, nullptr};
    int apLen[2] = {211, 347}, apPos[2] = {0, 0};
    float fb = 0.83f, damp = 0.35f;
    bool ok = false;
    bool begin();
    inline float tick(float in) {
        if (!ok) return 0.f;
        for (int j = 0; j < 2; j++) {
            float d = ap[j][apPos[j]];
            float w = in + d * 0.6f;
            ap[j][apPos[j]] = w;
            in = d - w * 0.6f;
            if (++apPos[j] >= apLen[j]) apPos[j] = 0;
        }
        float d0 = line[0][pos[0]], d1 = line[1][pos[1]], d2 = line[2][pos[2]], d3 = line[3][pos[3]];
        float h = (d0 + d1 + d2 + d3) * 0.5f;
        float o[4] = {d0 - h, d1 - h, d2 - h, d3 - h};
        for (int j = 0; j < 4; j++) {
            lp[j] += damp * (o[j] - lp[j]);
            line[j][pos[j]] = lp[j] * fb + in * ((j & 1) ? 0.5f : -0.5f);
            if (++pos[j] >= len[j]) pos[j] = 0;
        }
        return (d0 - d1 + d2 - d3) * 0.5f;
    }
};

// Tape echo: fractional read with slow wow, band-limited and saturated feedback (darkens each repeat).
struct DubDelay {
    int16_t* buf = nullptr;
    int size = 0, w = 0;
    float time = 11250.f, target = 11250.f, fb = 0.6f, wow = 0.f, wowPh = 0.f;
    float lpz = 0.f, hpz = 0.f, lpk = 0.4f, hpk = 0.06f;
    bool begin(int samples);
    inline float tick(float in) {
        if (!buf) return 0.f;
        time += (target - time) * 0.00005f;
        wowPh += 0.35f * INV_SR;
        if (wowPh > 1.f) wowPh -= 1.f;
        float d = time * (1.f + 0.0016f * sine((uint32_t)(wowPh * 4294967296.f)));
        if (d > size - 2) d = (float)(size - 2);       // never read past the buffer, whatever the tempo
        float rp = (float)w - d;
        while (rp < 0.f) rp += size;
        int i0 = (int)rp;
        float f = rp - i0;
        int i1 = i0 + 1 >= size ? 0 : i0 + 1;
        float y = (buf[i0] + (buf[i1] - buf[i0]) * f) * (1.f / 16384.f);
        lpz += lpk * (y - lpz) + 1e-18f;              // the tiny offset keeps a fading echo out of denormals (slow on every FPU)
        hpz += hpk * (lpz - hpz);
        float fbs = softclip((lpz - hpz) * fb * 1.15f);
        float wv = clampf(in + fbs, -1.9f, 1.9f);
        buf[w] = (int16_t)(wv * 16384.f);
        if (++w >= size) w = 0;
        return y;
    }
};

}
