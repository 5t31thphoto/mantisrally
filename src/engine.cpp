// Engines, tyres, surfaces, wind and the car's one-shots. Synthesised per sample at 24 kHz.
// Engine model: the crank turns a phase; each cylinder firing kicks a decaying exhaust pulse (its amplitude and
// timing from the engine's character: boxer rumble, I6 silk, cross-plane V8 lope, two-stroke ring), which rings
// a low body filter and three exhaust formants. Load brightens it; lifting off dulls it and lets it crackle.
#include "audio_int.h"
#include "audio.h"
#include "vehicle.h"
#include "journey.h"
#include "scene.h"
#include <math.h>
#include <string.h>

namespace mantis {
namespace snd {

using dsp::clampf;

struct EngChar {
    int fires;                      // firings per crank revolution
    int pattern;                    // pulses in the pattern
    float amp[8], off[8];           // per-firing amplitude and timing offset (fraction of an interval)
    float f1, f2, f3, q1, q2, q3;   // exhaust formants (Hz) and Qs
    float body, bright, jitter;     // body level, formant level, cycle jitter
};
static const EngChar CH[4] = {
    // boxer four (unequal headers): the rally burble
    {2, 4, {1.0f, 0.70f, 0.95f, 0.64f}, {0, 0.07f, 0, 0.05f}, 300.f, 980.f, 2500.f, 3.f, 4.f, 3.f, 0.55f, 0.75f, 0.03f},
    // inline six: smooth and thick
    {3, 6, {1.f, 0.94f, 0.97f, 0.95f, 0.99f, 0.93f}, {0, 0, 0, 0, 0, 0}, 240.f, 760.f, 1900.f, 2.5f, 3.f, 2.5f, 0.75f, 0.55f, 0.01f},
    // cross-plane V8: the lope
    {4, 8, {1.f, 0.6f, 0.92f, 0.7f, 0.97f, 0.56f, 0.86f, 0.76f}, {0, 0.04f, 0, 0.06f, 0, 0.03f, 0, 0.05f}, 180.f, 560.f, 1500.f, 3.f, 3.5f, 3.f, 0.9f, 0.6f, 0.04f},
    // two-stroke single: ring-ding
    {1, 1, {1.f}, {0}, 1100.f, 2400.f, 4200.f, 4.f, 6.f, 5.f, 0.3f, 1.1f, 0.06f},
};

static float s_ph = 0.f;            // position in the firing cycle (in firing intervals)
static int   s_k = 0;               // which firing is next
static float s_nextAt = 1.f;
static float s_pulse = 0.f, s_noisePulse = 0.f;
static dsp::SVF s_body, s_fA, s_fB, s_fC, s_intake, s_squeal, s_grav, s_road, s_wind, s_bov, s_pipe, s_wash;
static dsp::Pink s_pink;
static float s_rpmS = 900.f, s_thrS = 0.f, s_lastThr = 0.f, s_bovT = 0.f, s_popT = 0.f, s_grainE = 0.f, s_grainRate = 0.f;
static uint32_t s_whPh = 0, s_whine = 0;
static float s_sqG = 0.f, s_gravG = 0.f, s_windG = 0.f, s_roadG = 0.f, s_washG = 0.f;
// one-shots
struct Thump { float t, hz, amp, len; uint32_t ph; };
static Thump s_th[3];
static float s_crackE = 0.f, s_crackA = 0.f; static dsp::SVF s_crack;
static float s_hornT = 0.f; static uint32_t s_hornPh1 = 0, s_hornPh2 = 0;
static float s_splashE = 0.f; static dsp::SVF s_spl;
struct Chime { float t, hz, amp; uint32_t ph; };
static Chime s_ch[4];
// the awe pad
static dsp::Saw s_pad[3];
static dsp::SVF s_padLP;
static float s_padG = 0.f;

void engineInit() {
    memset(s_th, 0, sizeof(s_th)); memset(s_ch, 0, sizeof(s_ch));
    s_body.reset(); s_fA.reset(); s_fB.reset(); s_fC.reset();
}

void engineEvent(uint8_t kind, float a) {
    switch (kind) {
        case EE_BUMP: case EE_LAND: {
            for (Thump& t : s_th) if (t.t >= t.len) { t.t = 0.f; t.hz = kind == EE_LAND ? 70.f : 95.f; t.amp = clampf(a, 0.1f, 1.f) * (kind == EE_LAND ? 0.55f : 0.35f); t.len = 0.22f; t.ph = 0; break; }
            s_crackE = 1.f; s_crackA = clampf(a, 0.2f, 1.f) * (kind == EE_LAND ? 0.25f : 0.16f);
            break;
        }
        case EE_SHIFT: s_crackE = 1.f; s_crackA = 0.05f; if (a > 0.f && g_car.throttle > 0.6f && g_car.engine != ENG_I6) s_popT = 0.12f; break;
        case EE_HORN: s_hornT = 0.42f; break;
        case EE_SPLASH: s_splashE = fmaxf(s_splashE, clampf(a, 0.2f, 1.f)); break;
        case EE_CRASH: {
            s_crackE = 1.f; s_crackA = clampf(a * 0.08f, 0.2f, 0.6f);
            for (Thump& t : s_th) if (t.t >= t.len) { t.t = 0.f; t.hz = 60.f; t.amp = 0.7f; t.len = 0.35f; t.ph = 0; break; }
            break;
        }
        case EE_UI: {
            static const float NOTES[7][2] = {{1320, 0}, {990, 1320}, {784, 1175}, {659, 988}, {880, 1320}, {440, 330}, {660, 0}};
            int k = (int)a; if (k < 0 || k > 6) k = 0;
            for (int i = 0; i < 2; i++) { if (NOTES[k][i] <= 0.f) continue; Chime& c = s_ch[i]; c.t = -i * 0.09f; c.hz = NOTES[k][i]; c.amp = 0.10f; c.ph = 0; }
            break;
        }
    }
}

void engineBlock(Bus& b, int n) {
    const CarAudio& c = g_car;
    const EngChar& E = CH[c.engine & 3];
    const float dtb = (float)n / dsp::SR;
    // block-rate parameters
    float rpm = clampf(c.rpm, 400.f, 12000.f);
    float thr = clampf(c.throttle, 0.f, 1.f);
    if (s_lastThr > 0.55f && thr < 0.15f) {
        if (c.engine == ENG_BOXER4T && c.boost > 0.45f) s_bovT = 0.35f;          // blow-off valve
        if (c.rpm01 > 0.55f) s_popT = 0.55f + c.rpm01 * 0.4f;                     // the overrun crackle
    }
    s_lastThr = thr;
    float fireHz = rpm / 60.f * E.fires;
    float interval = 1.f / fireHz;
    float load = clampf(thr * 0.85f + 0.15f, 0.f, 1.f) * (c.limiter ? 0.2f : 1.f);
    float vol = (c.cockpit ? 0.85f : 0.65f);
    s_body.set(clampf(fireHz * 2.2f, 120.f, 900.f), 0.9f);
    float fsh = c.engine == ENG_2STROKE ? clampf(0.6f + c.rpm01 * 0.9f, 0.6f, 1.5f) : 1.f;      // the pipe "comes on"
    s_fA.set(E.f1 * fsh, E.q1); s_fB.set(E.f2 * fsh * (0.9f + 0.2f * load), E.q2); s_fC.set(E.f3 * (0.9f + 0.2f * c.rpm01), E.q3);
    s_intake.set(1800.f + c.rpm01 * 1600.f, 1.2f);
    s_pipe.set(clampf(fireHz * 6.f, 1500.f, 5200.f), 9.f);
    s_bov.set(3800.f, 1.5f);
    // tyres and surfaces
    float speed = c.speed;
    bool tar = c.surf == SURF_TARMAC;
    float sq = tar ? clampf((c.slide - 0.25f) * 1.6f + c.spin * 0.8f, 0.f, 1.f) * clampf(speed / 3.f, 0.f, 1.f) : 0.f;
    s_sqG += (sq * 0.11f - s_sqG) * clampf(dtb * 15.f, 0.f, 1.f);
    s_squeal.set(950.f + c.slide * 380.f + c.spin * 300.f, 7.f);
    float loose = tar ? 0.f : 1.f;
    s_grainRate = loose * (speed * 14.f + (c.slide + c.spin) * 900.f) * (c.air ? 0.f : 1.f);
    float gravTone = c.surf == SURF_SNOW ? 5200.f : (c.surf == SURF_GRAVEL ? 3200.f : (c.surf == SURF_SAND ? 4500.f : 1500.f));
    s_grav.set(gravTone, 1.4f);
    float roadT = (c.air ? 0.f : clampf(speed / 30.f, 0.f, 1.f)) * (tar ? 0.05f : (c.surf == SURF_MUD ? 0.09f : 0.07f));
    s_roadG += (roadT - s_roadG) * clampf(dtb * 8.f, 0.f, 1.f);
    s_road.set(c.surf == SURF_MUD ? 260.f : (tar ? 420.f : 650.f), 0.7f);
    float wk = clampf(speed / 45.f, 0.f, 1.2f);
    s_windG += (wk * wk * (c.cockpit ? 0.07f : 0.13f) + g_scene.wind * 0.02f - s_windG) * clampf(dtb * 4.f, 0.f, 1.f);
    s_wind.set(320.f + speed * 22.f, 0.8f);
    s_washG += ((c.water > 0.1f ? clampf(speed / 10.f, 0.2f, 1.f) * 0.12f : 0.f) - s_washG) * clampf(dtb * 6.f, 0.f, 1.f);
    s_wash.set(900.f, 0.6f);
    s_spl.set(1300.f, 0.8f);
    s_crack.set(600.f, 1.5f);
    uint32_t whDph = dsp::inc(2600.f + c.boost * 4200.f);
    uint32_t whineDph = dsp::inc(clampf(speed * 34.f, 40.f, 3000.f));
    float whG = c.engine == ENG_BOXER4T ? c.boost * thr * 0.018f : 0.f;
    float jeepWhine = c.engine == ENG_I6 ? clampf(speed / 25.f, 0.f, 1.f) * 0.012f : 0.f;
    float hornHz1 = c.engine == ENG_V8 ? 300.f : (c.engine == ENG_I6 ? 390.f : (c.engine == ENG_2STROKE ? 520.f : 430.f));
    uint32_t hd1 = dsp::inc(hornHz1), hd2 = dsp::inc(hornHz1 * 1.19f);
    // the awe pad: a soft chord when the world does something spectacular
    static const float CHORD[B_COUNT][3] = {{57, 64, 69}, {55, 62, 67}, {60, 64, 71}, {57, 60, 64}, {52, 59, 62}, {62, 69, 74},
                                            {50, 57, 62}, {60, 67, 72}, {65, 69, 72}, {57, 62, 66}, {53, 60, 63}, {62, 66, 73}};
    const float* ch = CHORD[g_scene.biomeA % B_COUNT];
    for (int i = 0; i < 3; i++) s_pad[i].dph = dsp::inc(dsp::midiHz(ch[i] - 12.f) * (1.f + 0.003f * i));
    s_padG += (g_scene.awe * 0.045f - s_padG) * clampf(dtb * 0.4f, 0.f, 1.f);
    s_padLP.set(500.f + g_scene.awe * 900.f, 0.7f);

    for (int i = 0; i < n; i++) {
        // ---- crank ----
        s_ph += 1.f / (interval * dsp::SR);
        if (s_ph >= s_nextAt) {
            int k = s_k % E.pattern;
            float a = E.amp[k] * (1.f + (rng.bi()) * E.jitter * 3.f);
            bool fire = true;
            if (c.limiter && (rng.next() & 3)) fire = false;                            // the limiter cuts sparks
            if (c.engine == ENG_2STROKE && thr < 0.25f && c.rpm01 < 0.45f && (rng.next() & 1)) fire = false;   // four-stroking
            if (fire) { s_pulse += a * (0.35f + 0.65f * load); s_noisePulse += a * load * 0.6f; }
            if (s_popT > 0.f && (rng.next() & 7) == 0) { s_pulse += 1.6f + rng.uni() * 1.4f; s_noisePulse += 1.2f; }   // backfire
            s_k++;
            s_ph -= s_nextAt;
            s_nextAt = 1.f + E.off[(s_k) % E.pattern] - E.off[k];
        }
        float nz = rng.bi();
        float ex = s_pulse + s_noisePulse * nz;
        s_pulse *= 0.955f; s_noisePulse *= 0.93f;
        float body = s_body.lp(ex) * E.body;
        float form = (s_fA.bp(ex) + s_fB.bp(ex) * 0.8f + s_fC.bp(ex) * 0.45f * load) * E.bright * (0.45f + 0.55f * load);
        float eng = body + form;
        if (c.engine == ENG_2STROKE) eng += s_pipe.bp(ex) * 0.6f * (0.3f + 0.7f * c.rpm01);
        eng += s_intake.bp(nz) * thr * c.rpm01 * 0.06f;
        s_whPh += whDph; eng += dsp::sine(s_whPh) * whG;
        s_whine += whineDph; eng += dsp::sine(s_whine) * jeepWhine;
        if (s_bovT > 0.f) { eng += s_bov.bp(nz) * s_bovT * 0.35f; s_bovT -= 1.f / dsp::SR; }
        if (s_popT > 0.f) s_popT -= 1.f / dsp::SR;
        eng = dsp::softclip(eng * 1.4f) * vol;
        // ---- tyres, ground, air ----
        float tyre = 0.f;
        if (s_sqG > 0.002f) tyre += s_squeal.bp(nz) * s_sqG * (0.8f + 0.2f * dsp::sine(s_whPh >> 3));
        if (s_grainRate > 1.f && rng.uni() < s_grainRate / dsp::SR) s_grainE = 0.4f + rng.uni() * 0.6f;
        if (s_grainE > 0.001f) { tyre += s_grav.bp(nz) * s_grainE * 0.35f; s_grainE *= 0.86f; }
        tyre += s_road.lp(nz) * s_roadG;
        float wind = s_wind.bp(s_pink.tick(nz)) * s_windG;
        float wash = s_washG > 0.002f ? s_wash.lp(nz) * s_washG : 0.f;
        // ---- one-shots ----
        float fx = 0.f;
        for (Thump& t : s_th) if (t.t < t.len) {
            float e = 1.f - t.t / t.len; e *= e;
            t.ph += dsp::inc(t.hz * (0.8f + 0.4f * e));
            fx += dsp::sine(t.ph) * e * t.amp;
            t.t += 1.f / dsp::SR;
        }
        if (s_crackE > 0.001f) { fx += s_crack.bp(nz) * s_crackE * s_crackA; s_crackE *= 0.996f; }
        if (s_hornT > 0.f) {
            s_hornPh1 += hd1; s_hornPh2 += hd2;
            float sq1 = (s_hornPh1 >> 31) ? 1.f : -1.f, sq2 = (s_hornPh2 >> 31) ? 1.f : -1.f;
            float e = clampf(s_hornT * 30.f, 0.f, 1.f);
            fx += (sq1 + sq2 * (c.engine == ENG_2STROKE ? 0.f : 1.f)) * 0.07f * e;
            s_hornT -= 1.f / dsp::SR;
        }
        if (s_splashE > 0.001f) { fx += s_spl.lp(nz) * s_splashE * 0.5f; s_splashE *= 0.99985f; if (s_splashE < 0.01f) s_splashE = 0.f; }
        float ui = 0.f;
        for (Chime& ch : s_ch) {
            if (ch.amp <= 0.f) continue;
            ch.t += 1.f / dsp::SR;
            if (ch.t < 0.f) continue;
            ch.ph += dsp::inc(ch.hz);
            float e = expf(-ch.t * 7.f);
            ui += dsp::sine(ch.ph) * ch.amp * e;
            if (ch.t > 0.8f) ch.amp = 0.f;
        }
        float pad = 0.f;
        if (s_padG > 0.001f) { pad = s_padLP.lp(s_pad[0].tick() + s_pad[1].tick() + s_pad[2].tick()) * s_padG; }
        b.dry[i] += eng + tyre + wind + wash + fx + ui + pad * 0.6f;
        b.rev[i] += eng * 0.12f + fx * 0.25f + ui * 0.5f + pad;
        b.dly[i] += (s_popT > 0.f ? eng * 0.15f : 0.f) + ui * 0.2f;
    }
}

}
}
