// Size over speed here: this code runs per object or per frame, not per pixel or per sample, and the app shares
// flash with others (see README, "Footprint").
#pragma GCC optimize("Os")
#include "haptics.h"
#include <M5Unified.h>
#include <math.h>

namespace hx {

struct Key { uint8_t level; uint16_t ms; };

static const Key G_BUMP[]    = {{235, 30}, {150, 50}, {200, 25}, {60, 90}, {0, 120}};      // a clank, then the hull rings
static const Key G_SWELL[]   = {{20, 500}, {85, 900}, {30, 900}, {0, 900}};
static const Key G_LUBDUB[]  = {{175, 45}, {30, 90}, {125, 40}, {0, 220}};
static const Key G_MUTE[]    = {{120, 30}, {0, 70}, {120, 30}, {0, 40}};
static const Key G_UNMUTE[]  = {{40, 60}, {150, 50}, {0, 60}};
static const Key G_SONAR[]   = {{200, 18}, {90, 60}, {30, 160}, {0, 120}};
static const Key G_ECHO[]    = {{110, 20}, {40, 80}, {0, 80}};
static const Key G_CREAK[]   = {{40, 120}, {95, 160}, {60, 90}, {110, 70}, {0, 200}};
static const Key G_THUNDER[] = {{40, 200}, {110, 200}, {60, 260}, {90, 200}, {30, 400}, {0, 500}};
static const Key G_SCANOK[]  = {{140, 30}, {0, 60}, {200, 40}, {0, 50}};
static const Key G_SCANNEW[] = {{120, 30}, {0, 50}, {170, 30}, {0, 50}, {230, 60}, {80, 200}, {0, 200}};
static const Key G_ERUPT[]   = {{60, 300}, {200, 400}, {140, 500}, {220, 300}, {90, 900}, {0, 900}};

struct Gesture { const Key* k; int n, i; uint32_t t0; uint8_t from; float scale; bool on; };
static Gesture s_g[3];
static uint32_t s_tapUntil = 0;
static uint8_t s_tap = 0;
static float s_engT = 0.f, s_eng = 0.f, s_engPh = 0.f, s_engRate = 6.f;
static float s_rumT = 0.f, s_rum = 0.f, s_rumPh = 0.f, s_rumJit = 0.f;
static uint32_t s_rumJitAt = 0, s_lastWrite = 0;
static uint8_t s_now = 255;

static void vib(uint8_t v) {
    uint32_t now = millis();
    if (v == s_now) return;
    if (v != 0 && s_now != 0 && (v > s_now ? v - s_now : s_now - v) < 4) return;
    if (v != 0 && now - s_lastWrite < 4) return;
    s_now = v;
    s_lastWrite = now;
    M5.Power.setVibration(v);
}

static void gesture(const Key* k, int n, float scale) {
    Gesture* slot = &s_g[0];
    for (Gesture& g : s_g) if (!g.on) { slot = &g; break; }
    *slot = {k, n, 0, millis(), 0, scale, true};
}
static void tap(uint8_t level, uint16_t ms) {
    uint32_t now = millis();
    if (level >= s_tap || now >= s_tapUntil) { s_tap = level; s_tapUntil = now + ms; }
}
#define G(arr) arr, (int)(sizeof(arr) / sizeof(arr[0]))

void begin() { s_now = 255; vib(0); for (Gesture& g : s_g) g.on = false; }
void pop(float level, float seconds) { tap((uint8_t)(60 + 180 * fminf(1.f, level)), (uint16_t)(seconds * 1000.f + 12.f)); }
void bump(float strength) { gesture(G(G_BUMP), 0.45f + 0.55f * fminf(1.f, strength)); }
void kick(float level) { tap((uint8_t)(70 + 90 * level), 38); }
void swell() { gesture(G(G_SWELL), 1.f); }
void heartbeat() { gesture(G(G_LUBDUB), 1.f); }
void confirmMute(bool muted) { if (muted) gesture(G(G_MUTE), 1.f); else gesture(G(G_UNMUTE), 1.f); }
void horn() { gesture(G(G_SONAR), 1.f); }
void echo(float strength) { gesture(G(G_ECHO), fmaxf(0.3f, fminf(1.f, strength))); }
void creak(float depthK) { gesture(G(G_CREAK), 0.4f + 0.6f * fminf(1.f, depthK)); }
void thunder(float power) { gesture(G(G_THUNDER), 0.3f + 0.5f * power); }
void scanTick(float level) { tap((uint8_t)(50 + 70 * level), 18); }
void scanDone(bool fresh) { if (fresh) gesture(G(G_SCANNEW), 1.f); else gesture(G(G_SCANOK), 0.8f); }
void eruption(float power) { gesture(G(G_ERUPT), 0.5f + 0.5f * power); }
// The screw: a low hum that pulses with the blades, rising in rate with speed. Felt mostly at flank.
void engine(float throttle, float speed) {
    s_engT = throttle > 0.02f ? 0.05f + 0.12f * fminf(1.f, speed / 10.f) + (throttle > 0.75f ? (throttle - 0.75f) * 0.5f : 0.f) : 0.f;
    s_engRate = 3.f + speed * 1.4f;
}
void rumble(float amount) { s_rumT = fminf(1.f, amount); }

void update(float dt) {
    uint32_t now = millis();
    float out = 0.f;
    if (now < s_tapUntil) out = s_tap / 255.f;
    s_eng += (s_engT - s_eng) * fminf(1.f, dt * 2.f);
    if (s_eng > 0.01f) {
        s_engPh += dt * s_engRate;
        float p = 0.5f + 0.5f * sinf(s_engPh * 6.2831853f);
        out = fmaxf(out, s_eng * (0.7f + 0.3f * p));
    }
    s_rum += (s_rumT - s_rum) * fminf(1.f, dt * 3.f);
    if (s_rum > 0.01f) {
        s_rumPh += dt * 4.f;
        if (now >= s_rumJitAt) { s_rumJit = (esp_random() & 255) / 255.f; s_rumJitAt = now + 50; }
        float p = 0.5f + 0.5f * sinf(s_rumPh * 6.2831853f);
        out = fmaxf(out, s_rum * (0.5f + 0.25f * p + 0.25f * s_rumJit));
    }
    for (Gesture& g : s_g) {
        if (!g.on) continue;
        uint32_t e = now - g.t0;
        while (g.i < g.n && e >= g.k[g.i].ms) { e -= g.k[g.i].ms; g.from = g.k[g.i].level; g.t0 += g.k[g.i].ms; g.i++; }
        if (g.i >= g.n) { g.on = false; continue; }
        float u = (float)e / (float)(g.k[g.i].ms + 1);
        out = fmaxf(out, (g.from + (g.k[g.i].level - g.from) * u) / 255.f * g.scale);
    }
    vib(out < 0.03f ? 0 : (uint8_t)fminf(255.f, 55.f + out * 200.f));
}
}
