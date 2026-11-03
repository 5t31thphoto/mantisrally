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
    // boxer four (unequal headers): the rally burble -- the reference engine, untouched
    {2, 4, {1.0f, 0.70f, 0.95f, 0.64f}, {0, 0.07f, 0, 0.05f}, 300.f, 980.f, 2500.f, 3.f, 4.f, 3.f, 0.55f, 0.75f, 0.03f},
    // the Jeep's nearly stock 4.0 six: a dull, low chug (fat slow pulses, low resonances, little top end)
    {3, 6, {1.f, 0.86f, 0.97f, 0.82f, 0.99f, 0.88f}, {0, 0.02f, 0, 0.03f, 0, 0.02f}, 120.f, 330.f, 780.f, 1.8f, 2.2f, 1.8f, 1.45f, 0.20f, 0.03f},
    // blown V8 on open pipes: deeper throat, more rumble, raspy rip on top
    {4, 8, {1.f, 0.55f, 0.95f, 0.66f, 1.f, 0.52f, 0.88f, 0.72f}, {0, 0.05f, 0, 0.07f, 0, 0.04f, 0, 0.06f}, 100.f, 320.f, 1100.f, 2.6f, 2.8f, 2.2f, 1.7f, 0.9f, 0.055f},
    // CR250 on a pipe: lower, with a throat under the snarl
    {1, 1, {1.f}, {0}, 520.f, 1300.f, 2600.f, 2.5f, 3.f, 3.f, 0.95f, 1.0f, 0.07f},
};
// how long each firing's pulse rings (longer = fatter, lower; shorter = sharper)
static const float PULSE_DECAY[4] = {0.955f, 0.978f, 0.972f, 0.935f};

static float s_ph = 0.f;            // position in the firing cycle (in firing intervals)
static int   s_k = 0;               // which firing is next
static float s_nextAt = 1.f;
static float s_pulse = 0.f, s_noisePulse = 0.f;
static dsp::SVF s_body, s_fA, s_fB, s_fC, s_intake, s_squeal, s_grav, s_road, s_wind, s_bov, s_pipe, s_wash;
static dsp::Pink s_pink;
static float s_rpmS = 900.f, s_thrS = 0.f, s_lastThr = 0.f, s_bovT = 0.f, s_popT = 0.f, s_grainE = 0.f, s_grainRate = 0.f;
static uint32_t s_whPh = 0, s_whine = 0, s_blowPh = 0;
static float s_sqG = 0.f, s_gravG = 0.f, s_windG = 0.f, s_roadG = 0.f, s_washG = 0.f;
// one-shots
struct Thump { float t, hz, amp, len; uint32_t ph; };
static Thump s_th[3];
static float s_crackE = 0.f, s_crackA = 0.f; static dsp::SVF s_crack;
static float s_hornT = 0.f; static uint32_t s_hornPh1 = 0, s_hornPh2 = 0; static dsp::SVF s_hornF;
// the clutch: a gear change cuts the engine's load for a moment, then it bites again
static float s_shiftCut = 0.f, s_bite = 0.f;
// Before/after switches for the designer's comparison (0 = sound pass 2 exactly):
//  1 shift timing (clutch in while the revs still hang at the old gear, a sag, then the bite; a blip on downshifts)
//  2 exhaust pipes (delay-line pipe resonance; the V8 split into two bank pipes in its cross-plane firing order)
//  4 tonal squeal (a stick-slip tone that flutters and jumps between nearby modes, over a little noise)
//  8 gravel through one body (all the grains coloured by a shared wheel-well / floor-pan resonance)
// 16 a lumpier STI idle
uint32_t g_sndVariant = 1 | 2;                  // the designer's picks: the new gear changes and the exhaust pipes
static uint8_t s_bankB = 0;
static float s_lastRpm = 900.f, s_holdRpm = 0.f, s_holdT = 0.f, s_blipT = 0.f;
// exhaust pipes: two short delay lines (a bank each on the V8), damped in the loop
static float s_pipeA[400], s_pipeB[400]; static int s_pipeW = 0; static float s_pipeLA = 0.f, s_pipeLB = 0.f;
static const uint8_t V8_BANK[8] = {0, 1, 0, 0, 1, 1, 1, 0};   // firing order 1-8-4-3-6-5-7-2 -> left / right bank
// tonal squeal
static uint32_t s_sqPh = 0; static float s_sqMode = 1.f, s_sqFlut = 1.f; static dsp::SVF s_sqTone;
// The squeal (measured tyres squeal with peaks in 500-1500 Hz; friction squeal is a fundamental plus harmonics, with a
// second fundamental): two rough harmonic tones that compete, stick-slip wavering in level, a slight pitch wander.
static uint32_t s_sq1 = 0, s_sq2 = 0; static float s_sqAM = 1.f, s_sqAMt = 0.f, s_sqWan = 0.f, s_sqMix = 0.5f, s_sqMixT = 0.5f;
static dsp::SVF s_scrub;
// The screech: pink noise, high-passed at 300 Hz, through a sharply resonant band-pass (~2.5 kHz) whose frequency is
// pushed around by a slow, irregular sample-and-hold within 1-4 kHz; an inverted pitch envelope at the start (the
// resonance drops into place); an undulating, stuttering level (tread micro-bounces); soft-clip saturation for grit.
static float s_pk0 = 0.f, s_pk1 = 0.f, s_pk2 = 0.f, s_shT = 0.f, s_shV = 1.f, s_shS = 1.f;
static dsp::SVF s_scHP, s_scBP;
static float s_screechRev = 0.f;
// matched to the reference recordings: a tonal core near 950 Hz that jumps pitch every 25-50 ms and hands over to its
// 2nd harmonic now and then (the Chrysler squeal), a resonant noise layer (the rubber screech), a deep 7-9 Hz pulse
static float s_tqF = 950.f, s_tqTgt = 950.f, s_tqT = 0.f, s_tqH2 = 0.4f, s_tqH2t = 0.4f, s_lfoPh = 0.f, s_lfoHz = 8.f;
static uint32_t s_tqPh = 0; static dsp::SVF s_sqLP;
// the squeal is three layers (the designer's call): the first version's breath and stick-snap chatter, the recipe's
// bright resonant screech, and the measured tonal core with its pulse
static dsp::SVF s_scBP2, s_sqBody; static float s_chatT = 0.f, s_chat = 1.f, s_chatTgt = 1.f;
static inline float partials(uint32_t ph) {          // five harmonics, falling off: bright and rich, not a flute
    return dsp::sine(ph) + 0.5f * dsp::sine(ph * 2u) + 0.28f * dsp::sine(ph * 3u) + 0.14f * dsp::sine(ph * 4u) + 0.07f * dsp::sine(ph * 5u);
}
// gravel's shared body
static dsp::SVF s_bodyA, s_bodyB;
// idle: an engine at idle hunts and lopes, it isn't a steady oscillator
static float s_idleWob = 0.f, s_idleTgt = 0.f;
static float s_knockE = 0.f; static dsp::SVF s_knock;
// tyres: the "skrrt" attack when a slide starts (stick-slip chatter), before the sustained squeal
static float s_sqPrev = 0.f, s_skrrtT = 1.f; static uint32_t s_chatPh = 0; static dsp::SVF s_skrrt;
// loose ground: low washing grains, and the odd stone ringing off the floor pan (muffled)
struct SGrain { float t, len, amp; dsp::SVF f; };
static SGrain s_sg[6];
static float s_dingT = 1.f, s_dingE = 0.f; static dsp::SVF s_ding, s_cabin;
static dsp::SVF s_thLP;
static float s_splashE = 0.f; static dsp::SVF s_spl;
struct Chime { float t, hz, amp, glide, dec; uint8_t wave; uint32_t ph; };   // wave: 0 sine 1 triangle 2 square 3 noise click
static Chime s_ch[4];
// the awe pad
static dsp::Saw s_pad[3];
static dsp::SVF s_padLP;
static float s_padG = 0.f;
#ifdef HOST
uint32_t g_stemMute = 0;   // desktop stems only: 1 engine, 2 tyres + road, 4 wind + wash, 8 one-shots, 16 the pad
#endif

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
        case EE_SHIFT:                                     // the driver clutches: load cut, a clunk, then the bite
            s_shiftCut = g_car.engine == ENG_I6 ? 0.22f : 0.14f;
            if (g_sndVariant & 1) {                         // the revs hang at the old gear while the clutch is in
                s_holdRpm = s_lastRpm; s_holdT = g_car.engine == ENG_I6 ? 0.12f : 0.07f;
                if (a < 0.f) s_blipT = 0.09f;               // a downshift: blip the throttle to match revs
            }
            s_crackE = 1.f; s_crackA = 0.07f;
            for (Thump& t : s_th) if (t.t >= t.len) { t.t = 0.f; t.hz = 130.f; t.amp = 0.12f; t.len = 0.07f; t.ph = 0; break; }
            if (a > 0.f && g_car.throttle > 0.6f && g_car.engine != ENG_I6) s_popT = 0.12f;
            break;
        case EE_HORN: s_hornT = 0.42f; break;
        case EE_SPLASH: s_splashE = fmaxf(s_splashE, clampf(a, 0.2f, 1.f)); break;
        case EE_CRASH: {
            s_crackE = 1.f; s_crackA = clampf(a * 0.08f, 0.2f, 0.6f);
            for (Thump& t : s_th) if (t.t >= t.len) { t.t = 0.f; t.hz = 0.f; t.amp = 0.45f; t.len = 0.08f; t.ph = 0; break; }
            break;
        }
        case EE_UI: {
            // seven different sounds, not one chime at seven pitches:
            //  0 tick: a soft click   1 ok: a little rising blip   2 sold: a register's bell and drawer
            //  3 arrive: a warm rising arpeggio   4 fork: a relay's tick-tock   5 warn: a low double buzz   6 mute: a falling blip
            int k = (int)a; if (k < 0 || k > 6) k = 0;
            for (Chime& c : s_ch) c.amp = 0.f;
            auto set = [&](int i, float t0, float hz, float glide, float amp, float dec, uint8_t wave) {
                Chime& c = s_ch[i]; c.t = -t0; c.hz = hz; c.glide = glide; c.amp = amp; c.dec = dec; c.wave = wave; c.ph = 0; };
            switch (k) {
                case 0: set(0, 0.f, 2600.f, 1.f, 0.10f, 120.f, 3); break;
                case 1: set(0, 0.f, 620.f, 1.6f, 0.07f, 22.f, 1); break;
                case 2: set(0, 0.f, 2100.f, 1.f, 0.06f, 6.f, 0); set(1, 0.f, 3420.f, 1.f, 0.04f, 7.f, 0); set(2, 0.16f, 140.f, 0.7f, 0.12f, 30.f, 3); break;
                case 3: set(0, 0.f, 392.f, 1.f, 0.06f, 3.f, 1); set(1, 0.12f, 494.f, 1.f, 0.055f, 3.f, 1); set(2, 0.24f, 587.f, 1.f, 0.05f, 2.5f, 1); set(3, 0.36f, 784.f, 1.f, 0.04f, 2.f, 1); break;
                case 4: set(0, 0.f, 1800.f, 1.f, 0.09f, 140.f, 3); set(1, 0.33f, 1400.f, 1.f, 0.08f, 140.f, 3); break;
                case 5: set(0, 0.f, 180.f, 1.f, 0.06f, 9.f, 2); set(1, 0.16f, 180.f, 1.f, 0.06f, 9.f, 2); break;
                case 6: set(0, 0.f, 700.f, 0.5f, 0.06f, 14.f, 1); break;
            }
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
    float rpmIn = rpm;
    if ((g_sndVariant & 1) && s_holdT > 0.f) {            // clutch in: the old gear's revs, sagging, until it bites
        float k = s_holdT / 0.07f; rpm = s_holdRpm * (0.9f + 0.1f * fminf(k, 1.f));
        s_holdT -= dtb;
    }
    if ((g_sndVariant & 1) && s_blipT > 0.f) { rpm *= 1.f + 0.25f * (s_blipT / 0.09f); s_blipT -= dtb; }
    s_lastRpm = rpmIn;
    // idle hunts a little (a slow random walk), so the idle sounds like an engine, not an oscillator
    float idleK = clampf(1.f - c.rpm01 * 4.f, 0.f, 1.f);
    if (rng.uni() < dtb * 2.f) s_idleTgt = rng.bi();
    s_idleWob += (s_idleTgt - s_idleWob) * clampf(dtb * 3.f, 0.f, 1.f);
    rpm *= 1.f + s_idleWob * ((g_sndVariant & 16) && c.engine == ENG_BOXER4T ? 0.055f : 0.03f) * idleK;
    float thr = clampf(c.throttle, 0.f, 1.f);
    if (s_shiftCut > 0.f) { s_shiftCut -= dtb; thr *= 0.08f; if (s_shiftCut <= 0.f) s_bite = 0.12f; }   // clutch in
    if ((g_sndVariant & 1) && s_blipT > 0.f) thr = 0.85f;                          // the blip
    if (s_bite > 0.f) s_bite -= dtb;
    if (s_lastThr > 0.55f && thr < 0.15f) {
        if (c.engine == ENG_BOXER4T && c.boost > 0.45f) s_bovT = 0.35f;          // blow-off valve
        if (c.rpm01 > 0.55f) s_popT = 0.55f + c.rpm01 * 0.4f;                     // the overrun crackle
    }
    s_lastThr = thr;
    float fireHz = rpm / 60.f * E.fires;
    float interval = 1.f / fireHz;
    float load = clampf(thr * 0.85f + 0.15f, 0.f, 1.f) * (c.limiter ? 0.2f : 1.f);
    float vol = (c.cockpit ? 0.85f : 0.65f) * (c.engine == ENG_V8 ? 1.25f : 1.f);   // the Stingray is the loudest thing on the road
    s_body.set(c.engine == ENG_2STROKE ? clampf(fireHz * 1.3f, 90.f, 380.f) : clampf(fireHz * 2.2f, 120.f, 900.f), 0.9f);   // the bike's throat sits low
    float fsh = c.engine == ENG_2STROKE ? clampf(0.6f + c.rpm01 * 0.9f, 0.6f, 1.5f) : 1.f;      // the pipe "comes on"
    s_fA.set(E.f1 * fsh, E.q1); s_fB.set(E.f2 * fsh * (0.9f + 0.2f * load), E.q2); s_fC.set(E.f3 * (0.9f + 0.2f * c.rpm01), E.q3);
    s_intake.set(1800.f + c.rpm01 * 1600.f, 1.2f);
    // pipe lengths (m): STI 2.2 (turbo-back, muffled), Jeep 2.6 (stock muffler), Stingray banks 1.75 / 1.9 (open)
    float pipeL = c.engine == ENG_I6 ? 2.6f : (c.engine == ENG_V8 ? 1.75f : 2.2f);
    auto clampI = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    int dA = clampI((int)(2.f * pipeL / 343.f * dsp::SR), 40, 399), dB = clampI((int)(2.f * 1.9f / 343.f * dsp::SR), 40, 399);
    float pipeFb = c.engine == ENG_V8 ? 0.62f : (c.engine == ENG_I6 ? 0.45f : 0.5f), pipeDamp = c.engine == ENG_V8 ? 0.55f : (c.engine == ENG_I6 ? 0.25f : 0.35f);
    bool pipes = (g_sndVariant & 2) && c.engine != ENG_2STROKE;
    s_pipe.set(clampf(fireHz * 3.f, 600.f, 2600.f), 3.5f);              // the expansion chamber: broad and meaty, not a notch
    s_bov.set(3800.f, 1.5f);
    // tyres and surfaces
    float speed = c.speed;
    bool tar = c.surf == SURF_TARMAC;
    float sq = tar ? clampf((c.slide - 0.25f) * 1.6f + c.spin * 0.8f, 0.f, 1.f) * clampf(speed / 3.f, 0.f, 1.f) : 0.f;
    s_sqG += (sq * 0.11f - s_sqG) * clampf(dtb * 15.f, 0.f, 1.f);
    s_squeal.set(950.f + c.slide * 380.f + c.spin * 300.f, 7.f);
    if (rng.uni() < dtb * 1.5f) s_sqMixT = 0.25f + rng.uni() * 0.5f;              // the two modes trade dominance
    s_sqMix += (s_sqMixT - s_sqMix) * clampf(dtb * 4.f, 0.f, 1.f);
    s_sqWan += (rng.bi() * 0.012f - s_sqWan) * clampf(dtb * 6.f, 0.f, 1.f);       // a slight wander
    float f1 = (760.f + c.slide * 280.f + c.spin * 200.f) * (1.f + s_sqWan) * (s_skrrtT < 0.28f ? 1.f + 0.25f * (1.f - s_skrrtT / 0.28f) : 1.f);
    s_scrub.set(1800.f, 0.7f);
    s_shT -= dtb;
    if (s_shT <= 0.f) { s_shT = 0.03f + rng.uni() * 0.06f; s_shV = 0.8f + rng.uni() * 0.45f; }   // sample-and-hold
    s_shS += (s_shV - s_shS) * clampf(dtb * 25.f, 0.f, 1.f);                                 // slewed a little
    float onsetDrop = s_skrrtT < 0.15f ? 1.f + 0.45f * (1.f - s_skrrtT / 0.15f) : 1.f;    // starts high, drops in
    float scF = clampf((1900.f + c.slide * 400.f + c.spin * 300.f) * s_shS * onsetDrop, 1000.f, 3200.f);
    s_scHP.set(300.f, 0.7f); s_scBP.set(scF, 6.f);
    s_tqT -= dtb;
    if (s_tqT <= 0.f) {
        s_tqT = 0.025f + rng.uni() * 0.025f;
        float base = 780.f + c.slide * 200.f + c.spin * 150.f;
        s_tqTgt = base * (1.f + rng.bi() * 0.22f);                                 // measured: ~25% pitch jumps
        s_tqH2t = rng.uni() < 0.25f ? 1.4f : 0.3f + rng.uni() * 0.3f;             // now and then the 2nd harmonic leads
        if (rng.uni() < 0.1f) s_lfoHz = 6.5f + rng.uni() * 3.f;
    }
    s_tqF += (s_tqTgt - s_tqF) * clampf(dtb * 90.f, 0.f, 1.f);
    s_tqH2 += (s_tqH2t - s_tqH2) * clampf(dtb * 60.f, 0.f, 1.f);
    s_sqLP.set(3000.f, 0.7f);
    float scF2 = clampf((2300.f + c.slide * 600.f + c.spin * 400.f) * s_shS * onsetDrop, 1000.f, 4000.f);
    s_scBP2.set(scF2, 14.f);
    s_sqBody.set(1350.f + c.slide * 250.f, 2.2f);
    float noiseMix = clampf(0.08f + c.slide * 0.1f + c.spin * 0.08f, 0.f, 0.3f);    // measured: the real squeal is mostly tone
    // a slide starts: SKRRT -- whenever the tyres cross into squealing; louder the faster they got there
    static bool armed = true; static float skAmp = 1.f;
    if (armed && sq >= 0.25f) { for (Thump& t : s_th) if (t.t >= t.len) { t.t = 0.f; t.hz = 0.f; t.amp = 0.12f; t.len = 0.1f; t.ph = 0; break; }
        s_skrrtT = 0.f; skAmp = clampf(0.55f + (sq - s_sqPrev) / fmaxf(dtb, 1e-3f) * 0.05f, 0.55f, 1.3f); armed = false; }
    if (sq < 0.08f) armed = true;
    s_sqPrev = sq;
    float skA = s_skrrtT < 0.28f ? (s_skrrtT < 0.01f ? s_skrrtT / 0.01f : expf(-(s_skrrtT - 0.01f) * 9.f)) : 0.f;
    s_skrrt.set(2500.f - s_skrrtT * 2400.f, 5.f);
    s_skrrtT += dtb;
    float loose = tar ? 0.f : 1.f;
    // loose ground: low tones washing as the stones shift (big grains, each its own low pitch), plus the odd stone
    // ringing off the floor pan, muffled by the body. A texture under the road roar, not a wall of noise.
    s_grainRate = loose * clampf(speed / 13.f, 0.f, 2.f) * (c.surf == SURF_SAND ? 10.f : 18.f) * (1.f + (c.slide + c.spin) * 0.8f) * (c.air ? 0.f : 1.f);
    float gLo = 150.f, gHi = 480.f, gAmp = 0.0125f;                            // a texture under the road roar
    switch (c.surf) {
        case SURF_DIRT: gLo = 100.f; gHi = 300.f; gAmp = 0.011f; break;
        case SURF_MUD:  gLo = 70.f;  gHi = 220.f; gAmp = 0.015f; break;
        case SURF_SNOW: gLo = 600.f; gHi = 1300.f; gAmp = 0.0045f; break;
        case SURF_SAND: gLo = 110.f; gHi = 260.f; gAmp = 0.0075f; break;
        default: break;
    }
    float dingRate = (c.surf == SURF_GRAVEL ? 2.2f : (c.surf == SURF_DIRT ? 0.7f : 0.f)) * clampf(speed / 15.f, 0.f, 1.5f) * (c.air ? 0.f : 1.f);
    s_cabin.set(c.cockpit ? 1500.f : 2600.f, 0.7f);
    s_bodyA.set(185.f, 2.f); s_bodyB.set(430.f, 2.5f);
    float roadT = (c.air ? 0.f : clampf(speed / 30.f, 0.f, 1.f)) * (tar ? 0.05f : (c.surf == SURF_MUD ? 0.06f : 0.055f));
    s_roadG += (roadT - s_roadG) * clampf(dtb * 8.f, 0.f, 1.f);
    s_road.set(c.surf == SURF_MUD ? 260.f : (tar ? 420.f : 650.f), 0.7f);
    float wk = clampf(speed / 45.f, 0.f, 1.2f);
    s_windG += (wk * wk * (c.cockpit ? 0.07f : 0.13f) + g_scene.wind * 0.02f - s_windG) * clampf(dtb * 4.f, 0.f, 1.f);
    s_wind.set(320.f + speed * 22.f, 0.8f);
    s_washG += ((c.water > 0.1f ? clampf(speed / 10.f, 0.2f, 1.f) * 0.12f : 0.f) - s_washG) * clampf(dtb * 6.f, 0.f, 1.f);
    s_wash.set(900.f, 0.6f);
    s_spl.set(1300.f, 0.8f);
    s_crack.set(420.f, 1.f);
    uint32_t whDph = dsp::inc(2600.f + c.boost * 4200.f);
    uint32_t whineDph = dsp::inc(clampf(speed * 34.f, 40.f, 3000.f));
    float whG = c.engine == ENG_BOXER4T ? c.boost * thr * 0.018f : 0.f;
    float jeepWhine = c.engine == ENG_I6 ? clampf(speed / 25.f, 0.f, 1.f) * 0.012f : 0.f;
    // the Roots blower: six lobes on rotors driven 1.6x the crank, louder with boost
    uint32_t blowDph = dsp::inc(clampf(rpm / 60.f * 1.6f * 6.f, 60.f, 4000.f));
    float blowG = c.engine == ENG_V8 ? (0.006f + 0.03f * thr * c.rpm01) : 0.f;
    // horns: STI a pair of electric disc horns, the Jeep one old disc horn (rasping, sagging), the Stingray loud
    // trumpet horns (brassy), the bike a thin little meep
    float hornHz1 = c.engine == ENG_V8 ? 370.f : (c.engine == ENG_I6 ? 330.f * (1.f - (0.42f - s_hornT) * 0.08f) : (c.engine == ENG_2STROKE ? 560.f : 420.f));
    float hornHz2 = c.engine == ENG_V8 ? 466.f : (c.engine == ENG_BOXER4T ? 500.f : 0.f);
    uint32_t hd1 = dsp::inc(hornHz1), hd2 = dsp::inc(hornHz2 > 0.f ? hornHz2 : 1.f);
    s_hornF.set(c.engine == ENG_V8 ? 900.f : (c.engine == ENG_I6 ? 1400.f : (c.engine == ENG_2STROKE ? 1700.f : 1100.f)), c.engine == ENG_V8 ? 1.5f : 2.5f);
    s_knock.set(3300.f, 6.f); s_ding.set(1400.f + rng.uni() * 600.f, 10.f); s_thLP.set(170.f, 0.8f);
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
            float lump = ((g_sndVariant & 16) && c.engine == ENG_BOXER4T) ? 0.4f : 0.22f;
            float a = E.amp[k] * (1.f + (rng.bi()) * (E.jitter * 3.f + idleK * lump));   // lumpier at idle
            bool fire = true;
            if (c.limiter && (rng.next() & 3)) fire = false;                            // the limiter cuts sparks
            if (c.engine == ENG_2STROKE && thr < 0.25f && c.rpm01 < 0.45f && (rng.next() & 1)) fire = false;   // four-stroking
            if (c.engine == ENG_I6 && (rng.next() % 220u) == 0u) fire = false;           // an old six misses now and then
            if (fire && pipes && c.engine == ENG_V8) s_bankB = V8_BANK[k & 7];
            if (fire) {
                float bite = s_bite > 0.f ? 1.25f : 1.f;                                // the clutch bites: a bark
                s_pulse += a * (0.35f + 0.65f * load) * bite;
                s_noisePulse += a * (load * (c.engine == ENG_V8 ? 0.95f : 0.6f) + idleK * 0.3f);   // V8 rasp; idle burble
                if (c.engine == ENG_I6 && load > 0.7f && c.rpm01 < 0.55f && (rng.next() % 70u) == 0u) s_knockE = 0.6f;   // a faint knock lugging
            }
            if (s_popT > 0.f && (rng.next() & 7) == 0) { s_pulse += 1.6f + rng.uni() * 1.4f; s_noisePulse += 1.2f; }   // backfire
            s_k++;
            s_ph -= s_nextAt;
            s_nextAt = 1.f + E.off[(s_k) % E.pattern] - E.off[k];
        }
        float nz = rng.bi();
        float ex = s_pulse + s_noisePulse * nz;
        if (pipes) {                                                                  // down the pipe(s): delay-line resonance
            int rA = (s_pipeW - dA + 400) % 400, rB = (s_pipeW - dB + 400) % 400;
            s_pipeLA += (s_pipeA[rA] - s_pipeLA) * pipeDamp; s_pipeLB += (s_pipeB[rB] - s_pipeLB) * pipeDamp;
            bool toB = c.engine == ENG_V8 && s_bankB;
            s_pipeA[s_pipeW] = (toB ? 0.f : ex) + s_pipeLA * pipeFb;
            s_pipeB[s_pipeW] = (toB ? ex : 0.f) + s_pipeLB * pipeFb;
            s_pipeW = (s_pipeW + 1) % 400;
            ex = (ex * 0.45f + (s_pipeA[(s_pipeW + 399) % 400] + (c.engine == ENG_V8 ? s_pipeB[(s_pipeW + 399) % 400] : 0.f)) * 0.55f) * (c.engine == ENG_BOXER4T ? 1.f : 1.2f);   // level-matched
        }
        s_pulse *= PULSE_DECAY[c.engine & 3]; s_noisePulse *= 0.93f;
        float body = s_body.lp(ex) * E.body;
        float form = (s_fA.bp(ex) + s_fB.bp(ex) * 0.8f + s_fC.bp(ex) * 0.45f * load) * E.bright * (0.45f + 0.55f * load);
        float eng = body + form;
        if (c.engine == ENG_2STROKE) { float on = c.rpm01 > 0.55f ? 1.f + (c.rpm01 - 0.55f) * 3.f : 0.4f + c.rpm01; eng += s_pipe.bp(ex) * 0.75f * on * (0.4f + 0.6f * thr); }
        eng += s_intake.bp(nz) * thr * c.rpm01 * (c.engine == ENG_I6 ? 0.012f : 0.06f);
        if (s_knockE > 0.001f) { eng += s_knock.bp(nz) * s_knockE * 0.05f; s_knockE *= 0.93f; }
        if (c.engine == ENG_V8) { s_blowPh += blowDph; eng += (dsp::sine(s_blowPh) * 0.7f + dsp::sine(s_blowPh * 2u) * 0.3f) * blowG; }
        s_whPh += whDph; eng += dsp::sine(s_whPh) * whG;
        s_whine += whineDph; eng += dsp::sine(s_whine) * jeepWhine;
        if (s_bovT > 0.f) { eng += s_bov.bp(nz) * s_bovT * 0.35f; s_bovT -= 1.f / dsp::SR; }
        if (s_popT > 0.f) s_popT -= 1.f / dsp::SR;
        eng = dsp::softclip(eng * 1.4f) * vol;
        if (pipes && c.engine != ENG_BOXER4T) eng *= c.engine == ENG_V8 ? 1.38f : 1.3f;   // level-matched for the comparison
        // ---- tyres, ground, air ----
        float tyre = 0.f;
        s_screechRev = 0.f;
        if (s_sqG > 0.002f || skA > 0.002f) {
            // stick-slip: the level wavers irregularly every few milliseconds (rougher during the SKRRT)
            s_sqAMt -= 1.f / dsp::SR;
            if (s_sqAMt <= 0.f) {
                bool sk = skA > 0.05f;
                s_sqAMt = (sk ? 0.012f : 0.006f) + rng.uni() * (sk ? 0.02f : 0.012f);
                s_sqAM = sk ? (rng.uni() < 0.35f ? 0.15f : 0.8f + rng.uni() * 0.4f) : 0.65f + rng.uni() * 0.5f;
            }
            float w = nz;                                                             // pink noise (Kellet)
            s_pk0 = 0.99765f * s_pk0 + w * 0.0990460f; s_pk1 = 0.96300f * s_pk1 + w * 0.2965164f; s_pk2 = 0.57000f * s_pk2 + w * 1.0526913f;
            float pink = (s_pk0 + s_pk1 + s_pk2 + w * 0.1848f) * 0.25f;
            float rs = s_scBP.bp(s_scHP.hp(pink)) * 3.f;                                  // the screech's resonant noise
            s_tqPh += dsp::inc(s_tqF * (1.f + nz * 0.015f));                              // the tonal core (a touch of jitter)
            float tq = dsp::sine(s_tqPh) * (1.6f - s_tqH2) + dsp::sine(s_tqPh * 2u) * s_tqH2 + dsp::sine(s_tqPh * 3u) * 0.25f;
            s_lfoPh += s_lfoHz / dsp::SR; if (s_lfoPh >= 1.f) s_lfoPh -= 1.f;
            float pl = 0.5f + 0.5f * dsp::sine((uint32_t)(s_lfoPh * 4294967295.f)); float pulse = 0.02f + 0.98f * pl * pl * sqrtf(pl);   // the deep pulse (measured ~1.0)
            float g = s_sqG + skA * skAmp * 0.9f;
            // layer 3: the measured tonal core with its deep pulse
            float v = s_sqLP.lp(dsp::softclip((tq * (1.f - noiseMix) * 0.8f + rs * noiseMix) * pulse * s_sqAM * 1.4f)) * 0.45f * 3.6f * g;
            // layer 2: the recipe's bright resonant screech
            float v2 = dsp::softclip(s_scBP2.bp(s_scHP.hp(pink)) * 4.f * s_sqAM * 2.2f) * 0.45f * 1.5f * g;
            // layer 1: the first version's breath, and the stick-snap chatter at the start
            s_chatT -= 1.f / dsp::SR;
            if (s_chatT <= 0.f) { s_chatT = 0.02f + rng.uni() * 0.025f; s_chatTgt = (rng.uni() < 0.5f) ? 1.f : 0.45f + 0.2f * rng.uni(); }
            s_chat += (s_chatTgt - s_chat) * 0.004f;                                      // smeared, not switched
            float v1 = s_squeal.bp(nz) * s_sqG * (0.8f + 0.2f * dsp::sine(s_whPh >> 3)) + s_skrrt.bp(nz) * skA * skAmp * s_chat * 0.22f;   // the chatter: lower
            v = v * 0.6f + v2 * 0.4f + v1 * 0.62f;                                  // the noise a little louder
            v = (v * 0.45f + s_sqBody.bp(v) * 1.5f) * 0.4f;                           // one resonant body: a single sound (level-matched)
            tyre += v;
            s_screechRev = v;
        }
        if (s_grainRate > 0.5f && rng.uni() < s_grainRate / dsp::SR) {             // a new low washing grain
            for (SGrain& g : s_sg) if (g.t >= g.len) {
                g.t = 0.f; g.len = 0.03f + rng.uni() * 0.05f; g.amp = gAmp * (0.5f + rng.uni());
                g.f.set(gLo + rng.uni() * (gHi - gLo), 3.f + rng.uni() * 4.f); break; }
        }
        float gs = 0.f;
        for (SGrain& g : s_sg) if (g.t < g.len) { float w = sinf(3.14159f * g.t / g.len); gs += g.f.bp(nz) * w * g.amp * 3.f; g.t += 1.f / dsp::SR; }
        if (dingRate > 0.f && rng.uni() < dingRate / dsp::SR) s_dingE = 0.5f + rng.uni() * 0.5f;   // a stone off the floor pan
        if (s_dingE > 0.001f) { gs += s_ding.bp(nz) * s_dingE * 0.03f; s_dingE *= 0.9985f; }
        if (g_sndVariant & 8) gs = gs * 0.15f + s_bodyA.bp(gs) * 0.7f + s_bodyB.bp(gs) * 0.55f;   // one body under it all (level-matched)
        tyre += s_cabin.lp(gs);                                                       // through the body
        tyre += s_road.lp(nz) * s_roadG;
        float wind = s_wind.bp(s_pink.tick(nz)) * s_windG;
        float wash = s_washG > 0.002f ? s_wash.lp(nz) * s_washG : 0.f;
        // ---- one-shots ----
        float fx = 0.f;
        for (Thump& t : s_th) if (t.t < t.len) {
            float e = 1.f - t.t / t.len; e *= e;
            t.ph += dsp::inc(t.hz * 0.7f);
            fx += (s_thLP.lp(nz) * 2.6f * (t.hz > 0.f ? 0.8f : 1.f) + (t.hz > 0.f ? dsp::sine(t.ph) * 0.2f : 0.f)) * e * t.amp;
            t.t += 1.f / dsp::SR;
        }
        if (s_crackE > 0.001f) { fx += s_crack.bp(nz) * s_crackE * s_crackA * 0.6f; s_crackE *= 0.993f; }
        if (s_hornT > 0.f) {
            s_hornPh1 += hd1; s_hornPh2 += hd2;
            float e1 = clampf(s_hornT * 30.f, 0.f, 1.f) * clampf((0.42f - s_hornT) * 60.f, 0.f, 1.f);
            float v;
            if (c.engine == ENG_V8) {                                                // trumpets: two bright saws, brassy
                v = ((s_hornPh1 >> 16) * (1.f / 32768.f) - 1.f) + ((s_hornPh2 >> 16) * (1.f / 32768.f) - 1.f);
                v = s_hornF.bp(v) * 2.2f + v * 0.15f; v *= 0.09f;
            } else if (c.engine == ENG_I6) {                                         // one old disc horn: a narrow pulse, rasping
                v = ((s_hornPh1 >> 30) == 0 ? 1.f : -0.33f) + nz * 0.15f;
                v = s_hornF.bp(v) * 1.8f + v * 0.25f; v *= 0.08f;
            } else if (c.engine == ENG_2STROKE) {                                    // a thin little meep
                v = (s_hornPh1 >> 31) ? 1.f : -1.f; v = s_hornF.bp(v) * 1.2f; v *= 0.05f;
            } else {                                                                 // two electric disc horns
                v = ((s_hornPh1 >> 31) ? 1.f : -1.f) + ((s_hornPh2 >> 31) ? 1.f : -1.f);
                v = s_hornF.bp(v) * 1.6f + v * 0.2f; v *= 0.065f;
            }
            fx += v * e1;
            s_hornT -= 1.f / dsp::SR;
        }
        if (s_splashE > 0.001f) { fx += s_spl.lp(nz) * s_splashE * 0.5f; s_splashE *= 0.99985f; if (s_splashE < 0.01f) s_splashE = 0.f; }
        float ui = 0.f;
        for (Chime& ch : s_ch) {
            if (ch.amp <= 0.f) continue;
            ch.t += 1.f / dsp::SR;
            if (ch.t < 0.f) continue;
            float hz = ch.hz * (1.f + (ch.glide - 1.f) * clampf(ch.t * 12.f, 0.f, 1.f));
            ch.ph += dsp::inc(hz);
            float env = expf(-ch.t * ch.dec) * clampf(ch.t * 400.f, 0.f, 1.f);
            float w;
            switch (ch.wave) {
                case 1: { float p = (ch.ph >> 16) * (1.f / 65536.f); w = p < 0.5f ? p * 4.f - 1.f : 3.f - p * 4.f; break; }
                case 2: w = (ch.ph >> 31) ? 0.7f : -0.7f; break;
                case 3: w = nz * (ch.t < 0.006f ? 1.f : 0.f) + dsp::sine(ch.ph) * 0.3f; break;
                default: w = dsp::sine(ch.ph); break;
            }
            ui += w * ch.amp * env;
            if (ch.t > 1.2f) ch.amp = 0.f;
        }
        float pad = 0.f;
        if (s_padG > 0.001f) { pad = s_padLP.lp(s_pad[0].tick() + s_pad[1].tick() + s_pad[2].tick()) * s_padG; }
#ifdef HOST
        if (g_stemMute & 1) eng = 0.f;
        if (g_stemMute & 2) tyre = 0.f;
        if (g_stemMute & 4) { wind = 0.f; wash = 0.f; }
        if (g_stemMute & 8) { fx = 0.f; ui = 0.f; }
        if (g_stemMute & 16) pad = 0.f;
#endif
        b.dry[i] += eng + tyre + wind + wash + fx + ui + pad * 0.6f;
        b.rev[i] += eng * 0.12f + fx * 0.25f + ui * 0.5f + pad + s_screechRev * 0.3f;   // the screech placed in a space
        b.dly[i] += (s_popT > 0.f ? eng * 0.15f : 0.f) + ui * 0.2f;
    }
}

}
}
