// The car radio. See radio.h.
//
// Each station is a timeline of segments, alternating SONG and BREAK, made from the station's seed as time reaches
// them (so all three stations advance on one clock whether or not you're listening). A song is a key, a tempo, a
// meter, two chord progressions and a lead motif, laid out as intro / A / B / A / B / outro; everything that happens
// on a 16th note comes from a hash of the song, the bar and the step, so tuning in mid-song just starts from the next
// step. A break is a DJ recording (and every third time an ad) over a soft bed, or the station's musical ident.
//
// Cost: at most 8 plucked strings, 4 electric-piano voices, a bass, a lead and four drums, all at 12 kHz; while the
// radio is off or paused nothing is synthesised at all.
#include "radio.h"
#include "audio.h"
#include "dsp.h"
#include "scene.h"
#include "mem.h"
#include <string.h>
#include <math.h>
#ifndef HOST
#include <SD.h>
#include "display.h"
#include "storage.h"
#endif

namespace mantis {

using dsp::clampf;

static constexpr int RSR = AUDIO_SR / 2;                 // the radio's own rate
static constexpr float RINV = 1.f / RSR;
static inline uint32_t rinc(float hz) { return (uint32_t)(int64_t)(hz * (4294967296.f / RSR)); }
static inline float midiHz(float m) { return 440.f * powf(2.f, (m - 69.f) * (1.f / 12.f)); }
static inline float sineAt(uint32_t ph) { return dsp::sine(ph); }
static inline int fold(int note, int lo, int hi) { while (note < lo) note += 12; while (note > hi) note -= 12; return note; }   // into [lo, hi]
// SVF coefficients for the radio's rate (dsp::SVF assumes 24 kHz: twice the frequency there is the same filter here)
static inline void setF(dsp::SVF& f, float hz, float q) { f.set(hz * 2.f, q); }

#ifdef HOST
float g_radioSolo[8] = {1, 1, 1, 1, 1, 1, 1, 1};     // host tools: each part's level (strings, piano, bass, lead, kick, snare, cymbals, record)
#define SOLO(k) g_radioSolo[k]
#else
#define SOLO(k) 1.f
#endif
static inline uint32_t hsh(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du ^ (d + 0x27D4EB2Fu) * 0x9E3779B9u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static inline float h01(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) { return (hsh(a, b, c, d) >> 8) * (1.f / 16777216.f); }

// ================================================================================================ stations
static const char* FREQ[RADIO_N] = {"OFF", "88.7 FM", "96.1 FM", "101.5 FM"};
static const char* NAME[RADIO_N] = {"RADIO OFF", "LO-FI MANTIS", "THE ODD METER", "BACKROADS"};
const char* radioFreq(uint8_t s) { return FREQ[s < RADIO_N ? s : 0]; }
const char* radioName(uint8_t s) { return NAME[s < RADIO_N ? s : 0]; }

static volatile uint8_t s_station = RADIO_OFF, s_vol = 4;
static volatile bool s_paused = false;
void radioSetStation(uint8_t s) { s_station = s < RADIO_N ? s : (uint8_t)RADIO_OFF; }
uint8_t radioStation() { return s_station; }
void radioSetVolume(int v) { s_vol = (uint8_t)(v < 1 ? 1 : (v > RADIO_VOL_MAX ? RADIO_VOL_MAX : v)); }
int radioVolume() { return s_vol; }
void radioSetPaused(bool p) { s_paused = p; }

// ================================================================================================ the recordings
// /radio/radio.bin (tools/radio/radio_studio.py): "MRAD", u16 version (1), u16 count, count x 16-byte entries
// {u8 station (0 = any), u8 kind, u16 rate, u32 samples, u32 offset (from the end of the table), u32 bytes}, then
// the clips: IMA ADPCM, 4 bits, low nibble first, decoder starting at predictor 0, index 0.
enum : uint8_t { CK_ID = 0, CK_TALK, CK_MORNING, CK_DAY, CK_EVENING, CK_NIGHT, CK_RAIN, CK_SNOW, CK_FOG, CK_CLEAR, CK_AD, CK_N };
struct Clip { uint8_t station, kind; uint16_t rate; uint32_t samples; const uint8_t* data; uint32_t bytes; };
static constexpr int CLIP_MAX = 160;
static Clip* s_clips = nullptr;                          // the table, in PSRAM (only when there is a bank)
static volatile int s_nClips = 0;
int radioClipCount() { return s_nClips; }

static void parseBank(const uint8_t* d, uint32_t n) {
    if (!d || n < 8 || memcmp(d, "MRAD", 4) != 0) return;
    int count = d[6] | (d[7] << 8);
    uint32_t table = 8 + (uint32_t)count * 16;
    if (table > n || count <= 0) return;
    if (count > CLIP_MAX) count = CLIP_MAX;
    Clip* clips = (Clip*)memSlow(sizeof(Clip) * count);
    if (!clips) return;
    int k = 0;
    for (int i = 0; i < count && k < CLIP_MAX; i++) {
        const uint8_t* e = d + 8 + i * 16;
        Clip c;
        c.station = e[0]; c.kind = e[1]; c.rate = (uint16_t)(e[2] | (e[3] << 8));
        c.samples = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        uint32_t off = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        c.bytes = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (c.kind >= CK_N || c.station >= RADIO_N || c.rate < 4000 || c.rate > 16000) continue;
        if (table + off + c.bytes > n || c.bytes < (c.samples + 1) / 2) continue;
        c.data = d + table + off;
        clips[k++] = c;
    }
    s_clips = clips;
    __atomic_store_n(&s_nClips, k, __ATOMIC_RELEASE);
}

void radioLoadClips() {
#ifndef HOST
    if (!storageOk()) return;
    static const uint32_t MAX_BYTES = 1536 * 1024;          // the tool keeps a bank under 1 MB; anything bigger is cut
    displayHoldBus();                                       // the card shares the SPI bus with the screen
    File f = SD.open("/radio/radio.bin");
    uint8_t* buf = nullptr; uint32_t n = 0;
    if (f) {
        n = f.size();
        if (n > MAX_BYTES) n = MAX_BYTES;
        buf = n ? (uint8_t*)memSlow(n) : nullptr;           // PSRAM: read a few times a minute, in order
        if (buf && f.read(buf, n) != (size_t)n) n = 0;
        f.close();
    }
    displayReleaseBus();
    if (buf && n) parseBank(buf, n);
#endif
}
#ifdef HOST
void radioUseBank(const uint8_t* data, uint32_t bytes) { parseBank(data, bytes); }
#endif

static const int8_t ADPCM_IDX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t ADPCM_STEP[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

struct ClipPlay {
    const Clip* c = nullptr;
    uint32_t pos = 0;             // samples decoded
    int pred = 0, idx = 0;
    float frac = 0.f, a = 0.f, b = 0.f, step = 0.f;   // resampling to the radio's rate
    int delay = 0;                // radio samples before it starts
    bool on = false;
    void start(const Clip* clip, int delaySamples) {
        c = clip; pos = 0; pred = 0; idx = 0; frac = 0.f; a = b = 0.f; delay = delaySamples; on = clip != nullptr;
        step = clip ? (float)clip->rate * RINV : 0.f;
    }
    inline float next1() {                         // one sample at the clip's own rate
        if (pos >= c->samples) return 0.f;
        uint8_t byte = c->data[pos >> 1];
        int nib = (pos & 1) ? (byte >> 4) : (byte & 15);
        pos++;
        int st = ADPCM_STEP[idx];
        int diff = st >> 3;
        if (nib & 4) diff += st;
        if (nib & 2) diff += st >> 1;
        if (nib & 1) diff += st >> 2;
        pred += (nib & 8) ? -diff : diff;
        if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
        idx += ADPCM_IDX[nib];
        if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
        return pred * (1.f / 32768.f);
    }
    inline float tick() {
        if (!on) return 0.f;
        if (delay > 0) { delay--; return 0.f; }
        frac += step;
        while (frac >= 1.f) { frac -= 1.f; a = b; b = next1(); }
        if (pos >= c->samples && frac >= 0.999f) on = false;
        return a + (b - a) * frac;
    }
    float secs() const { return c ? (float)c->samples / c->rate : 0.f; }
};

// ================================================================================================ voices
static dsp::Rng s_rng;

// Plucked strings (Karplus-Strong with a fractional delay): guitars, banjo, upright and picked bass.
static constexpr int KS_N = 512, KS_V = 8;
struct KS {
    int16_t* buf = nullptr;
    int w = 0, excite = 0;
    float L = 100.f, decay = 0.996f, amp = 0.f, exLP = 0.f, bright = 0.5f, prev = 0.f, gain = 0.f;
    int delay = 0;
    bool on = false;
    void pluck(float hz, float a, float br, float ring, int del = 0) {
        L = clampf((float)RSR / hz - 0.5f, 2.f, KS_N - 4.f);
        amp = a; bright = br; excite = (int)L + 1; delay = del; on = true; gain = 1.f;
        // ring: seconds for the note to fall 60 dB (the averaging filter takes its own share)
        decay = powf(0.001f, 1.f / (ring * hz));
        decay = clampf(decay, 0.9f, 0.99995f);
    }
    void damp(float k) { gain *= k; }
    inline float tick() {
        if (delay > 0) { delay--; return 0.f; }
        float rp = (float)w - L;
        if (rp < 0.f) rp += KS_N;
        int i0 = (int)rp; float f = rp - i0;
        int i1 = (i0 + 1) & (KS_N - 1), im = (i0 - 1) & (KS_N - 1);
        float x0 = buf[i0] + (buf[i1] - buf[i0]) * f, xm = buf[im] + (buf[i0] - buf[im]) * f;
        float y = (x0 + xm) * 0.5f * decay * (1.f / 8192.f) * gain;
        if (excite > 0) { exLP += bright * (s_rng.bi() - exLP); y += exLP * amp; excite--; }
        float s = clampf(y * 8192.f, -32767.f, 32767.f);
        buf[w] = (int16_t)s;
        w = (w + 1) & (KS_N - 1);
        if (excite <= 0 && fabsf(y) < 0.0002f) { if (++quiet > 600) on = false; } else quiet = 0;
        return y;
    }
    int quiet = 0;
};
static KS s_ks[KS_V];

// Electric piano: two-operator FM, the bell of the attack fading into a round tone
struct EP {
    uint32_t pc = 0, pm = 0, dc = 0, dm = 0;
    float env = 0.f, dec = 0.9999f, idx = 0.f, idxDec = 0.9995f, amp = 0.f;
    int delay = 0;
    bool on = false;
    void play(float hz, float a, int del, float sustainSec) {
        dc = rinc(hz); dm = rinc(hz * 1.0f); amp = a; env = 1.f; idx = 1.6f; delay = del; on = true;
        dec = powf(0.001f, 1.f / (sustainSec * RSR)); idxDec = powf(0.2f, 1.f / (0.25f * RSR));
    }
    inline float tick(float wow) {
        if (delay > 0) { delay--; return 0.f; }
        pm += (uint32_t)(dm * wow); pc += (uint32_t)(dc * wow);
        float m = sineAt(pm);
        float s = sineAt(pc + (uint32_t)(int32_t)(m * idx * 683565275.6f));
        env *= dec; idx = 0.35f + (idx - 0.35f) * idxDec;
        if (env < 0.0005f) on = false;
        return s * env * amp;
    }
};
static EP s_ep[4];
static int s_epNext = 0;

// Bass: a round sine with a touch of its octave (lo-fi), or a plucked string
struct Bass {
    uint32_t ph = 0, dph = 0; float env = 0.f, dec = 0.9998f, amp = 0.f; int delay = 0;
    void play(float hz, float a, float len, int del) { dph = rinc(hz); amp = a; env = 1.f; dec = powf(0.01f, 1.f / (len * RSR)); delay = del; }
    inline float tick(float wow) {
        if (delay > 0) { delay--; return 0.f; }
        if (env < 0.0003f) return 0.f;
        ph += (uint32_t)(dph * wow);
        float x = sineAt(ph) + 0.22f * sineAt(ph << 1);
        env *= dec;
        float a = env > 0.97f ? (1.f - env) * 33.f : 1.f;   // a soft attack, no click
        return x * env * a * amp;
    }
};
static Bass s_bass;

// The lead: flute (lo-fi), fiddle and harmonica (country)
enum : uint8_t { LEAD_FLUTE, LEAD_FIDDLE, LEAD_HARP };
struct Lead {
    dsp::Saw saw; uint32_t sph = 0, vib = 0; dsp::SVF f, f2;
    float hz = 220.f, target = 220.f, env = 0.f, envTarget = 0.f, amp = 0.f; int kind = LEAD_FLUTE;
    int gateLeft = 0;
    void note(float h, float a, int lenSamples) { target = h; if (env < 0.02f) hz = h; envTarget = 1.f; amp = a; gateLeft = lenSamples; }
    inline float tick(float wow) {
        if (gateLeft > 0) { if (--gateLeft == 0) envTarget = 0.f; }
        float rate = envTarget > env ? (kind == LEAD_FLUTE ? 0.004f : 0.006f) : 0.0025f;
        env += (envTarget - env) * rate;
        if (env < 0.0005f && envTarget == 0.f) return 0.f;
        hz += (target - hz) * 0.006f;
        vib += rinc(kind == LEAD_FIDDLE ? 5.6f : 4.8f);
        float vd = 1.f + (kind == LEAD_FLUTE ? 0.004f : 0.007f) * sineAt(vib) * clampf(env * 1.5f - 0.3f, 0.f, 1.f);
        float h = hz * vd * wow;
        float x;
        if (kind == LEAD_FLUTE) {
            sph += rinc(h);
            x = sineAt(sph) + 0.08f * sineAt(sph * 3u) + s_rng.bi() * 0.05f * env;
            x = f.lp(x);
        } else {
            saw.dph = rinc(h);
            float s = saw.tick();
            x = kind == LEAD_FIDDLE ? f.bp(s) * 1.6f + f2.lp(s) * 0.25f : f.bp(s) * 1.2f + f2.bp(s) * 0.6f;
        }
        return x * env * amp;
    }
};
static Lead s_lead;

// Drums
struct Kick { uint32_t ph = 0; float hz = 50.f, env = 0.f, amp = 0.f, click = 0.f; int delay = 0;
    void hit(float a, int del = 0) { env = 1.f; amp = a; hz = 120.f; ph = 0; click = a; delay = del; }
    inline float tick() { if (delay > 0) { delay--; return 0.f; } if (env < 0.0005f) return 0.f;
        ph += rinc(hz); hz = 44.f + (hz - 44.f) * 0.9965f; env *= 0.99945f; click *= 0.8f;
        return sineAt(ph) * env * amp + s_rng.bi() * click * 0.25f; } };
struct Snare { dsp::SVF f; uint32_t ph = 0; float env = 0.f, tone = 0.f, amp = 0.f, dec = 0.9985f; int delay = 0; bool brush = false;
    void hit(float a, bool br, int del = 0) { env = 1.f; tone = br ? 0.f : 1.f; amp = a; brush = br; delay = del;
        dec = br ? 0.99935f : 0.99855f; setF(f, br ? 2600.f : 1900.f, br ? 0.6f : 0.9f); }
    inline float tick() { if (delay > 0) { delay--; return 0.f; } if (env < 0.0005f) return 0.f;
        ph += rinc(185.f); float n = s_rng.bi(); float x = (brush ? f.lp(n) * 0.8f : f.bp(n) * 1.6f) * env + sineAt(ph) * tone * 0.5f;
        float a = brush ? clampf((1.f - env) * 25.f, 0.f, 1.f) * 0.7f + 0.3f : 1.f;
        env *= dec; tone *= 0.9975f; return x * amp * a; } };
struct Hat { dsp::SVF f; float env = 0.f, amp = 0.f, dec = 0.993f; int delay = 0; bool init = false;
    void hit(float a, float len, int del = 0) { if (!init) { setF(f, 4200.f, 0.7f); init = true; } env = 1.f; amp = a; dec = powf(0.01f, 1.f / (len * RSR)); delay = del; }
    inline float tick() { if (delay > 0) { delay--; return 0.f; } if (env < 0.0005f) return 0.f;
        float n = s_rng.bi(); float x = n - f.lp(n); env *= dec; return x * env * amp; } };
static Kick s_kick; static Snare s_snare; static Hat s_hat, s_ride;

// ================================================================================================ harmony
enum : uint8_t { Q_MAJ, Q_MIN, Q_MAJ7, Q_MIN7, Q_DOM7, Q_ADD9, Q_SUS2, Q_SUS4, Q_MAJ9, Q_MIN9, Q_N };
static const int8_t QUAL[Q_N][5] = {
    {0, 4, 7, 12, 16}, {0, 3, 7, 12, 15}, {0, 4, 7, 11, 14}, {0, 3, 7, 10, 14}, {0, 4, 7, 10, 14},
    {0, 4, 7, 14, 16}, {0, 2, 7, 12, 14}, {0, 5, 7, 12, 17}, {0, 4, 11, 14, 19}, {0, 3, 10, 14, 17}};
struct Chord { int8_t root; uint8_t q; };
struct Prog { uint8_t n; Chord c[4]; };
static const Prog LOFI_PROG[] = {
    {4, {{2, Q_MIN9}, {7, Q_DOM7}, {0, Q_MAJ9}, {9, Q_MIN7}}}, {4, {{0, Q_MAJ7}, {9, Q_MIN9}, {2, Q_MIN7}, {7, Q_DOM7}}},
    {4, {{5, Q_MAJ9}, {4, Q_MIN7}, {2, Q_MIN9}, {0, Q_MAJ7}}}, {4, {{9, Q_MIN9}, {5, Q_MAJ7}, {0, Q_MAJ9}, {7, Q_SUS4}}},
    {2, {{0, Q_MAJ9}, {5, Q_MAJ9}}}, {4, {{4, Q_MIN7}, {9, Q_MIN9}, {2, Q_MIN7}, {7, Q_DOM7}}}};
static const Prog MATH_PROG[] = {
    {4, {{0, Q_ADD9}, {5, Q_MAJ7}, {9, Q_MIN7}, {7, Q_SUS2}}}, {4, {{9, Q_MIN9}, {5, Q_ADD9}, {0, Q_ADD9}, {7, Q_SUS4}}},
    {4, {{5, Q_MAJ9}, {7, Q_SUS2}, {4, Q_MIN7}, {9, Q_MIN7}}}, {3, {{0, Q_MAJ9}, {2, Q_MIN9}, {5, Q_MAJ7}}},
    {4, {{0, Q_ADD9}, {4, Q_MIN7}, {5, Q_ADD9}, {5, Q_SUS2}}}};
static const Prog FOLK_PROG[] = {
    {4, {{0, Q_MAJ}, {5, Q_MAJ}, {0, Q_MAJ}, {7, Q_MAJ}}}, {4, {{0, Q_MAJ}, {7, Q_MAJ}, {9, Q_MIN}, {5, Q_MAJ}}},
    {4, {{0, Q_MAJ}, {5, Q_MAJ}, {7, Q_MAJ}, {0, Q_MAJ}}}, {4, {{0, Q_MAJ}, {9, Q_MIN}, {5, Q_MAJ}, {7, Q_MAJ}}},
    {4, {{5, Q_MAJ}, {0, Q_MAJ}, {7, Q_MAJ}, {0, Q_MAJ}}}};
static const int8_t PENTA_MAJ[5] = {0, 2, 4, 7, 9}, PENTA_MIN[5] = {0, 3, 5, 7, 10}, MAJOR[7] = {0, 2, 4, 5, 7, 9, 11};

// ================================================================================================ songs
enum : uint8_t { SEC_INTRO, SEC_A, SEC_B, SEC_OUTRO };
struct Song {
    uint32_t seed = 0;
    uint8_t station = RADIO_LOFI;
    float bpm = 80.f, swing = 0.5f;
    int steps = 16;               // 16ths in a bar
    uint8_t group[6] = {4, 4, 4, 4, 0, 0}, nGroup = 4;  // accents (16ths), math rock's odd meters
    int key = 0;                  // MIDI of the tonic, octave 4 (60..71)
    Prog progA, progB;
    int barsPerChord = 1;
    int bars = 40, introBars = 4, outroBars = 4;
    bool waltz = false, banjo = false, harp = false;
    int8_t motif[32];             // lead: scale steps per 8th note over two bars (-1: rest, -2: hold)
    int8_t arp[8]; uint8_t arpN = 8;
    float stepLen = 0.f;          // radio samples per 16th
    uint32_t lenSamples = 0;
};

static uint8_t sectionOf(const Song& s, int bar) {
    if (bar < s.introBars) return SEC_INTRO;
    if (bar >= s.bars - s.outroBars) return SEC_OUTRO;
    int body = s.bars - s.introBars - s.outroBars, b = bar - s.introBars;
    int part = body / 4; if (part < 1) part = 1;
    int k = b / part;                                   // A B A B
    return (k & 1) ? SEC_B : SEC_A;
}

static void shapeSong(uint8_t st, uint32_t seed, Song& s) {
    s.seed = seed; s.station = st;
    float r0 = h01(seed, 1), r1 = h01(seed, 2), r2 = h01(seed, 3);
    float targetSec = 150.f + 80.f * h01(seed, 4);
    if (st == RADIO_LOFI) {
        s.bpm = 72.f + (int)(r0 * 16.f); s.swing = 0.56f + 0.06f * r1; s.steps = 16;
        static const uint8_t g[4] = {4, 4, 4, 4}; memcpy(s.group, g, 4); s.nGroup = 4;
        static const int8_t keys[7] = {0, 2, 3, 5, 7, 8, 10};
        s.key = 60 + keys[(int)(r2 * 7.f) % 7];
        int pa = (int)(h01(seed, 5) * 6.f) % 6, pb = (pa + 1 + (int)(h01(seed, 6) * 5.f)) % 6;
        s.progA = LOFI_PROG[pa]; s.progB = LOFI_PROG[pb]; s.barsPerChord = (s.progA.n == 2 || h01(seed, 7) < 0.3f) ? 2 : 1;
    } else if (st == RADIO_MATH) {
        s.bpm = 128.f + (int)(r0 * 32.f); s.swing = 0.5f;
        int m = (int)(r1 * 4.f) % 4;                    // 7/8, 5/4, 4/4 displaced, 9/8
        static const uint8_t G[4][6] = {{4, 4, 6, 0, 0, 0}, {6, 6, 4, 4, 0, 0}, {6, 6, 4, 0, 0, 0}, {4, 4, 4, 6, 0, 0}};
        static const uint8_t NG[4] = {3, 4, 3, 4}, ST[4] = {14, 20, 16, 18};
        memcpy(s.group, G[m], 6); s.nGroup = NG[m]; s.steps = ST[m];
        static const int8_t keys[6] = {4, 9, 2, 6, 1, 11};
        s.key = 60 + keys[(int)(r2 * 6.f) % 6];
        if (s.key > 66) s.key -= 12;
        int pa = (int)(h01(seed, 5) * 5.f) % 5, pb = (pa + 1 + (int)(h01(seed, 6) * 4.f)) % 5;
        s.progA = MATH_PROG[pa]; s.progB = MATH_PROG[pb]; s.barsPerChord = 1;
        // the tapping pattern: chord tones over two octaves (0..7), never the same twice running
        int last = -1;
        for (int i = 0; i < 8; i++) { int v; int tries = 0; do { v = (int)(h01(seed, 20 + i, tries) * 8.f) % 8; tries++; } while (v == last && tries < 8); s.arp[i] = (int8_t)v; last = v; }
        s.arpN = (uint8_t)(h01(seed, 8) < 0.5f ? 6 : 8);
    } else {
        s.waltz = h01(seed, 9) < 0.3f;
        s.bpm = s.waltz ? 96.f + (int)(r0 * 30.f) : 92.f + (int)(r0 * 34.f); s.swing = 0.5f + (s.waltz ? 0.f : 0.04f * r1);
        s.steps = s.waltz ? 12 : 16;
        static const uint8_t g4[4] = {4, 4, 4, 4}, g3[3] = {4, 4, 4};
        if (s.waltz) { memcpy(s.group, g3, 3); s.nGroup = 3; } else { memcpy(s.group, g4, 4); s.nGroup = 4; }
        static const int8_t keys[5] = {7, 2, 9, 0, 4};
        s.key = 60 + keys[(int)(r2 * 5.f) % 5];
        if (s.key > 64) s.key -= 12;
        int pa = (int)(h01(seed, 5) * 5.f) % 5, pb = (pa + 1 + (int)(h01(seed, 6) * 4.f)) % 5;
        s.progA = FOLK_PROG[pa]; s.progB = FOLK_PROG[pb]; s.barsPerChord = h01(seed, 7) < 0.4f ? 2 : 1;
        s.banjo = !s.waltz && h01(seed, 10) < 0.55f;
        s.harp = h01(seed, 11) < 0.45f;
    }
    s.stepLen = (float)RSR * 60.f / s.bpm / 4.f;
    float barSec = s.steps * s.stepLen / RSR;
    int bars = (int)(targetSec / barSec);
    int unit = s.progA.n * s.barsPerChord;               // whole progressions
    bars = (bars / unit) * unit; if (bars < unit * 4) bars = unit * 4;
    s.introBars = unit >= 4 ? 4 : unit * 2; s.outroBars = s.introBars;
    s.bars = bars + s.introBars + s.outroBars;
    s.lenSamples = (uint32_t)(s.bars * s.steps * s.stepLen);
    // the lead's motif: two bars of 8ths, a stepwise walk on the pentatonic scale with a shape that comes back
    int pos = 2 + (int)(h01(seed, 12) * 3.f);
    float density = st == RADIO_LOFI ? 0.42f : 0.55f;
    for (int i = 0; i < 32; i++) {
        float r = h01(seed, 40 + i);
        if (i >= s.steps) { s.motif[i] = -1; continue; }
        if (r < density) { int stp = (int)(h01(seed, 80 + i) * 5.f) - 2; pos += stp == 0 ? 1 : stp; pos = pos < 0 ? 1 : (pos > 9 ? 8 : pos); s.motif[i] = (int8_t)pos; }
        else s.motif[i] = (i > 0 && s.motif[i - 1] >= 0 && r < density + 0.25f) ? -2 : -1;
    }
}

// ================================================================================================ the timeline
enum : uint8_t { SEG_SONG, SEG_BREAK };
struct Station {
    uint32_t seed = 0;
    uint32_t segIdx = 0; int64_t segStart = 0; uint32_t segLen = 1; uint8_t segType = SEG_SONG; uint32_t segSeed = 0;
    int16_t clipA = -1, clipB = -1;
    int breaks = 0;
};
static Station s_st[RADIO_N];
static int64_t s_T = 0;                                  // the radio clock: samples at RSR since boot

static int pickClip(uint8_t st, uint8_t kind, uint32_t r) {
    int n = s_nClips;
    for (int pass = 0; pass < 2; pass++) {                 // this station's own recordings first, then any station's
        uint8_t want = pass ? 0 : st;
        int k = 0;
        for (int i = 0; i < n; i++) if (s_clips[i].kind == kind && s_clips[i].station == want) k++;
        if (!k) continue;
        int pick = (int)(r % (uint32_t)k);
        for (int i = 0; i < n; i++) if (s_clips[i].kind == kind && s_clips[i].station == want && pick-- == 0) return i;
    }
    return -1;
}
static uint8_t todKind() {
    float t = g_scene.tod;
    if (t > 0.2f && t < 0.42f) return CK_MORNING;
    if (t < 0.65f && t >= 0.42f) return CK_DAY;
    if (t >= 0.65f && t < 0.82f) return CK_EVENING;
    return CK_NIGHT;
}
static uint8_t weatherKind() {
    if (g_scene.snow > 0.3f) return CK_SNOW;
    if (g_scene.rain > 0.3f) return CK_RAIN;
    if (g_scene.fog > 0.4f) return CK_FOG;
    return CK_CLEAR;
}
static constexpr float STING_SEC = 2.4f;
static void makeSeg(uint8_t si) {
    Station& S = s_st[si];
    S.segType = (S.segIdx & 1) ? SEG_BREAK : SEG_SONG;
    S.segSeed = hsh(S.seed, S.segIdx);
    S.clipA = S.clipB = -1;
    if (S.segType == SEG_SONG) {
        Song tmp; shapeSong(si, S.segSeed, tmp);
        S.segLen = tmp.lenSamples;
        return;
    }
    // a break: the DJ, and every third break an ad
    uint32_t r = S.segSeed;
    float len = 0.6f;
    if (s_nClips > 0) {
        uint8_t kind;
        float u = h01(r, 1);
        if ((S.breaks & 3) == 0) kind = CK_ID;
        else if (u < 0.4f) kind = CK_TALK;
        else if (u < 0.7f) kind = todKind();
        else kind = weatherKind();
        int a = pickClip(si, kind, hsh(r, 2));
        if (a < 0) a = pickClip(si, CK_TALK, hsh(r, 3));
        if (a < 0) a = pickClip(si, CK_ID, hsh(r, 4));
        int b = (S.breaks % 3 == 2) ? pickClip(si, CK_AD, hsh(r, 5)) : -1;
        S.clipA = (int16_t)a; S.clipB = (int16_t)b;
        if (a >= 0) len += (float)s_clips[a].samples / s_clips[a].rate + 0.5f;
        if (b >= 0) len += (float)s_clips[b].samples / s_clips[b].rate + 0.6f;
    }
    if (S.clipA < 0 && S.clipB < 0) len = STING_SEC;
    S.segLen = (uint32_t)((len + 0.8f) * RSR);
    S.breaks++;
}
static void advance(uint8_t si) {
    Station& S = s_st[si];
    while (s_T >= S.segStart + (int64_t)S.segLen) { S.segStart += S.segLen; S.segIdx++; makeSeg(si); }
}

// ================================================================================================ playing the current station
struct Play {
    uint8_t station = RADIO_OFF;
    uint32_t segIdx = 0xFFFFFFFFu;
    Song song;
    int64_t nextStep = 0; int stepIdx = 0;
    bool inSong = false;
    ClipPlay clipA, clipB;
    float bed = 0.f, bedTarget = 0.f;
    int stingStep = 0; int64_t stingNext = 0, bedNext = 0;
};
static Play s_p;
static float s_level = 0.f, s_static = 0.f, s_crackle = 0.f, s_hissLP = 0.f;
static uint32_t s_wowPh = 0;
static dsp::SVF s_outLP, s_outHP, s_voiceBP;
static float s_last = 0.f;
static uint8_t s_lastStation = RADIO_OFF;

static void killVoices() {
    for (KS& k : s_ks) { k.on = false; }
    for (EP& e : s_ep) e.on = false;
    s_bass.env = 0.f; s_lead.envTarget = 0.f; s_lead.env = 0.f;
    s_kick.env = 0.f; s_snare.env = 0.f; s_hat.env = 0.f; s_ride.env = 0.f;
}

static inline int chordTone(const Song& s, const Chord& c, int i, int baseMidi) {   // the i-th tone of the chord at or above baseMidi
    const int8_t* q = QUAL[c.q];
    int root = s.key + c.root;
    while (root < baseMidi) root += 12;
    while (root >= baseMidi + 12) root -= 12;
    int oct = i / 5, k = i % 5;
    return root + q[k] + 12 * oct;
}
static const Chord& chordAt(const Song& s, int bar) {
    const Prog& p = sectionOf(s, bar) == SEC_B ? s.progB : s.progA;
    return p.c[(bar / s.barsPerChord) % p.n];
}
static int ksFree(int from, int to) {                    // a free string in [from, to), else the quietest
    int best = from; float bq = 1e9f;
    for (int i = from; i < to; i++) { if (!s_ks[i].on) return i; float q = s_ks[i].gain * s_ks[i].amp; if (q < bq) { bq = q; best = i; } }
    return best;
}
static void epChord(const Song& s, const Chord& c, float vel, uint32_t r) {
    // a rootless voicing in the middle of the keyboard (3rd, 7th, 9th, 5th), a lazy roll from the bottom
    const int8_t* q = QUAL[c.q];
    int root = s.key + c.root; while (root > 62) root -= 12; while (root < 50) root += 12;
    int notes[4] = {root + q[1], root + q[3] % 24, root + q[4], root + q[2]};
    for (int i = 0; i < 4; i++) { while (notes[i] < 55) notes[i] += 12; while (notes[i] > 76) notes[i] -= 12; }
    for (int i = 0; i < 4; i++) {
        EP& e = s_ep[s_epNext]; s_epNext = (s_epNext + 1) & 3;
        int del = (int)(i * (0.012f + 0.01f * h01(r, i)) * RSR);
        e.play(midiHz((float)notes[i]), vel * (0.22f - i * 0.014f), del, 2.2f);
    }
}
static void strum(const Song& s, const Chord& c, float vel, bool down, float spread, uint32_t r, bool upper = false) {
    // a six-string open voicing: root low, then the chord stacked up; strummed from the low string or the high one
    int root = s.key + c.root - 24; root = fold(root, 40, 51);
    const int8_t* q = QUAL[c.q];
    int str[6] = {root, root + 7, root + 12, root + q[1] + 12, root + 19, root + 24};
    if (c.q == Q_MIN) { str[3] = root + 15; }
    if (c.q == Q_SUS2) { str[3] = root + 14; } if (c.q == Q_SUS4) { str[3] = root + 17; }
    int first = upper ? 2 : 0;
    for (int i = first; i < 6; i++) {
        int k = down ? i - first : 5 - i;
        KS& g = s_ks[2 + i];                             // strings 2..7 are the guitar's six
        float del = k * spread * (0.8f + 0.4f * h01(r, i));
        g.pluck(midiHz((float)str[i]), vel * (0.24f - i * 0.014f), 0.55f + 0.2f * h01(r, 7 + i), 1.4f, (int)(del * RSR));
    }
}

static void buildPlaySong(uint8_t si) {
    Station& S = s_st[si];
    shapeSong(si, S.segSeed, s_p.song);
    s_lead.kind = si == RADIO_FOLK ? (s_p.song.harp ? LEAD_HARP : LEAD_FIDDLE) : LEAD_FLUTE;
    if (s_lead.kind == LEAD_FLUTE) { setF(s_lead.f, 2400.f, 0.7f); }
    else if (s_lead.kind == LEAD_FIDDLE) { setF(s_lead.f, 1250.f, 1.4f); setF(s_lead.f2, 2600.f, 0.7f); }
    else { setF(s_lead.f, 900.f, 2.2f); setF(s_lead.f2, 2200.f, 2.5f); }
}

// one 16th of the current song
static void songStep(int idx, int delay) {
    const Song& s = s_p.song;
    int bar = idx / s.steps, st = idx % s.steps;
    if (bar >= s.bars) return;
    uint8_t sec = sectionOf(s, bar);
    const Chord& ch = chordAt(s, bar);
    bool chordStart = st == 0 && (bar % s.barsPerChord) == 0;
    uint32_t r = hsh(s.seed, bar, st);
    bool lastBar = bar == s.bars - 1;
    bool groupStart = false; int gi = 0;
    for (int g = 0, acc = 0; g < s.nGroup; acc += s.group[g], g++) if (st == acc) { groupStart = true; gi = g; }
    // in the last bar only the downbeat rings out
    if (lastBar && st != 0) return;

    if (s.station == RADIO_LOFI) {
        bool drums = sec != SEC_INTRO && !(sec == SEC_OUTRO && bar >= s.bars - 2);
        if (chordStart) epChord(s, ch, sec == SEC_B ? 1.0f : 0.9f, r);
        else if (st == 10 && h01(r, 1) < 0.3f && sec != SEC_INTRO) epChord(s, ch, 0.55f, r);
        if (st == 0) s_bass.play(midiHz((float)(chordTone(s, ch, 0, 36))), 0.34f, 1.6f, delay);
        else if (st == 10 && h01(r, 2) < 0.45f && drums) s_bass.play(midiHz((float)(chordTone(s, ch, h01(r, 3) < 0.5f ? 2 : 0, 36))), 0.26f, 0.7f, delay);
        if (drums) {
            if (st == 0 || (st == 10 && h01(r, 4) < 0.65f) || (st == 7 && h01(r, 5) < 0.25f)) s_kick.hit(st == 0 ? 0.9f : 0.7f, delay);
            if (st == 4 || st == 12) s_snare.hit(0.38f + 0.07f * h01(r, 6), false, delay + (int)(0.012f * RSR));
            if ((st & 1) == 0) s_hat.hit(0.09f + 0.05f * h01(r, 7) + ((st & 3) == 2 ? 0.03f : 0.f), st == 14 && h01(r, 8) < 0.2f ? 0.18f : 0.045f, delay);
            else if (h01(r, 9) < 0.12f) s_hat.hit(0.05f, 0.03f, delay);
        }
        if (sec == SEC_B && (st & 1) == 0) {
            const int half = s.steps / 2, base = (bar & 1) * half;
            int8_t v = s.motif[base + st / 2];
            if (v >= 0) {
                int deg = v % 5, oct = v / 5;
                int note = s.key + PENTA_MAJ[deg] + 12 * oct;
                note = fold(note, 67, 86);
                int len = 1; while (st / 2 + len < half && s.motif[base + st / 2 + len] == -2) len++;
                s_lead.note(midiHz((float)note), 0.085f, (int)(len * 2 * s.stepLen * 0.9f));
            }
        }
    } else if (s.station == RADIO_MATH) {
        bool drums = sec != SEC_INTRO && !(sec == SEC_OUTRO && bar >= s.bars - 2);
        bool stop = sec == SEC_B && ((bar - s.introBars) % 8) == 7;      // the last bar of a B phrase: a stop
        // tapped guitar: every 16th, two octaves of chord tones
        {
            int i = s.arp[(idx) % s.arpN];
            int note = chordTone(s, ch, i < 5 ? i : i - 2, 64) + (i >= 5 ? 12 : 0);
            while (note > 88) note -= 12;
            float acc = groupStart ? 1.f : 0.7f;
            int v = ksFree(0, 2);
            s_ks[v].pluck(midiHz((float)note), 0.27f * acc, 0.75f, 0.9f, delay);
        }
        // the second guitar in B: an answering line in 8ths, higher
        if (sec == SEC_B && (st & 1) == 0 && !stop && h01(r, 10) < 0.7f) {
            int i = s.arp[(idx / 2 + 3) % s.arpN];
            int note = chordTone(s, ch, i % 5, 72);
            KS& g = s_ks[5 + (idx / 2) % 3];
            g.pluck(midiHz((float)note), 0.17f, 0.6f, 1.2f, delay + (int)(0.004f * RSR));
        }
        if (groupStart && !(stop && st > 0)) {
            int note = chordTone(s, ch, gi == 2 && h01(r, 11) < 0.4f ? 2 : 0, 33);
            KS& b = s_ks[4];
            b.pluck(midiHz((float)note), 0.45f, 0.3f, 1.2f, delay);
        }
        if (drums && !(stop && st > 0)) {
            if (groupStart && (gi == 0 || h01(r, 12) < 0.55f)) s_kick.hit(gi == 0 ? 0.75f : 0.62f, delay);
            int mid = s.group[0];
            if (st == mid || (s.nGroup >= 3 && st == s.group[0] + s.group[1] + s.group[2] / 2 + 1)) s_snare.hit(0.42f, false, delay);
            if ((st & 1) == 0) s_ride.hit(0.07f + (groupStart ? 0.04f : 0.f), 0.22f, delay);
            // a fill in the last bar of each phrase
            bool phraseEnd = ((bar - s.introBars) % 8) == 7 && sec == SEC_A;
            if (phraseEnd && st >= s.steps - 4) s_snare.hit(0.22f + 0.06f * (st - (s.steps - 4)), false, delay);
        }
    } else {   // country / folk
        bool band = sec != SEC_INTRO;
        int half = s.steps / 2;
        if (s.waltz) {
            if (st == 0) { KS& b = s_ks[0]; b.pluck(midiHz((float)chordTone(s, ch, 0, 40)), 0.5f, 0.35f, 0.9f, delay); }
            if (st == 4 || st == 8) strum(s, ch, st == 4 ? 0.9f : 0.75f, true, 0.008f, r, true);
            if (band && (st == 4 || st == 8)) s_snare.hit(0.25f, true, delay);
        } else {
            // boom-chick: bass on 1 and 3 (root, then the fifth), the guitar on 2 and 4 with a light upstroke after
            if (st == 0 || st == half) {
                KS& b = s_ks[0];
                int tone = st == 0 ? 0 : 2;
                b.pluck(midiHz((float)chordTone(s, ch, tone, 40)), 0.5f, 0.35f, 0.8f, delay);
                if (band) s_kick.hit(0.35f, delay);
            }
            if (st == 4 || st == 12) strum(s, ch, 0.95f, true, 0.007f, r, true);
            if ((st == 6 || st == 14) && h01(r, 13) < 0.7f) strum(s, ch, 0.45f, false, 0.004f, r, true);
            if (band && (st == 4 || st == 12)) s_snare.hit(0.27f, true, delay);
            if (band && (st & 1) == 0 && st != 4 && st != 12) s_hat.hit(0.035f, 0.05f, delay);
        }
        // banjo rolls in A (forward roll over the chord's tones, high)
        if (s.banjo && sec == SEC_A) {
            static const int8_t ROLL[8] = {0, 1, 2, 0, 1, 2, 0, 2};
            int note = chordTone(s, ch, ROLL[st & 7] + 1, 67);
            KS& b = s_ks[1];
            b.pluck(midiHz((float)note), 0.16f + (st % 3 == 0 ? 0.04f : 0.f), 0.85f, 0.5f, delay);
        }
        // the fiddle or harmonica in B: the motif, longer notes
        if (sec == SEC_B && (st & 1) == 0) {
            int8_t v = s.motif[(bar & 1) * (s.steps / 2) + st / 2];
            if (v >= 0) {
                int deg = v % 5, oct = v / 5;
                int note = s.key + PENTA_MAJ[deg] + 12 * oct;
                note = fold(note, 64, 84);
                int len = 1; while (st / 2 + len < s.steps / 2 && s.motif[(bar & 1) * (s.steps / 2) + st / 2 + len] == -2) len++;
                s_lead.note(midiHz((float)note), s.harp ? 0.055f : 0.07f, (int)(len * 2 * s.stepLen * 0.95f));
            }
        }
    }
}

// the station's sung ident (no recordings): a four-note call and the tonic chord
static void stingStep(uint8_t si, int k, uint32_t seed) {
    static const int8_t CALL[RADIO_N][4] = {{0, 0, 0, 0}, {7, 4, 2, 0}, {0, 7, 11, 14}, {0, 4, 7, 12}};
    Song& s = s_p.song;
    if (k == 0) {
        shapeSong(si, seed, s);
        buildPlaySong(si);
    }
    if (k < 4) {
        int note = s.key + 12 + CALL[si][k];
        if (si == RADIO_MATH) { KS& g = s_ks[k & 1]; g.pluck(midiHz((float)note), 0.2f, 0.75f, 1.0f); }
        else s_lead.note(midiHz((float)note), 0.16f, (int)(0.28f * RSR));
    }
    if (k == 4) {
        Chord c = {0, si == RADIO_LOFI ? Q_MAJ9 : (si == RADIO_MATH ? Q_ADD9 : Q_MAJ)};
        if (si == RADIO_LOFI) epChord(s, c, 1.f, seed); else strum(s, c, 0.9f, true, 0.02f, seed);
        s_kick.hit(0.4f);
    }
}

void radioInit() {
    for (KS& k : s_ks) if (!k.buf) k.buf = (int16_t*)dsp::allocFast(KS_N * sizeof(int16_t));
    for (int i = 1; i < RADIO_N; i++) {
        Station& S = s_st[i];
        S.seed = hsh(0x52414449u, i, 0x0F00u);
        S.segIdx = 0; makeSeg((uint8_t)i);
        S.segStart = -(int64_t)(S.segLen * (0.15f + 0.5f * h01(S.seed, 99)));   // each station is somewhere in its first song
    }
    setF(s_hat.f, 4200.f, 0.7f); setF(s_ride.f, 6000.f, 0.8f);
    s_outHP.reset(); s_outLP.reset();
}

// render n samples at the radio's rate into out (replacing)
static void render12(float* out, int n) {
    uint8_t si = s_station;
    for (int i = 1; i < RADIO_N; i++) advance((uint8_t)i);
    bool live = si != RADIO_OFF && !s_paused;
    float target = live ? 1.f : 0.f;
    if (si != s_lastStation) {                           // tuning: a moment of static, the old station gone
        if (si != RADIO_OFF) s_static = 1.f;
        killVoices(); s_p.segIdx = 0xFFFFFFFFu; s_p.clipA.on = s_p.clipB.on = false;
        s_lastStation = si;
    }
    if (!live && s_level < 0.0005f && s_static < 0.001f) { memset(out, 0, n * sizeof(float)); s_level = 0.f; s_T += n; return; }

    // the segment we're in
    Station* S = si != RADIO_OFF ? &s_st[si] : nullptr;
    if (S && s_p.segIdx != S->segIdx) {
        s_p.segIdx = S->segIdx;
        s_p.clipA.on = s_p.clipB.on = false;
        s_p.station = si;
        if (S->segType == SEG_SONG) {
            buildPlaySong(si);
            int64_t into = s_T - S->segStart;
            int idx = into <= 0 ? 0 : (int)(into / s_p.song.stepLen) + (into > 0 ? 1 : 0);
            if (into <= 0) idx = 0;
            s_p.stepIdx = idx;
            s_p.nextStep = S->segStart + (int64_t)(idx * s_p.song.stepLen);
            s_p.inSong = true; s_p.bedTarget = 0.f;
        } else {
            s_p.inSong = false;
            int64_t into = s_T - S->segStart;
            float lead = 0.4f * RSR;
            // a recording plays only if we were here when it began (a voice doesn't start mid-sentence)
            s_p.clipA.on = s_p.clipB.on = false;
            if (S->clipA >= 0 && into <= (int64_t)lead) s_p.clipA.start(&s_clips[S->clipA], (int)(lead - into));
            if (S->clipB >= 0) {
                float startB = lead + (S->clipA >= 0 ? (float)s_clips[S->clipA].samples / s_clips[S->clipA].rate * RSR + 0.5f * RSR : 0.f);
                if (into <= (int64_t)startB) s_p.clipB.start(&s_clips[S->clipB], (int)(startB - into));
            }
            s_p.bedTarget = (S->clipA >= 0 || S->clipB >= 0) ? 1.f : 0.f;
            s_p.stingStep = 0; s_p.stingNext = S->segStart + (int64_t)(0.3f * RSR);
            if (S->clipA < 0 && S->clipB < 0 && into > (int64_t)(0.3f * RSR)) s_p.stingStep = 99;   // tuned in after the ident began
            // the bed: the next song's key, an electric-piano chord breathing under the voice
            if (s_p.bedTarget > 0.f) { shapeSong(si, hsh(S->seed, S->segIdx + 1), s_p.song); buildPlaySong(si); s_p.bed = 0.f; }
            s_p.bedNext = s_T;                                   // the first chord as the break begins (or as you tune in)
        }
    }

    const bool lofi = si == RADIO_LOFI;
    setF(s_outLP, lofi ? 3300.f : 4800.f, 0.6f);
    setF(s_outHP, 110.f, 0.7f);
    setF(s_voiceBP, 1800.f, 0.45f);
    static const float LEVEL[RADIO_N] = {1.f, 1.f, 1.15f, 1.8f};   // the stations' music matched in loudness (10 minutes each)
    const float level = LEVEL[si];
    for (int i = 0; i < n; i++) {
        // events
        if (S && live) {
            if (s_p.inSong) {
                if (s_T - s_p.nextStep > (int64_t)(2.f * s_p.song.stepLen)) {      // back from a pause: pick up where the song is now
                    s_p.stepIdx = (int)((s_T - S->segStart) / s_p.song.stepLen) + 1;
                    s_p.nextStep = S->segStart + (int64_t)(s_p.stepIdx * s_p.song.stepLen);
                }
                while (s_T >= s_p.nextStep && s_p.stepIdx < s_p.song.bars * s_p.song.steps) {
                    int idx = s_p.stepIdx;
                    int sw = (idx & 1) ? (int)((s_p.song.swing - 0.5f) * 2.f * s_p.song.stepLen) : 0;
                    songStep(idx, sw);
                    s_p.stepIdx++;
                    s_p.nextStep = S->segStart + (int64_t)(s_p.stepIdx * s_p.song.stepLen);
                }
            } else if (s_p.bedTarget <= 0.f && s_p.stingStep <= 4 && s_T >= s_p.stingNext) {
                stingStep(si, s_p.stingStep, hsh(S->seed, S->segIdx + 1));
                s_p.stingStep++;
                s_p.stingNext += (int64_t)((s_p.stingStep == 4 ? 0.34f : 0.3f) * RSR);
            } else if (s_p.bedTarget > 0.f && s_T >= s_p.bedNext) {
                s_p.bedNext = s_T + (int64_t)(2.4f * RSR);
                Chord c = {0, lofi ? Q_MAJ9 : (si == RADIO_MATH ? Q_ADD9 : Q_MAJ)};
                epChord(s_p.song, c, 0.55f, (uint32_t)(s_T >> 8));
            }
        }
        // voices
        s_wowPh += rinc(lofi ? 0.55f : 0.0f);
        float wow = lofi ? 1.f + 0.0022f * sineAt(s_wowPh) : 1.f;
        float x = 0.f, part = 0.f;
        for (KS& k : s_ks) if (k.on) part += k.tick();
        x += part * SOLO(0); part = 0.f;
        for (EP& e : s_ep) if (e.on) part += e.tick(wow);
        x += part * SOLO(1);
        x += s_bass.tick(wow) * SOLO(2);
        x += s_lead.tick(wow) * SOLO(3);
        x += s_kick.tick() * SOLO(4) + s_snare.tick() * SOLO(5) + (s_hat.tick() + s_ride.tick()) * SOLO(6);
        float v = s_p.clipA.tick() + s_p.clipB.tick();
        x *= level;
        if (s_p.clipA.on || s_p.clipB.on) x = x * 0.35f;                 // the music ducks under the voice
        x += v * 0.9f;
        if (lofi) {                                                      // the record: crackle and a little hiss
            if (s_rng.uni() < 0.0009f) s_crackle = s_rng.bi() * 0.18f;
            s_crackle *= 0.55f;
            s_hissLP += 0.3f * (s_rng.bi() - s_hissLP);
            x += (s_crackle + s_hissLP * 0.006f) * SOLO(7);
        }
        if (s_static > 0.001f) {                                         // tuning static
            s_static *= 0.99975f;
            x = x * (1.f - s_static) + s_voiceBP.bp(s_rng.bi()) * s_static * 0.22f;
        }
        x = s_outHP.hp(s_outLP.lp(x));
        s_level += (target - s_level) * 0.0015f;
        out[i] = dsp::softclip(x * 1.4f) * s_level;
        s_T++;
    }
}

static constexpr float REV = 0.06f;                      // a breath of the cabin's room on the radio
void radioBlock(float* dry, float* rev, int n) {
    static float buf[256];
    int m = n / 2; if (m > 256) m = 256;
    if (s_station == RADIO_OFF && s_lastStation == RADIO_OFF && s_level < 0.0005f) {
        // off: the stations play on (their clocks only)
        s_T += m;
        for (int i = 1; i < RADIO_N; i++) advance((uint8_t)i);
        return;
    }
    render12(buf, m);
    float vol = (float)s_vol / RADIO_VOL_MAX;
    float g = 0.55f * vol * vol;
    for (int j = 0; j < m; j++) {
        float c = buf[j] * g;
        float c0 = (s_last + c) * 0.5f;
        dry[2 * j] += c0; dry[2 * j + 1] += c;
        rev[2 * j] += c0 * REV; rev[2 * j + 1] += c * REV;
        s_last = c;
    }
}

#ifdef HOST
void radioSkipToBreak(uint8_t si) {
    Station& S = s_st[si];
    advance(si);
    if (S.segType == SEG_SONG) {                         // move the clock to just before this song ends
        int64_t end = S.segStart + S.segLen;
        int64_t back = (int64_t)(6.f * RSR);
        int64_t shift = end - back - s_T;
        if (shift > 0) {
            for (int i = 1; i < RADIO_N; i++) s_st[i].segStart -= shift;   // the same as time passing for every station
        }
        s_p.segIdx = 0xFFFFFFFFu;
    }
}
#endif

}
