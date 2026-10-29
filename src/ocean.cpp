// Engine / road bed — synthesized, aggressive.
// Same bus interface the audio task expects.
#include <math.h>
#include "audio_int.h"
#include "audio.h"
#include "universe.h"
#include <string.h>

namespace mantis {
namespace snd {

static float s_rpm = 1200.f;
static float s_phase[6] = {0};
static float s_gearPhase = 0.f;
static float s_popT = 0.f;
static float s_lastThrottle = 0.f;
static float s_screech = 0.f;
static int   s_gear = 1;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float softsat(float x) {
    // soft clip
    return x / (1.f + fabsf(x) * 0.7f);
}

void oceanInit() {
    s_rpm = 1200.f;
    for (int i = 0; i < 6; i++) s_phase[i] = 0.f;
    s_gearPhase = 0.f; s_popT = 0.f; s_lastThrottle = 0.f;
    s_screech = 0.f; s_gear = 1;
}

void oceanBlock(Bus& b, int n, const Scene& s) {
    float throttle = s.throttle;
    float speed = s.speed;          // m/s
    float slip = s.slip;
    float rough = s.roughness;
    float rain = s.rain;
    float hb = s.handbrake;

    // Target RPM from speed + throttle (manual-ish 5 speed)
    float speedKmh = speed * 3.6f;
    // gear by speed
    int wantGear = 1;
    if (speedKmh > 25.f) wantGear = 2;
    if (speedKmh > 45.f) wantGear = 3;
    if (speedKmh > 75.f) wantGear = 4;
    if (speedKmh > 110.f) wantGear = 5;
    if (wantGear != s_gear) {
        s_gear = wantGear;
        s_popT = 0.08f;   // shift blip
    }
    static const float RATIO[6] = {0, 4.2f, 2.6f, 1.7f, 1.2f, 0.9f};
    float targetRpm = 900.f + speedKmh * 28.f * RATIO[s_gear] / 2.6f;
    targetRpm += throttle * 1800.f;
    if (throttle < 0.05f && speed > 5.f) targetRpm *= 0.82f; // overrun drop
    targetRpm = clampf(targetRpm, 800.f, 7800.f);

    float rpmA = 1.f - expf(-0.12f); // ~per block smoothing later per sample
    // lift-off crackle
    if (s_lastThrottle > 0.35f && throttle < 0.12f && s_rpm > 3200.f)
        s_popT = fmaxf(s_popT, 0.18f + (s_rpm - 3200.f) / 8000.f);
    s_lastThrottle = throttle;

    float load = clampf(throttle * 0.7f + speed / 50.f * 0.3f, 0.f, 1.f);
    s_screech += ((slip * 0.85f + hb * 0.4f) - s_screech) * 0.15f;

    for (int i = 0; i < n; i++) {
        s_rpm += (targetRpm - s_rpm) * 0.0009f;

        float hz = s_rpm / 60.f;   // fundamental cycles/sec
        // harmonics: 0.5, 1, 2, 3, 4, 5.5 — exhaust-ish stack
        static const float HMULT[6] = {0.5f, 1.f, 2.f, 3.f, 4.f, 5.5f};
        static const float HAMP[6]  = {0.35f, 1.0f, 0.55f, 0.28f, 0.18f, 0.12f};
        float eng = 0.f;
        for (int h = 0; h < 6; h++) {
            s_phase[h] += (hz * HMULT[h]) / (float)AUDIO_SR;
            if (s_phase[h] > 1.f) s_phase[h] -= (float)(int)s_phase[h];
            // slight wavefold for aggression
            float ph = s_phase[h] * kTau;
            float wave = sinf(ph);
            wave += 0.25f * sinf(ph * 2.f);
            // pulse width varies with load
            float pw = 0.15f + load * 0.2f;
            if (s_phase[h] < pw) wave *= 1.15f;
            eng += wave * HAMP[h];
        }
        eng *= 0.18f + load * 0.22f;
        // high-rpm scream
        if (s_rpm > 5000.f) {
            float scream = (s_rpm - 5000.f) / 2800.f;
            eng += sinf(s_phase[1] * kTau * 7.f) * scream * 0.08f;
        }
        // turbo spool under load
        if (load > 0.45f && s_rpm > 2800.f) {
            float tw = (load - 0.45f) * clampf((s_rpm - 2800.f) / 3000.f, 0.f, 1.f);
            eng += sinf(s_phase[4] * kTau * 1.7f) * tw * 0.06f;
            eng += rng.bi() * tw * 0.025f;
        }
        eng = softsat(eng);

        // overrun pops
        float pop = 0.f;
        if (s_popT > 0.f) {
            s_popT -= 1.f / (float)AUDIO_SR;
            if ((rng.next() & 0x3f) == 0)
                pop = (rng.bi() * 0.75f + 0.25f) * 0.55f * clampf(s_popT * 6.f, 0.f, 1.f);
        }

        // tire screech / gravel
        float nse = rng.bi();
        float gravel = nse * (0.03f + rough * 0.10f) * clampf(speed / 22.f, 0.f, 1.f);
        float screech = 0.f;
        if (s_screech > 0.08f) {
            // band-ish noise by modulating
            float mod = sinf(s_phase[2] * kTau * 3.7f);
            screech = nse * mod * s_screech * 0.22f;
        }
        float wind = nse * 0.025f * clampf(speed / 40.f, 0.f, 1.f);
        float wet = nse * 0.02f * rain;

        float dry = eng + pop + gravel + screech + wind + wet;
        b.dry[i] += dry * soloGain(TAG_ENGINE);
        b.rev[i] += (eng * 0.35f + gravel * 0.5f) * soloGain(TAG_SEA);
        b.dly[i] += pop * 0.4f;
    }
}

void oceanBump(float strength) {
    s_popT = fmaxf(s_popT, 0.05f + strength * 0.15f);
}
void oceanThunder(float, float) {}
void oceanCritter(uint8_t, float) {}
void oceanUi(uint8_t) {}
void oceanSonar(float strength, float) {
    // horn: short aggressive pulse via pop
    s_popT = fmaxf(s_popT, 0.12f + strength * 0.1f);
}
void oceanMoment(uint8_t) {}

}
}
