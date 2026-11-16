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
    if (v != 0 && s_now != 0 && (v > s_now ? v - s_now : s_now - v) < 2) return;   // fine steps (the clock runs it often)
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

// ---- the surface under you: a texture per material. The motor can't do pitch, so each is a rhythm and a grain:
//  tarmac   a faint fine hum              gravel  a dense random crackle, now and then a stone
//  dirt     a soft rolling undulation     mud     slow heavy squelch
//  snow     a soft crunch                 sand    a smooth heavy drag
//  grass    a soft patter                 rock    sharp knocks
//  timber deck: the planks' rattle under the tyres (capped where the motor blurs) and a knock at every bent (3 m)
//  concrete bridge: smooth, and ka-thunk at each expansion joint (15 m): front axle, then rear
static uint8_t s_sf = 0, s_dk = 0; static float s_sp = 0.f, s_sl = 0.f; static bool s_air = false;
static float s_txPh = 0.f, s_txJ = 0.f, s_txJT = 0.f, s_dist = 0.f, s_rearT = -1.f;
void surface(uint8_t surf, uint8_t deck, float speed, float slip, bool air) { s_sf = surf; s_dk = deck; s_sp = speed; s_sl = slip; s_air = air; }
static float texture(float dt) {
    if (s_air || s_sp < 0.5f) { s_dist = 0.f; return 0.f; }
    const float sk = fminf(1.f, s_sp / 20.f);
    float out = 0.f;
    s_txJT -= dt;
    auto jitter = [&](float every, float lo, float hi) { if (s_txJT <= 0.f) { s_txJT = every * (0.6f + 0.8f * (esp_random() & 255) / 255.f); s_txJ = lo + (hi - lo) * (esp_random() & 255) / 255.f; } return s_txJ; };
    if (s_dk == 1) {                                                    // timber planks
        const float rate = fminf(s_sp / 0.32f, 28.f);
        s_txPh += dt * rate;
        out = (0.16f + 0.14f * ((int)(s_txPh) & 1)) * sk;               // board, gap, board...
        const float before = s_dist; s_dist += s_sp * dt;
        if (floorf(s_dist / 3.f) != floorf(before / 3.f)) tap((uint8_t)(120 + 80 * sk), 26);   // the bent
    } else if (s_dk == 2) {                                             // concrete bridge
        out = 0.06f * sk;
        const float before = s_dist; s_dist += s_sp * dt;
        if (floorf(s_dist / 15.f) != floorf(before / 15.f)) { tap((uint8_t)(170 + 70 * sk), 32); s_rearT = 2.6f / fmaxf(s_sp, 1.f); }
    } else {
        s_dist = 0.f;
        switch (s_sf) {
            case 0: out = jitter(0.02f, 0.02f, 0.07f) * sk; break;                                    // tarmac
            case 1: out = jitter(0.018f, 0.12f, 0.42f) * sk; if ((esp_random() & 1023) < (uint32_t)(dt * 2000.f * sk)) tap(200, 18); break;   // gravel
            case 2: s_txPh += dt * 6.f; out = (0.16f + 0.1f * sinf(s_txPh * 6.2831853f)) * sk + jitter(0.06f, 0.f, 0.08f) * sk; break;  // dirt
            case 3: s_txPh += dt * 2.4f; out = (0.18f + 0.16f * sinf(s_txPh * 6.2831853f)) * sk; break;  // mud
            case 4: out = jitter(0.035f, 0.05f, 0.2f) * sk; break;                                     // snow
            case 5: s_txPh += dt * 1.4f; out = (0.2f + 0.05f * sinf(s_txPh * 6.2831853f)) * sk; break;   // sand
            case 6: out = jitter(0.045f, 0.04f, 0.16f) * sk; break;                                    // grass
            default: out = jitter(0.05f, 0.05f, 0.15f) * sk; if ((esp_random() & 1023) < (uint32_t)(dt * 6000.f * sk)) tap(230, 22); break;   // rock
        }
    }
    if (s_rearT >= 0.f) { s_rearT -= dt; if (s_rearT < 0.f) tap((uint8_t)(150 + 60 * sk), 30); }
    return fminf(1.f, out + s_sl * 0.3f);
}

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
    out = fmaxf(out, texture(dt));
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
