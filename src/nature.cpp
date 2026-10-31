// Ported from MantisPlanet (nature.cpp): wind, leaves, water, surf, rain, birds, insects, frogs, thunder.
// The rally game feeds it a NatScene built from its own scene each audio block (audio.cpp).
#include "audio_int.h"
#include "audio.h"
#include "universe.h"
#include <string.h>

namespace mantis {
namespace snd {

using dsp::clampf;
static inline float smooth(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static inline float bumpf(float x, float a, float b, float c, float d) { return smooth(a, b, x) * (1.f - smooth(c, d, x)); }
static inline float wrapPi(float a) { return a - mantis::kTau * floorf((a + 3.14159265f) / mantis::kTau); }

// ================= one-shot pool: filtered noise grains and decaying pings =================
struct Grain {
    bool on = false;
    int delay = 0, att = 0, age = 0;
    float env = 0, dec = 0, amp = 0, rev = 0.1f;
    float fc0 = 1000, fc1 = 1000, q = 1, sweep = 0;      // sweep: 0..1 per block progress
    int sweepN = 1;
    uint8_t mode = 0;                                     // 0 BP, 1 LP
    dsp::SVF f;
};
struct Ping {
    bool on = false;
    int delay = 0;
    uint32_t ph = 0;
    float dph = 0, mul = 1, env = 0, dec = 0, amp = 0, rev = 0.15f;
};
static Grain s_gr[40];
static Ping s_pg[16];

static inline float decFor(float tauSec) { return expf(-1.f / (tauSec * dsp::SR)); }

static void grain(float delaySec, uint8_t mode, float fc0, float fc1, float q, float attSec, float tauSec, float amp, float rev = 0.12f) {
    for (Grain& g : s_gr) {
        if (g.on) continue;
        g.on = true;
        g.delay = (int)(delaySec * dsp::SR);
        g.att = (int)(attSec * dsp::SR) + 1;
        g.age = 0;
        g.env = 0.f;
        g.dec = decFor(tauSec);
        g.amp = amp;
        g.rev = rev;
        g.mode = mode;
        g.fc0 = fc0; g.fc1 = fc1; g.q = q;
        g.sweepN = (int)((attSec + tauSec * 2.5f) * dsp::SR / BLOCK) + 1;
        g.sweep = 0.f;
        g.f.reset();
        g.f.set(fc0, q);
        return;
    }
}
static void ping(float delaySec, float hz, float endMul, float sweepSec, float tauSec, float amp, float rev = 0.15f) {
    for (Ping& p : s_pg) {
        if (p.on) continue;
        p.on = true;
        p.delay = (int)(delaySec * dsp::SR);
        p.ph = 0;
        p.dph = hz * (4294967296.f / dsp::SR);
        p.mul = powf(endMul, 1.f / (sweepSec * dsp::SR + 1.f));
        p.env = amp;
        p.amp = amp;
        p.dec = decFor(tauSec);
        p.rev = rev;
        return;
    }
}

static void shotsBlock(Bus& b, int n) {
    for (Grain& g : s_gr) {
        if (!g.on) continue;
        if (g.delay >= n) { g.delay -= n; continue; }
        if (g.fc0 != g.fc1) {
            g.sweep = fminf(1.f, g.sweep + 1.f / g.sweepN);
            g.f.set(g.fc0 * powf(g.fc1 / g.fc0, g.sweep), g.q);
        }
        for (int i = g.delay; i < n; i++) {
            float e = g.age < g.att ? g.amp * (float)g.age / g.att : (g.env *= g.dec);
            if (g.age == g.att) g.env = g.amp;
            g.age++;
            float l, bp;
            g.f.tick(rng.bi(), l, bp);
            float v = (g.mode ? l : bp) * e;
            b.dry[i] += v;
            b.rev[i] += v * g.rev;
        }
        g.delay = 0;
        if (g.age > g.att && g.env < 0.0005f) g.on = false;
    }
    for (Ping& p : s_pg) {
        if (!p.on) continue;
        if (p.delay >= n) { p.delay -= n; continue; }
        for (int i = p.delay; i < n; i++) {
            float v = dsp::sine(p.ph) * p.env;
            p.ph += (uint32_t)p.dph;
            p.dph *= p.mul;
            p.env *= p.dec;
            b.dry[i] += v;
            b.rev[i] += v * p.rev;
        }
        p.delay = 0;
        if (p.env < 0.0005f) p.on = false;
    }
}

// ================= bumps, critters =================
// ---- impacts (the rally) ----
void natureImpact(uint8_t kind, float st) {
    float a = clampf(st, 0.15f, 1.f);
    switch (kind) {
        case IMP_TREE:                                        // a hollow wooden knock, the trunk shudders, leaves rain down
            grain(0.f, 1, 420.f, 160.f, 2.f, 0.002f, 0.09f, 0.42f * a, 0.25f);
            ping(0.f, 210.f, 0.82f, 0.08f, 0.14f, 0.28f * a, 0.3f);
            ping(0.004f, 345.f, 0.9f, 0.05f, 0.09f, 0.14f * a, 0.3f);
            grain(0.03f, 0, 2600.f, 1900.f, 0.8f, 0.03f, 0.18f, 0.10f * a, 0.35f);
            for (int i = 0; i < 4; i++) grain(0.2f + i * (0.09f + rng.uni() * 0.1f), 0, 2300.f + rng.uni() * 900.f, 1800.f, 0.9f, 0.015f, 0.05f, (0.05f - i * 0.008f) * a, 0.35f);
            break;
        case IMP_ROCK:                                        // a hard crack, then steel scraping on stone
            grain(0.f, 0, 3800.f, 2400.f, 1.4f, 0.f, 0.012f, 0.5f * a, 0.15f);
            grain(0.f, 1, 700.f, 250.f, 1.5f, 0.002f, 0.07f, 0.32f * a, 0.2f);
            ping(0.f, 1650.f, 0.97f, 0.1f, 0.12f, 0.08f * a, 0.25f);
            grain(0.02f, 0, 2700.f, 2100.f, 3.f, 0.03f, 0.22f, 0.14f * a, 0.2f);
            break;
        case IMP_DEER:                                        // a comedic thud... and a wet splat... and a drip or two
            grain(0.f, 1, 520.f, 140.f, 1.2f, 0.003f, 0.08f, 0.5f, 0.2f);
            ping(0.f, 150.f, 0.55f, 0.09f, 0.1f, 0.36f, 0.2f);
            grain(0.06f, 1, 2600.f, 260.f, 1.4f, 0.006f, 0.16f, 0.32f, 0.3f);
            grain(0.09f, 0, 1300.f, 500.f, 2.5f, 0.01f, 0.09f, 0.16f, 0.3f);
            ping(0.38f, 1150.f, 1.25f, 0.05f, 0.05f, 0.05f, 0.4f);
            ping(0.61f, 980.f, 1.3f, 0.05f, 0.05f, 0.035f, 0.4f);
            break;
        case IMP_CRUNCH: {                                    // landing too fast: panels fold, the floor slams, bits rattle off
            grain(0.f, 1, 380.f, 90.f, 1.2f, 0.002f, 0.14f, 0.55f * a, 0.2f);
            int n = 6 + (int)(a * 10.f);
            float t = 0.f;
            for (int i = 0; i < n; i++) {
                t += 0.008f + rng.uni() * 0.03f * (1.f + i * 0.15f);
                grain(t, 0, 1200.f + rng.uni() * 2200.f, 900.f + rng.uni() * 900.f, 2.f + rng.uni() * 3.f, 0.f, 0.012f + rng.uni() * 0.02f, (0.32f - i * 0.015f) * a, 0.18f);
            }
            ping(0.01f, 720.f, 0.94f, 0.2f, 0.22f, 0.07f * a, 0.3f);
            for (int i = 0; i < 4; i++) grain(0.25f + i * (0.06f + rng.uni() * 0.08f), 0, 3000.f + rng.uni() * 1500.f, 2500.f, 2.f, 0.f, 0.01f, 0.08f * a * (1.f - i * 0.2f), 0.25f);
            break;
        }
        case IMP_SHUTTER:                                     // ka-chunk: the mirror flips up, the shutter, the mirror back
            grain(0.f, 0, 4200.f, 3000.f, 1.6f, 0.f, 0.006f, 0.30f, 0.05f);
            grain(0.004f, 0, 1100.f, 800.f, 2.5f, 0.f, 0.022f, 0.20f, 0.05f);
            grain(0.075f, 0, 3600.f, 2800.f, 1.6f, 0.f, 0.006f, 0.24f, 0.05f);
            grain(0.079f, 0, 900.f, 700.f, 2.5f, 0.f, 0.03f, 0.16f, 0.05f);
            break;
        case IMP_WIPE:                                        // the wiper: a soft rubber swish across the glass
            grain(0.f, 0, 1500.f, 900.f, 0.9f, 0.18f, 0.16f, 0.035f, 0.1f);
            break;
        case IMP_WIPE_DEER:                                   // ...a slow, deliberate wipe, with the faintest squeak at the end
            grain(0.f, 0, 1300.f, 800.f, 0.9f, 0.35f, 0.3f, 0.04f, 0.1f);
            ping(0.62f, 1850.f, 0.88f, 0.12f, 0.07f, 0.018f, 0.1f);
            break;
    }
}

void natureBump(uint8_t kind) {
    ping(0.f, 145.f, 0.6f, 0.1f, 0.08f, 0.38f, 0.2f);
    grain(0.f, 0, 380.f, 300.f, 0.9f, 0.002f, 0.05f, 0.2f);
    if (kind == BUMP_TREE) {                               // a branch shaken: a brush, then the leaves settle
        grain(0.01f, 0, 3000.f, 2200.f, 0.8f, 0.025f, 0.09f, 0.11f, 0.3f);
        grain(0.08f, 0, 1800.f, 1400.f, 0.7f, 0.06f, 0.22f, 0.07f, 0.35f);
        int k = 2 + (int)(rng.uni() * 2.f);
        float t = 0.12f;
        for (int i = 0; i < k; i++) {                      // a couple of soft flutters, never evenly spaced
            t += 0.07f + rng.uni() * 0.12f;
            grain(t, 0, 2200.f + rng.uni() * 900.f, 1800.f, 0.9f, 0.015f, 0.045f, 0.035f + rng.uni() * 0.02f, 0.35f);
        }
    } else if (kind == BUMP_ROCK) {
        grain(0.f, 0, 4200.f, 4200.f, 1.2f, 0.f, 0.002f, 0.25f);
        ping(0.f, 1400.f, 1.f, 0.02f, 0.03f, 0.08f);
    } else {
        grain(0.f, 1, 900.f, 250.f, 3.f, 0.01f, 0.08f, 0.18f, 0.3f);
        for (int i = 0; i < 2; i++) ping(0.04f + i * 0.07f, 360.f + rng.uni() * 240.f, 1.15f, 0.04f, 0.03f, 0.03f, 0.5f);
    }
}

void natureCritter(uint8_t kind, float dist) {
    float g = clampf(1.2f - dist * 0.06f, 0.2f, 1.f);
    if (kind == CRIT_DEER) {
        grain(0.f, 0, 1400.f, 900.f, 1.5f, 0.01f, 0.07f, 0.12f * g, 0.3f);            // snort
        for (int i = 0; i < 7; i++) {                                                  // hooves, receding
            float t = 0.12f + i * (0.1f + rng.uni() * 0.03f), k = g * (1.f - i * 0.11f);
            grain(t, 0, 700.f, 600.f, 2.5f, 0.001f, 0.012f, 0.16f * k, 0.35f);
            ping(t, 300.f, 0.8f, 0.02f, 0.014f, 0.12f * k, 0.35f);
        }
    } else if (kind == CRIT_RABBIT) {
        for (int i = 0; i < 4; i++) grain(i * 0.07f, 0, 2400.f, 1800.f, 1.f, 0.003f, 0.02f, 0.08f * g, 0.2f);
    } else if (kind == CRIT_DUCK) {                         // two quacks, then wings beating away
        for (int q = 0; q < 2; q++) {
            float t0 = q * 0.22f;
            grain(t0, 0, 950.f, 800.f, 4.f, 0.008f, 0.07f, 0.14f * g, 0.3f);
            grain(t0, 0, 1900.f, 1600.f, 3.f, 0.008f, 0.05f, 0.05f * g, 0.3f);
        }
        for (int i = 0; i < 10; i++) grain(0.45f + i * 0.06f, 0, 900.f, 1100.f, 1.f, 0.008f, 0.025f, (0.1f - i * 0.008f) * g, 0.4f);
    } else if (kind == CRIT_HERON) {                        // a rough "fraank" and slow wingbeats
        grain(0.f, 0, 700.f, 560.f, 2.f, 0.012f, 0.16f, 0.16f * g, 0.35f);
        for (int i = 0; i < 6; i++) grain(0.3f + i * 0.26f, 0, 800.f, 900.f, 1.f, 0.02f, 0.05f, (0.08f - i * 0.01f) * g, 0.4f);
    } else if (kind == CRIT_BEAVER) {                       // a waddle through the reeds
        for (int i = 0; i < 4; i++) grain(i * 0.12f, 0, 1500.f, 1200.f, 1.f, 0.01f, 0.03f, 0.06f * g, 0.25f);
    } else if (kind == CRIT_SPLASH) {                       // the plunge
        grain(0.f, 1, 1800.f, 400.f, 2.f, 0.004f, 0.12f, 0.24f * g, 0.4f);
        grain(0.02f, 0, 3200.f, 2000.f, 1.f, 0.006f, 0.06f, 0.08f * g, 0.4f);
        for (int i = 0; i < 3; i++) ping(0.08f + i * 0.07f, 360.f + rng.uni() * 200.f, 1.12f, 0.04f, 0.03f, 0.04f * g, 0.5f);
    } else if (kind == CRIT_SQUIRREL) {                     // a soft chitter and claws on bark
        for (int i = 0; i < 6; i++) grain(i * 0.045f, 0, 2500.f + rng.uni() * 300.f, 2400.f, 9.f, 0.006f, 0.02f, 0.05f * g, 0.25f);
        for (int i = 0; i < 5; i++) grain(0.35f + i * 0.08f, 0, 1300.f, 1100.f, 1.2f, 0.006f, 0.02f, 0.04f * g, 0.25f);
    } else {
        for (int i = 0; i < 12; i++) grain(i * 0.035f, 0, 1100.f, 1300.f, 1.f, 0.004f, 0.015f, (0.14f - i * 0.009f) * g, 0.4f);
        for (int i = 0; i < 3; i++) ping(0.05f + i * 0.13f, 3900.f, 0.8f, 0.04f, 0.035f, 0.05f * g, 0.4f);
    }
}

// ================= thunder =================
struct Thunder { bool on; int startIn; float power, close, t, len; float bumpT[6], bumpA[6], bumpTau[6]; };
static Thunder s_th[3];
static dsp::SVF s_thLP, s_thBP, s_thCrack;
static float s_brown = 0.f;
void natureThunder(float delay, float power, float close) {
    for (Thunder& t : s_th) {
        if (t.on) continue;
        t.on = true;
        t.startIn = (int)(delay * dsp::SR);
        t.power = power; t.close = close; t.t = 0.f;
        t.len = 3.f + (1.f - close) * 3.f;
        for (int k = 0; k < 6; k++) {
            t.bumpT[k] = k == 0 ? 0.f : rng.uni() * (t.len * 0.6f);
            t.bumpA[k] = k == 0 ? 1.f : 0.3f + rng.uni() * 0.6f;
            t.bumpTau[k] = 0.4f + rng.uni() * 1.1f;
        }
        return;
    }
}
static float thunderEnv(const Thunder& t, float at) {
    float env = 0.f;
    for (int k = 0; k < 6; k++) {
        float e = at - t.bumpT[k];
        if (e > 0.f) env += t.bumpA[k] * (e < 0.08f ? e / 0.08f : expf(-(e - 0.08f) / t.bumpTau[k]));
    }
    return env;
}
static void thunderBlock(Bus& b, int n) {
    s_thLP.set(240.f, 0.7f);
    s_thBP.set(380.f, 0.9f);
    for (Thunder& t : s_th) {
        if (!t.on) continue;
        if (t.startIn >= n) { t.startIn -= n; continue; }
        float span = (float)(n - t.startIn) / dsp::SR;
        // envelopes move slowly: evaluate at the block edges, interpolate in between
        float e0 = thunderEnv(t, t.t), e1 = thunderEnv(t, t.t + span);
        float c0 = (t.close > 0.35f && t.t < 0.6f) ? expf(-t.t * 7.f) * (t.close - 0.2f) * 1.6f : 0.f;
        float c1 = (t.close > 0.35f && t.t < 0.6f) ? expf(-(t.t + span) * 7.f) * (t.close - 0.2f) * 1.6f : 0.f;
        s_thCrack.set(6000.f * powf(0.06f, clampf(t.t / 0.35f, 0.f, 1.f)), 0.8f);
        float inv = 1.f / (n - t.startIn);
        for (int i = t.startIn, j = 0; i < n; i++, j++) {
            float u = j * inv;
            float w = rng.bi();
            s_brown = (s_brown + w * 0.05f) * 0.996f;
            float env = e0 + (e1 - e0) * u;
            float rumble = (s_thLP.lp(s_brown) * 1.6f + s_thBP.bp(s_brown) * 4.5f) * env;
            float cr = c0 + (c1 - c0) * u;
            float crack = cr > 0.f ? s_thCrack.lp(w) * cr : 0.f;
            float v = (rumble * (0.35f + 0.5f * t.close) + crack) * t.power;
            b.dry[i] += v * 0.7f;
            b.rev[i] += v * 0.6f;
        }
        t.t += span;
        t.startIn = 0;
        if (t.t > t.len + 2.f) t.on = false;
    }
}

// ================= wind, leaves =================
static dsp::Pink s_pink;
static dsp::SVF s_windBP, s_windLow, s_whistle;
static float s_windG = 0.f;
// leaves: a soft shimmer that breathes with the wind (no grains, no clicks)
static dsp::SVF s_leafHi, s_leafLo;
static float s_flut = 0.f, s_flutT = 0.f, s_leafG = 0.f;
static int s_flutN = 0;
static void windBlock(Bus& b, int n, const NatScene& s) {
    float gust = s.gust;
    s_windBP.set(230.f + 950.f * gust + 300.f * s.storm, 0.75f);
    s_windLow.set(160.f + 120.f * gust, 0.6f);
    s_whistle.set(950.f + 800.f * gust, 9.f);
    float target = (0.04f + 0.42f * gust) * (0.55f + 0.45f * s.open) * (1.f - 0.35f * s.hush);
    // Leaves: thousands of tiny contacts blur into a soft "shhh". Two noise bands (a shimmer and a
    // leafy body) under a flutter that glides between random levels every few ms, so it moves
    // like foliage without a single sharp onset. Gusts swell it; under trees it's much stronger.
    s_leafHi.set(2600.f + 1500.f * gust, 0.8f);
    s_leafLo.set(1100.f + 300.f * gust, 0.9f);
    float leafTarget = (0.015f + 0.2f * gust * gust) * (0.12f + 0.88f * s.forestNear);
    for (int i = 0; i < n; i++) {
        s_windG += (target - s_windG) * 0.0005f;
        s_leafG += (leafTarget - s_leafG) * 0.0002f;      // foliage answers a gust a beat late
        float w = rng.bi();
        float p = s_pink.tick(w);
        float v = (s_windBP.bp(p) * 1.5f + s_windLow.lp(p) * 0.35f) * s_windG;
        v += s_whistle.bp(w) * s.storm * gust * 0.05f;
        if (--s_flutN <= 0) { s_flutN = 100 + (int)(rng.next() % 140u); s_flutT = rng.uni(); }
        s_flut += (s_flutT - s_flut) * 0.006f;              // ~7 ms glide: flutter, never a click
        v += (s_leafHi.bp(w) * 0.8f + s_leafLo.bp(rng.bi()) * 0.5f) * (0.6f + 0.4f * s_flut) * s_leafG;
        b.dry[i] += v;
        b.rev[i] += v * 0.3f;
    }
}

// ================= water: lapping + bubbles =================
static dsp::SVF s_lapLP;
static float s_lapBrown = 0.f, s_lapEnv = 0.f, s_lapTarget = 0.f, s_lapTimer = 0.f, s_lapG = 0.f;
static dsp::SVF s_brook, s_roarBP, s_roarHi;
static dsp::Pink s_roarPink;
static float s_roarG = 0.f;
static float s_brookG = 0.f, s_bFlut = 0.f, s_bFlutT = 0.f;
static int s_bFlutN = 0;
static void brookBlock(Bus& b, int n, const NatScene& s) {
    float target = s.streamNear * 0.09f;
    if (target < 0.001f && s_brookG < 0.0005f) return;
    s_brook.set(1700.f, 0.8f);
    if (rng.uni() < s.streamNear * 3.f * n / dsp::SR) ping(0.f, 380.f + rng.uni() * 300.f, 1.1f, 0.04f, 0.025f, 0.02f, 0.5f);
    for (int i = 0; i < n; i++) {
        s_brookG += (target - s_brookG) * 0.0003f;
        if (--s_bFlutN <= 0) { s_bFlutN = 40 + (int)(rng.next() % 90u); s_bFlutT = rng.uni(); }
        s_bFlut += (s_bFlutT - s_bFlut) * 0.01f;           // a babble: busy, but never a click
        float v = s_brook.bp(rng.bi()) * (0.45f + 0.55f * s_bFlut) * s_brookG;
        b.dry[i] += v;
        b.rev[i] += v * 0.35f;
    }
}

// The garden: a bamboo water fountain tipping now and then (a hollow "tock"), and glass wind chimes.
static float s_shishiT = 8.f, s_chimeT = 5.f;
static void gardenBlock(const NatScene& s, int n) {
    if (s.japan < 0.5f) return;
    float dt = (float)n / dsp::SR;
    s_shishiT -= dt; s_chimeT -= dt;
    if (s_shishiT <= 0.f) {
        s_shishiT = 12.f + rng.uni() * 12.f;
        if (s.lakeNear > 0.1f || s.streamNear > 0.1f) {
            ping(0.f, 430.f, 0.93f, 0.05f, 0.09f, 0.11f, 0.45f);
            grain(0.f, 0, 650.f, 600.f, 6.f, 0.001f, 0.04f, 0.08f, 0.45f);
            grain(0.15f, 1, 1200.f, 600.f, 1.f, 0.05f, 0.4f, 0.03f, 0.4f);        // the water running out
        }
    }
    if (s_chimeT <= 0.f) {
        s_chimeT = 6.f + rng.uni() * 10.f / (0.3f + s.gust);
        int k = 2 + (int)(rng.uni() * 3.f);
        for (int i = 0; i < k; i++) ping(i * (0.12f + rng.uni() * 0.15f), 2600.f + rng.uni() * 1300.f, 1.f, 0.1f, 0.7f, 0.02f, 0.75f);
    }
}

// The surf: a swell gathers, breaks, and hisses back down the sand.
static dsp::SVF s_surfLP, s_surfHi;
static dsp::Pink s_surfPink;
static float s_surfT = 0.f, s_surfPeriod = 9.f, s_surfG = 0.f, s_surfBrown = 0.f;
static void surfBlock(Bus& b, int n, const NatScene& s) {
    float target = s.oceanNear * 0.3f;
    if (target < 0.001f && s_surfG < 0.0005f) return;
    float dt = (float)n / dsp::SR;
    s_surfT += dt;
    if (s_surfT > s_surfPeriod) { s_surfT = 0.f; s_surfPeriod = 7.f + rng.uni() * 5.f; }
    float u = s_surfT / s_surfPeriod;
    float build = u < 0.55f ? u / 0.55f : 0.f;                              // the swell gathering
    float crash = u >= 0.55f ? expf(-(u - 0.55f) * s_surfPeriod * 1.6f) : 0.f;   // breaking
    float wash = u >= 0.58f ? expf(-(u - 0.58f) * s_surfPeriod * 0.5f) : 0.f;    // hissing back
    s_surfLP.set(300.f + 900.f * crash + 200.f * build, 0.7f);
    s_surfHi.set(3500.f, 0.6f);
    for (int i = 0; i < n; i++) {
        s_surfG += (target - s_surfG) * 0.0002f;
        float w = rng.bi(), p = s_surfPink.tick(w);
        s_surfBrown = (s_surfBrown + w * 0.03f) * 0.997f;
        float v = (s_surfLP.lp(p) * (0.25f + 0.6f * build * build + 1.4f * crash) + s_surfHi.bp(w) * 0.5f * wash + s_surfBrown * 0.6f) * s_surfG;
        b.dry[i] += v;
        b.rev[i] += v * 0.35f;
    }
}

static void roarBlock(Bus& b, int n, const NatScene& s) {
    float target = s.fallsNear * 0.16f;
    if (target < 0.001f && s_roarG < 0.0005f) return;
    s_roarBP.set(700.f, 0.6f);
    s_roarHi.set(2600.f, 0.7f);
    for (int i = 0; i < n; i++) {
        s_roarG += (target - s_roarG) * 0.0002f;
        float w = rng.bi(), p = s_roarPink.tick(w);
        float v = (s_roarBP.bp(p) * 1.4f + s_roarHi.bp(w) * 0.35f) * s_roarG;   // a steady soft roar
        b.dry[i] += v;
        b.rev[i] += v * 0.4f;
    }
}

static void waterBlock(Bus& b, int n, const NatScene& s) {
    brookBlock(b, n, s);
    roarBlock(b, n, s);
    surfBlock(b, n, s);
    gardenBlock(s, n);
    float lv = s.lakeNear;
    s_lapG += (lv - s_lapG) * 0.05f;
    if (s_lapG < 0.01f && s.rain < 0.05f) return;
    s_lapLP.set(520.f, 0.9f);
    s_lapTimer -= (float)n / dsp::SR;
    if (s_lapTimer <= 0.f) { s_lapTarget = 0.4f + rng.uni() * 0.6f; s_lapTimer = 1.2f + rng.uni() * 2.f; }
    float bubbleRate = (s_lapG * 0.5f + s.rain * s_lapG * 6.f) * n / dsp::SR;
    if (rng.uni() < bubbleRate) ping(rng.uni() * 0.01f, 320.f + rng.uni() * 360.f, 1.12f + rng.uni() * 0.08f, 0.05f, 0.025f + rng.uni() * 0.02f, 0.015f + 0.02f * rng.uni(), 0.6f);
    for (int i = 0; i < n; i++) {
        s_lapTarget *= 0.99998f;
        s_lapEnv += (s_lapTarget - s_lapEnv) * (s_lapTarget > s_lapEnv ? 0.00012f : 0.00005f);
        s_lapBrown = (s_lapBrown + rng.bi() * 0.04f) * 0.997f;
        float v = s_lapLP.lp(s_lapBrown) * s_lapEnv * s_lapG * 1.8f;
        b.dry[i] += v;
        b.rev[i] += v * 0.4f;
    }
}

// ================= rain =================
static float s_rainHP = 0.f, s_rainG = 0.f;
static dsp::SVF s_rainLP, s_drop[4];
static float s_dropE[4] = {0, 0, 0, 0};
static int s_dropI = 0;
static void rainBlock(Bus& b, int n, const NatScene& s) {
    float r = s.rain;
    s_rainG += (r - s_rainG) * 0.08f;
    if (s_rainG < 0.005f) return;
    s_rainLP.set(5200.f + 1500.f * s.storm, 0.7f);
    const float hk = dsp::coef(1400.f);
    float pDrop = s_rainG * (0.006f + 0.01f * s.storm) * (1.f - s.snowy);
    float hiss = powf(s_rainG, 1.3f) * (0.16f + 0.1f * s.storm) * (1.f - 0.85f * s.snowy);   // snow falls nearly silent
    for (int i = 0; i < n; i++) {
        float w = rng.bi();
        s_rainHP += hk * (w - s_rainHP);
        float v = s_rainLP.lp(w - s_rainHP) * hiss;
        if (rng.uni() < pDrop) {
            int k = s_dropI = (s_dropI + 1) & 3;
            bool leaf = rng.uni() < s.forestNear * 0.7f;
            s_drop[k].set(leaf ? 700.f + rng.uni() * 1100.f : 1800.f + rng.uni() * 4200.f, 5.f);
            s_dropE[k] = (0.25f + rng.uni() * 0.75f) * (leaf ? 1.4f : 1.f);
        }
        for (int k = 0; k < 4; k++) {
            if (s_dropE[k] < 0.001f) continue;
            v += s_drop[k].bp(rng.bi() * s_dropE[k]) * 0.16f * s_rainG;
            s_dropE[k] *= 0.985f;
        }
        b.dry[i] += v;
        b.rev[i] += v * 0.25f;
    }
}

// ================= birds =================
enum : uint8_t { SP_SONG, SP_TRILL, SP_FEEBEE, SP_DOVE, SP_OWL, SP_LOON, SP_CUCKOO, SP_CROW, SP_HAWK, SP_COYOTE, SP_THRUSH, SP_GULL, SP_ICESING, SP_PARROT, SP_HOWLER, SP_N };
struct Syl { float f0, f1, dur, gap, fm, fmD, harm, breath; };
struct Bird {
    bool on = false;
    uint8_t sp = 0;
    Syl syl[28];
    int n = 0, i = 0;
    float t = 0.f, angle = 0.f, dist = 0.5f, gain = 0.f, lpk = 0.5f, lpz = 0.f;
    uint32_t ph = 0, fmPh = 0;
    float f = 1000.f, fmul = 1.f;
};
static Bird s_bird[4];
static float s_birdClock = 0.f;
struct Pending { bool on; float at; uint8_t sp; uint32_t id; };
static Pending s_reply = {false, 0, 0, 0};

static int makeSong(Syl* out, uint8_t sp, uint32_t id) {
    dsp::Rng r; r.s = 0x9E3779B9u ^ (id * 0x85EBCA77u) ^ (sp * 0xC2B2AE35u);
    dsp::Rng v; v.s = rng.next() | 1u;                 // per-performance variation
    int n = 0;
    switch (sp) {
        case SP_SONG: {
            float base = 2300.f + r.uni() * 1500.f;
            int k = 4 + (int)(r.uni() * 4);
            static const float steps[6] = {1.f, 1.12f, 1.26f, 0.89f, 1.34f, 0.84f};
            for (int j = 0; j < k; j++) {
                float f0 = base * steps[r.next() % 6], f1 = f0 * (0.78f + r.uni() * 0.5f);
                bool trill = r.uni() < 0.3f;
                out[n++] = {f0 * (0.97f + v.uni() * 0.06f), f1, 0.06f + r.uni() * 0.09f, 0.03f + r.uni() * 0.05f, trill ? 28.f + r.uni() * 20.f : 0.f, trill ? 0.06f : 0.f, 0.08f, 0.f};
            }
        } break;
        case SP_TRILL: {                                   // a wren: warbles, then a trill or two at stepped pitches
            float base = 3800.f + r.uni() * 900.f;
            int w = 2 + (int)(r.next() % 2u);
            for (int j = 0; j < w; j++) {
                float f0 = base * (0.9f + r.uni() * 0.3f);
                out[n++] = {f0, f0 * (0.85f + r.uni() * 0.3f), 0.06f + r.uni() * 0.03f, 0.04f + v.uni() * 0.02f, 0.f, 0.f, 0.06f, 0.f};
            }
            int segs = 1 + (int)(r.next() % 2u);
            for (int g = 0; g < segs; g++) {
                float fs = base * (g == 0 ? 1.08f : 0.86f) * (0.95f + r.uni() * 0.1f);
                int k = 6 + (int)(r.next() % 5u);
                float period = 1.f / (12.f + r.uni() * 4.f);     // 12-16 notes a second: a trill, not a ratchet
                float dur = period * 0.62f;
                for (int j = 0; j < k && n < 27; j++) {
                    float acc = 1.f - 0.15f * j / k;            // it hurries a little toward the end
                    float jit = 1.f + (v.uni() - 0.5f) * 0.2f;  // and is never machine-even
                    float fj = fs * (1.f + 0.02f * (v.uni() - 0.5f));
                    out[n++] = {fj, fj * 0.9f, dur * jit, (period - dur) * acc * jit, 0.f, 0.f, 0.05f, 0.f};
                }
                if (n < 27) out[n - 1].gap += 0.06f;             // a breath between trills
            }
        } break;
        case SP_FEEBEE: {
            float f = 3700.f + r.uni() * 400.f;
            out[n++] = {f, f * 0.99f, 0.28f + v.uni() * 0.05f, 0.08f, 0.f, 0.f, 0.02f, 0.f};
            out[n++] = {f * 0.85f, f * 0.84f, 0.32f, 0.2f, 0.f, 0.f, 0.02f, 0.f};
            if (r.uni() < 0.5f) out[n++] = {f * 0.85f, f * 0.84f, 0.2f, 0.f, 0.f, 0.f, 0.02f, 0.f};
        } break;
        case SP_DOVE: {
            float f = 480.f + r.uni() * 90.f;
            out[n++] = {f, f * 1.08f, 0.35f, 0.12f, 0.f, 0.f, 0.15f, 0.12f};
            out[n++] = {f * 1.12f, f * 0.95f, 0.55f, 0.25f, 0.f, 0.f, 0.15f, 0.12f};
            for (int j = 0; j < 3; j++) out[n++] = {f, f * 0.97f, 0.4f, 0.22f, 0.f, 0.f, 0.15f, 0.12f};
        } break;
        case SP_OWL: {
            float f = 370.f + r.uni() * 40.f;
            out[n++] = {f, f * 0.97f, 0.45f, 0.35f, 0.f, 0.f, 0.1f, 0.18f};
            out[n++] = {f, f * 0.98f, 0.16f, 0.06f, 0.f, 0.f, 0.1f, 0.18f};
            out[n++] = {f, f * 0.98f, 0.16f, 0.25f, 0.f, 0.f, 0.1f, 0.18f};
            out[n++] = {f * 1.02f, f * 0.94f, 0.7f, 0.f, 0.f, 0.f, 0.1f, 0.2f};
        } break;
        case SP_CUCKOO: {                                 // cu-coo, a falling third, a few times over
            int k = 2 + (int)(r.uni() * 3.f);
            float f = 640.f + r.uni() * 40.f;
            for (int j = 0; j < k && n < 26; j++) {
                out[n++] = {f, f * 0.985f, 0.2f, 0.12f, 0.f, 0.f, 0.12f, 0.04f};
                out[n++] = {f * 0.84f, f * 0.83f, 0.3f, 0.55f + v.uni() * 0.15f, 0.f, 0.f, 0.12f, 0.04f};
            }
        } break;
        case SP_CROW: {                                   // a few rough caws
            int k = 2 + (int)(r.uni() * 2.f);
            float f = 480.f + r.uni() * 80.f;
            for (int j = 0; j < k; j++) out[n++] = {f, f * 0.82f, 0.26f, 0.22f + v.uni() * 0.1f, 32.f, 0.06f, 0.6f, 0.35f};
        } break;
        case SP_ICESING: {                                // frozen lake ice "singing": a long falling pew, echoing
            out[n++] = {2600.f, 320.f, 1.1f, 0.6f, 0.f, 0.f, 0.f, 0.f};
            out[n++] = {1900.f, 280.f, 0.9f, 0.f, 0.f, 0.f, 0.f, 0.f};
        } break;
        case SP_PARROT: {                                 // rough squawks
            int k = 2 + (int)(r.uni() * 3.f);
            float f = 1600.f + r.uni() * 700.f;
            for (int j = 0; j < k; j++) out[n++] = {f, f * (0.75f + r.uni() * 0.4f), 0.12f, 0.1f + v.uni() * 0.15f, 45.f, 0.08f, 0.7f, 0.45f};
        } break;
        case SP_HOWLER: {                                 // howler monkeys: a far, rolling roar
            for (int j = 0; j < 4; j++) out[n++] = {340.f + r.uni() * 60.f, 300.f, 0.7f, 0.15f, 9.f, 0.08f, 0.8f, 0.6f};
        } break;
        case SP_GULL: {                                   // kyow, kyow, kyow
            int k = 3 + (int)(r.uni() * 3.f);
            float f = 1300.f + r.uni() * 300.f;
            for (int j = 0; j < k; j++) out[n++] = {f * 1.1f, f * 0.82f, 0.17f, 0.12f + v.uni() * 0.08f, 0.f, 0.f, 0.45f, 0.22f};
        } break;
        case SP_THRUSH: {                                 // the varied thrush: one long eerie buzzing whistle, then another
            float f = 2700.f + r.uni() * 600.f;
            out[n++] = {f, f * 0.99f, 1.f + v.uni() * 0.3f, 1.4f + v.uni(), 70.f, 0.015f, 0.08f, 0.04f};
            out[n++] = {f * (0.82f + r.uni() * 0.3f), f * 0.8f, 0.9f, 0.f, 70.f, 0.015f, 0.08f, 0.04f};
        } break;
        case SP_HAWK: {                                   // a long, falling "kee-eeer" from high up
            float f = 2400.f + r.uni() * 300.f;
            out[n++] = {f * 0.9f, f, 0.12f, 0.02f, 0.f, 0.f, 0.25f, 0.12f};
            out[n++] = {f, f * 0.72f, 0.9f, 0.f, 18.f, 0.03f, 0.25f, 0.15f};
        } break;
        case SP_COYOTE: {                                 // a rising howl, then yips
            float f = 520.f + r.uni() * 60.f;
            out[n++] = {f, f * 1.5f, 0.8f, 0.f, 5.f, 0.02f, 0.3f, 0.12f};
            out[n++] = {f * 1.5f, f * 1.3f, 1.2f, 0.35f, 5.f, 0.025f, 0.3f, 0.12f};
            for (int j = 0; j < 4; j++) out[n++] = {f * 1.8f, f * 2.1f, 0.07f, 0.09f + v.uni() * 0.05f, 0.f, 0.f, 0.3f, 0.15f};
            out[n++] = {f * 1.2f, f * 1.45f, 0.9f, 0.f, 5.f, 0.02f, 0.3f, 0.12f};
        } break;
        case SP_LOON: {                                   // the wail across the water
            float f = 640.f + r.uni() * 60.f;
            out[n++] = {f, f * 1.55f, 1.1f, 0.05f, 5.5f, 0.012f, 0.18f, 0.03f};
            out[n++] = {f * 1.55f, f * 1.45f, 1.3f, 0.25f, 5.f, 0.018f, 0.18f, 0.03f};
            out[n++] = {f * 1.45f, f * 1.15f, 1.4f, 0.f, 5.f, 0.015f, 0.18f, 0.03f};
        } break;
    }
    return n;
}

static void startBird(uint8_t sp, uint32_t id, float dist, float angle) {
    for (Bird& b : s_bird) {
        if (b.on) continue;
        b.on = true;
        b.sp = sp;
        b.n = makeSong(b.syl, sp, id);
        b.i = 0; b.t = -0.01f;
        b.dist = dist;
        b.angle = angle;
        b.lpk = 0.95f - dist * 0.7f;
        b.lpz = 0.f;
        b.ph = 0; b.fmPh = 0;
        b.f = b.syl[0].f0;
        b.fmul = powf(b.syl[0].f1 / b.syl[0].f0, 1.f / (b.syl[0].dur * dsp::SR));
        return;
    }
}

static void birdSchedule(const NatScene& s, float dt) {
    float tod = s.tod;
    float dawn = bumpf(tod, 0.19f, 0.25f, 0.33f, 0.40f);
    float day = bumpf(tod, 0.30f, 0.36f, 0.62f, 0.70f);
    float dusk = bumpf(tod, 0.64f, 0.69f, 0.77f, 0.83f);
    float night = tod > 0.84f || tod < 0.17f ? 1.f : 0.f;
    float afterRain = (s.rainAgo > 4.f && s.rainAgo < 70.f) ? 0.9f : 0.f;
    float hush = 1.f - s.rain * 0.92f - s.storm * 0.08f;
    float still = 0.7f + 0.3f * clampf(s.stillFor / 10.f, 0.f, 1.f);
    float rate = ((dawn * 1.6f + day * 0.45f + dusk * 0.4f + afterRain) * hush + night * 0.12f) * still * (1.f - 0.45f * s.hush);   // songs per second
    s_birdClock += dt;
    if (s_reply.on && s.t >= s_reply.at) {
        startBird(s_reply.sp, s_reply.id, 0.35f + rng.uni() * 0.6f, s.yaw + (rng.uni() - 0.5f) * 5.f);
        s_reply.on = false;
    }
    if (rng.uni() > rate * dt) return;
    uint8_t sp;
    float r = rng.uni();
    if (s.desert > 0.5f) sp = night > 0.5f ? SP_COYOTE : (r < 0.35f ? SP_HAWK : SP_DOVE);
    else if (s.tundra > 0.5f && night > 0.5f) sp = s.lakeNear > 0.2f && r < 0.5f ? SP_ICESING : SP_OWL;   // the desert: hawks by day, coyotes by night
    else if (night > 0.5f) sp = (s.lakeNear > 0.25f && r < 0.6f) ? SP_LOON : SP_OWL;
    else if (dusk > 0.5f && s.lakeNear > 0.3f && r < 0.25f) sp = SP_LOON;
    else if (s.oceanNear > 0.2f && r < 0.5f) sp = SP_GULL;
    else if (s.tundra > 0.5f && s.lakeNear > 0.2f && r < 0.35f) sp = SP_ICESING;
    else if (s.tropical > 0.5f && dawn > 0.3f && r < 0.25f) sp = SP_HOWLER;
    else if (s.tropical > 0.5f && r < 0.5f) sp = SP_PARROT;
    else if (s.giant > 0.5f && r < 0.45f) sp = SP_THRUSH;
    else if (s.cuckoo > 0.5f && r < 0.22f) sp = SP_CUCKOO;
    else if (s.crows > 0.5f && r < 0.25f) sp = SP_CROW;
    else if (r < 0.38f) sp = SP_SONG;
    else if (r < 0.38f + 0.2f + 0.15f * s.forestNear) sp = SP_TRILL;
    else if (r < 0.78f) sp = SP_FEEBEE;
    else sp = SP_DOVE;
    uint32_t id = rng.next() & 3u;
    startBird(sp, id, 0.2f + rng.uni() * 0.8f, s.yaw + (rng.uni() - 0.5f) * 6.f);
    if (sp != SP_LOON && sp != SP_HAWK && sp != SP_ICESING && rng.uni() < 0.45f) s_reply = {true, s.t + 0.8f + rng.uni() * 2.f, sp, (id + 1) & 3u};
}

static void birdsBlock(Bus& bus, int n, const NatScene& s) {
    birdSchedule(s, (float)n / dsp::SR);
    for (Bird& b : s_bird) {
        if (!b.on) continue;
        float front = 0.55f + 0.45f * cosf(wrapPi(b.angle - s.yaw));
        float base = (b.sp == SP_LOON || b.sp == SP_COYOTE || b.sp == SP_ICESING ? 0.26f : b.sp == SP_HOWLER ? 0.2f : b.sp == SP_HAWK ? 0.1f : b.sp == SP_OWL || b.sp == SP_DOVE || b.sp == SP_CUCKOO ? 0.22f : b.sp == SP_CROW ? 0.12f : 0.15f) * (1.f - 0.65f * b.dist) * front;
        for (int i = 0; i < n && b.on; i++) {
            Syl& y = b.syl[b.i];
            float v = 0.f;
            if (b.t >= 0.f && b.t < y.dur) {
                float u = b.t / y.dur;
                float a = u < 0.12f ? u / 0.12f : (1.f - u) / 0.88f;
                a = a * a * (3.f - 2.f * a);
                float fm = y.fm > 0.f ? 1.f + y.fmD * dsp::sine(b.fmPh) : 1.f;
                b.fmPh += (uint32_t)(y.fm * (4294967296.f / dsp::SR));
                b.ph += (uint32_t)(b.f * fm * (4294967296.f / dsp::SR));
                b.f *= b.fmul;
                v = (dsp::sine(b.ph) + y.harm * dsp::sine(b.ph * 2u) + y.breath * rng.bi()) * a;
            }
            b.t += 1.f / dsp::SR;
            if (b.t >= y.dur + y.gap) {
                if (++b.i >= b.n) { b.on = false; break; }
                Syl& z = b.syl[b.i];
                b.t = 0.f;
                b.f = z.f0;
                b.fmul = powf(z.f1 / z.f0, 1.f / (z.dur * dsp::SR));
            }
            b.lpz += b.lpk * (v - b.lpz);
            float o = b.lpz * base;
            bus.dry[i] += o;
            bus.rev[i] += o * (0.25f + 0.75f * b.dist);
            if (b.sp == SP_LOON) bus.dly[i] += o * 0.25f;
        }
    }
}

// ================= insects and frogs =================
struct Cricket { uint32_t ph, dph; float period, t, amp, on, target, e; int pulses; float pl, pg; };
static Cricket s_cr[4];
static float s_crLevel = 0.f;
static dsp::SVF s_cicada;
static float s_cicEnv = 0.f, s_cicT = 0.f, s_cicLevel = 0.f;
static uint32_t s_cicPh = 0;
struct Croak { float t, next; int left; float pulse; uint32_t ph; int pn; };
static Croak s_croak[2];
struct Peep { float t, next; uint32_t ph; float f; };
static Peep s_peep[3];
static float s_frogLevel = 0.f;

static void insectsBlock(Bus& b, int n, const NatScene& s) {
    float dtb = (float)n / dsp::SR;
    float tod = s.tod;
    float nightish = bumpf(tod, 0.70f, 0.78f, 1.1f, 1.2f) + bumpf(tod, -0.2f, -0.1f, 0.14f, 0.21f);
    float hush = s.speed > 0.05f ? 0.35f : 0.35f + 0.65f * clampf(s.stillFor / 6.f, 0.f, 1.f);
    float crT = nightish * (1.f - s.rain) * (1.f - s.storm) * hush * 0.09f;
    s_crLevel += (crT - s_crLevel) * clampf(dtb * 0.6f, 0.f, 1.f);
    float cicT = bumpf(tod, 0.42f, 0.48f, 0.62f, 0.68f) * s.open * (1.f - s.cloud * 0.8f) * (1.f - s.rain) * 0.028f;
    s_cicLevel += (cicT - s_cicLevel) * clampf(dtb * 0.3f, 0.f, 1.f);
    float frT = (nightish * 0.8f + s.wet * 0.5f + s.frogBoost * 0.7f) * fmaxf(s.lakeNear, s.frogBoost * 0.6f) * (1.f - s.rain * 0.5f) * hush;
    s_frogLevel += (frT - s_frogLevel) * clampf(dtb * 0.4f, 0.f, 1.f);

    // individual crickets come and go
    for (Cricket& c : s_cr) {
        if (rng.uni() < dtb * 0.08f) c.target = rng.uni() < 0.7f ? 1.f : 0.f;
        c.on += (c.target - c.on) * clampf(dtb * 0.5f, 0.f, 1.f);
    }
    if (s_cicLevel > 0.0005f) {
        s_cicT += dtb;
        float cyc = fmodf(s_cicT, 11.f);
        float tgt = cyc < 3.f ? cyc / 3.f : cyc < 6.f ? 1.f : cyc < 8.f ? 1.f - (cyc - 6.f) / 2.f : 0.f;
        s_cicEnv += (tgt - s_cicEnv) * 0.1f;
        s_cicada.set(5300.f, 7.f);
    }
    float gate[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        float v = 0.f;
        if (s_crLevel > 0.0003f) {
            if ((i & 31) == 0) {                                   // pulse gates move in 1.3 ms steps
                for (int k = 0; k < 4; k++) {
                    Cricket& c = s_cr[k];
                    c.t += 32.f / dsp::SR;
                    if (c.t > c.period) c.t -= c.period;
                    float q = c.t / (c.pl + c.pg);
                    int idx = (int)q;
                    gate[k] = (c.on > 0.01f && idx < c.pulses && (q - idx) * (c.pl + c.pg) < c.pl) ? c.amp * c.on : 0.f;
                }
            }
            for (int k = 0; k < 4; k++) {
                Cricket& c = s_cr[k];
                c.e += (gate[k] - c.e) * 0.2f;
                if (c.e < 0.0005f) continue;
                c.ph += c.dph;
                v += dsp::sine(c.ph) * c.e;
            }
            v *= s_crLevel;
        }
        if (s_cicLevel > 0.0005f) {
            s_cicPh += dsp::inc(185.f);
            float am = 0.5f + 0.5f * dsp::sine(s_cicPh);
            v += s_cicada.bp(rng.bi()) * am * am * s_cicEnv * s_cicLevel * 3.f;
        }
        b.dry[i] += v;
        b.rev[i] += v * 0.35f;
    }
    if (s_frogLevel > 0.003f) {
        for (Croak& c : s_croak) {
            c.next -= dtb;
            if (c.next <= 0.f && c.left <= 0) { c.left = 2 + (int)(rng.uni() * 2); c.t = 0.f; c.next = 2.f + rng.uni() * 5.f; }
        }
        for (Peep& p : s_peep) {
            p.next -= dtb;
            if (p.next <= 0.f) { p.t = 0.f; p.next = 0.5f + rng.uni() * 1.2f; p.f = 2550.f + rng.uni() * 300.f; }
        }
        for (int i = 0; i < n; i++) {
            float v = 0.f;
            for (Croak& c : s_croak) {
                if (c.left <= 0) continue;
                c.t += 1.f / dsp::SR;
                if (c.t >= 0.f) {
                    if (--c.pn <= 0) { c.pn = (int)(0.036f * dsp::SR); c.pulse = 0.5f; }   // a buzz of 28 Hz pulses
                    c.pulse *= 0.9909f;
                    c.ph += dsp::inc(430.f);
                    v += dsp::sine(c.ph) * c.pulse;
                }
                if (c.t > 0.26f) { c.left--; c.t = -0.18f; }
            }
            for (Peep& p : s_peep) {
                if (p.t > 0.07f) continue;
                p.t += 1.f / dsp::SR;
                float u = p.t / 0.07f;
                p.ph += (uint32_t)(p.f * (1.f + 0.2f * u) * (4294967296.f / dsp::SR));
                v += dsp::sine(p.ph) * sinf(u * 3.14159f) * 0.18f;
            }
            v *= s_frogLevel * 0.2f;
            b.dry[i] += v;
            b.rev[i] += v * 0.5f;
        }
    }
}

// ================= the flock passing overhead =================
static dsp::SVF s_wing;
static float s_wingPh = 0.f;
static void wingsBlock(Bus& b, int n, const NatScene& s) {
    if (s.murmur < 0.02f) return;
    s_wingPh += (float)n / dsp::SR;
    float swoop = 0.5f + 0.5f * sinf(s_wingPh * 0.55f) * sinf(s_wingPh * 0.21f + 1.f);
    s_wing.set(700.f + 900.f * swoop, 0.8f);
    float g = s.murmur * swoop * swoop * 0.09f;
    for (int i = 0; i < n; i++) {
        float v = s_wing.bp(rng.bi()) * g;
        b.dry[i] += v;
        b.rev[i] += v * 0.5f;
    }
}

void natureInit() {
    for (int k = 0; k < 4; k++) {
        Cricket& c = s_cr[k];
        c.ph = 0;
        c.dph = dsp::inc(4250.f + k * 170.f + rng.uni() * 60.f);
        c.period = 0.42f + rng.uni() * 0.35f;
        c.t = rng.uni() * c.period;
        c.amp = 0.5f + rng.uni() * 0.5f;
        c.on = c.target = k < 2 ? 1.f : 0.f;
        c.e = 0.f;
        c.pulses = 3 + (k & 1);
        c.pl = 0.016f; c.pg = 0.011f;
    }
    for (Croak& c : s_croak) { c.t = 0; c.next = rng.uni() * 4.f; c.left = 0; c.ph = 0; c.pulse = 0; c.pn = 0; }
    for (Peep& p : s_peep) { p.t = 1.f; p.next = rng.uni() * 2.f; p.ph = 0; p.f = 2600.f; }
    for (Thunder& t : s_th) t.on = false;
}

void natureBlock(Bus& b, int n, const NatScene& s) {
    windBlock(b, n, s);
    waterBlock(b, n, s);
    rainBlock(b, n, s);
    insectsBlock(b, n, s);
    birdsBlock(b, n, s);
    wingsBlock(b, n, s);
    thunderBlock(b, n);
    shotsBlock(b, n);
}

}
}
